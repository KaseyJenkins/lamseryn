#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdalign.h>

#include "macros.h" // THREAD_LOCAL
#include "logger.h" // LOG_COMPILE_LEVEL / LOG_DEBUG for default stats gating

#ifndef REQ_ARENA_STATS
// Default stats gating follows compile-time logging ceiling:
// - DEBUG/TRACE builds: stats on
// - INFO/WARN/ERROR builds: stats off
// Can be overridden explicitly with -DREQ_ARENA_STATS=0/1.
#if LOG_COMPILE_LEVEL >= LOG_DEBUG
#define REQ_ARENA_STATS 1
#else
#define REQ_ARENA_STATS 0
#endif
#endif

#if REQ_ARENA_STATS
extern THREAD_LOCAL uint64_t g_req_arena_new_chunks;
extern THREAD_LOCAL uint64_t g_req_arena_new_chunk_bytes;
extern THREAD_LOCAL uint64_t g_req_arena_reset_calls;
extern THREAD_LOCAL uint64_t g_req_arena_reset_freed_chunks;
extern THREAD_LOCAL uint64_t g_req_arena_reset_freed_bytes;
extern THREAD_LOCAL uint64_t g_req_arena_destroy_calls;
extern THREAD_LOCAL uint64_t g_req_arena_destroy_freed_chunks;
extern THREAD_LOCAL uint64_t g_req_arena_destroy_freed_bytes;
#endif

#ifndef REQ_ARENA_CHUNK_MIN
#define REQ_ARENA_CHUNK_MIN 1024u
#endif

#ifndef REQ_ARENA_CHUNK_CACHE
// Cache small (REQ_ARENA_CHUNK_MIN) chunks per thread to reduce malloc/free churn.
#define REQ_ARENA_CHUNK_CACHE 1
#endif

#ifndef REQ_ARENA_CHUNK_CACHE_CAP
// Max number of cached 1KB chunks retained per thread.
// 1024 => ~1MB per thread worst-case.
#define REQ_ARENA_CHUNK_CACHE_CAP 1024u
#endif

#ifndef REQ_ARENA_RETAIN_MAX
// Keep small first chunks between requests to avoid churn; free if larger.
#define REQ_ARENA_RETAIN_MAX 4096u
#endif

struct req_arena_chunk {
  struct req_arena_chunk *next;
  size_t cap;
  size_t off;
  alignas(max_align_t) unsigned char data[];
};

#if REQ_ARENA_CHUNK_CACHE
extern THREAD_LOCAL struct req_arena_chunk *g_req_arena_chunk_cache;
extern THREAD_LOCAL uint32_t g_req_arena_chunk_cache_count;
#if REQ_ARENA_STATS
extern THREAD_LOCAL uint64_t g_req_arena_cache_hits;
extern THREAD_LOCAL uint64_t g_req_arena_cache_misses;
extern THREAD_LOCAL uint64_t g_req_arena_cache_put_chunks;
extern THREAD_LOCAL uint64_t g_req_arena_cache_put_bytes;
extern THREAD_LOCAL uint64_t g_req_arena_cache_drop_chunks;
extern THREAD_LOCAL uint64_t g_req_arena_cache_drop_bytes;
extern THREAD_LOCAL uint64_t g_req_arena_cache_count_peak;
extern THREAD_LOCAL uint64_t g_req_arena_cache_shutdown_freed_chunks;
extern THREAD_LOCAL uint64_t g_req_arena_cache_shutdown_freed_bytes;
#endif
#endif

#if REQ_ARENA_CHUNK_CACHE
static inline void req_arena_release_chunk(struct req_arena_chunk *c, int from_reset);
#endif

struct req_arena {
  struct req_arena_chunk *head;
  struct req_arena_chunk *tail;
};

static inline void req_arena_init(struct req_arena *a) {
  if (!a) {
    return;
  }
  a->head = NULL;
  a->tail = NULL;
}

static inline void req_arena_destroy(struct req_arena *a) {
  if (!a) {
    return;
  }
#if REQ_ARENA_STATS
  g_req_arena_destroy_calls++;
#endif
  struct req_arena_chunk *c = a->head;
  while (c) {
    struct req_arena_chunk *n = c->next;
#if REQ_ARENA_CHUNK_CACHE
    req_arena_release_chunk(c, /*from_reset=*/0);
#else
#if REQ_ARENA_STATS
    g_req_arena_destroy_freed_chunks++;
    g_req_arena_destroy_freed_bytes += (uint64_t)c->cap;
#endif
    free(c);
#endif
    c = n;
  }
  a->head = NULL;
  a->tail = NULL;
}

#if REQ_ARENA_CHUNK_CACHE
static inline void req_arena_cache_destroy_tls(void) {
  struct req_arena_chunk *c = g_req_arena_chunk_cache;
  while (c) {
    struct req_arena_chunk *n = c->next;
#if REQ_ARENA_STATS
    g_req_arena_cache_shutdown_freed_chunks++;
    g_req_arena_cache_shutdown_freed_bytes += (uint64_t)c->cap;
#endif
    free(c);
    c = n;
  }
  g_req_arena_chunk_cache = NULL;
  g_req_arena_chunk_cache_count = 0;
}

static inline void req_arena_release_chunk(struct req_arena_chunk *c, int from_reset) {
  if (!c) {
    return;
  }

  if (c->cap == (size_t)REQ_ARENA_CHUNK_MIN
      && g_req_arena_chunk_cache_count < (uint32_t)REQ_ARENA_CHUNK_CACHE_CAP) {
    c->off = 0;
    c->next = g_req_arena_chunk_cache;
    g_req_arena_chunk_cache = c;
    g_req_arena_chunk_cache_count++;
#if REQ_ARENA_STATS
    g_req_arena_cache_put_chunks++;
    g_req_arena_cache_put_bytes += (uint64_t)c->cap;
    if ((uint64_t)g_req_arena_chunk_cache_count > g_req_arena_cache_count_peak) {
      g_req_arena_cache_count_peak = (uint64_t)g_req_arena_chunk_cache_count;
    }
#endif
    return;
  }

  // Cache full or not a cacheable chunk; free to allocator.
#if REQ_ARENA_STATS
  if (c->cap == (size_t)REQ_ARENA_CHUNK_MIN) {
    g_req_arena_cache_drop_chunks++;
    g_req_arena_cache_drop_bytes += (uint64_t)c->cap;
  }
  if (from_reset) {
    g_req_arena_reset_freed_chunks++;
    g_req_arena_reset_freed_bytes += (uint64_t)c->cap;
  } else {
    g_req_arena_destroy_freed_chunks++;
    g_req_arena_destroy_freed_bytes += (uint64_t)c->cap;
  }
#else
  (void)from_reset;
#endif
  free(c);
}
#endif

static inline struct req_arena_chunk *req_arena_new_chunk(size_t need) {
  size_t cap = (need < (size_t)REQ_ARENA_CHUNK_MIN) ? (size_t)REQ_ARENA_CHUNK_MIN : need;

#if REQ_ARENA_CHUNK_CACHE
  if (cap == (size_t)REQ_ARENA_CHUNK_MIN && g_req_arena_chunk_cache) {
    struct req_arena_chunk *c = g_req_arena_chunk_cache;
    g_req_arena_chunk_cache = c->next;
    if (g_req_arena_chunk_cache_count) {
      g_req_arena_chunk_cache_count--;
    }
    c->next = NULL;
    c->cap = cap;
    c->off = 0;
#if REQ_ARENA_STATS
    g_req_arena_cache_hits++;
#endif
    return c;
  }
#if REQ_ARENA_STATS
  g_req_arena_cache_misses++;
#endif
#endif

  struct req_arena_chunk *c = (struct req_arena_chunk *)malloc(sizeof(*c) + cap);
  if (!c) {
    return NULL;
  }
  c->next = NULL;
  c->cap = cap;
  c->off = 0;
#if REQ_ARENA_STATS
  g_req_arena_new_chunks++;
  g_req_arena_new_chunk_bytes += (uint64_t)cap;
#endif
  return c;
}

// align must be a power-of-two.
static inline void *req_arena_alloc(struct req_arena *a, size_t n, size_t align) {
  if (!a) {
    return NULL;
  }
  if (align == 0) {
    align = 1;
  }

  struct req_arena_chunk *t = a->tail;
  if (t) {
    size_t aligned_off = (t->off + (align - 1)) & ~(align - 1);
    size_t end = aligned_off + n;
    if (end >= aligned_off && end <= t->cap) {
      void *p = t->data + aligned_off;
      t->off = end;
      return p;
    }
  }

  // Need a new chunk.
  size_t need = n + align;
  struct req_arena_chunk *nc = req_arena_new_chunk(need);
  if (!nc) {
    return NULL;
  }

  if (!a->head) {
    a->head = nc;
  }
  if (a->tail) {
    a->tail->next = nc;
  }
  a->tail = nc;

  size_t aligned_off = (nc->off + (align - 1)) & ~(align - 1);
  size_t end = aligned_off + n;
  if (end < aligned_off || end > nc->cap) {
    return NULL;
  }

  void *p = nc->data + aligned_off;
  nc->off = end;
  return p;
}

static inline char *req_arena_strndup(struct req_arena *a, const char *s, size_t n) {
  if (!s) {
    return NULL;
  }
  char *p = (char *)req_arena_alloc(a, n + 1, 1);
  if (!p) {
    return NULL;
  }
  memcpy(p, s, n);
  p[n] = 0;
  return p;
}

// Reset between requests: free all but optionally keep a small first chunk.
static inline void req_arena_reset(struct req_arena *a) {
  if (!a || !a->head) {
    return;
  }

#if REQ_ARENA_STATS
  g_req_arena_reset_calls++;
#endif

  struct req_arena_chunk *first = a->head;
  struct req_arena_chunk *c = first->next;
  while (c) {
    struct req_arena_chunk *n = c->next;
#if REQ_ARENA_CHUNK_CACHE
    req_arena_release_chunk(c, /*from_reset=*/1);
#else
#if REQ_ARENA_STATS
    g_req_arena_reset_freed_chunks++;
    g_req_arena_reset_freed_bytes += (uint64_t)c->cap;
#endif
    free(c);
#endif
    c = n;
  }

  a->head = NULL;
  a->tail = NULL;

  if (first->cap > (size_t)REQ_ARENA_RETAIN_MAX) {
#if REQ_ARENA_CHUNK_CACHE
    req_arena_release_chunk(first, /*from_reset=*/1);
#else
#if REQ_ARENA_STATS
    g_req_arena_reset_freed_chunks++;
    g_req_arena_reset_freed_bytes += (uint64_t)first->cap;
#endif
    free(first);
#endif
    return;
  }

  first->off = 0;
  first->next = NULL;
  a->head = first;
  a->tail = first;
}
