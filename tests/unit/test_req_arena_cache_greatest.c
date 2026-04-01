#include "../vendor/greatest_color.h"
#include "../vendor/greatest.h"

// Override cache cap for deterministic testing.
#define REQ_ARENA_STATS 1
#define REQ_ARENA_CHUNK_CACHE 1
#define REQ_ARENA_CHUNK_CACHE_CAP 2u

#include "req_arena.h"

static void reset_cache_and_stats(void) {
#if REQ_ARENA_CHUNK_CACHE
  req_arena_cache_destroy_tls();
#endif
#if REQ_ARENA_STATS
  g_req_arena_new_chunks = 0;
  g_req_arena_new_chunk_bytes = 0;
  g_req_arena_reset_calls = 0;
  g_req_arena_reset_freed_chunks = 0;
  g_req_arena_reset_freed_bytes = 0;
  g_req_arena_destroy_calls = 0;
  g_req_arena_destroy_freed_chunks = 0;
  g_req_arena_destroy_freed_bytes = 0;

#if REQ_ARENA_CHUNK_CACHE
  g_req_arena_cache_hits = 0;
  g_req_arena_cache_misses = 0;
  g_req_arena_cache_put_chunks = 0;
  g_req_arena_cache_put_bytes = 0;
  g_req_arena_cache_drop_chunks = 0;
  g_req_arena_cache_drop_bytes = 0;
  g_req_arena_cache_count_peak = 0;
  g_req_arena_cache_shutdown_freed_chunks = 0;
  g_req_arena_cache_shutdown_freed_bytes = 0;
#endif
#endif
}

TEST t_cache_caps_and_reuses_small_chunks(void) {
  reset_cache_and_stats();

  // Allocate several arenas concurrently so we create several 1KB chunks.
  enum {
    N = 5
  };
  struct req_arena arenas[N];
  for (int i = 0; i < N; ++i) {
    req_arena_init(&arenas[i]);
    void *p = req_arena_alloc(&arenas[i], 8, 8);
    ASSERT(p != NULL);
    ASSERT(arenas[i].head != NULL);
  }

  // Now destroy them all; only 2 chunks should be cached; the rest dropped.
  for (int i = 0; i < N; ++i) {
    req_arena_destroy(&arenas[i]);
  }

#if REQ_ARENA_CHUNK_CACHE
  ASSERT_EQ(g_req_arena_chunk_cache_count, (uint32_t)REQ_ARENA_CHUNK_CACHE_CAP);
#endif

#if REQ_ARENA_STATS && REQ_ARENA_CHUNK_CACHE
  ASSERT_EQ(g_req_arena_cache_put_chunks, 2u);
  ASSERT_EQ(g_req_arena_cache_drop_chunks, 3u);
#endif

  // Next allocation should reuse a cached chunk (a hit) and not increase new_chunks.
#if REQ_ARENA_STATS
  uint64_t before_new_chunks = g_req_arena_new_chunks;
#endif

  struct req_arena a;
  req_arena_init(&a);
  void *q = req_arena_alloc(&a, 16, 8);
  ASSERT(q != NULL);

#if REQ_ARENA_CHUNK_CACHE
  ASSERT_EQ(g_req_arena_chunk_cache_count, 1u);
#endif
#if REQ_ARENA_STATS && REQ_ARENA_CHUNK_CACHE
  ASSERT_EQ(g_req_arena_cache_hits, 1u);
  ASSERT_EQ(g_req_arena_new_chunks, before_new_chunks);
#endif

  req_arena_destroy(&a);

#if REQ_ARENA_CHUNK_CACHE
  ASSERT_EQ(g_req_arena_chunk_cache_count, 2u);
#endif

  // Cleanup TLS cache for test runner.
#if REQ_ARENA_CHUNK_CACHE
  req_arena_cache_destroy_tls();
#endif

  PASS();
}

SUITE(s_req_arena_cache) {
  RUN_TEST(t_cache_caps_and_reuses_small_chunks);
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv) {
  GREATEST_MAIN_BEGIN();
  RUN_SUITE(s_req_arena_cache);
#if REQ_ARENA_CHUNK_CACHE
  req_arena_cache_destroy_tls();
#endif
  GREATEST_MAIN_END();
}
