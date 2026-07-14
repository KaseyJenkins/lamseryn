#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "conn.h"
#include "time_utils.h"
#include "tx.h"

static inline int tx_is_eagain(ssize_t sent) {
  return (sent == -EAGAIN || sent == -EWOULDBLOCK);
}

static _Thread_local time_t g_tx_cached_date_sec = (time_t)-1;
static _Thread_local char g_tx_cached_date_line[64];

static const char *tx_cached_date_header_line(void) {
  time_t now = time(NULL);
  if (now == (time_t)-1) {
    g_tx_cached_date_sec = (time_t)-1;
    g_tx_cached_date_line[0] = '\0';
    return "";
  }

  if (g_tx_cached_date_sec == now && g_tx_cached_date_line[0] != '\0') {
    return g_tx_cached_date_line;
  }

  char date_value[40];
  if (time_format_http_date(date_value, sizeof(date_value), now) == 0) {
    g_tx_cached_date_sec = (time_t)-1;
    g_tx_cached_date_line[0] = '\0';
    return "";
  }

  int dn = snprintf(g_tx_cached_date_line,
                    sizeof(g_tx_cached_date_line),
                    "Date: %s\r\n",
                    date_value);
  if (dn <= 0 || (size_t)dn >= sizeof(g_tx_cached_date_line)) {
    g_tx_cached_date_sec = (time_t)-1;
    g_tx_cached_date_line[0] = '\0';
    return "";
  }

  g_tx_cached_date_sec = now;
  return g_tx_cached_date_line;
}

#if ENABLE_ITEST_ECHO
static int tx_status_in_forced_fail_list(const char *status_line,
                                         const char *list) {
  if (!status_line || !list || !list[0]) {
    return 0;
  }

  if (!status_line[0] || !status_line[1] || !status_line[2]) {
    return 0;
  }

  // status_line is expected to start with "XYZ ...".
  const char c0 = status_line[0];
  const char c1 = status_line[1];
  const char c2 = status_line[2];
  if (c0 < '0' || c0 > '9' || c1 < '0' || c1 > '9' || c2 < '0' || c2 > '9') {
    return 0;
  }

  const char *p = list;
  while (*p) {
    while (*p == ' ' || *p == '\t' || *p == ',') {
      p++;
    }
    if (!*p) {
      break;
    }

    const char *token_start = p;
    while (*p && *p != ',') {
      p++;
    }
    const char *token_end = p;
    while (token_end > token_start
           && (token_end[-1] == ' ' || token_end[-1] == '\t')) {
      token_end--;
    }

    size_t token_len = (size_t)(token_end - token_start);
    if (token_len == 3
        && token_start[0] == c0
        && token_start[1] == c1
        && token_start[2] == c2) {
      return 1;
    }
  }

  return 0;
}

static int tx_test_force_header_build_fail(const char *status_line) {
  const char *list = getenv("TX_TEST_FORCE_HEADER_BUILD_FAIL_STATUS");
  return tx_status_in_forced_fail_list(status_line, list);
}
#endif

void tx_init(struct tx_state_t *tx) {
  if (!tx) {
    return;
  }
  memset(tx, 0, sizeof(*tx));
  tx->file_fd = -1;
}

void tx_reset(struct tx_state_t *tx) {
  if (!tx) {
    return;
  }

  tx_close_attached_file(tx);

  if (tx->dyn_buf) {
    free(tx->dyn_buf);
    tx->dyn_buf = NULL;
  }

  tx->write_buf = NULL;
  tx->write_len = 0;
  tx->write_off = 0;
  tx->content_length_hint = 0;
  tx->resp_kind = RK_NONE;

  tx->write_poll_armed = 0;

  tx->keepalive = 0;
  tx->drain_after_headers = 0;
  tx->recv_armed = 0;

#if ENABLE_ITEST_ECHO
  tx->itest_static_mode = NULL;
#endif
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
                     int emit_content_length,
                     size_t content_len,
                     const void *body,
                     size_t body_send_len,
                     int keepalive,
                     int drain_after_headers,
                     const char *extra_headers,
                     const char **buf,
                     size_t *len) {
  if (!tx || !status_line || !buf || !len) {
    return -1;
  }

  if (body_send_len > 0 && !body) {
    return -1;
  }

#if ENABLE_ITEST_ECHO
  const char *itest_static_mode = tx->itest_static_mode;
  tx->itest_static_mode = NULL;

  // Integration-test hook: force header-builder failure for selected statuses
  // to exercise emergency fallback paths.
  if (tx_test_force_header_build_fail(status_line)) {
    return -1;
  }
#endif

  const char *conn_hdr = keepalive ? "keep-alive" : "close";
  const char *date_hdr = tx_cached_date_header_line();

#if ENABLE_ITEST_ECHO
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

  if (!extra_headers) {
    extra_headers = "";
  }

  // Three modes controlled by content_type and emit_content_length:
  //   content_type != NULL              -> Content-Type + Content-Length (200/206 etc.)
  //   content_type == NULL, emit_cl=1   -> Content-Length only (301, 204, 416)
  //   content_type == NULL, emit_cl=0   -> neither header (304 Not Modified)
  // Sizing: "Content-Type: " (14) + longest MIME ~38 + "\r\n" (2)
  //       + "Content-Length: " (16) + 20 digits + "\r\n" (2) = ~92. 128 is safe.
  char content_lines[128];
  if (content_type) {
    int cl = snprintf(content_lines, sizeof(content_lines),
                      "Content-Type: %s\r\n"
                      "Content-Length: %zu\r\n",
                      content_type, content_len);
    if (cl <= 0 || (size_t)cl >= sizeof(content_lines)) {
      return -1;
    }
  } else if (emit_content_length) {
    int cl = snprintf(content_lines, sizeof(content_lines),
                      "Content-Length: %zu\r\n",
                      content_len);
    if (cl <= 0 || (size_t)cl >= sizeof(content_lines)) {
      return -1;
    }
  } else {
    content_lines[0] = '\0';
  }

  int hlen = snprintf(NULL,
                      0,
                      "HTTP/1.1 %s\r\n"
                      "%s"
                      "%s"
                      "Connection: %s\r\n"
#if ENABLE_ITEST_ECHO
                      "%s"
#endif
                      "%s"
                      "\r\n",
                      status_line,
                      date_hdr,
                      content_lines,
                      conn_hdr
#if ENABLE_ITEST_ECHO
                      ,
                      itest_hdr
#endif
                      ,
                      extra_headers
  );
  if (hlen <= 0) {
    return -1;
  }

  if (body_send_len > (SIZE_MAX - (size_t)hlen - 1u)) {
    return -1;
  }

  size_t total = (size_t)hlen + body_send_len;
  char *owned = (char *)malloc(total + 1);
  if (!owned) {
    return -1;
  }

  int written = snprintf(owned,
                         (size_t)hlen + 1,
                         "HTTP/1.1 %s\r\n"
                         "%s"
                         "%s"
                         "Connection: %s\r\n"
#if ENABLE_ITEST_ECHO
                         "%s"
#endif
                         "%s"
                         "\r\n",
                         status_line,
                         date_hdr,
                         content_lines,
                         conn_hdr
#if ENABLE_ITEST_ECHO
                         ,
                         itest_hdr
#endif
                         ,
                         extra_headers);
  if (written != hlen) {
    free(owned);
    return -1;
  }
  if (body_send_len) {
    memcpy(owned + (size_t)hlen, body, body_send_len);
  }
  owned[total] = '\0';

  if (tx->dyn_buf) {
    free(tx->dyn_buf);
    tx->dyn_buf = NULL;
  }

  tx->dyn_buf = owned;
  tx->content_length_hint = content_len;
  tx->keepalive = keepalive ? 1 : 0;
  tx->drain_after_headers = drain_after_headers ? 1 : 0;

  *buf = owned;
  *len = total;

  return 0;
}

int tx_begin_sendfile(struct tx_state_t *tx, off_t offset, size_t length) {
  if (!tx) {
    return -1;
  }

  tx->file_rem = length;
  tx->file_off = offset;
  return 0;
}

void tx_close_attached_file(struct tx_state_t *tx) {
  if (!tx) {
    return;
  }
  if (tx->file_fd >= 0) {
    close(tx->file_fd);
    tx->file_fd = -1;
  }
  tx->file_off = 0;
  tx->file_rem = 0;
}

int tx_attach_sendfile(struct tx_state_t *tx, int fd, off_t offset, size_t length) {
  if (!tx || fd < 0 || length == 0) {
    return -1;
  }

  tx_close_attached_file(tx);
  tx->file_fd = fd;
  tx->file_rem = length;
  tx->file_off = offset;
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
