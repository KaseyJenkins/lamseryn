#include "include/access_log.h"
#include "include/conn.h"
#include "include/worker_ctx.h"
#include "include/logger.h"
#include "include/time_utils.h"

#include <inttypes.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <strings.h>
#include <sys/eventfd.h>
#include <sys/uio.h>
#include <string.h>
#include <unistd.h>

#ifndef ACCESS_LOG_ENABLE_TEST_HOOKS
#define ACCESS_LOG_ENABLE_TEST_HOOKS 0
#endif

static struct access_log_cfg g_access_log_cfg;
static int g_access_log_fd = -1;
static THREAD_LOCAL uint64_t g_access_log_sample_seq = 0;
static volatile uint64_t g_access_log_emitted = 0;
static volatile uint64_t g_access_log_dropped = 0;
static volatile uint64_t g_access_log_write_err = 0;
static volatile uint64_t g_access_log_direct_flush_count = 0;
static volatile uint64_t g_access_log_direct_flush_lines = 0;
static volatile uint64_t g_access_log_direct_flush_bytes = 0;
static volatile uint64_t g_access_log_direct_flush_reason_ring_full = 0;
static volatile uint64_t g_access_log_direct_flush_reason_age = 0;
static volatile uint64_t g_access_log_direct_flush_reason_non_ka = 0;
static volatile uint64_t g_access_log_direct_flush_reason_shutdown = 0;
static volatile uint64_t g_access_log_direct_flush_reason_threshold = 0;
static volatile uint64_t g_access_log_reopen_attempt = 0;
static volatile uint64_t g_access_log_reopen_success = 0;
static volatile uint64_t g_access_log_reopen_fail = 0;
static int g_access_log_file_sink = 0;
#if ACCESS_LOG_ENABLE_TEST_HOOKS
static int g_access_log_test_force_format_fail = 0;
#endif

#define ACCESS_LOG_LINE_MAX 1024u
#define ACCESS_LOG_TARGET_MAX_DEFAULT 256u
#define ACCESS_LOG_MAX_SHARDS 128u
#define ACCESS_LOG_SHARD_CAP 1024u
#define ACCESS_LOG_BATCH_MAX_LINES 64u
#define ACCESS_LOG_BATCH_MAX_BYTES (64u * 1024u)
#define ACCESS_LOG_SW_RING_CAP 256u
#define ACCESS_LOG_SW_FLUSH_LINES 64u
#define ACCESS_LOG_SW_FLUSH_BYTES (32u * 1024u)
#define ACCESS_LOG_SW_FLUSH_MAX_AGE_MS 200u

struct access_log_qentry {
  uint64_t ts_ms;
  unsigned worker;
  char method[8];
  char target[ACCESS_LOG_TARGET_MAX_DEFAULT];
  unsigned target_len;
  char remote_ip[INET6_ADDRSTRLEN];
  uint16_t remote_port;
  unsigned status;
  uint64_t bytes;
  unsigned dur_ms;
  unsigned char keepalive;
  unsigned char tls;
};

struct access_log_queue {
  struct access_log_qentry *entries;
  unsigned cap;
  unsigned prod_head;
  unsigned cons_tail;
};

struct access_log_runtime_q {
  struct access_log_queue *shards;
  unsigned shard_count;
  int stop;
  int started;
  int wake_fd;
  pthread_t thread;
};

static struct access_log_runtime_q g_access_log_q;

enum access_log_direct_flush_reason {
  AL_FLUSH_REASON_THRESHOLD = 1,
  AL_FLUSH_REASON_RING_FULL = 2,
  AL_FLUSH_REASON_AGE = 3,
  AL_FLUSH_REASON_NON_KA = 4,
  AL_FLUSH_REASON_SHUTDOWN = 5,
};

struct access_log_sw_slot {
  size_t len;
  char line[ACCESS_LOG_LINE_MAX];
};

struct access_log_direct_state {
  struct access_log_sw_slot *slots;
  unsigned cap;
  unsigned head;
  unsigned tail;
  unsigned count;
  size_t pending_bytes;
  uint64_t first_pending_ts_ms;
};

static int g_access_log_direct_mode = 0;
static struct access_log_direct_state *g_access_log_direct_states = NULL;
static unsigned g_access_log_direct_state_count = 0;

static void access_log_direct_count_flush_reason(enum access_log_direct_flush_reason reason) {
  switch (reason) {
  case AL_FLUSH_REASON_RING_FULL:
    (void)__sync_add_and_fetch(&g_access_log_direct_flush_reason_ring_full, 1ull);
    break;
  case AL_FLUSH_REASON_AGE:
    (void)__sync_add_and_fetch(&g_access_log_direct_flush_reason_age, 1ull);
    break;
  case AL_FLUSH_REASON_NON_KA:
    (void)__sync_add_and_fetch(&g_access_log_direct_flush_reason_non_ka, 1ull);
    break;
  case AL_FLUSH_REASON_SHUTDOWN:
    (void)__sync_add_and_fetch(&g_access_log_direct_flush_reason_shutdown, 1ull);
    break;
  case AL_FLUSH_REASON_THRESHOLD:
  default:
    (void)__sync_add_and_fetch(&g_access_log_direct_flush_reason_threshold, 1ull);
    break;
  }
}

static int access_log_env_enabled(const char *name) {
  const char *v = getenv(name);
  if (!v || !v[0]) {
    return 0;
  }
  if (!strcasecmp(v, "1") || !strcasecmp(v, "true") || !strcasecmp(v, "yes")
      || !strcasecmp(v, "on")) {
    return 1;
  }
  return 0;
}

static int access_log_write_all(const char *buf, size_t len, int *write_err) {
  size_t off = 0;
  while (off < len) {
    ssize_t wr = write(g_access_log_fd, buf + off, len - off);
    if (wr > 0) {
      off += (size_t)wr;
      continue;
    }
    if (wr < 0 && errno == EINTR) {
      continue;
    }
    if (write_err) {
      *write_err = (wr < 0) ? errno : EIO;
    }
    return -1;
  }
  return 0;
}

static int access_log_writev_all(const struct iovec *iov, int iovcnt, int *write_err) {
  if (!iov || iovcnt <= 0) {
    return 0;
  }

  struct iovec local_iov[ACCESS_LOG_SW_FLUSH_LINES];
  if (iovcnt > (int)ACCESS_LOG_SW_FLUSH_LINES) {
    iovcnt = (int)ACCESS_LOG_SW_FLUSH_LINES;
  }
  memcpy(local_iov, iov, (size_t)iovcnt * sizeof(local_iov[0]));

  struct iovec *cur = local_iov;
  int cur_cnt = iovcnt;

  while (cur_cnt > 0) {
    ssize_t wr = writev(g_access_log_fd, cur, cur_cnt);
    if (wr > 0) {
      size_t consumed = (size_t)wr;
      int idx = 0;
      while (idx < cur_cnt && consumed >= cur[idx].iov_len) {
        consumed -= cur[idx].iov_len;
        idx++;
      }
      cur += idx;
      cur_cnt -= idx;
      if (cur_cnt == 0) {
        return 0;
      }
      if (consumed > 0) {
        cur[0].iov_base = (char *)cur[0].iov_base + consumed;
        cur[0].iov_len -= consumed;
      }
      continue;
    }
    if (wr < 0 && errno == EINTR) {
      continue;
    }
    if (write_err) {
      *write_err = (wr < 0) ? errno : EIO;
    }
    return -1;
  }

  return 0;
}

static size_t access_log_format_qentry_line(char *out,
                                            size_t out_cap,
                                            const struct access_log_qentry *item) {
  if (!out || out_cap == 0 || !item) {
    return 0;
  }

#if ACCESS_LOG_ENABLE_TEST_HOOKS
  if (g_access_log_test_force_format_fail) {
    return 0;
  }
#endif

  struct access_log_event ev;
  memset(&ev, 0, sizeof(ev));
  ev.ts_ms = item->ts_ms;
  ev.worker = item->worker;
  ev.method = (item->method[0] != '\0') ? item->method : "-";
  ev.target = item->target;
  ev.target_len = item->target_len;
  ev.remote_ip = (item->remote_ip[0] != '\0') ? item->remote_ip : NULL;
  ev.remote_port = (unsigned)item->remote_port;
  ev.status = item->status;
  ev.bytes = item->bytes;
  ev.dur_ms = item->dur_ms;
  ev.keepalive = item->keepalive ? 1 : 0;
  ev.tls = item->tls ? 1 : 0;

  return access_log_format_text_line(out, out_cap, &ev, ACCESS_LOG_TARGET_MAX_DEFAULT);
}

static void access_log_direct_flush_state(struct access_log_direct_state *st,
                                          enum access_log_direct_flush_reason reason) {
  if (!g_access_log_direct_mode || !st || !st->slots || st->count == 0) {
    return;
  }

  uint64_t flush_lines = (uint64_t)st->count;
  uint64_t flush_bytes = (uint64_t)st->pending_bytes;
  (void)__sync_add_and_fetch(&g_access_log_direct_flush_count, 1ull);
  (void)__sync_add_and_fetch(&g_access_log_direct_flush_lines, flush_lines);
  (void)__sync_add_and_fetch(&g_access_log_direct_flush_bytes, flush_bytes);
  access_log_direct_count_flush_reason(reason);

  while (st->count > 0) {
    struct iovec vec[ACCESS_LOG_SW_FLUSH_LINES];
    unsigned nvec = 0;
    unsigned idx = st->tail;

    while (nvec < ACCESS_LOG_SW_FLUSH_LINES && nvec < st->count) {
      vec[nvec].iov_base = st->slots[idx].line;
      vec[nvec].iov_len = st->slots[idx].len;
      nvec++;
      idx = (idx + 1u) % st->cap;
    }

    if (nvec == 0) {
      break;
    }

    int write_err = 0;
    if (access_log_writev_all(vec, (int)nvec, &write_err) == 0) {
      (void)__sync_add_and_fetch(&g_access_log_emitted, (uint64_t)nvec);
    } else {
      (void)__sync_add_and_fetch(&g_access_log_dropped, (uint64_t)nvec);
      if (write_err != 0) {
        (void)__sync_add_and_fetch(&g_access_log_write_err, (uint64_t)nvec);
        LOGW_RL(LOGC_CORE,
                1000,
                "access log write failed: fd=%d err=%d(%s) lines=%u",
                g_access_log_fd,
                write_err,
                strerror(write_err),
                nvec);
      }
    }

    st->tail = (st->tail + nvec) % st->cap;
    st->count -= nvec;
  }

  st->pending_bytes = 0;
  st->first_pending_ts_ms = 0;
}

static void access_log_direct_flush_all(void) {
  if (!g_access_log_direct_mode || !g_access_log_direct_states
      || g_access_log_direct_state_count == 0) {
    return;
  }

  for (unsigned i = 0; i < g_access_log_direct_state_count; ++i) {
    access_log_direct_flush_state(&g_access_log_direct_states[i], AL_FLUSH_REASON_SHUTDOWN);
  }
}

static void access_log_direct_maybe_flush_shard(unsigned shard_id, uint64_t now_ms) {
  if (!g_access_log_direct_mode || !g_access_log_direct_states
      || g_access_log_direct_state_count == 0) {
    return;
  }

  if (shard_id >= g_access_log_direct_state_count) {
    shard_id = 0;
  }

  struct access_log_direct_state *st = &g_access_log_direct_states[shard_id];
  if (!st->slots || st->count == 0 || st->first_pending_ts_ms == 0) {
    return;
  }

  if (now_ms >= st->first_pending_ts_ms
      && (now_ms - st->first_pending_ts_ms) >= ACCESS_LOG_SW_FLUSH_MAX_AGE_MS) {
    access_log_direct_flush_state(st, AL_FLUSH_REASON_AGE);
  }
}

static int access_log_direct_append(unsigned shard_id, const struct access_log_qentry *ev) {
  if (!g_access_log_direct_mode || !g_access_log_direct_states
      || g_access_log_direct_state_count == 0 || !ev) {
    return -1;
  }

  if (shard_id >= g_access_log_direct_state_count) {
    shard_id = 0;
  }

  struct access_log_direct_state *st = &g_access_log_direct_states[shard_id];
  if (!st->slots || st->cap == 0) {
    return -1;
  }

  if (st->count >= st->cap) {
    access_log_direct_flush_state(st, AL_FLUSH_REASON_RING_FULL);
  }

  if (st->count >= st->cap) {
    (void)__sync_add_and_fetch(&g_access_log_dropped, 1ull);
    return -1;
  }

  struct access_log_sw_slot *slot = &st->slots[st->head];
  size_t line_len = access_log_format_qentry_line(slot->line, sizeof(slot->line), ev);
  if (line_len == 0 || line_len > ACCESS_LOG_LINE_MAX) {
    (void)__sync_add_and_fetch(&g_access_log_dropped, 1ull);
    LOGW_RL(LOGC_CORE,
            1000,
            "access log format failed: dropping direct entry shard=%u",
            shard_id);
    return -1;
  }

  slot->len = line_len;
  st->head = (st->head + 1u) % st->cap;
  st->count++;
  st->pending_bytes += line_len;
  if (st->first_pending_ts_ms == 0) {
    st->first_pending_ts_ms = ev->ts_ms;
  }

  int age_flush = 0;
  if (st->first_pending_ts_ms > 0 && ev->ts_ms >= st->first_pending_ts_ms
      && (ev->ts_ms - st->first_pending_ts_ms) >= ACCESS_LOG_SW_FLUSH_MAX_AGE_MS) {
    age_flush = 1;
  }

  if (st->count >= ACCESS_LOG_SW_FLUSH_LINES
      || st->pending_bytes >= ACCESS_LOG_SW_FLUSH_BYTES
      || age_flush) {
    enum access_log_direct_flush_reason reason = AL_FLUSH_REASON_THRESHOLD;
    if (age_flush) {
      reason = AL_FLUSH_REASON_AGE;
    }
    access_log_direct_flush_state(st, reason);
  }

  return 0;
}

static int access_log_q_all_empty(void) {
  if (!g_access_log_q.shards || g_access_log_q.shard_count == 0) {
    return 1;
  }

  for (unsigned i = 0; i < g_access_log_q.shard_count; ++i) {
    struct access_log_queue *q = &g_access_log_q.shards[i];
    unsigned head = __atomic_load_n(&q->prod_head, __ATOMIC_ACQUIRE);
    unsigned tail = __atomic_load_n(&q->cons_tail, __ATOMIC_ACQUIRE);
    if (head != tail) {
      return 0;
    }
  }
  return 1;
}

static void access_log_notify_writer(void) {
  if (g_access_log_q.wake_fd < 0) {
    return;
  }

  uint64_t one = 1;
  ssize_t wr = write(g_access_log_q.wake_fd, &one, sizeof(one));
  if (wr == (ssize_t)sizeof(one)) {
    return;
  }
  if (wr < 0 && (errno == EAGAIN || errno == EINTR)) {
    return;
  }
}

static void *access_log_writer_thread(void *arg) {
  (void)arg;

  char batch_buf[ACCESS_LOG_BATCH_MAX_BYTES];

  for (;;) {
    int did_work = 0;

    for (unsigned i = 0; i < g_access_log_q.shard_count; ++i) {
      struct access_log_queue *q = &g_access_log_q.shards[i];
      unsigned tail = __atomic_load_n(&q->cons_tail, __ATOMIC_RELAXED);
      unsigned head = __atomic_load_n(&q->prod_head, __ATOMIC_ACQUIRE);

      while (tail != head) {
        size_t batch_len = 0;
        unsigned batch_lines = 0;
        unsigned batch_tail = tail;

        while (batch_tail != head && batch_lines < ACCESS_LOG_BATCH_MAX_LINES) {
          struct access_log_qentry *item = &q->entries[batch_tail];
          char line_buf[ACCESS_LOG_LINE_MAX];
          size_t line_len = access_log_format_qentry_line(line_buf, sizeof(line_buf), item);
          if (line_len == 0 || line_len > ACCESS_LOG_LINE_MAX) {
            (void)__sync_add_and_fetch(&g_access_log_dropped, 1ull);
            LOGW_RL(LOGC_CORE,
                    1000,
                    "access log format failed: dropping queued entry shard=%u",
                    i);
            batch_tail = (batch_tail + 1u) % q->cap;
            continue;
          }
          if (batch_lines > 0 && (batch_len + line_len) > ACCESS_LOG_BATCH_MAX_BYTES) {
            break;
          }
          memcpy(batch_buf + batch_len, line_buf, line_len);
          batch_len += line_len;
          batch_lines++;
          batch_tail = (batch_tail + 1u) % q->cap;
        }

        if (batch_lines == 0) {
          // All scanned entries were invalid and already accounted as dropped.
          // Advance to batch_tail to avoid rescanning and overcounting.
          tail = batch_tail;
          __atomic_store_n(&q->cons_tail, tail, __ATOMIC_RELEASE);
          head = __atomic_load_n(&q->prod_head, __ATOMIC_ACQUIRE);
          continue;
        }

        tail = batch_tail;
        __atomic_store_n(&q->cons_tail, tail, __ATOMIC_RELEASE);

        int write_err = 0;
        if (access_log_write_all(batch_buf, batch_len, &write_err) == 0) {
          (void)__sync_add_and_fetch(&g_access_log_emitted,
                                     (uint64_t)batch_lines);
        } else {
          (void)__sync_add_and_fetch(&g_access_log_dropped,
                                     (uint64_t)batch_lines);
          if (write_err != 0) {
            (void)__sync_add_and_fetch(&g_access_log_write_err,
                                       (uint64_t)batch_lines);
            LOGW_RL(LOGC_CORE,
                    1000,
                    "access log write failed: fd=%d err=%d(%s) lines=%u total=%zu",
                    g_access_log_fd,
                    write_err,
                    strerror(write_err),
                    batch_lines,
                    batch_len);
          }
        }
        did_work = 1;
        head = __atomic_load_n(&q->prod_head, __ATOMIC_ACQUIRE);
      }
    }

    if (did_work) {
      continue;
    }

    if (__atomic_load_n(&g_access_log_q.stop, __ATOMIC_ACQUIRE)
        && access_log_q_all_empty()) {
      break;
    }

    // Avoid sleeping on wakefd while any shard already has pending entries.
    if (!access_log_q_all_empty()) {
      continue;
    }

    uint64_t wake_count = 0;
    ssize_t rd = read(g_access_log_q.wake_fd, &wake_count, sizeof(wake_count));
    if (rd == (ssize_t)sizeof(wake_count)) {
      continue;
    }
    if (rd < 0 && errno == EINTR) {
      continue;
    }
    if (rd < 0 && errno == EAGAIN) {
      struct pollfd pfd;
      memset(&pfd, 0, sizeof(pfd));
      pfd.fd = g_access_log_q.wake_fd;
      pfd.events = POLLIN;
      int pr = poll(&pfd, 1, -1);
      if (pr < 0 && errno == EINTR) {
        continue;
      }
      continue;
    }
  }

  return NULL;
}

static int access_log_enqueue(unsigned shard_id, const struct access_log_qentry *ev) {
  if (!ev || !g_access_log_q.shards || g_access_log_q.shard_count == 0
      || ev->target_len > ACCESS_LOG_TARGET_MAX_DEFAULT) {
    return -1;
  }

  if (shard_id >= g_access_log_q.shard_count) {
    shard_id = 0;
  }

  struct access_log_queue *q = &g_access_log_q.shards[shard_id];
  unsigned head = __atomic_load_n(&q->prod_head, __ATOMIC_RELAXED);
  unsigned tail = __atomic_load_n(&q->cons_tail, __ATOMIC_ACQUIRE);
  unsigned next = (head + 1u) % q->cap;
  if (next == tail) {
    // If producers observe full queue, nudge writer in case of a missed wake.
    access_log_notify_writer();
    return -1;
  }

  struct access_log_qentry *slot = &q->entries[head];
  *slot = *ev;
  __atomic_store_n(&q->prod_head, next, __ATOMIC_RELEASE);
  if (head == tail) {
    access_log_notify_writer();
  }
  return 0;
}

static void access_log_queue_init_reset(void) {
  memset(&g_access_log_q, 0, sizeof(g_access_log_q));
  g_access_log_q.wake_fd = -1;
}

static void access_log_queue_free(void) {
  if (g_access_log_q.wake_fd >= 0) {
    close(g_access_log_q.wake_fd);
    g_access_log_q.wake_fd = -1;
  }
  if (g_access_log_q.shards) {
    for (unsigned i = 0; i < g_access_log_q.shard_count; ++i) {
      free(g_access_log_q.shards[i].entries);
      g_access_log_q.shards[i].entries = NULL;
    }
  }
  if (g_access_log_direct_states) {
    for (unsigned i = 0; i < g_access_log_direct_state_count; ++i) {
      free(g_access_log_direct_states[i].slots);
      g_access_log_direct_states[i].slots = NULL;
    }
  }
  free(g_access_log_q.shards);
  free(g_access_log_direct_states);
  access_log_queue_init_reset();
}

void access_log_cfg_from_globals(struct access_log_cfg *out, const struct globals_cfg *g) {
  if (!out) {
    return;
  }

  memset(out, 0, sizeof(*out));
  out->sample = 1u;
  out->min_status = 100u;
  snprintf(out->format, sizeof(out->format), "%s", "text");

  if (!g) {
    return;
  }

  if (g->present & GF_ACCESS_LOG_ENABLED) {
    out->enabled = g->access_log_enabled ? 1 : 0;
  }
  if (g->present & GF_ACCESS_LOG_PATH) {
    snprintf(out->path, sizeof(out->path), "%s", g->access_log_path);
  }
  if (g->present & GF_ACCESS_LOG_FORMAT) {
    snprintf(out->format, sizeof(out->format), "%s", g->access_log_format);
  }
  if (g->present & GF_ACCESS_LOG_SAMPLE) {
    out->sample = g->access_log_sample;
  }
  if (g->present & GF_ACCESS_LOG_MIN_STATUS) {
    out->min_status = g->access_log_min_status;
  }
}

size_t access_log_sanitize_target(char *out,
                                  size_t out_cap,
                                  const char *target,
                                  size_t target_len,
                                  unsigned target_max,
                                  int *truncated) {
  if (!out || out_cap == 0) {
    return 0;
  }

  if (truncated) {
    *truncated = 0;
  }

  out[0] = '\0';
  if (!target || target_len == 0) {
    return 0;
  }

  size_t src_limit = target_len;
  if (target_max > 0 && src_limit > (size_t)target_max) {
    src_limit = (size_t)target_max;
    if (truncated) {
      *truncated = 1;
    }
  }

  size_t w = 0;
  for (size_t i = 0; i < src_limit; ++i) {
    if (w + 1 >= out_cap) {
      if (truncated) {
        *truncated = 1;
      }
      break;
    }
    unsigned char ch = (unsigned char)target[i];
    if (ch == ' ' || ch == '\t') {
      out[w++] = '_';
      continue;
    }
    if (ch == '"' || ch == '\\') {
      out[w++] = '_';
      continue;
    }
    if (!isprint(ch) || ch > 0x7e) {
      out[w++] = '?';
      continue;
    }
    out[w++] = (char)ch;
  }

  out[w] = '\0';
  if (src_limit < target_len && truncated) {
    *truncated = 1;
  }
  return w;
}

size_t access_log_format_text_line(char *out,
                                   size_t out_cap,
                                   const struct access_log_event *ev,
                                   unsigned target_max) {
  if (!out || out_cap == 0 || !ev) {
    return 0;
  }

  char target_buf[512];
  int target_truncated = 0;
  size_t target_n = access_log_sanitize_target(target_buf,
                                               sizeof(target_buf),
                                               ev->target,
                                               ev->target_len,
                                               target_max,
                                               &target_truncated);

  const char *method = (ev->method && ev->method[0]) ? ev->method : "-";
  const char *target = (target_n > 0) ? target_buf : "-";
  const char *ip = (ev->remote_ip && ev->remote_ip[0]) ? ev->remote_ip : "-";
  unsigned port = ev->remote_port;

  int n = snprintf(out,
                   out_cap,
                   "ts_ms=%" PRIu64 " worker=%u ip=%s port=%u method=%s target=%s status=%u bytes=%" PRIu64
                   " dur_ms=%u ka=%d tls=%d trunc=%d\n",
                   ev->ts_ms,
                   ev->worker,
                   ip,
                   port,
                   method,
                   target,
                   ev->status,
                   ev->bytes,
                   ev->dur_ms,
                   ev->keepalive ? 1 : 0,
                   ev->tls ? 1 : 0,
                   target_truncated ? 1 : 0);
  if (n <= 0 || (size_t)n >= out_cap) {
    if (out_cap > 0) {
      out[0] = '\0';
    }
    return 0;
  }
  return (size_t)n;
}

static unsigned access_log_status_from_kind(enum resp_kind kind) {
  switch (kind) {
  case RK_OK_CLOSE:
  case RK_OK_KA:
    return 200u;
  case RK_400:
    return 400u;
  case RK_403:
    return 403u;
  case RK_404:
    return 404u;
  case RK_405:
    return 405u;
  case RK_408:
    return 408u;
  case RK_413:
    return 413u;
  case RK_431:
    return 431u;
  case RK_500:
    return 500u;
  case RK_501:
    return 501u;
  case RK_503:
    return 503u;
  default:
    return 0u;
  }
}

static uint64_t access_log_content_length_hint(const struct conn *c) {
  if (!c || !c->tx.write_buf || c->tx.write_len == 0) {
    return 0u;
  }

  const char *buf = c->tx.write_buf;
  size_t len = c->tx.write_len;
  size_t pos = 0;

  while (pos + 1 < len) {
    size_t line_start = pos;
    while (pos + 1 < len) {
      if (buf[pos] == '\r' && buf[pos + 1] == '\n') {
        break;
      }
      pos++;
    }
    if (pos + 1 >= len) {
      break;
    }

    size_t line_len = pos - line_start;
    if (line_len == 0) {
      break;
    }

    if (line_len > 15 && strncasecmp(buf + line_start, "Content-Length:", 15) == 0) {
      size_t i = line_start + 15;
      while (i < line_start + line_len && (buf[i] == ' ' || buf[i] == '\t')) {
        i++;
      }
      uint64_t v = 0;
      int have_digit = 0;
      while (i < line_start + line_len && buf[i] >= '0' && buf[i] <= '9') {
        have_digit = 1;
        v = (v * 10u) + (uint64_t)(buf[i] - '0');
        i++;
      }
      if (have_digit) {
        return v;
      }
      return 0u;
    }

    pos += 2;
  }

  return 0u;
}

static const char *access_log_method_name(const struct conn *c) {
  if (!c || !c->h1.method_set) {
    return "-";
  }

  switch (c->h1.method) {
  case HTTP_GET:
    return "GET";
  case HTTP_HEAD:
    return "HEAD";
  case HTTP_POST:
    return "POST";
  case HTTP_PUT:
    return "PUT";
  case HTTP_DELETE:
    return "DELETE";
  case HTTP_CONNECT:
    return "CONNECT";
  case HTTP_OPTIONS:
    return "OPTIONS";
  case HTTP_TRACE:
    return "TRACE";
  case HTTP_PATCH:
    return "PATCH";
  default:
    return "-";
  }
}

int access_log_runtime_init(const struct globals_cfg *g) {
  access_log_cfg_from_globals(&g_access_log_cfg, g);
  g_access_log_fd = -1;
  g_access_log_file_sink = 0;
  g_access_log_emitted = 0;
  g_access_log_dropped = 0;
  g_access_log_write_err = 0;
  g_access_log_direct_flush_count = 0;
  g_access_log_direct_flush_lines = 0;
  g_access_log_direct_flush_bytes = 0;
  g_access_log_direct_flush_reason_ring_full = 0;
  g_access_log_direct_flush_reason_age = 0;
  g_access_log_direct_flush_reason_non_ka = 0;
  g_access_log_direct_flush_reason_shutdown = 0;
  g_access_log_direct_flush_reason_threshold = 0;
  g_access_log_reopen_attempt = 0;
  g_access_log_reopen_success = 0;
  g_access_log_reopen_fail = 0;
#if ACCESS_LOG_ENABLE_TEST_HOOKS
  g_access_log_test_force_format_fail = access_log_env_enabled("ACCESS_LOG_TEST_FORCE_FORMAT_FAIL");
#endif
  access_log_queue_init_reset();

  if (!g_access_log_cfg.enabled) {
    LOGI(LOGC_CORE, "access log: disabled");
    return 0;
  }

  if (strcasecmp(g_access_log_cfg.format, "text") != 0) {
    LOGW(LOGC_CORE,
         "access log: unsupported format '%s' (expected text); disabling",
         g_access_log_cfg.format);
    g_access_log_cfg.enabled = 0;
    return 0;
  }

  if (!g_access_log_cfg.path[0] || !strcasecmp(g_access_log_cfg.path, "stderr")) {
    g_access_log_fd = STDERR_FILENO;
    g_access_log_file_sink = 0;
  } else if (!strcasecmp(g_access_log_cfg.path, "stdout")) {
    g_access_log_fd = STDOUT_FILENO;
    g_access_log_file_sink = 0;
  } else {
    int fd = open(g_access_log_cfg.path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    if (fd < 0) {
      LOGE(LOGC_CORE,
           "access log: open failed path=%s err=%d(%s)",
           g_access_log_cfg.path,
           errno,
           strerror(errno));
      return -1;
    }
    g_access_log_fd = fd;
    g_access_log_file_sink = 1;
  }

  LOGI(LOGC_CORE,
       "access log: enabled format=%s path=%s sample=1/%u min_status=%u",
       g_access_log_cfg.format,
       g_access_log_cfg.path[0] ? g_access_log_cfg.path : "stderr",
       g_access_log_cfg.sample,
       g_access_log_cfg.min_status);

  unsigned shard_count = 1u;
  if (g && g->workers > 0) {
    shard_count = g->workers;
  }
  if (shard_count > ACCESS_LOG_MAX_SHARDS) {
    shard_count = ACCESS_LOG_MAX_SHARDS;
  }

  int force_direct_per_worker = access_log_env_enabled("ACCESS_LOG_DIRECT_PER_WORKER");
  int force_async_writer = access_log_env_enabled("ACCESS_LOG_FORCE_ASYNC_WRITER");
  int use_direct_mode = 0;

  g_access_log_q.shard_count = shard_count;
  // Default to direct-per-worker mode; workers=1 is always direct.
  if (g_access_log_q.shard_count == 1u || force_direct_per_worker || !force_async_writer) {
    use_direct_mode = 1;
  }

  if (use_direct_mode) {
    g_access_log_direct_mode = 1;
    g_access_log_direct_state_count = g_access_log_q.shard_count;
    g_access_log_direct_states = calloc(g_access_log_direct_state_count,
                                        sizeof(*g_access_log_direct_states));
    if (!g_access_log_direct_states) {
      LOGE(LOGC_CORE,
           "access log: direct states alloc failed workers=%u",
           g_access_log_direct_state_count);
      if (g_access_log_fd >= 0 && g_access_log_fd != STDOUT_FILENO
          && g_access_log_fd != STDERR_FILENO) {
        close(g_access_log_fd);
      }
      g_access_log_fd = -1;
      return -1;
    }

    for (unsigned i = 0; i < g_access_log_direct_state_count; ++i) {
      struct access_log_direct_state *st = &g_access_log_direct_states[i];
      st->cap = ACCESS_LOG_SW_RING_CAP;
      st->slots = calloc(st->cap, sizeof(*st->slots));
      if (!st->slots) {
        LOGE(LOGC_CORE,
             "access log: direct ring alloc failed worker=%u cap=%u",
             i,
             st->cap);
        access_log_queue_free();
        if (g_access_log_fd >= 0 && g_access_log_fd != STDOUT_FILENO
            && g_access_log_fd != STDERR_FILENO) {
          close(g_access_log_fd);
        }
        g_access_log_fd = -1;
        return -1;
      }
    }

    LOGI(LOGC_CORE,
          "access log: direct mode enabled workers=%u force_direct=%d force_async=%d",
         g_access_log_q.shard_count,
          force_direct_per_worker ? 1 : 0,
          force_async_writer ? 1 : 0);
    return 0;
  }

  LOGI(LOGC_CORE,
       "access log: async writer mode enabled workers=%u (forced)",
       g_access_log_q.shard_count);

  g_access_log_q.shards = calloc(g_access_log_q.shard_count, sizeof(*g_access_log_q.shards));
  if (!g_access_log_q.shards) {
    LOGE(LOGC_CORE,
         "access log: shard alloc failed count=%u",
         g_access_log_q.shard_count);
    if (g_access_log_fd >= 0 && g_access_log_fd != STDOUT_FILENO
        && g_access_log_fd != STDERR_FILENO) {
      close(g_access_log_fd);
    }
    g_access_log_fd = -1;
    return -1;
  }

  for (unsigned i = 0; i < g_access_log_q.shard_count; ++i) {
    g_access_log_q.shards[i].cap = ACCESS_LOG_SHARD_CAP;
    g_access_log_q.shards[i].entries = calloc(g_access_log_q.shards[i].cap,
                                              sizeof(*g_access_log_q.shards[i].entries));
    if (!g_access_log_q.shards[i].entries) {
      LOGE(LOGC_CORE,
           "access log: shard queue alloc failed shard=%u cap=%u",
           i,
           g_access_log_q.shards[i].cap);
      access_log_queue_free();
      if (g_access_log_fd >= 0 && g_access_log_fd != STDOUT_FILENO
          && g_access_log_fd != STDERR_FILENO) {
        close(g_access_log_fd);
      }
      g_access_log_fd = -1;
      return -1;
    }
  }

  g_access_log_q.wake_fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
  if (g_access_log_q.wake_fd < 0) {
    LOGE(LOGC_CORE,
         "access log: wake fd init failed err=%d(%s)",
         errno,
         strerror(errno));
    access_log_queue_free();
    if (g_access_log_fd >= 0 && g_access_log_fd != STDOUT_FILENO
        && g_access_log_fd != STDERR_FILENO) {
      close(g_access_log_fd);
    }
    g_access_log_fd = -1;
    return -1;
  }

  if (pthread_create(&g_access_log_q.thread, NULL, access_log_writer_thread, NULL) != 0) {
    LOGE(LOGC_CORE, "access log: writer thread start failed");
    access_log_queue_free();
    if (g_access_log_fd >= 0 && g_access_log_fd != STDOUT_FILENO
        && g_access_log_fd != STDERR_FILENO) {
      close(g_access_log_fd);
    }
    g_access_log_fd = -1;
    return -1;
  }
  g_access_log_q.started = 1;

  return 0;
}

int access_log_runtime_reopen(void) {
  if (!g_access_log_cfg.enabled || g_access_log_fd < 0) {
    return 0;
  }

  if (!g_access_log_file_sink || !g_access_log_cfg.path[0]) {
    return 0;
  }

  (void)__sync_add_and_fetch(&g_access_log_reopen_attempt, 1ull);

  int new_fd = open(g_access_log_cfg.path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
  if (new_fd < 0) {
    (void)__sync_add_and_fetch(&g_access_log_reopen_fail, 1ull);
    LOGW(LOGC_CORE,
         "access log: reopen open failed path=%s err=%d(%s)",
         g_access_log_cfg.path,
         errno,
         strerror(errno));
    return -1;
  }

  if (dup2(new_fd, g_access_log_fd) < 0) {
    int dup2_err = errno;
    close(new_fd);
    (void)__sync_add_and_fetch(&g_access_log_reopen_fail, 1ull);
    LOGW(LOGC_CORE,
         "access log: reopen dup2 failed target_fd=%d path=%s err=%d(%s)",
         g_access_log_fd,
         g_access_log_cfg.path,
         dup2_err,
         strerror(dup2_err));
    return -1;
  }

  int fd_flags = fcntl(g_access_log_fd, F_GETFD);
  if (fd_flags >= 0 && (fd_flags & FD_CLOEXEC) == 0) {
    if (fcntl(g_access_log_fd, F_SETFD, fd_flags | FD_CLOEXEC) != 0) {
      LOGW(LOGC_CORE,
           "access log: reopen set FD_CLOEXEC failed fd=%d err=%d(%s)",
           g_access_log_fd,
           errno,
           strerror(errno));
    }
  }

  if (close(new_fd) != 0) {
    LOGW(LOGC_CORE,
         "access log: reopen close(new_fd) failed fd=%d err=%d(%s)",
         new_fd,
         errno,
         strerror(errno));
  }

  (void)__sync_add_and_fetch(&g_access_log_reopen_success, 1ull);
  LOGI(LOGC_CORE, "access log: reopened path=%s", g_access_log_cfg.path);
  return 0;
}

void access_log_runtime_shutdown(void) {
  if (g_access_log_direct_mode) {
    access_log_direct_flush_all();
  }

  if (g_access_log_q.started) {
    __atomic_store_n(&g_access_log_q.stop, 1, __ATOMIC_RELEASE);
    access_log_notify_writer();
    pthread_join(g_access_log_q.thread, NULL);
  }
  access_log_queue_free();

  if (g_access_log_cfg.enabled) {
    LOGI(LOGC_CORE,
         "access log counters: emitted=%" PRIu64 " dropped=%" PRIu64 " write_err=%" PRIu64 " "
         "direct_flush_count=%" PRIu64 " direct_flush_lines=%" PRIu64 " direct_flush_bytes=%" PRIu64 " "
         "direct_flush_reason_threshold=%" PRIu64 " direct_flush_reason_ring_full=%" PRIu64 " "
         "direct_flush_reason_age=%" PRIu64 " direct_flush_reason_non_ka=%" PRIu64 " "
         "direct_flush_reason_shutdown=%" PRIu64 " "
         "reopen_attempt=%" PRIu64 " reopen_success=%" PRIu64 " reopen_fail=%" PRIu64,
         __sync_add_and_fetch(&g_access_log_emitted, 0),
         __sync_add_and_fetch(&g_access_log_dropped, 0),
         __sync_add_and_fetch(&g_access_log_write_err, 0),
         __sync_add_and_fetch(&g_access_log_direct_flush_count, 0),
         __sync_add_and_fetch(&g_access_log_direct_flush_lines, 0),
         __sync_add_and_fetch(&g_access_log_direct_flush_bytes, 0),
         __sync_add_and_fetch(&g_access_log_direct_flush_reason_threshold, 0),
         __sync_add_and_fetch(&g_access_log_direct_flush_reason_ring_full, 0),
         __sync_add_and_fetch(&g_access_log_direct_flush_reason_age, 0),
         __sync_add_and_fetch(&g_access_log_direct_flush_reason_non_ka, 0),
         __sync_add_and_fetch(&g_access_log_direct_flush_reason_shutdown, 0),
         __sync_add_and_fetch(&g_access_log_reopen_attempt, 0),
         __sync_add_and_fetch(&g_access_log_reopen_success, 0),
         __sync_add_and_fetch(&g_access_log_reopen_fail, 0));
  }

  if (g_access_log_fd >= 0 && g_access_log_fd != STDOUT_FILENO
      && g_access_log_fd != STDERR_FILENO) {
    close(g_access_log_fd);
  }
  g_access_log_fd = -1;
}

void access_log_emit_from_conn(struct worker_ctx *w, struct conn *c) {
  if (!w || !c || !g_access_log_cfg.enabled || g_access_log_fd < 0) {
    return;
  }

  unsigned status = access_log_status_from_kind(c->tx.resp_kind);
  if (status == 0u || status < g_access_log_cfg.min_status) {
    return;
  }

  unsigned sample = g_access_log_cfg.sample ? g_access_log_cfg.sample : 1u;
  g_access_log_sample_seq++;
  if (sample > 1u && (g_access_log_sample_seq % (uint64_t)sample) != 0u) {
    return;
  }

  const char *method = access_log_method_name(c);

  uint64_t now_ms = w->now_cached_ms ? w->now_cached_ms : time_now_ms_monotonic();
  uint64_t dur_ms = 0;
  if (c->dl.header_start_ms > 0 && now_ms >= c->dl.header_start_ms) {
    dur_ms = now_ms - c->dl.header_start_ms;
  }

  struct access_log_qentry qev;
  memset(&qev, 0, sizeof(qev));
  qev.ts_ms = now_ms;
  qev.worker = (unsigned)w->cfg.thread_id;
  qev.status = status;
  qev.bytes = access_log_content_length_hint(c);
  qev.dur_ms = (dur_ms > 0xffffffffull) ? 0xffffffffu : (unsigned)dur_ms;
  qev.keepalive = c->tx.keepalive ? 1 : 0;
  qev.tls = c->tls_enabled ? 1 : 0;
  qev.remote_port = c->remote_port;

  if (c->remote_ip[0] != '\0') {
    memcpy(qev.remote_ip, c->remote_ip, sizeof(qev.remote_ip));
  }

  if (!method || !method[0]) {
    method = "-";
  }
  snprintf(qev.method, sizeof(qev.method), "%s", method);

  if (c->h1.target && c->h1.target_len > 0) {
    size_t target_len = (size_t)c->h1.target_len;
    if (target_len > ACCESS_LOG_TARGET_MAX_DEFAULT) {
      target_len = ACCESS_LOG_TARGET_MAX_DEFAULT;
    }
    memcpy(qev.target, c->h1.target, target_len);
    qev.target_len = (unsigned)target_len;
  }

  if (g_access_log_direct_mode) {
    unsigned shard_id = (w->cfg.thread_id >= 0) ? (unsigned)w->cfg.thread_id : 0u;
    (void)access_log_direct_append(shard_id, &qev);
    return;
  }

  unsigned shard_id = (w->cfg.thread_id >= 0) ? (unsigned)w->cfg.thread_id : 0u;
  if (access_log_enqueue(shard_id, &qev) == 0) {
    return;
  }

  (void)__sync_add_and_fetch(&g_access_log_dropped, 1);
  LOGW_RL(LOGC_CORE,
          1000,
          "access log enqueue dropped: shard_count=%u shard_cap=%u",
          g_access_log_q.shard_count,
          ACCESS_LOG_SHARD_CAP);
}

void access_log_runtime_poll(struct worker_ctx *w) {
  if (!w || !g_access_log_direct_mode) {
    return;
  }

  uint64_t now_ms = w->now_cached_ms ? w->now_cached_ms : time_now_ms_monotonic();
  unsigned shard_id = (w->cfg.thread_id >= 0) ? (unsigned)w->cfg.thread_id : 0u;
  access_log_direct_maybe_flush_shard(shard_id, now_ms);
}
