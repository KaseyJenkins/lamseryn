#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#ifndef ARRAY_SIZE
#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))
#endif

static volatile sig_atomic_t g_stop = 0;

static void on_sigint(int sig) {
  (void)sig;
  g_stop = 1;
}

static uint64_t now_ns(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

struct options {
  const char *host;
  const char *port;
  const char *path;
  const char *method;
  const char *extra_header;
  int concurrency;
  int duration_sec;
  uint64_t max_requests; // 0 = unlimited
  int pipeline_depth;
  size_t body_bytes;
  int initial_stagger_ms;
};

struct lat_buf {
  uint64_t *v;
  size_t len;
  size_t cap;
};

static int lat_push(struct lat_buf *b, uint64_t v) {
  if (b->len == b->cap) {
    size_t ncap = b->cap ? b->cap * 2 : 1024;
    uint64_t *nv = (uint64_t *)realloc(b->v, ncap * sizeof(uint64_t));
    if (!nv) {
      return -1;
    }
    b->v = nv;
    b->cap = ncap;
  }
  b->v[b->len++] = v;
  return 0;
}

static int cmp_u64(const void *a, const void *b) {
  uint64_t va = *(const uint64_t *)a;
  uint64_t vb = *(const uint64_t *)b;
  if (va < vb) {
    return -1;
  }
  if (va > vb) {
    return 1;
  }
  return 0;
}

static uint64_t pctile(struct lat_buf *b, double pct) {
  if (b->len == 0) {
    return 0;
  }
  size_t idx = (size_t)((pct / 100.0) * (double)(b->len - 1) + 0.5);
  if (idx >= b->len) {
    idx = b->len - 1;
  }
  qsort(b->v, b->len, sizeof(uint64_t), cmp_u64);
  return b->v[idx];
}

enum parse_phase {
  PH_HDRS = 0,
  PH_BODY_CL,
  PH_CHUNK_SIZE,
  PH_CHUNK_BODY,
  PH_CHUNK_CRLF,
  PH_DONE,
};

struct http_state {
  enum parse_phase ph;
  int chunked;
  size_t content_length;
  size_t body_read;
  size_t chunk_rem;
  size_t header_bytes;
};

struct conn {
  int fd;
  int connected;
  int closing;
  int want_write;
  uint64_t ready_after_ns;
  uint64_t req_sent;
  uint64_t req_done;
  uint64_t errors;
  uint64_t inflight;
  uint64_t start_ns_q[64];
  int q_head;
  int q_tail;
  struct http_state h;
  char rbuf[65536];
  size_t rlen;
};

struct bench {
  struct options opt;
  int epfd;
  char *req;
  size_t req_len;
  size_t req_cap;
  struct conn *conns;
  int nconns;
  uint64_t start_ns;
  uint64_t end_ns;
  uint64_t queued_total;
  uint64_t sent_total;
  uint64_t done_total;
  uint64_t err_total;
  uint64_t err_event;
  uint64_t err_connect;
  uint64_t err_send;
  uint64_t err_recv;
  uint64_t err_parse;
  uint64_t err_reopen;
  uint64_t bytes_total;
  struct lat_buf lat;
};

static int set_nonblock(int fd) {
  int fl = fcntl(fd, F_GETFL, 0);
  if (fl < 0) {
    return -1;
  }
  if (fcntl(fd, F_SETFL, fl | O_NONBLOCK) < 0) {
    return -1;
  }
  return 0;
}

static int dial_once(const char *host, const char *port) {
  struct addrinfo hints, *res = NULL;
  memset(&hints, 0, sizeof(hints));
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_family = AF_UNSPEC;
  hints.ai_flags = AI_NUMERICSERV;

  int rc = getaddrinfo(host, port, &hints, &res);
  if (rc != 0) {
    return -1;
  }

  int fd = -1;
  for (struct addrinfo *rp = res; rp; rp = rp->ai_next) {
    fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
    if (fd < 0) {
      continue;
    }
    if (set_nonblock(fd) != 0) {
      close(fd);
      fd = -1;
      continue;
    }
    rc = connect(fd, rp->ai_addr, rp->ai_addrlen);
    if (rc == 0 || errno == EINPROGRESS) {
      break;
    }
    close(fd);
    fd = -1;
  }
  if (res) {
    freeaddrinfo(res);
  }
  return fd;
}

static void http_reset(struct http_state *h) {
  memset(h, 0, sizeof(*h));
  h->ph = PH_HDRS;
}

static void conn_reset(struct conn *c) {
  c->connected = 0;
  c->closing = 0;
  c->want_write = 0;
  c->ready_after_ns = 0;
  c->req_sent = 0;
  c->req_done = 0;
  c->errors = 0;
  c->inflight = 0;
  c->q_head = c->q_tail = 0;
  c->rlen = 0;
  http_reset(&c->h);
}

static int conn_reopen(struct bench *b, struct conn *c) {
  if (c->fd >= 0) {
    epoll_ctl(b->epfd, EPOLL_CTL_DEL, c->fd, NULL);
    close(c->fd);
  }
  int fd = dial_once(b->opt.host, b->opt.port);
  if (fd < 0) {
    b->err_connect++;
    return -1;
  }
  c->fd = fd;
  conn_reset(c);
  struct epoll_event ev = {0};
  ev.data.ptr = c;
  ev.events = EPOLLIN | EPOLLOUT | EPOLLRDHUP | EPOLLERR | EPOLLET;
  if (epoll_ctl(b->epfd, EPOLL_CTL_ADD, fd, &ev) != 0) {
    close(fd);
    c->fd = -1;
    b->err_connect++;
    return -1;
  }
  return 0;
}

static int parse_headers(struct http_state *h, const char *buf, size_t len, size_t *body_off) {
  const char *p = memmem(buf, len, "\r\n\r\n", 4);
  if (!p) {
    return 0; // need more
  }
  size_t hdr_len = (size_t)(p - buf) + 4;
  h->chunked = 0;
  h->content_length = 0;
  h->body_read = 0;
  h->chunk_rem = 0;
  h->header_bytes = hdr_len;

  const char *line = buf;
  const char *end = buf + hdr_len;
  while (line < end) {
    const char *nl = memchr(line, '\n', (size_t)(end - line));
    if (!nl) {
      break;
    }
    size_t linelen = (size_t)(nl - line);
    if (linelen && line[linelen - 1] == '\r') {
      linelen--;
    }
    if (linelen == 0) {
      line = nl + 1;
      continue;
    }
    if (linelen >= 15 && strncasecmp(line, "Content-Length:", 15) == 0) {
      const char *v = line + 15;
      while (*v == ' ' || *v == '\t') {
        v++;
      }
      h->content_length = (size_t)strtoull(v, NULL, 10);
    } else if (linelen >= 18 && strncasecmp(line, "Transfer-Encoding:", 18) == 0) {
      const char *v = line + 18;
      while (*v == ' ' || *v == '\t') {
        v++;
      }
      if (strncasecmp(v, "chunked", 7) == 0) {
        h->chunked = 1;
      }
    }
    line = nl + 1;
  }
  *body_off = hdr_len;
  if (h->chunked) {
    h->ph = PH_CHUNK_SIZE;
  } else {
    h->ph = PH_BODY_CL;
  }
  return 1;
}

static int parse_chunk_size(const char *buf, size_t len, size_t *consumed, size_t *out_size) {
  const char *nl = memchr(buf, '\n', len);
  if (!nl) {
    return 0;
  }
  const char *line_end = nl;
  const char *line = buf;
  while (line_end > line && (line_end[-1] == '\r' || line_end[-1] == ' ' || line_end[-1] == '\t')) {
    line_end--;
  }
  char tmp[64];
  size_t l = (size_t)(line_end - line);
  if (l >= sizeof(tmp)) {
    l = sizeof(tmp) - 1;
  }
  memcpy(tmp, line, l);
  tmp[l] = '\0';
  unsigned long sz = strtoul(tmp, NULL, 16);
  *out_size = (size_t)sz;
  *consumed = (size_t)(nl - buf) + 1;
  return 1;
}

static int try_consume(struct bench *b, struct conn *c) {
  size_t offset = 0;

  for (;;) {
    if (c->h.ph == PH_HDRS) {
      size_t body_off = 0;
      int hdr_ok = parse_headers(&c->h, c->rbuf + offset, c->rlen - offset, &body_off);
      if (!hdr_ok) {
        break;
      }
      offset += body_off;
    }

    if (c->h.ph == PH_BODY_CL) {
      size_t need = c->h.content_length;
      size_t avail = (offset <= c->rlen) ? c->rlen - offset : 0;
      size_t take = (avail >= need - c->h.body_read) ? (need - c->h.body_read) : avail;
      c->h.body_read += take;
      offset += take;
      if (c->h.body_read >= need) {
        c->h.ph = PH_DONE;
      } else {
        break;
      }
    }

    if (c->h.ph == PH_CHUNK_SIZE) {
      size_t used = 0, chunk_sz = 0;
      if (!parse_chunk_size(c->rbuf + offset, c->rlen - offset, &used, &chunk_sz)) {
        break;
      }
      offset += used;
      c->h.chunk_rem = chunk_sz;
      if (chunk_sz == 0) {
        c->h.ph = PH_DONE;
      } else {
        c->h.ph = PH_CHUNK_BODY;
      }
    }

    if (c->h.ph == PH_CHUNK_BODY) {
      size_t avail = (offset <= c->rlen) ? c->rlen - offset : 0;
      size_t take = (avail >= c->h.chunk_rem) ? c->h.chunk_rem : avail;
      c->h.chunk_rem -= take;
      offset += take;
      if (c->h.chunk_rem == 0) {
        c->h.ph = PH_CHUNK_CRLF;
      } else {
        break;
      }
    }

    if (c->h.ph == PH_CHUNK_CRLF) {
      if (c->rlen - offset < 2) {
        break;
      }
      offset += 2;
      c->h.ph = PH_CHUNK_SIZE;
    }

    if (c->h.ph == PH_DONE) {
      uint64_t end_ns = now_ns();
      uint64_t lat = 0;
      if (c->q_head != c->q_tail) {
        uint64_t start = c->start_ns_q[c->q_head];
        c->q_head = (c->q_head + 1) % (int)ARRAY_SIZE(c->start_ns_q);
        if (end_ns > start) {
          lat = end_ns - start;
        }
      }
      if (lat && lat_push(&b->lat, lat) != 0) {
        return -1;
      }
      b->done_total++;
      c->req_done++;
      if (c->inflight > 0) {
        c->inflight--;
      }
      http_reset(&c->h);

      size_t remaining = c->rlen - offset;
      if (remaining > 0) {
        memmove(c->rbuf, c->rbuf + offset, remaining);
      }
      c->rlen = remaining;
      offset = 0;

      if (g_stop || now_ns() >= b->end_ns
          || (b->opt.max_requests && b->sent_total >= b->opt.max_requests)) {
        return 0;
      }

      c->want_write = 1;
      return 0; // let write path send next request
    }
  }

  // Keep any unprocessed data at front
  if (offset > 0 && offset < c->rlen) {
    size_t remaining = c->rlen - offset;
    memmove(c->rbuf, c->rbuf + offset, remaining);
    c->rlen = remaining;
  } else if (offset >= c->rlen) {
    c->rlen = 0;
  }
  return 0;
}

static int queue_request(struct bench *b, struct conn *c) {
  if (c->closing) {
    return -1;
  }
  if (b->opt.max_requests && b->queued_total >= b->opt.max_requests) {
    return -1;
  }
  if (c->want_write) {
    return -1;
  }
  if (c->req_sent != 0) {
    return -1;
  }
  if (c->inflight >= (uint64_t)b->opt.pipeline_depth) {
    return -1;
  }
  int next_tail = (c->q_tail + 1) % (int)ARRAY_SIZE(c->start_ns_q);
  if (next_tail == c->q_head) {
    return -1; // queue full
  }
  c->start_ns_q[c->q_tail] = now_ns();
  c->q_tail = next_tail;
  c->inflight++;
  c->want_write = 1;
  b->queued_total++;
  return 0;
}

static int ensure_send(struct bench *b, struct conn *c) {
  while (c->want_write) {
    ssize_t n = send(c->fd, b->req + c->req_sent, b->req_len - c->req_sent, MSG_NOSIGNAL);
    if (n < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        return 0;
      }
      if (c->inflight > 0) {
        c->inflight--;
      }
      c->req_sent = 0;
      c->want_write = 0;
      // force reconnect path to avoid leaving partial bytes on a reused socket
      c->closing = 1;
      return -1;
    }
    if (n == 0) {
      return 0;
    }

    c->req_sent += (size_t)n;
    if (c->req_sent >= b->req_len) {
      c->req_sent = 0;
      c->want_write = 0;
      b->sent_total++;
      return 0;
    }
  }
  return 0;
}

static int rebuild_request(struct bench *b) {
  const char *hdr = b->opt.extra_header ? b->opt.extra_header : "";
  size_t hdrlen = strlen(hdr);
  size_t body = b->opt.body_bytes;

  size_t need = (size_t)snprintf(NULL,
                                 0,
                                 "%s %s HTTP/1.1\r\n"
                                 "Host: %s\r\n"
                                 "User-Agent: bench-client/0.1\r\n"
                                 "Accept: */*\r\n"
                                 "Connection: keep-alive\r\n"
                                 "Content-Length: %zu\r\n"
                                 "%.*s%s"
                                 "\r\n",
                                 b->opt.method,
                                 b->opt.path,
                                 b->opt.host,
                                 body,
                                 (int)hdrlen,
                                 hdr,
                                 (hdrlen ? "\r\n" : ""));
  need += body;
  need += 1; // NUL

  if (need > b->req_cap) {
    char *nr = (char *)realloc(b->req, need);
    if (!nr) {
      return -1;
    }
    b->req = nr;
    b->req_cap = need;
  }

  int n = snprintf(b->req,
                   b->req_cap,
                   "%s %s HTTP/1.1\r\n"
                   "Host: %s\r\n"
                   "User-Agent: bench-client/0.1\r\n"
                   "Accept: */*\r\n"
                   "Connection: keep-alive\r\n"
                   "Content-Length: %zu\r\n"
                   "%.*s%s"
                   "\r\n",
                   b->opt.method,
                   b->opt.path,
                   b->opt.host,
                   body,
                   (int)hdrlen,
                   hdr,
                   (hdrlen ? "\r\n" : ""));
  if (n <= 0) {
    return -1;
  }
  size_t off = (size_t)n;
  if (body > 0) {
    if (off + body + 1 > b->req_cap) {
      return -1;
    }
    memset(b->req + off, 'A', body);
    off += body;
  }
  b->req[off] = '\0';
  b->req_len = off;
  return 0;
}

static int init_conns(struct bench *b) {
  b->conns = (struct conn *)calloc((size_t)b->opt.concurrency, sizeof(struct conn));
  if (!b->conns) {
    return -1;
  }
  b->nconns = b->opt.concurrency;
  for (int i = 0; i < b->nconns; i++) {
    b->conns[i].fd = -1;
    if (conn_reopen(b, &b->conns[i]) != 0) {
      return -1;
    }
  }
  return 0;
}

static void close_all(struct bench *b) {
  for (int i = 0; i < b->nconns; i++) {
    if (b->conns[i].fd >= 0) {
      close(b->conns[i].fd);
    }
    b->conns[i].fd = -1;
  }
}

static int handle_event(struct bench *b, struct conn *c, uint32_t events) {
  if (events & (EPOLLHUP | EPOLLERR | EPOLLRDHUP)) {
    b->err_event++;
    c->closing = 1;
    return -1;
  }
  if (!c->connected) {
    int err = 0;
    socklen_t sl = sizeof(err);
    if (getsockopt(c->fd, SOL_SOCKET, SO_ERROR, &err, &sl) != 0 || err != 0) {
      b->err_connect++;
      return -1;
    }
    c->connected = 1;
    uint64_t delay_ns =
      (b->opt.initial_stagger_ms > 0) ? (uint64_t)b->opt.initial_stagger_ms * 1000000ull : 0;
    c->ready_after_ns = now_ns() + delay_ns;
  }
  if (events & EPOLLOUT) {
    if (c->want_write && ensure_send(b, c) != 0) {
      b->err_send++;
      return -1;
    }
  }
  if (events & EPOLLIN) {
    for (;;) {
      ssize_t n = recv(c->fd, c->rbuf + c->rlen, sizeof(c->rbuf) - c->rlen, 0);
      if (n == 0) {
        c->closing = 1;
        return -1;
      } else if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
          break;
        }
        b->err_recv++;
        return -1;
      }
      c->rlen += (size_t)n;
      b->bytes_total += (uint64_t)n;
      if (try_consume(b, c) != 0) {
        b->err_parse++;
        return -1;
      }
    }
  }
  return 0;
}

static int add_events(struct bench *b, struct conn *c) {
  struct epoll_event ev = {0};
  ev.data.ptr = c;
  ev.events = EPOLLIN | EPOLLOUT | EPOLLRDHUP | EPOLLERR | EPOLLET;
  if (epoll_ctl(b->epfd, EPOLL_CTL_MOD, c->fd, &ev) != 0) {
    return -1;
  }
  return 0;
}

static void usage(const char *prog) {
  fprintf(stderr,
          "Usage: %s [-h host] [-p port] [-c conns] [-d seconds] [-n requests] [-M method] [-H "
          "header] [-P pipeline] [-B body_bytes] [-S initial_stagger_ms] [path]\n",
          prog);
}

static int parse_args(int argc, char **argv, struct options *opt) {
  opt->host = "127.0.0.1";
  opt->port = "8080";
  opt->path = "/";
  opt->method = "GET";
  opt->extra_header = NULL;
  opt->concurrency = 32;
  opt->duration_sec = 10;
  opt->max_requests = 0;
  opt->pipeline_depth = 1;
  opt->body_bytes = 0;
  opt->initial_stagger_ms = 0;

  int ch;
  while ((ch = getopt(argc, argv, "h:p:c:d:n:M:H:P:B:S:")) != -1) {
    switch (ch) {
    case 'h':
      opt->host = optarg;
      break;
    case 'p':
      opt->port = optarg;
      break;
    case 'c':
      opt->concurrency = atoi(optarg);
      break;
    case 'd':
      opt->duration_sec = atoi(optarg);
      break;
    case 'n':
      opt->max_requests = (uint64_t)strtoull(optarg, NULL, 10);
      break;
    case 'M':
      opt->method = optarg;
      break;
    case 'H':
      opt->extra_header = optarg;
      break;
    case 'P':
      opt->pipeline_depth = atoi(optarg);
      break;
    case 'B':
      opt->body_bytes = (size_t)strtoull(optarg, NULL, 10);
      break;
    case 'S':
      opt->initial_stagger_ms = atoi(optarg);
      break;
    default:
      usage(argv[0]);
      return -1;
    }
  }
  if (opt->concurrency <= 0) {
    opt->concurrency = 1;
  }
  if (opt->duration_sec <= 0) {
    opt->duration_sec = 10;
  }
  if (opt->pipeline_depth <= 0) {
    opt->pipeline_depth = 1;
  }
  if (opt->pipeline_depth > (int)ARRAY_SIZE(((struct conn *)0)->start_ns_q)) {
    opt->pipeline_depth = (int)ARRAY_SIZE(((struct conn *)0)->start_ns_q);
  }
  if (optind < argc) {
    opt->path = argv[optind];
  }
  return 0;
}

static void print_summary(struct bench *b) {
  double elapsed = (double)(now_ns() - b->start_ns) / 1e9;
  double rps = elapsed > 0 ? (double)b->done_total / elapsed : 0.0;
  uint64_t p50 = pctile(&b->lat, 50.0);
  uint64_t p95 = pctile(&b->lat, 95.0);
  uint64_t p99 = pctile(&b->lat, 99.0);
  printf("=== bench_client summary ===\n");
  printf("host=%s port=%s path=%s conns=%d duration=%ds\n",
         b->opt.host,
         b->opt.port,
         b->opt.path,
         b->opt.concurrency,
         b->opt.duration_sec);
  printf("sent=%llu done=%llu errors=%llu bytes=%llu\n",
         (unsigned long long)b->sent_total,
         (unsigned long long)b->done_total,
         (unsigned long long)b->err_total,
         (unsigned long long)b->bytes_total);
  printf("errors_detail: event=%llu connect=%llu send=%llu recv=%llu parse=%llu reopen=%llu\n",
         (unsigned long long)b->err_event,
         (unsigned long long)b->err_connect,
         (unsigned long long)b->err_send,
         (unsigned long long)b->err_recv,
         (unsigned long long)b->err_parse,
         (unsigned long long)b->err_reopen);
  printf("rps=%.2f p50=%.2fms p95=%.2fms p99=%.2fms\n",
         rps,
         (double)p50 / 1e6,
         (double)p95 / 1e6,
         (double)p99 / 1e6);
}

int main(int argc, char **argv) {
  struct bench b;
  memset(&b, 0, sizeof(b));

  if (parse_args(argc, argv, &b.opt) != 0) {
    return 1;
  }

  if (rebuild_request(&b) != 0) {
    fprintf(stderr, "failed to build request string\n");
    return 1;
  }

  signal(SIGINT, on_sigint);

  b.epfd = epoll_create1(EPOLL_CLOEXEC);
  if (b.epfd < 0) {
    perror("epoll_create1");
    return 1;
  }

  if (init_conns(&b) != 0) {
    fprintf(stderr, "failed to init connections\n");
    return 1;
  }

  b.start_ns = now_ns();
  b.end_ns = b.start_ns + (uint64_t)b.opt.duration_sec * 1000000000ull;

  struct epoll_event evs[256];
  while (!g_stop) {
    int timeout_ms = 100;
    int nfds = epoll_wait(b.epfd, evs, (int)ARRAY_SIZE(evs), timeout_ms);
    if (nfds < 0) {
      if (errno == EINTR) {
        continue;
      }
      perror("epoll_wait");
      break;
    }
    uint64_t now = now_ns();
    if (now >= b.end_ns && b.opt.max_requests == 0) {
      g_stop = 1;
    }

    for (int i = 0; i < nfds; i++) {
      struct conn *c = (struct conn *)evs[i].data.ptr;
      if (handle_event(&b, c, evs[i].events) != 0) {
        b.err_total++;
        if (conn_reopen(&b, c) != 0) {
          b.err_reopen++;
        }
        continue;
      }
      add_events(&b, c);
      while (!g_stop && now < b.end_ns
             && (!b.opt.max_requests || b.queued_total < b.opt.max_requests) && c->connected
             && now >= c->ready_after_ns && !c->want_write
             && c->inflight < (uint64_t)b.opt.pipeline_depth) {
        if (queue_request(&b, c) != 0) {
          break;
        }
      }
    }

    if (b.opt.max_requests && b.done_total >= b.opt.max_requests
        && b.sent_total >= b.opt.max_requests) {
      int inflight = 0;
      for (int i = 0; i < b.nconns; i++) {
        inflight += (b.conns[i].inflight != 0);
      }
      if (inflight == 0) {
        break;
      }
    }

    if (now >= b.end_ns) {
      int inflight = 0;
      for (int i = 0; i < b.nconns; i++) {
        inflight += (b.conns[i].inflight != 0);
      }
      if (inflight == 0) {
        break;
      }
    }
  }

  close_all(&b);
  print_summary(&b);
  free(b.lat.v);
  free(b.conns);
  free(b.req);
  close(b.epfd);
  return 0;
}
