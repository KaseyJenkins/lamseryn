#pragma once

#include <stdint.h>
#include <stddef.h>
#include <sys/types.h>
#include <netinet/in.h>

#include "macros.h"
#include "types.h"
#include "req_arena.h"
#include <llhttp.h>

#ifndef ENABLE_ITEST_ECHO
#define ENABLE_ITEST_ECHO 0
#endif

#ifndef REQ_TARGET_MAX
#define REQ_TARGET_MAX 2048
#endif

#ifndef REQ_PATH_MAX
#define REQ_PATH_MAX 2048
#endif

#ifndef REQ_HDRS_MAX
#define REQ_HDRS_MAX 32
#endif

#ifndef REQ_HDR_NAME_MAX
#define REQ_HDR_NAME_MAX 32
#endif

#ifndef REQ_HDR_VALUE_MAX
#define REQ_HDR_VALUE_MAX 128
#endif

struct req_hdr_entry {
  uint8_t name_len;
  uint16_t value_len;
  char *name;
  char *value;
};

// Per-connection deadline and timeout state.
struct deadline_state {
  uint64_t last_active_ms;
  uint64_t header_start_ms;
  uint64_t header_start_us;
  uint64_t write_start_ms;
  int ka_idle;
  int closing;
  int draining;
  int abortive_close;
  uint64_t drain_deadline_ms;

  enum deadline_kind deadline_kind;
  uint64_t deadline_ms;
  int deadline_active;
};

// Per-connection worker linkage (active-set and timing wheel).
struct worker_link {
  int in_active_set;
  int active_idx;
  struct conn *tw_prev;
  struct conn *tw_next;
  uint32_t tw_slot;
};

// Per-connection HTTP/1 parser and header state.
struct http1_state {
  llhttp_t parser;

  // Which request headers we store for later (feature-driven, per-connection).
  uint64_t req_hdr_store_mask;
  // Which stored headers we've already captured for this request (to ignore duplicates).
  uint64_t req_hdr_seen_mask;

  // Request start-line fields.
  llhttp_method_t method;
  uint8_t method_set;

  // Raw request target (origin-form), collected via llhttp on_url.
  uint16_t target_len;
  uint8_t target_too_long;
  char *target;
  uint16_t target_cap; // <= REQ_TARGET_MAX

  // Parsed path/query view over target.
  uint16_t path_len;
  uint16_t query_off;
  uint16_t query_len;

  // Decoded and normalized absolute path (starts with '/').
  uint16_t path_dec_len;
  uint16_t path_norm_len;
  uint8_t path_bad;
  uint8_t path_ends_with_slash;
  char *path_dec;
  uint16_t path_dec_cap;
  char *path_norm;
  uint16_t path_norm_cap;

  // If set, the method is not supported by the server and we should reply 405.
  uint8_t method_not_allowed;

  int headers_done;
  int message_done;
  int parse_error;
  int internal_error;
  int header_too_big;
  int header_fields_too_many;
  int unsupported_te;
  int want_keepalive;
  size_t parser_bytes;
  uint64_t body_remaining;
  uint64_t body_bytes;
  int body_too_big;
  int pending_line_error;

  // Minimal header parsing state used for framing correctness.
  uint8_t hdr_state;
  uint8_t hdr_interest;
  uint8_t hdr_name_len;
  uint8_t hdr_val_len;
  char hdr_name[32];
  char hdr_val[64];

  // Raw header value accumulator for request-header capture.
  uint8_t hdr_val_raw_len;
  char hdr_val_raw[REQ_HDR_VALUE_MAX];

  // Bounded request-header capture (lowercased name, raw value).
  uint8_t req_hdr_count;

  // Total header fields seen for this request (counts ALL header fields, not just stored ones).
  // hdr_fields_max is configured per connection.
  uint16_t hdr_fields_count;
  uint16_t hdr_fields_max;
  // Captured headers used by server logic (only those enabled by store mask).
  // Backed by the per-request arena.
  struct req_hdr_entry *req_hdrs;
  uint8_t req_hdr_cap;

  // Per-request non-moving arena for request target/path/header copies.
  // Reset on keep-alive request boundary; destroyed on conn recycle.
  struct req_arena arena;

  uint8_t cl_count;
  uint8_t te_count;
  uint8_t expect_count;
  uint8_t cl_invalid;
  uint8_t te_chunked;
  uint8_t te_other;
  uint8_t expect_100_continue;
  uint8_t expect_unsupported;
  uint64_t cl_value;
};

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

// Connection state shared between translation units.
struct conn {
  // HTTP/1 parsing and header state.
  struct http1_state h1;

  // RX pipelining support:
  // - rx_tail: last up to 3 bytes of the current request's header stream,
  //            used to detect "\r\n\r\n" across recv boundaries.
  // - rx_stash: bytes that arrived *after* the current request's header
  //             terminator (start of the next pipelined request).
  //             Allocated lazily only if we actually need to stash.
  uint8_t rx_tail_len;
  char rx_tail[3];
  uint16_t rx_stash_len;
  char *rx_stash;

  // Transmit pipeline state.
  struct tx_state_t tx;

  // Deadline/timeout state.
  struct deadline_state dl;

  // Per-connection worker/timer linkage and activity hints.
  int noselect_streak;
  struct worker_link link;

  // Core identity and lifetime.
  int fd;
  int refcnt;

  // Monotonic per-worker generation for identity/debug correlation.
  uint32_t generation;

  // Per-connection persistent ops used as user_data keys.
  struct op_ctx op_read;
  struct op_ctx op_write;
  struct op_ctx op_write_ready;
  struct op_ctx op_close;

  // Dedicated freelist link (separate from timing-wheel linkage).
  struct conn *free_next;

  const struct vhost_t *vhost;

  // Remote peer address captured at accept time.
  char remote_ip[INET6_ADDRSTRLEN];
  uint16_t remote_port;

  // TLS transport state.
  int tls_enabled;
  int tls_handshake_done;
  int tls_want_read;
  int tls_want_write;
  void *tls_handle;
};
