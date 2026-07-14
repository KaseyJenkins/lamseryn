#define _POSIX_C_SOURCE 200809L

#include "../vendor/greatest_color.h"
#include "../vendor/greatest.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <unistd.h>
#include <string.h>
#include <time.h>

#include "include/conn.h"
#include "include/tx.h"

static struct tx_state_t tx_test_init(void) {
  struct tx_state_t tx;
  tx_init(&tx);
  return tx;
}

TEST t_after_full_response_431_draining_shutwr(void) {
  struct tx_state_t tx = tx_test_init();
  tx.resp_kind = RK_431;
  tx.keepalive = 0;

  enum tx_after_action a = tx_after_full_response(&tx, /*conn_draining=*/1);
  ASSERT_EQ(a, TX_AFTER_SHUT_WR_AND_READ);
  PASS();
}

TEST t_after_full_response_503_close(void) {
  struct tx_state_t tx = tx_test_init();
  tx.resp_kind = RK_503;
  tx.keepalive = 1;

  enum tx_after_action a = tx_after_full_response(&tx, /*conn_draining=*/0);
  ASSERT_EQ(a, TX_AFTER_CLOSE);
  PASS();
}

TEST t_after_full_response_keepalive_reset(void) {
  struct tx_state_t tx = tx_test_init();
  tx.resp_kind = RK_OK_KA;
  tx.keepalive = 1;

  enum tx_after_action a = tx_after_full_response(&tx, /*conn_draining=*/0);
  ASSERT_EQ(a, TX_AFTER_KEEPALIVE_RESET);
  PASS();
}

TEST t_after_full_response_default_close(void) {
  struct tx_state_t tx = tx_test_init();
  tx.resp_kind = RK_OK_CLOSE;
  tx.keepalive = 0;

  enum tx_after_action a = tx_after_full_response(&tx, /*conn_draining=*/0);
  ASSERT_EQ(a, TX_AFTER_CLOSE);
  PASS();
}

TEST t_pending_headers_returns_remaining_slice(void) {
  struct tx_state_t tx = tx_test_init();
  tx.write_buf = "abcdef";
  tx.write_len = 6;
  tx.write_off = 2;

  struct tx_next_io out = {0};
  ASSERT_EQ(tx_pending_headers(&tx, &out), 1);
  ASSERT_EQ((int)out.len, 4);
  ASSERT(memcmp(out.buf, "cdef", 4) == 0);
  PASS();
}

TEST t_pending_headers_returns_zero_when_fully_sent(void) {
  struct tx_state_t tx = tx_test_init();
  tx.write_buf = "abc";
  tx.write_len = 3;
  tx.write_off = 3;

  struct tx_next_io out = {0};
  ASSERT_EQ(tx_pending_headers(&tx, &out), 0);
  PASS();
}

TEST t_pollout_helpers_reflect_state(void) {
  struct tx_state_t tx = tx_test_init();

  ASSERT_EQ(tx_pollout_is_armed(&tx), 0);
  ASSERT_EQ(tx_should_arm_pollout(&tx), 1);

  tx_notify_poll_armed(&tx);
  ASSERT_EQ(tx_pollout_is_armed(&tx), 1);
  ASSERT_EQ(tx_should_arm_pollout(&tx), 0);

  tx_notify_poll_disarmed_staged(&tx);
  ASSERT_EQ(tx_pollout_is_armed(&tx), 0);
  ASSERT_EQ(tx_should_arm_pollout(&tx), 1);
  PASS();
}

TEST t_next_sendfile_chunk_caps_to_policy_max(void) {
  struct tx_state_t tx = tx_test_init();

  tx.file_rem = (size_t)(2u << 20);
  ASSERT_EQ((int)tx_next_sendfile_chunk(&tx), (int)(1u << 20));

  tx.file_rem = (size_t)(64u << 10);
  ASSERT_EQ((int)tx_next_sendfile_chunk(&tx), (int)(64u << 10));

  tx.file_rem = 0;
  ASSERT_EQ((int)tx_next_sendfile_chunk(&tx), 0);
  PASS();
}

TEST t_sendfile_step_mapping_matches_decisions(void) {
  ASSERT_EQ(tx_sendfile_step_from_decision(TX_RESUME_SENDFILE), TX_SF_STEP_RESUME);
  ASSERT_EQ(tx_sendfile_step_from_decision(TX_NOOP), TX_SF_STEP_RETRY);
  ASSERT_EQ(tx_sendfile_step_from_decision(TX_ARM_POLLOUT), TX_SF_STEP_ARM_POLLOUT);
  ASSERT_EQ(tx_sendfile_step_from_decision(TX_ERROR_CLOSE), TX_SF_STEP_ERROR_CLOSE);
  ASSERT_EQ(tx_sendfile_step_from_decision(TX_SHUT_WR_AND_READ), TX_SF_STEP_SHUT_WR_AND_READ);
  ASSERT_EQ(tx_sendfile_step_from_decision(TX_DONE_KEEPALIVE), TX_SF_STEP_AFTER_FULL_RESPONSE);
  PASS();
}

TEST t_attach_sendfile_replaces_existing_file(void) {
  int old_pipe[2];
  int new_pipe[2];
  ASSERT_EQ(pipe(old_pipe), 0);
  ASSERT_EQ(pipe(new_pipe), 0);

  struct tx_state_t tx = tx_test_init();
  tx.file_fd = old_pipe[0];

  ASSERT_EQ(tx_attach_sendfile(&tx, new_pipe[0], 12, 34), 0);
  ASSERT_EQ(tx.file_fd, new_pipe[0]);
  ASSERT_EQ((int)tx.file_off, 12);
  ASSERT_EQ((int)tx.file_rem, 34);
  ASSERT_EQ(close(old_pipe[0]), -1);

  close(old_pipe[1]);
  close(new_pipe[1]);
  tx_close_attached_file(&tx);
  PASS();
}

TEST t_close_attached_file_clears_file_state(void) {
  int pipefd[2];
  ASSERT_EQ(pipe(pipefd), 0);

  struct tx_state_t tx = tx_test_init();
  tx.file_fd = pipefd[0];
  tx.file_off = 7;
  tx.file_rem = 9;

  tx_close_attached_file(&tx);
  ASSERT_EQ(tx.file_fd, -1);
  ASSERT_EQ((int)tx.file_off, 0);
  ASSERT_EQ((int)tx.file_rem, 0);
  ASSERT_EQ(close(pipefd[0]), -1);

  close(pipefd[1]);
  PASS();
}

TEST t_reset_closes_attached_file(void) {
  int pipefd[2];
  ASSERT_EQ(pipe(pipefd), 0);

  struct tx_state_t tx = tx_test_init();
  tx.file_fd = pipefd[0];
  tx.file_off = 7;
  tx.file_rem = 9;

  tx_reset(&tx);
  ASSERT_EQ(tx.file_fd, -1);
  ASSERT_EQ((int)tx.file_off, 0);
  ASSERT_EQ((int)tx.file_rem, 0);
  ASSERT_EQ(close(pipefd[0]), -1);

  close(pipefd[1]);
  PASS();
}

TEST t_reset_initialized_empty_tx_does_not_close_stdin(void) {
  struct tx_state_t tx = tx_test_init();

  errno = 0;
  int before = fcntl(STDIN_FILENO, F_GETFD);
  if (before < 0 && errno == EBADF) {
    SKIPm("stdin already closed");
  }

  tx_reset(&tx);
  ASSERT_EQ(tx.file_fd, -1);
  errno = 0;
  int rc = fcntl(STDIN_FILENO, F_GETFD);
  ASSERT(rc >= 0 || errno != EBADF);
  PASS();
}

TEST t_reset_clears_recv_armed(void) {
  struct tx_state_t tx = tx_test_init();
  tx.recv_armed = 1;

  tx_reset(&tx);
  ASSERT_EQ(tx.recv_armed, 0);
  PASS();
}

#if ENABLE_ITEST_ECHO
TEST t_reset_clears_itest_static_mode(void) {
  struct tx_state_t tx = tx_test_init();
  tx.itest_static_mode = "sendfile";

  tx_reset(&tx);
  ASSERT_EQ(tx.itest_static_mode, NULL);
  PASS();
}
#endif

SUITE(s_tx) {
  RUN_TEST(t_after_full_response_431_draining_shutwr);
  RUN_TEST(t_after_full_response_503_close);
  RUN_TEST(t_after_full_response_keepalive_reset);
  RUN_TEST(t_after_full_response_default_close);
  RUN_TEST(t_pending_headers_returns_remaining_slice);
  RUN_TEST(t_pending_headers_returns_zero_when_fully_sent);
  RUN_TEST(t_pollout_helpers_reflect_state);
  RUN_TEST(t_next_sendfile_chunk_caps_to_policy_max);
  RUN_TEST(t_sendfile_step_mapping_matches_decisions);
  RUN_TEST(t_attach_sendfile_replaces_existing_file);
  RUN_TEST(t_close_attached_file_clears_file_state);
  RUN_TEST(t_reset_closes_attached_file);
  RUN_TEST(t_reset_initialized_empty_tx_does_not_close_stdin);
  RUN_TEST(t_reset_clears_recv_armed);
#if ENABLE_ITEST_ECHO
  RUN_TEST(t_reset_clears_itest_static_mode);
#endif
}

// ---------------------------------------------------------------------------
// tx_build_headers: emit_content_length parameter
// ---------------------------------------------------------------------------

static int extract_date_line(const char *buf, char *out, size_t out_cap) {
  if (!buf || !out || out_cap == 0) {
    return -1;
  }
  const char *date = strstr(buf, "Date: ");
  if (!date) {
    return -1;
  }
  const char *eol = strstr(date, "\r\n");
  if (!eol) {
    return -1;
  }
  size_t n = (size_t)(eol - date);
  if (n + 1 > out_cap) {
    return -1;
  }
  memcpy(out, date, n);
  out[n] = '\0';
  return 0;
}

static int build_headers_capture_date(struct tx_state_t *tx, char *date_out, size_t out_cap) {
  const char *buf = NULL;
  size_t len = 0;
  int r = tx_build_headers(tx,
                           "200 OK",
                           /*content_type=*/"text/plain",
                           /*emit_content_length=*/1,
                           /*content_len=*/42,
                           /*body=*/NULL,
                           /*body_send_len=*/0,
                           /*keepalive=*/1,
                           /*drain_after_headers=*/0,
                           /*extra_headers=*/NULL,
                           &buf,
                           &len);
  if (r != 0 || !buf || len == 0) {
    return -1;
  }
  return extract_date_line(buf, date_out, out_cap);
}

static uint64_t monotonic_ms(void) {
  struct timespec ts;
  if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
    return 0;
  }
  return (uint64_t)ts.tv_sec * 1000ull + (uint64_t)(ts.tv_nsec / 1000000ull);
}

static void sleep_ms(unsigned ms) {
  struct timespec req;
  req.tv_sec = (time_t)(ms / 1000u);
  req.tv_nsec = (long)((ms % 1000u) * 1000000u);
  (void)nanosleep(&req, NULL);
}

// 304 case: content_type=NULL, emit_content_length=0 -> omit both headers.
TEST t_build_headers_null_type_no_emit_cl_omits_both(void) {
  struct tx_state_t tx;
  memset(&tx, 0, sizeof(tx));
  const char *buf = NULL;
  size_t len = 0;
  int r = tx_build_headers(&tx,
                           "304 Not Modified",
                           /*content_type=*/NULL,
                           /*emit_content_length=*/0,
                           /*content_len=*/0,
                           /*body=*/NULL,
                           /*body_send_len=*/0,
                           /*keepalive=*/1,
                           /*drain_after_headers=*/0,
                           /*extra_headers=*/NULL,
                           &buf, &len);
  ASSERT_EQ(r, 0);
  ASSERT(buf != NULL && len > 0);
  ASSERT(strstr(buf, "Date: ") != NULL);
  ASSERT_EQ(strstr(buf, "Content-Type:"), NULL);
  ASSERT_EQ(strstr(buf, "Content-Length:"), NULL);
  tx_discard(&tx);
  PASS();
}

// 301/204 case: content_type=NULL, emit_content_length=1 -> Content-Length only.
TEST t_build_headers_null_type_emit_cl_zero(void) {
  struct tx_state_t tx;
  memset(&tx, 0, sizeof(tx));
  const char *buf = NULL;
  size_t len = 0;
  int r = tx_build_headers(&tx,
                           "301 Moved Permanently",
                           /*content_type=*/NULL,
                           /*emit_content_length=*/1,
                           /*content_len=*/0,
                           /*body=*/NULL,
                           /*body_send_len=*/0,
                           /*keepalive=*/1,
                           /*drain_after_headers=*/0,
                           /*extra_headers=*/NULL,
                           &buf, &len);
  ASSERT_EQ(r, 0);
  ASSERT(buf != NULL && len > 0);
  ASSERT(strstr(buf, "Date: ") != NULL);
  ASSERT_EQ(strstr(buf, "Content-Type:"), NULL);
  ASSERT(strstr(buf, "Content-Length: 0\r\n") != NULL);
  tx_discard(&tx);
  PASS();
}

// 200 regression: content_type provided -> both headers emitted.
TEST t_build_headers_type_provided_emits_both(void) {
  struct tx_state_t tx;
  memset(&tx, 0, sizeof(tx));
  const char *buf = NULL;
  size_t len = 0;
  int r = tx_build_headers(&tx,
                           "200 OK",
                           /*content_type=*/"text/plain",
                           /*emit_content_length=*/1,
                           /*content_len=*/42,
                           /*body=*/NULL,
                           /*body_send_len=*/0,
                           /*keepalive=*/1,
                           /*drain_after_headers=*/0,
                           /*extra_headers=*/NULL,
                           &buf, &len);
  ASSERT_EQ(r, 0);
  ASSERT(buf != NULL && len > 0);
  ASSERT(strstr(buf, "Date: ") != NULL);
  ASSERT(strstr(buf, "Content-Type: text/plain\r\n") != NULL);
  ASSERT(strstr(buf, "Content-Length: 42\r\n") != NULL);
  tx_discard(&tx);
  PASS();
}

TEST t_build_headers_rejects_total_size_overflow(void) {
  struct tx_state_t tx;
  memset(&tx, 0, sizeof(tx));

  const char *buf = NULL;
  size_t len = 0;
  static const char body_byte = 'x';

  int r = tx_build_headers(&tx,
                           "200 OK",
                           /*content_type=*/"text/plain",
                           /*emit_content_length=*/1,
                           /*content_len=*/42,
                           /*body=*/&body_byte,
                           /*body_send_len=*/SIZE_MAX,
                           /*keepalive=*/1,
                           /*drain_after_headers=*/0,
                           /*extra_headers=*/NULL,
                           &buf,
                           &len);
  ASSERT_EQ(r, -1);
  ASSERT_EQ(buf, NULL);
  ASSERT_EQ((int)len, 0);
  ASSERT_EQ(tx.dyn_buf, NULL);
  PASS();
}

TEST t_build_headers_date_reused_within_same_second(void) {
  struct tx_state_t tx;
  memset(&tx, 0, sizeof(tx));

  uint64_t start = monotonic_ms();
  if (start == 0) {
    tx_discard(&tx);
    FAILm("monotonic clock unavailable");
  }

  uint64_t deadline = start + 1500ull;
  while (1) {
    char date1[64];
    char date2[64];
    time_t t0 = time(NULL);
    ASSERT_NEQ(t0, (time_t)-1);
    ASSERT_EQ(build_headers_capture_date(&tx, date1, sizeof(date1)), 0);
    time_t t1 = time(NULL);
    ASSERT_NEQ(t1, (time_t)-1);
    ASSERT_EQ(build_headers_capture_date(&tx, date2, sizeof(date2)), 0);
    time_t t2 = time(NULL);
    ASSERT_NEQ(t2, (time_t)-1);

    if (t0 == t1 && t1 == t2) {
      ASSERT_EQ(strcmp(date1, date2), 0);
      tx_discard(&tx);
      PASS();
    }

    if (monotonic_ms() >= deadline) {
      break;
    }

    sleep_ms(1);
  }

  tx_discard(&tx);
  FAILm("timeout while sampling two header builds within the same second");
}

TEST t_build_headers_date_refreshes_after_second_rollover(void) {
  struct tx_state_t tx;
  memset(&tx, 0, sizeof(tx));

  char date1[64];
  char date2[64];
  ASSERT_EQ(build_headers_capture_date(&tx, date1, sizeof(date1)), 0);

  time_t sec = time(NULL);
  ASSERT_NEQ(sec, (time_t)-1);
  uint64_t start = monotonic_ms();
  if (start == 0) {
    tx_discard(&tx);
    FAILm("monotonic clock unavailable");
  }

  uint64_t deadline = start + 3000ull;
  while (time(NULL) == sec) {
    if (monotonic_ms() >= deadline) {
      tx_discard(&tx);
      FAILm("timeout waiting for second rollover");
    }
    sleep_ms(1);
  }

  ASSERT_EQ(build_headers_capture_date(&tx, date2, sizeof(date2)), 0);
  ASSERT_NEQ(strcmp(date1, date2), 0);

  tx_discard(&tx);
  PASS();
}

SUITE(s_tx_build_headers) {
  RUN_TEST(t_build_headers_null_type_no_emit_cl_omits_both);
  RUN_TEST(t_build_headers_null_type_emit_cl_zero);
  RUN_TEST(t_build_headers_type_provided_emits_both);
  RUN_TEST(t_build_headers_rejects_total_size_overflow);
  RUN_TEST(t_build_headers_date_reused_within_same_second);
  RUN_TEST(t_build_headers_date_refreshes_after_second_rollover);
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv) {
  GREATEST_MAIN_BEGIN();
  RUN_SUITE(s_tx);
  RUN_SUITE(s_tx_build_headers);
  GREATEST_MAIN_END();
}
