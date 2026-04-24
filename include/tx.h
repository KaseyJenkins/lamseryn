#pragma once

#include <stddef.h>
#include <sys/types.h>

#include "types.h"

struct tx_state_t;

enum tx_io_kind {
  TX_IO_HEADERS = 1,
  TX_IO_SENDFILE = 2
};

enum tx_decision {
  TX_NOOP = 0,
  TX_SEND_HEADERS,
  TX_START_SENDFILE,
  TX_RESUME_SENDFILE,
  TX_ARM_POLLOUT,
  TX_CANCEL_POLLOUT,
  TX_SHUT_WR_AND_READ,
  TX_DONE_KEEPALIVE,
  TX_DONE_CLOSE,
  TX_ERROR_CLOSE
};

struct tx_next_io {
  const char *buf;
  size_t len;
};

enum tx_after_action {
  TX_AFTER_CLOSE = 0,
  TX_AFTER_SHUT_WR_AND_READ,
  TX_AFTER_KEEPALIVE_RESET,
};

enum tx_sendfile_step {
  TX_SF_STEP_RETRY = 0,
  TX_SF_STEP_RESUME,
  TX_SF_STEP_ARM_POLLOUT,
  TX_SF_STEP_ERROR_CLOSE,
  TX_SF_STEP_SHUT_WR_AND_READ,
  TX_SF_STEP_AFTER_FULL_RESPONSE,
};

void tx_reset(struct tx_state_t *tx);

enum tx_decision tx_begin_headers(struct tx_state_t *tx,
                                  enum resp_kind rk,
                                  const char *buf,
                                  size_t len,
                                  int keepalive,
                                  int drain_after_headers,
                                  struct tx_next_io *out);

int tx_build_headers(struct tx_state_t *tx,
                     const char *status_line,
                     const char *content_type,
                     size_t content_len,
                     const void *body,
                     size_t body_send_len,
                     int keepalive,
                     int drain_after_headers,
                     const char *extra_headers,
                     const char **buf,
                     size_t *len);

int tx_begin_sendfile(struct tx_state_t *tx, off_t offset, size_t length);

enum tx_decision tx_on_io_result(struct tx_state_t *tx,
                                 enum tx_io_kind kind,
                                 ssize_t sent,
                                 struct tx_next_io *out);

enum tx_decision tx_on_pollout(struct tx_state_t *tx, struct tx_next_io *out);

void tx_notify_poll_armed(struct tx_state_t *tx);
void tx_notify_poll_disarmed_staged(struct tx_state_t *tx);

// Pollout arming state helpers.
int tx_pollout_is_armed(const struct tx_state_t *tx);
int tx_should_arm_pollout(const struct tx_state_t *tx);

void tx_discard(struct tx_state_t *tx);

// Return pending header bytes to send (write_buf/write_off/write_len), if any.
// Returns 1 and fills out on success, 0 if no pending header bytes exist.
int tx_pending_headers(const struct tx_state_t *tx, struct tx_next_io *out);

// Decide post-response lifecycle action after response bytes are fully sent.
enum tx_after_action tx_after_full_response(const struct tx_state_t *tx, int conn_draining);

// Compute the next sendfile chunk size according to TX policy.
size_t tx_next_sendfile_chunk(const struct tx_state_t *tx);

// Classify the next sendfile handling step from a TX decision.
enum tx_sendfile_step tx_sendfile_step_from_decision(enum tx_decision d);