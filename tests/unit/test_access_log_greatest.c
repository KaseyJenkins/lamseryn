#include "../vendor/greatest_color.h"
#include "../vendor/greatest.h"

#include "include/access_log.h"

#include <string.h>

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
  ev.status = 200u;
  ev.bytes = 1234ull;
  ev.dur_ms = 3u;
  ev.keepalive = 1;
  ev.tls = 0;

  size_t n = access_log_format_text_line(line, sizeof(line), &ev, 256u);

  ASSERT(n > 0);
  ASSERT(strstr(line, "ts_ms=1710956000123") != NULL);
  ASSERT(strstr(line, "worker=2") != NULL);
  ASSERT(strstr(line, "method=GET") != NULL);
  ASSERT(strstr(line, "target=/index.html?x=1") != NULL);
  ASSERT(strstr(line, "status=200") != NULL);
  ASSERT(strstr(line, "bytes=1234") != NULL);
  ASSERT(strstr(line, "dur_ms=3") != NULL);
  ASSERT(strstr(line, "ka=1") != NULL);
  ASSERT(strstr(line, "tls=0") != NULL);
  ASSERT(strstr(line, "trunc=0") != NULL);
  ASSERT(line[n - 1] == '\n');

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
  ev.dur_ms = 1u;
  ev.keepalive = 0;
  ev.tls = 1;

  size_t n = access_log_format_text_line(line, sizeof(line), &ev, 4u);

  ASSERT(n > 0);
  ASSERT(strstr(line, "target=/abc") != NULL);
  ASSERT(strstr(line, "trunc=1") != NULL);

  PASS();
}

SUITE(access_log_greatest) {
  RUN_TEST(t_cfg_defaults_without_present_bits);
  RUN_TEST(t_sanitize_target_replaces_unsafe_and_marks_truncation);
  RUN_TEST(t_format_text_line_has_expected_fields);
  RUN_TEST(t_format_text_line_truncates_target);
}

GREATEST_MAIN_DEFS();
int main(int argc, char **argv) {
  GREATEST_MAIN_BEGIN();
  RUN_SUITE(access_log_greatest);
  GREATEST_MAIN_END();
}
