#pragma once

#include <limits.h>
#include <stdint.h>
#include <stddef.h>
#include "macros.h"

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

// Operation kind encoded in user_data.
enum op_type {
  OP_CLOSE = 1,
  OP_WRITE,
  OP_WRITE_READY,
  OP_READ,
  OP_ACCEPT, // Non-connection op.
  OP_WAKE, // Non-connection op.
  OP_SWEEP, // Non-connection op.
  OP_ACCEPT_BACKOFF // Non-connection op.
};

struct conn;

struct op_ctx {
  uint32_t magic;
  enum op_type type;
  struct conn *c; // Used for connection-bound ops; NULL for non-connection ops.
  int fd; // Used by ACCEPT/WAKE; -1 for timers.
};

// Response classification.
enum resp_kind {
  RK_NONE = 0,
  RK_OK_CLOSE,
  RK_OK_KA,
  RK_400,
  RK_403,
  RK_404,
  RK_405,
  RK_408,
  RK_413,
  RK_431,
  RK_500,
  RK_501,
  RK_503
};

// Deadline classification.
enum deadline_kind {
  DK_NONE = 0,
  DK_DRAIN,
  DK_KA_IDLE,
  DK_INITIAL_IDLE,
  DK_HEADER_TIMEOUT,
  DK_BODY_TIMEOUT,
  DK_WRITE_TIMEOUT,
};

// Feature flags for optional behaviors.
// These flags drive request-header storage policy for downstream logic.
// Protocol-safety headers (Content-Length/Transfer-Encoding) remain enforced regardless.
enum {
  CFG_FEAT_STATIC = 1ULL << 0,
  CFG_FEAT_RANGE = 1ULL << 1,
  CFG_FEAT_CONDITIONAL = 1ULL << 2,
  CFG_FEAT_COMPRESSION = 1ULL << 3,
  CFG_FEAT_AUTH = 1ULL << 4,
};

struct vhost_t {
  char name[64];
  char bind[64];
  uint16_t port;
  char docroot[PATH_MAX];
  int docroot_fd;
  uint64_t features; // CFG_FEAT_* bits.
  uint16_t max_header_fields; // Applied during connection initialization.

  // TLS configuration; vhost keys may override global defaults.
  uint8_t tls_enabled;
  uint8_t tls_enabled_set;
  char tls_cert_file[PATH_MAX];
  char tls_key_file[PATH_MAX];
  char tls_min_version[16];
  char tls_ciphers[256];
  char tls_ciphersuites[256];
  uint8_t tls_session_tickets;
  uint8_t tls_session_tickets_set;
  uint8_t tls_session_cache;
  uint8_t tls_session_cache_set;
  void *tls_ctx_handle; // Opaque TLS context owned by startup/shutdown lifecycle.
};

enum {
  GF_LOG_LEVEL = 1u << 0,
  GF_LOG_CATEGORIES = 1u << 1,
  GF_QUEUE_DEPTH = 1u << 2,
  GF_PRE_ACCEPTS = 1u << 3,
  GF_WORKERS = 1u << 4,
  GF_INITIAL_IDLE_TIMEOUT = 1u << 5,
  GF_KA_IDLE_CLOSE = 1u << 6,
  GF_HEADER_TIMEOUT = 1u << 7,
  GF_BODY_TIMEOUT = 1u << 8,
  GF_WRITE_TIMEOUT = 1u << 9,
  GF_DRAIN_TIMEOUT = 1u << 10,
  GF_ACCEPT_BACKOFF_MS = 1u << 11,
  GF_SHUTDOWN_GRACE_MS = 1u << 12,
  GF_DEFAULT_MAX_HDR_FIELDS = 1u << 13,
  GF_TLS_ENABLED = 1u << 14,
  GF_TLS_CERT_FILE = 1u << 15,
  GF_TLS_KEY_FILE = 1u << 16,
  GF_TLS_MIN_VERSION = 1u << 17,
  GF_TLS_CIPHERS = 1u << 18,
  GF_TLS_CIPHERSUITES = 1u << 19,
  GF_TLS_SESSION_TICKETS = 1u << 20,
  GF_TLS_SESSION_CACHE = 1u << 21,
  GF_WAKE_PIPE_MODE = 1u << 22,
  GF_ACCESS_LOG_ENABLED = 1u << 23,
  GF_ACCESS_LOG_PATH = 1u << 24,
  GF_ACCESS_LOG_FORMAT = 1u << 25,
  GF_ACCESS_LOG_SAMPLE = 1u << 26,
  GF_ACCESS_LOG_MIN_STATUS = 1u << 27,
};

// Runtime-global tunables parsed from INI [globals].
struct globals_cfg {
  uint32_t present; // Bitmask of GF_*.

  int log_level; // LOG_ERROR..LOG_TRACE.
  unsigned log_categories; // Bitmask (LOGC_*).

  unsigned queue_depth; // Ring depth.
  unsigned pre_accepts; // Initial accepts per worker.
  unsigned workers; // Worker thread count.

  unsigned initial_idle_timeout_ms;
  unsigned keepalive_idle_close_ms; // Keep-alive idle close timeout.
  unsigned header_timeout_ms;
  unsigned body_timeout_ms;
  unsigned write_timeout_ms;
  unsigned drain_timeout_ms;

  unsigned accept_backoff_ms;
  unsigned shutdown_grace_ms;

  unsigned default_max_header_fields; // Optional global default for vhosts.

  // Optional global TLS defaults consumed by vhosts.
  unsigned tls_enabled;
  char tls_cert_file[PATH_MAX];
  char tls_key_file[PATH_MAX];
  char tls_min_version[16];
  char tls_ciphers[256];
  char tls_ciphersuites[256];
  unsigned tls_session_tickets;
  unsigned tls_session_cache;

  // 0 = shared, 1 = per-worker.
  unsigned wake_pipe_mode;

  // Access-log knobs.
  unsigned access_log_enabled;
  char access_log_path[PATH_MAX];
  char access_log_format[16];
  unsigned access_log_sample;
  unsigned access_log_min_status;
};

struct config_t {
  int vhost_count;
  struct vhost_t vhosts[32];

  // Runtime-global knobs.
  struct globals_cfg g;
};
