#pragma once

#include <stdint.h>
#include <stddef.h>
#include <liburing.h>
#include <llhttp.h>

#include "instrumentation/instrumentation.h"
#include "include/macros.h"
#include "include/config.h"
#include "include/types.h"

struct conn;

// Per-worker configuration (thread id, CPU pinning, wake pipe, config)
struct worker_cfg {
  int thread_id;
  int cpu_core; // -1 means no pin
  int wake_rd; // pipe read end for waking

  const struct config_t *config;
};

// Per-worker context: ring, listeners, timing wheel, stats, etc.
struct worker_ctx {
  struct worker_cfg cfg;

  // Listeners (cold compared to the ring)
  int num_listeners;
  int listen_fds[32];

  // HOT: io_uring and batching state
  struct io_uring ring;
  int need_submit; // hot: ring_ops
  int sqes_pending; // hot: ring_ops
  uint64_t now_cached_ms; // hot: CQE loop timing
  int is_draining; // cached g_shutdown_state == SHUTDOWN_DRAIN

  // HTTP parser settings (mostly immutable after init)
  llhttp_settings_t http_settings;

  // Accept throttling (rarely touched vs ring)
  int accept_paused;
  int accept_backoff_armed;

  // Active FD bookkeeping (cold unless you tear down a lot)
  int *active_fds;
  size_t active_count;
  size_t active_cap;

  // Accept strategy flags
  int accept_multishot;
  int accept_cancel_inflight;

  // Persistent ops for robustness (never malloc/free per completion)
  struct op_ctx wake_static;
  struct op_ctx sweep_static;

  // persistent accept ops (one per listener) + backoff
  struct op_ctx accept_static[32];
  uint8_t accept_inited[32];
  struct op_ctx accept_backoff_static;

  // Centralized counters (generated from counters.def, gated by instrumentation level)
  struct {
#include "instrumentation/counters_fields.h"
  } stats;

  // Timing wheel (per worker)
  uint32_t tw_slots; // == TW_SLOTS
  uint32_t tw_mask; // tw_slots - 1, requires power-of-two slots
  uint32_t tw_tick_ms; // == TW_TICK_MS
  uint64_t tw_now_tick;
  uint64_t next_tick_deadline_ms;

  // Potentially big; keep it near the end to avoid pushing hot fields around
  struct conn *tw_buckets[TW_SLOTS];

  // monotonically increasing generation for new conns (per worker)
  uint32_t next_conn_generation;

} __attribute__((aligned(64)));