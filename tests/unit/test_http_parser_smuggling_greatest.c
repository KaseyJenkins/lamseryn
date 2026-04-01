// Table-driven adversarial parser corpus for HTTP/1.1 smuggling/desync classes.
#include "../vendor/greatest_color.h"
#include "../vendor/greatest.h"

#include "include/conn.h"
#include "include/http_pipeline.h"
#include "http_parser.h"

#include <string.h>

struct corpus_case {
  const char *name;
  const char *req;
  enum http_action want_action;
  int want_close_after_send;
  int want_drain_after_headers;
  uint16_t hdr_fields_max;
};

static llhttp_settings_t g_settings;

static void init_conn_parser(struct conn *c, uint16_t hdr_fields_max) {
  memset(c, 0, sizeof(*c));
  req_arena_init(&c->h1.arena);
  c->h1.hdr_fields_max = hdr_fields_max ? hdr_fields_max : 100;

  http_parser_settings_assign_server(&g_settings);
  http_parser_init(&c->h1.parser, &g_settings);
  c->h1.parser.data = c;
}

static void destroy_conn_parser(struct conn *c) {
  req_arena_destroy(&c->h1.arena);
}

TEST t_smuggling_adversarial_corpus(void) {
  static const struct corpus_case cases[] = {
    {
      .name = "cl_plus_sign_invalid",
      .req = "GET / HTTP/1.1\r\n"
             "Host: x\r\n"
             "Content-Length: +5\r\n"
             "Connection: close\r\n"
             "\r\n",
      .want_action = HP_ACTION_RESP_400,
      .want_close_after_send = 1,
      .want_drain_after_headers = 0,
    },
    {
      .name = "cl_embedded_space_invalid",
      .req = "GET / HTTP/1.1\r\n"
             "Host: x\r\n"
             "Content-Length: 1 2\r\n"
             "Connection: close\r\n"
             "\r\n",
      .want_action = HP_ACTION_RESP_400,
      .want_close_after_send = 1,
      .want_drain_after_headers = 0,
    },
    {
      .name = "te_chunked_parameter_unsupported",
      .req = "GET / HTTP/1.1\r\n"
             "Host: x\r\n"
             "Transfer-Encoding: chunked;foo=bar\r\n"
             "Connection: close\r\n"
             "\r\n",
      .want_action = HP_ACTION_RESP_501,
      .want_close_after_send = 1,
      .want_drain_after_headers = 0,
    },
    {
      .name = "te_space_separated_tokens_unsupported",
      .req = "GET / HTTP/1.1\r\n"
             "Host: x\r\n"
             "Transfer-Encoding: chunked gzip\r\n"
             "Connection: close\r\n"
             "\r\n",
      .want_action = HP_ACTION_RESP_501,
      .want_close_after_send = 1,
      .want_drain_after_headers = 0,
    },
    {
      .name = "te_chunked_identity_unsupported",
      .req = "GET / HTTP/1.1\r\n"
             "Host: x\r\n"
             "Transfer-Encoding: chunked, identity\r\n"
             "Connection: close\r\n"
             "\r\n",
      .want_action = HP_ACTION_RESP_400,
      .want_close_after_send = 1,
      .want_drain_after_headers = 0,
    },
    {
      .name = "te_double_comma_malformed_unsupported",
      .req = "GET / HTTP/1.1\r\n"
             "Host: x\r\n"
             "Transfer-Encoding: chunked,,gzip\r\n"
             "Connection: close\r\n"
             "\r\n",
      .want_action = HP_ACTION_RESP_400,
      .want_close_after_send = 1,
      .want_drain_after_headers = 0,
    },
    {
      .name = "path_encoded_slash_rejected",
      .req = "GET /a%2fb HTTP/1.1\r\n"
             "Host: x\r\n"
             "Connection: close\r\n"
             "\r\n",
      .want_action = HP_ACTION_RESP_400,
      .want_close_after_send = 1,
      .want_drain_after_headers = 0,
    },
    {
      .name = "path_bad_percent_encoding_rejected",
      .req = "GET /%zz HTTP/1.1\r\n"
             "Host: x\r\n"
             "Connection: close\r\n"
             "\r\n",
      .want_action = HP_ACTION_RESP_400,
      .want_close_after_send = 1,
      .want_drain_after_headers = 0,
    },
    {
      .name = "path_dot_segments_root_escape_normalized_ok",
      .req = "GET /../../etc/passwd HTTP/1.1\r\n"
             "Host: x\r\n"
             "Connection: close\r\n"
             "\r\n",
      .want_action = HP_ACTION_RESP_OK,
    },
    {
      .name = "path_dot_segments_mixed_normalized_ok",
      .req = "GET /a/.././../b HTTP/1.1\r\n"
             "Host: x\r\n"
             "Connection: close\r\n"
             "\r\n",
      .want_action = HP_ACTION_RESP_OK,
    },
    {
      .name = "header_field_cap_431",
      .req = "GET / HTTP/1.1\r\n"
             "Host: x\r\n"
             "X-A: 1\r\n"
             "Connection: close\r\n"
             "\r\n",
      .want_action = HP_ACTION_RESP_431,
      .want_close_after_send = 0,
      .want_drain_after_headers = 1,
      .hdr_fields_max = 1,
    },
  };

  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
    const struct corpus_case *tc = &cases[i];
    struct conn c;
    init_conn_parser(&c, tc->hdr_fields_max);

    struct http_pipeline_result r = http_pipeline_feed(&c, tc->req, strlen(tc->req));
    if (r.action != tc->want_action) {
      FAILm(tc->name);
    }
    ASSERT_EQ(tc->want_action, r.action);

    if (tc->want_action == HP_ACTION_RESP_OK) {
      struct http_apply_plan p = http_pipeline_build_apply_plan(&c, &r);
      ASSERT_EQ(p.kind, HP_APPLY_OK);
      destroy_conn_parser(&c);
      continue;
    }

    struct http_apply_plan p = http_pipeline_build_apply_plan(&c, &r);
    ASSERT_EQ(p.kind, HP_APPLY_ERROR);
    ASSERT_EQ(p.error.keepalive, 0);
    ASSERT_EQ(p.error.close_after_send, tc->want_close_after_send);
    ASSERT_EQ(p.error.drain_after_headers, tc->want_drain_after_headers);

    destroy_conn_parser(&c);
  }

  PASS();
}

TEST t_header_malformed_matrix_escalates_400(void) {
  static const char *bad_lines[] = {
    "Ho st: x", // invalid header field-name token
    "X-Test: \x01", // invalid control character in value
  };

  for (size_t i = 0; i < sizeof(bad_lines) / sizeof(bad_lines[0]); ++i) {
    struct conn c;
    init_conn_parser(&c, 100);

    const char *start = "GET / HTTP/1.1\r\n";
    (void)http_pipeline_feed(&c, start, strlen(start));

    struct http_pipeline_result r1 = http_pipeline_feed(&c, bad_lines[i], strlen(bad_lines[i]));

    if (r1.action == HP_ACTION_RESP_400) {
      struct http_apply_plan p = http_pipeline_build_apply_plan(&c, &r1);
      ASSERT_EQ(p.kind, HP_APPLY_ERROR);
      ASSERT_EQ(p.error.keepalive, 0);
      ASSERT_EQ(p.error.close_after_send, 1);
      ASSERT_EQ(p.error.drain_after_headers, 0);

      destroy_conn_parser(&c);
      continue;
    }

    ASSERT_EQ(r1.action, HP_ACTION_CONTINUE);

    struct http_pipeline_result r2 = http_pipeline_feed(&c, "\r\n\r\n", 4);
    ASSERT_EQ(r2.action, HP_ACTION_RESP_400);
    struct http_apply_plan p = http_pipeline_build_apply_plan(&c, &r2);
    ASSERT_EQ(p.kind, HP_APPLY_ERROR);
    ASSERT_EQ(p.error.keepalive, 0);
    ASSERT_EQ(p.error.close_after_send, 1);
    ASSERT_EQ(p.error.drain_after_headers, 0);

    destroy_conn_parser(&c);
  }

  PASS();
}

SUITE(s_http_parser_smuggling) {
  RUN_TEST(t_smuggling_adversarial_corpus);
  RUN_TEST(t_header_malformed_matrix_escalates_400);
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv) {
  GREATEST_MAIN_BEGIN();
  RUN_SUITE(s_http_parser_smuggling);
  GREATEST_MAIN_END();
}
