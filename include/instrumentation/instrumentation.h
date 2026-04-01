#pragma once

#ifndef INSTRUMENTATION_LEVEL
#define INSTRUMENTATION_LEVEL 1  // Default to operations-level counters.
#endif

#define LVL_NONE 0
#define LVL_OPS  1
#define LVL_DEV  2

#define HAS_OPS (INSTRUMENTATION_LEVEL >= LVL_OPS)
#define HAS_DEV (INSTRUMENTATION_LEVEL >= LVL_DEV)

_Static_assert(
  INSTRUMENTATION_LEVEL == LVL_NONE ||
  INSTRUMENTATION_LEVEL == LVL_OPS  ||
  INSTRUMENTATION_LEVEL == LVL_DEV,
  "INSTRUMENTATION_LEVEL must be 0, 1, or 2"
);