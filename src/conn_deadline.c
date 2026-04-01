#include <stdint.h>

#include "include/config.h"
#include "include/conn_deadline.h"
#include "include/time_utils.h"
#include "include/timing_wheel.h"
#include "include/worker_ctx.h"

void conn_enter_keepalive_idle(struct conn *c, uint64_t now_ms) {
  if (!c) {
    return;
  }
  c->dl.last_active_ms = now_ms;
  c->dl.ka_idle = 1;
  c->dl.closing = 0;
  c->dl.header_start_ms = now_ms;
}

void conn_mark_activity(struct conn *c, uint64_t now_ms) {
  if (!c) {
    return;
  }
  c->dl.last_active_ms = now_ms;
  c->dl.ka_idle = 0;
}

void conn_start_draining(struct worker_ctx *w, struct conn *c, uint64_t now_ms) {
  if (!c) {
    return;
  }
  c->dl.draining = 1;
  unsigned drain_ms = DRAIN_TIMEOUT_MS;
  if (w && w->cfg.config && w->cfg.config->g.present & GF_DRAIN_TIMEOUT) {
    drain_ms = w->cfg.config->g.drain_timeout_ms;
  }
  c->dl.drain_deadline_ms = now_ms + drain_ms;
}

void conn_clear_draining(struct conn *c) {
  if (!c) {
    return;
  }
  c->dl.draining = 0;
  c->dl.drain_deadline_ms = 0;
}

void conn_mark_closing(struct conn *c) {
  if (!c) {
    return;
  }
  c->dl.closing = 1;
}

void conn_prepare_close_after_send(struct worker_ctx *w, struct conn *c) {
  if (!w || !c) {
    return;
  }
  conn_mark_closing(c);
  tw_cancel(w, c);
}

void conn_arm_write_timeout(struct worker_ctx *w, struct conn *c) {
  if (!w || !c) {
    return;
  }
  uint64_t now = w->now_cached_ms ? w->now_cached_ms : time_now_ms_monotonic();
  c->dl.write_start_ms = now;
  tw_reschedule(w, c, now);
}

void conn_clear_write_timeout(struct worker_ctx *w, struct conn *c) {
  if (!w || !c) {
    return;
  }
  if (!c->dl.write_start_ms) {
    return;
  }
  c->dl.write_start_ms = 0;
  uint64_t now = w->now_cached_ms ? w->now_cached_ms : time_now_ms_monotonic();
  tw_reschedule(w, c, now);
}

void conn_prepare_431_draining(struct worker_ctx *w,
                               struct conn *c,
                               uint64_t start_ms,
                               uint64_t resched_ms) {
  if (!w || !c) {
    return;
  }
  conn_start_draining(w, c, start_ms);
  tw_reschedule(w, c, resched_ms);
}
