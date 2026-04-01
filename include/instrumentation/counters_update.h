#pragma once

#include "instrumentation.h"

// Keep macro definitions hand-aligned for readability.
// clang-format off

// All counters live under w->stats.*.
#define CTR_PATH(w, name) ((w)->stats.name)

// Operations counters available at level >= LVL_OPS.
#if INSTRUMENTATION_LEVEL >= LVL_OPS
  #define CTR_INC_OPS(w, name)        do { CTR_PATH(w, name)++; } while (0)
  #define CTR_ADD_OPS(w, name, v)     do { CTR_PATH(w, name) += (v); } while (0)
  #define CTR_ZERO_OPS(w, name)       do { CTR_PATH(w, name) = 0; } while (0)
#else
  #define CTR_INC_OPS(w, name)        do { } while (0)
  #define CTR_ADD_OPS(w, name, v)     do { } while (0)
  #define CTR_ZERO_OPS(w, name)       do { } while (0)
#endif

// Development counters available at level >= LVL_DEV.
#if INSTRUMENTATION_LEVEL >= LVL_DEV
  #define CTR_INC_DEV(w, name)        do { CTR_PATH(w, name)++; } while (0)
  #define CTR_ADD_DEV(w, name, v)     do { CTR_PATH(w, name) += (v); } while (0)
  #define CTR_ZERO_DEV(w, name)       do { CTR_PATH(w, name) = 0; } while (0)
  #define CTR_SET_DEV(w, name, v)     do { CTR_PATH(w, name) = (v); } while (0)
  #define CTR_DEC_DEV_IF(w, name)     do { if (CTR_PATH(w, name)) CTR_PATH(w, name)--; } while (0)
  #define CTR_UPDATE_PEAK_DEV(w, cur, peak) \
                                      do { if (CTR_PATH(w, cur) > CTR_PATH(w, peak)) CTR_PATH(w, peak) = CTR_PATH(w, cur); } while (0)
// Compare a local value against a peak counter field.
#define CTR_UPDATE_PEAK_DEV_VAL(w, cur_val, peak) \
                                      do { if ((cur_val) > CTR_PATH(w, peak)) CTR_PATH(w, peak) = (cur_val); } while (0)
#define DEV_ONLY(code)                do { code; } while (0)
#else
  #define CTR_INC_DEV(w, name)        do { } while (0)
  #define CTR_ADD_DEV(w, name, v)     do { } while (0)
  #define CTR_ZERO_DEV(w, name)       do { } while (0)
  #define CTR_SET_DEV(w, name, v)     do { } while (0)
  #define CTR_DEC_DEV_IF(w, name)     do { } while (0)
  #define CTR_UPDATE_PEAK_DEV(w, cur, peak) do { } while (0)
  #define CTR_UPDATE_PEAK_DEV_VAL(w, cur_val, peak) do { } while (0)
  #define DEV_ONLY(code)              do { } while (0)
#endif
// clang-format on