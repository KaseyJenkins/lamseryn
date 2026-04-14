#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <inttypes.h>

#include <liburing.h>

#include "logger.h"
#include "include/macros.h"
#include "include/types.h"
#include "include/config.h"
#include "include/conn.h"
#include "include/conn_store.h"
#include "include/conn_deadline.h"
#include "include/conn_lifecycle.h"
#include "include/http_headers.h"
#include "include/http_parser.h"
#include "include/req_arena.h"
#include "include/rx_stash.h"
#include "include/tls.h"
#include "include/timing_wheel.h"
#include "include/time_utils.h"
#include "include/worker_ctx.h"
#include "include/ring_ops.h"
#include "include/worker_loop.h"
#include "include/accept_controller.h"
#include "include/conn_close.h"

#include "instrumentation/counters_update.h"

// ---------------------------------------------------------------------------
// Active connection count (process-global, atomic)
// ---------------------------------------------------------------------------

static volatile uint64_t g_active_conns_global = 0;

uint64_t active_conns_inc(void) {
  return __sync_add_and_fetch((uint64_t *)&g_active_conns_global, 1);
}

uint64_t active_conns_dec(void) {
  for (;;) {
    uint64_t before =
      __sync_add_and_fetch((uint64_t *)&g_active_conns_global, 0);
    if (before == 0) {
      return 0;
    }
    if (__sync_bool_compare_and_swap((uint64_t *)&g_active_conns_global,
                                     before,
                                     before - 1)) {
      return before - 1;
    }
  }
}

uint64_t active_conns_total(void) {
  return __sync_add_and_fetch((uint64_t *)&g_active_conns_global, 0);
}

// ---------------------------------------------------------------------------
// Connection freelist (per-thread)
// ---------------------------------------------------------------------------

static THREAD_LOCAL struct conn *conn_free_list = NULL;
static THREAD_LOCAL size_t conn_free_count = 0;

#ifndef CONN_FREELIST_STATS
#if LOG_COMPILE_LEVEL >= LOG_DEBUG
#define CONN_FREELIST_STATS 1
#else
#define CONN_FREELIST_STATS 0
#endif
#endif

#if CONN_FREELIST_STATS
static THREAD_LOCAL uint64_t conn_alloc_hits = 0;
static THREAD_LOCAL uint64_t conn_alloc_misses = 0;
static THREAD_LOCAL uint64_t conn_recycle_calls = 0;
static THREAD_LOCAL uint64_t conn_free_peak = 0;
#endif

#ifndef CONN_FREELIST_CAP
#define CONN_FREELIST_CAP (1u << 15)
#endif

struct conn *conn_alloc(void) {
  struct conn *c = conn_free_list;
  if (c) {
    conn_free_list = c->free_next;
    conn_free_count--;
    memset(c, 0, sizeof(*c));
#if CONN_FREELIST_STATS
    conn_alloc_hits++;
#endif
    return c;
  }
#if CONN_FREELIST_STATS
  conn_alloc_misses++;
#endif
  return (struct conn *)calloc(1, sizeof(struct conn));
}

void conn_recycle(struct conn *c) {
  if (!c) {
    return;
  }

  if (c->tls_enabled || c->tls_handle) {
    tls_conn_destroy(c);
  }

  if (c->rx_stash) {
    rx_stash_free(c->rx_stash);
    c->rx_stash = NULL;
    c->rx_stash_len = 0;
  }

  if (c->tx.dyn_buf) {
    free(c->tx.dyn_buf);
    c->tx.dyn_buf = NULL;
  }

  req_arena_destroy(&c->h1.arena);
  c->h1.target = NULL;
  c->h1.path_dec = NULL;
  c->h1.path_norm = NULL;
  c->h1.req_hdrs = NULL;

  c->dl.deadline_active = 0;
  c->link.tw_prev = NULL;
  c->link.tw_next = NULL;
  c->link.in_active_set = 0;
  c->link.active_idx = -1;

  c->fd = -1;

  if (conn_free_count >= CONN_FREELIST_CAP) {
    free(c);
    return;
  }
  c->free_next = conn_free_list;
  conn_free_list = c;
  conn_free_count++;
#if CONN_FREELIST_STATS
  conn_recycle_calls++;
  if (conn_free_count > conn_free_peak) {
    conn_free_peak = conn_free_count;
  }
#endif
}

void conn_ref(struct conn *c) {
  if (c) {
    c->refcnt++;
  }
}

void conn_put(struct conn *c) {
  if (!c) {
    return;
  }
  if (--c->refcnt == 0) {
    conn_recycle(c);
  }
}

void conn_freelist_log_stats(int thread_id) {
#if CONN_FREELIST_STATS
  LOGD(LOGC_CORE,
  "Thread %d freelist: alloc_hits=%" PRIu64 " alloc_misses=%" PRIu64 " recycled=%" PRIu64 " "
  "free_peak=%" PRIu64 " free_count=%zu cap=%u",
       thread_id,
       conn_alloc_hits,
       conn_alloc_misses,
       conn_recycle_calls,
       conn_free_peak,
       (size_t)conn_free_count,
       (unsigned)CONN_FREELIST_CAP);
#else
  UNUSED(thread_id);
#endif
}

void conn_freelist_drain(void) {
  while (conn_free_list) {
    struct conn *next = conn_free_list->free_next;
    free(conn_free_list);
    conn_free_list = next;
  }
  conn_free_count = 0;
}

// ---------------------------------------------------------------------------
// TX file state cleanup
// ---------------------------------------------------------------------------

void tx_close_file(struct conn *c) {
  if (!c) {
    return;
  }
  if (c->tx.file_fd >= 0) {
    close(c->tx.file_fd);
    c->tx.file_fd = -1;
  }
  c->tx.file_off = 0;
  c->tx.file_rem = 0;
}

// ---------------------------------------------------------------------------
// Active-set bookkeeping
// ---------------------------------------------------------------------------

void worker_active_add(struct worker_ctx *w, int fd) {
  struct conn *c = conn_get(fd);
  if (!c) {
    return;
  }
  if (c->link.in_active_set) {
    return;
  }

  if (w->active_count == w->active_cap) {
    size_t new_cap = w->active_cap ? w->active_cap * 2 : 1024;
    int *nf = (int *)realloc(w->active_fds, new_cap * sizeof(int));
    if (!nf) {
      return;
    }
    w->active_fds = nf;
    w->active_cap = new_cap;
  }

  size_t idx = w->active_count;
  w->active_fds[idx] = fd;
  w->active_count++;

  c->link.in_active_set = 1;
  c->link.active_idx = (int)idx;
}

void worker_active_remove(struct worker_ctx *w, int fd) {
  struct conn *c = conn_get(fd);
  if (!c || !c->link.in_active_set) {
    return;
  }

  size_t count = w->active_count;
  size_t idx = (size_t)c->link.active_idx;

  if (idx >= count || w->active_fds[idx] != fd) {
    for (size_t i = 0; i < count; ++i) {
      if (w->active_fds[i] == fd) {
        idx = i;
        break;
      }
    }
    if (idx >= count || w->active_fds[idx] != fd) {
      c->link.in_active_set = 0;
      c->link.active_idx = -1;
      return;
    }
  }

  size_t last = count - 1;
  int swapped_fd = w->active_fds[last];

  w->active_fds[idx] = swapped_fd;
  w->active_count = last;

  if (idx != last) {
    struct conn *cs = conn_get(swapped_fd);
    if (cs) {
      cs->link.active_idx = (int)idx;
    }
  }

  c->link.in_active_set = 0;
  c->link.active_idx = -1;
}

// ---------------------------------------------------------------------------
// Connection init
// ---------------------------------------------------------------------------

int conn_init(struct worker_ctx *w, int fd, const struct vhost_t *vhost) {
  struct conn *c = conn_alloc();
  if (!c) {
    return -1;
  }

  http_parser_init(&c->h1.parser, &w->http_settings);
  c->h1.parser.data = c;

  req_arena_init(&c->h1.arena);

  c->vhost = vhost;

  uint64_t feats = c->vhost ? c->vhost->features : 0;
  c->h1.req_hdr_store_mask = http_headers_store_mask(feats);

  uint16_t mhf = c->vhost ? c->vhost->max_header_fields : (uint16_t)100;
  c->h1.hdr_fields_max = mhf;

  uint64_t now = (w && w->now_cached_ms) ? w->now_cached_ms : time_now_ms_monotonic();
  conn_mark_activity(c, now);

  c->fd = fd;
  c->refcnt = 1;
  c->link.active_idx = -1;

  // Capture remote peer address at accept time.
  {
    struct sockaddr_storage peer;
    socklen_t peer_len = sizeof(peer);
    if (getpeername(fd, (struct sockaddr *)&peer, &peer_len) == 0) {
      if (peer.ss_family == AF_INET) {
        const struct sockaddr_in *sin = (const struct sockaddr_in *)&peer;
        inet_ntop(AF_INET, &sin->sin_addr, c->remote_ip, sizeof(c->remote_ip));
        c->remote_port = ntohs(sin->sin_port);
      } else if (peer.ss_family == AF_INET6) {
        const struct sockaddr_in6 *sin6 = (const struct sockaddr_in6 *)&peer;
        if (IN6_IS_ADDR_V4MAPPED(&sin6->sin6_addr)) {
          struct in_addr v4;
          memcpy(&v4, &sin6->sin6_addr.s6_addr[12], sizeof(v4));
          inet_ntop(AF_INET, &v4, c->remote_ip, sizeof(c->remote_ip));
        } else {
          inet_ntop(AF_INET6, &sin6->sin6_addr, c->remote_ip, sizeof(c->remote_ip));
        }
        c->remote_port = ntohs(sin6->sin6_port);
      }
    }
  }

  c->tx.file_fd = -1;
  c->tx.file_off = 0;
  c->tx.file_rem = 0;

  if (w) {
    c->generation = ++w->next_conn_generation;
  }

  c->op_read = (struct op_ctx){.magic = OP_MAGIC, .type = OP_READ, .c = c, .fd = -1};
  c->op_write = (struct op_ctx){.magic = OP_MAGIC, .type = OP_WRITE, .c = c, .fd = -1};
  c->op_write_ready = (struct op_ctx){.magic = OP_MAGIC, .type = OP_WRITE_READY, .c = c, .fd = -1};
  c->op_close = (struct op_ctx){.magic = OP_MAGIC, .type = OP_CLOSE, .c = c, .fd = -1};

  c->dl.header_start_ms = now;
  c->dl.header_start_us = time_now_us_monotonic();

  if (conn_store_put(fd, c) != 0) {
    conn_recycle(c);
    return -1;
  }
  (void)active_conns_inc();
  worker_active_add(w, fd);

  tw_reschedule(w, c, now);

  return 0;
}

// ---------------------------------------------------------------------------
// Connection request reset (keep-alive boundary)
// ---------------------------------------------------------------------------

void conn_reset_request(struct worker_ctx *w, struct conn *c) {
  c->h1.headers_done = 0;
  c->h1.message_done = 0;
  c->h1.parse_error = 0;
  c->h1.internal_error = 0;
  c->h1.header_too_big = 0;
  c->h1.header_fields_too_many = 0;
  c->h1.unsupported_te = 0;
  c->h1.want_keepalive = 0;
  http_parser_reset_header_state(c);
  c->h1.cl_count = 0;
  c->h1.te_count = 0;
  c->h1.expect_count = 0;
  c->h1.cl_invalid = 0;
  c->h1.te_chunked = 0;
  c->h1.te_other = 0;
  c->h1.expect_100_continue = 0;
  c->h1.expect_unsupported = 0;
  c->h1.cl_value = 0;
  c->h1.method = HTTP_DELETE;
  c->h1.method_set = 0;
  c->h1.target_len = 0;
  c->h1.target_too_long = 0;
  c->h1.path_len = 0;
  c->h1.query_off = 0;
  c->h1.query_len = 0;
  c->h1.path_dec_len = 0;
  c->h1.path_norm_len = 0;
  c->h1.path_bad = 0;
  c->h1.path_ends_with_slash = 0;
  c->h1.method_not_allowed = 0;

  req_arena_reset(&c->h1.arena);
  c->h1.target = NULL;
  c->h1.target_cap = 0;
  c->h1.path_dec = NULL;
  c->h1.path_dec_cap = 0;
  c->h1.path_norm = NULL;
  c->h1.path_norm_cap = 0;
  c->h1.req_hdrs = NULL;
  c->h1.req_hdr_cap = 0;
  c->h1.req_hdr_count = 0;
  c->h1.req_hdr_seen_mask = 0;
  c->h1.hdr_fields_count = 0;
  http_parser_init(&c->h1.parser, &w->http_settings);
  c->h1.parser.data = c;
  c->h1.parser_bytes = 0;
  c->h1.body_remaining = 0;
  c->h1.body_bytes = 0;
  c->h1.body_too_big = 0;
  c->h1.pending_line_error = 0;
  c->rx_tail_len = 0;

  uint64_t now = (w && w->now_cached_ms) ? w->now_cached_ms : time_now_ms_monotonic();
  conn_enter_keepalive_idle(c, now);
  c->dl.write_start_ms = 0;

  c->noselect_streak = 0;

  if (c->tx.file_fd >= 0) {
    close(c->tx.file_fd);
    c->tx.file_fd = -1;
    c->tx.file_off = 0;
    c->tx.file_rem = 0;
  }
  if (c->tx.dyn_buf) {
    free(c->tx.dyn_buf);
    c->tx.dyn_buf = NULL;
  }
  c->tx.write_buf = NULL;
  c->tx.write_len = 0;
  c->tx.write_off = 0;
  c->tx.content_length_hint = 0;
  c->tx.resp_kind = RK_NONE;
  c->tx.keepalive = 0;
  c->tx.drain_after_headers = 0;
  c->tx.write_poll_armed = 0;
  c->tx.recv_armed = 0;
  conn_clear_draining(c);

  c->op_read.c = c;
  c->op_write.c = c;
  c->op_write_ready.c = c;
  c->op_close.c = c;
  c->op_close.fd = -1;

  tw_reschedule(w, c, now);
}

// ---------------------------------------------------------------------------
// schedule_or_sync_close — stage async close or do sync close
// ---------------------------------------------------------------------------

int schedule_or_sync_close(struct worker_ctx *w, int fd) {
  struct conn *c = conn_get(fd);
  if (!c) {
    return 1;
  }

  enum deadline_kind dk = c->dl.deadline_kind;
  int abortive = c->dl.abortive_close;
  c->dl.abortive_close = 0;

  tw_cancel(w, c);
  conn_mark_closing(c);

  tx_close_file(c);
  if (c->tls_enabled) {
    (void)tls_conn_shutdown(c);
  }

  int fd_to_close = c->fd;
  if (fd_to_close < 0) {
    worker_active_remove(w, fd);
    conn_store_del(fd);
    (void)active_conns_dec();
    return 1;
  }

  conn_ref(c);

  // Deterministic timeout enforcement: for idle/drain expiries we prefer a
  // synchronous close. This avoids depending on IORING_OP_CLOSE behavior
  // (which can be delayed or unsupported on some kernels).
  if (dk == DK_INITIAL_IDLE || dk == DK_KA_IDLE || dk == DK_DRAIN || abortive) {
    int fd_to_close2 = fd_to_close;
    c->fd = -1;

    (void)worker_loop_cancel_write_poll_if_armed(w, c);
    maybe_flush(w, /*urgent=*/0);

    if (abortive) {
      if (dk == DK_WRITE_TIMEOUT) {
        CTR_INC_DEV(w, cnt_write_timeout_abortive_close);
      }

      struct linger ling = {.l_onoff = 1, .l_linger = 0};
      (void)setsockopt(fd_to_close2, SOL_SOCKET, SO_LINGER, &ling, sizeof(ling));
      (void)close(fd_to_close2);
    } else {
      (void)shutdown(fd_to_close2, SHUT_RDWR);
      (void)close(fd_to_close2);
    }
    worker_active_remove(w, fd_to_close2);
    conn_store_del(fd_to_close2);
    (void)active_conns_dec();
    conn_put(c);
    conn_put(c);
    return 1;
  }

  struct io_uring_sqe *sqe = get_sqe_batching(w);
  if (!sqe) {
    c->fd = -1;
    (void)close(fd_to_close);

    worker_active_remove(w, fd_to_close);
    conn_store_del(fd_to_close);
    (void)active_conns_dec();

    conn_put(c);
    conn_put(c);
    return 1;
  }

  c->fd = -1;
  io_uring_prep_close(sqe, fd_to_close);

  c->op_close.fd = fd_to_close;
  c->op_close.c = c;
  io_uring_sqe_set_data64(sqe, (uint64_t)(uintptr_t)&c->op_close);

  (void)worker_loop_cancel_write_poll_if_armed(w, c);

  mark_post(w);
  bool fd_pressure = accept_is_fd_pressure(w);
  maybe_flush(w, /*urgent=*/fd_pressure);

  worker_active_remove(w, fd_to_close);
  conn_store_del(fd_to_close);
  (void)active_conns_dec();

  conn_put(c);
  return 0;
}
