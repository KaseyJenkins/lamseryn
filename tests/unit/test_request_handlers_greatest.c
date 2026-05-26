#include "include/config.h"
#include "include/conn.h"
#include "include/http_pipeline.h"
#include "include/http_headers.h"
#include "include/tx.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include "../vendor/greatest_color.h"
#include "../vendor/greatest.h"
#include "include/request_handlers.h"

// Satisfy externs from request_handlers.c
const char RESP_400[] = "B4";
const size_t RESP_400_len = sizeof(RESP_400) - 1;
const char RESP_403[] = "B403";
const size_t RESP_403_len = sizeof(RESP_403) - 1;
const char RESP_404[] = "B404";
const size_t RESP_404_len = sizeof(RESP_404) - 1;
const char RESP_405[] = "B405";
const size_t RESP_405_len = sizeof(RESP_405) - 1;
const char RESP_413[] = "H413";
const size_t RESP_413_len = sizeof(RESP_413) - 1;
const char RESP_431[] = "H431";
const size_t RESP_431_len = sizeof(RESP_431) - 1;
const char RESP_501[] = "H501";
const size_t RESP_501_len = sizeof(RESP_501) - 1;
const char RESP_408[] = "H408";
const size_t RESP_408_len = sizeof(RESP_408) - 1;
const char RESP_500[] = "H500";
const size_t RESP_500_len = sizeof(RESP_500) - 1;
const char RESP_503[] = "H503";
const size_t RESP_503_len = sizeof(RESP_503) - 1;

static int g_static_serve_result = 0;
static int g_static_serve_errno = 0;
static int g_static_serve_calls = 0;

static int g_tx_build_headers_calls = 0;
static char g_tx_last_status[64];
static char g_tx_last_extra[1024];

static void reset_tx_stubs(void) {
  g_tx_build_headers_calls = 0;
  g_tx_last_status[0] = '\0';
  g_tx_last_extra[0] = '\0';
}

// Stub for auth_basic_check: all test vhosts have auth_store=NULL so this
// would return 0 in the real implementation anyway.
int auth_basic_check(struct conn *c) {
  (void)c;
  return 0;
}

int static_serve_try_prepare_docroot_response(struct conn *c, int docroot_fd, int *static_open_err) {
  (void)c;
  (void)docroot_fd;
  g_static_serve_calls++;
  if (static_open_err) {
    *static_open_err = g_static_serve_errno;
  }
  return g_static_serve_result;
}

const char *http_header_find_value(const struct req_hdr_entry *hdrs,
                                   uint8_t hdr_count,
                                   enum http_header_id id,
                                   uint8_t *out_len) {
  if (out_len) {
    *out_len = 0;
  }
  if (!hdrs || hdr_count == 0) {
    return NULL;
  }

  for (uint8_t i = 0; i < hdr_count; ++i) {
    if ((enum http_header_id)hdrs[i].id == id) {
      if (out_len) {
        *out_len = hdrs[i].value_len;
      }
      return hdrs[i].value;
    }
  }
  return NULL;
}

enum tx_decision tx_begin_headers(struct tx_state_t *tx,
                                  enum resp_kind rk,
                                  const char *buf,
                                  size_t len,
                                  int keepalive,
                                  int drain_after_headers,
                                  struct tx_next_io *out) {
  (void)tx;
  (void)rk;
  (void)buf;
  (void)len;
  (void)keepalive;
  (void)drain_after_headers;
  (void)out;
  return TX_SEND_HEADERS;
}

int tx_build_headers(struct tx_state_t *tx,
                     const char *status_line,
                     const char *content_type,
                     int emit_content_length,
                     size_t content_len,
                     const void *body,
                     size_t body_send_len,
                     int keepalive,
                     int drain_after_headers,
                     const char *extra_headers,
                     const char **buf,
                     size_t *len) {
  (void)tx;
  (void)content_type;
  (void)emit_content_length;
  (void)content_len;
  (void)body;
  (void)body_send_len;
  (void)keepalive;
  (void)drain_after_headers;

  g_tx_build_headers_calls++;
  snprintf(g_tx_last_status, sizeof(g_tx_last_status), "%s", status_line ? status_line : "");
  snprintf(g_tx_last_extra,
           sizeof(g_tx_last_extra),
           "%s",
           extra_headers ? extra_headers : "");

  static const char fake[] = "H";
  if (buf) {
    *buf = fake;
  }
  if (len) {
    *len = sizeof(fake) - 1;
  }
  return 0;
}

TEST t_ok_variants_have_no_fixed_response_mapping(void) {
  struct response_view rv1 = request_select_response(RK_OK_KA, 1);
  ASSERT_EQ(rv1.buf, NULL);
  ASSERT_EQ(rv1.len, 0);

  struct response_view rv2 = request_select_response(RK_OK_CLOSE, 0);
  ASSERT_EQ(rv2.buf, NULL);
  ASSERT_EQ(rv2.len, 0);
  PASS();
}

TEST t_errors_map(void) {
  struct request_response_plan p400 = request_build_response_plan(RK_400, 0, 0, 1);
  ASSERT_STR_EQ(p400.status_line, "400 Bad Request");
  ASSERT_EQ(p400.keepalive, 0);

  struct request_response_plan p403 = request_build_response_plan(RK_403, 0, 0, 1);
  ASSERT_STR_EQ(p403.status_line, "403 Forbidden");

  struct request_response_plan p404 = request_build_response_plan(RK_404, 0, 0, 1);
  ASSERT_STR_EQ(p404.status_line, "404 Not Found");

  struct request_response_plan p405 = request_build_response_plan(RK_405, 0, 0, 1);
  ASSERT_STR_EQ(p405.status_line, "405 Method Not Allowed");

  struct request_response_plan p413 = request_build_response_plan(RK_413, 0, 0, 1);
  ASSERT_STR_EQ(p413.status_line, "413 Payload Too Large");

  struct request_response_plan p431 = request_build_response_plan(RK_431, 0, 1, 0);
  ASSERT_STR_EQ(p431.status_line, "431 Request Header Fields Too Large");

  struct request_response_plan p501 = request_build_response_plan(RK_501, 0, 0, 1);
  ASSERT_STR_EQ(p501.status_line, "501 Not Implemented");

  struct request_response_plan p408 = request_build_response_plan(RK_408, 0, 0, 1);
  ASSERT_STR_EQ(p408.status_line, "408 Request Timeout");

  struct request_response_plan p500 = request_build_response_plan(RK_500, 0, 0, 1);
  ASSERT_STR_EQ(p500.status_line, "500 Internal Server Error");

#if ENABLE_OVERLOAD_503
  struct request_response_plan p503 = request_build_response_plan(RK_503, 0, 0, 1);
  ASSERT_STR_EQ(p503.status_line, "503 Service Unavailable");
#else
  struct request_response_plan p503 = request_build_response_plan(RK_503, 0, 0, 1);
  ASSERT_EQ(p503.status_line, NULL);
#endif
  PASS();
}

TEST t_policy_fail_closed_response_plan_error_maps_500_close(void) {
  struct request_response_plan in = request_build_response_plan(RK_405,
                                                                /*keepalive=*/1,
                                                                /*drain_after_headers=*/1,
                                                                /*close_after_send=*/0);
  struct request_response_plan out = request_policy_fail_closed_response_plan(in, -1);
  ASSERT_EQ(out.kind, RK_500);
  ASSERT_EQ(out.keepalive, 0);
  ASSERT_EQ(out.drain_after_headers, 0);
  ASSERT_EQ(out.close_after_send, 1);
  ASSERT_STR_EQ(out.status_line, "500 Internal Server Error");
  PASS();
}

TEST t_policy_fail_closed_response_plan_success_keeps_plan(void) {
  struct request_response_plan in = request_build_response_plan(RK_404,
                                                                /*keepalive=*/0,
                                                                /*drain_after_headers=*/0,
                                                                /*close_after_send=*/1);
  struct request_response_plan out = request_policy_fail_closed_response_plan(in, 1);
  ASSERT_EQ(out.kind, RK_404);
  ASSERT_EQ(out.keepalive, 0);
  ASSERT_EQ(out.drain_after_headers, 0);
  ASSERT_EQ(out.close_after_send, 1);
  ASSERT_STR_EQ(out.status_line, "404 Not Found");
  PASS();
}

TEST t_route_plan_static_eligible(void) {
  struct conn c;
  struct vhost_t vh;
  char path[] = "/index.html";
  memset(&c, 0, sizeof(c));
  memset(&vh, 0, sizeof(vh));

  vh.features = CFG_FEAT_STATIC;
  vh.docroot[0] = '/';
  vh.docroot_fd = 10;
  c.vhost = &vh;
  c.h1.path_bad = 0;
  c.h1.path_norm = path;
  c.h1.path_norm_len = (uint16_t)(sizeof(path) - 1);

  struct request_route_plan plan = request_build_route_plan(&c);
  ASSERT_EQ(plan.try_static, 1);
  PASS();
}

TEST t_route_plan_static_ineligible(void) {
  struct conn c;
  struct vhost_t vh;
  char path[] = "/index.html";
  memset(&c, 0, sizeof(c));
  memset(&vh, 0, sizeof(vh));

  vh.features = 0;
  vh.docroot[0] = '/';
  vh.docroot_fd = 10;
  c.vhost = &vh;
  c.h1.path_bad = 0;
  c.h1.path_norm = path;
  c.h1.path_norm_len = (uint16_t)(sizeof(path) - 1);

  struct request_route_plan plan = request_build_route_plan(&c);
  ASSERT_EQ(plan.try_static, 0);
  PASS();
}

TEST t_route_plan_method_not_allowed_override(void) {
  struct conn c;
  memset(&c, 0, sizeof(c));
  c.h1.method_not_allowed = 1;

  struct request_route_plan plan = request_build_route_plan(&c);
  ASSERT_EQ(plan.has_method_not_allowed_response, 1);
  ASSERT_EQ(plan.try_static, 0);

  ASSERT_EQ(plan.method_not_allowed_response.kind, RK_405);
  ASSERT_EQ(plan.method_not_allowed_response.keepalive, 0);
  ASSERT_EQ(plan.method_not_allowed_response.drain_after_headers, 0);
  ASSERT_EQ(plan.method_not_allowed_response.close_after_send, 1);
  ASSERT_STR_EQ(plan.method_not_allowed_response.status_line, "405 Method Not Allowed");
  PASS();
}

TEST t_static_open_error_kind_mapping(void) {
  ASSERT_EQ(request_static_open_error_kind(EACCES), RK_403);
  ASSERT_EQ(request_static_open_error_kind(EPERM), RK_403);
  ASSERT_EQ(request_static_open_error_kind(ELOOP), RK_403);
  ASSERT_EQ(request_static_open_error_kind(ENOENT), RK_404);
  ASSERT_EQ(request_static_open_error_kind(ENOTDIR), RK_404);
  PASS();
}

TEST t_static_open_err_merge_and_finalize(void) {
  int e;

  e = 0;
  e = request_static_open_err_merge(e, EACCES);
  ASSERT_EQ(e, EACCES);

  e = request_static_open_err_merge(e, 0);
  ASSERT_EQ(e, EACCES);

  ASSERT_EQ(request_static_open_err_finalize(0), ENOENT);
  ASSERT_EQ(request_static_open_err_finalize(ENOTDIR), ENOTDIR);
  PASS();
}

TEST t_static_fallback_plan_mapping(void) {
  struct request_response_plan p403 = request_build_static_fallback_plan(EACCES);
  ASSERT_EQ(p403.kind, RK_403);
  ASSERT_EQ(p403.keepalive, 0);
  ASSERT_EQ(p403.drain_after_headers, 0);
  ASSERT_EQ(p403.close_after_send, 1);
  ASSERT_STR_EQ(p403.status_line, "403 Forbidden");

  struct request_response_plan p404 = request_build_static_fallback_plan(ENOENT);
  ASSERT_EQ(p404.kind, RK_404);
  ASSERT_EQ(p404.keepalive, 0);
  ASSERT_EQ(p404.drain_after_headers, 0);
  ASSERT_EQ(p404.close_after_send, 1);
  ASSERT_STR_EQ(p404.status_line, "404 Not Found");
  PASS();
}

TEST t_route_apply_plan_method_not_allowed_overrides_ok(void) {
  struct conn c;
  struct request_response_plan ok;
  struct request_static_outcome out;
  memset(&c, 0, sizeof(c));
  c.h1.method_not_allowed = 1;

  ok = request_build_response_plan(RK_OK_KA,
                                   /*keepalive=*/1,
                                   /*drain_after_headers=*/0,
                                   /*close_after_send=*/0);
  memset(&out, 0, sizeof(out));
  struct request_route_apply_plan p = request_build_route_apply_plan(&c, ok, out);
  ASSERT_EQ(p.send_terminal_response, 1);
  ASSERT_EQ(p.terminal_response.kind, RK_405);
  ASSERT_EQ(p.terminal_response.keepalive, 0);
  ASSERT_EQ(p.terminal_response.close_after_send, 1);
  PASS();
}

TEST t_route_apply_plan_static_fallback_overrides_ok(void) {
  struct conn c;
  struct vhost_t vh;
  char path[] = "/index.html";
  struct request_response_plan ok;
  struct request_route_plan route;
  struct request_static_outcome out;
  memset(&c, 0, sizeof(c));
  memset(&vh, 0, sizeof(vh));

  vh.features = CFG_FEAT_STATIC;
  vh.docroot[0] = '/';
  vh.docroot_fd = 10;
  c.vhost = &vh;
  c.h1.path_bad = 0;
  c.h1.path_norm = path;
  c.h1.path_norm_len = (uint16_t)(sizeof(path) - 1);

  ok = request_build_response_plan(RK_OK_KA,
                                   /*keepalive=*/1,
                                   /*drain_after_headers=*/0,
                                   /*close_after_send=*/0);
  route = request_build_route_plan(&c);
  out = request_build_static_outcome(&route, EACCES);
  struct request_route_apply_plan p = request_build_route_apply_plan(&c, ok, out);
  ASSERT_EQ(p.send_terminal_response, 1);
  ASSERT_EQ(p.terminal_response.kind, RK_403);
  ASSERT_EQ(p.terminal_response.keepalive, 0);
  ASSERT_EQ(p.terminal_response.close_after_send, 1);
  PASS();
}

TEST t_route_apply_plan_default_404_when_no_static_attempt(void) {
  struct conn c;
  struct vhost_t vh;
  char path[] = "/index.html";
  struct request_response_plan ok;
  struct request_route_plan route;
  struct request_static_outcome out;
  memset(&c, 0, sizeof(c));
  memset(&vh, 0, sizeof(vh));

  vh.features = CFG_FEAT_STATIC;
  vh.docroot[0] = '/';
  vh.docroot_fd = 10;
  c.vhost = &vh;
  c.h1.path_bad = 0;
  c.h1.path_norm = path;
  c.h1.path_norm_len = (uint16_t)(sizeof(path) - 1);

  ok = request_build_response_plan(RK_OK_KA,
                                   /*keepalive=*/1,
                                   /*drain_after_headers=*/0,
                                   /*close_after_send=*/0);
  route = request_build_route_plan(&c);
  out = request_build_static_outcome(&route, EACCES);
  out.open_attempted = 0;
  struct request_route_apply_plan p = request_build_route_apply_plan(&c, ok, out);
  ASSERT_EQ(p.send_terminal_response, 1);
  ASSERT_EQ(p.terminal_response.kind, RK_404);
  ASSERT_EQ(p.terminal_response.keepalive, 0);
  ASSERT_EQ(p.terminal_response.close_after_send, 1);
  PASS();
}

TEST t_static_outcome_shapes_attempt_and_err(void) {
  struct request_route_plan route = {0};
  struct request_static_outcome out;

  route.try_static = 1;
  out = request_build_static_outcome(&route, EACCES);
  ASSERT_EQ(out.open_attempted, 1);
  ASSERT_EQ(out.open_err, EACCES);

  route.try_static = 0;
  out = request_build_static_outcome(&route, EACCES);
  ASSERT_EQ(out.open_attempted, 0);
  ASSERT_EQ(out.open_err, 0);
  PASS();
}

TEST t_static_serve_plan_head_mode(void) {
  struct conn c;
  memset(&c, 0, sizeof(c));
  c.h1.method = HTTP_HEAD;

  struct request_static_serve_plan p = request_build_static_serve_plan(&c, 1024);
  ASSERT_EQ(p.mode, REQUEST_STATIC_SERVE_HEAD);
  PASS();
}

TEST t_static_serve_plan_buffered_mode_small_get(void) {
  struct conn c;
  memset(&c, 0, sizeof(c));
  c.h1.method = HTTP_GET;

  struct request_static_serve_plan p = request_build_static_serve_plan(&c, 4096);
  ASSERT_EQ(p.mode, REQUEST_STATIC_SERVE_BUFFERED);
  PASS();
}

TEST t_static_serve_plan_sendfile_mode_large_or_empty(void) {
  struct conn c;
  memset(&c, 0, sizeof(c));
  c.h1.method = HTTP_GET;

  struct request_static_serve_plan p0 = request_build_static_serve_plan(&c, 0);
  ASSERT_EQ(p0.mode, REQUEST_STATIC_SERVE_SENDFILE);

  struct request_static_serve_plan p1 = request_build_static_serve_plan(&c, (size_t)(512 * 1024));
  ASSERT_EQ(p1.mode, REQUEST_STATIC_SERVE_SENDFILE);
  PASS();
}

TEST t_request_dispatch_ok_null_guards(void) {
  struct conn c;
  struct http_ok_plan okplan;
  memset(&c, 0, sizeof(c));
  memset(&okplan, 0, sizeof(okplan));

  struct request_ok_dispatch d1 = request_dispatch_ok(NULL, &okplan);
  ASSERT_EQ(d1.kind, REQUEST_OK_NO_RESPONSE);

  struct request_ok_dispatch d2 = request_dispatch_ok(&c, NULL);
  ASSERT_EQ(d2.kind, REQUEST_OK_NO_RESPONSE);
  PASS();
}

TEST t_request_dispatch_ok_method_not_allowed_terminal_405(void) {
  struct conn c;
  struct http_ok_plan okplan;
  memset(&c, 0, sizeof(c));
  memset(&okplan, 0, sizeof(okplan));

  c.h1.method_not_allowed = 1;
  c.h1.want_keepalive = 1;
  okplan.kind = RK_OK_KA;
  okplan.keepalive = 1;
  okplan.close_after_send = 0;

  struct request_ok_dispatch d = request_dispatch_ok(&c, &okplan);
  ASSERT_EQ(d.kind, REQUEST_OK_HEADER_RESPONSE);
  ASSERT_EQ(d.response.kind, RK_405);
  ASSERT_EQ(d.response.keepalive, 0);
  ASSERT_EQ(d.response.close_after_send, 1);
  ASSERT_EQ(c.h1.want_keepalive, 1); // helper must not mutate; caller applies
  PASS();
}

TEST t_request_dispatch_ok_non_static_defaults_404(void) {
  struct conn c;
  struct http_ok_plan okplan;
  memset(&c, 0, sizeof(c));
  memset(&okplan, 0, sizeof(okplan));

  c.h1.want_keepalive = 1;
  okplan.kind = RK_OK_KA;
  okplan.keepalive = 1;
  okplan.close_after_send = 0;

  struct request_ok_dispatch d = request_dispatch_ok(&c, &okplan);
  ASSERT_EQ(d.kind, REQUEST_OK_HEADER_RESPONSE);
  ASSERT_EQ(d.response.kind, RK_404);
  ASSERT_EQ(d.response.keepalive, 0);
  ASSERT_EQ(d.response.close_after_send, 1);
  ASSERT_EQ(c.h1.want_keepalive, 1); // helper must not mutate; caller applies
  PASS();
}

TEST t_request_dispatch_ok_static_success_returns_tx_buffer(void) {
  struct conn c;
  struct vhost_t vh;
  struct http_ok_plan okplan;
  char path[] = "/index.html";
  memset(&c, 0, sizeof(c));
  memset(&vh, 0, sizeof(vh));
  memset(&okplan, 0, sizeof(okplan));

  vh.features = CFG_FEAT_STATIC;
  vh.docroot[0] = '/';
  vh.docroot_fd = 9;
  c.vhost = &vh;
  c.h1.path_norm = path;
  c.h1.path_norm_len = (uint16_t)(sizeof(path) - 1);

  okplan.kind = RK_OK_KA;
  okplan.keepalive = 1;
  okplan.close_after_send = 0;

  g_static_serve_calls = 0;
  g_static_serve_result = 1;
  g_static_serve_errno = 0;

  struct request_ok_dispatch d = request_dispatch_ok(&c, &okplan);
  ASSERT_EQ(g_static_serve_calls, 1);
  ASSERT_EQ(d.kind, REQUEST_OK_TX_BUFFER);
  PASS();
}

TEST t_request_dispatch_ok_static_fallback_uses_open_errno(void) {
  struct conn c;
  struct vhost_t vh;
  struct http_ok_plan okplan;
  char path[] = "/index.html";
  memset(&c, 0, sizeof(c));
  memset(&vh, 0, sizeof(vh));
  memset(&okplan, 0, sizeof(okplan));

  vh.features = CFG_FEAT_STATIC;
  vh.docroot[0] = '/';
  vh.docroot_fd = 9;
  c.vhost = &vh;
  c.h1.path_norm = path;
  c.h1.path_norm_len = (uint16_t)(sizeof(path) - 1);
  c.h1.want_keepalive = 1;

  okplan.kind = RK_OK_KA;
  okplan.keepalive = 1;
  okplan.close_after_send = 0;

  g_static_serve_calls = 0;
  g_static_serve_result = 0;
  g_static_serve_errno = EACCES;

  struct request_ok_dispatch d = request_dispatch_ok(&c, &okplan);
  ASSERT_EQ(g_static_serve_calls, 1);
  ASSERT_EQ(d.kind, REQUEST_OK_HEADER_RESPONSE);
  ASSERT_EQ(d.response.kind, RK_403);
  ASSERT_EQ(d.response.keepalive, 0);
  ASSERT_EQ(d.response.close_after_send, 1);
  ASSERT_EQ(c.h1.want_keepalive, 1); // helper must not mutate; caller applies
  PASS();
}

TEST t_request_dispatch_ok_options_preflight_route_cors_enabled(void) {
  struct conn c;
  struct vhost_t vh;
  struct route_policy_rule rr;
  const struct route_policy_rule *route_rules[1];
  struct req_hdr_entry hdrs[2];
  struct http_ok_plan okplan;
  char path[] = "/api/users";
  static char origin[] = "https://client.example";
  static char req_method[] = "POST";

  memset(&c, 0, sizeof(c));
  memset(&vh, 0, sizeof(vh));
  memset(&rr, 0, sizeof(rr));
  memset(hdrs, 0, sizeof(hdrs));
  memset(&okplan, 0, sizeof(okplan));

  c.vhost = &vh;
  c.h1.method = HTTP_OPTIONS;
  c.h1.method_not_allowed = 1;
  c.h1.path_norm = path;
  c.h1.path_norm_len = (uint16_t)(sizeof(path) - 1);

  hdrs[0].id = (uint8_t)HDR_ID_ORIGIN;
  hdrs[0].value = origin;
  hdrs[0].value_len = (uint8_t)strlen(origin);
  hdrs[1].id = (uint8_t)HDR_ID_ACCESS_CONTROL_REQUEST_METHOD;
  hdrs[1].value = req_method;
  hdrs[1].value_len = (uint8_t)strlen(req_method);
  c.h1.req_hdrs = hdrs;
  c.h1.req_hdr_count = 2;

  snprintf(rr.path_prefix, sizeof(rr.path_prefix), "%s", "/api");
  rr.path_prefix_len = (uint16_t)strlen(rr.path_prefix);
  rr.cors.enabled = 1;
  rr.cors.enabled_set = 1;
  rr.cors.allow_origin_set = 1;
  rr.cors.allow_methods_set = 1;
  snprintf(rr.cors.allow_origin, sizeof(rr.cors.allow_origin), "%s", "https://app.example.com");
  snprintf(rr.cors.allow_methods, sizeof(rr.cors.allow_methods), "%s", "GET,POST,OPTIONS");
  route_rules[0] = &rr;
  vh.route_rules = route_rules;
  vh.route_rule_count = 1;
  vh.route_rule_cap = 1;

  reset_tx_stubs();
  g_static_serve_calls = 0;
  okplan.kind = RK_405;
  okplan.keepalive = 0;
  okplan.close_after_send = 1;

  struct request_ok_dispatch d = request_dispatch_ok(&c, &okplan);
  ASSERT_EQ(d.kind, REQUEST_OK_TX_BUFFER);
  ASSERT_EQ(g_static_serve_calls, 0);
  ASSERT_EQ(g_tx_build_headers_calls, 1);
  ASSERT_STR_EQ(g_tx_last_status, "204 No Content");
  ASSERT(strstr(g_tx_last_extra, "Access-Control-Allow-Origin: https://app.example.com") != NULL);
  ASSERT(strstr(g_tx_last_extra, "Access-Control-Allow-Methods: GET,POST,OPTIONS") != NULL);
  ASSERT(strstr(g_tx_last_extra, "Vary: Origin") != NULL);
  PASS();
}

TEST t_request_dispatch_ok_options_preflight_missing_hdr_falls_back_405(void) {
  struct conn c;
  struct vhost_t vh;
  struct route_policy_rule rr;
  const struct route_policy_rule *route_rules[1];
  struct req_hdr_entry hdrs[1];
  struct http_ok_plan okplan;
  char path[] = "/api/users";
  static char origin[] = "https://client.example";

  memset(&c, 0, sizeof(c));
  memset(&vh, 0, sizeof(vh));
  memset(&rr, 0, sizeof(rr));
  memset(hdrs, 0, sizeof(hdrs));
  memset(&okplan, 0, sizeof(okplan));

  c.vhost = &vh;
  c.h1.method = HTTP_OPTIONS;
  c.h1.method_not_allowed = 1;
  c.h1.path_norm = path;
  c.h1.path_norm_len = (uint16_t)(sizeof(path) - 1);

  hdrs[0].id = (uint8_t)HDR_ID_ORIGIN;
  hdrs[0].value = origin;
  hdrs[0].value_len = (uint8_t)strlen(origin);
  c.h1.req_hdrs = hdrs;
  c.h1.req_hdr_count = 1;

  snprintf(rr.path_prefix, sizeof(rr.path_prefix), "%s", "/api");
  rr.path_prefix_len = (uint16_t)strlen(rr.path_prefix);
  rr.cors.enabled = 1;
  rr.cors.enabled_set = 1;
  rr.cors.allow_origin_set = 1;
  snprintf(rr.cors.allow_origin, sizeof(rr.cors.allow_origin), "%s", "*");
  route_rules[0] = &rr;
  vh.route_rules = route_rules;
  vh.route_rule_count = 1;
  vh.route_rule_cap = 1;

  reset_tx_stubs();
  okplan.kind = RK_405;
  okplan.keepalive = 0;
  okplan.close_after_send = 1;

  struct request_ok_dispatch d = request_dispatch_ok(&c, &okplan);
  ASSERT_EQ(d.kind, REQUEST_OK_HEADER_RESPONSE);
  ASSERT_EQ(d.response.kind, RK_405);
  ASSERT_EQ(g_tx_build_headers_calls, 0);
  PASS();
}

TEST t_request_dispatch_ok_options_no_cors_falls_back_405(void) {
  struct conn c;
  struct vhost_t vh;
  struct req_hdr_entry hdrs[2];
  struct http_ok_plan okplan;
  char path[] = "/api/users";
  static char origin[] = "https://client.example";
  static char req_method[] = "POST";

  memset(&c, 0, sizeof(c));
  memset(&vh, 0, sizeof(vh));
  memset(hdrs, 0, sizeof(hdrs));
  memset(&okplan, 0, sizeof(okplan));

  c.vhost = &vh;
  c.h1.method = HTTP_OPTIONS;
  c.h1.method_not_allowed = 1;
  c.h1.path_norm = path;
  c.h1.path_norm_len = (uint16_t)(sizeof(path) - 1);

  hdrs[0].id = (uint8_t)HDR_ID_ORIGIN;
  hdrs[0].value = origin;
  hdrs[0].value_len = (uint8_t)strlen(origin);
  hdrs[1].id = (uint8_t)HDR_ID_ACCESS_CONTROL_REQUEST_METHOD;
  hdrs[1].value = req_method;
  hdrs[1].value_len = (uint8_t)strlen(req_method);
  c.h1.req_hdrs = hdrs;
  c.h1.req_hdr_count = 2;

  reset_tx_stubs();
  okplan.kind = RK_405;
  okplan.keepalive = 0;
  okplan.close_after_send = 1;

  struct request_ok_dispatch d = request_dispatch_ok(&c, &okplan);
  ASSERT_EQ(d.kind, REQUEST_OK_HEADER_RESPONSE);
  ASSERT_EQ(d.response.kind, RK_405);
  ASSERT_EQ(g_tx_build_headers_calls, 0);
  PASS();
}

TEST t_request_policy_extra_headers_route_override_and_dedup(void) {
  struct conn c;
  struct vhost_t vh;
  struct route_policy_rule rr;
  struct security_headers_policy vh_sec;
  struct cors_policy vh_cors;
  const struct route_policy_rule *route_rules[1];
  char path[] = "/api/items";
  static char custom_dup[] = "X-Frame-Options: FROM-CUSTOM\r\n";
  static char custom_ok[] = "X-Test-Custom: keep\r\n";
  char extra[1024];

  memset(&c, 0, sizeof(c));
  memset(&vh, 0, sizeof(vh));
  memset(&rr, 0, sizeof(rr));
  memset(&vh_sec, 0, sizeof(vh_sec));
  memset(&vh_cors, 0, sizeof(vh_cors));
  memset(route_rules, 0, sizeof(route_rules));
  memset(extra, 0, sizeof(extra));

  c.vhost = &vh;
  c.h1.path_norm = path;
  c.h1.path_norm_len = (uint16_t)(sizeof(path) - 1);

  vh.security_headers = &vh_sec;
  vh.security_headers->enabled = 1;
  vh.security_headers->enabled_set = 1;
  snprintf(vh.security_headers->headers[0].name,
           sizeof(vh.security_headers->headers[0].name),
           "%s",
           "X-Frame-Options");
  snprintf(vh.security_headers->headers[0].value,
           sizeof(vh.security_headers->headers[0].value),
           "%s",
           "DENY");
  vh.security_headers->header_count = 1;

  vh.cors = &vh_cors;
  vh.cors->enabled = 1;
  vh.cors->enabled_set = 1;
  vh.cors->allow_origin_set = 1;
  snprintf(vh.cors->allow_origin, sizeof(vh.cors->allow_origin), "%s", "*");

  vh.custom_headers[0] = custom_dup;
  vh.custom_headers[1] = custom_ok;
  vh.custom_headers_count = 2;

  snprintf(rr.path_prefix, sizeof(rr.path_prefix), "%s", "/api");
  rr.path_prefix_len = (uint16_t)strlen(rr.path_prefix);
  rr.security_headers.enabled = 1;
  rr.security_headers.enabled_set = 1;
  snprintf(rr.security_headers.headers[0].name,
           sizeof(rr.security_headers.headers[0].name),
           "%s",
           "X-Frame-Options");
  snprintf(rr.security_headers.headers[0].value,
           sizeof(rr.security_headers.headers[0].value),
           "%s",
           "SAMEORIGIN");
  rr.security_headers.header_count = 1;
  rr.cors.enabled = 1;
  rr.cors.enabled_set = 1;
  rr.cors.allow_origin_set = 1;
  snprintf(rr.cors.allow_origin, sizeof(rr.cors.allow_origin), "%s", "https://route.example");

  route_rules[0] = &rr;
  vh.route_rules = route_rules;
  vh.route_rule_count = 1;
  vh.route_rule_cap = 1;

  int rc = request_build_policy_extra_headers(&c, RK_405, extra);
  ASSERT_EQ(rc, 1);
  ASSERT(strstr(extra, "X-Frame-Options: SAMEORIGIN\r\n") != NULL);
  ASSERT(strstr(extra, "X-Frame-Options: FROM-CUSTOM\r\n") == NULL);
  ASSERT(strstr(extra, "X-Test-Custom: keep\r\n") != NULL);
  ASSERT(strstr(extra, "Access-Control-Allow-Origin: https://route.example\r\n") != NULL);
  ASSERT(strstr(extra, "Vary: Origin\r\n") != NULL);
  PASS();
}

TEST t_request_policy_extra_headers_unsupported_kind_returns_none(void) {
  struct conn c;
  struct vhost_t vh;
  struct security_headers_policy vh_sec;
  char extra[1024];

  memset(&c, 0, sizeof(c));
  memset(&vh, 0, sizeof(vh));
  memset(&vh_sec, 0, sizeof(vh_sec));
  memset(extra, 0, sizeof(extra));

  c.vhost = &vh;
  vh.security_headers = &vh_sec;
  vh.security_headers->enabled = 1;
  vh.security_headers->enabled_set = 1;
  snprintf(vh.security_headers->headers[0].name,
           sizeof(vh.security_headers->headers[0].name),
           "%s",
           "X-Frame-Options");
  snprintf(vh.security_headers->headers[0].value,
           sizeof(vh.security_headers->headers[0].value),
           "%s",
           "DENY");
  vh.security_headers->header_count = 1;

  int rc = request_build_policy_extra_headers(&c, RK_400, extra);
  ASSERT_EQ(rc, 0);
  ASSERT_EQ(extra[0], '\0');
  PASS();
}

TEST t_request_policy_extra_headers_route_disables_inherited_security(void) {
  struct conn c;
  struct vhost_t vh;
  struct route_policy_rule rr;
  struct security_headers_policy vh_sec;
  const struct route_policy_rule *route_rules[1];
  char path[] = "/api/items";
  char extra[1024];

  memset(&c, 0, sizeof(c));
  memset(&vh, 0, sizeof(vh));
  memset(&rr, 0, sizeof(rr));
  memset(&vh_sec, 0, sizeof(vh_sec));
  memset(route_rules, 0, sizeof(route_rules));
  memset(extra, 0, sizeof(extra));

  c.vhost = &vh;
  c.h1.path_norm = path;
  c.h1.path_norm_len = (uint16_t)(sizeof(path) - 1);

  vh.security_headers = &vh_sec;
  vh.security_headers->enabled = 1;
  vh.security_headers->enabled_set = 1;
  snprintf(vh.security_headers->headers[0].name,
           sizeof(vh.security_headers->headers[0].name),
           "%s",
           "X-Frame-Options");
  snprintf(vh.security_headers->headers[0].value,
           sizeof(vh.security_headers->headers[0].value),
           "%s",
           "DENY");
  vh.security_headers->header_count = 1;

  snprintf(rr.path_prefix, sizeof(rr.path_prefix), "%s", "/api");
  rr.path_prefix_len = (uint16_t)strlen(rr.path_prefix);
  rr.inherit_security_headers = 0;
  rr.inherit_security_headers_set = 1;
  rr.security_headers.enabled = 0;
  rr.security_headers.enabled_set = 1;
  route_rules[0] = &rr;
  vh.route_rules = route_rules;
  vh.route_rule_count = 1;
  vh.route_rule_cap = 1;

  int rc = request_build_policy_extra_headers(&c, RK_404, extra);
  ASSERT_EQ(rc, 0);
  ASSERT_EQ(extra[0], '\0');
  PASS();
}

TEST t_request_policy_extra_headers_overflow_returns_error(void) {
  struct conn c;
  struct vhost_t vh;
  char extra[1024];
  char value[280];
  char h0[340];
  char h1[340];
  char h2[340];
  char h3[340];

  memset(&c, 0, sizeof(c));
  memset(&vh, 0, sizeof(vh));
  memset(extra, 0, sizeof(extra));
  memset(value, 'A', sizeof(value) - 1);
  value[sizeof(value) - 1] = '\0';

  snprintf(h0, sizeof(h0), "X-Long-0: %s\r\n", value);
  snprintf(h1, sizeof(h1), "X-Long-1: %s\r\n", value);
  snprintf(h2, sizeof(h2), "X-Long-2: %s\r\n", value);
  snprintf(h3, sizeof(h3), "X-Long-3: %s\r\n", value);

  c.vhost = &vh;
  vh.custom_headers[0] = h0;
  vh.custom_headers[1] = h1;
  vh.custom_headers[2] = h2;
  vh.custom_headers[3] = h3;
  vh.custom_headers_count = 4;

  int rc = request_build_policy_extra_headers(&c, RK_405, extra);
  ASSERT_EQ(rc, -1);
  ASSERT_EQ(extra[0], '\0');
  PASS();
}

TEST t_request_policy_extra_headers_null_route_list_is_safe(void) {
  struct conn c;
  struct vhost_t vh;
  char path[] = "/api/items";
  char extra[1024];

  memset(&c, 0, sizeof(c));
  memset(&vh, 0, sizeof(vh));
  memset(extra, 0, sizeof(extra));

  c.vhost = &vh;
  c.h1.path_norm = path;
  c.h1.path_norm_len = (uint16_t)(sizeof(path) - 1);

  // Defensive coverage: route count may be set before pointer storage is allocated.
  vh.route_rule_count = 1;
  vh.route_rules = NULL;

  int rc = request_build_policy_extra_headers(&c, RK_405, extra);
  ASSERT_EQ(rc, 0);
  ASSERT_EQ(extra[0], '\0');
  PASS();
}

SUITE(s_request_handlers) {
  RUN_TEST(t_ok_variants_have_no_fixed_response_mapping);
  RUN_TEST(t_errors_map);
  RUN_TEST(t_policy_fail_closed_response_plan_error_maps_500_close);
  RUN_TEST(t_policy_fail_closed_response_plan_success_keeps_plan);
  RUN_TEST(t_route_plan_static_eligible);
  RUN_TEST(t_route_plan_static_ineligible);
  RUN_TEST(t_route_plan_method_not_allowed_override);
  RUN_TEST(t_static_open_error_kind_mapping);
  RUN_TEST(t_static_open_err_merge_and_finalize);
  RUN_TEST(t_static_fallback_plan_mapping);
  RUN_TEST(t_route_apply_plan_method_not_allowed_overrides_ok);
  RUN_TEST(t_route_apply_plan_static_fallback_overrides_ok);
  RUN_TEST(t_route_apply_plan_default_404_when_no_static_attempt);
  RUN_TEST(t_static_outcome_shapes_attempt_and_err);
  RUN_TEST(t_static_serve_plan_head_mode);
  RUN_TEST(t_static_serve_plan_buffered_mode_small_get);
  RUN_TEST(t_static_serve_plan_sendfile_mode_large_or_empty);
  RUN_TEST(t_request_dispatch_ok_null_guards);
  RUN_TEST(t_request_dispatch_ok_method_not_allowed_terminal_405);
  RUN_TEST(t_request_dispatch_ok_non_static_defaults_404);
  RUN_TEST(t_request_dispatch_ok_static_success_returns_tx_buffer);
  RUN_TEST(t_request_dispatch_ok_static_fallback_uses_open_errno);
  RUN_TEST(t_request_dispatch_ok_options_preflight_route_cors_enabled);
  RUN_TEST(t_request_dispatch_ok_options_preflight_missing_hdr_falls_back_405);
  RUN_TEST(t_request_dispatch_ok_options_no_cors_falls_back_405);
  RUN_TEST(t_request_policy_extra_headers_route_override_and_dedup);
  RUN_TEST(t_request_policy_extra_headers_unsupported_kind_returns_none);
  RUN_TEST(t_request_policy_extra_headers_route_disables_inherited_security);
  RUN_TEST(t_request_policy_extra_headers_overflow_returns_error);
  RUN_TEST(t_request_policy_extra_headers_null_route_list_is_safe);
}

GREATEST_MAIN_DEFS();
int main(int argc, char **argv) {
  GREATEST_MAIN_BEGIN();
  RUN_SUITE(s_request_handlers);
  GREATEST_MAIN_END();
}
