#pragma once

#include <stddef.h>
#include <sys/types.h>

#include "types.h"

#ifndef ENABLE_ITEST_ECHO
#define ENABLE_ITEST_ECHO 0
#endif

// Per-connection transmit (TX) state for writes.
struct tx_state_t {
  size_t write_len;
  size_t write_off;
  size_t content_length_hint;
  const char *write_buf;
  void *dyn_buf;
  enum resp_kind resp_kind;
  int keepalive;
  int drain_after_headers;
  int write_poll_armed;
  int recv_armed;

  // Optional streaming file body state (static file serving).
  // When file_fd >= 0 and file_rem > 0, we must send headers first (via write_buf)
  // and then stream the file body using sendfile() on POLLOUT readiness.
  int file_fd;
  size_t file_rem;
  off_t file_off;

#if ENABLE_ITEST_ECHO
  // Integration-test only: when non-NULL, tx_set_dynamic_response_ex() appends
  // an X-Itest-Static-Mode header and then clears this field.
  const char *itest_static_mode;
#endif
};

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

void tx_init(struct tx_state_t *tx);
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
                     int emit_content_length,
                     size_t content_len,
                     const void *body,
                     size_t body_send_len,
                     int keepalive,
                     int drain_after_headers,
                     const char *extra_headers,
                     const char **buf,
                     size_t *len);

int tx_begin_sendfile(struct tx_state_t *tx, off_t offset, size_t length);

// Attach a file body to the TX state for sendfile progression. Replaces and
// closes any previously attached file on success. The caller retains ownership
// when this returns non-zero.
int tx_attach_sendfile(struct tx_state_t *tx, int fd, off_t offset, size_t length);

// Close and clear any file body attached to the TX state.
void tx_close_attached_file(struct tx_state_t *tx);

enum tx_decision tx_on_io_result(struct tx_state_t *tx,
                                 enum tx_io_kind kind,
                                 ssize_t sent,
                                 struct tx_next_io *out);

enum tx_decision tx_on_pollout(struct tx_state_t *tx, struct tx_next_io *out);

void tx_notify_poll_armed(struct tx_state_t *tx);
void tx_notify_poll_disarmed_staged(struct tx_state_t *tx);

// Recv arming state helpers.
int tx_recv_is_armed(const struct tx_state_t *tx);
void tx_recv_mark_armed(struct tx_state_t *tx);
void tx_recv_mark_disarmed(struct tx_state_t *tx);

// Pollout arming state helpers.
int tx_pollout_is_armed(const struct tx_state_t *tx);
int tx_should_arm_pollout(const struct tx_state_t *tx);

// Return non-zero when a sendfile body is attached and still has bytes left.
int tx_sendfile_is_active(const struct tx_state_t *tx);

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