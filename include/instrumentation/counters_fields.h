#pragma once

#include "instrumentation.h"

// Keep this macro table hand-aligned for readability.
// clang-format off

// Map CTR(OPS, name)/CTR(DEV, name) to field declarations or no-op.
#if INSTRUMENTATION_LEVEL >= LVL_OPS
  #define CTR_OPS(name) uint64_t name;
#else
  #define CTR_OPS(name)
#endif

#if INSTRUMENTATION_LEVEL >= LVL_DEV
  #define CTR_DEV(name) uint64_t name;
#else
  #define CTR_DEV(name)
#endif

// Dispatch CTR(level, name) to CTR_OPS or CTR_DEV.
#define CTR(level, name) CTR_##level(name)

// Expand the registry
#include "counters.def"

// If level == LVL_NONE and no fields are emitted, keep the struct legal.
#if INSTRUMENTATION_LEVEL == LVL_NONE
  uint64_t __stats_dummy;
#endif

// Cleanup
#undef CTR
#undef CTR_OPS
#undef CTR_DEV
// clang-format on