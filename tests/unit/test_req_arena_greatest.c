#include "../vendor/greatest_color.h"
#include "../vendor/greatest.h"

#include <stdint.h>
#include <string.h>

#include "req_arena.h"

TEST t_init_alloc_and_alignment(void) {
  struct req_arena a;
  req_arena_init(&a);

  void *p1 = req_arena_alloc(&a, 1, 8);
  ASSERT(p1 != NULL);
  ASSERT_EQ((uintptr_t)p1 % 8u, 0u);

  void *p2 = req_arena_alloc(&a, 15, 16);
  ASSERT(p2 != NULL);
  ASSERT_EQ((uintptr_t)p2 % 16u, 0u);

  void *p3 = req_arena_alloc(&a, 3, 0);
  ASSERT(p3 != NULL);

  req_arena_destroy(&a);
  PASS();
}

TEST t_strndup_nul_terminates(void) {
  struct req_arena a;
  req_arena_init(&a);

  const char *s = "abcdef";
  char *p = req_arena_strndup(&a, s, 3);
  ASSERT(p != NULL);
  ASSERT_STR_EQ(p, "abc");

  req_arena_destroy(&a);
  PASS();
}

TEST t_reset_retains_small_first_chunk(void) {
  struct req_arena a;
  req_arena_init(&a);

  char *p = (char *)req_arena_alloc(&a, 32, 1);
  ASSERT(p != NULL);
  ASSERT(a.head != NULL);
  ASSERT(a.tail != NULL);
  struct req_arena_chunk *first = a.head;

  // Force a second chunk.
  void *big = req_arena_alloc(&a, (size_t)REQ_ARENA_CHUNK_MIN * 2u, 8);
  ASSERT(big != NULL);
  ASSERT(a.tail != first);

  req_arena_reset(&a);

  ASSERT(a.head == first);
  ASSERT(a.tail == first);
  ASSERT(first->next == NULL);
  ASSERT_EQ(first->off, 0u);

  // And it should still be usable after reset.
  void *p2 = req_arena_alloc(&a, 16, 8);
  ASSERT(p2 != NULL);

  req_arena_destroy(&a);
  PASS();
}

TEST t_reset_drops_large_first_chunk(void) {
  struct req_arena a;
  req_arena_init(&a);

  // First allocation large enough to create a first chunk > retain max.
  (void)req_arena_alloc(&a, (size_t)REQ_ARENA_RETAIN_MAX + 1024u, 8);
  ASSERT(a.head != NULL);

  req_arena_reset(&a);

  // For large first chunks, reset should free it and leave arena empty.
  ASSERT(a.head == NULL);
  ASSERT(a.tail == NULL);

  // Then it should allocate a new chunk fine.
  void *p2 = req_arena_alloc(&a, 8, 8);
  ASSERT(p2 != NULL);
  ASSERT(a.head != NULL);

  req_arena_destroy(&a);
  PASS();
}

TEST t_many_small_allocs_rollover_and_nonmoving(void) {
  struct req_arena a;
  req_arena_init(&a);

  enum {
    N = 2048
  };
  unsigned char *ptrs[N];

  for (int i = 0; i < N; ++i) {
    unsigned char *p = (unsigned char *)req_arena_alloc(&a, 1, 1);
    ASSERT(p != NULL);
    *p = (unsigned char)(i & 0xff);
    ptrs[i] = p;
    if (i > 0) {
      ASSERT(ptrs[i] != ptrs[i - 1]);
    }
  }

  // With a minimum chunk size of 1024, 2048 single-byte allocs should require
  // at least two chunks. (This also indirectly asserts allocations are monotonic
  // and do not overwrite earlier ones.)
  ASSERT(a.head != NULL);
  ASSERT(a.head->next != NULL);
  ASSERT(a.tail != NULL);
  ASSERT(a.tail != a.head);

  // Non-moving: earlier pointers remain valid and retain their contents.
  for (int i = 0; i < N; i += 127) {
    ASSERT_EQ(*ptrs[i], (unsigned char)(i & 0xff));
  }

  req_arena_destroy(&a);
  PASS();
}

SUITE(s_req_arena) {
  RUN_TEST(t_init_alloc_and_alignment);
  RUN_TEST(t_strndup_nul_terminates);
  RUN_TEST(t_reset_retains_small_first_chunk);
  RUN_TEST(t_reset_drops_large_first_chunk);
  RUN_TEST(t_many_small_allocs_rollover_and_nonmoving);
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv) {
  GREATEST_MAIN_BEGIN();
  RUN_SUITE(s_req_arena);

#if REQ_ARENA_CHUNK_CACHE
  req_arena_cache_destroy_tls();
#endif

  GREATEST_MAIN_END();
}
