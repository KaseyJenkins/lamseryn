#pragma once

#include <stddef.h>
#include <stdint.h>

struct conn;

#ifndef RX_STASH_PREALLOC
#define RX_STASH_PREALLOC 64
#endif

// Initialize/destroy the per-thread stash buffer pool.
void rx_stash_pool_init(unsigned prealloc);
void rx_stash_pool_destroy(void);

// Return pool total and in-use counts when pointers are non-NULL.
void rx_stash_pool_get_stats(uint64_t *total, uint64_t *inuse);

// Allocate/free a stash buffer from the per-thread pool.
char *rx_stash_alloc(void);
void rx_stash_free(char *p);

// Per-connection stash/tail utilities for pipelined request boundaries.
int conn_rx_stash_append(struct conn *c, const char *data, size_t n);
void conn_rx_stash_consume(struct conn *c, size_t n);
void conn_rx_tail_update_after_feed(struct conn *c, const char *fed, size_t fed_len);
void conn_split_on_header_end(struct conn *c,
                              const char *buf,
                              size_t n,
                              size_t *feed_len,
                              size_t *tail_len);
