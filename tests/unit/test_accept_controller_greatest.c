// tests/unit/test_accept_controller_greatest.c
#include "../vendor/greatest_color.h"
#include "../vendor/greatest.h"

#include <errno.h>
#include <string.h>
#include <liburing.h>

#include "include/worker_ctx.h"
#include "include/accept_controller.h"
#include "include/types.h"
#include "include/macros.h"
#include "logger.h" // link ../src/logger.c

// Minimal stub to satisfy accept_controller’s dependency on worker code.
void worker_accept_success(struct worker_ctx *w, int cfd) {
  (void)w;
  (void)cfd;
}

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

TEST t_emfile_backoff_sets_flags_and_arms_timer_real_ring(void) {
  struct worker_ctx w;
  memset(&w, 0, sizeof(w));
  if (open_ring_or_skip(&w.ring, 8) < 0) {
    PASS(); // skipped
  }

  // Ensure cancel loop has something to do (avoid the num_listeners==0 corner)
  w.num_listeners = 1;
  w.listen_fds[0] = 3;

  struct io_uring_cqe cqe;
  memset(&cqe, 0, sizeof(cqe));
  cqe.res = -EMFILE;

  struct op_ctx op = {.magic = OP_MAGIC, .type = OP_ACCEPT, .c = NULL, .fd = -1};

  accept_handle_cqe(&w, &op, &cqe);

  ASSERT_EQ(w.accept_paused, 1);
  ASSERT_EQ(w.accept_backoff_armed, 1);
  // In this configuration we should also have marked cancel inflight.
  ASSERT_EQ(w.accept_cancel_inflight, 1);
  ASSERT_EQ(w.accept_backoff_static.magic, OP_MAGIC);
  ASSERT_EQ(w.accept_backoff_static.type, OP_ACCEPT_BACKOFF);

  close_ring(&w.ring);
  PASS();
}

TEST t_eagain_does_not_pause_or_arm_backoff(void) {
  struct worker_ctx w;
  memset(&w, 0, sizeof(w));
  if (open_ring_or_skip(&w.ring, 8) < 0) {
    PASS(); // skipped
  }

  struct io_uring_cqe cqe;
  memset(&cqe, 0, sizeof(cqe));
  cqe.res = -EAGAIN;

  struct op_ctx op = {.magic = OP_MAGIC, .type = OP_ACCEPT, .c = NULL, .fd = 3};

  accept_handle_cqe(&w, &op, &cqe);

  ASSERT_EQ(w.accept_paused, 0);
  ASSERT_EQ(w.accept_backoff_armed, 0);
  ASSERT_EQ(w.accept_cancel_inflight, 0);

  // cnt_accept_eagain is DEV-level; default test build is OPS-level.
  // Verify the OPS aggregate without referencing DEV-only fields.
  ASSERT_EQ((int)w.stats.cnt_accept_err, 1);

  close_ring(&w.ring);
  PASS();
}

TEST t_backoff_handler_resets_flags_and_rearms(void) {
  struct worker_ctx w;
  memset(&w, 0, sizeof(w));
  if (open_ring_or_skip(&w.ring, 8) < 0) {
    PASS(); // skipped
  }

  w.accept_paused = 1;
  w.accept_backoff_armed = 1;
  w.accept_cancel_inflight = 1;
  w.num_listeners = 2;
  w.listen_fds[0] = 3;
  w.listen_fds[1] = 4;

  // Initialize accept_static entries to keep user_data stable.
  w.accept_static[0] =
    (struct op_ctx){.magic = OP_MAGIC, .type = OP_ACCEPT, .c = NULL, .fd = w.listen_fds[0]};
  w.accept_static[1] =
    (struct op_ctx){.magic = OP_MAGIC, .type = OP_ACCEPT, .c = NULL, .fd = w.listen_fds[1]};

  accept_handle_backoff(&w);

  ASSERT_EQ(w.accept_paused, 0);
  ASSERT_EQ(w.accept_backoff_armed, 0);
  ASSERT_EQ(w.accept_cancel_inflight, 0);

  close_ring(&w.ring);
  PASS();
}

TEST t_accept_arm_startup_initializes_and_posts(void) {
  struct worker_ctx w;
  memset(&w, 0, sizeof(w));
  if (open_ring_or_skip(&w.ring, 16) < 0) {
    PASS(); // skipped
  }

  w.num_listeners = 1;
  w.listen_fds[0] = 3;

  int armed = accept_arm_startup(&w);
  ASSERT_EQ(armed, 1);
  ASSERT_EQ(w.accept_inited[0], 1);
  ASSERT_EQ(w.accept_static[0].magic, OP_MAGIC);
  ASSERT_EQ(w.accept_static[0].type, OP_ACCEPT);
  ASSERT_EQ(w.accept_static[0].fd, 3);

  close_ring(&w.ring);
  PASS();
}

TEST t_accept_is_fd_pressure_reflects_pause_or_cancel(void) {
  struct worker_ctx w;
  memset(&w, 0, sizeof(w));

  ASSERT_EQ(accept_is_fd_pressure(&w), 0);

  w.accept_paused = 1;
  ASSERT_EQ(accept_is_fd_pressure(&w), 1);

  w.accept_paused = 0;
  w.accept_cancel_inflight = 1;
  ASSERT_EQ(accept_is_fd_pressure(&w), 1);

  ASSERT_EQ(accept_is_fd_pressure(NULL), 0);
  PASS();
}

TEST t_accept_try_handle_cqe_dispatches_accept_backoff(void) {
  struct worker_ctx w;
  memset(&w, 0, sizeof(w));
  if (open_ring_or_skip(&w.ring, 8) < 0) {
    PASS(); // skipped
  }

  w.accept_paused = 1;
  w.accept_backoff_armed = 1;
  w.accept_cancel_inflight = 1;

  struct op_ctx op = {.magic = OP_MAGIC, .type = OP_ACCEPT_BACKOFF, .c = NULL, .fd = -1};
  struct io_uring_cqe cqe;
  memset(&cqe, 0, sizeof(cqe));

  ASSERT_EQ(accept_try_handle_cqe(&w, &op, &cqe), 1);
  ASSERT_EQ(w.accept_paused, 0);
  ASSERT_EQ(w.accept_backoff_armed, 0);
  ASSERT_EQ(w.accept_cancel_inflight, 0);

  op.type = OP_WAKE;
  ASSERT_EQ(accept_try_handle_cqe(&w, &op, &cqe), 0);
  ASSERT_EQ(accept_try_handle_cqe(NULL, &op, &cqe), 0);

  close_ring(&w.ring);
  PASS();
}

TEST t_accept_enter_drain_pauses_and_cancels(void) {
  struct worker_ctx w;
  memset(&w, 0, sizeof(w));
  if (open_ring_or_skip(&w.ring, 8) < 0) {
    PASS(); // skipped
  }

  w.num_listeners = 1;
  w.listen_fds[0] = 3;
  w.accept_backoff_armed = 1;

  accept_enter_drain(&w);

  ASSERT_EQ(w.accept_paused, 1);
  ASSERT_EQ(w.accept_backoff_armed, 0);
  ASSERT_EQ(w.accept_cancel_inflight, 1);

  close_ring(&w.ring);
  PASS();
}

SUITE(s_accept_controller) {
  RUN_TEST(t_emfile_backoff_sets_flags_and_arms_timer_real_ring);
  RUN_TEST(t_eagain_does_not_pause_or_arm_backoff);
  RUN_TEST(t_backoff_handler_resets_flags_and_rearms);
  RUN_TEST(t_accept_arm_startup_initializes_and_posts);
  RUN_TEST(t_accept_is_fd_pressure_reflects_pause_or_cancel);
  RUN_TEST(t_accept_try_handle_cqe_dispatches_accept_backoff);
  RUN_TEST(t_accept_enter_drain_pauses_and_cancels);
}

GREATEST_MAIN_DEFS();
int main(int argc, char **argv) {
  GREATEST_MAIN_BEGIN();
  RUN_SUITE(s_accept_controller);
  GREATEST_MAIN_END();
}
