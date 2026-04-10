#include "../vendor/greatest_color.h"
#include "../vendor/greatest.h"

#include "include/access_log.h"
#include "include/conn.h"
#include "include/time_utils.h"
#include "include/worker_ctx.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int parse_u64_field(const char *line, const char *key, uint64_t *out) {
  if (!line || !key || !out) {
    return -1;
  }

  const char *p = strstr(line, key);
  if (!p) {
    return -1;
  }
  p += strlen(key);

  errno = 0;
  char *end = NULL;
  unsigned long long v = strtoull(p, &end, 10);
  if (errno != 0 || end == p) {
    return -1;
  }

  *out = (uint64_t)v;
  return 0;
}

TEST t_cfg_defaults_without_present_bits(void) {
  struct access_log_cfg out;
  struct globals_cfg g;
  memset(&g, 0, sizeof(g));

  access_log_cfg_from_globals(&out, &g);

  ASSERT_EQ(out.enabled, 0);
  ASSERT_EQ(out.sample, (unsigned)1);
  ASSERT_EQ(out.min_status, (unsigned)100);
  ASSERT_EQ(strcmp(out.format, "text"), 0);
  ASSERT_EQ(out.path[0], '\0');

  PASS();
}

TEST t_sanitize_target_replaces_unsafe_and_marks_truncation(void) {
  char out[32];
  const char raw[] = "/a b\t\"\\\x01z";
  int truncated = 0;

  size_t n = access_log_sanitize_target(out, sizeof(out), raw, sizeof(raw) - 1, 6u, &truncated);

  ASSERT(n > 0);
  ASSERT_EQ(strcmp(out, "/a_b__"), 0);
  ASSERT_EQ(truncated, 1);

  PASS();
}

TEST t_format_text_line_has_expected_fields(void) {
  char line[1024];
  struct access_log_event ev;
  memset(&ev, 0, sizeof(ev));

  ev.ts_ms = 1710956000123ull;
  ev.worker = 2u;
  ev.method = "GET";
  ev.target = "/index.html?x=1";
  ev.target_len = strlen(ev.target);
  ev.remote_ip = "192.168.1.10";
  ev.remote_port = 54321u;
  ev.status = 200u;
  ev.bytes = 1234ull;
  ev.dur_us = 3000u;
  ev.keepalive = 1;
  ev.tls = 0;

  size_t n = access_log_format_text_line(line, sizeof(line), &ev, 256u);

  ASSERT(n > 0);
  ASSERT(strstr(line, "ts_ms=1710956000123") != NULL);
  ASSERT(strstr(line, "worker=2") != NULL);
  ASSERT(strstr(line, "ip=192.168.1.10") != NULL);
  ASSERT(strstr(line, "port=54321") != NULL);
  ASSERT(strstr(line, "method=GET") != NULL);
  ASSERT(strstr(line, "target=/index.html?x=1") != NULL);
  ASSERT(strstr(line, "status=200") != NULL);
  ASSERT(strstr(line, "bytes=1234") != NULL);
  ASSERT(strstr(line, "dur_us=3000") != NULL);
  ASSERT(strstr(line, "ka=1") != NULL);
  ASSERT(strstr(line, "tls=0") != NULL);
  ASSERT(strstr(line, "trunc=0") != NULL);
  ASSERT(line[n - 1] == '\n');

  PASS();
}

TEST t_format_text_line_ipv6(void) {
  char line[1024];
  struct access_log_event ev;
  memset(&ev, 0, sizeof(ev));

  ev.ts_ms = 1u;
  ev.worker = 0u;
  ev.method = "POST";
  ev.target = "/api";
  ev.target_len = 4;
  ev.remote_ip = "::1";
  ev.remote_port = 1234u;
  ev.status = 200u;
  ev.bytes = 0u;
  ev.dur_us = 1000u;
  ev.keepalive = 1;
  ev.tls = 0;

  size_t n = access_log_format_text_line(line, sizeof(line), &ev, 256u);

  ASSERT(n > 0);
  ASSERT(strstr(line, "ip=::1") != NULL);
  ASSERT(strstr(line, "port=1234") != NULL);

  PASS();
}

TEST t_format_text_line_missing_ip(void) {
  char line[1024];
  struct access_log_event ev;
  memset(&ev, 0, sizeof(ev));

  ev.ts_ms = 1u;
  ev.worker = 0u;
  ev.method = "GET";
  ev.target = "/";
  ev.target_len = 1;
  ev.remote_ip = NULL;
  ev.remote_port = 0u;
  ev.status = 200u;
  ev.bytes = 0u;
  ev.dur_us = 0u;
  ev.keepalive = 0;
  ev.tls = 0;

  size_t n = access_log_format_text_line(line, sizeof(line), &ev, 256u);

  ASSERT(n > 0);
  ASSERT(strstr(line, "ip=-") != NULL);
  ASSERT(strstr(line, "port=0") != NULL);

  PASS();
}

TEST t_format_text_line_truncates_target(void) {
  char line[512];
  struct access_log_event ev;
  memset(&ev, 0, sizeof(ev));

  ev.ts_ms = 1u;
  ev.worker = 0u;
  ev.method = "GET";
  ev.target = "/abcdef";
  ev.target_len = strlen(ev.target);
  ev.status = 404u;
  ev.bytes = 0u;
  ev.dur_us = 1000u;
  ev.keepalive = 0;
  ev.tls = 1;

  size_t n = access_log_format_text_line(line, sizeof(line), &ev, 4u);

  ASSERT(n > 0);
  ASSERT(strstr(line, "target=/abc") != NULL);
  ASSERT(strstr(line, "trunc=1") != NULL);

  PASS();
}

TEST t_format_text_line_error_bytes_zero(void) {
  char line[512];
  struct access_log_event ev;
  memset(&ev, 0, sizeof(ev));

  ev.ts_ms = 100u;
  ev.worker = 1u;
  ev.method = "POST";
  ev.target = "/";
  ev.target_len = 1;
  ev.remote_ip = "10.0.0.1";
  ev.remote_port = 9999u;
  ev.status = 405u;
  ev.bytes = 0u;
  ev.dur_us = 0u;
  ev.keepalive = 0;
  ev.tls = 0;

  size_t n = access_log_format_text_line(line, sizeof(line), &ev, 256u);

  ASSERT(n > 0);
  ASSERT(strstr(line, "status=405") != NULL);
  ASSERT(strstr(line, "bytes=0 ") != NULL);

  PASS();
}

TEST t_format_text_line_large_bytes(void) {
  char line[512];
  struct access_log_event ev;
  memset(&ev, 0, sizeof(ev));

  ev.ts_ms = 200u;
  ev.worker = 0u;
  ev.method = "GET";
  ev.target = "/big.bin";
  ev.target_len = 8;
  ev.remote_ip = "127.0.0.1";
  ev.remote_port = 12345u;
  ev.status = 200u;
  ev.bytes = 524288ull;
  ev.dur_us = 50000u;
  ev.keepalive = 1;
  ev.tls = 0;

  size_t n = access_log_format_text_line(line, sizeof(line), &ev, 256u);

  ASSERT(n > 0);
  ASSERT(strstr(line, "bytes=524288") != NULL);

  PASS();
}

TEST t_format_text_line_max_bytes(void) {
  char line[512];
  struct access_log_event ev;
  memset(&ev, 0, sizeof(ev));

  ev.ts_ms = 300u;
  ev.worker = 3u;
  ev.method = "GET";
  ev.target = "/max.bin";
  ev.target_len = 8;
  ev.remote_ip = "127.0.0.1";
  ev.remote_port = 23456u;
  ev.status = 200u;
  ev.bytes = UINT64_MAX;
  ev.dur_us = 7000u;
  ev.keepalive = 1;
  ev.tls = 0;

  size_t n = access_log_format_text_line(line, sizeof(line), &ev, 256u);

  ASSERT(n > 0);
  ASSERT(strstr(line, "bytes=18446744073709551615") != NULL);

  PASS();
}

TEST t_emit_from_conn_writes_dur_us_from_runtime_timing(void) {
  char tmpl[] = "/tmp/lamseryn_access_log_ut_XXXXXX";
  int tmpfd = mkstemp(tmpl);
  ASSERT(tmpfd >= 0);
  close(tmpfd);

  int runtime_inited = 0;

  struct globals_cfg g;
  memset(&g, 0, sizeof(g));
  g.present = GF_ACCESS_LOG_ENABLED | GF_ACCESS_LOG_PATH | GF_ACCESS_LOG_FORMAT
              | GF_ACCESS_LOG_SAMPLE | GF_ACCESS_LOG_MIN_STATUS;
  g.access_log_enabled = 1u;
  snprintf(g.access_log_path, sizeof(g.access_log_path), "%s", tmpl);
  snprintf(g.access_log_format, sizeof(g.access_log_format), "text");
  g.access_log_sample = 1u;
  g.access_log_min_status = 100u;
  g.workers = 1u;

  if (access_log_runtime_init(&g) != 0) {
    (void)unlink(tmpl);
    FAILm("access_log_runtime_init failed");
  }
  runtime_inited = 1;

  struct worker_ctx w;
  memset(&w, 0, sizeof(w));
  w.cfg.thread_id = 0;

  struct conn c;
  memset(&c, 0, sizeof(c));
  c.fd = 42;
  c.generation = 7u;
  c.h1.method_set = 1;
  c.h1.method = HTTP_GET;
  c.h1.target = "/dur-us-test";
  c.h1.target_len = (uint16_t)strlen(c.h1.target);
  c.tx.resp_kind = RK_OK_CLOSE;
  c.tx.keepalive = 0;
  c.tx.content_length_hint = 42u;
  snprintf(c.remote_ip, sizeof(c.remote_ip), "%s", "127.0.0.1");
  c.remote_port = 8080u;

  uint64_t now_us = time_now_us_monotonic();
  c.dl.header_start_us = now_us - 5000u;

  access_log_emit_from_conn(&w, &c);
  access_log_runtime_shutdown();
  runtime_inited = 0;

  int fd = open(tmpl, O_RDONLY);
  if (fd < 0) {
    (void)unlink(tmpl);
    FAILm("open emitted access-log file failed");
  }

  char line[2048];
  ssize_t n = read(fd, line, sizeof(line) - 1);
  (void)close(fd);
  (void)unlink(tmpl);

  ASSERT(n > 0);
  line[n] = '\0';

  ASSERT(strstr(line, "method=GET") != NULL);
  ASSERT(strstr(line, "status=200") != NULL);
  ASSERT(strstr(line, "bytes=42") != NULL);

  uint64_t dur_us = 0;
  ASSERT_EQ(parse_u64_field(line, "dur_us=", &dur_us), 0);
  ASSERT(dur_us >= 5000u);
  ASSERT(dur_us < 10000000u);

  if (runtime_inited) {
    access_log_runtime_shutdown();
  }

  PASS();
}

SUITE(access_log_greatest) {
  RUN_TEST(t_cfg_defaults_without_present_bits);
  RUN_TEST(t_sanitize_target_replaces_unsafe_and_marks_truncation);
  RUN_TEST(t_format_text_line_has_expected_fields);
  RUN_TEST(t_format_text_line_ipv6);
  RUN_TEST(t_format_text_line_missing_ip);
  RUN_TEST(t_format_text_line_truncates_target);
  RUN_TEST(t_format_text_line_error_bytes_zero);
  RUN_TEST(t_format_text_line_large_bytes);
  RUN_TEST(t_format_text_line_max_bytes);
  RUN_TEST(t_emit_from_conn_writes_dur_us_from_runtime_timing);
}

GREATEST_MAIN_DEFS();
int main(int argc, char **argv) {
  GREATEST_MAIN_BEGIN();
  RUN_SUITE(access_log_greatest);
  GREATEST_MAIN_END();
}
