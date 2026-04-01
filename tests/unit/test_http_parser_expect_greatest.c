// Greatest-based tests for Expect header handling via server parser callbacks.
#include "../vendor/greatest_color.h"
#include "../vendor/greatest.h"

#include "include/conn.h"
#include "include/http_pipeline.h"
#include "http_parser.h"

#include <string.h>

static llhttp_settings_t g_settings;

static void init_conn_parser(struct conn *c) {
  memset(c, 0, sizeof(*c));
  req_arena_init(&c->h1.arena);
  c->h1.hdr_fields_max = 100;

  http_parser_settings_assign_server(&g_settings);
  http_parser_init(&c->h1.parser, &g_settings);
  c->h1.parser.data = c;
}

static void destroy_conn_parser(struct conn *c) {
  req_arena_destroy(&c->h1.arena);
}

TEST t_expect_100_continue_header_only_ok(void) {
  struct conn c;
  init_conn_parser(&c);

  const char *req = "GET / HTTP/1.1\r\n"
                    "Host: x\r\n"
                    "Expect: 100-continue\r\n"
                    "\r\n";

  struct http_pipeline_result r = http_pipeline_feed(&c, req, strlen(req));
  ASSERT_EQ(c.h1.parse_error, 0);
  ASSERT_EQ(c.h1.expect_100_continue, 1);
  ASSERT_EQ(c.h1.expect_unsupported, 0);
  ASSERT_EQ(r.action, HP_ACTION_RESP_OK);

  destroy_conn_parser(&c);
  PASS();
}

TEST t_expect_100_continue_content_length_body_ok(void) {
  struct conn c;
  init_conn_parser(&c);

  const char *hdrs = "GET / HTTP/1.1\r\n"
                     "Host: x\r\n"
                     "Expect: 100-continue\r\n"
                     "Content-Length: 4\r\n"
                     "\r\n";

  struct http_pipeline_result r1 = http_pipeline_feed(&c, hdrs, strlen(hdrs));
  ASSERT_EQ(c.h1.parse_error, 0);
  ASSERT_EQ(c.h1.expect_100_continue, 1);
  ASSERT_EQ(c.h1.message_done, 0);
  ASSERT_EQ(r1.action, HP_ACTION_CONTINUE);

  const char *body = "test";
  struct http_pipeline_result r2 = http_pipeline_feed(&c, body, 4);
  ASSERT_EQ(c.h1.parse_error, 0);
  ASSERT_EQ(c.h1.message_done, 1);
  ASSERT_EQ(r2.action, HP_ACTION_RESP_OK);

  destroy_conn_parser(&c);
  PASS();
}

TEST t_expect_unsupported_rejected_400(void) {
  struct conn c;
  init_conn_parser(&c);

  const char *req = "GET / HTTP/1.1\r\n"
                    "Host: x\r\n"
                    "Expect: something-else\r\n"
                    "\r\n";

  struct http_pipeline_result r = http_pipeline_feed(&c, req, strlen(req));
  ASSERT_EQ(c.h1.expect_unsupported, 1);
  ASSERT_EQ(c.h1.parse_error, 1);
  ASSERT_EQ(c.h1.want_keepalive, 0);
  ASSERT_EQ(r.action, HP_ACTION_RESP_400);

  destroy_conn_parser(&c);
  PASS();
}

TEST t_expect_mixed_case_100_continue_ok(void) {
  struct conn c;
  init_conn_parser(&c);

  const char *req = "GET / HTTP/1.1\r\n"
                    "Host: x\r\n"
                    "eXpEcT: 100-CoNtInUe\r\n"
                    "\r\n";

  struct http_pipeline_result r = http_pipeline_feed(&c, req, strlen(req));
  ASSERT_EQ(c.h1.parse_error, 0);
  ASSERT_EQ(c.h1.expect_100_continue, 1);
  ASSERT_EQ(c.h1.expect_unsupported, 0);
  ASSERT_EQ(r.action, HP_ACTION_RESP_OK);

  destroy_conn_parser(&c);
  PASS();
}

TEST t_expect_mixed_token_list_rejected_400(void) {
  struct conn c;
  init_conn_parser(&c);

  const char *req = "GET / HTTP/1.1\r\n"
                    "Host: x\r\n"
                    "Expect: 100-continue, foo\r\n"
                    "\r\n";

  struct http_pipeline_result r = http_pipeline_feed(&c, req, strlen(req));
  ASSERT_EQ(c.h1.expect_unsupported, 1);
  ASSERT_EQ(c.h1.parse_error, 1);
  ASSERT_EQ(r.action, HP_ACTION_RESP_400);

  destroy_conn_parser(&c);
  PASS();
}

SUITE(s_http_parser_expect) {
  RUN_TEST(t_expect_100_continue_header_only_ok);
  RUN_TEST(t_expect_100_continue_content_length_body_ok);
  RUN_TEST(t_expect_unsupported_rejected_400);
  RUN_TEST(t_expect_mixed_case_100_continue_ok);
  RUN_TEST(t_expect_mixed_token_list_rejected_400);
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv) {
  GREATEST_MAIN_BEGIN();
  RUN_SUITE(s_http_parser_expect);
  GREATEST_MAIN_END();
}
