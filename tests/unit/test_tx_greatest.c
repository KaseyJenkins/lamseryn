#include "../vendor/greatest_color.h"
#include "../vendor/greatest.h"

#include <string.h>

#include "include/conn.h"
#include "include/tx.h"

TEST t_after_full_response_431_draining_shutwr(void) {
  struct tx_state_t tx;
  memset(&tx, 0, sizeof(tx));
  tx.resp_kind = RK_431;
  tx.keepalive = 0;

  enum tx_after_action a = tx_after_full_response(&tx, /*conn_draining=*/1);
  ASSERT_EQ(a, TX_AFTER_SHUT_WR_AND_READ);
  PASS();
}

TEST t_after_full_response_503_close(void) {
  struct tx_state_t tx;
  memset(&tx, 0, sizeof(tx));
  tx.resp_kind = RK_503;
  tx.keepalive = 1;

  enum tx_after_action a = tx_after_full_response(&tx, /*conn_draining=*/0);
  ASSERT_EQ(a, TX_AFTER_CLOSE);
  PASS();
}

TEST t_after_full_response_keepalive_reset(void) {
  struct tx_state_t tx;
  memset(&tx, 0, sizeof(tx));
  tx.resp_kind = RK_OK_KA;
  tx.keepalive = 1;

  enum tx_after_action a = tx_after_full_response(&tx, /*conn_draining=*/0);
  ASSERT_EQ(a, TX_AFTER_KEEPALIVE_RESET);
  PASS();
}

TEST t_after_full_response_default_close(void) {
  struct tx_state_t tx;
  memset(&tx, 0, sizeof(tx));
  tx.resp_kind = RK_OK_CLOSE;
  tx.keepalive = 0;

  enum tx_after_action a = tx_after_full_response(&tx, /*conn_draining=*/0);
  ASSERT_EQ(a, TX_AFTER_CLOSE);
  PASS();
}

TEST t_pending_headers_returns_remaining_slice(void) {
  struct tx_state_t tx;
  memset(&tx, 0, sizeof(tx));
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
  struct tx_state_t tx;
  memset(&tx, 0, sizeof(tx));
  tx.write_buf = "abc";
  tx.write_len = 3;
  tx.write_off = 3;

  struct tx_next_io out = {0};
  ASSERT_EQ(tx_pending_headers(&tx, &out), 0);
  PASS();
}

TEST t_pollout_helpers_reflect_state(void) {
  struct tx_state_t tx;
  memset(&tx, 0, sizeof(tx));

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
  struct tx_state_t tx;
  memset(&tx, 0, sizeof(tx));

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
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv) {
  GREATEST_MAIN_BEGIN();
  RUN_SUITE(s_tx);
  GREATEST_MAIN_END();
}
