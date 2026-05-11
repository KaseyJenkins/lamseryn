#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>

static inline uint64_t time_now_ms_monotonic(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000ull + (uint64_t)(ts.tv_nsec / 1000000ull);
}

static inline uint64_t time_now_us_monotonic(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)(ts.tv_nsec / 1000ull);
}

// Produces an IMF-fixdate timestamp, e.g. "Sun, 06 Nov 1994 08:49:37 GMT".
// Uses fixed English day/month names so formatting is locale-independent.
// Returns bytes written (excluding NUL), or 0 on failure.
static inline size_t time_format_http_date(char *buf, size_t cap, time_t when) {
  if (!buf || cap == 0) {
    return 0;
  }

  static const char *const days[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
  static const char *const months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                       "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

  struct tm tm;
  if (!gmtime_r(&when, &tm)) {
    return 0;
  }
  if (tm.tm_wday < 0 || tm.tm_wday > 6 || tm.tm_mon < 0 || tm.tm_mon > 11) {
    return 0;
  }

  int n = snprintf(buf,
                   cap,
                   "%s, %02d %s %04d %02d:%02d:%02d GMT",
                   days[tm.tm_wday],
                   tm.tm_mday,
                   months[tm.tm_mon],
                   1900 + tm.tm_year,
                   tm.tm_hour,
                   tm.tm_min,
                   tm.tm_sec);
  if (n <= 0 || (size_t)n >= cap) {
    return 0;
  }
  return (size_t)n;
}
