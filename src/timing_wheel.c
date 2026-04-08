#include <stdint.h>
#include <errno.h>
#include <string.h>
#include <liburing.h>

#include "instrumentation/instrumentation.h"
#include "instrumentation/counters_update.h"
#include "request_handlers.h"
#include "include/logger.h"
#include "include/macros.h"
#include "include/types.h"
#include "include/conn.h"
#include "include/worker_ctx.h"
#include "include/timing_wheel.h"
#include "include/ring_ops.h"
#include "include/conn_deadline.h"
#include "include/conn_close.h"

static inline int tw_conn_is_body_in_progress(const struct conn *c) {
  return c && !c->dl.ka_idle && c->h1.headers_done && !c->h1.message_done;
}

static inline int tw_conn_is_write_in_progress(const struct conn *c) {
  if (!c) {
    return 0;
  }
  // Only enforce write-timeout when the send path explicitly started it.
  // This avoids unintentionally arming write deadlines for "closing" paths
  // that happen to have tx buffers staged (e.g. 408 staged by the wheel).
  if (!c->dl.write_start_ms) {
    return 0;
  }
  if (c->tx.write_buf && c->tx.write_len > c->tx.write_off) {
    return 1;
  }
  if (c->tx.file_fd >= 0 && c->tx.file_rem > 0) {
    return 1;
  }
  return 0;
}

// Coarse connection state classification used for deadline decisions.
enum tw_conn_state {
  TW_CONN_STATE_NONE = 0,
  TW_CONN_STATE_KA_IDLE,
  TW_CONN_STATE_INITIAL_IDLE,
  TW_CONN_STATE_HEADER_IN_PROGRESS,
  TW_CONN_STATE_BODY_IN_PROGRESS,
  TW_CONN_STATE_WRITE_IN_PROGRESS,
  TW_CONN_STATE_DRAINING,
  TW_CONN_STATE_CLOSING_NO_DEADLINE,
};

static inline enum tw_conn_state tw_conn_get_state(const struct conn *c) {
  if (!c || c->fd < 0) {
    return TW_CONN_STATE_NONE;
  }
  if (c->dl.draining) {
    return TW_CONN_STATE_DRAINING;
  }
  if (conn_tls_handshake_pending(c)) {
    // TLS connections before handshake completion are semantically idle:
    // no HTTP data has been exchanged, so use the initial-idle timeout
    // and its direct-close path (no plaintext 408 on a TLS port).
    return TW_CONN_STATE_INITIAL_IDLE;
  }
  if (conn_is_closing_no_deadline(c)) {
    // Closing conns normally have no deadlines, except if we explicitly
    // armed a write-timeout for close-after-send behavior.
    if (tw_conn_is_write_in_progress(c)) {
      return TW_CONN_STATE_WRITE_IN_PROGRESS;
    }
    return TW_CONN_STATE_CLOSING_NO_DEADLINE;
  }
  if (conn_is_keepalive_idle(c)) {
    return TW_CONN_STATE_KA_IDLE;
  }
  if (conn_is_initial_idle(c)) {
    return TW_CONN_STATE_INITIAL_IDLE;
  }
  if (conn_is_header_in_progress(c)) {
    return TW_CONN_STATE_HEADER_IN_PROGRESS;
  }
  if (tw_conn_is_body_in_progress(c)) {
    return TW_CONN_STATE_BODY_IN_PROGRESS;
  }
  if (tw_conn_is_write_in_progress(c)) {
    return TW_CONN_STATE_WRITE_IN_PROGRESS;
  }
  return TW_CONN_STATE_NONE;
}

static inline void tw_conn_ref(struct conn *c) {
  if (c) {
    c->refcnt++;
  }
}

// Compute active deadline for a connection based on current state and config.
static inline uint64_t tw_compute_conn_deadline(struct worker_ctx *w,
                                                struct conn *c,
                                                uint64_t now_ms) {
  (void)now_ms;

  const struct config_t *cfg = (w && w->cfg.config) ? w->cfg.config : NULL;
  const struct globals_cfg *g = cfg ? &cfg->g : NULL;

  unsigned keepalive_idle_ms =
    (g && (g->present & GF_KA_IDLE_CLOSE)) ? g->keepalive_idle_close_ms : IDLE_CLOSE_MS;
  unsigned initial_idle_ms = (g && (g->present & GF_INITIAL_IDLE_TIMEOUT))
                               ? g->initial_idle_timeout_ms
                               : INITIAL_IDLE_TIMEOUT_MS;
  unsigned header_to_ms =
    (g && (g->present & GF_HEADER_TIMEOUT)) ? g->header_timeout_ms : HEADER_TIMEOUT_MS;
  unsigned body_to_ms =
    (g && (g->present & GF_BODY_TIMEOUT)) ? g->body_timeout_ms : BODY_TIMEOUT_MS;
  unsigned write_to_ms =
    (g && (g->present & GF_WRITE_TIMEOUT)) ? g->write_timeout_ms : WRITE_TIMEOUT_MS;

  enum tw_conn_state st = tw_conn_get_state(c);
  switch (st) {
  case TW_CONN_STATE_DRAINING:
    c->dl.deadline_kind = DK_DRAIN;
    return c->dl.drain_deadline_ms;

  case TW_CONN_STATE_KA_IDLE:
    c->dl.deadline_kind = DK_KA_IDLE;
    return c->dl.last_active_ms + keepalive_idle_ms;

  case TW_CONN_STATE_INITIAL_IDLE:
    c->dl.deadline_kind = DK_INITIAL_IDLE;
    return c->dl.last_active_ms + initial_idle_ms;

  case TW_CONN_STATE_HEADER_IN_PROGRESS:
    c->dl.deadline_kind = DK_HEADER_TIMEOUT;
    return c->dl.header_start_ms + header_to_ms;

  case TW_CONN_STATE_BODY_IN_PROGRESS:
    c->dl.deadline_kind = DK_BODY_TIMEOUT;
    return c->dl.last_active_ms + body_to_ms;

  case TW_CONN_STATE_WRITE_IN_PROGRESS:
    c->dl.deadline_kind = DK_WRITE_TIMEOUT;
    return c->dl.write_start_ms ? (c->dl.write_start_ms + write_to_ms)
                                : (c->dl.last_active_ms + write_to_ms);

  case TW_CONN_STATE_CLOSING_NO_DEADLINE:
  case TW_CONN_STATE_NONE:
  default:
    c->dl.deadline_kind = DK_NONE;
    return 0;
  }
}

static inline uint32_t tw_slot_for(struct worker_ctx *w, uint64_t now_ms, uint64_t deadline_ms) {
  uint64_t dt = (deadline_ms > now_ms) ? (deadline_ms - now_ms) : 0;
  uint64_t ticks_ahead = (w->tw_tick_ms ? (dt / w->tw_tick_ms) : 0);
  return (uint32_t)((w->tw_now_tick + ticks_ahead) & w->tw_mask);
}

static inline void tw_remove_internal(struct worker_ctx *w, struct conn *c) {
  if (!w || !c || !c->dl.deadline_active) {
    return;
  }
  uint32_t slot = c->link.tw_slot;
  if (c->link.tw_prev) {
    c->link.tw_prev->link.tw_next = c->link.tw_next;
  } else {
    w->tw_buckets[slot] = c->link.tw_next;
  }
  if (c->link.tw_next) {
    c->link.tw_next->link.tw_prev = c->link.tw_prev;
  }
  c->link.tw_prev = c->link.tw_next = NULL;
  c->dl.deadline_active = 0;
}

static inline void tw_insert_internal(struct worker_ctx *w,
                                      struct conn *c,
                                      uint64_t now_ms,
                                      uint64_t deadline_ms) {
  if (!w || !c || c->fd < 0) {
    return;
  }
  c->dl.deadline_ms = deadline_ms;
  uint32_t slot = tw_slot_for(w, now_ms, deadline_ms);
  c->link.tw_slot = slot;
  struct conn *head = w->tw_buckets[slot];
  c->link.tw_next = head;
  c->link.tw_prev = NULL;
  if (head) {
    head->link.tw_prev = c;
  }
  w->tw_buckets[slot] = c;
  c->dl.deadline_active = 1;
}

void tw_init(struct worker_ctx *w, uint32_t slots, uint32_t tick_ms, uint64_t now_ms) {
  if (!w) {
    return;
  }
  w->tw_slots = slots;
  w->tw_mask = (uint32_t)(slots - 1);
  w->tw_tick_ms = tick_ms;
  w->tw_now_tick = 0;
  for (uint32_t i = 0; i < w->tw_slots; ++i) {
    w->tw_buckets[i] = NULL;
  }
  w->next_tick_deadline_ms = now_ms + w->tw_tick_ms;
}

void tw_reschedule(struct worker_ctx *w, struct conn *c, uint64_t now_ms) {
  if (!w || !c) {
    return;
  }
  uint64_t d = tw_compute_conn_deadline(w, c, now_ms);
  if (c->dl.deadline_active) {
    tw_remove_internal(w, c);
  }
  if (d) {
    tw_insert_internal(w, c, now_ms, d);
  }
}

void tw_cancel(struct worker_ctx *w, struct conn *c) {
  if (!w || !c) {
    return;
  }
  if (c->dl.deadline_active) {
    tw_remove_internal(w, c);
  }
  c->dl.deadline_kind = DK_NONE;
  c->dl.deadline_ms = 0;
}

void tw_process_tick(struct worker_ctx *w, uint64_t now_ms) {
#if INSTRUMENTATION_LEVEL >= LVL_DEV
  uint64_t t0_us = log_now_ms() * 1000ull;

  uint64_t processed = 0;
  uint64_t expired = 0;
  uint64_t reinserted = 0;
#endif

  uint32_t slot = (uint32_t)(w->tw_now_tick & w->tw_mask);
  struct conn *cur = w->tw_buckets[slot];
  w->tw_buckets[slot] = NULL;
  w->tw_now_tick++;

  while (cur) {
    struct conn *next = cur->link.tw_next;

    cur->link.tw_next = cur->link.tw_prev = NULL;
    cur->dl.deadline_active = 0;

#if INSTRUMENTATION_LEVEL >= LVL_DEV
    processed++;
#endif

    if (now_ms >= cur->dl.deadline_ms) {
#if INSTRUMENTATION_LEVEL >= LVL_DEV
      expired++;
#endif
      int fd = cur->fd;

      int rc_close = -1;
      int alive_after_action = 1;

      switch (cur->dl.deadline_kind) {
      case DK_DRAIN:
      case DK_KA_IDLE:
      case DK_INITIAL_IDLE:
      case DK_WRITE_TIMEOUT:
        if (cur->dl.deadline_kind == DK_WRITE_TIMEOUT) {
          cur->dl.abortive_close = 1;
        }
        rc_close = schedule_or_sync_close(w, fd);
        alive_after_action = (rc_close == 0);
        break;

      case DK_HEADER_TIMEOUT:
      case DK_BODY_TIMEOUT: {
        CTR_INC_OPS(w, cnt_408);
        struct io_uring_sqe *sqe_w = get_sqe_batching(w);
        if (!sqe_w) {
          CTR_INC_DEV(w, cnt_sqe_starvation_close);
          rc_close = schedule_or_sync_close(w, fd);
          alive_after_action = (rc_close == 0);
        } else {
          cur->tx.resp_kind = RK_408;
          {
            struct response_view rv = request_select_response(RK_408, 0);
            cur->tx.write_buf = rv.buf;
            cur->tx.write_len = rv.len;
          }
          cur->tx.write_off = 0;
          cur->dl.closing = 1;

          tw_conn_ref(cur);
          io_uring_prep_send(sqe_w, fd, cur->tx.write_buf, cur->tx.write_len, MSG_NOSIGNAL);
          io_uring_sqe_set_data64(sqe_w, (uint64_t)(uintptr_t)&cur->op_write);
          mark_post(w);
        }
        break;
      }

      default:
        break;
      }

      if (alive_after_action) {
        uint64_t next_deadline = tw_compute_conn_deadline(w, cur, now_ms);
        if (next_deadline) {
          tw_insert_internal(w, cur, now_ms, next_deadline);
        }
      }

    } else {
#if INSTRUMENTATION_LEVEL >= LVL_DEV
      reinserted++;
#endif
      tw_insert_internal(w, cur, now_ms, cur->dl.deadline_ms);
    }

    cur = next;
  }

#if INSTRUMENTATION_LEVEL >= LVL_DEV
  CTR_INC_DEV(w, cnt_tw_ticks);
  CTR_ADD_DEV(w, cnt_tw_processed, processed);
  CTR_ADD_DEV(w, cnt_tw_expired, expired);
  CTR_ADD_DEV(w, cnt_tw_reinserted, reinserted);

  CTR_UPDATE_PEAK_DEV_VAL(w, processed, cnt_tw_bucket_max);

  uint64_t dt_us = (log_now_ms() * 1000ull) - t0_us;
  CTR_ADD_DEV(w, cnt_tw_tick_us_total, dt_us);
  CTR_UPDATE_PEAK_DEV_VAL(w, dt_us, cnt_tw_tick_us_max);
#endif
}
