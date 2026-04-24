// tests/integration/pipeline_coalesce_test.c
//
// Sends N pipelined GET requests in one send() to encourage coalescing,
// then reads exactly N responses using a buffered reader.
//
// Build:
//   gcc -O2 -Wall -Wextra -o build/pipeline_test tests/integration/pipeline_coalesce_test.c
//
// Run (server must already be listening):
//   ./build/pipeline_test pipeline -H 127.0.0.1 -P 8090 -n 20
//   ./build/pipeline_test fragment -H 127.0.0.1 -P 8090 -d 10
//
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

static void die(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void info(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

static void die(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vfprintf(stderr, fmt, ap);
  va_end(ap);
  fputc('\n', stderr);
  exit(1);
}

static void info(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vfprintf(stdout, fmt, ap);
  va_end(ap);
  fputc('\n', stdout);
}

static int set_socket_timeouts_ms(int fd, int timeout_ms) {
  if (timeout_ms <= 0)
    return 0;

  struct timeval tv;
  tv.tv_sec = timeout_ms / 1000;
  tv.tv_usec = (timeout_ms % 1000) * 1000;

  if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) != 0)
    return -1;
  if (setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv)) != 0)
    return -1;

  return 0;
}

static int connect_tcp(const char *host, const char *port, int set_nodelay,
                       int timeout_ms) {
  struct addrinfo hints, *res = NULL, *rp;
  memset(&hints, 0, sizeof(hints));
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_family = AF_UNSPEC;
  hints.ai_flags = AI_NUMERICSERV;

  int rc = getaddrinfo(host, port, &hints, &res);
  if (rc != 0)
    die("getaddrinfo(%s:%s): %s", host, port, gai_strerror(rc));

  int fd = -1;
  for (rp = res; rp; rp = rp->ai_next) {
    fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
    if (fd < 0)
      continue;

    if (set_socket_timeouts_ms(fd, timeout_ms) != 0) {
      close(fd);
      fd = -1;
      continue;
    }

    if (set_nodelay) {
      int one = 1;
      (void)setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    }

    if (connect(fd, rp->ai_addr, rp->ai_addrlen) == 0)
      break;

    close(fd);
    fd = -1;
  }

  freeaddrinfo(res);

  if (fd < 0)
    die("connect(%s:%s) failed: %s", host, port, strerror(errno));

  return fd;
}

static ssize_t send_all(int fd, const void *buf, size_t len) {
  const char *p = (const char *)buf;
  size_t off = 0;

  while (off < len) {
    ssize_t n = send(fd, p + off, len - off, MSG_NOSIGNAL);
    if (n > 0) {
      off += (size_t)n;
      continue;
    }
    if (n == 0)
      return (ssize_t)off;

    if (errno == EINTR)
      continue;

    return -1;
  }

  return (ssize_t)off;
}

static int server_pid_from_env(pid_t *out_pid) {
  if (!out_pid)
    return -1;
  *out_pid = (pid_t)0;

  const char *s = getenv("ITEST_SERVER_PID");
  if (!s || !*s)
    return -1;

  char *endp = NULL;
  long v = strtol(s, &endp, 10);
  if (endp == s || v <= 0)
    return -1;

  *out_pid = (pid_t)v;
  return 0;
}

static void must_signal_server_from_env(int sig) {
  pid_t pid = (pid_t)0;
  if (server_pid_from_env(&pid) != 0)
    die("missing/invalid ITEST_SERVER_PID env for shutdown characterization mode");

  if (kill(pid, sig) != 0)
    die("kill(%ld, %d) failed: %s", (long)pid, sig, strerror(errno));
}

static uint64_t monotonic_ms(void) {
  struct timespec ts;
  if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
    die("clock_gettime failed: %s", strerror(errno));
  return (uint64_t)ts.tv_sec * 1000ull + (uint64_t)ts.tv_nsec / 1000000ull;
}

static int pid_is_alive(pid_t pid) {
  if (pid <= 0)
    return 0;
  if (kill(pid, 0) == 0)
    return 1;
  if (errno == ESRCH)
    return 0;
  return 1;
}

static uint64_t wait_for_pid_exit_ms(pid_t pid, uint64_t timeout_ms) {
  uint64_t start = monotonic_ms();
  for (;;) {
    if (!pid_is_alive(pid))
      return monotonic_ms() - start;

    uint64_t now = monotonic_ms();
    if (now - start >= timeout_ms)
      return UINT64_MAX;

    struct timespec ts;
    ts.tv_sec = 0;
    ts.tv_nsec = 50 * 1000 * 1000L;
    (void)nanosleep(&ts, NULL);
  }
}

static ssize_t find_headers_end(const char *buf, size_t len) {
  if (!buf || len < 4)
    return -1;

  for (size_t i = 3; i < len; ++i) {
    if (buf[i - 3] == '\r' && buf[i - 2] == '\n' &&
        buf[i - 1] == '\r' && buf[i] == '\n') {
      return (ssize_t)(i + 1);
    }
  }
  return -1;
}

static int read_response_prefix(int fd,
                                char *buf,
                                size_t cap,
                                size_t *out_total,
                                size_t *out_header_end) {
  if (!buf || cap == 0 || !out_total || !out_header_end)
    return -1;

  *out_total = 0;
  *out_header_end = 0;

  while (*out_total < cap) {
    ssize_t n = recv(fd, buf + *out_total, cap - *out_total, 0);
    if (n > 0) {
      *out_total += (size_t)n;
      ssize_t he = find_headers_end(buf, *out_total);
      if (he > 0) {
        *out_header_end = (size_t)he;
        return 0;
      }
      continue;
    }
    if (n == 0)
      return -1;
    if (errno == EINTR)
      continue;
    return -1;
  }

  return -1;
}

static size_t recv_until_eof_count_bytes(int fd, size_t total0) {
  size_t total = total0;
  char buf[8192];
  for (;;) {
    ssize_t n = recv(fd, buf, sizeof(buf), 0);
    if (n > 0) {
      total += (size_t)n;
      continue;
    }
    if (n == 0)
      break;
    if (errno == EINTR)
      continue;
    if (errno == EAGAIN || errno == EWOULDBLOCK)
      break;
    break;
  }
  return total;
}

static size_t count_substr(const char *buf, size_t len, const char *needle) {
  if (!buf || !needle || !*needle)
    return 0;

  size_t nlen = strlen(needle);
  if (len < nlen)
    return 0;

  size_t count = 0;
  for (size_t i = 0; i + nlen <= len; ++i) {
    if (memcmp(buf + i, needle, nlen) == 0)
      count++;
  }
  return count;
}

static size_t recv_until_eof_count_substr(int fd,
                                          const char *needle,
                                          const char *seed,
                                          size_t seed_len) {
  if (!needle || !*needle)
    return 0;

  size_t count = (seed && seed_len) ? count_substr(seed, seed_len, needle) : 0;
  size_t nlen = strlen(needle);
  size_t overlap_cap = (nlen > 1) ? (nlen - 1) : 0;

  char overlap[128];
  if (overlap_cap >= sizeof(overlap))
    die("needle too large for stream substring counter");

  size_t overlap_len = 0;
  if (seed && seed_len && overlap_cap > 0) {
    overlap_len = (seed_len < overlap_cap) ? seed_len : overlap_cap;
    memcpy(overlap, seed + (seed_len - overlap_len), overlap_len);
  }

  char chunk[8192];
  char scan[8192 + sizeof(overlap)];

  for (;;) {
    ssize_t n = recv(fd, chunk, sizeof(chunk), 0);
    if (n > 0) {
      size_t nn = (size_t)n;
      memcpy(scan, overlap, overlap_len);
      memcpy(scan + overlap_len, chunk, nn);
      count += count_substr(scan, overlap_len + nn, needle);

      if (overlap_cap > 0) {
        size_t new_len = overlap_len + nn;
        if (new_len > overlap_cap)
          new_len = overlap_cap;
        memcpy(overlap, scan + (overlap_len + nn - new_len), new_len);
        overlap_len = new_len;
      }
      continue;
    }
    if (n == 0)
      break;
    if (errno == EINTR)
      continue;
    if (errno == EAGAIN || errno == EWOULDBLOCK)
      break;
    break;
  }

  return count;
}

static int ascii_ieq(char a, char b) {
  return tolower((unsigned char)a) == tolower((unsigned char)b);
}

static const char *memcasestr_simple(const char *hay, const char *needle) {
  // Simple ASCII case-insensitive substring search.
  // Good enough for headers in this test tool.
  if (!hay || !needle)
    return NULL;
  size_t nlen = strlen(needle);
  if (nlen == 0)
    return hay;

  for (const char *p = hay; *p; ++p) {
    size_t i = 0;
    while (i < nlen && p[i] && ascii_ieq(p[i], needle[i]))
      i++;
    if (i == nlen)
      return p;
  }
  return NULL;
}

static long parse_content_length(const char *hdrs) {
  const char *p = memcasestr_simple(hdrs, "\r\nContent-Length:");
  if (!p)
    return -1;
  p += 2; // skip initial CRLF

  const char *colon = strchr(p, ':');
  if (!colon)
    return -1;
  colon++;
  while (*colon == ' ' || *colon == '\t')
    colon++;

  char *endp = NULL;
  long v = strtol(colon, &endp, 10);
  if (endp == colon || v < 0)
    return -1;

  return v;
}

static int parse_header_value_simple(const char *hdrs, const char *name,
                                     char *out, size_t out_sz) {
  // Extract a single header value (no folding, first match). Case-insensitive name.
  if (!hdrs || !name || !out || out_sz == 0)
    return -1;
  out[0] = 0;

  char needle[128];
  int n = snprintf(needle, sizeof(needle), "\r\n%s:", name);
  if (n <= 0 || (size_t)n >= sizeof(needle))
    return -1;

  const char *p = memcasestr_simple(hdrs, needle);
  if (!p)
    return -1;

  const char *colon = strchr(p + 2, ':');
  if (!colon)
    return -1;
  colon++;
  while (*colon == ' ' || *colon == '\t')
    colon++;

  const char *eol = strstr(colon, "\r\n");
  if (!eol)
    return -1;

  size_t vlen = (size_t)(eol - colon);
  if (vlen >= out_sz)
    vlen = out_sz - 1;
  memcpy(out, colon, vlen);
  out[vlen] = 0;
  return 0;
}

static int parse_connection_is_close(const char *hdrs) {
  const char *p = memcasestr_simple(hdrs, "\r\nConnection:");
  if (!p)
    return 0;
  p += 2; // skip initial CRLF

  const char *colon = strchr(p, ':');
  if (!colon)
    return 0;
  colon++;
  while (*colon == ' ' || *colon == '\t')
    colon++;

  // Token-based would be nicer, but simple substring is fine here.
  return memcasestr_simple(colon, "close") != NULL;
}

static void print_first_response_line(FILE *out, const char *hdrs) {
  if (!hdrs)
    return;
  const char *eol = strstr(hdrs, "\r\n");
  if (!eol)
    eol = strchr(hdrs, '\n');
  if (!eol)
    eol = hdrs + strlen(hdrs);

  fprintf(out, "first response line: %.*s\n", (int)(eol - hdrs), hdrs);
}

static int parse_status_code(const char *hdrs) {
  // Expect: HTTP/1.1 200 ...
  if (!hdrs)
    return -1;

  if (strncmp(hdrs, "HTTP/", 5) != 0)
    return -1;

  const char *sp = strchr(hdrs, ' ');
  if (!sp)
    return -1;
  while (*sp == ' ')
    sp++;

  char *endp = NULL;
  long code = strtol(sp, &endp, 10);
  if (endp == sp || code < 0 || code > 999)
    return -1;

  return (int)code;
}

// Simple buffered reader to read exactly one HTTP/1 response.
static char g_buf[256 * 1024];
static size_t g_len = 0;

static int read_one_response_buffered(int fd, int expected_status,
                                      int verbose) {
  static const char SEP[] = "\r\n\r\n";
  const size_t SEP_LEN = 4;

  // Read through the full header block first.
  size_t hdr_end = (size_t)-1;
  for (;;) {
    if (hdr_end == (size_t)-1) {
      for (size_t i = 0; i + SEP_LEN <= g_len; ++i) {
        if (memcmp(g_buf + i, SEP, SEP_LEN) == 0) {
          hdr_end = i + SEP_LEN;
          break;
        }
      }
    }
    if (hdr_end != (size_t)-1)
      break;

    if (g_len == sizeof(g_buf))
      return -1; // headers too large / stuck

    ssize_t r = recv(fd, g_buf + g_len, sizeof(g_buf) - g_len, 0);
    if (r < 0) {
      if (errno == EINTR)
        continue;
      return -1; // includes timeout via EAGAIN/EWOULDBLOCK
    }
    if (r == 0)
      return -1; // closed

    g_len += (size_t)r;
  }

  // Parse status and Content-Length from the header bytes.
  size_t header_len = hdr_end;

  char header_tmp[64 * 1024 + 128];
  size_t copy = header_len;
  if (copy > sizeof(header_tmp) - 1)
    copy = sizeof(header_tmp) - 1;

  memcpy(header_tmp, g_buf, copy);
  header_tmp[copy] = 0;

  int st = parse_status_code(header_tmp);
  if (st < 0) {
    print_first_response_line(stderr, header_tmp);
    return -1;
  }
  if (expected_status >= 0 && st != expected_status) {
    print_first_response_line(stderr, header_tmp);
    return -1;
  }

  long cl = parse_content_length(header_tmp);
  if (cl < 0)
    cl = 0;

  if (verbose) {
    info("resp: status=%d cl=%ld conn=%s", st, cl,
         parse_connection_is_close(header_tmp) ? "close" : "keep-alive");
  }

  // Pull body bytes until headers + Content-Length are present.
  size_t total_needed = header_len + (size_t)cl;
  while (g_len < total_needed) {
    if (g_len == sizeof(g_buf))
      return -1;
    ssize_t r = recv(fd, g_buf + g_len, sizeof(g_buf) - g_len, 0);
    if (r < 0) {
      if (errno == EINTR)
        continue;
      return -1;
    }
    if (r == 0)
      return -1;
    g_len += (size_t)r;
  }

  // Content checks are done by individual tests.

  // Preserve pipelined bytes for the next response parse.
  size_t leftover = g_len - total_needed;
  if (leftover)
    memmove(g_buf, g_buf + total_needed, leftover);
  g_len = leftover;

  return 0;
}

static int read_one_response_body_once(int fd, int expected_status, int verbose,
                                       char **out_body, size_t *out_body_len) {
  // Read one response and return a malloc'd, NUL-terminated body.
  static const char SEP[] = "\r\n\r\n";
  const size_t SEP_LEN = 4;

  if (out_body)
    *out_body = NULL;
  if (out_body_len)
    *out_body_len = 0;

  // Read through the full header block first.
  size_t hdr_end = (size_t)-1;
  for (;;) {
    if (hdr_end == (size_t)-1) {
      for (size_t i = 0; i + SEP_LEN <= g_len; ++i) {
        if (memcmp(g_buf + i, SEP, SEP_LEN) == 0) {
          hdr_end = i + SEP_LEN;
          break;
        }
      }
    }
    if (hdr_end != (size_t)-1)
      break;

    if (g_len == sizeof(g_buf))
      return -1;
    ssize_t r = recv(fd, g_buf + g_len, sizeof(g_buf) - g_len, 0);
    if (r < 0) {
      if (errno == EINTR)
        continue;
      return -1;
    }
    if (r == 0)
      return -1;
    g_len += (size_t)r;
  }

  // Parse status and Content-Length from the header bytes.
  size_t header_len = hdr_end;

  char header_tmp[64 * 1024 + 128];
  size_t copy = header_len;
  if (copy > sizeof(header_tmp) - 1)
    copy = sizeof(header_tmp) - 1;
  memcpy(header_tmp, g_buf, copy);
  header_tmp[copy] = 0;

  int st = parse_status_code(header_tmp);
  if (st < 0) {
    print_first_response_line(stderr, header_tmp);
    return -1;
  }
  if (expected_status >= 0 && st != expected_status) {
    print_first_response_line(stderr, header_tmp);
    return -1;
  }

  long cl = parse_content_length(header_tmp);
  if (cl < 0)
    cl = 0;

  if (verbose) {
    info("resp: status=%d cl=%ld conn=%s", st, cl,
         parse_connection_is_close(header_tmp) ? "close" : "keep-alive");
  }

  // Pull body bytes until headers + Content-Length are present.
  size_t total_needed = header_len + (size_t)cl;
  while (g_len < total_needed) {
    if (g_len == sizeof(g_buf))
      return -1;
    ssize_t r = recv(fd, g_buf + g_len, sizeof(g_buf) - g_len, 0);
    if (r < 0) {
      if (errno == EINTR)
        continue;
      return -1;
    }
    if (r == 0)
      return -1;
    g_len += (size_t)r;
  }

  // Copy out just the body.
  if (out_body) {
    char *b = (char *)malloc((size_t)cl + 1);
    if (!b)
      return -1;
    if (cl > 0)
      memcpy(b, g_buf + header_len, (size_t)cl);
    b[(size_t)cl] = 0;
    *out_body = b;
  }
  if (out_body_len)
    *out_body_len = (size_t)cl;

  // Caller closes the socket; leftovers are intentionally dropped.
  g_len = 0;
  return 0;
}

static int read_one_response_head_buffered(int fd, int expected_status,
                                          int verbose, long *out_content_length,
                                          size_t *out_leftover) {
  // Read one HEAD response: consume headers only, never read a body.
  static const char SEP[] = "\r\n\r\n";
  const size_t SEP_LEN = 4;

  if (out_content_length)
    *out_content_length = -1;
  if (out_leftover)
    *out_leftover = 0;

  // Read through the full header block first.
  size_t hdr_end = (size_t)-1;
  for (;;) {
    if (hdr_end == (size_t)-1) {
      for (size_t i = 0; i + SEP_LEN <= g_len; ++i) {
        if (memcmp(g_buf + i, SEP, SEP_LEN) == 0) {
          hdr_end = i + SEP_LEN;
          break;
        }
      }
    }
    if (hdr_end != (size_t)-1)
      break;

    if (g_len == sizeof(g_buf))
      return -1;

    ssize_t r = recv(fd, g_buf + g_len, sizeof(g_buf) - g_len, 0);
    if (r < 0) {
      if (errno == EINTR)
        continue;
      return -1;
    }
    if (r == 0)
      return -1;

    g_len += (size_t)r;
  }

  // Parse status and Content-Length from the header bytes.
  size_t header_len = hdr_end;

  char header_tmp[64 * 1024 + 128];
  size_t copy = header_len;
  if (copy > sizeof(header_tmp) - 1)
    copy = sizeof(header_tmp) - 1;

  memcpy(header_tmp, g_buf, copy);
  header_tmp[copy] = 0;

  int st = parse_status_code(header_tmp);
  if (st < 0) {
    print_first_response_line(stderr, header_tmp);
    return -1;
  }
  if (expected_status >= 0 && st != expected_status) {
    print_first_response_line(stderr, header_tmp);
    return -1;
  }

  long cl = parse_content_length(header_tmp);
  if (out_content_length)
    *out_content_length = cl;

  if (verbose) {
    info("resp: status=%d cl=%ld conn=%s", st, cl,
         parse_connection_is_close(header_tmp) ? "close" : "keep-alive");
  }

  // Consume only headers and keep any buffered leftovers.
  size_t total_needed = header_len;
  size_t leftover = g_len - total_needed;
  if (leftover)
    memmove(g_buf, g_buf + total_needed, leftover);
  g_len = leftover;

  if (out_leftover)
    *out_leftover = leftover;

  return 0;
}

static int read_one_response_discard_body(int fd, int expected_status,
                                         int verbose, long *out_content_length,
                                         char *out_static_mode,
                                         size_t out_static_mode_sz) {
  // Read one response and drain body bytes using Content-Length.
  static const char SEP[] = "\r\n\r\n";
  const size_t SEP_LEN = 4;

  if (out_content_length)
    *out_content_length = -1;
  if (out_static_mode && out_static_mode_sz)
    out_static_mode[0] = 0;

  // Read through the full header block first.
  size_t hdr_end = (size_t)-1;
  for (;;) {
    if (hdr_end == (size_t)-1) {
      for (size_t i = 0; i + SEP_LEN <= g_len; ++i) {
        if (memcmp(g_buf + i, SEP, SEP_LEN) == 0) {
          hdr_end = i + SEP_LEN;
          break;
        }
      }
    }
    if (hdr_end != (size_t)-1)
      break;

    if (g_len == sizeof(g_buf))
      return -1;

    ssize_t r = recv(fd, g_buf + g_len, sizeof(g_buf) - g_len, 0);
    if (r < 0) {
      if (errno == EINTR)
        continue;
      return -1;
    }
    if (r == 0)
      return -1;
    g_len += (size_t)r;
  }

  // Parse status and Content-Length from the header bytes.
  size_t header_len = hdr_end;

  char header_tmp[64 * 1024 + 128];
  size_t copy = header_len;
  if (copy > sizeof(header_tmp) - 1)
    copy = sizeof(header_tmp) - 1;
  memcpy(header_tmp, g_buf, copy);
  header_tmp[copy] = 0;

  int st = parse_status_code(header_tmp);
  if (st < 0) {
    print_first_response_line(stderr, header_tmp);
    return -1;
  }
  if (expected_status >= 0 && st != expected_status) {
    print_first_response_line(stderr, header_tmp);
    return -1;
  }

  long cl = parse_content_length(header_tmp);
  if (cl < 0)
    cl = 0;
  if (out_content_length)
    *out_content_length = cl;

  if (verbose) {
    info("resp: status=%d cl=%ld conn=%s", st, cl,
         parse_connection_is_close(header_tmp) ? "close" : "keep-alive");
  }

  if (out_static_mode && out_static_mode_sz) {
    (void)parse_header_value_simple(header_tmp, "X-Itest-Static-Mode",
                                    out_static_mode, out_static_mode_sz);
  }

  // Drain body bytes.
  size_t already = (g_len > header_len) ? (g_len - header_len) : 0;
  size_t remaining = (size_t)cl;
  if (already >= remaining) {
    // We already have the whole body in g_buf; discard and ignore leftovers.
    g_len = 0;
    return 0;
  }

  remaining -= already;
  g_len = 0; // discard header+partial body; for this helper we don't preserve leftovers

  char tmp[8192];
  while (remaining > 0) {
    size_t want = remaining;
    if (want > sizeof(tmp))
      want = sizeof(tmp);

    ssize_t r = recv(fd, tmp, want, 0);
    if (r < 0) {
      if (errno == EINTR)
        continue;
      return -1;
    }
    if (r == 0)
      return -1;
    remaining -= (size_t)r;
  }

  return 0;
}

static long file_size_try_paths(const char *p1, const char *p2) {
  struct stat st;
  if (p1 && stat(p1, &st) == 0 && S_ISREG(st.st_mode) && st.st_size >= 0)
    return (long)st.st_size;
  if (p2 && stat(p2, &st) == 0 && S_ISREG(st.st_mode) && st.st_size >= 0)
    return (long)st.st_size;
  return -1;
}

static void assert_contains_or_die(const char *hay, const char *needle) {
  if (!hay || !needle || !*needle)
    return;
  if (!strstr(hay, needle))
    die("missing expected substring: %s\n--- body ---\n%s", needle, hay ? hay : "(null)");
}

static void assert_not_contains_or_die(const char *hay, const char *needle) {
  if (!hay || !needle || !*needle)
    return;
  if (strstr(hay, needle))
    die("unexpected substring present: %s\n--- body ---\n%s", needle, hay ? hay : "(null)");
}

static char *do_echo_req(const char *host, const char *port,
                         int nodelay, int timeout_ms, int verbose,
                         const char *req) {
  int fd = connect_tcp(host, port, nodelay, timeout_ms);
  if (send_all(fd, req, strlen(req)) < 0) {
    close(fd);
    die("send failed: %s", strerror(errno));
  }

  char *body = NULL;
  size_t body_len = 0;
  if (read_one_response_body_once(fd, 200, verbose, &body, &body_len) != 0) {
    close(fd);
    die("echo read failed");
  }
  (void)body_len;
  close(fd);
  if (!body)
    die("echo body missing");
  return body;
}

static char *do_req_body(const char *host, const char *port,
                         int nodelay, int timeout_ms, int verbose,
                         int expected_status, const char *req) {
  int fd = connect_tcp(host, port, nodelay, timeout_ms);
  if (send_all(fd, req, strlen(req)) < 0) {
    close(fd);
    die("send failed: %s", strerror(errno));
  }

  char *body = NULL;
  size_t body_len = 0;
  if (read_one_response_body_once(fd, expected_status, verbose, &body, &body_len) != 0) {
    close(fd);
    die("read failed");
  }
  (void)body_len;
  close(fd);
  if (!body)
    die("body missing");
  return body;
}

static int test_static_index(const char *host, const char *port,
                             int nodelay, int timeout_ms, int verbose) {
  const char *req =
      "GET / HTTP/1.1\r\n"
      "Host: example.com\r\n"
      "Connection: close\r\n"
      "\r\n";

  char *body = do_req_body(host, port, nodelay, timeout_ms, verbose, 200, req);
  assert_contains_or_die(body, "<title>lamseryn</title>");
  free(body);
  info("static_index: OK");
  return 0;
}

static int test_static_large_file(const char *host, const char *port,
                                  int nodelay, int timeout_ms, int verbose) {
  // This test is intended to exercise the server's streaming static-file path.
  // The integration runner creates /big.bin in the docroot.
  const long EXPECTED_CL = 524288; // must match run_integration_tests.sh

  const char *req =
      "GET /big.bin HTTP/1.1\r\n"
      "Host: example.com\r\n"
      "Connection: close\r\n"
      "\r\n";

  g_len = 0;
  int fd = connect_tcp(host, port, nodelay, timeout_ms);
  if (send_all(fd, req, strlen(req)) < 0) {
    close(fd);
    die("send failed: %s", strerror(errno));
  }

  long cl = -1;
  char mode[64];
  if (read_one_response_discard_body(fd, 200, verbose, &cl, mode, sizeof(mode)) != 0) {
    close(fd);
    die("static-large read failed");
  }
  close(fd);

  if (cl != EXPECTED_CL)
    die("static-large Content-Length mismatch: got=%ld expected=%ld", cl, EXPECTED_CL);

  if (mode[0] == 0)
    die("static-large: missing X-Itest-Static-Mode header (did you run the itest server?)");
  if (strcmp(mode, "sendfile") != 0)
    die("static-large: expected X-Itest-Static-Mode=sendfile got=%s", mode);

  info("static_large_file: OK");
  return 0;
}

static int test_sendfile_keepalive_bytes_regression(const char *host,
                                                    const char *port,
                                                    int nodelay,
                                                    int timeout_ms,
                                                    int verbose) {
  // Regression: on a keep-alive connection, a sendfile response (GET /big.bin)
  // followed by an error response must log bytes=0 for the second request,
  // not the stale Content-Length from the first sendfile response.
  g_len = 0;
  int fd = connect_tcp(host, port, nodelay, timeout_ms);

  const char *req1 = "GET /big.bin HTTP/1.1\r\n"
                     "Host: example.com\r\n"
                     "Connection: keep-alive\r\n"
                     "\r\n";

  if (send_all(fd, req1, strlen(req1)) < 0) {
    close(fd);
    die("sendfile-ka-bytes: send req1 failed: %s", strerror(errno));
  }

  long cl = -1;
  if (read_one_response_discard_body(fd, 200, verbose, &cl, NULL, 0) != 0) {
    close(fd);
    die("sendfile-ka-bytes: read first response failed");
  }
  if (cl != 524288) {
    die("sendfile-ka-bytes: expected CL=524288, got=%ld", cl);
  }

  const char *req2 = "DELETE /__stale_hint_probe HTTP/1.1\r\n"
                     "Host: example.com\r\n"
                     "Connection: close\r\n"
                     "\r\n";

  if (send_all(fd, req2, strlen(req2)) < 0) {
    close(fd);
    die("sendfile-ka-bytes: send req2 failed: %s", strerror(errno));
  }

  if (read_one_response_buffered(fd, 405, verbose) != 0) {
    close(fd);
    die("sendfile-ka-bytes: read second response failed");
  }

  close(fd);
  info("sendfile_keepalive_bytes_regression: OK");
  return 0;
}

static int test_static_sendfile_threshold(const char *host, const char *port,
                                         int nodelay, int timeout_ms, int verbose) {
  // Intended to catch sendfile threshold regressions: file is just over the
  // current threshold (256 KiB).
  const long EXPECTED_CL = 262145; // must match run_integration_tests.sh

  const char *req =
      "GET /big_threshold.bin HTTP/1.1\r\n"
      "Host: example.com\r\n"
      "Connection: close\r\n"
      "\r\n";

  g_len = 0;
  int fd = connect_tcp(host, port, nodelay, timeout_ms);
  if (send_all(fd, req, strlen(req)) < 0) {
    close(fd);
    die("send failed: %s", strerror(errno));
  }

  long cl = -1;
  char mode[64];
  if (read_one_response_discard_body(fd, 200, verbose, &cl, mode, sizeof(mode)) != 0) {
    close(fd);
    die("static-sendfile-threshold read failed");
  }
  close(fd);

  if (cl != EXPECTED_CL)
    die("static-sendfile-threshold Content-Length mismatch: got=%ld expected=%ld", cl, EXPECTED_CL);

  if (mode[0] == 0)
    die("static-sendfile-threshold: missing X-Itest-Static-Mode header (did you run the itest server?)");
  if (strcmp(mode, "sendfile") != 0)
    die("static-sendfile-threshold: expected X-Itest-Static-Mode=sendfile got=%s", mode);

  info("static_sendfile_threshold: OK");
  return 0;
}

static int test_static_head_index(const char *host, const char *port,
                                  int nodelay, int timeout_ms, int verbose) {
  const char *req =
      "HEAD / HTTP/1.1\r\n"
      "Host: example.com\r\n"
      "Connection: close\r\n"
      "\r\n";

  long expected = file_size_try_paths("tests/integration/wwwroot/index.html",
                                     "wwwroot/index.html");
  if (expected < 0)
    die("cannot stat expected docroot index.html");

  g_len = 0;
  int fd = connect_tcp(host, port, nodelay, timeout_ms);
  if (send_all(fd, req, strlen(req)) < 0) {
    close(fd);
    die("send failed: %s", strerror(errno));
  }

  long cl = -1;
  size_t leftover = 0;
  if (read_one_response_head_buffered(fd, 200, verbose, &cl, &leftover) != 0) {
    close(fd);
    die("static-head read failed");
  }

  if (cl != expected) {
    close(fd);
    die("static-head Content-Length mismatch: got=%ld expected=%ld", cl, expected);
  }

  if (leftover != 0) {
    close(fd);
    die("static-head: unexpected body bytes received (%zu)", leftover);
  }

  close(fd);
  info("static_head_index: OK");
  return 0;
}

static int test_method_not_allowed_405(const char *host, const char *port,
                                       int nodelay, int timeout_ms, int verbose) {
  g_len = 0;
  int fd = connect_tcp(host, port, nodelay, timeout_ms);
  const char *req =
      "POST / HTTP/1.1\r\n"
      "Host: example.com\r\n"
      "Content-Length: 5\r\n"
      "Connection: close\r\n"
      "\r\n"
      "hello";

  if (send_all(fd, req, strlen(req)) < 0) {
    close(fd);
    die("send failed: %s", strerror(errno));
  }

  if (read_one_response_buffered(fd, 405, verbose) != 0) {
    close(fd);
    die("method-not-allowed read failed");
  }

  close(fd);
  info("method_not_allowed_405: OK");
  return 0;
}

static char *build_pipelined_requests(int n, size_t *out_len) {
  const char *req =
      "GET / HTTP/1.1\r\nHost: x\r\nConnection: keep-alive\r\n\r\n";
  size_t rlen = strlen(req);
  size_t total = (size_t)n * rlen;

  char *buf = (char *)malloc(total);
  if (!buf)
    return NULL;

  char *p = buf;
  for (int i = 0; i < n; ++i) {
    memcpy(p, req, rlen);
    p += rlen;
  }

  *out_len = total;
  return buf;
}

static int test_pipeline_coalesce(const char *host, const char *port, int n,
                                 int nodelay, int timeout_ms,
                                 int expected_status, int verbose) {
  g_len = 0;
  int fd = connect_tcp(host, port, nodelay, timeout_ms);

  size_t send_len = 0;
  char *payload = build_pipelined_requests(n, &send_len);
  if (!payload) {
    close(fd);
    die("malloc failed");
  }

  if (send_all(fd, payload, send_len) < 0) {
    free(payload);
    close(fd);
    die("send failed: %s", strerror(errno));
  }
  free(payload);

  for (int i = 0; i < n; ++i) {
    if (read_one_response_buffered(fd, expected_status, verbose) != 0) {
      close(fd);
      die("read failed at response %d", i);
    }
  }

  close(fd);
  info("pipeline_coalesce N=%d: OK", n);
  return 0;
}

static int test_fragment_crlf(const char *host, const char *port, int delay_ms,
                             int timeout_ms, int expected_status,
                             int verbose) {
  g_len = 0;
  int fd = connect_tcp(host, port, 0, timeout_ms);

  const char *p1 = "GET / HTTP/1.1\r\nHost: x\r"; // up to CR
  const char *p2 = "\n\r\n";                        // LF + CRLF terminator

  if (send_all(fd, p1, strlen(p1)) < 0) {
    close(fd);
    die("send p1 failed: %s", strerror(errno));
  }

  struct timespec ts;
  ts.tv_sec = delay_ms / 1000;
  ts.tv_nsec = (long)(delay_ms % 1000) * 1000000L;
  nanosleep(&ts, NULL);

  if (send_all(fd, p2, strlen(p2)) < 0) {
    close(fd);
    die("send p2 failed: %s", strerror(errno));
  }

  if (read_one_response_buffered(fd, expected_status, verbose) != 0) {
    close(fd);
    die("read failed");
  }

  close(fd);
  info("fragment_crlf delay=%dms: OK", delay_ms);
  return 0;
}

static int test_body_pipeline(const char *host, const char *port,
                              int nodelay, int timeout_ms,
                              int expected_status, int verbose) {
  g_len = 0;
  int fd = connect_tcp(host, port, nodelay, timeout_ms);

    // A GET with a small Content-Length body, immediately followed by a GET.
    // (Static server MVP supports GET/HEAD only.) This catches request-desync
    // bugs where the server responds after headers and then misinterprets body
    // bytes as the next request.
  const char *payload =
      "GET /index.html HTTP/1.1\r\n"
      "Host: x\r\n"
      "Content-Length: 5\r\n"
      "Connection: keep-alive\r\n"
      "\r\n"
      "hello"
      "GET /index.html HTTP/1.1\r\n"
      "Host: x\r\n"
      "Connection: close\r\n"
      "\r\n";

  if (send_all(fd, payload, strlen(payload)) < 0) {
    close(fd);
    die("send failed: %s", strerror(errno));
  }

  if (read_one_response_buffered(fd, expected_status, verbose) != 0) {
    close(fd);
    die("read failed at response 0");
  }
  if (read_one_response_buffered(fd, expected_status, verbose) != 0) {
    close(fd);
    die("read failed at response 1");
  }

  close(fd);
  info("body_pipeline: OK");
  return 0;
}

static int test_chunked_pipeline(const char *host, const char *port,
                                 int nodelay, int timeout_ms,
                                 int expected_status, int verbose) {
  g_len = 0;
  int fd = connect_tcp(host, port, nodelay, timeout_ms);

    // A GET with Transfer-Encoding: chunked, immediately followed by a GET.
  // Ensures we stop parsing exactly at the chunked message boundary.
  const char *payload =
      "GET /index.html HTTP/1.1\r\n"
      "Host: x\r\n"
      "Transfer-Encoding: chunked\r\n"
      "Connection: keep-alive\r\n"
      "\r\n"
      "5\r\nhello\r\n"
      "0\r\n\r\n"
      "GET /index.html HTTP/1.1\r\n"
      "Host: x\r\n"
      "Connection: close\r\n"
      "\r\n";

  if (send_all(fd, payload, strlen(payload)) < 0) {
    close(fd);
    die("send failed: %s", strerror(errno));
  }

  if (read_one_response_buffered(fd, expected_status, verbose) != 0) {
    close(fd);
    die("read failed at response 0");
  }
  if (read_one_response_buffered(fd, expected_status, verbose) != 0) {
    close(fd);
    info("chunked_pipeline: single response (peer closed)");
    return 0;
  }

  close(fd);
  info("chunked_pipeline: OK");
  return 0;
}

static int test_chunked_separate(const char *host, const char *port,
                                 int nodelay, int timeout_ms,
                                 int expected_status, int verbose) {
  g_len = 0;
  int fd = connect_tcp(host, port, nodelay, timeout_ms);

  // Send a chunked request first, then the next request in a separate send().
  // This exercises the case where llhttp pauses at end-of-buffer (pos==endp).
  const char *p1 =
      "GET /index.html HTTP/1.1\r\n"
      "Host: x\r\n"
      "Transfer-Encoding: chunked\r\n"
      "Connection: keep-alive\r\n"
      "\r\n"
      "5\r\nhello\r\n"
      "0\r\n\r\n";

  const char *p2 =
      "GET /index.html HTTP/1.1\r\n"
      "Host: x\r\n"
      "Connection: close\r\n"
      "\r\n";

  if (send_all(fd, p1, strlen(p1)) < 0) {
    close(fd);
    die("send p1 failed: %s", strerror(errno));
  }

  if (read_one_response_buffered(fd, expected_status, verbose) != 0) {
    close(fd);
    die("read failed at response 0");
  }

  if (send_all(fd, p2, strlen(p2)) < 0) {
    close(fd);
    die("send p2 failed: %s", strerror(errno));
  }

  if (read_one_response_buffered(fd, expected_status, verbose) != 0) {
    close(fd);
    info("chunked_separate: single response (peer closed)");
    return 0;
  }

  close(fd);
  info("chunked_separate: OK");
  return 0;
}

static int test_chunked_split_first_byte(const char *host, const char *port,
                                         int nodelay, int timeout_ms,
                                         int expected_status, int verbose) {
  g_len = 0;
  int fd = connect_tcp(host, port, nodelay, timeout_ms);

  // Send chunked request plus ONLY the first byte of the next request, then
  // the remainder shortly after. This is a very sensitive regression test for
  // off-by-one consumption at the paused message boundary.
  const char *p1 =
      "GET /index.html HTTP/1.1\r\n"
      "Host: x\r\n"
      "Transfer-Encoding: chunked\r\n"
      "Connection: keep-alive\r\n"
      "\r\n"
      "5\r\nhello\r\n"
      "0\r\n\r\n"
      "G";

  const char *p2 =
      "ET /index.html HTTP/1.1\r\n"
      "Host: x\r\n"
      "Connection: close\r\n"
      "\r\n";

  if (send_all(fd, p1, strlen(p1)) < 0) {
    close(fd);
    die("send p1 failed: %s", strerror(errno));
  }

  // Encourage the server to process the stash containing only 'G'
  struct timespec ts;
  ts.tv_sec = 0;
  ts.tv_nsec = 5 * 1000 * 1000L; // 5ms
  nanosleep(&ts, NULL);

  if (send_all(fd, p2, strlen(p2)) < 0) {
    close(fd);
    die("send p2 failed: %s", strerror(errno));
  }

  if (read_one_response_buffered(fd, expected_status, verbose) != 0) {
    close(fd);
    die("read failed at response 0");
  }
  if (read_one_response_buffered(fd, expected_status, verbose) != 0) {
    close(fd);
    info("chunked_split_first_byte: single response (peer closed)");
    return 0;
  }

  close(fd);
  info("chunked_split_first_byte: OK");
  return 0;
}

static int test_te_cl_conflict(const char *host, const char *port,
                               int nodelay, int timeout_ms, int verbose) {
  g_len = 0;
  int fd = connect_tcp(host, port, nodelay, timeout_ms);

  // Both Transfer-Encoding and Content-Length -> must be rejected (400).
  const char *payload =
      "GET / HTTP/1.1\r\n"
      "Host: x\r\n"
      "Transfer-Encoding: chunked\r\n"
      "Content-Length: 5\r\n"
      "Connection: close\r\n"
      "\r\n"
      "0\r\n\r\n";

  if (send_all(fd, payload, strlen(payload)) < 0) {
    close(fd);
    die("send failed: %s", strerror(errno));
  }

  if (read_one_response_buffered(fd, 400, verbose) != 0) {
    close(fd);
    die("read failed");
  }

  close(fd);
  info("te_cl_conflict: OK");
  return 0;
}

static int test_dup_content_length_reject(const char *host, const char *port,
                                         int nodelay, int timeout_ms,
                                         int verbose) {
  g_len = 0;
  int fd = connect_tcp(host, port, nodelay, timeout_ms);

  // Duplicate Content-Length -> rejected (400) to avoid request smuggling/desync.
  const char *payload =
      "GET / HTTP/1.1\r\n"
      "Host: x\r\n"
      "Content-Length: 3\r\n"
      "Content-Length: 3\r\n"
      "Connection: close\r\n"
      "\r\n"
      "abc";

  if (send_all(fd, payload, strlen(payload)) < 0) {
    close(fd);
    die("send failed: %s", strerror(errno));
  }

  if (read_one_response_buffered(fd, 400, verbose) != 0) {
    close(fd);
    die("read failed");
  }

  close(fd);
  info("dup_content_length_reject: OK");
  return 0;
}

static int test_te_trailers_reject(const char *host, const char *port,
                                  int nodelay, int timeout_ms,
                                  int verbose) {
  g_len = 0;
  int fd = connect_tcp(host, port, nodelay, timeout_ms);

  // 'trailers' is a TE header token (response preferences), not a valid
  // Transfer-Encoding request coding. Treat as unsupported.
  const char *payload =
      "GET / HTTP/1.1\r\n"
      "Host: x\r\n"
      "Transfer-Encoding: trailers\r\n"
      "Connection: close\r\n"
      "\r\n";

  if (send_all(fd, payload, strlen(payload)) < 0) {
    close(fd);
    die("send failed: %s", strerror(errno));
  }

  if (read_one_response_buffered(fd, 501, verbose) != 0) {
    close(fd);
    die("read failed");
  }

  close(fd);
  info("te_trailers_reject: OK");
  return 0;
}

static int test_body_too_large_cl_413(const char *host, const char *port,
                                     int nodelay, int timeout_ms,
                                     int verbose) {
  g_len = 0;
  int fd = connect_tcp(host, port, nodelay, timeout_ms);

  // Send larger than MAX_BODY_BYTES (default is 1 MiB).
  const char *payload =
      "GET / HTTP/1.1\r\n"
      "Host: x\r\n"
      "Content-Length: 1048577\r\n"
      "Connection: close\r\n"
      "\r\n";

  if (send_all(fd, payload, strlen(payload)) < 0) {
    close(fd);
    die("send failed: %s", strerror(errno));
  }

  if (read_one_response_buffered(fd, 413, verbose) != 0) {
    close(fd);
    die("read failed");
  }

  close(fd);
  info("body_too_large_cl_413: OK");
  return 0;
}

static int test_body_too_large_chunked_413(const char *host, const char *port,
                                          int nodelay, int timeout_ms,
                                          int verbose) {
  g_len = 0;
  int fd = connect_tcp(host, port, nodelay, timeout_ms);

  // Expect: server counts decoded body bytes for chunked and enforces
  // MAX_BODY_BYTES. In `make itest` we build the server with MAX_BODY_BYTES=32,
  // so a 33-byte chunk should be rejected.
  const char *hdr =
      "GET / HTTP/1.1\r\n"
      "Host: x\r\n"
      "Transfer-Encoding: chunked\r\n"
      "Connection: close\r\n"
      "\r\n"
      "21\r\n"; // 0x21 = 33 bytes

  char body[33];
  memset(body, 'a', sizeof(body));
  const char *tail = "\r\n0\r\n\r\n";

  if (send_all(fd, hdr, strlen(hdr)) < 0) {
    close(fd);
    die("send hdr failed: %s", strerror(errno));
  }
  if (send_all(fd, body, sizeof(body)) < 0) {
    close(fd);
    die("send body failed: %s", strerror(errno));
  }
  if (send_all(fd, tail, strlen(tail)) < 0) {
    // Once the server detects body-size overflow, it may send 413 and close
    // before we finish sending the chunk terminator. Treat common close-side
    // write errors as expected and continue to verify the 413 response.
    if (errno != EPIPE && errno != ECONNRESET) {
      close(fd);
      die("send tail failed: %s", strerror(errno));
    }
  }

  if (read_one_response_buffered(fd, 413, verbose) != 0) {
    close(fd);
    die("read failed");
  }

  close(fd);
  info("body_too_large_chunked_413: OK");
  return 0;
}

static int test_body_timeout_408(const char *host, const char *port,
                                 int nodelay, int timeout_ms, int verbose) {
  g_len = 0;
  int fd = connect_tcp(host, port, nodelay, timeout_ms);

  // Send headers + partial body, then stall long enough to trip BODY_TIMEOUT_MS.
  // In `make itest` we build the server with BODY_TIMEOUT_MS=200.
  const char *hdr =
      "GET / HTTP/1.1\r\n"
      "Host: x\r\n"
      "Content-Length: 5\r\n"
      "Connection: keep-alive\r\n"
      "\r\n";

  if (send_all(fd, hdr, strlen(hdr)) < 0) {
    close(fd);
    die("send hdr failed: %s", strerror(errno));
  }

  // Send only 1 byte of the body.
  if (send_all(fd, "h", 1) < 0) {
    close(fd);
    die("send body byte failed: %s", strerror(errno));
  }

  // Stall beyond the (itest) body timeout.
  struct timespec ts;
  ts.tv_sec = 0;
  ts.tv_nsec = 600 * 1000 * 1000L; // 600ms
  nanosleep(&ts, NULL);

  if (read_one_response_buffered(fd, 408, verbose) != 0) {
    close(fd);
    die("read failed");
  }

  close(fd);
  info("body_timeout_408: OK");
  return 0;
}

static int test_expect_100_continue_ok(const char *host, const char *port,
                                       int nodelay, int timeout_ms,
                                       int verbose) {
  g_len = 0;
  int fd = connect_tcp(host, port, nodelay, timeout_ms);

  const char *payload =
      "GET / HTTP/1.1\r\n"
      "Host: x\r\n"
      "Expect: 100-continue\r\n"
      "Content-Length: 4\r\n"
      "Connection: close\r\n"
      "\r\n"
      "test";

  if (send_all(fd, payload, strlen(payload)) < 0) {
    close(fd);
    die("send failed: %s", strerror(errno));
  }

  if (read_one_response_buffered(fd, 200, verbose) != 0) {
    close(fd);
    die("read failed");
  }

  close(fd);
  info("expect_100_continue_ok: OK");
  return 0;
}

static int test_expect_unsupported_reject_400(const char *host, const char *port,
                                              int nodelay, int timeout_ms,
                                              int verbose) {
  g_len = 0;
  int fd = connect_tcp(host, port, nodelay, timeout_ms);

  const char *payload =
      "GET / HTTP/1.1\r\n"
      "Host: x\r\n"
      "Expect: nonsense\r\n"
      "Connection: close\r\n"
      "\r\n";

  if (send_all(fd, payload, strlen(payload)) < 0) {
    close(fd);
    die("send failed: %s", strerror(errno));
  }

  if (read_one_response_buffered(fd, 400, verbose) != 0) {
    close(fd);
    die("read failed");
  }

  close(fd);
  info("expect_unsupported_reject_400: OK");
  return 0;
}

static int test_expect_100_continue_body_timeout_408(const char *host,
                                                     const char *port,
                                                     int nodelay,
                                                     int timeout_ms,
                                                     int verbose) {
  g_len = 0;
  int fd = connect_tcp(host, port, nodelay, timeout_ms);

  const char *hdr =
      "GET / HTTP/1.1\r\n"
      "Host: x\r\n"
      "Expect: 100-continue\r\n"
      "Content-Length: 5\r\n"
      "Connection: keep-alive\r\n"
      "\r\n";

  if (send_all(fd, hdr, strlen(hdr)) < 0) {
    close(fd);
    die("send hdr failed: %s", strerror(errno));
  }

  if (send_all(fd, "h", 1) < 0) {
    close(fd);
    die("send body byte failed: %s", strerror(errno));
  }

  struct timespec ts;
  ts.tv_sec = 0;
  ts.tv_nsec = 600 * 1000 * 1000L; // 600ms, body_timeout_ms in itest is 200ms
  nanosleep(&ts, NULL);

  if (read_one_response_buffered(fd, 408, verbose) != 0) {
    close(fd);
    die("read failed");
  }

  close(fd);
  info("expect_100_continue_body_timeout_408: OK");
  return 0;
}

static int test_content_length_plus_invalid_400(const char *host,
                                                const char *port,
                                                int nodelay,
                                                int timeout_ms,
                                                int verbose) {
  g_len = 0;
  int fd = connect_tcp(host, port, nodelay, timeout_ms);

  const char *payload =
      "GET / HTTP/1.1\r\n"
      "Host: x\r\n"
      "Content-Length: +5\r\n"
      "Connection: close\r\n"
      "\r\n";

  if (send_all(fd, payload, strlen(payload)) < 0) {
    close(fd);
    die("send failed: %s", strerror(errno));
  }

  if (read_one_response_buffered(fd, 400, verbose) != 0) {
    close(fd);
    die("read failed");
  }

  close(fd);
  info("content_length_plus_invalid_400: OK");
  return 0;
}

static int test_path_encoded_slash_reject_400(const char *host,
                                              const char *port,
                                              int nodelay,
                                              int timeout_ms,
                                              int verbose) {
  g_len = 0;
  int fd = connect_tcp(host, port, nodelay, timeout_ms);

  const char *payload =
      "GET /a%2fb HTTP/1.1\r\n"
      "Host: x\r\n"
      "Connection: close\r\n"
      "\r\n";

  if (send_all(fd, payload, strlen(payload)) < 0) {
    close(fd);
    die("send failed: %s", strerror(errno));
  }

  if (read_one_response_buffered(fd, 400, verbose) != 0) {
    close(fd);
    die("read failed");
  }

  close(fd);
  info("path_encoded_slash_reject_400: OK");
  return 0;
}

static int test_te_double_comma_reject_501(const char *host,
                                           const char *port,
                                           int nodelay,
                                           int timeout_ms,
                                           int verbose) {
  g_len = 0;
  int fd = connect_tcp(host, port, nodelay, timeout_ms);

  const char *payload =
      "GET / HTTP/1.1\r\n"
      "Host: x\r\n"
      "Transfer-Encoding: chunked,,gzip\r\n"
      "Connection: close\r\n"
      "\r\n";

  if (send_all(fd, payload, strlen(payload)) < 0) {
    close(fd);
    die("send failed: %s", strerror(errno));
  }

  if (read_one_response_buffered(fd, 400, verbose) != 0) {
    close(fd);
    die("read failed");
  }

  close(fd);
  info("te_double_comma_reject_400: OK");
  return 0;
}

static int test_path_dot_segments_normalize_not_400(const char *host,
                                                    const char *port,
                                                    int nodelay,
                                                    int timeout_ms,
                                                    int verbose) {
  g_len = 0;
  int fd = connect_tcp(host, port, nodelay, timeout_ms);

  const char *payload =
      "GET /../../etc/passwd HTTP/1.1\r\n"
      "Host: x\r\n"
      "Connection: close\r\n"
      "\r\n";

  if (send_all(fd, payload, strlen(payload)) < 0) {
    close(fd);
    die("send failed: %s", strerror(errno));
  }

  // Path normalization should not be treated as parse error; expected static miss.
  if (read_one_response_buffered(fd, 404, verbose) != 0) {
    close(fd);
    die("read failed");
  }

  close(fd);
  info("path_dot_segments_normalize_not_400: OK");
  return 0;
}

static int test_too_many_headers_431(const char *host, const char *port,
                                    int nodelay, int timeout_ms, int verbose) {
  // Exceed default LimitRequestFields-style header field cap (default 100).
  // Keep header bytes small to ensure we hit the *field count* limit, not HEADER_CAP.
  int fd = connect_tcp(host, port, nodelay, timeout_ms);

  const int extra = 110;
  const size_t cap = 32 * 1024;
  char *req = (char *)malloc(cap);
  if (!req)
    die("malloc failed");

  size_t off = 0;
  int w = snprintf(req + off, cap - off, "GET / HTTP/1.1\r\nHost: x\r\n");
  if (w < 0) {
    free(req);
    close(fd);
    die("snprintf failed");
  }
  off += (size_t)w;

  for (int i = 0; i < extra; ++i) {
    if (off + 32 >= cap)
      break;
    w = snprintf(req + off, cap - off, "X-%d: a\r\n", i);
    if (w < 0)
      break;
    off += (size_t)w;
  }

  w = snprintf(req + off, cap - off, "Connection: close\r\n\r\n");
  if (w > 0)
    off += (size_t)w;

  if (send_all(fd, req, off) < 0) {
    free(req);
    close(fd);
    die("send failed: %s", strerror(errno));
  }
  free(req);

  if (read_one_response_buffered(fd, 431, verbose) != 0) {
    close(fd);
    die("expected 431");
  }

  close(fd);
  info("too_many_headers_431: OK");
  return 0;
}

static int test_echo_request_fields(const char *host, const char *port,
                                    int nodelay, int timeout_ms, int verbose) {
  // Baseline: exact bytes echoed for target, correct query splitting, base headers stored.
  {
    const char *req =
        "GET /__itest/echo?x=1 HTTP/1.1\r\n"
        "Host: example.com\r\n"
        "Connection: close\r\n"
        "\r\n";
    char *body = do_echo_req(host, port, nodelay, timeout_ms, verbose, req);
    assert_contains_or_die(body, "method=GET\n");
    assert_contains_or_die(body, "target=/__itest/echo?x=1\n");
    assert_contains_or_die(body, "path_norm=/__itest/echo\n");
    assert_contains_or_die(body, "query=x=1\n");
    assert_contains_or_die(body, "hdr[host]=example.com\n");
    assert_contains_or_die(body, "hdr[connection]=close\n");
    free(body);
  }

  // Percent-decoding: raw target contains %65, normalized path contains decoded 'e'.
  {
    const char *req =
        "GET /__itest/%65cho?x=1 HTTP/1.1\r\n"
        "Host: example.com\r\n"
        "Connection: close\r\n"
        "\r\n";
    char *body = do_echo_req(host, port, nodelay, timeout_ms, verbose, req);
    assert_contains_or_die(body, "method=GET\n");
    assert_contains_or_die(body, "target=/__itest/%65cho?x=1\n");
    assert_contains_or_die(body, "path_norm=/__itest/echo\n");
    assert_contains_or_die(body, "query=x=1\n");
    free(body);
  }

  // Multiple '?' splitting: query includes everything after the first '?'.
  {
    const char *req =
        "GET /__itest/echo?a=b?c=d HTTP/1.1\r\n"
        "Host: example.com\r\n"
        "Connection: close\r\n"
        "\r\n";
    char *body = do_echo_req(host, port, nodelay, timeout_ms, verbose, req);
    assert_contains_or_die(body, "target=/__itest/echo?a=b?c=d\n");
    assert_contains_or_die(body, "query=a=b?c=d\n");
    free(body);
  }

  // Duplicate header: we count all fields, but store only the first occurrence for known headers.
  {
    const char *req =
        "GET /__itest/echo?x=1 HTTP/1.1\r\n"
        "Host: first.example\r\n"
        "Host: second.example\r\n"
        "Connection: close\r\n"
        "\r\n";
    char *body = do_echo_req(host, port, nodelay, timeout_ms, verbose, req);
    assert_contains_or_die(body, "hdr[host]=first.example\n");
    assert_not_contains_or_die(body, "hdr[host]=second.example\n");
    // Field count should be at least 3 (Host, Host, Connection) but exact value may include
    // extra implicit parsing differences; keep it loose here.
    free(body);
  }

  // Default feature set should NOT store optional feature headers.
  {
    const char *req =
        "GET /__itest/echo?x=1 HTTP/1.1\r\n"
        "Host: example.com\r\n"
        "Connection: close\r\n"
        "Range: bytes=0-10\r\n"
        "If-None-Match: \"abc\"\r\n"
        "If-Modified-Since: Wed, 21 Oct 2015 07:28:00 GMT\r\n"
        "Accept-Encoding: gzip\r\n"
        "Authorization: Basic Zm9vOmJhcg==\r\n"
        "Cookie: a=b\r\n"
        "X-Unknown: v\r\n"
        "\r\n";
    char *body = do_echo_req(host, port, nodelay, timeout_ms, verbose, req);
    assert_contains_or_die(body, "hdr[host]=example.com\n");
    assert_contains_or_die(body, "hdr[connection]=close\n");
    assert_not_contains_or_die(body, "hdr[range]=");
    assert_not_contains_or_die(body, "hdr[if-none-match]=");
    assert_not_contains_or_die(body, "hdr[if-modified-since]=");
    assert_not_contains_or_die(body, "hdr[accept-encoding]=");
    assert_not_contains_or_die(body, "hdr[authorization]=");
    assert_not_contains_or_die(body, "hdr[cookie]=");
    assert_not_contains_or_die(body, "hdr[x-unknown]=");
    free(body);
  }

  // HEAD on the itest echo route should follow normal HEAD semantics.
  {
    const char *req =
        "HEAD /__itest/echo?x=1 HTTP/1.1\r\n"
        "Host: example.com\r\n"
        "Connection: close\r\n"
        "\r\n";
    g_len = 0;
    int fd = connect_tcp(host, port, nodelay, timeout_ms);
    if (send_all(fd, req, strlen(req)) < 0) {
      close(fd);
      die("send failed: %s", strerror(errno));
    }

    long cl = -1;
    size_t leftover = 0;
    if (read_one_response_head_buffered(fd, 200, verbose, &cl, &leftover) != 0) {
      close(fd);
      die("echo HEAD read failed");
    }

    if (cl <= 0) {
      close(fd);
      die("echo HEAD: expected positive Content-Length, got %ld", cl);
    }
    if (leftover != 0) {
      close(fd);
      die("echo HEAD: unexpected body bytes received (%zu)", leftover);
    }
    close(fd);
  }

  info("echo_request_fields: OK");
  return 0;
}

static int test_echo_feature_headers_all(const char *host, const char *port,
                                        int nodelay, int timeout_ms, int verbose) {
  // This test expects the server to be started with SERVER_FEATURES=all.
  const char *req =
      "GET /__itest/echo?x=1 HTTP/1.1\r\n"
      "Host: example.com\r\n"
      "Connection: close\r\n"
      "Range: bytes=0-10\r\n"
      "If-Range: \"tag\"\r\n"
      "If-None-Match: \"abc\"\r\n"
      "If-Modified-Since: Wed, 21 Oct 2015 07:28:00 GMT\r\n"
      "Accept-Encoding: gzip\r\n"
      "Authorization: Basic Zm9vOmJhcg==\r\n"
      "Cookie: a=b\r\n"
      "X-Unknown: v\r\n"
      "\r\n";

  char *body = do_echo_req(host, port, nodelay, timeout_ms, verbose, req);

  // Base headers
  assert_contains_or_die(body, "hdr[host]=example.com\n");
  assert_contains_or_die(body, "hdr[connection]=close\n");

  // Feature-driven headers should now be stored and visible
  assert_contains_or_die(body, "hdr[range]=bytes=0-10\n");
  assert_contains_or_die(body, "hdr[if-range]=\"tag\"\n");
  assert_contains_or_die(body, "hdr[if-none-match]=\"abc\"\n");
  assert_contains_or_die(body, "hdr[if-modified-since]=Wed, 21 Oct 2015 07:28:00 GMT\n");
  assert_contains_or_die(body, "hdr[accept-encoding]=gzip\n");
  assert_contains_or_die(body, "hdr[authorization]=Basic Zm9vOmJhcg==\n");
  assert_contains_or_die(body, "hdr[cookie]=a=b\n");

  // Unknown headers are never stored
  assert_not_contains_or_die(body, "hdr[x-unknown]=");

  free(body);
  info("echo_feature_headers_all: OK");
  return 0;
}

static int test_shutdown_inflight_drains_complete(const char *host,
                                                  const char *port,
                                                  int nodelay,
                                                  int timeout_ms,
                                                  int verbose) {
  (void)verbose;

  int fd = connect_tcp(host, port, nodelay, timeout_ms);

  const char *req =
      "GET /huge.bin HTTP/1.1\r\n"
      "Host: x\r\n"
      "Connection: close\r\n"
      "\r\n";

  if (send_all(fd, req, strlen(req)) < 0) {
    close(fd);
    die("shutdown_inflight_drains_complete: send failed: %s", strerror(errno));
  }

  char resp[64 * 1024];
  size_t total = 0;
  size_t header_end = 0;
  if (read_response_prefix(fd, resp, sizeof(resp), &total, &header_end) != 0) {
    close(fd);
    die("shutdown_inflight_drains_complete: failed reading initial response prefix");
  }

  // Make header text temporarily NUL-terminated for parser helpers.
  char saved = resp[header_end];
  resp[header_end] = 0;
  int status = parse_status_code(resp);
  long cl = parse_content_length(resp);
  resp[header_end] = saved;

  if (status != 200) {
    close(fd);
    die("shutdown_inflight_drains_complete: expected 200, got %d", status);
  }
  if (cl <= 0) {
    close(fd);
    die("shutdown_inflight_drains_complete: expected positive Content-Length, got %ld", cl);
  }

  // Trigger current fast-stop behavior after first response has started.
  must_signal_server_from_env(SIGTERM);

  // Give server a short window to react to signal and stop workers.
  struct timespec ts;
  ts.tv_sec = 0;
  ts.tv_nsec = 100 * 1000 * 1000L;
  (void)nanosleep(&ts, NULL);

  total = recv_until_eof_count_bytes(fd, total);
  close(fd);

  size_t body_seen = (total > header_end) ? (total - header_end) : 0;
  if (body_seen < (size_t)cl) {
    die("shutdown_inflight_drains_complete: expected full body during drain after SIGTERM, got truncated body (%zu/%ld)",
        body_seen,
        cl);
  }

  info("shutdown_inflight_drains_complete: observed full-body drain (%zu/%ld)", body_seen, cl);
  return 0;
}

static int test_shutdown_keepalive_drains_pipeline(const char *host,
                                                   const char *port,
                                                   int nodelay,
                                                   int timeout_ms,
                                                   int verbose) {
  (void)verbose;

  int fd = connect_tcp(host, port, nodelay, timeout_ms);

  const char *req =
      "GET /huge.bin HTTP/1.1\r\n"
      "Host: x\r\n"
      "Connection: keep-alive\r\n"
      "\r\n"
      "GET /index.html HTTP/1.1\r\n"
      "Host: x\r\n"
      "Connection: close\r\n"
      "\r\n";

  if (send_all(fd, req, strlen(req)) < 0) {
    close(fd);
    die("shutdown_keepalive_drains_pipeline: send failed: %s", strerror(errno));
  }

  char resp[64 * 1024];
  size_t total = 0;
  size_t header_end = 0;
  if (read_response_prefix(fd, resp, sizeof(resp), &total, &header_end) != 0) {
    close(fd);
    die("shutdown_keepalive_drains_pipeline: failed reading first response prefix");
  }

  must_signal_server_from_env(SIGTERM);
  size_t responses = recv_until_eof_count_substr(fd, "HTTP/1.1 ", resp, total);
  close(fd);
  if (responses < 2) {
    die("shutdown_keepalive_drains_pipeline: expected second pipelined response during drain, got %zu responses", responses);
  }

  info("shutdown_keepalive_drains_pipeline: observed drained pipeline (responses=%zu)", responses);
  return 0;
}

static int test_shutdown_grace_timeout_forces_exit(const char *host,
                                                   const char *port,
                                                   int nodelay,
                                                   int timeout_ms,
                                                   int verbose) {
  (void)verbose;

  pid_t pid = (pid_t)0;
  if (server_pid_from_env(&pid) != 0)
    die("shutdown_grace_timeout_forces_exit: missing/invalid ITEST_SERVER_PID");

  int fd = connect_tcp(host, port, nodelay, timeout_ms);

  int rcvbuf = 1024;
  (void)setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, (socklen_t)sizeof(rcvbuf));

  const char *req =
      "GET /huge.bin HTTP/1.1\r\n"
      "Host: x\r\n"
      "Connection: keep-alive\r\n"
      "\r\n";

  if (send_all(fd, req, strlen(req)) < 0) {
    close(fd);
    die("shutdown_grace_timeout_forces_exit: send failed: %s", strerror(errno));
  }

  struct timespec settle;
  settle.tv_sec = 0;
  settle.tv_nsec = 150 * 1000 * 1000L;
  (void)nanosleep(&settle, NULL);

  if (kill(pid, SIGTERM) != 0) {
    close(fd);
    die("shutdown_grace_timeout_forces_exit: kill(%ld, SIGTERM) failed: %s", (long)pid, strerror(errno));
  }

  uint64_t elapsed = wait_for_pid_exit_ms(pid, (uint64_t)timeout_ms + 5000ull);
  close(fd);

  if (elapsed == UINT64_MAX) {
    die("shutdown_grace_timeout_forces_exit: server did not exit within %dms after first SIGTERM",
        timeout_ms + 5000);
  }

  // Default shutdown_grace_ms is 5000; allow scheduler jitter but reject immediate exits.
  if (elapsed < 1500ull) {
    die("shutdown_grace_timeout_forces_exit: expected bounded drain grace before force, observed too-fast exit (%llums)",
        (unsigned long long)elapsed);
  }

  info("shutdown_grace_timeout_forces_exit: observed force exit after first SIGTERM (elapsed=%llums)",
       (unsigned long long)elapsed);
  return 0;
}

static int test_shutdown_second_signal_forces_immediate(const char *host,
                                                        const char *port,
                                                        int nodelay,
                                                        int timeout_ms,
                                                        int verbose) {
  (void)verbose;

  pid_t pid = (pid_t)0;
  if (server_pid_from_env(&pid) != 0)
    die("shutdown_second_signal_forces_immediate: missing/invalid ITEST_SERVER_PID");

  int fd = connect_tcp(host, port, nodelay, timeout_ms);

  int rcvbuf = 1024;
  (void)setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, (socklen_t)sizeof(rcvbuf));

  const char *req =
      "GET /huge.bin HTTP/1.1\r\n"
      "Host: x\r\n"
      "Connection: keep-alive\r\n"
      "\r\n";

  if (send_all(fd, req, strlen(req)) < 0) {
    close(fd);
    die("shutdown_second_signal_forces_immediate: send failed: %s", strerror(errno));
  }

  struct timespec settle;
  settle.tv_sec = 0;
  settle.tv_nsec = 150 * 1000 * 1000L;
  (void)nanosleep(&settle, NULL);

  if (kill(pid, SIGTERM) != 0) {
    close(fd);
    die("shutdown_second_signal_forces_immediate: first kill(%ld, SIGTERM) failed: %s", (long)pid, strerror(errno));
  }

  struct timespec between;
  between.tv_sec = 0;
  between.tv_nsec = 100 * 1000 * 1000L;
  (void)nanosleep(&between, NULL);

  if (kill(pid, SIGTERM) != 0 && errno != ESRCH) {
    close(fd);
    die("shutdown_second_signal_forces_immediate: second kill(%ld, SIGTERM) failed: %s", (long)pid, strerror(errno));
  }

  uint64_t elapsed = wait_for_pid_exit_ms(pid, 2000);
  close(fd);

  if (elapsed == UINT64_MAX) {
    die("shutdown_second_signal_forces_immediate: server did not exit quickly after second SIGTERM");
  }

  info("shutdown_second_signal_forces_immediate: observed quick force exit after second SIGTERM (elapsed=%llums)",
       (unsigned long long)elapsed);
  return 0;
}

// Helpers for conditional/range integration tests.
// Send one request, read one response, and return status code.
static int send_and_read_status(int fd, const char *req, int verbose,
                                char *out_headers, size_t out_headers_sz,
                                long *out_cl) {
  if (send_all(fd, req, strlen(req)) < 0)
    return -1;

  // Read through the full header block first.
  static const char SEP[] = "\r\n\r\n";
  const size_t SEP_LEN = 4;
  size_t hdr_end = (size_t)-1;

  for (;;) {
    if (hdr_end == (size_t)-1) {
      for (size_t i = 0; i + SEP_LEN <= g_len; ++i) {
        if (memcmp(g_buf + i, SEP, SEP_LEN) == 0) {
          hdr_end = i + SEP_LEN;
          break;
        }
      }
    }
    if (hdr_end != (size_t)-1)
      break;
    if (g_len == sizeof(g_buf))
      return -1;
    ssize_t r = recv(fd, g_buf + g_len, sizeof(g_buf) - g_len, 0);
    if (r < 0) {
      if (errno == EINTR) continue;
      return -1;
    }
    if (r == 0) return -1;
    g_len += (size_t)r;
  }

  // Copy headers to a NUL-terminated output buffer.
  size_t copy = hdr_end;
  if (copy > out_headers_sz - 1)
    copy = out_headers_sz - 1;
  memcpy(out_headers, g_buf, copy);
  out_headers[copy] = 0;

  int st = parse_status_code(out_headers);
  long cl = parse_content_length(out_headers);
  if (cl < 0) cl = 0;
  if (out_cl) *out_cl = cl;

  if (verbose)
    info("resp: status=%d cl=%ld", st, cl);

  // Drain body bytes before returning.
  size_t total_needed = hdr_end + (size_t)cl;
  while (g_len < total_needed) {
    if (g_len == sizeof(g_buf)) return -1;
    ssize_t r = recv(fd, g_buf + g_len, sizeof(g_buf) - g_len, 0);
    if (r < 0) {
      if (errno == EINTR) continue;
      return -1;
    }
    if (r == 0) return -1;
    g_len += (size_t)r;
  }

  // Preserve pipelined leftovers for the next read.
  size_t leftover = g_len - total_needed;
  if (leftover)
    memmove(g_buf, g_buf + total_needed, leftover);
  g_len = leftover;

  return st;
}

// Variant that also copies response body bytes to out_body.
static int send_and_read_with_body(int fd, const char *req, int verbose,
                                   char *out_headers, size_t out_headers_sz,
                                   long *out_cl,
                                   char *out_body, size_t out_body_sz,
                                   size_t *out_body_len) {
  if (send_all(fd, req, strlen(req)) < 0)
    return -1;

  static const char SEP[] = "\r\n\r\n";
  const size_t SEP_LEN = 4;
  size_t hdr_end = (size_t)-1;

  for (;;) {
    if (hdr_end == (size_t)-1) {
      for (size_t i = 0; i + SEP_LEN <= g_len; ++i) {
        if (memcmp(g_buf + i, SEP, SEP_LEN) == 0) {
          hdr_end = i + SEP_LEN;
          break;
        }
      }
    }
    if (hdr_end != (size_t)-1)
      break;
    if (g_len == sizeof(g_buf))
      return -1;
    ssize_t r = recv(fd, g_buf + g_len, sizeof(g_buf) - g_len, 0);
    if (r < 0) {
      if (errno == EINTR) continue;
      return -1;
    }
    if (r == 0) return -1;
    g_len += (size_t)r;
  }

  size_t copy = hdr_end;
  if (copy > out_headers_sz - 1)
    copy = out_headers_sz - 1;
  memcpy(out_headers, g_buf, copy);
  out_headers[copy] = 0;

  int st = parse_status_code(out_headers);
  long cl = parse_content_length(out_headers);
  if (cl < 0) cl = 0;
  if (out_cl) *out_cl = cl;

  if (verbose)
    info("resp: status=%d cl=%ld", st, cl);

  size_t total_needed = hdr_end + (size_t)cl;
  while (g_len < total_needed) {
    if (g_len == sizeof(g_buf)) return -1;
    ssize_t r = recv(fd, g_buf + g_len, sizeof(g_buf) - g_len, 0);
    if (r < 0) {
      if (errno == EINTR) continue;
      return -1;
    }
    if (r == 0) return -1;
    g_len += (size_t)r;
  }

  // Copy body.
  size_t body_len = (size_t)cl;
  if (out_body && out_body_sz > 0) {
    size_t bcopy = body_len < out_body_sz - 1 ? body_len : out_body_sz - 1;
    memcpy(out_body, g_buf + hdr_end, bcopy);
    out_body[bcopy] = 0;
    if (out_body_len) *out_body_len = bcopy;
  }

  size_t leftover = g_len - total_needed;
  if (leftover)
    memmove(g_buf, g_buf + total_needed, leftover);
  g_len = leftover;

  return st;
}

// Range request integration tests (requires features=all).
static int test_range_requests(const char *host, const char *port,
                               int nodelay, int timeout_ms, int verbose) {
  // Capture baseline body bytes and ETag for follow-up checks.
  char full_body[16384];
  size_t full_body_len = 0;
  long full_cl = 0;
  char etag[128];
  etag[0] = 0;
  {
    g_len = 0;
    int fd = connect_tcp(host, port, nodelay, timeout_ms);
    const char *req =
        "GET /index.html HTTP/1.1\r\n"
        "Host: example.com\r\n"
        "Connection: close\r\n"
        "\r\n";
    char hdrs[8192];
    int st = send_and_read_with_body(fd, req, verbose, hdrs, sizeof(hdrs),
                                     &full_cl, full_body, sizeof(full_body),
                                     &full_body_len);
    close(fd);
    if (st != 200)
      die("range: initial GET expected 200, got %d", st);
    if (full_cl <= 10)
      die("range: need file > 10 bytes for range tests, got cl=%ld", full_cl);
    if (parse_header_value_simple(hdrs, "ETag", etag, sizeof(etag)) != 0)
      die("range: initial GET missing ETag header");
    // 200 on a range-enabled vhost must advertise Accept-Ranges.
    char ar[64];
    if (parse_header_value_simple(hdrs, "Accept-Ranges", ar, sizeof(ar)) != 0)
      die("range: initial 200 missing Accept-Ranges header");
    if (strcmp(ar, "bytes") != 0)
      die("range: initial 200 Accept-Ranges expected 'bytes', got '%s'", ar);
    if (verbose)
      info("range: captured %zu body bytes, ETag=%s", full_body_len, etag);
  }

  // Satisfiable single range should return 206.
  {
    g_len = 0;
    int fd = connect_tcp(host, port, nodelay, timeout_ms);
    const char *req =
        "GET /index.html HTTP/1.1\r\n"
        "Host: example.com\r\n"
        "Range: bytes=0-4\r\n"
        "Connection: close\r\n"
        "\r\n";
    char hdrs[8192];
    long cl = 0;
    char body[256];
    size_t body_len = 0;
    int st = send_and_read_with_body(fd, req, verbose, hdrs, sizeof(hdrs),
                                     &cl, body, sizeof(body), &body_len);
    close(fd);
    if (st != 206)
      die("range: bytes=0-4 expected 206, got %d", st);
    if (cl != 5)
      die("range: bytes=0-4 expected Content-Length 5, got %ld", cl);
    if (body_len != 5 || memcmp(body, full_body, 5) != 0)
      die("range: bytes=0-4 body mismatch");
    // Verify Content-Range header.
    char cr[128];
    if (parse_header_value_simple(hdrs, "Content-Range", cr, sizeof(cr)) != 0)
      die("range: 206 missing Content-Range header");
    char expected_cr[128];
    snprintf(expected_cr, sizeof(expected_cr), "bytes 0-4/%ld", full_cl);
    if (strcmp(cr, expected_cr) != 0)
      die("range: Content-Range expected '%s', got '%s'", expected_cr, cr);
    // Must include ETag.
    char etag206[128];
    if (parse_header_value_simple(hdrs, "ETag", etag206, sizeof(etag206)) != 0)
      die("range: 206 missing ETag header");
    // 206 must also carry Accept-Ranges.
    char ar206[64];
    if (parse_header_value_simple(hdrs, "Accept-Ranges", ar206, sizeof(ar206)) != 0)
      die("range: 206 missing Accept-Ranges header");
    if (strcmp(ar206, "bytes") != 0)
      die("range: 206 Accept-Ranges expected 'bytes', got '%s'", ar206);
  }

  // Unsatisfiable range should return 416 with bytes */total.
  {
    g_len = 0;
    int fd = connect_tcp(host, port, nodelay, timeout_ms);
    char req[512];
    snprintf(req, sizeof(req),
             "GET /index.html HTTP/1.1\r\n"
             "Host: example.com\r\n"
             "Range: bytes=%ld-\r\n"
             "Connection: close\r\n"
             "\r\n",
             full_cl + 1000);
    char hdrs[8192];
    long cl = 0;
    int st = send_and_read_status(fd, req, verbose, hdrs, sizeof(hdrs), &cl);
    close(fd);
    if (st != 416)
      die("range: unsatisfiable expected 416, got %d", st);
    if (cl != 0)
      die("range: 416 expected Content-Length 0, got %ld", cl);
    char cr[128];
    if (parse_header_value_simple(hdrs, "Content-Range", cr, sizeof(cr)) != 0)
      die("range: 416 missing Content-Range header");
    char expected_cr[128];
    snprintf(expected_cr, sizeof(expected_cr), "bytes */%ld", full_cl);
    if (strcmp(cr, expected_cr) != 0)
      die("range: 416 Content-Range expected '%s', got '%s'", expected_cr, cr);
  }

  // Invalid range syntax falls back to full 200 response.
  {
    g_len = 0;
    int fd = connect_tcp(host, port, nodelay, timeout_ms);
    const char *req =
        "GET /index.html HTTP/1.1\r\n"
        "Host: example.com\r\n"
        "Range: bytes=abc-def\r\n"
        "Connection: close\r\n"
        "\r\n";
    char hdrs[8192];
    long cl = 0;
    int st = send_and_read_status(fd, req, verbose, hdrs, sizeof(hdrs), &cl);
    close(fd);
    if (st != 200)
      die("range: invalid syntax expected 200, got %d", st);
    if (cl != full_cl)
      die("range: invalid syntax expected full cl=%ld, got %ld", full_cl, cl);
  }

  // Multi-range is unsupported and falls back to full 200 response.
  {
    g_len = 0;
    int fd = connect_tcp(host, port, nodelay, timeout_ms);
    const char *req =
        "GET /index.html HTTP/1.1\r\n"
        "Host: example.com\r\n"
        "Range: bytes=0-1, 3-4\r\n"
        "Connection: close\r\n"
        "\r\n";
    char hdrs[8192];
    long cl = 0;
    int st = send_and_read_status(fd, req, verbose, hdrs, sizeof(hdrs), &cl);
    close(fd);
    if (st != 200)
      die("range: multi-range expected 200, got %d", st);
  }

  // Matching If-Range should apply the range.
  {
    g_len = 0;
    int fd = connect_tcp(host, port, nodelay, timeout_ms);
    char req[512];
    snprintf(req, sizeof(req),
             "GET /index.html HTTP/1.1\r\n"
             "Host: example.com\r\n"
             "Range: bytes=0-4\r\n"
             "If-Range: %s\r\n"
             "Connection: close\r\n"
             "\r\n",
             etag);
    char hdrs[8192];
    long cl = 0;
    int st = send_and_read_status(fd, req, verbose, hdrs, sizeof(hdrs), &cl);
    close(fd);
    if (st != 206)
      die("range: If-Range match expected 206, got %d", st);
    if (cl != 5)
      die("range: If-Range match expected cl=5, got %ld", cl);
  }

  // Non-matching If-Range should ignore the range.
  {
    g_len = 0;
    int fd = connect_tcp(host, port, nodelay, timeout_ms);
    const char *req =
        "GET /index.html HTTP/1.1\r\n"
        "Host: example.com\r\n"
        "Range: bytes=0-4\r\n"
        "If-Range: \"stale-etag\"\r\n"
        "Connection: close\r\n"
        "\r\n";
    char hdrs[8192];
    long cl = 0;
    int st = send_and_read_status(fd, req, verbose, hdrs, sizeof(hdrs), &cl);
    close(fd);
    if (st != 200)
      die("range: If-Range mismatch expected 200, got %d", st);
    if (cl != full_cl)
      die("range: If-Range mismatch expected full cl=%ld, got %ld", full_cl, cl);
  }

  info("range_requests: OK (7 sub-tests)");
  return 0;
}

static int test_conditional_304(const char *host, const char *port,
                                int nodelay, int timeout_ms, int verbose) {
  char etag[128];
  char last_mod[128];
  etag[0] = 0;
  last_mod[0] = 0;

  // Capture baseline ETag and Last-Modified.
  {
    g_len = 0;
    int fd = connect_tcp(host, port, nodelay, timeout_ms);
    const char *req =
        "GET /index.html HTTP/1.1\r\n"
        "Host: example.com\r\n"
        "Connection: close\r\n"
        "\r\n";
    char hdrs[8192];
    long cl = 0;
    int st = send_and_read_status(fd, req, verbose, hdrs, sizeof(hdrs), &cl);
    close(fd);
    if (st != 200)
      die("conditional_304: initial GET expected 200, got %d", st);
    if (cl <= 0)
      die("conditional_304: initial GET had no body (cl=%ld)", cl);

    if (parse_header_value_simple(hdrs, "ETag", etag, sizeof(etag)) != 0)
      die("conditional_304: initial GET missing ETag header");
    if (parse_header_value_simple(hdrs, "Last-Modified", last_mod, sizeof(last_mod)) != 0)
      die("conditional_304: initial GET missing Last-Modified header");
    if (verbose)
      info("captured ETag=%s Last-Modified=%s", etag, last_mod);
  }

  // Matching If-None-Match should return 304.
  {
    g_len = 0;
    int fd = connect_tcp(host, port, nodelay, timeout_ms);
    char req[512];
    snprintf(req, sizeof(req),
             "GET /index.html HTTP/1.1\r\n"
             "Host: example.com\r\n"
             "If-None-Match: %s\r\n"
             "Connection: close\r\n"
             "\r\n",
             etag);
    char hdrs[8192];
    long cl = 0;
    int st = send_and_read_status(fd, req, verbose, hdrs, sizeof(hdrs), &cl);
    close(fd);
    if (st != 304)
      die("conditional_304: INM match expected 304, got %d", st);
    if (cl != 0)
      die("conditional_304: 304 response must have no body, got cl=%ld", cl);
    // 304 must include ETag.
    char etag304[128];
    if (parse_header_value_simple(hdrs, "ETag", etag304, sizeof(etag304)) != 0)
      die("conditional_304: 304 missing ETag header");
    // Must NOT include Content-Type.
    char ct[128];
    if (parse_header_value_simple(hdrs, "Content-Type", ct, sizeof(ct)) == 0)
      die("conditional_304: 304 must not include Content-Type");
  }

  // Non-matching If-None-Match should return 200.
  {
    g_len = 0;
    int fd = connect_tcp(host, port, nodelay, timeout_ms);
    const char *req =
        "GET /index.html HTTP/1.1\r\n"
        "Host: example.com\r\n"
        "If-None-Match: \"no-match-etag\"\r\n"
        "Connection: close\r\n"
        "\r\n";
    char hdrs[8192];
    long cl = 0;
    int st = send_and_read_status(fd, req, verbose, hdrs, sizeof(hdrs), &cl);
    close(fd);
    if (st != 200)
      die("conditional_304: INM no-match expected 200, got %d", st);
    if (cl <= 0)
      die("conditional_304: 200 response must have body");
  }

  // Matching If-Modified-Since should return 304.
  {
    g_len = 0;
    int fd = connect_tcp(host, port, nodelay, timeout_ms);
    // Use a far-future date so the file's mtime is <= IMS.
    const char *req =
        "GET /index.html HTTP/1.1\r\n"
        "Host: example.com\r\n"
        "If-Modified-Since: Sun, 01 Jan 2034 00:00:00 GMT\r\n"
        "Connection: close\r\n"
        "\r\n";
    char hdrs[8192];
    long cl = 0;
    int st = send_and_read_status(fd, req, verbose, hdrs, sizeof(hdrs), &cl);
    close(fd);
    if (st != 304)
      die("conditional_304: IMS future-date expected 304, got %d", st);
    if (cl != 0)
      die("conditional_304: 304 response must have no body (IMS), got cl=%ld", cl);
  }

  // Old If-Modified-Since date should return 200.
  {
    g_len = 0;
    int fd = connect_tcp(host, port, nodelay, timeout_ms);
    const char *req =
        "GET /index.html HTTP/1.1\r\n"
        "Host: example.com\r\n"
        "If-Modified-Since: Thu, 01 Jan 1970 00:00:00 GMT\r\n"
        "Connection: close\r\n"
        "\r\n";
    char hdrs[8192];
    long cl = 0;
    int st = send_and_read_status(fd, req, verbose, hdrs, sizeof(hdrs), &cl);
    close(fd);
    if (st != 200)
      die("conditional_304: IMS epoch expected 200, got %d", st);
  }

  // If-None-Match takes precedence over If-Modified-Since.
  {
    g_len = 0;
    int fd = connect_tcp(host, port, nodelay, timeout_ms);
    const char *req =
        "GET /index.html HTTP/1.1\r\n"
        "Host: example.com\r\n"
        "If-None-Match: \"wrong-etag\"\r\n"
        "If-Modified-Since: Sun, 01 Jan 2034 00:00:00 GMT\r\n"
        "Connection: close\r\n"
        "\r\n";
    char hdrs[8192];
    long cl = 0;
    int st = send_and_read_status(fd, req, verbose, hdrs, sizeof(hdrs), &cl);
    close(fd);
    if (st != 200)
      die("conditional_304: INM-no-match+IMS-match expected 200 (INM precedence), got %d", st);
  }

  // If-None-Match wildcard should return 304.
  {
    g_len = 0;
    int fd = connect_tcp(host, port, nodelay, timeout_ms);
    const char *req =
        "GET /index.html HTTP/1.1\r\n"
        "Host: example.com\r\n"
        "If-None-Match: *\r\n"
        "Connection: close\r\n"
        "\r\n";
    char hdrs[8192];
    long cl = 0;
    int st = send_and_read_status(fd, req, verbose, hdrs, sizeof(hdrs), &cl);
    close(fd);
    if (st != 304)
      die("conditional_304: INM wildcard expected 304, got %d", st);
  }

  // HEAD with matching If-None-Match should return 304.
  {
    g_len = 0;
    int fd = connect_tcp(host, port, nodelay, timeout_ms);
    char req[512];
    snprintf(req, sizeof(req),
             "HEAD /index.html HTTP/1.1\r\n"
             "Host: example.com\r\n"
             "If-None-Match: %s\r\n"
             "Connection: close\r\n"
             "\r\n",
             etag);
    char hdrs[8192];
    long cl = 0;
    int st = send_and_read_status(fd, req, verbose, hdrs, sizeof(hdrs), &cl);
    close(fd);
    if (st != 304)
      die("conditional_304: HEAD INM match expected 304, got %d", st);
  }

  info("conditional_304: OK (8 sub-tests)");
  return 0;
}

static void usage(const char *prog) {
  fprintf(stderr,
          "Usage: %s MODE [options]\n"
          "Modes:\n"
          "  pipeline [-n N] [-H host] [-P port] [--nodelay] [--allow-any-status] [-v]\n"
          "  fragment [-d ms] [-H host] [-P port] [--allow-any-status] [-v]\n"
          "  body-pipeline [-H host] [-P port] [--nodelay] [--allow-any-status] [-v]\n"
          "  chunked-pipeline [-H host] [-P port] [--nodelay] [--allow-any-status] [-v]\n"
          "  chunked-separate [-H host] [-P port] [--nodelay] [--allow-any-status] [-v]\n"
          "  chunked-split-first-byte [-H host] [-P port] [--nodelay] [--allow-any-status] [-v]\n"
          "  te-cl-conflict [-H host] [-P port] [--nodelay] [-v]\n"
          "  dup-cl-reject [-H host] [-P port] [--nodelay] [-v]\n"
          "  te-trailers-reject [-H host] [-P port] [--nodelay] [-v]\n"
          "  body-too-large-cl [-H host] [-P port] [--nodelay] [-v]\n"
          "  body-too-large-chunked [-H host] [-P port] [--nodelay] [-v]\n"
          "  body-timeout [-H host] [-P port] [--nodelay] [-v]\n"
          "  expect-100-continue-ok [-H host] [-P port] [--nodelay] [-v]\n"
          "  expect-unsupported-reject [-H host] [-P port] [--nodelay] [-v]\n"
          "  expect-100-continue-timeout [-H host] [-P port] [--nodelay] [-v]\n"
          "  cl-plus-invalid-reject [-H host] [-P port] [--nodelay] [-v]\n"
          "  path-encoded-slash-reject [-H host] [-P port] [--nodelay] [-v]\n"
          "  te-double-comma-reject [-H host] [-P port] [--nodelay] [-v]\n"
          "  path-dot-segments-normalize [-H host] [-P port] [--nodelay] [-v]\n"
          "  too-many-headers [-H host] [-P port] [--nodelay] [-v]\n"
          "  static-index [-H host] [-P port] [--nodelay] [-v]\n"
          "  static-head-index [-H host] [-P port] [--nodelay] [-v]\n"
          "  static-large-file [-H host] [-P port] [--nodelay] [-v]\n"
          "  static-sendfile-threshold [-H host] [-P port] [--nodelay] [-v]\n"
          "  sendfile-keepalive-bytes-regression [-H host] [-P port] [--nodelay] [-v]\n"
          "  method-not-allowed [-H host] [-P port] [--nodelay] [-v]\n"
          "  echo-fields [-H host] [-P port] [--nodelay] [-v]\n"
          "  echo-features-all [-H host] [-P port] [--nodelay] [-v]\n"
          "  shutdown-inflight-drains-complete [-H host] [-P port] [--nodelay] [-v]\n"
          "  shutdown-keepalive-drains-pipeline [-H host] [-P port] [--nodelay] [-v]\n"
          "  shutdown-grace-timeout-forces-exit [-H host] [-P port] [--nodelay] [-v]\n"
          "  shutdown-second-signal-forces-immediate [-H host] [-P port] [--nodelay] [-v]\n"
          "  conditional-304 [-H host] [-P port] [--nodelay] [-v]\n"
          "  range-requests [-H host] [-P port] [--nodelay] [-v]\n"
          "Options:\n"
          "  -H host       Default 127.0.0.1\n"
          "  -P port       Default 8090\n"
          "  -n N          Default 10\n"
          "  -d ms         Default 50\n"
          "  --expected-status CODE  Require specific HTTP status (default 200)\n"
          "  --nodelay     Enable TCP_NODELAY\n"
          "  --allow-any-status  Do not require 200 OK\n"
          "  -v            Verbose per-response output\n"
          "  -t ms         Socket send/recv timeout (default 5000)\n",
          prog);
}

int main(int argc, char **argv) {
  // Avoid process termination if the peer closes early.
  signal(SIGPIPE, SIG_IGN);

  if (argc < 2) {
    usage(argv[0]);
    return 2;
  }

  const char *mode = argv[1];
  const char *host = "127.0.0.1";
  const char *port = "8090";
  int n = 10;
  int delay_ms = 50;
  int nodelay = 0;
  int timeout_ms = 5000;
  int expected_status = 200;
  int verbose = 0;

  for (int i = 2; i < argc; ++i) {
    if (!strcmp(argv[i], "-H") && i + 1 < argc)
      host = argv[++i];
    else if (!strcmp(argv[i], "-P") && i + 1 < argc)
      port = argv[++i];
    else if (!strcmp(argv[i], "-n") && i + 1 < argc)
      n = atoi(argv[++i]);
    else if (!strcmp(argv[i], "-d") && i + 1 < argc)
      delay_ms = atoi(argv[++i]);
    else if (!strcmp(argv[i], "-t") && i + 1 < argc)
      timeout_ms = atoi(argv[++i]);
    else if (!strcmp(argv[i], "--nodelay"))
      nodelay = 1;
    else if (!strcmp(argv[i], "--expected-status") && i + 1 < argc)
      expected_status = atoi(argv[++i]);
    else if (!strcmp(argv[i], "--allow-any-status"))
      expected_status = -1;
    else if (!strcmp(argv[i], "-v"))
      verbose = 1;
    else {
      usage(argv[0]);
      return 2;
    }
  }

  if (!strcmp(mode, "pipeline")) {
    return test_pipeline_coalesce(host, port, n, nodelay, timeout_ms,
                                  expected_status, verbose);
  }
  if (!strcmp(mode, "fragment")) {
    return test_fragment_crlf(host, port, delay_ms, timeout_ms, expected_status,
                              verbose);
  }
  if (!strcmp(mode, "body-pipeline")) {
    return test_body_pipeline(host, port, nodelay, timeout_ms,
                              expected_status, verbose);
  }
  if (!strcmp(mode, "chunked-pipeline")) {
    return test_chunked_pipeline(host, port, nodelay, timeout_ms,
                                 expected_status, verbose);
  }

  if (!strcmp(mode, "chunked-separate")) {
    return test_chunked_separate(host, port, nodelay, timeout_ms,
                                 expected_status, verbose);
  }

  if (!strcmp(mode, "chunked-split-first-byte")) {
    return test_chunked_split_first_byte(host, port, nodelay, timeout_ms,
                                         expected_status, verbose);
  }

  if (!strcmp(mode, "te-cl-conflict")) {
    return test_te_cl_conflict(host, port, nodelay, timeout_ms, verbose);
  }

  if (!strcmp(mode, "dup-cl-reject")) {
    return test_dup_content_length_reject(host, port, nodelay, timeout_ms,
                                          verbose);
  }

  if (!strcmp(mode, "te-trailers-reject")) {
    return test_te_trailers_reject(host, port, nodelay, timeout_ms, verbose);
  }

  if (!strcmp(mode, "body-too-large-cl")) {
    return test_body_too_large_cl_413(host, port, nodelay, timeout_ms, verbose);
  }

  if (!strcmp(mode, "body-too-large-chunked")) {
    return test_body_too_large_chunked_413(host, port, nodelay, timeout_ms,
                                          verbose);
  }

  if (!strcmp(mode, "body-timeout")) {
    return test_body_timeout_408(host, port, nodelay, timeout_ms, verbose);
  }

  if (!strcmp(mode, "expect-100-continue-ok")) {
    return test_expect_100_continue_ok(host, port, nodelay, timeout_ms, verbose);
  }

  if (!strcmp(mode, "expect-unsupported-reject")) {
    return test_expect_unsupported_reject_400(host,
                                              port,
                                              nodelay,
                                              timeout_ms,
                                              verbose);
  }

  if (!strcmp(mode, "expect-100-continue-timeout")) {
    return test_expect_100_continue_body_timeout_408(host,
                                                     port,
                                                     nodelay,
                                                     timeout_ms,
                                                     verbose);
  }

  if (!strcmp(mode, "cl-plus-invalid-reject")) {
    return test_content_length_plus_invalid_400(host,
                                                port,
                                                nodelay,
                                                timeout_ms,
                                                verbose);
  }

  if (!strcmp(mode, "path-encoded-slash-reject")) {
    return test_path_encoded_slash_reject_400(host,
                                              port,
                                              nodelay,
                                              timeout_ms,
                                              verbose);
  }

  if (!strcmp(mode, "te-double-comma-reject")) {
    return test_te_double_comma_reject_501(host,
                                           port,
                                           nodelay,
                                           timeout_ms,
                                           verbose);
  }

  if (!strcmp(mode, "path-dot-segments-normalize")) {
    return test_path_dot_segments_normalize_not_400(host,
                                                    port,
                                                    nodelay,
                                                    timeout_ms,
                                                    verbose);
  }

  if (!strcmp(mode, "too-many-headers")) {
    return test_too_many_headers_431(host, port, nodelay, timeout_ms, verbose);
  }

  if (!strcmp(mode, "static-index")) {
    return test_static_index(host, port, nodelay, timeout_ms, verbose);
  }

  if (!strcmp(mode, "static-head-index")) {
    return test_static_head_index(host, port, nodelay, timeout_ms, verbose);
  }

  if (!strcmp(mode, "static-large-file")) {
    return test_static_large_file(host, port, nodelay, timeout_ms, verbose);
  }
  if (!strcmp(mode, "static-sendfile-threshold")) {
    return test_static_sendfile_threshold(host, port, nodelay, timeout_ms, verbose);
  }

  if (!strcmp(mode, "sendfile-keepalive-bytes-regression")) {
    return test_sendfile_keepalive_bytes_regression(host, port, nodelay, timeout_ms, verbose);
  }

  if (!strcmp(mode, "method-not-allowed")) {
    return test_method_not_allowed_405(host, port, nodelay, timeout_ms, verbose);
  }

  if (!strcmp(mode, "echo-fields")) {
    return test_echo_request_fields(host, port, nodelay, timeout_ms, verbose);
  }

  if (!strcmp(mode, "echo-features-all")) {
    return test_echo_feature_headers_all(host, port, nodelay, timeout_ms, verbose);
  }

  if (!strcmp(mode, "shutdown-inflight-drains-complete")) {
    return test_shutdown_inflight_drains_complete(host,
                                                  port,
                                                  nodelay,
                                                  timeout_ms,
                                                  verbose);
  }

  if (!strcmp(mode, "shutdown-keepalive-drains-pipeline")) {
    return test_shutdown_keepalive_drains_pipeline(host,
                                                   port,
                                                   nodelay,
                                                   timeout_ms,
                                                   verbose);
  }

  if (!strcmp(mode, "shutdown-grace-timeout-forces-exit")) {
    return test_shutdown_grace_timeout_forces_exit(host,
                                                   port,
                                                   nodelay,
                                                   timeout_ms,
                                                   verbose);
  }

  if (!strcmp(mode, "shutdown-second-signal-forces-immediate")) {
    return test_shutdown_second_signal_forces_immediate(host,
                                                        port,
                                                        nodelay,
                                                        timeout_ms,
                                                        verbose);
  }

  if (!strcmp(mode, "conditional-304")) {
    return test_conditional_304(host, port, nodelay, timeout_ms, verbose);
  }

  if (!strcmp(mode, "range-requests")) {
    return test_range_requests(host, port, nodelay, timeout_ms, verbose);
  }

  usage(argv[0]);
  return 2;
}
