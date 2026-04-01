#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "include/macros.h"
#include "include/conn.h"
#include "include/http_boundary.h"
#include "include/rx_stash.h"

struct rx_stash_block {
  struct rx_stash_block *next;
  char data[RBUF_SZ];
};

static THREAD_LOCAL struct rx_stash_block *rx_stash_free_list = NULL;
static THREAD_LOCAL uint64_t rx_stash_blocks_total = 0;
static THREAD_LOCAL uint64_t rx_stash_blocks_inuse = 0;

void rx_stash_pool_init(unsigned prealloc) {
  for (unsigned i = 0; i < prealloc; ++i) {
    struct rx_stash_block *b = (struct rx_stash_block *)malloc(sizeof(*b));
    if (!b) {
      break;
    }
    b->next = rx_stash_free_list;
    rx_stash_free_list = b;
    rx_stash_blocks_total++;
  }
}

void rx_stash_pool_destroy(void) {
  struct rx_stash_block *b = rx_stash_free_list;
  while (b) {
    struct rx_stash_block *next = b->next;
    free(b);
    b = next;
  }
  rx_stash_free_list = NULL;
  rx_stash_blocks_total = 0;
  rx_stash_blocks_inuse = 0;
}

void rx_stash_pool_get_stats(uint64_t *total, uint64_t *inuse) {
  if (total) {
    *total = rx_stash_blocks_total;
  }
  if (inuse) {
    *inuse = rx_stash_blocks_inuse;
  }
}

char *rx_stash_alloc(void) {
  struct rx_stash_block *b = rx_stash_free_list;
  if (b) {
    rx_stash_free_list = b->next;
    b->next = NULL;
    rx_stash_blocks_inuse++;
    return b->data;
  }

  b = (struct rx_stash_block *)malloc(sizeof(*b));
  if (!b) {
    return NULL;
  }
  b->next = NULL;
  rx_stash_blocks_total++;
  rx_stash_blocks_inuse++;
  return b->data;
}

void rx_stash_free(char *p) {
  if (!p) {
    return;
  }
  struct rx_stash_block *b =
    (struct rx_stash_block *)((char *)p - offsetof(struct rx_stash_block, data));
  b->next = rx_stash_free_list;
  rx_stash_free_list = b;
  if (rx_stash_blocks_inuse) {
    rx_stash_blocks_inuse--;
  }
}

void conn_rx_stash_consume(struct conn *c, size_t n) {
  if (!c || n == 0 || c->rx_stash_len == 0) {
    return;
  }
  if (n >= c->rx_stash_len) {
    c->rx_stash_len = 0;
    if (c->rx_stash) {
      rx_stash_free(c->rx_stash);
      c->rx_stash = NULL;
    }
    return;
  }
  if (!c->rx_stash) {
    return;
  }
  memmove(c->rx_stash, c->rx_stash + n, (size_t)c->rx_stash_len - n);
  c->rx_stash_len = (uint16_t)((size_t)c->rx_stash_len - n);
}

int conn_rx_stash_append(struct conn *c, const char *data, size_t n) {
  if (!c || !data || n == 0) {
    return 0;
  }
  if ((size_t)c->rx_stash_len + n > (size_t)RBUF_SZ) {
    return -1;
  }
  if (!c->rx_stash) {
    c->rx_stash = rx_stash_alloc();
    if (!c->rx_stash) {
      return -1;
    }
  }
  memcpy(c->rx_stash + c->rx_stash_len, data, n);
  c->rx_stash_len = (uint16_t)((size_t)c->rx_stash_len + n);
  return 0;
}

void conn_rx_tail_update_after_feed(struct conn *c, const char *fed, size_t fed_len) {
  if (!c) {
    return;
  }

  size_t prev_len = (size_t)c->rx_tail_len;
  char prev[3] = {0, 0, 0};
  if (prev_len) {
    memcpy(prev, c->rx_tail, prev_len);
  }

  size_t total = prev_len + fed_len;
  size_t take = (total < 3) ? total : 3;
  for (size_t i = 0; i < take; ++i) {
    size_t idx = total - take + i;
    char ch = (idx < prev_len) ? prev[idx] : fed[idx - prev_len];
    c->rx_tail[i] = ch;
  }
  c->rx_tail_len = (uint8_t)take;
}

void conn_split_on_header_end(struct conn *c,
                              const char *buf,
                              size_t n,
                              size_t *feed_len,
                              size_t *tail_len) {
  if (feed_len) {
    *feed_len = n;
  }
  if (tail_len) {
    *tail_len = 0;
  }
  if (!feed_len || !tail_len || !c || !buf || n == 0) {
    return;
  }

#if RBUF_SZ <= 512
  return;
#endif

  if (c->h1.headers_done || c->h1.parse_error || c->h1.header_too_big) {
    return;
  }

  ssize_t end = http_find_header_terminator_end(c->rx_tail, (size_t)c->rx_tail_len, buf, n);
  if (end < 0) {
    return;
  }

  size_t prefix_len = (size_t)c->rx_tail_len;
  size_t f = ((size_t)end > prefix_len) ? ((size_t)end - prefix_len) : 0;
  if (f > n) {
    f = n;
  }
  *feed_len = f;
  *tail_len = n - f;
}
