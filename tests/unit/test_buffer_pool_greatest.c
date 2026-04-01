#include "../vendor/greatest_color.h"
#include "../vendor/greatest.h"

#include <stdlib.h>
#include <string.h>
#include <liburing.h>

#include "include/buffer_pool.h"
#include "include/macros.h"

static int open_ring_or_skip(struct io_uring *ring, unsigned entries) {
  int rc = io_uring_queue_init(entries, ring, 0);
  if (rc < 0) {
    GREATEST_SKIPm("io_uring_queue_init failed; kernel/lib not available");
    return -1;
  }
  return 0;
}

static void close_ring(struct io_uring *ring) {
  io_uring_queue_exit(ring);
}

TEST t_init_success_get_return_destroy(void) {
  struct io_uring ring;
  if (open_ring_or_skip(&ring, 64) < 0) {
    PASS();
  }

  ASSERT_EQ(buffer_pool_init(&ring, 8, 5), 0);
  ASSERT_EQ(buffer_pool_entries(), 8);
  ASSERT_EQ(buffer_pool_group_id(), 5);

  int valid = 0;
  char *b0 = buffer_pool_get(0, &valid);
  ASSERT(b0 != NULL);
  ASSERT_EQ(valid, 1);

  valid = 123;
  char *bx = buffer_pool_get(999, &valid);
  ASSERT_EQ(bx, NULL);
  ASSERT_EQ(valid, 0);

  ASSERT_EQ(buffer_pool_return(0), 1);
  ASSERT_EQ(buffer_pool_return(999), 0);

  buffer_pool_destroy(&ring);
  close_ring(&ring);
  PASS();
}

TEST t_init_fail_invalid_args(void) {
  struct io_uring ring;
  if (open_ring_or_skip(&ring, 64) < 0) {
    PASS();
  }

  ASSERT_EQ(buffer_pool_init(&ring, 0, 1), -1);
  buffer_pool_destroy(&ring);
  close_ring(&ring);
  PASS();
}

TEST t_reinit_is_ok_and_destroy_idempotent(void) {
  struct io_uring ring;
  if (open_ring_or_skip(&ring, 64) < 0) {
    PASS();
  }

  ASSERT_EQ(buffer_pool_init(&ring, 4, 2), 0);
  ASSERT_EQ(buffer_pool_init(&ring, 8, 3), 0);
  ASSERT_EQ(buffer_pool_entries(), 8);
  ASSERT_EQ(buffer_pool_group_id(), 3);

  buffer_pool_destroy(&ring);
  buffer_pool_destroy(&ring);
  close_ring(&ring);
  PASS();
}

TEST t_out_of_range_get_and_return(void) {
  struct io_uring ring;
  if (open_ring_or_skip(&ring, 64) < 0) {
    PASS();
  }

  ASSERT_EQ(buffer_pool_init(&ring, 4, 7), 0);

  int valid = 1;
  char *b = buffer_pool_get(4, &valid);
  ASSERT_EQ(b, NULL);
  ASSERT_EQ(valid, 0);

  ASSERT_EQ(buffer_pool_return(4), 0);

  buffer_pool_destroy(&ring);
  close_ring(&ring);
  PASS();
}

SUITE(s_buffer_pool) {
  RUN_TEST(t_init_success_get_return_destroy);
  RUN_TEST(t_init_fail_invalid_args);
  RUN_TEST(t_reinit_is_ok_and_destroy_idempotent);
  RUN_TEST(t_out_of_range_get_and_return);
}

GREATEST_MAIN_DEFS();
int main(int argc, char **argv) {
  GREATEST_MAIN_BEGIN();
  RUN_SUITE(s_buffer_pool);
  GREATEST_MAIN_END();
}
