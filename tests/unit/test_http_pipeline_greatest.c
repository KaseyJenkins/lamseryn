// Greatest-based tests for http_pipeline using the real llhttp parser.
#include "../vendor/greatest_color.h"
#include "../vendor/greatest.h"

#include "include/http_pipeline.h"
#include "include/conn.h"
#include "include/http_parser.h"
#include <llhttp.h>
#include <string.h>

// Minimal callbacks: set headers_done and want_keepalive at headers-complete.
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
    if (flags & (unsigned)F_CHUNKED) {
      c->h1.unsupported_te = 1;
      c->h1.want_keepalive = 0;
      return 0;
    }
    if (flags & (unsigned)F_TRANSFER_ENCODING) {
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

// Long-lived settings so llhttp can safely reference them.
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

// Existing baseline
TEST t_headers_complete_ok(void) {
  struct conn c;
  init_conn_parser(&c);

  const char *req = "GET / HTTP/1.1\r\nHost: x\r\n\r\n";
  struct http_pipeline_result r = http_pipeline_feed(&c, req, strlen(req));

  ASSERT_EQ(c.h1.headers_done, 1);
  ASSERT_EQ(c.h1.message_done, 1);
  ASSERT_EQ(r.headers_complete_transition, 1);
  ASSERT_EQ(r.action, HP_ACTION_RESP_OK);
  PASS();
}

TEST t_exactly_at_cap_is_ok(void) {
  struct conn c;
  init_conn_parser(&c);

  const char *reqline = "GET / HTTP/1.1\r\n";
  (void)http_pipeline_feed(&c, reqline, strlen(reqline));

  // Next chunk lands exactly on HEADER_CAP.
  const size_t n = 6; // "X: y\r\n"
  ASSERT(HEADER_CAP > n);
  c.h1.parser_bytes = HEADER_CAP - n;

  struct http_pipeline_result r = http_pipeline_feed(&c, "X: y\r\n", n);

  ASSERT_EQ(c.h1.header_too_big, 0);
  ASSERT_EQ(c.h1.headers_done, 0);
  ASSERT_EQ(r.action, HP_ACTION_CONTINUE);
  PASS();
}

// HTTP/1.1 default keep-alive (RESP_OK + want_keepalive=1)
TEST t_keepalive_default_http11(void) {
  struct conn c;
  init_conn_parser(&c);
  const char *req = "GET / HTTP/1.1\r\nHost: x\r\n\r\n";
  struct http_pipeline_result r = http_pipeline_feed(&c, req, strlen(req));
  ASSERT_EQ(c.h1.headers_done, 1);
  ASSERT_EQ(c.h1.message_done, 1);
  ASSERT_EQ(c.h1.want_keepalive, 1);
  ASSERT_EQ(r.action, HP_ACTION_RESP_OK);
  PASS();
}

// HTTP/1.0 default close, explicit close header
TEST t_close_default_http10(void) {
  struct conn c;
  init_conn_parser(&c);
  const char *req = "GET / HTTP/1.0\r\nConnection: close\r\n\r\n";
  struct http_pipeline_result r = http_pipeline_feed(&c, req, strlen(req));
  ASSERT_EQ(c.h1.headers_done, 1);
  ASSERT_EQ(c.h1.message_done, 1);
  ASSERT_EQ(c.h1.want_keepalive, 0);
  ASSERT_EQ(r.action, HP_ACTION_RESP_OK);
  PASS();
}

// HTTP/1.0 but keep-alive requested
TEST t_keepalive_http10_with_header(void) {
  struct conn c;
  init_conn_parser(&c);
  const char *req = "GET / HTTP/1.0\r\nConnection: keep-alive\r\n\r\n";
  struct http_pipeline_result r = http_pipeline_feed(&c, req, strlen(req));
  ASSERT_EQ(c.h1.headers_done, 1);
  ASSERT_EQ(c.h1.message_done, 1);
  ASSERT_EQ(c.h1.want_keepalive, 1);
  ASSERT_EQ(r.action, HP_ACTION_RESP_OK);
  PASS();
}

// CR_EXPECTED/LF_EXPECTED are tolerated until next chunk completes the line.
TEST t_tolerate_incomplete_crlf_then_ok(void) {
  struct conn c;
  init_conn_parser(&c);

  const char *start = "GET / HTTP/1.1\r\nHost: value\r"; // ends at CR
  struct http_pipeline_result r1 = http_pipeline_feed(&c, start, strlen(start));
  // Expect tolerated error (+ CONTINUE), no parse_error latched
  ASSERT_EQ(c.h1.parse_error, 0);
  ASSERT_EQ(r1.tolerated_error, 0); // llhttp expects more data, not an error
  ASSERT_EQ(r1.action, HP_ACTION_CONTINUE);

  // Provide LF to complete the header line, then end of headers
  const char *end = "\n\r\n";
  struct http_pipeline_result r2 = http_pipeline_feed(&c, end, strlen(end));
  ASSERT_EQ(c.h1.headers_done, 1);
  ASSERT_EQ(c.h1.message_done, 1);
  ASSERT_EQ(r2.headers_complete_transition, 1);
  ASSERT_EQ(r2.action, HP_ACTION_RESP_OK);
  PASS();
}

// invalid header token WITH newline in the same chunk: first detection is tolerated.
TEST t_invalid_token_same_chunk_with_lf_is_tolerated_once(void) {
  struct conn c;
  init_conn_parser(&c);

  const char *reqline = "GET / HTTP/1.1\r\n";
  (void)http_pipeline_feed(&c, reqline, strlen(reqline));

  // Space in field-name → INVALID_HEADER_TOKEN; newline arrives in same chunk.
  const char *bad = "Ho st: x\r\n";
  struct http_pipeline_result r = http_pipeline_feed(&c, bad, strlen(bad));
  // Current behavior: first detection sets pending_line_error and tolerates
  ASSERT_EQ(c.h1.parse_error, 0);
  ASSERT_EQ(r.tolerated_error, 1);
  ASSERT_EQ(r.action, HP_ACTION_CONTINUE);
  PASS();
}

// multi-chunk header growth to exceed HEADER_CAP → 431
TEST t_header_cap_431_multi_chunks(void) {
  struct conn c;
  init_conn_parser(&c);

  const char *reqline = "GET / HTTP/1.1\r\n";
  (void)http_pipeline_feed(&c, reqline, strlen(reqline));

  // Position so that the next small chunk pushes us over HEADER_CAP.
  size_t bump = 16;
  ASSERT(HEADER_CAP > bump);
  c.h1.parser_bytes = HEADER_CAP - 1;
  char buf[16];
  memset(buf, 'A', sizeof(buf));

  struct http_pipeline_result r = http_pipeline_feed(&c, buf, sizeof(buf));
  ASSERT_EQ(c.h1.header_too_big, 1);
  ASSERT_EQ(r.header_too_big_transition, 1);
  ASSERT_EQ(r.action, HP_ACTION_RESP_431);
  PASS();
}

// idempotence once headers_done is latched
TEST t_idempotent_after_headers_done(void) {
  struct conn c;
  init_conn_parser(&c);

  const char *req = "GET / HTTP/1.1\r\nHost: x\r\n\r\n";
  struct http_pipeline_result r1 = http_pipeline_feed(&c, req, strlen(req));
  ASSERT_EQ(c.h1.headers_done, 1);
  ASSERT_EQ(c.h1.message_done, 1);
  ASSERT_EQ(r1.action, HP_ACTION_RESP_OK);

  // Feed extra bytes (pretend body/noise); pipeline answers RESP_OK again
  const char *extra = "ignored-body";
  struct http_pipeline_result r2 = http_pipeline_feed(&c, extra, strlen(extra));
  ASSERT_EQ(r2.headers_complete_transition, 0);
  ASSERT_EQ(r2.action, HP_ACTION_RESP_OK);
  PASS();
}

// Content-Length body must be fully received before RESP_OK.
TEST t_content_length_body_requires_full_read(void) {
  struct conn c;
  init_conn_parser(&c);

  const char *hdrs = "POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\n\r\n";
  struct http_pipeline_result r1 = http_pipeline_feed(&c, hdrs, strlen(hdrs));
  ASSERT_EQ(c.h1.headers_done, 1);
  ASSERT_EQ(c.h1.message_done, 0);
  ASSERT_EQ(r1.action, HP_ACTION_CONTINUE);

  const char *body = "hello";
  struct http_pipeline_result r2 = http_pipeline_feed(&c, body, 5);
  ASSERT_EQ(c.h1.message_done, 1);
  ASSERT_EQ(r2.action, HP_ACTION_RESP_OK);
  PASS();
}

// --- Cap dance: precise boundaries ---

TEST t_cap_minus_one_then_one_is_ok(void) {
  struct conn c;
  init_conn_parser(&c);

  const char *reqline = "GET / HTTP/1.1\r\n";
  (void)http_pipeline_feed(&c, reqline, strlen(reqline));

  // Land at HEADER_CAP - 1, then feed 1 byte to reach exactly CAP (OK)
  ASSERT(HEADER_CAP > 1);
  c.h1.parser_bytes = HEADER_CAP - 1;

  char one = 'A';
  struct http_pipeline_result r = http_pipeline_feed(&c, &one, 1);
  ASSERT_EQ(c.h1.header_too_big, 0);
  ASSERT_EQ(r.action, HP_ACTION_CONTINUE);
  PASS();
}

TEST t_cap_plus_one_is_431(void) {
  struct conn c;
  init_conn_parser(&c);

  const char *reqline = "GET / HTTP/1.1\r\n";
  (void)http_pipeline_feed(&c, reqline, strlen(reqline));

  // Exactly at CAP, then feed 1 → should flip 431 immediately
  c.h1.parser_bytes = HEADER_CAP;
  char one = 'A';
  struct http_pipeline_result r = http_pipeline_feed(&c, &one, 1);
  ASSERT_EQ(c.h1.header_too_big, 1);
  ASSERT_EQ(r.header_too_big_transition, 1);
  ASSERT_EQ(r.action, HP_ACTION_RESP_431);
  PASS();
}

// --- Sticky invalid header token across multiple chunks ---

TEST t_sticky_invalid_token_multichunk_then_400(void) {
  struct conn c;
  init_conn_parser(&c);

  const char *start = "GET / HTTP/1.1\r\n";
  (void)http_pipeline_feed(&c, start, strlen(start));

  // Chunk 1: invalid field-name (space) without LF
  const char *bad1 = "Ho st:";
  struct http_pipeline_result r1 = http_pipeline_feed(&c, bad1, strlen(bad1));
  ASSERT_EQ(c.h1.parse_error, 0);
  ASSERT_EQ(r1.tolerated_error, 1);
  ASSERT_EQ(r1.action, HP_ACTION_CONTINUE);

  // Chunk 2: still same line, still no LF → keep tolerating
  const char *bad2 = " x";
  struct http_pipeline_result r2 = http_pipeline_feed(&c, bad2, strlen(bad2));
  ASSERT_EQ(c.h1.parse_error, 0);
  ASSERT_EQ(r2.tolerated_error, 1);
  ASSERT_EQ(r2.action, HP_ACTION_CONTINUE);

  // Now terminate the line → escalate to 400
  const char *end = "\r\n";
  struct http_pipeline_result r3 = http_pipeline_feed(&c, end, 2);
  ASSERT_EQ(c.h1.parse_error, 1);
  ASSERT_EQ(r3.action, HP_ACTION_RESP_400);
  PASS();
}

// --- Mirror selected “real” tests for symmetry ---

TEST t_zero_len_nop_greatest(void) {
  struct conn c;
  init_conn_parser(&c);
  struct http_pipeline_result r = http_pipeline_feed(&c, NULL, 0);
  ASSERT_EQ(r.action, HP_ACTION_CONTINUE);
  ASSERT_EQ((int)c.h1.parser_bytes, 0);
  PASS();
}

TEST t_draining_431_defers_to_continue_greatest(void) {
  struct conn c;
  init_conn_parser(&c);

  // Force the state: header too big already detected, and draining is active
  c.h1.header_too_big = 1;
  c.dl.draining = 1;

  char x = 'x';
  struct http_pipeline_result r = http_pipeline_feed(&c, &x, 1);
  ASSERT_EQ(r.action, HP_ACTION_CONTINUE);
  PASS();
}

// if body_too_big is latched by higher-level parsing, pipeline emits 413.
TEST t_body_too_big_maps_to_413(void) {
  struct conn c;
  init_conn_parser(&c);
  c.h1.body_too_big = 1;
  char x = 'x';
  struct http_pipeline_result r = http_pipeline_feed(&c, &x, 1);
  ASSERT_EQ(r.action, HP_ACTION_RESP_413);
  PASS();
}

TEST t_ok_plan_method_not_allowed_maps_405_close(void) {
  struct conn c;
  init_conn_parser(&c);
  c.h1.method_not_allowed = 1;
  c.h1.want_keepalive = 1;

  struct http_ok_plan p = http_pipeline_ok_plan(&c);
  ASSERT_EQ(p.kind, RK_405);
  ASSERT_EQ(p.keepalive, 0);
  ASSERT_EQ(p.close_after_send, 1);
  PASS();
}

TEST t_ok_plan_default_keepalive_maps_ok_ka(void) {
  struct conn c;
  init_conn_parser(&c);
  c.h1.method_not_allowed = 0;
  c.h1.want_keepalive = 1;

  struct http_ok_plan p = http_pipeline_ok_plan(&c);
  ASSERT_EQ(p.kind, RK_OK_KA);
  ASSERT_EQ(p.keepalive, 1);
  ASSERT_EQ(p.close_after_send, 0);
  PASS();
}

TEST t_internal_error_plan_maps_500_close(void) {
  struct http_error_plan p;
  memset(&p, 0, sizeof(p));

  http_pipeline_internal_error_plan(&p);

  ASSERT_EQ(p.kind, RK_500);
  ASSERT_EQ(p.keepalive, 0);
  ASSERT_EQ(p.drain_after_headers, 0);
  ASSERT_EQ(p.close_after_send, 1);
  PASS();
}

TEST t_apply_plan_internal_error_precedes_action(void) {
  struct conn c;
  init_conn_parser(&c);
  c.h1.internal_error = 1;

  struct http_pipeline_result r;
  memset(&r, 0, sizeof(r));
  r.action = HP_ACTION_RESP_400;

  struct http_apply_plan p = http_pipeline_build_apply_plan(&c, &r);
  ASSERT_EQ(p.kind, HP_APPLY_INTERNAL_ERROR);
  ASSERT_EQ(p.error.kind, RK_500);
  ASSERT_EQ(p.error.close_after_send, 1);
  PASS();
}

TEST t_apply_plan_error_mapping_uses_error_plan(void) {
  struct conn c;
  init_conn_parser(&c);

  struct http_pipeline_result r;
  memset(&r, 0, sizeof(r));
  r.action = HP_ACTION_RESP_431;

  struct http_apply_plan p = http_pipeline_build_apply_plan(&c, &r);
  ASSERT_EQ(p.kind, HP_APPLY_ERROR);
  ASSERT_EQ(p.error.kind, RK_431);
  ASSERT_EQ(p.error.drain_after_headers, 1);
  PASS();
}

TEST t_apply_plan_ok_mapping_uses_ok_plan(void) {
  struct conn c;
  init_conn_parser(&c);
  c.h1.want_keepalive = 1;

  struct http_pipeline_result r;
  memset(&r, 0, sizeof(r));
  r.action = HP_ACTION_RESP_OK;

  struct http_apply_plan p = http_pipeline_build_apply_plan(&c, &r);
  ASSERT_EQ(p.kind, HP_APPLY_OK);
  ASSERT_EQ(p.reschedule_on_ok, 1);
  ASSERT_EQ(p.ok.kind, RK_OK_KA);
  PASS();
}

SUITE(s_http_pipeline) {
  RUN_TEST(t_headers_complete_ok);
  RUN_TEST(t_exactly_at_cap_is_ok);

  RUN_TEST(t_keepalive_default_http11);
  RUN_TEST(t_close_default_http10);
  RUN_TEST(t_keepalive_http10_with_header);

  RUN_TEST(t_tolerate_incomplete_crlf_then_ok);
  RUN_TEST(t_invalid_token_same_chunk_with_lf_is_tolerated_once);

  RUN_TEST(t_header_cap_431_multi_chunks);
  RUN_TEST(t_idempotent_after_headers_done);

  RUN_TEST(t_content_length_body_requires_full_read);

  RUN_TEST(t_cap_minus_one_then_one_is_ok);
  RUN_TEST(t_cap_plus_one_is_431);
  RUN_TEST(t_sticky_invalid_token_multichunk_then_400);
  RUN_TEST(t_zero_len_nop_greatest);
  RUN_TEST(t_draining_431_defers_to_continue_greatest);
  RUN_TEST(t_body_too_big_maps_to_413);
  RUN_TEST(t_ok_plan_method_not_allowed_maps_405_close);
  RUN_TEST(t_ok_plan_default_keepalive_maps_ok_ka);
  RUN_TEST(t_internal_error_plan_maps_500_close);
  RUN_TEST(t_apply_plan_internal_error_precedes_action);
  RUN_TEST(t_apply_plan_error_mapping_uses_error_plan);
  RUN_TEST(t_apply_plan_ok_mapping_uses_ok_plan);
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv) {
  GREATEST_MAIN_BEGIN(); // supports -f filter, -v verbose, etc.
  RUN_SUITE(s_http_pipeline);
  GREATEST_MAIN_END();
}
