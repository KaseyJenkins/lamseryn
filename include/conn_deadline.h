#pragma once

#include <stdint.h>

#include "include/conn.h"

struct worker_ctx;

static inline int conn_is_closing_no_deadline(const struct conn *c) {
  return c && c->dl.closing && !c->dl.draining;
}

static inline int conn_is_keepalive_idle(const struct conn *c) {
  return c && c->dl.ka_idle && c->h1.parser_bytes == 0;
}

static inline int conn_is_initial_idle(const struct conn *c) {
  return c && !c->dl.ka_idle && !c->h1.headers_done && c->h1.parser_bytes == 0;
}

static inline int conn_is_header_in_progress(const struct conn *c) {
  return c && !c->dl.ka_idle && !c->h1.headers_done && c->h1.parser_bytes > 0;
}

static inline int conn_tls_handshake_pending(const struct conn *c) {
  return c && c->tls_enabled && !c->tls_handshake_done;
}

void conn_enter_keepalive_idle(struct conn *c, uint64_t now_ms);
void conn_mark_activity(struct conn *c, uint64_t now_ms);
void conn_start_draining(struct worker_ctx *w, struct conn *c, uint64_t now_ms);
void conn_clear_draining(struct conn *c);
void conn_mark_closing(struct conn *c);

void conn_prepare_close_after_send(struct worker_ctx *w, struct conn *c);
void conn_arm_write_timeout(struct worker_ctx *w, struct conn *c);
void conn_clear_write_timeout(struct worker_ctx *w, struct conn *c);
void conn_prepare_431_draining(struct worker_ctx *w,
                               struct conn *c,
                               uint64_t start_ms,
                               uint64_t resched_ms);
