#include "logger.h"

#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <stdlib.h>

#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
#define LOG_TLS _Thread_local
#elif defined(__GNUC__) || defined(__clang__)
#define LOG_TLS __thread
#else
#define LOG_TLS
#endif

static volatile int g_log_level = LOG_WARN;
static volatile unsigned g_log_mask = LOGC_ALL;
static LOG_TLS int g_tls_tid = -1;

static uint64_t default_now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000ull + (uint64_t)(ts.tv_nsec / 1000000ull);
}

static log_now_ms_fn_t g_now_fn = default_now_ms;

static char level_char(int lvl) {
  if (lvl < 0) {
    lvl = 0;
  }
  if (lvl > 4) {
    lvl = 4;
  }
  static const char L[] = "EWIDT";
  return L[lvl];
}

void log_set_level(int level) {
  if (level < LOG_ERROR) {
    level = LOG_ERROR;
  }
  if (level > LOG_TRACE) {
    level = LOG_TRACE;
  }
  g_log_level = level;
}
int log_get_level(void) {
  return g_log_level;
}

void log_set_categories(unsigned mask) {
  g_log_mask = mask ? mask : LOGC_ALL;
}
unsigned log_get_categories(void) {
  return g_log_mask;
}

void log_set_time_fn(log_now_ms_fn_t fn) {
  g_now_fn = fn ? fn : default_now_ms;
}
uint64_t log_now_ms(void) {
  return g_now_fn ? g_now_fn() : default_now_ms();
}

void log_set_thread_id(int tid) {
  g_tls_tid = tid;
}
int log_get_thread_id(void) {
  return g_tls_tid;
}

int log_should_emit(int level, unsigned cat) {
  return (level <= g_log_level) && ((cat & g_log_mask) != 0);
}

static int parse_level(const char *s, int fallback) {
  if (!s || !*s) {
    return fallback;
  }
  if (!strcasecmp(s, "error")) {
    return LOG_ERROR;
  }
  if (!strcasecmp(s, "warn") || !strcasecmp(s, "warning")) {
    return LOG_WARN;
  }
  if (!strcasecmp(s, "info")) {
    return LOG_INFO;
  }
  if (!strcasecmp(s, "debug")) {
    return LOG_DEBUG;
  }
  if (!strcasecmp(s, "trace")) {
    return LOG_TRACE;
  }
  return fallback;
}

static unsigned parse_mask(const char *s, unsigned fallback) {
  if (!s || !*s) {
    return fallback;
  }
  unsigned m = 0;
  char buf[256];
  size_t n = strnlen(s, sizeof(buf) - 1);
  memcpy(buf, s, n);
  buf[n] = 0;
  for (char *tok = strtok(buf, ","); tok; tok = strtok(NULL, ",")) {
    while (isspace((unsigned char)*tok)) {
      tok++;
    }
    if (!strcasecmp(tok, "all")) {
      m |= LOGC_ALL;
    } else if (!strcasecmp(tok, "core")) {
      m |= LOGC_CORE;
    } else if (!strcasecmp(tok, "accept")) {
      m |= LOGC_ACCEPT;
    } else if (!strcasecmp(tok, "io")) {
      m |= LOGC_IO;
    } else if (!strcasecmp(tok, "http")) {
      m |= LOGC_HTTP;
    } else if (!strcasecmp(tok, "buf")) {
      m |= LOGC_BUF;
    } else if (!strcasecmp(tok, "timer")) {
      m |= LOGC_TIMER;
    } else if (!strcasecmp(tok, "poll")) {
      m |= LOGC_POLL;
    }
  }
  return m ? m : fallback;
}

void log_init_from_env(void) {
  const char *lvl = getenv("LOG_LEVEL");
  const char *cats = getenv("LOG_CATS");
  log_set_level(parse_level(lvl, g_log_level));
  log_set_categories(parse_mask(cats, g_log_mask));
}

void log_emit(int level, unsigned cat, const char *fmt, ...) {
  (void)cat;
  char buf[512];

  uint64_t ts = log_now_ms();
  int tid = g_tls_tid;

  int off = snprintf(buf, sizeof(buf), "%" PRIu64 " [%d] %c ", ts, tid, level_char(level));
  if (off < 0) {
    off = 0;
  }
  if (off >= (int)sizeof(buf)) {
    off = (int)sizeof(buf) - 1;
  }

  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(buf + off, sizeof(buf) - (size_t)off, fmt, ap);
  va_end(ap);
  if (n < 0) {
    n = 0;
  }

  int len = off + n;
  if (len > (int)sizeof(buf) - 2) {
    len = (int)sizeof(buf) - 2;
  }
  buf[len++] = '\n';
  buf[len] = '\0';

  ssize_t wr = write(STDERR_FILENO, buf, (size_t)len);
  (void)wr;
}
