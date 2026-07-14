#pragma once

#include <stdint.h>

#include "types.h"

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