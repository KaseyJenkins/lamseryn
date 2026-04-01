#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "conn.h"
#include "tx.h"

static inline int tx_is_eagain(ssize_t sent) {
  return (sent == -EAGAIN || sent == -EWOULDBLOCK);
}

void tx_reset(struct tx_state_t *tx) {
  if (!tx) {
    return;
  }

  if (tx->dyn_buf) {
    free(tx->dyn_buf);
    tx->dyn_buf = NULL;
  }

  tx->write_buf = NULL;
  tx->write_len = 0;
  tx->write_off = 0;
  tx->resp_kind = RK_NONE;

  tx->write_poll_armed = 0;

  tx->keepalive = 0;
  tx->drain_after_headers = 0;

  tx->file_rem = 0;
  tx->file_off = 0;
}

enum tx_decision tx_begin_headers(struct tx_state_t *tx,
                                  enum resp_kind rk,
                                  const char *buf,
                                  size_t len,
                                  int keepalive,
                                  int drain_after_headers,
                                  struct tx_next_io *out) {
  if (!tx || !buf || len == 0) {
    return TX_ERROR_CLOSE;
  }

  tx->resp_kind = rk;
  tx->write_buf = buf;
  tx->write_len = len;
  tx->write_off = 0;

  tx->keepalive = keepalive ? 1 : 0;
  tx->drain_after_headers = drain_after_headers ? 1 : 0;

  if (out) {
    out->buf = buf;
    out->len = len;
  }

  return TX_SEND_HEADERS;
}

int tx_build_headers(struct tx_state_t *tx,
                     const char *status_line,
                     const char *content_type,
                     size_t content_len,
                     const void *body,
                     size_t body_send_len,
                     int keepalive,
                     int drain_after_headers,
                     const char **buf,
                     size_t *len) {
  if (!tx || !status_line || !buf || !len) {
    return -1;
  }

  if (body_send_len > 0 && !body) {
    return -1;
  }

  const char *conn_hdr = keepalive ? "keep-alive" : "close";
  if (!content_type) {
    content_type = "application/octet-stream";
  }

#if ENABLE_ITEST_ECHO
  const char *itest_static_mode = tx->itest_static_mode;
  tx->itest_static_mode = NULL;

  const char *itest_hdr = "";
  char itest_line[96];
  if (itest_static_mode) {
    int n =
      snprintf(itest_line, sizeof(itest_line), "X-Itest-Static-Mode: %s\r\n", itest_static_mode);
    if (n > 0 && (size_t)n < sizeof(itest_line)) {
      itest_hdr = itest_line;
    }
  }
#endif

  char head[640];
  int hlen = snprintf(head,
                      sizeof(head),
                      "HTTP/1.1 %s\r\n"
                      "Content-Type: %s\r\n"
                      "Content-Length: %zu\r\n"
                      "Connection: %s\r\n"
#if ENABLE_ITEST_ECHO
                      "%s"
#endif
                      "\r\n",
                      status_line,
                      content_type,
                      content_len,
                      conn_hdr
#if ENABLE_ITEST_ECHO
                      ,
                      itest_hdr
#endif
  );
  if (hlen <= 0 || (size_t)hlen >= sizeof(head)) {
    return -1;
  }

  size_t total = (size_t)hlen + body_send_len;
  char *owned = (char *)malloc(total);
  if (!owned) {
    return -1;
  }

  memcpy(owned, head, (size_t)hlen);
  if (body_send_len) {
    memcpy(owned + (size_t)hlen, body, body_send_len);
  }

  if (tx->dyn_buf) {
    free(tx->dyn_buf);
    tx->dyn_buf = NULL;
  }

  tx->dyn_buf = owned;
  tx->keepalive = keepalive ? 1 : 0;
  tx->drain_after_headers = drain_after_headers ? 1 : 0;

  *buf = owned;
  *len = total;

  return 0;
}

int tx_begin_sendfile(struct tx_state_t *tx, size_t file_size) {
  if (!tx) {
    return -1;
  }

  tx->file_rem = file_size;
  tx->file_off = 0;
  return 0;
}

enum tx_decision tx_on_io_result(struct tx_state_t *tx,
                                 enum tx_io_kind kind,
                                 ssize_t sent,
                                 struct tx_next_io *out) {
  if (!tx) {
    return TX_NOOP;
  }

  if (sent == 0) {
    return TX_ERROR_CLOSE;
  }

  if (sent < 0) {
    if (tx_is_eagain(sent)) {
      return TX_ARM_POLLOUT;
    }
    if (kind == TX_IO_SENDFILE && sent == -EINTR) {
      return TX_NOOP;
    }
    return TX_ERROR_CLOSE;
  }

  if (kind == TX_IO_HEADERS) {
    if (!tx->write_buf || tx->write_len == 0) {
      return TX_ERROR_CLOSE;
    }

    tx->write_off += (size_t)sent;

    if (tx->write_off < tx->write_len) {
      if (out) {
        out->buf = tx->write_buf + tx->write_off;
        out->len = tx->write_len - tx->write_off;
      }
      return TX_SEND_HEADERS;
    }

    if (tx->drain_after_headers) {
      return TX_SHUT_WR_AND_READ;
    }

    if (tx->file_fd >= 0 && tx->file_rem > 0) {
      return TX_START_SENDFILE;
    }

    return tx->keepalive ? TX_DONE_KEEPALIVE : TX_DONE_CLOSE;
  }

  if (kind == TX_IO_SENDFILE) {
    // Note: sendfile offset is advanced by the kernel through &file_off;
    // here we only account for remaining bytes.
    if (tx->file_rem == 0) {
      return tx->keepalive ? TX_DONE_KEEPALIVE : TX_DONE_CLOSE;
    }

    size_t s = (size_t)sent;
    if (s > tx->file_rem) {
      s = tx->file_rem;
    }
    tx->file_rem -= s;

    if (tx->file_rem > 0) {
      return TX_RESUME_SENDFILE;
    }

    return tx->keepalive ? TX_DONE_KEEPALIVE : TX_DONE_CLOSE;
  }

  return TX_NOOP;
}

enum tx_decision tx_on_pollout(struct tx_state_t *tx, struct tx_next_io *out) {
  if (!tx) {
    return TX_NOOP;
  }

  if (tx->file_fd >= 0 && tx->file_rem > 0 && (!tx->write_buf || tx->write_off >= tx->write_len)) {
    return TX_RESUME_SENDFILE;
  }

  struct tx_next_io tmp = {0};
  if (tx->resp_kind != RK_NONE && tx_pending_headers(tx, &tmp)) {
    if (out) {
      *out = tmp;
    }
    return TX_SEND_HEADERS;
  }

  return TX_NOOP;
}

void tx_notify_poll_armed(struct tx_state_t *tx) {
  if (tx) {
    tx->write_poll_armed = 1;
  }
}

void tx_notify_poll_disarmed_staged(struct tx_state_t *tx) {
  if (tx) {
    tx->write_poll_armed = 0;
  }
}

int tx_pollout_is_armed(const struct tx_state_t *tx) {
  return (tx && tx->write_poll_armed) ? 1 : 0;
}

int tx_should_arm_pollout(const struct tx_state_t *tx) {
  return (tx && !tx->write_poll_armed) ? 1 : 0;
}

void tx_discard(struct tx_state_t *tx) {
  if (!tx) {
    return;
  }

  if (tx->dyn_buf) {
    free(tx->dyn_buf);
    tx->dyn_buf = NULL;
  }

  tx->write_buf = NULL;
  tx->write_len = 0;
  tx->write_off = 0;

}

int tx_pending_headers(const struct tx_state_t *tx, struct tx_next_io *out) {
  if (!tx || !out) {
    return 0;
  }
  if (!tx->write_buf || tx->write_len == 0) {
    return 0;
  }
  if (tx->write_off >= tx->write_len) {
    return 0;
  }

  out->buf = tx->write_buf + tx->write_off;
  out->len = tx->write_len - tx->write_off;
  return 1;
}

enum tx_after_action tx_after_full_response(const struct tx_state_t *tx, int conn_draining) {
  if (!tx) {
    return TX_AFTER_CLOSE;
  }

  if (conn_draining && tx->resp_kind == RK_431) {
    return TX_AFTER_SHUT_WR_AND_READ;
  }

  if (tx->resp_kind == RK_503) {
    return TX_AFTER_CLOSE;
  }

  if (tx->keepalive) {
    return TX_AFTER_KEEPALIVE_RESET;
  }

  return TX_AFTER_CLOSE;
}

size_t tx_next_sendfile_chunk(const struct tx_state_t *tx) {
  const size_t TX_MAX_SENDFILE_CHUNK = (size_t)(1u << 20); // 1 MiB
  if (!tx || tx->file_rem == 0) {
    return 0;
  }
  return (tx->file_rem > TX_MAX_SENDFILE_CHUNK) ? TX_MAX_SENDFILE_CHUNK : tx->file_rem;
}

enum tx_sendfile_step tx_sendfile_step_from_decision(enum tx_decision d) {
  switch (d) {
  case TX_RESUME_SENDFILE:
    return TX_SF_STEP_RESUME;
  case TX_NOOP:
    return TX_SF_STEP_RETRY;
  case TX_ARM_POLLOUT:
    return TX_SF_STEP_ARM_POLLOUT;
  case TX_ERROR_CLOSE:
    return TX_SF_STEP_ERROR_CLOSE;
  case TX_SHUT_WR_AND_READ:
    return TX_SF_STEP_SHUT_WR_AND_READ;
  default:
    return TX_SF_STEP_AFTER_FULL_RESPONSE;
  }
}
