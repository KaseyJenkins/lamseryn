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
  c->h1.max_body_bytes = (uint64_t)MAX_BODY_BYTES;

  http_parser_settings_assign_server(&g_settings);
  http_parser_init(&c->h1.parser, &g_settings);
  c->h1.parser.data = c;
}

static void attach_route_with_body_limit(struct conn *c,
                                         struct vhost_t *vh,
                                         struct route_policy_rule *rr,
                                         const struct route_policy_rule **route_rules,
                                         const char *prefix,
                                         uint64_t max_body_bytes) {
  memset(vh, 0, sizeof(*vh));
  memset(rr, 0, sizeof(*rr));
  snprintf(rr->path_prefix, sizeof(rr->path_prefix), "%s", prefix);
  rr->path_prefix_len = (uint16_t)strlen(rr->path_prefix);
  rr->max_body_bytes = max_body_bytes;
  rr->max_body_bytes_set = 1u;
  route_rules[0] = rr;
  vh->route_rules = route_rules;
  vh->route_rule_count = 1;
  vh->route_rule_cap = 1;
  c->vhost = vh;
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

TEST t_route_max_body_bytes_allows_content_length_over_default(void) {
  struct conn c;
  struct vhost_t vh;
  struct route_policy_rule rr;
  const struct route_policy_rule *route_rules[1];
  init_conn_parser(&c);
  attach_route_with_body_limit(&c, &vh, &rr, route_rules, "/upload", (uint64_t)MAX_BODY_BYTES + 1u);

  const char *hdrs = "GET /upload HTTP/1.1\r\n"
                     "Host: x\r\n"
                     "Content-Length: 1048577\r\n"
                     "\r\n";

  struct http_pipeline_result r = http_pipeline_feed(&c, hdrs, strlen(hdrs));
  ASSERT_EQ(c.h1.parse_error, 0);
  ASSERT_EQ(c.h1.body_too_big, 0);
  ASSERT_EQ(c.h1.max_body_bytes, (uint64_t)MAX_BODY_BYTES + 1u);
  ASSERT_EQ(c.h1.body_remaining, (uint64_t)MAX_BODY_BYTES + 1u);
  ASSERT_EQ(c.h1.message_done, 0);
  ASSERT_EQ(r.action, HP_ACTION_CONTINUE);

  destroy_conn_parser(&c);
  PASS();
}

TEST t_route_max_body_bytes_rejects_content_length_over_route(void) {
  struct conn c;
  struct vhost_t vh;
  struct route_policy_rule rr;
  const struct route_policy_rule *route_rules[1];
  init_conn_parser(&c);
  attach_route_with_body_limit(&c, &vh, &rr, route_rules, "/login", 4u);

  const char *hdrs = "GET /login HTTP/1.1\r\n"
                     "Host: x\r\n"
                     "Content-Length: 5\r\n"
                     "\r\n";

  struct http_pipeline_result r = http_pipeline_feed(&c, hdrs, strlen(hdrs));
  ASSERT_EQ(c.h1.parse_error, 0);
  ASSERT_EQ(c.h1.body_too_big, 1);
  ASSERT_EQ(c.h1.max_body_bytes, (uint64_t)4u);
  ASSERT_EQ(r.action, HP_ACTION_RESP_413);

  destroy_conn_parser(&c);
  PASS();
}

TEST t_route_max_body_bytes_nonmatch_uses_default(void) {
  struct conn c;
  struct vhost_t vh;
  struct route_policy_rule rr;
  const struct route_policy_rule *route_rules[1];
  init_conn_parser(&c);
  attach_route_with_body_limit(&c, &vh, &rr, route_rules, "/upload", (uint64_t)MAX_BODY_BYTES + 1u);

  const char *hdrs = "GET /other HTTP/1.1\r\n"
                     "Host: x\r\n"
                     "Content-Length: 1048577\r\n"
                     "\r\n";

  struct http_pipeline_result r = http_pipeline_feed(&c, hdrs, strlen(hdrs));
  ASSERT_EQ(c.h1.body_too_big, 1);
  ASSERT_EQ(c.h1.max_body_bytes, (uint64_t)MAX_BODY_BYTES);
  ASSERT_EQ(r.action, HP_ACTION_RESP_413);

  destroy_conn_parser(&c);
  PASS();
}

TEST t_route_max_body_bytes_rejects_chunked_over_route(void) {
  struct conn c;
  struct vhost_t vh;
  struct route_policy_rule rr;
  const struct route_policy_rule *route_rules[1];
  init_conn_parser(&c);
  attach_route_with_body_limit(&c, &vh, &rr, route_rules, "/login", 4u);

  const char *hdrs = "GET /login HTTP/1.1\r\n"
                     "Host: x\r\n"
                     "Transfer-Encoding: chunked\r\n"
                     "\r\n";
  struct http_pipeline_result r1 = http_pipeline_feed(&c, hdrs, strlen(hdrs));
  ASSERT_EQ(c.h1.body_too_big, 0);
  ASSERT_EQ(c.h1.max_body_bytes, (uint64_t)4u);
  ASSERT_EQ(r1.action, HP_ACTION_CONTINUE);

  const char *chunk = "5\r\nhello\r\n0\r\n\r\n";
  struct http_pipeline_result r2 = http_pipeline_feed(&c, chunk, strlen(chunk));
  ASSERT_EQ(c.h1.body_too_big, 1);
  ASSERT_EQ(r2.action, HP_ACTION_RESP_413);

  destroy_conn_parser(&c);
  PASS();
}

TEST t_route_max_body_bytes_more_specific_route_wins(void) {
  struct conn c;
  struct vhost_t vh;
  struct route_policy_rule upload;
  struct route_policy_rule tiny;
  const struct route_policy_rule *route_rules[2];
  init_conn_parser(&c);
  memset(&vh, 0, sizeof(vh));
  memset(&upload, 0, sizeof(upload));
  memset(&tiny, 0, sizeof(tiny));
  snprintf(tiny.path_prefix, sizeof(tiny.path_prefix), "%s", "/upload/tiny");
  tiny.path_prefix_len = (uint16_t)strlen(tiny.path_prefix);
  tiny.max_body_bytes = 4u;
  tiny.max_body_bytes_set = 1u;
  snprintf(upload.path_prefix, sizeof(upload.path_prefix), "%s", "/upload");
  upload.path_prefix_len = (uint16_t)strlen(upload.path_prefix);
  upload.max_body_bytes = 100u;
  upload.max_body_bytes_set = 1u;
  route_rules[0] = &tiny;
  route_rules[1] = &upload;
  vh.route_rules = route_rules;
  vh.route_rule_count = 2;
  vh.route_rule_cap = 2;
  c.vhost = &vh;

  const char *hdrs = "GET /upload/tiny/file HTTP/1.1\r\n"
                     "Host: x\r\n"
                     "Content-Length: 5\r\n"
                     "\r\n";
  struct http_pipeline_result r = http_pipeline_feed(&c, hdrs, strlen(hdrs));
  ASSERT_EQ(c.h1.max_body_bytes, (uint64_t)4u);
  ASSERT_EQ(c.h1.body_too_big, 1);
  ASSERT_EQ(r.action, HP_ACTION_RESP_413);

  destroy_conn_parser(&c);
  PASS();
}

SUITE(s_http_parser_expect) {
  RUN_TEST(t_expect_100_continue_header_only_ok);
  RUN_TEST(t_expect_100_continue_content_length_body_ok);
  RUN_TEST(t_expect_unsupported_rejected_400);
  RUN_TEST(t_expect_mixed_case_100_continue_ok);
  RUN_TEST(t_expect_mixed_token_list_rejected_400);
  RUN_TEST(t_route_max_body_bytes_allows_content_length_over_default);
  RUN_TEST(t_route_max_body_bytes_rejects_content_length_over_route);
  RUN_TEST(t_route_max_body_bytes_nonmatch_uses_default);
  RUN_TEST(t_route_max_body_bytes_rejects_chunked_over_route);
  RUN_TEST(t_route_max_body_bytes_more_specific_route_wins);
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv) {
  GREATEST_MAIN_BEGIN();
  RUN_SUITE(s_http_parser_expect);
  GREATEST_MAIN_END();
}
