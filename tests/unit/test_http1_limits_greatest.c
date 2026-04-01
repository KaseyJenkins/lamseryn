// Greatest-based tests for the LimitRequestFields-style header field cap.
#include "../vendor/greatest_color.h"
#include "../vendor/greatest.h"

#include "include/conn.h"
#include "include/http1_limits.h"
#include "include/http_parser.h"

#include <string.h>

static llhttp_settings_t g_settings;

static void init_conn(struct conn *c) {
  memset(c, 0, sizeof(*c));
  memset(&g_settings, 0, sizeof(g_settings));
  http_parser_init(&c->h1.parser, &g_settings);
  c->h1.parser.data = c;
}

TEST t_under_limit_ok(void) {
  struct conn c;
  init_conn(&c);

  c.h1.hdr_fields_max = 3;
  c.h1.want_keepalive = 1;

  for (int i = 0; i < 3; ++i) {
    c.h1.hdr_name_len = 1;
    http1_header_field_seen(&c);
    ASSERT_EQ(c.h1.header_too_big, 0);
    ASSERT_EQ(c.h1.header_fields_too_many, 0);
    ASSERT_EQ(c.h1.hdr_fields_count, (uint16_t)(i + 1));
  }

  PASS();
}

TEST t_over_limit_sets_431(void) {
  struct conn c;
  init_conn(&c);

  c.h1.hdr_fields_max = 2;
  c.h1.want_keepalive = 1;

  c.h1.hdr_name_len = 1;
  http1_header_field_seen(&c);
  ASSERT_EQ(c.h1.header_too_big, 0);

  c.h1.hdr_name_len = 1;
  http1_header_field_seen(&c);
  ASSERT_EQ(c.h1.header_too_big, 0);

  c.h1.hdr_name_len = 1;
  http1_header_field_seen(&c);
  ASSERT_EQ(c.h1.header_too_big, 1);
  ASSERT_EQ(c.h1.header_fields_too_many, 1);
  ASSERT_EQ(c.h1.want_keepalive, 0);
  ASSERT_EQ(c.h1.hdr_fields_count, (uint16_t)3);

  PASS();
}

TEST t_empty_name_not_counted(void) {
  struct conn c;
  init_conn(&c);

  c.h1.hdr_fields_max = 1;
  c.h1.hdr_name_len = 0;
  http1_header_field_seen(&c);

  ASSERT_EQ(c.h1.hdr_fields_count, 0);
  ASSERT_EQ(c.h1.header_too_big, 0);
  PASS();
}

TEST t_no_increment_after_too_big(void) {
  struct conn c;
  init_conn(&c);

  c.h1.hdr_fields_max = 1;
  c.h1.hdr_name_len = 1;
  http1_header_field_seen(&c);
  ASSERT_EQ(c.h1.header_too_big, 0);

  c.h1.hdr_name_len = 1;
  http1_header_field_seen(&c);
  ASSERT_EQ(c.h1.header_too_big, 1);
  uint16_t before = c.h1.hdr_fields_count;

  c.h1.hdr_name_len = 1;
  http1_header_field_seen(&c);
  ASSERT_EQ(c.h1.hdr_fields_count, before);

  PASS();
}

SUITE(s_http1_limits) {
  RUN_TEST(t_under_limit_ok);
  RUN_TEST(t_over_limit_sets_431);
  RUN_TEST(t_empty_name_not_counted);
  RUN_TEST(t_no_increment_after_too_big);
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv) {
  GREATEST_MAIN_BEGIN();
  RUN_SUITE(s_http1_limits);
  GREATEST_MAIN_END();
}
