// Greatest-based real-llhttp smoke tests for http_pipeline.

#include "../vendor/greatest_color.h"
#include "../vendor/greatest.h"

#include <llhttp.h>
#include <string.h>

#include "http_parser.h"
#include "include/conn.h"
#include "include/http_pipeline.h"

// Minimal callbacks (similar to production)
static int on_message_begin(llhttp_t *p) {
  (void)p;
  return 0;
}

static int on_headers_complete(llhttp_t *p) {
  struct conn *c = (struct conn *)p->data;
  if (c) {
    c->h1.headers_done = 1;
    c->h1.want_keepalive = llhttp_should_keep_alive(p) ? 1 : 0;

    unsigned flags = (unsigned)p->flags;
    c->h1.unsupported_te = 0;
    c->h1.body_remaining = 0;
    c->h1.message_done = 0;
    // Treat chunked as supported by the parser/pipeline layer.
    // Higher-level request handling may still enforce TE policies.
    if ((flags & (unsigned)F_TRANSFER_ENCODING) && !(flags & (unsigned)F_CHUNKED)) {
      c->h1.unsupported_te = 1;
      c->h1.want_keepalive = 0;
      return 0;
    }
    if (flags & (unsigned)F_CONTENT_LENGTH) {
      c->h1.body_remaining = p->content_length;
      c->h1.message_done = (c->h1.body_remaining == 0);
    } else {
      c->h1.body_remaining = 0;
      c->h1.message_done = 1;
    }
  }
  return 0;
}

static int on_message_complete(llhttp_t *p) {
  struct conn *c = (struct conn *)p->data;
  if (c) {
    c->h1.message_done = 1;
  }
  return 0;
}

// Long-lived settings so llhttp can safely reference them
static llhttp_settings_t g_settings;

static void init_conn_parser(struct conn *c) {
  memset(c, 0, sizeof(*c));
  http_parser_settings_assign(&g_settings,
                              on_message_begin,
                              NULL,
                              NULL,
                              NULL,
                              NULL,
                              on_headers_complete,
                              on_message_complete);
  http_parser_init(&c->h1.parser, &g_settings);
  c->h1.parser.data = c;
}

TEST t_zero_len_nop(void) {
  struct conn c;
  init_conn_parser(&c);

  struct http_pipeline_result r = http_pipeline_feed(&c, NULL, 0);
  ASSERT_EQ(r.action, HP_ACTION_CONTINUE);
  ASSERT_EQ((int)c.h1.parser_bytes, 0);
  PASS();
}

TEST t_tolerate_invalid_header_token_until_lf_escalates(void) {
  struct conn c;
  init_conn_parser(&c);

  // Enter header-parsing state
  const char *start = "GET / HTTP/1.1\r\n";
  (void)http_pipeline_feed(&c, start, strlen(start));

  // Invalid header field-name: space in field name (no LF yet)
  const char *bad_no_lf = "Ho st: x";
  struct http_pipeline_result r1 = http_pipeline_feed(&c, bad_no_lf, strlen(bad_no_lf));

  // http_pipeline should tolerate until newline arrives
  ASSERT_EQ(c.h1.parse_error, 0);
  ASSERT_EQ(r1.tolerated_error, 1);
  ASSERT_EQ(r1.action, HP_ACTION_CONTINUE);

  // Now end the line with CRLF, which should trigger escalation to 400
  const char *end_line = "\r\n";
  struct http_pipeline_result r2 = http_pipeline_feed(&c, end_line, 2);

  ASSERT_EQ(c.h1.parse_error, 1);
  ASSERT_EQ(r2.action, HP_ACTION_RESP_400);
  PASS();
}

TEST t_content_length_body_requires_full_read(void) {
  struct conn c;
  init_conn_parser(&c);

  const char *hdrs = "POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\n\r\n";
  struct http_pipeline_result r1 = http_pipeline_feed(&c, hdrs, strlen(hdrs));
  ASSERT_EQ(c.h1.headers_done, 1);
  ASSERT_EQ(c.h1.message_done, 0);
  ASSERT_EQ((int)c.h1.body_remaining, 5);
  ASSERT_EQ(r1.action, HP_ACTION_CONTINUE);

  const char *body = "hello";
  struct http_pipeline_result r2 = http_pipeline_feed(&c, body, 5);
  // In the server, read path decrements body_remaining; here we just need message_done.
  ASSERT_EQ(c.h1.message_done, 1);
  ASSERT_EQ(r2.action, HP_ACTION_RESP_OK);
  PASS();
}

TEST t_draining_431_defers_to_continue(void) {
  struct conn c;
  init_conn_parser(&c);

  // Force state: header too big already detected, but draining is active
  c.h1.header_too_big = 1;
  c.dl.draining = 1;

  char x = 'x';
  struct http_pipeline_result r = http_pipeline_feed(&c, &x, 1);

  // Pipeline should defer to the draining path and keep reading
  ASSERT_EQ(r.action, HP_ACTION_CONTINUE);
  PASS();
}

SUITE(s_http_pipeline_real) {
  RUN_TEST(t_zero_len_nop);
  RUN_TEST(t_tolerate_invalid_header_token_until_lf_escalates);
  RUN_TEST(t_content_length_body_requires_full_read);
  RUN_TEST(t_draining_431_defers_to_continue);
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv) {
  GREATEST_MAIN_BEGIN();
  RUN_SUITE(s_http_pipeline_real);
  GREATEST_MAIN_END();
}
