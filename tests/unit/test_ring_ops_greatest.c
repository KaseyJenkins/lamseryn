#include "../vendor/greatest_color.h"
#include "../vendor/greatest.h"

#include <string.h>
#include <liburing.h>

#include "include/worker_ctx.h"
#include "include/ring_ops.h"
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

TEST t_mark_post_sets_flags(void) {
  struct worker_ctx w;
  memset(&w, 0, sizeof(w));
  mark_post(&w);
  mark_post(&w);
  ASSERT_EQ(w.need_submit, 1);
  ASSERT_EQ(w.sqes_pending, 2);
  PASS();
}

TEST t_maybe_flush_urgent_resets_flags(void) {
  struct worker_ctx w;
  memset(&w, 0, sizeof(w));
  ASSERT_EQ(open_ring_or_skip(&w.ring, 8), 0);

  w.need_submit = 1;
  w.sqes_pending = 3;

  maybe_flush(&w, /*urgent=*/1);
  ASSERT_EQ(w.need_submit, 0);
  ASSERT_EQ(w.sqes_pending, 0);

  close_ring(&w.ring);
  PASS();
}

TEST t_maybe_flush_batch_threshold_resets_flags(void) {
  struct worker_ctx w;
  memset(&w, 0, sizeof(w));
  ASSERT_EQ(open_ring_or_skip(&w.ring, 16), 0);

  w.need_submit = 1;
  w.sqes_pending = SUBMIT_BATCH_SZ;

  maybe_flush(&w, /*urgent=*/0);
  ASSERT_EQ(w.need_submit, 0);
  ASSERT_EQ(w.sqes_pending, 0);

  close_ring(&w.ring);
  PASS();
}

TEST t_get_sqe_retry_once_no_submit_when_available(void) {
  struct io_uring ring;
  if (open_ring_or_skip(&ring, 4) < 0) {
    PASS();
  }

  int did = -1;
  struct io_uring_sqe *sqe = get_sqe_retry_once(&ring, &did);
  ASSERT(sqe != NULL);
  ASSERT_EQ(did, 0);

  close_ring(&ring);
  PASS();
}

TEST t_get_sqe_retry_once_does_submit_when_full(void) {
  struct io_uring ring;
  if (open_ring_or_skip(&ring, 4) < 0) {
    PASS();
  }

  // Fill all SQEs so the next get returns NULL.
  for (;;) {
    struct io_uring_sqe *sqe = io_uring_get_sqe(&ring);
    if (!sqe) {
      break;
    }
    io_uring_prep_nop(sqe);
  }

  int did = -1;
  struct io_uring_sqe *sqe2 = get_sqe_retry_once(&ring, &did);
  (void)sqe2;
  // We only assert that a submit was attempted.
  ASSERT_EQ(did, 1);

  close_ring(&ring);
  PASS();
}

SUITE(s_ring_ops) {
  RUN_TEST(t_mark_post_sets_flags);
  RUN_TEST(t_maybe_flush_urgent_resets_flags);
  RUN_TEST(t_maybe_flush_batch_threshold_resets_flags);
  RUN_TEST(t_get_sqe_retry_once_no_submit_when_available);
  RUN_TEST(t_get_sqe_retry_once_does_submit_when_full);
}

GREATEST_MAIN_DEFS();
int main(int argc, char **argv) {
  GREATEST_MAIN_BEGIN();
  RUN_SUITE(s_ring_ops);
  GREATEST_MAIN_END();
}
