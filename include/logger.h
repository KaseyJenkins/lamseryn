#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(__GNUC__) || defined(__clang__)
#define LOG_ATTR_PRINTF(fmtarg, firstvararg) __attribute__((format(printf, fmtarg, firstvararg)))
#else
#define LOG_ATTR_PRINTF(fmtarg, firstvararg)
#endif

enum log_level {
  LOG_ERROR = 0,
  LOG_WARN = 1,
  LOG_INFO = 2,
  LOG_DEBUG = 3,
  LOG_TRACE = 4
};

#ifndef LOG_COMPILE_LEVEL
#define LOG_COMPILE_LEVEL LOG_WARN
#endif

enum {
  LOGC_CORE = 1u << 0,
  LOGC_ACCEPT = 1u << 1,
  LOGC_IO = 1u << 2,
  LOGC_HTTP = 1u << 3,
  LOGC_BUF = 1u << 4,
  LOGC_TIMER = 1u << 5,
  LOGC_POLL = 1u << 6,
  LOGC_ALL = 0xFFFFFFFFu
};

void log_init_from_env(void);
void log_set_level(int level);
int log_get_level(void);
void log_set_categories(unsigned mask);
unsigned log_get_categories(void);

typedef uint64_t (*log_now_ms_fn_t)(void);
void log_set_time_fn(log_now_ms_fn_t fn);
uint64_t log_now_ms(void);

void log_set_thread_id(int tid);
int log_get_thread_id(void);

int log_should_emit(int level, unsigned cat);

void log_emit(int level, unsigned cat, const char *fmt, ...) LOG_ATTR_PRINTF(3, 4);

// Keep macro helpers compact/hand-aligned for readability.
// clang-format off

#define LOG_ENABLED(lvl,cat) (((lvl) <= LOG_COMPILE_LEVEL) && log_should_emit((lvl),(cat)))

#define LOG_DO(lvl,cat,fmt,...) \
  do { if (LOG_ENABLED((lvl),(cat))) log_emit((lvl),(cat),(fmt), ##__VA_ARGS__); } while (0)

#define LOGE(cat,fmt,...) LOG_DO(LOG_ERROR,(cat),(fmt),##__VA_ARGS__)
#define LOGW(cat,fmt,...) LOG_DO(LOG_WARN, (cat),(fmt),##__VA_ARGS__)
#define LOGI(cat,fmt,...) LOG_DO(LOG_INFO, (cat),(fmt),##__VA_ARGS__)
#define LOGD(cat,fmt,...) LOG_DO(LOG_DEBUG,(cat),(fmt),##__VA_ARGS__)
#define LOGT(cat,fmt,...) LOG_DO(LOG_TRACE,(cat),(fmt),##__VA_ARGS__)

#define _LOG_CONCAT2(a,b) a##b
#define _LOG_CONCAT(a,b) _LOG_CONCAT2(a,b)

#define LOGD_RL(cat,interval_ms,fmt,...) \
  do { static uint64_t _LOG_CONCAT(_last_, __LINE__); \
       uint64_t _now = log_now_ms(); \
       if (_now - _LOG_CONCAT(_last_, __LINE__) >= (uint64_t)(interval_ms)) { \
         _LOG_CONCAT(_last_, __LINE__) = _now; \
         LOGD((cat),(fmt),##__VA_ARGS__); \
       } } while (0)

#define LOGI_RL(cat,interval_ms,fmt,...) \
  do { static uint64_t _LOG_CONCAT(_lasti_, __LINE__); \
       uint64_t _now = log_now_ms(); \
       if (_now - _LOG_CONCAT(_lasti_, __LINE__) >= (uint64_t)(interval_ms)) { \
         _LOG_CONCAT(_lasti_, __LINE__) = _now; \
         LOGI((cat),(fmt),##__VA_ARGS__); \
       } } while (0)

#define LOGW_RL(cat,interval_ms,fmt,...) \
  do { static uint64_t _LOG_CONCAT(_lastw_, __LINE__); \
       uint64_t _now = log_now_ms(); \
       if (_now - _LOG_CONCAT(_lastw_, __LINE__) >= (uint64_t)(interval_ms)) { \
         _LOG_CONCAT(_lastw_, __LINE__) = _now; \
         LOGW((cat),(fmt),##__VA_ARGS__); \
       } } while (0)

#define LOGD_EVERY_N(cat,N,fmt,...) \
  do { static unsigned _LOG_CONCAT(_n_, __LINE__); \
       if (((_LOG_CONCAT(_n_, __LINE__)++) % (unsigned)(N)) == 0) { \
         LOGD((cat),(fmt),##__VA_ARGS__); \
       } } while (0)
// clang-format on

#ifdef __cplusplus
}
#endif