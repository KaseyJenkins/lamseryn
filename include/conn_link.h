#pragma once

#include <stdint.h>

struct conn;

// Per-connection worker linkage (active-set and timing wheel).
struct worker_link {
  int in_active_set;
  int active_idx;
  struct conn *tw_prev;
  struct conn *tw_next;
  uint32_t tw_slot;
};