#pragma once

#include <stdint.h>
#include <stddef.h>
#include "include/conn.h"
#include "include/conn_store.h"

struct worker_ctx;
struct vhost_t;

// --- Active connection count (process-global, atomic) ---

uint64_t active_conns_inc(void);
uint64_t active_conns_dec(void);
uint64_t active_conns_total(void);

// --- Connection freelist (per-thread) ---

struct conn *conn_alloc(void);
void conn_recycle(struct conn *c);
void conn_ref(struct conn *c);
void conn_put(struct conn *c);

// --- Connection lifecycle ---

int conn_init(struct worker_ctx *w, int fd, const struct vhost_t *vhost);
void conn_reset_request(struct worker_ctx *w, struct conn *c);

// --- TX file state cleanup ---

void tx_close_file(struct conn *c);

// --- Active-set bookkeeping ---

void worker_active_add(struct worker_ctx *w, int fd);
void worker_active_remove(struct worker_ctx *w, int fd);

// --- Conn lookup (trivial wrapper) ---

static inline struct conn *conn_get(int fd) {
  if (fd < 0) {
    return NULL;
  }
  return conn_store_get(fd);
}

// --- Freelist stats logging (per-thread, call from each worker teardown) ---

void conn_freelist_log_stats(int thread_id);

// --- Freelist teardown (per-thread, call at worker exit) ---

void conn_freelist_drain(void);
