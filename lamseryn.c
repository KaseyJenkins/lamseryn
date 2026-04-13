// Main server translation unit.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stddef.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/poll.h>
#include <netdb.h>
#include <net/if.h>
#include <unistd.h>
#include <stdbool.h>
#include <ctype.h>
#include <arpa/inet.h>

#include <liburing.h>
#include <llhttp.h>
#include <time.h>

#include "logger.h"
#include "request_handlers.h"
#include "http_parser.h"
#include "url.h"

#include "instrumentation/instrumentation.h"
#include "instrumentation/counters_update.h"

#include "include/macros.h"
#include "include/types.h"
#include "include/config.h"
#include "include/net_server.h"
#include "include/conn.h"
#include "include/http_headers.h"
#include "include/http1_limits.h"
#include "include/conn_store.h"
#include "include/buffer_pool.h"
#include "include/http_pipeline.h"
#include "include/worker_ctx.h"
#include "include/accept_controller.h"
#include "include/timing_wheel.h"
#include "include/ring_ops.h"
#include "include/conn_close.h"
#include "include/http_boundary.h"
#include "include/tx.h"
#include "include/rx_stash.h"
#include "include/rx_buffers.h"
#include "include/conn_deadline.h"
#include "include/config_ini.h"
#include "include/access_log.h"
#include "include/net_utils.h"
#include "include/static_serve_utils.h"
#include "include/time_utils.h"
#include "include/worker_loop.h"
#include "include/tls.h"
#include "include/itest_echo.h"
#include "include/version.h"

#define DEFAULT_PORT SERVER_DEFAULT_PORT
#define BACKLOG SERVER_BACKLOG
#define QUEUE_DEPTH IOURING_QUEUE_DEPTH
#define PRE_ACCEPTS CONFIG_PRE_ACCEPTS

#ifndef ENABLE_SOCK_SNDBUF
#define ENABLE_SOCK_SNDBUF 0
#endif
#ifndef ENABLE_SOCK_RCVBUF
#define ENABLE_SOCK_RCVBUF 0
#endif
#ifndef ENABLE_TCP_NODELAY
#define ENABLE_TCP_NODELAY 0
#endif
#ifndef ENABLE_TCP_NOTSENT_LOWAT
#define ENABLE_TCP_NOTSENT_LOWAT 0
#endif

#define SOCK_SND_BUF CONFIG_SOCK_SND_BUF
#define SOCK_RCV_BUF CONFIG_SOCK_RCV_BUF

#define TCP_NOTSENT_LOWAT_VAL CONFIG_TCP_NOTSENT_LOWAT

static volatile sig_atomic_t g_running = 1;
static volatile sig_atomic_t g_tls_reload_requested = 0;
static volatile sig_atomic_t g_startup_failed = 0;
enum shutdown_state {
  SHUTDOWN_RUNNING = 0,
  SHUTDOWN_DRAIN,
  SHUTDOWN_FORCE,
};
static volatile sig_atomic_t g_shutdown_state = SHUTDOWN_RUNNING;
static volatile sig_atomic_t g_shutdown_signal_count = 0;
static unsigned g_shutdown_grace_ms = 5000u;
static volatile uint64_t g_active_conns_global = 0;
static const char *shutdown_state_name(int state);

static inline uint64_t active_conns_inc(void) {
  return __sync_add_and_fetch((uint64_t *)&g_active_conns_global, 1);
}

static inline uint64_t active_conns_dec(void) {
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

static const char *shutdown_state_name(int state) {
  switch (state) {
  case SHUTDOWN_RUNNING:
    return "running";
  case SHUTDOWN_DRAIN:
    return "drain";
  case SHUTDOWN_FORCE:
    return "force";
  default:
    return "unknown";
  }
}

static inline uint64_t active_conns_total(void) {
  return __sync_add_and_fetch((uint64_t *)&g_active_conns_global, 0);
}

const char RESP_400[] = "HTTP/1.1 400 Bad Request\r\n"
                        "Content-Length: 0\r\n"
                        "Connection: close\r\n"
                        "\r\n";
const char RESP_403[] = "HTTP/1.1 403 Forbidden\r\n"
                        "Content-Length: 0\r\n"
                        "Connection: close\r\n"
                        "\r\n";
const char RESP_404[] = "HTTP/1.1 404 Not Found\r\n"
                        "Content-Length: 0\r\n"
                        "Connection: close\r\n"
                        "\r\n";
const char RESP_405[] = "HTTP/1.1 405 Method Not Allowed\r\n"
                        "Content-Length: 0\r\n"
                        "Connection: close\r\n"
                        "\r\n";
const char RESP_413[] = "HTTP/1.1 413 Payload Too Large\r\n"
                        "Content-Length: 0\r\n"
                        "Connection: close\r\n"
                        "\r\n";
const char RESP_431[] = "HTTP/1.1 431 Request Header Fields Too Large\r\n"
                        "Content-Length: 0\r\n"
                        "Connection: close\r\n"
                        "\r\n";
const char RESP_501[] = "HTTP/1.1 501 Not Implemented\r\n"
                        "Content-Length: 0\r\n"
                        "Connection: close\r\n"
                        "\r\n";
const char RESP_408[] = "HTTP/1.1 408 Request Timeout\r\n"
                        "Content-Length: 0\r\n"
                        "Connection: close\r\n"
                        "\r\n";

const char RESP_500[] = "HTTP/1.1 500 Internal Server Error\r\n"
                        "Content-Length: 0\r\n"
                        "Connection: close\r\n"
                        "\r\n";

#ifndef ENABLE_ITEST_ECHO
#define ENABLE_ITEST_ECHO 0
#endif

#if ENABLE_OVERLOAD_503
const char RESP_503[] = "HTTP/1.1 503 Service Unavailable\r\n"
                        "Content-Length: 0\r\n"
                        "Connection: close\r\n"
                        "\r\n";
#endif

const size_t RESP_400_len = sizeof(RESP_400) - 1;
const size_t RESP_403_len = sizeof(RESP_403) - 1;
const size_t RESP_404_len = sizeof(RESP_404) - 1;
const size_t RESP_405_len = sizeof(RESP_405) - 1;
const size_t RESP_413_len = sizeof(RESP_413) - 1;
const size_t RESP_431_len = sizeof(RESP_431) - 1;
const size_t RESP_501_len = sizeof(RESP_501) - 1;
const size_t RESP_408_len = sizeof(RESP_408) - 1;
const size_t RESP_500_len = sizeof(RESP_500) - 1;
#if ENABLE_OVERLOAD_503
const size_t RESP_503_len = sizeof(RESP_503) - 1;
#endif

static inline struct conn *conn_alloc(void);
static inline void conn_recycle(struct conn *c);

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

static void handle_signal(int sig) {
  UNUSED(sig);
  if (g_shutdown_signal_count == 0) {
    g_shutdown_signal_count = 1;
    g_shutdown_state = SHUTDOWN_DRAIN;
  } else {
    g_shutdown_signal_count = 2;
    g_shutdown_state = SHUTDOWN_FORCE;
    g_running = 0;
  }
}

static void handle_reload_signal(int sig) {
  UNUSED(sig);
  g_tls_reload_requested = 1;
}

static int make_wake_pipe(int pipefd[2]) {
  if (pipe(pipefd) < 0) {
    LOGE(LOGC_CORE, "pipe() failed: %s", strerror(errno));
    return -1;
  }
  (void)net_set_nonblock(pipefd[0]);
  (void)net_set_nonblock(pipefd[1]);
  return 0;
}

static void wake_workers_via_pipe(int wake_wr, int threads) {
  if (wake_wr < 0 || threads <= 0) {
    return;
  }

  // Shared wake pipe needs one byte per worker to ensure all poll waiters wake.
  for (int i = 0; i < threads; ++i) {
    char x = 'X';
    ssize_t wrote = write(wake_wr, &x, 1);
    if (wrote > 0) {
      continue;
    }
    if (wrote == 0) {
      continue;
    }
    if (errno == EAGAIN || errno == EINTR) {
      continue;
    }
    LOGE(LOGC_CORE, "wake write error (shared): %s", strerror(errno));
  }
}

static void wake_workers_via_pipes(const int *wake_wrs, int threads) {
  if (!wake_wrs || threads <= 0) {
    return;
  }

  // One byte per worker-specific pipe is enough.
  for (int i = 0; i < threads; ++i) {
    int wake_wr = wake_wrs[i];
    if (wake_wr < 0) {
      continue;
    }

    char x = 'X';
    ssize_t wrote = write(wake_wr, &x, 1);
    if (wrote > 0) {
      continue;
    }
    if (wrote == 0) {
      continue;
    }
    if (errno == EAGAIN || errno == EINTR) {
      continue;
    }
    LOGE(LOGC_CORE, "wake write error (worker=%d): %s", i, strerror(errno));
  }
}

enum wake_pipe_mode {
  WAKE_PIPE_SHARED = 0,
  WAKE_PIPE_PER_WORKER,
};

static enum wake_pipe_mode parse_wake_pipe_mode(const struct config_t *cfg) {
  if (cfg && (cfg->g.present & GF_WAKE_PIPE_MODE)) {
    return (cfg->g.wake_pipe_mode == 1u) ? WAKE_PIPE_PER_WORKER : WAKE_PIPE_SHARED;
  }

  return WAKE_PIPE_SHARED;
}

// Minimal heuristic for per-thread queue depth with INI/env precedence
static unsigned calc_queue_depth(const struct config_t *cfg) {
  if (cfg && cfg->g.present & GF_QUEUE_DEPTH) {
    return cfg->g.queue_depth;
  }

  const char *qd_s = getenv("QUEUE_DEPTH");
  if (qd_s && *qd_s) {
    long v = strtol(qd_s, NULL, 10);
    if (v > 0 && v < (1 << 20)) {
      return (unsigned)v;
    }
  }

  unsigned pre = (cfg && cfg->g.present & GF_PRE_ACCEPTS) ? cfg->g.pre_accepts : PRE_ACCEPTS;
    unsigned base = pre + 3 * 256 + 64;
  if (base < QUEUE_DEPTH) {
    base = QUEUE_DEPTH;
  }
  return base;
}

static inline void submit_direct(struct worker_ctx *w) {
  CTR_INC_DEV(w, cnt_submit_direct);
  (void)io_uring_submit(&w->ring);
}

// Coarse connection state classification used for deadline decisions.
enum conn_state {
  CONN_STATE_NONE = 0,
  CONN_STATE_KA_IDLE,
  CONN_STATE_INITIAL_IDLE,
  CONN_STATE_HEADER_IN_PROGRESS,
  CONN_STATE_DRAINING,
  CONN_STATE_CLOSING_NO_DEADLINE,
};

static inline enum conn_state conn_get_state(const struct conn *c) {
  if (!c || c->fd < 0) {
    return CONN_STATE_NONE;
  }
  if (c->dl.draining) {
    return CONN_STATE_DRAINING;
  }
  if (conn_is_closing_no_deadline(c)) {
    return CONN_STATE_CLOSING_NO_DEADLINE;
  }
  if (conn_is_keepalive_idle(c)) {
    return CONN_STATE_KA_IDLE;
  }
  if (conn_is_initial_idle(c)) {
    return CONN_STATE_INITIAL_IDLE;
  }
  if (conn_is_header_in_progress(c)) {
    return CONN_STATE_HEADER_IN_PROGRESS;
  }
  return CONN_STATE_NONE;
}

static inline struct conn *conn_get(int fd) {
  if (fd < 0) {
    return NULL;
  }
  return conn_store_get(fd);
}

static inline void worker_active_add(struct worker_ctx *w, int fd) {
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

static inline void worker_active_remove(struct worker_ctx *w, int fd) {
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

static void apply_globals_logging(const struct config_t *cfg) {
  if (!cfg) {
    return;
  }
  if (cfg->g.present & GF_LOG_LEVEL) {
    log_set_level(cfg->g.log_level);
  }
  if (cfg->g.present & GF_LOG_CATEGORIES) {
    log_set_categories(cfg->g.log_categories);
  }
}

static const struct vhost_t *vhost_for_fd(const struct config_t *cfg, int fd);

static int conn_init(struct worker_ctx *w, int fd) {
  struct conn *c = conn_alloc();
  if (!c) {
    return -1;
  }

  http_parser_init(&c->h1.parser, &w->http_settings);
  c->h1.parser.data = c;

  req_arena_init(&c->h1.arena);

  c->vhost = (w && w->cfg.config) ? vhost_for_fd(w->cfg.config, fd) : NULL;

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

static void conn_reset_request(struct worker_ctx *w, struct conn *c) {
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

static inline void tx_close_file(struct conn *c) {
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

static inline void conn_ref(struct conn *c);
static inline void post_recv_ptr(struct worker_ctx *w, struct conn *c);
int schedule_or_sync_close(struct worker_ctx *w, int fd);
static int conn_try_process_stash(struct worker_ctx *w, struct conn *c, int cfd);
static void write_handle_cqe(struct worker_ctx *w, struct conn *c, int cfd, int res);
static void http_apply_action(struct worker_ctx *w,
                              struct conn *c,
                              int cfd,
                              struct http_pipeline_result hres,
                              const char *chunk,
                              size_t chunk_len);
static inline struct worker_loop_read_ops worker_loop_build_read_ops(void);
static inline struct worker_loop_tls_write_ops worker_loop_build_tls_write_ops(void);
static inline struct worker_loop_tls_hs_ops worker_loop_build_tls_hs_ops(void);

static inline void stage_header_response_send(struct worker_ctx *w,
                                              struct conn *c,
                                              int cfd,
                                              struct io_uring_sqe *sqe_w,
                                              enum resp_kind kind,
                                              int keepalive,
                                              int drain_after_headers,
                                              int close_after_send) {
  if (!w || !c) {
    return;
  }

  // During graceful drain, do not allow keep-alive reuse for terminal responses.
  const int force_close = (g_shutdown_state == SHUTDOWN_DRAIN);
  const int effective_keepalive = force_close ? 0 : keepalive;
  const int effective_close_after_send = force_close ? 1 : close_after_send;
  if (force_close && keepalive) {
    CTR_INC_DEV(w, cnt_shutdown_drain_ka_suppressed);
  }

  struct request_response_plan plan = request_build_response_plan(kind,
                                                                  effective_keepalive,
                                                                  drain_after_headers,
                                                                  effective_close_after_send);
  struct tx_next_io out = {0};
  (void)tx_begin_headers(&c->tx,
                         plan.kind,
                         plan.response.buf,
                         plan.response.len,
                         plan.keepalive,
                         plan.drain_after_headers,
                         &out);

  if (plan.close_after_send) {
    conn_prepare_close_after_send(w, c);
  }

  if (c->tls_enabled) {
    struct worker_loop_tls_write_ops tops = worker_loop_build_tls_write_ops();
    (void)worker_loop_tls_try_send_pending(w, c, cfd, &tops);
    return;
  }

  if (!sqe_w) {
    return;
  }

  conn_ref(c);
  io_uring_prep_send(sqe_w, cfd, out.buf, out.len, MSG_NOSIGNAL);
  io_uring_sqe_set_data64(sqe_w, (uint64_t)(uintptr_t)&c->op_write);
  mark_post(w);
}

static inline void stage_tx_buffer_send(struct worker_ctx *w,
                                        struct conn *c,
                                        int cfd,
                                        struct io_uring_sqe *sqe_w) {
  if (!w || !c) {
    return;
  }

  if (c->tls_enabled) {
    struct worker_loop_tls_write_ops tops = worker_loop_build_tls_write_ops();
    (void)worker_loop_tls_try_send_pending(w, c, cfd, &tops);
    return;
  }

  if (!sqe_w) {
    return;
  }

  conn_ref(c);
  io_uring_prep_send(sqe_w, cfd, c->tx.write_buf, c->tx.write_len, MSG_NOSIGNAL);
  io_uring_sqe_set_data64(sqe_w, (uint64_t)(uintptr_t)&c->op_write);
  mark_post(w);
}

static void http_apply_action(struct worker_ctx *w,
                              struct conn *c,
                              int cfd,
                              struct http_pipeline_result hres,
                              const char *chunk,
                              size_t chunk_len) {
  struct http_apply_plan plan = http_pipeline_build_apply_plan(c, &hres);

  // Internal server error (e.g. OOM during request capture): respond 500 and close.
  // This should take precedence over client-induced parse errors.
  if (plan.kind == HP_APPLY_INTERNAL_ERROR) {
    struct http_error_plan ieplan = plan.error;

    struct io_uring_sqe *sqe_w = c->tls_enabled ? NULL : get_sqe_batching(w);
    if (!c->tls_enabled && !sqe_w) {
      CTR_INC_DEV(w, cnt_sqe_starvation_close);
      schedule_or_sync_close(w, cfd);
      return;
    }

    stage_header_response_send(w,
                               c,
                               cfd,
                               sqe_w,
                               ieplan.kind,
                               ieplan.keepalive,
                               ieplan.drain_after_headers,
                               ieplan.close_after_send);
    return;
  }

  // Instrumentation and logging based on HTTP pipeline transitions
  if (hres.header_too_big_transition) {
    CTR_INC_DEV(w, cnt_431);
    const char *reason = c->h1.header_fields_too_many ? "fields" : "bytes";
    size_t cap = c->h1.header_fields_too_many ? (size_t)c->h1.hdr_fields_max : (size_t)HEADER_CAP;
    size_t parsed =
      c->h1.header_fields_too_many ? (size_t)c->h1.hdr_fields_count : c->h1.parser_bytes;
    LOGW(LOGC_HTTP,
         "fd=%u gen=%u: 431 reason=%s cap=%zu parsed=%zu headers_done=%d",
         (unsigned)c->fd,
         (unsigned)c->generation,
         reason,
         cap,
         parsed,
         c->h1.headers_done);
  }

  if (hres.parse_error_transition) {
    CTR_INC_DEV(w, cnt_400_llhttp);
    const char *ename = llhttp_errno_name(hres.err);
    const char *reason = llhttp_get_error_reason(&c->h1.parser);
    size_t preview = chunk_len < 64 ? chunk_len : 64;
    LOGW(LOGC_HTTP,
         "400 llhttp_err=%d(%s) reason=\"%s\" bytes=%zu headers_done=%d "
         "chunk_len=%zu preview=\"%.*s\" rx_stash_len=%u rx_tail_len=%u "
         "closing=%d draining=%d keepalive=%d resp_kind=%d",
         (int)hres.err,
         ename ? ename : "?",
         reason ? reason : "?",
         c->h1.parser_bytes,
         c->h1.headers_done,
         (size_t)chunk_len,
         (int)preview,
         chunk ? chunk : "",
         (unsigned)c->rx_stash_len,
         (unsigned)c->rx_tail_len,
         c->dl.closing,
         c->dl.draining,
         c->tx.keepalive,
         (int)c->tx.resp_kind);
  } else if (hres.tolerated_error && hres.err != HPE_OK) {
    const char *ename = llhttp_errno_name(hres.err);
    const char *reason = llhttp_get_error_reason(&c->h1.parser);
    LOGD_EVERY_N(LOGC_HTTP,
                 64,
                 "llhttp transient err=%d(%s) reason=\"%s\" bytes=%zu",
                 (int)hres.err,
                 ename ? ename : "?",
                 reason ? reason : "?",
                 c->h1.parser_bytes);
  }

  if (hres.headers_complete_transition) {
    CTR_INC_DEV(w, cnt_headers_complete);
  }

  // For successful requests, clear any old KA/header deadline from the wheel.
  // (State becomes NONE once headers_done is set.)
  if (plan.reschedule_on_ok) {
    tw_reschedule(w, c, w->now_cached_ms);
  }

  if (plan.kind == HP_APPLY_ERROR) {
    struct http_error_plan eplan = plan.error;
    struct io_uring_sqe *sqe_w = c->tls_enabled ? NULL : get_sqe_batching(w);
    if (!c->tls_enabled && !sqe_w) {
      CTR_INC_DEV(w, cnt_sqe_starvation_close);
      schedule_or_sync_close(w, cfd);
    } else {
      if (eplan.kind == RK_431) {
        conn_prepare_431_draining(w, c, c->dl.last_active_ms, w->now_cached_ms);
      }

      stage_header_response_send(w,
                                 c,
                                 cfd,
                                 sqe_w,
                                 eplan.kind,
                                 eplan.keepalive,
                                 eplan.drain_after_headers,
                                 eplan.close_after_send);
    }
  } else if (plan.kind == HP_APPLY_CONTINUE) {
    if (!conn_is_closing_no_deadline(c)) {
      // Do NOT reset pending_line_error here; keep it sticky until LF or headers_complete
      post_recv_ptr(w, c);
    }
    // refresh header-related deadline (initial idle or header timeout)
    tw_reschedule(w, c, w->now_cached_ms);
  } else {
    struct io_uring_sqe *sqe_w = c->tls_enabled ? NULL : get_sqe_batching(w);
    if (!c->tls_enabled && !sqe_w) {
      CTR_INC_DEV(w, cnt_sqe_starvation_close);
      schedule_or_sync_close(w, cfd);
    } else {
      struct http_ok_plan okplan = plan.ok;

      // During drain we finish one in-flight request and then close.
      if (g_shutdown_state == SHUTDOWN_DRAIN) {
        if (okplan.keepalive) {
          CTR_INC_DEV(w, cnt_shutdown_drain_ka_suppressed);
        }
        okplan.keepalive = 0;
        c->h1.want_keepalive = 0;
      }

#if ENABLE_ITEST_ECHO
      if (itest_echo_try_prepare_response(c)) {
        stage_tx_buffer_send(w, c, cfd, sqe_w);
        return;
      }
#endif

      struct request_route_plan route_plan = request_build_route_plan(c);
      int static_open_err = 0;

      // Static docroot-backed serving: if DOCROOT is configured, map path_norm to a file.
      const struct vhost_t *vh = c->vhost;
      if (route_plan.try_static) {
        if (static_serve_try_prepare_docroot_response(c, vh->docroot_fd, &static_open_err)) {
          stage_tx_buffer_send(w, c, cfd, sqe_w);
          return;
        }
      }

      struct request_response_plan ok_response =
        request_build_response_plan(okplan.kind,
                                    okplan.keepalive,
                                    /*drain_after_headers=*/0,
                                    okplan.close_after_send);
      static_open_err = request_static_open_err_finalize(static_open_err);
      struct request_static_outcome static_outcome =
        request_build_static_outcome(&route_plan, static_open_err);
      struct request_route_apply_plan terminal_plan =
        request_build_route_apply_plan(c, ok_response, static_outcome);
      if (!terminal_plan.send_terminal_response) {
        return;
      }
      c->h1.want_keepalive = terminal_plan.terminal_response.keepalive;
      stage_header_response_send(w,
                                 c,
                                 cfd,
                                 sqe_w,
                                 terminal_plan.terminal_response.kind,
                                 terminal_plan.terminal_response.keepalive,
                                 terminal_plan.terminal_response.drain_after_headers,
                                 terminal_plan.terminal_response.close_after_send);
    }
  }
}

static int conn_try_process_stash(struct worker_ctx *w, struct conn *c, int cfd) {
  if (!w || !c || cfd < 0) {
    return 0;
  }
  if (c->rx_stash_len == 0) {
    return 0;
  }
  if (!c->rx_stash) {
    return 0;
  }
  if (c->dl.closing || c->dl.draining) {
    return 0;
  }

  // Treat stash as already-received bytes becoming visible now.
  conn_mark_activity(c, w->now_cached_ms);
  if (!c->h1.headers_done && c->h1.parser_bytes == 0) {
    c->dl.header_start_ms = w->now_cached_ms;
    c->dl.header_start_us = time_now_us_monotonic();
  }

  size_t n = (size_t)c->rx_stash_len;
  struct http_pipeline_result agg;
  memset(&agg, 0, sizeof(agg));
  agg.action = HP_ACTION_CONTINUE;
  agg.err = HPE_OK;

  size_t consumed = 0;

  // Stage 1: headers (boundary-limited)
  if (!c->h1.headers_done && !c->h1.parse_error && !c->h1.header_too_big && !c->h1.unsupported_te) {
    size_t feed_len = n;
    size_t unused_tail = 0;
    conn_split_on_header_end(c, c->rx_stash, n, &feed_len, &unused_tail);

    if (feed_len > 0) {
      struct http_pipeline_result r1 = http_pipeline_feed(c, c->rx_stash, feed_len);
      agg.header_too_big_transition |= r1.header_too_big_transition;
      agg.parse_error_transition |= r1.parse_error_transition;
      agg.headers_complete_transition |= r1.headers_complete_transition;
      agg.tolerated_error |= r1.tolerated_error;
      if (r1.err != HPE_OK) {
        agg.err = r1.err;
      }

      conn_rx_tail_update_after_feed(c, c->rx_stash, feed_len);

      // If llhttp paused at the boundary, consume only up to error_pos so
      // pipelined bytes remain for the next request.
      size_t used = feed_len;
      if (r1.err == HPE_PAUSED) {
        const char *pos = llhttp_get_error_pos(&c->h1.parser);
        const char *base = c->rx_stash;
        if (pos && pos >= base && pos <= base + feed_len) {
          used = (size_t)(pos - base);
        }
      }
      consumed = used;
    }
  }

  // Stage 2: body bytes (Content-Length or chunked)
  if (consumed < n && c->h1.headers_done && !c->h1.message_done && !c->h1.parse_error
      && !c->h1.header_too_big && !c->h1.unsupported_te) {
    size_t avail = n - consumed;
    bool is_chunked = (c->h1.te_count > 0 && c->h1.te_chunked && !c->h1.te_other);
    if (is_chunked) {
      size_t body_feed = avail;
      if (body_feed > 0) {
        struct http_pipeline_result r2 = http_pipeline_feed(c, c->rx_stash + consumed, body_feed);
        agg.header_too_big_transition |= r2.header_too_big_transition;
        agg.parse_error_transition |= r2.parse_error_transition;
        agg.headers_complete_transition |= r2.headers_complete_transition;
        agg.tolerated_error |= r2.tolerated_error;
        if (r2.err != HPE_OK) {
          agg.err = r2.err;
        }

        size_t used = body_feed;
        if (r2.err == HPE_PAUSED) {
          const char *pos = llhttp_get_error_pos(&c->h1.parser);
          const char *base = c->rx_stash + consumed;
          // llhttp records `error_pos` as the current pointer when pausing.
          // For on_message_complete pause this is effectively the first byte
          // of the next pipelined request (or endp), so do NOT +1.
          if (pos && pos >= base && pos <= base + body_feed) {
            used = (size_t)(pos - base);
          }
        }
        consumed += used;
      }
    } else {
      uint64_t rem = c->h1.body_remaining;
      size_t body_feed = (rem < (uint64_t)avail) ? (size_t)rem : avail;
      if (body_feed > 0) {
        struct http_pipeline_result r2 = http_pipeline_feed(c, c->rx_stash + consumed, body_feed);
        agg.header_too_big_transition |= r2.header_too_big_transition;
        agg.parse_error_transition |= r2.parse_error_transition;
        agg.headers_complete_transition |= r2.headers_complete_transition;
        agg.tolerated_error |= r2.tolerated_error;
        if (r2.err != HPE_OK) {
          agg.err = r2.err;
        }

        if (c->h1.body_remaining >= (uint64_t)body_feed) {
          c->h1.body_remaining -= (uint64_t)body_feed;
        } else {
          c->h1.body_remaining = 0;
        }
        consumed += body_feed;
      }
    }
  }

  agg.action = http_pipeline_classify_action(c);

  http_apply_action(w, c, cfd, agg, c->rx_stash, consumed ? consumed : n);

  conn_rx_stash_consume(c, consumed);
  return (agg.action != HP_ACTION_CONTINUE);
}

static inline void conn_ref(struct conn *c);
static inline void conn_put(struct conn *c);
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

static inline void conn_ref(struct conn *c) {
  if (c) {
    c->refcnt++;
  }
}

static inline struct conn *conn_alloc(void) {
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

static inline void conn_recycle(struct conn *c) {
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

static inline void conn_put(struct conn *c) {
  if (!c) {
    return;
  }
  if (--c->refcnt == 0) {
    conn_recycle(c);
  }
}

static inline void str_to_lower(char *s, size_t n) {
  for (size_t i = 0; i < n; ++i) {
    unsigned char c = (unsigned char)s[i];
    if (c >= 'A' && c <= 'Z') {
      s[i] = (char)(c + 32);
    }
  }
}

static inline void conn_arm_recv(struct conn *c) {
  if (c) {
    c->tx.recv_armed = 1;
  }
}
static inline void conn_disarm_recv(struct conn *c) {
  if (c) {
    c->tx.recv_armed = 0;
  }
}

static inline void post_recv_ptr(struct worker_ctx *w, struct conn *c);

static void read_handle_cqe(struct worker_ctx *w,
                            struct conn *c,
                            int cfd,
                            struct io_uring_cqe *cqe) {
  const struct worker_loop_read_ops rops = worker_loop_build_read_ops();

  if (!w || !cqe) {
    return;
  }

  if (!c || cfd < 0 || c->dl.closing) {
    if (c) {
      conn_put(c);
    }
    io_uring_cqe_seen(&w->ring, cqe);
    return;
  }

  if (conn_tls_handshake_pending(c)) {
    conn_disarm_recv(c);
    const struct worker_loop_tls_hs_ops hs_ops = worker_loop_build_tls_hs_ops();

    if (cqe->res < 0 && cqe->res != -EAGAIN && cqe->res != -EINTR) {
      schedule_or_sync_close(w, cfd);
    } else {
      conn_mark_activity(c, w->now_cached_ms);
      (void)worker_loop_tls_handshake_progress(w, c, cfd, &hs_ops);
    }

    io_uring_cqe_seen(&w->ring, cqe);
    conn_put(c);
    return;
  }

  if (c->tls_enabled) {
    const struct worker_loop_tls_write_ops tops = worker_loop_build_tls_write_ops();
    worker_loop_tls_read_handle_cqe(w, c, cfd, cqe, &rops, &tops);
    return;
  }

  worker_loop_read_handle_cqe(w, c, cfd, cqe, &rops);
}

static inline struct worker_loop_read_ops worker_loop_build_read_ops(void) {
  return (struct worker_loop_read_ops){
    .is_closing_no_deadline = conn_is_closing_no_deadline,
    .post_recv_ptr = post_recv_ptr,
    .schedule_or_sync_close = schedule_or_sync_close,
    .mark_activity = conn_mark_activity,
    .split_on_header_end = conn_split_on_header_end,
    .rx_tail_update_after_feed = conn_rx_tail_update_after_feed,
    .rx_stash_append = conn_rx_stash_append,
    .apply_action = http_apply_action,
    .conn_put = conn_put,
  };
}

static inline struct worker_loop_write_ops worker_loop_build_write_ops(void) {
  return (struct worker_loop_write_ops){
    .maybe_flush = maybe_flush,
    .arm_write_timeout = conn_arm_write_timeout,
    .clear_write_timeout = conn_clear_write_timeout,
    .tx_close_file = tx_close_file,
    .emit_access_log = access_log_emit_from_conn,
    .post_recv_ptr = post_recv_ptr,
    .conn_reset_request = conn_reset_request,
    .conn_try_process_stash = conn_try_process_stash,
    .tw_reschedule = tw_reschedule,
    .conn_ref = conn_ref,
    .conn_put = conn_put,
    .schedule_or_sync_close = schedule_or_sync_close,
  };
}

static inline struct worker_loop_tls_write_ops worker_loop_build_tls_write_ops(void) {
  return (struct worker_loop_tls_write_ops){
    .arm_write_timeout = conn_arm_write_timeout,
    .post_tls_pollout_ptr = worker_loop_post_tls_pollout,
    .is_closing_no_deadline = conn_is_closing_no_deadline,
    .post_recv_ptr = post_recv_ptr,
    .schedule_or_sync_close = schedule_or_sync_close,
    .write_handle_cqe = write_handle_cqe,
  };
}

static inline struct worker_loop_tls_hs_ops worker_loop_build_tls_hs_ops(void) {
  return (struct worker_loop_tls_hs_ops){
    .is_closing_no_deadline = conn_is_closing_no_deadline,
    .post_recv_ptr = post_recv_ptr,
    .post_tls_pollout_ptr = worker_loop_post_tls_pollout,
    .tw_reschedule = tw_reschedule,
    .schedule_or_sync_close = schedule_or_sync_close,
  };
}

static void write_handle_cqe(struct worker_ctx *w, struct conn *c, int cfd, int res) {
  const struct worker_loop_write_ops wops = worker_loop_build_write_ops();

  worker_loop_write_handle_cqe(w, c, cfd, res, &wops);
}

static void write_ready_handle_cqe(struct worker_ctx *w, struct conn *c, int cfd) {
  if (conn_tls_handshake_pending(c)) {
    tx_notify_poll_disarmed_staged(&c->tx);
    conn_mark_activity(c, w->now_cached_ms);
    const struct worker_loop_tls_hs_ops hs_ops = worker_loop_build_tls_hs_ops();
    (void)worker_loop_tls_handshake_progress(w, c, cfd, &hs_ops);
    return;
  }

  if (c && c->tls_enabled) {
    struct worker_loop_tls_write_ops tops = worker_loop_build_tls_write_ops();
    worker_loop_tls_write_ready_handle_cqe(w, c, cfd, conn_mark_activity, &tops);
    return;
  }

  const struct worker_loop_write_ops wops = worker_loop_build_write_ops();

  worker_loop_write_ready_handle_cqe(w, c, cfd, &wops);
}

static void worker_loop_conn_write(struct worker_ctx *w, struct conn *c, int cfd, int res) {
  write_handle_cqe(w, c, cfd, res);
}

static void worker_loop_conn_write_ready(struct worker_ctx *w, struct conn *c, int cfd) {
  write_ready_handle_cqe(w, c, cfd);
}

static void worker_loop_conn_read(struct worker_ctx *w,
                                  struct conn *c,
                                  int cfd,
                                  struct io_uring_cqe *cqe) {
  read_handle_cqe(w, c, cfd, cqe);
}

static void worker_loop_conn_put_cb(struct conn *c) {
  conn_put(c);
}

static inline void post_recv_ptr(struct worker_ctx *w, struct conn *c) {
  if (!w || !c) {
    return;
  }
  if (c->fd < 0) {
    return;
  }
  if (conn_is_closing_no_deadline(c)) {
    return;
  }
  if (c->tx.recv_armed) {
    return;
  }

  struct io_uring_sqe *sqe = get_sqe_batching(w);
  if (!sqe) {
    return;
  }

  if (c->tls_enabled) {
    conn_ref(c);
    io_uring_prep_poll_add(sqe, c->fd, POLLIN);
    io_uring_sqe_set_data64(sqe, (uint64_t)(uintptr_t)&c->op_read);
    conn_arm_recv(c);
    mark_post(w);
    CTR_INC_DEV(w, cnt_read_ptr_posts);
    return;
  }

  conn_ref(c);
  io_uring_prep_recv(sqe, c->fd, NULL, RBUF_SZ, 0);
  sqe->flags = 0;
  sqe->buf_group = buffer_pool_group_id();
  sqe->flags |= IOSQE_BUFFER_SELECT;
  io_uring_sqe_set_data64(sqe, (uint64_t)(uintptr_t)&c->op_read);

  conn_arm_recv(c);

  mark_post(w);
  CTR_INC_DEV(w, cnt_read_ptr_posts);
}

static inline unsigned parse_zone_scope_id(const char *zone) {
  if (!zone || !*zone) {
    return 0;
  }
  char *end = NULL;
  unsigned long v = strtoul(zone, &end, 10);
  if (end && *end == '\0') {
    if (v > 0xfffffffful) {
      return 0;
    }
    return (unsigned)v;
  }
  return if_nametoindex(zone);
}

static const struct vhost_t *vhost_for_fd(const struct config_t *cfg, int fd) {
  struct sockaddr_storage ss;
  socklen_t len = sizeof(ss);
  if (getsockname(fd, (struct sockaddr *)&ss, &len) != 0) {
    return NULL;
  }

  uint16_t port = 0;
  char addrbuf[INET6_ADDRSTRLEN] = {0};

  int is_linklocal = 0;
  unsigned scope_id = 0;

  if (ss.ss_family == AF_INET) {
    const struct sockaddr_in *sin = (const struct sockaddr_in *)&ss;
    port = ntohs(sin->sin_port);
    if (!inet_ntop(AF_INET, &sin->sin_addr, addrbuf, sizeof(addrbuf))) {
      addrbuf[0] = '\0';
    }
  } else if (ss.ss_family == AF_INET6) {
    const struct sockaddr_in6 *sin6 = (const struct sockaddr_in6 *)&ss;
    port = ntohs(sin6->sin6_port);
    if (IN6_IS_ADDR_V4MAPPED(&sin6->sin6_addr)) {
      struct in_addr v4;
      memcpy(&v4, &sin6->sin6_addr.s6_addr[12], sizeof(v4));
      if (!inet_ntop(AF_INET, &v4, addrbuf, sizeof(addrbuf))) {
        addrbuf[0] = '\0';
      }
    } else {
      is_linklocal = IN6_IS_ADDR_LINKLOCAL(&sin6->sin6_addr) ? 1 : 0;
      scope_id = sin6->sin6_scope_id;
      if (!inet_ntop(AF_INET6, &sin6->sin6_addr, addrbuf, sizeof(addrbuf))) {
        addrbuf[0] = '\0';
      }
    }
  } else {
    return NULL;
  }

  // Prefer exact bind matches over wildcard binds on the same port.
  for (int i = 0; i < cfg->vhost_count; ++i) {
    const struct vhost_t *vh = &cfg->vhosts[i];
    if (vh->port != port) {
      continue;
    }
    if (!is_wildcard_bind(vh->bind) && addrbuf[0] != '\0') {
      if (is_linklocal) {
        // Link-local addresses are zone-sensitive.
        char bind_addr[INET6_ADDRSTRLEN] = {0};
        char bind_zone[64] = {0};
        int has_zone =
          split_zone_id(vh->bind, bind_addr, sizeof(bind_addr), bind_zone, sizeof(bind_zone));
        if (bind_addr[0] != '\0' && strcasecmp(addrbuf, bind_addr) == 0) {
          if (has_zone) {
            unsigned z = parse_zone_scope_id(bind_zone);
            if (z != 0 && z == scope_id) {
              return vh;
            }
          }
        }
      } else {
        char bind_norm[INET6_ADDRSTRLEN] = {0};
        strip_zone_id(vh->bind, bind_norm, sizeof(bind_norm));
        if (bind_norm[0] != '\0' && strcasecmp(addrbuf, bind_norm) == 0) {
          return vh;
        }
      }
    }
  }

  const struct vhost_t *first_wc = NULL;
  for (int i = 0; i < cfg->vhost_count; ++i) {
    const struct vhost_t *vh = &cfg->vhosts[i];
    if (vh->port != port) {
      continue;
    }
    if (is_wildcard_bind(vh->bind)) {
      if (!first_wc) {
        first_wc = vh;
      }
    }
  }
  if (first_wc) {
    return first_wc;
  }
  return NULL;
}

// Accept success hook.
void worker_accept_success(struct worker_ctx *w, int cfd) {
  net_tune_socket_buffers(cfd);

  if (conn_init(w, cfd) != 0) {
    (void)schedule_or_sync_close(w, cfd);
    return;
  }

  struct conn *cacc = conn_get(cfd);
  if (!cacc) {
    return;
  }

  if (!cacc->vhost) {
    LOGW(LOGC_CORE, "fd=%d: no vhost matched local addr/port; closing", cfd);
    (void)schedule_or_sync_close(w, cfd);
    return;
  }

  if (tls_vhost_effective_enabled(w->cfg.config, cacc->vhost)) {
    char terr[256] = {0};
    int tls_init_rc = 0;

    tls_ctx_rdlock();
    struct tls_ctx *ctx = (struct tls_ctx *)cacc->vhost->tls_ctx_handle;
    if (!ctx || tls_conn_init(cacc, ctx, cfd, terr) != 0) {
      tls_init_rc = -1;
    }
    tls_ctx_unlock();

    if (tls_init_rc != 0) {
      LOGE(LOGC_CORE,
           "TLS conn init failed for vhost '%s'%s%s",
           cacc->vhost->name,
           terr[0] ? ": " : "",
           terr[0] ? terr : "");
      (void)schedule_or_sync_close(w, cfd);
      return;
    }

    cacc->dl.header_start_ms = w->now_cached_ms;
    cacc->dl.header_start_us = time_now_us_monotonic();
    const struct worker_loop_tls_hs_ops hs_ops = worker_loop_build_tls_hs_ops();
    if (worker_loop_tls_handshake_progress(w, cacc, cfd, &hs_ops) < 0) {
      return;
    }
    if (conn_tls_handshake_pending(cacc)) {
      return;
    }
  }

  post_recv_ptr(w, cacc);

  if (!cacc->tx.recv_armed && !cacc->dl.closing && cacc->fd >= 0) {
#if ENABLE_OVERLOAD_503
    struct io_uring_sqe *sqe_w = get_sqe_batching(w);
    if (!sqe_w) {
      CTR_INC_DEV(w, cnt_sqe_starvation_close);
      schedule_or_sync_close(w, cfd);
    } else {
      struct response_view rv503 = request_select_response(RK_503, 0);
      struct tx_next_io out = {0};
      (void)tx_begin_headers(&cacc->tx,
                             RK_503,
                             rv503.buf,
                             rv503.len,
                             /*keepalive=*/0,
                             /*drain_after_headers=*/0,
                             &out);
      cacc->dl.last_active_ms = w->now_cached_ms;

      conn_prepare_close_after_send(w, cacc);

      conn_ref(cacc);
      io_uring_prep_send(sqe_w, cfd, out.buf, out.len, MSG_NOSIGNAL);
      io_uring_sqe_set_data64(sqe_w, (uint64_t)(uintptr_t)&cacc->op_write);
      mark_post(w);
    }
#else
    CTR_INC_DEV(w, cnt_sqe_starvation_close);
    (void)schedule_or_sync_close(w, cfd);
#endif
  }
}

static void *worker_main(void *arg) {
  struct worker_ctx *w = (struct worker_ctx *)arg;

  log_set_thread_id(w->cfg.thread_id);
#ifdef __linux__
  if (w->cfg.cpu_core >= 0) {
    cpu_set_t cs;
    CPU_ZERO(&cs);
    CPU_SET(w->cfg.cpu_core, &cs);
    int rc = pthread_setaffinity_np(pthread_self(), sizeof(cs), &cs);
    if (rc != 0) {
      LOGW(LOGC_CORE, "setaffinity: %s", strerror(rc));
    }
  }
#endif

  if (accept_setup_listeners(w) != 0) {
    return NULL;
  }

  struct io_uring_params params;
  memset(&params, 0, sizeof(params));

  const char *env_sqp = getenv("USE_SQPOLL");
  if (env_sqp && env_sqp[0] == '1') {
    params.flags |= IORING_SETUP_SQPOLL;
    params.sq_thread_idle = 2000;

    const char *env_cpu = getenv("USE_SQPOLL_CPU");
    if (env_cpu && *env_cpu) {
      char *endp = NULL;
      long v = strtol(env_cpu, &endp, 10);
      if (endp && *endp == '\0' && v >= 0 && v <= INT_MAX) {
        params.flags |= IORING_SETUP_SQ_AFF;
        params.sq_thread_cpu = (int)v;
      }
    }
  }

  unsigned qd = calc_queue_depth(w->cfg.config);
  int rc = io_uring_queue_init_params(qd, &w->ring, &params);
  if (rc < 0 && (params.flags & IORING_SETUP_SQPOLL)) {
    LOGW(LOGC_IO, "SQPOLL unavailable (%s); retrying without", strerror(-rc));
    memset(&params, 0, sizeof(params));
    rc = io_uring_queue_init_params(qd, &w->ring, &params);
  }
  if (rc < 0) {
    LOGE(LOGC_IO, "io_uring_queue_init: %s", strerror(-rc));
    accept_close_listeners(w);
    return NULL;
  }

  if (conn_store_init(256) != 0) {
    LOGE(LOGC_CORE, "fd→conn map init failed");
    io_uring_queue_exit(&w->ring);
    accept_close_listeners(w);
    return NULL;
  }

  http_parser_settings_assign_server(&w->http_settings);

  uint16_t buf_entries = BUF_RING_ENTRIES;
  uint16_t buf_group_id = (uint16_t)(BUF_GROUP_BASE + w->cfg.thread_id);
  if (buffer_pool_init(&w->ring, buf_entries, buf_group_id) != 0) {
    io_uring_queue_exit(&w->ring);
    accept_close_listeners(w);
    return NULL;
  }

  rx_stash_pool_init(RX_STASH_PREALLOC);

  int accept_armed = accept_arm_startup(w);
  submit_if_pending(w);

  LOGD(LOGC_ACCEPT, "accept_multishot=%d accept_armed=%d", w->accept_multishot, accept_armed);

  if (w->cfg.wake_rd >= 0) {
    w->wake_static.magic = OP_MAGIC;
    w->wake_static.type = OP_WAKE;
    w->wake_static.c = NULL;
    w->wake_static.fd = w->cfg.wake_rd;

    int did_submit = 0;
    struct io_uring_sqe *sqe = get_sqe_retry_once(&w->ring, &did_submit);
    if (did_submit) {
      CTR_INC_DEV(w, cnt_submit_inline);
    }
    if (sqe) {
      io_uring_prep_poll_add(sqe, w->cfg.wake_rd, POLLIN);
      io_uring_sqe_set_data64(sqe, (uint64_t)(uintptr_t)&w->wake_static);
      mark_post(w);
    } else {
      LOGE(LOGC_CORE, "worker %d: failed to arm wake poll op", w->cfg.thread_id);
      g_startup_failed = 1;
      g_running = 0;
      goto worker_cleanup;
    }
    submit_if_pending(w);
  }

  {
    uint64_t now0 = time_now_ms_monotonic();
    tw_init(w, TW_SLOTS, TW_TICK_MS, now0);
  }

  {
    int did_submit = 0;
    struct __kernel_timespec ts;
    ts.tv_sec = w->tw_tick_ms / 1000;
    ts.tv_nsec = (w->tw_tick_ms % 1000) * 1000000;

    w->sweep_static.magic = OP_MAGIC;
    w->sweep_static.type = OP_SWEEP;
    w->sweep_static.c = NULL;
    w->sweep_static.fd = -1;

    struct io_uring_sqe *sqe = get_sqe_retry_once(&w->ring, &did_submit);
    if (did_submit) {
      CTR_INC_DEV(w, cnt_submit_inline);
    }
    if (sqe) {
      io_uring_prep_timeout(sqe, &ts, 0, 0);
      io_uring_sqe_set_data64(sqe, (uint64_t)(uintptr_t)&w->sweep_static);
      mark_post(w);
    } else {
      LOGE(LOGC_CORE, "worker %d: failed to arm sweep timer op", w->cfg.thread_id);
      g_startup_failed = 1;
      g_running = 0;
      goto worker_cleanup;
    }
    submit_if_pending(w);
  }

  while (g_running) {
    struct io_uring_cqe *cqes[CQE_BATCH];
    unsigned cqe_count = 0;
    int ret = worker_loop_fetch_cqes(w, cqes, CQE_BATCH, &cqe_count);
    if (ret < 0) {
      if (ret == -EINTR) {
        continue;
      }
      if (!g_running) {
        break;
      }
      LOGE(LOGC_IO, "wait_cqe: %s", strerror(-ret));
      break;
    }

    w->now_cached_ms = time_now_ms_monotonic();
    access_log_runtime_poll(w);

    const struct worker_loop_conn_handlers conn_handlers = {
      .on_write = worker_loop_conn_write,
      .on_write_ready = worker_loop_conn_write_ready,
      .on_read = worker_loop_conn_read,
      .on_conn_put = worker_loop_conn_put_cb,
    };

    (void)worker_loop_process_cqe_batch(w,
                                        cqes,
                                        cqe_count,
                                        &conn_handlers,
                                        (g_shutdown_state == SHUTDOWN_RUNNING));
    submit_if_pending(w);
  }

worker_cleanup:
  {
    struct io_uring_cqe *cqe2;
    while (io_uring_peek_cqe(&w->ring, &cqe2) == 0) {
      io_uring_cqe_seen(&w->ring, cqe2);
    }
  }

#if INSTRUMENTATION_LEVEL >= LVL_OPS
  LOGI(LOGC_CORE,
       "Thread %d: accept_ok=%lu accept_err=%lu eagain=%lu emfile=%lu enfile=%lu other=%lu "
       "send_err=%lu close_err=%lu 408=%lu pool_size=%u",
       w->cfg.thread_id,
       (unsigned long)CTR_PATH(w, cnt_accept_ok),
       (unsigned long)CTR_PATH(w, cnt_accept_err),
       (unsigned long)CTR_PATH(w, cnt_accept_eagain),
       (unsigned long)CTR_PATH(w, cnt_accept_emfile),
       (unsigned long)CTR_PATH(w, cnt_accept_enfile),
       (unsigned long)CTR_PATH(w, cnt_accept_other),
       (unsigned long)CTR_PATH(w, cnt_send_err),
       (unsigned long)CTR_PATH(w, cnt_close_err),
       (unsigned long)CTR_PATH(w, cnt_408),
       (unsigned)buffer_pool_entries());
#endif

#if INSTRUMENTATION_LEVEL >= LVL_DEV
  LOGI(LOGC_CORE,
       "Thread %d: 431=%lu 400_llhttp=%lu headers_complete=%lu "
       "eof_idle=%lu eof_midreq=%lu buf_noselect=%lu badid=%lu "
       "buf_in_use_peak=%lu sqe_starve_close=%lu write_timeout_abortive_close=%lu "
       "read_ptr_posts=%lu drain_ka_suppressed=%lu",
       w->cfg.thread_id,
       (unsigned long)CTR_PATH(w, cnt_431),
       (unsigned long)CTR_PATH(w, cnt_400_llhttp),
       (unsigned long)CTR_PATH(w, cnt_headers_complete),
       (unsigned long)CTR_PATH(w, cnt_eof_idle),
       (unsigned long)CTR_PATH(w, cnt_eof_midreq),
       (unsigned long)CTR_PATH(w, cnt_buf_noselect),
       (unsigned long)CTR_PATH(w, cnt_buf_badid),
       (unsigned long)CTR_PATH(w, buf_in_use_max),
       (unsigned long)CTR_PATH(w, cnt_sqe_starvation_close),
       (unsigned long)CTR_PATH(w, cnt_write_timeout_abortive_close),
       (unsigned long)CTR_PATH(w, cnt_read_ptr_posts),
       (unsigned long)CTR_PATH(w, cnt_shutdown_drain_ka_suppressed));
#endif
#if INSTRUMENTATION_LEVEL >= LVL_DEV
  {
    unsigned long batched_sqes = (unsigned long)CTR_PATH(w, cnt_sqes_flushed);
    unsigned long batched_calls = (unsigned long)CTR_PATH(w, cnt_submit_calls);
    unsigned long urgent_calls = (unsigned long)CTR_PATH(w, cnt_submit_urgent);

    unsigned long direct_sqes = (unsigned long)CTR_PATH(w, cnt_sqes_direct_posted);
    unsigned long direct_calls = (unsigned long)CTR_PATH(w, cnt_submit_direct);
    unsigned long inline_calls = (unsigned long)CTR_PATH(w, cnt_submit_inline);

    unsigned long total_sqes = batched_sqes + direct_sqes;
    unsigned long total_calls = batched_calls + direct_calls + inline_calls;

    double batched_spc = batched_calls ? (double)batched_sqes / (double)batched_calls : 0.0;
    double overall_spc = total_calls ? (double)total_sqes / (double)total_calls : 0.0;

    long saved_batched = (long)batched_sqes - (long)batched_calls;
    long saved_total = (long)total_sqes - (long)total_calls;

    LOGI(LOGC_CORE,
         "Thread %d batching: submits batched=%lu urgent=%lu direct=%lu inline=%lu | "
         "SQEs batched=%lu direct=%lu | SPC batched=%.2f overall=%.2f | "
         "saved_submits≈ batched=%ld overall=%ld",
         w->cfg.thread_id,
         batched_calls,
         urgent_calls,
         direct_calls,
         inline_calls,
         batched_sqes,
         direct_sqes,
         batched_spc,
         overall_spc,
         saved_batched,
         saved_total);
  }
#endif

#if INSTRUMENTATION_LEVEL >= LVL_DEV
  LOGI(LOGC_CORE,
       "Thread %d TW: ticks=%lu processed=%lu expired=%lu reinserted=%lu "
       "bucket_max=%lu tick_avg=%.1f us tick_max=%lu us",
       w->cfg.thread_id,
       (unsigned long)CTR_PATH(w, cnt_tw_ticks),
       (unsigned long)CTR_PATH(w, cnt_tw_processed),
       (unsigned long)CTR_PATH(w, cnt_tw_expired),
       (unsigned long)CTR_PATH(w, cnt_tw_reinserted),
       (unsigned long)CTR_PATH(w, cnt_tw_bucket_max),
       CTR_PATH(w, cnt_tw_ticks)
         ? ((double)CTR_PATH(w, cnt_tw_tick_us_total) / (double)CTR_PATH(w, cnt_tw_ticks))
         : 0.0,
       (unsigned long)CTR_PATH(w, cnt_tw_tick_us_max));
#endif

#if CONN_FREELIST_STATS
  LOGD(LOGC_CORE,
  "Thread %d freelist: alloc_hits=%" PRIu64 " alloc_misses=%" PRIu64 " recycled=%" PRIu64 " "
  "free_peak=%" PRIu64 " free_count=%zu cap=%u",
       w->cfg.thread_id,
       conn_alloc_hits,
       conn_alloc_misses,
       conn_recycle_calls,
       conn_free_peak,
       (size_t)conn_free_count,
       (unsigned)CONN_FREELIST_CAP);
#endif

#if REQ_ARENA_STATS
  LOGD(LOGC_CORE,
       "Thread %d req_arena: new_chunks=%llu new_bytes=%llu reset_calls=%llu "
       "reset_freed_chunks=%llu reset_freed_bytes=%llu destroy_calls=%llu "
       "destroy_freed_chunks=%llu destroy_freed_bytes=%llu",
       w->cfg.thread_id,
       (unsigned long long)g_req_arena_new_chunks,
       (unsigned long long)g_req_arena_new_chunk_bytes,
       (unsigned long long)g_req_arena_reset_calls,
       (unsigned long long)g_req_arena_reset_freed_chunks,
       (unsigned long long)g_req_arena_reset_freed_bytes,
       (unsigned long long)g_req_arena_destroy_calls,
       (unsigned long long)g_req_arena_destroy_freed_chunks,
       (unsigned long long)g_req_arena_destroy_freed_bytes);

#if REQ_ARENA_CHUNK_CACHE
  LOGD(LOGC_CORE,
       "Thread %d req_arena_cache: hits=%llu misses=%llu put_chunks=%llu put_bytes=%llu "
       "drop_chunks=%llu drop_bytes=%llu peak=%llu count=%u",
       w->cfg.thread_id,
       (unsigned long long)g_req_arena_cache_hits,
       (unsigned long long)g_req_arena_cache_misses,
       (unsigned long long)g_req_arena_cache_put_chunks,
       (unsigned long long)g_req_arena_cache_put_bytes,
       (unsigned long long)g_req_arena_cache_drop_chunks,
       (unsigned long long)g_req_arena_cache_drop_bytes,
       (unsigned long long)g_req_arena_cache_count_peak,
       (unsigned)g_req_arena_chunk_cache_count);

  req_arena_cache_destroy_tls();
  LOGD(LOGC_CORE,
       "Thread %d req_arena_cache_shutdown: freed_chunks=%llu freed_bytes=%llu",
       w->cfg.thread_id,
       (unsigned long long)g_req_arena_cache_shutdown_freed_chunks,
       (unsigned long long)g_req_arena_cache_shutdown_freed_bytes);
#endif
#endif
  buffer_pool_destroy(&w->ring);

  uint64_t rx_stash_total = 0;
  uint64_t rx_stash_inuse = 0;
  rx_stash_pool_get_stats(&rx_stash_total, &rx_stash_inuse);
  LOGD(LOGC_CORE,
       "Thread %d rx_stash_pool: total=%" PRIu64 " inuse=%" PRIu64,
       w->cfg.thread_id,
       rx_stash_total,
       rx_stash_inuse);
  rx_stash_pool_destroy();

  io_uring_queue_exit(&w->ring);
  accept_close_listeners(w);
  free(w->active_fds);
  w->active_fds = NULL;
  w->active_count = 0;
  w->active_cap = 0;

  conn_store_free();

  while (conn_free_list) {
    struct conn *next = conn_free_list->free_next;
    free(conn_free_list);
    conn_free_list = next;
  }
  conn_free_count = 0;

  return NULL;
}
int main(int argc, char **argv) {
  UNUSED(argc);
  UNUSED(argv);

  struct sigaction sa;
  memset(&sa, 0, sizeof(sa));
  sa.sa_handler = handle_signal;
  sigaction(SIGINT, &sa, NULL);
  sigaction(SIGTERM, &sa, NULL);

  struct sigaction sa_reload;
  memset(&sa_reload, 0, sizeof(sa_reload));
  sa_reload.sa_handler = handle_reload_signal;
  sigaction(SIGHUP, &sa_reload, NULL);

  struct sigaction sa_pipe;
  memset(&sa_pipe, 0, sizeof(sa_pipe));
  sa_pipe.sa_handler = SIG_IGN;
  sigaction(SIGPIPE, &sa_pipe, NULL);

  int threads = 2;

  struct config_t config = {0};
  char cerr[256] = {0};
  int tls_ready = 0;
  int startup_failed = 0;

  const char *ini_path = getenv("SERVER_CONFIG");
  if (!ini_path || !*ini_path) {
    ini_path = "server.ini";
  }
  if (config_load_ini(ini_path, &config, cerr) != 0 || config.vhost_count == 0) {
    LOGE(LOGC_CORE, "No vhosts configured in %s: %s", ini_path, cerr);
    return 1;
  }

  if (config.g.present & GF_SHUTDOWN_GRACE_MS) {
    g_shutdown_grace_ms = config.g.shutdown_grace_ms;
  }

  int any_port = 0;
  for (int i = 0; i < config.vhost_count; ++i) {
    if (config.vhosts[i].port) {
      any_port = 1;
      break;
    }
  }
  if (!any_port) {
    LOGE(LOGC_CORE, "No ports configured via INI vhosts");
    return 1;
  }

  if (config.g.present & GF_WORKERS) {
    threads = (int)config.g.workers;
  }

  if (threads < 1) {
    threads = 1;
  }
  if (threads > 128) {
    threads = 128;
  }

  apply_globals_logging(&config);
  log_init_from_env();
  log_set_time_fn(time_now_ms_monotonic);

  LOGI(LOGC_CORE, "lamseryn %s starting", LAMSERYN_VERSION_STRING);
  LOGI(LOGC_CORE, "Configured vhosts=%d", config.vhost_count);
  LOGI(LOGC_CORE,
       "shutdown policy scaffold: grace_ms=%u state=%s",
       g_shutdown_grace_ms,
       shutdown_state_name((int)g_shutdown_state));

  if (access_log_runtime_init(&config.g) != 0) {
    return 1;
  }

  if (tls_global_init() != 0) {
    LOGE(LOGC_CORE, "TLS global init failed");
    return 1;
  }
  tls_ready = 1;

  LOGI(LOGC_CORE, "TLS runtime: %s", tls_runtime_version());

  if (tls_init_vhost_contexts(&config, cerr) != 0) {
    LOGE(LOGC_CORE, "%s", cerr[0] ? cerr : "TLS context initialization failed");
    tls_global_cleanup();
    return 1;
  }

  LOGI(LOGC_CORE, "sizeof(struct conn)=%zu", sizeof(struct conn));

  struct rlimit rl;
  if (getrlimit(RLIMIT_NOFILE, &rl) == 0) {
    rl.rlim_cur = rl.rlim_max;
    setrlimit(RLIMIT_NOFILE, &rl);
  }

  enum wake_pipe_mode wake_mode = parse_wake_pipe_mode(&config);
  int use_shared_wake = (wake_mode == WAKE_PIPE_SHARED);
  LOGI(LOGC_CORE, "wake pipe mode: %s", use_shared_wake ? "shared" : "per-worker");

  int shared_wake_pipe[2] = {-1, -1};
  if (use_shared_wake) {
    if (make_wake_pipe(shared_wake_pipe) != 0) {
      LOGE(LOGC_CORE, "Failed to create shared wake pipe");
      return 1;
    }
  }

  int *wake_rds = NULL;
  int *wake_wrs = NULL;
  if (!use_shared_wake) {
    wake_rds = calloc((size_t)threads, sizeof(*wake_rds));
    wake_wrs = calloc((size_t)threads, sizeof(*wake_wrs));
    if (!wake_rds || !wake_wrs) {
      LOGE(LOGC_CORE, "Failed to allocate wake fd arrays");
      free(wake_rds);
      free(wake_wrs);
      if (shared_wake_pipe[0] >= 0) {
        close(shared_wake_pipe[0]);
      }
      if (shared_wake_pipe[1] >= 0) {
        close(shared_wake_pipe[1]);
      }
      return 1;
    }
    for (int i = 0; i < threads; ++i) {
      wake_rds[i] = -1;
      wake_wrs[i] = -1;
    }
  }

  pthread_t *tids = calloc((size_t)threads, sizeof(pthread_t));

  // struct worker_ctx is explicitly over-aligned (64B) to reduce false sharing.
  // GCC does NOT guarantee sizeof(struct worker_ctx) is a multiple of that alignment,
  // which makes a plain array potentially misaligned for element i>0.
  // Allocate each struct worker_ctx separately with the proper alignment.
  struct worker_ctx **workers = calloc((size_t)threads, sizeof(*workers));

  if (!tids || !workers) {
    LOGE(LOGC_CORE, "Allocation failure");
    free(tids);
    free(workers);
    if (shared_wake_pipe[0] >= 0) {
      close(shared_wake_pipe[0]);
    }
    if (shared_wake_pipe[1] >= 0) {
      close(shared_wake_pipe[1]);
    }
    if (wake_rds) {
      for (int i = 0; i < threads; ++i) {
        if (wake_rds[i] >= 0) {
          close(wake_rds[i]);
        }
      }
    }
    if (wake_wrs) {
      for (int i = 0; i < threads; ++i) {
        if (wake_wrs[i] >= 0) {
          close(wake_wrs[i]);
        }
      }
    }
    free(wake_rds);
    free(wake_wrs);
    return 1;
  }

  int cpu_count = (int)sysconf(_SC_NPROCESSORS_ONLN);
  pthread_attr_t attr;
  pthread_attr_init(&attr);

  for (int i = 0; i < threads; ++i) {
    struct worker_ctx *w = NULL;
    size_t align = (size_t)_Alignof(struct worker_ctx);
    if (align < sizeof(void *)) {
      align = sizeof(void *);
    }
    int prc = posix_memalign((void **)&w, align, sizeof(*w));
    if (prc != 0 || !w) {
      LOGE(LOGC_CORE, "posix_memalign(struct worker_ctx) failed: %s", strerror(prc ? prc : ENOMEM));
      startup_failed = 1;
      threads = i;
      g_running = 0;
      break;
    }
    memset(w, 0, sizeof(*w));
    workers[i] = w;

    workers[i]->cfg.thread_id = i;
    workers[i]->cfg.cpu_core = (cpu_count > 0) ? (i % cpu_count) : -1;
    workers[i]->cfg.config = &config;

    if (use_shared_wake) {
      workers[i]->cfg.wake_rd = shared_wake_pipe[0];
    } else if (wake_rds && wake_wrs) {
      int wp[2] = {-1, -1};
      if (make_wake_pipe(wp) == 0) {
        wake_rds[i] = wp[0];
        wake_wrs[i] = wp[1];
      } else {
        LOGE(LOGC_CORE, "Failed to make wake pipe for worker %d", i);
        startup_failed = 1;
        g_running = 0;
        wake_workers_via_pipes(wake_wrs, i);
        free(workers[i]);
        workers[i] = NULL;
        threads = i;
        break;
      }
      workers[i]->cfg.wake_rd = wake_rds[i];
    } else {
      workers[i]->cfg.wake_rd = -1;
    }

    int rc = pthread_create(&tids[i], &attr, worker_main, workers[i]);
    if (rc != 0) {
      LOGE(LOGC_CORE, "pthread_create[%d]: %s", i, strerror(rc));
      startup_failed = 1;
      g_running = 0;
      if (use_shared_wake) {
        wake_workers_via_pipe(shared_wake_pipe[1], i);
      } else {
        wake_workers_via_pipes(wake_wrs, i);
        if (wake_rds && wake_rds[i] >= 0) {
          close(wake_rds[i]);
          wake_rds[i] = -1;
        }
        if (wake_wrs && wake_wrs[i] >= 0) {
          close(wake_wrs[i]);
          wake_wrs[i] = -1;
        }
      }
      threads = i;
      break;
    }
  }
  pthread_attr_destroy(&attr);

  LOGI(LOGC_CORE, "Started %d worker threads", threads);

  int drain_announced = 0;
  uint64_t drain_started_ms = 0;
  while (g_running) {
    pause();
    if (g_tls_reload_requested && g_shutdown_state == SHUTDOWN_RUNNING) {
      g_tls_reload_requested = 0;

      if (access_log_runtime_reopen() != 0) {
        LOGW(LOGC_CORE, "access log reopen failed on SIGHUP");
      }

      char rerr[256] = {0};
      if (tls_reload_vhost_contexts(&config, rerr) == 0) {
        LOGI(LOGC_CORE, "TLS contexts reloaded on SIGHUP");
      } else {
        LOGE(LOGC_CORE, "TLS context reload failed%s%s", rerr[0] ? ": " : "", rerr[0] ? rerr : "");
      }
    }

    if (g_shutdown_state == SHUTDOWN_DRAIN) {
      if (!drain_announced) {
        drain_announced = 1;
        drain_started_ms = time_now_ms_monotonic();
        LOGI(LOGC_CORE,
             "shutdown drain start: active_conns=%" PRIu64 " grace_ms=%u",
             active_conns_total(),
             g_shutdown_grace_ms);
        if (use_shared_wake) {
          wake_workers_via_pipe(shared_wake_pipe[1], threads);
        } else {
          wake_workers_via_pipes(wake_wrs, threads);
        }
      }

      while (g_running && g_shutdown_state == SHUTDOWN_DRAIN) {
        uint64_t active = active_conns_total();
        if (active == 0) {
          LOGI(LOGC_CORE, "shutdown drain complete: active_conns=0");
          g_running = 0;
          break;
        }

        uint64_t now_ms = time_now_ms_monotonic();
        if (drain_started_ms && now_ms >= drain_started_ms
            && (now_ms - drain_started_ms) >= (uint64_t)g_shutdown_grace_ms) {
          g_shutdown_state = SHUTDOWN_FORCE;
          g_shutdown_signal_count = 2;
          LOGW(LOGC_CORE,
              "shutdown grace deadline reached: active_conns=%" PRIu64 " elapsed_ms=%" PRIu64 " grace_ms=%u",
               active,
              (now_ms - drain_started_ms),
               g_shutdown_grace_ms);
          g_running = 0;
          break;
        }

        struct timespec ts = {.tv_sec = 0, .tv_nsec = 50 * 1000 * 1000L};
        (void)nanosleep(&ts, NULL);
      }
    }
  }
  LOGI(LOGC_CORE,
       "shutdown signal observed: state=%s signals=%d grace_ms=%u",
       shutdown_state_name((int)g_shutdown_state),
       (int)g_shutdown_signal_count,
       g_shutdown_grace_ms);

  if (g_startup_failed) {
    startup_failed = 1;
  }

  if (use_shared_wake) {
    wake_workers_via_pipe(shared_wake_pipe[1], threads);
  } else {
    wake_workers_via_pipes(wake_wrs, threads);
  }

  for (int i = 0; i < threads; ++i) {
    if (tids[i]) {
      pthread_join(tids[i], NULL);
    }
  }

  if (use_shared_wake) {
    if (shared_wake_pipe[0] >= 0) {
      close(shared_wake_pipe[0]);
    }
    if (shared_wake_pipe[1] >= 0) {
      close(shared_wake_pipe[1]);
    }
  } else {
    if (wake_rds) {
      for (int i = 0; i < threads; ++i) {
        if (wake_rds[i] >= 0) {
          close(wake_rds[i]);
        }
      }
    }
    if (wake_wrs) {
      for (int i = 0; i < threads; ++i) {
        if (wake_wrs[i] >= 0) {
          close(wake_wrs[i]);
        }
      }
    }
  }

  for (int i = 0; i < config.vhost_count; ++i) {
    if (config.vhosts[i].docroot_fd >= 0) {
      close(config.vhosts[i].docroot_fd);
    }
  }

  for (int i = 0; i < config.vhost_count; ++i) {
    if (config.vhosts[i].tls_ctx_handle) {
      tls_ctx_free((struct tls_ctx *)config.vhosts[i].tls_ctx_handle);
      config.vhosts[i].tls_ctx_handle = NULL;
    }
  }

  if (tls_ready) {
    tls_global_cleanup();
  }

  free(tids);
  if (workers) {
    for (int i = 0; i < threads; ++i) {
      free(workers[i]);
    }
  }
  free(workers);
  free(wake_rds);
  free(wake_wrs);
  access_log_runtime_shutdown();
  return startup_failed ? 1 : 0;
}