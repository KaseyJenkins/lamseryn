#pragma once

#include "types.h"

struct access_log_cfg {
  int enabled;
  char path[PATH_MAX];
  char format[16];
  unsigned sample;
  unsigned min_status;
};

struct access_log_event {
  uint64_t ts_ms;
  unsigned worker;
  const char *method;
  const char *target;
  size_t target_len;
  const char *remote_ip;
  unsigned remote_port;
  unsigned status;
  uint64_t bytes;
  unsigned dur_ms;
  int keepalive;
  int tls;
};

struct worker_ctx;
struct conn;

// Builds a normalized access-log config view from [globals].
void access_log_cfg_from_globals(struct access_log_cfg *out, const struct globals_cfg *g);

// Sanitizes request target into printable ASCII and bounds its size.
// Returns bytes written to out (excluding trailing NUL).
size_t access_log_sanitize_target(char *out,
                                  size_t out_cap,
                                  const char *target,
                                  size_t target_len,
                                  unsigned target_max,
                                  int *truncated);

// Formats a single text key=value access log line ending with '\n'.
// Returns bytes written (excluding trailing NUL), or 0 on failure.
size_t access_log_format_text_line(char *out,
                                   size_t out_cap,
                                   const struct access_log_event *ev,
                                   unsigned target_max);

// Initializes access-log runtime sink/config from [globals].
// Returns 0 on success, -1 when sink initialization fails.
int access_log_runtime_init(const struct globals_cfg *g);

// Releases runtime sink resources opened by access_log_runtime_init().
void access_log_runtime_shutdown(void);

// Reopens file-backed access-log sink after external rotation.
// Returns 0 on success or no-op, -1 on reopen failure.
int access_log_runtime_reopen(void);

// Emits one access log line from connection/request state when filters allow it.
void access_log_emit_from_conn(struct worker_ctx *w, struct conn *c);

// Periodic runtime hook called from worker loop to flush aged direct-mode buffers.
void access_log_runtime_poll(struct worker_ctx *w);
