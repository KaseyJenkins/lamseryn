#include "include/config.h"
#include "include/conn.h"
#include "include/http_pipeline.h"
#include <errno.h>
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

int static_serve_try_prepare_docroot_response(struct conn *c, int docroot_fd, int *static_open_err) {
  (void)c;
  (void)docroot_fd;
  g_static_serve_calls++;
  if (static_open_err) {
    *static_open_err = g_static_serve_errno;
  }
  return g_static_serve_result;
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
  struct response_view r400 = request_select_response(RK_400, 0);
  ASSERT_EQ(r400.buf, RESP_400);
  ASSERT_EQ(r400.len, RESP_400_len);

  struct response_view r403 = request_select_response(RK_403, 0);
  ASSERT_EQ(r403.buf, RESP_403);
  ASSERT_EQ(r403.len, RESP_403_len);

  struct response_view r404 = request_select_response(RK_404, 0);
  ASSERT_EQ(r404.buf, RESP_404);
  ASSERT_EQ(r404.len, RESP_404_len);

  struct response_view r405 = request_select_response(RK_405, 0);
  ASSERT_EQ(r405.buf, RESP_405);
  ASSERT_EQ(r405.len, RESP_405_len);

  struct response_view r413 = request_select_response(RK_413, 0);
  ASSERT_EQ(r413.buf, RESP_413);
  ASSERT_EQ(r413.len, RESP_413_len);

  struct response_view r431 = request_select_response(RK_431, 0);
  ASSERT_EQ(r431.buf, RESP_431);
  ASSERT_EQ(r431.len, RESP_431_len);

  struct response_view r501 = request_select_response(RK_501, 0);
  ASSERT_EQ(r501.buf, RESP_501);
  ASSERT_EQ(r501.len, RESP_501_len);

  struct response_view r408 = request_select_response(RK_408, 0);
  ASSERT_EQ(r408.buf, RESP_408);
  ASSERT_EQ(r408.len, RESP_408_len);

  struct response_view r500 = request_select_response(RK_500, 0);
  ASSERT_EQ(r500.buf, RESP_500);
  ASSERT_EQ(r500.len, RESP_500_len);

#if ENABLE_OVERLOAD_503
  struct response_view r503 = request_select_response(RK_503, 0);
  ASSERT_EQ(r503.buf, RESP_503);
  ASSERT_EQ(r503.len, RESP_503_len);
#else
  struct response_view r503 = request_select_response(RK_503, 0);
  ASSERT_EQ(r503.buf, NULL);
  ASSERT_EQ(r503.len, 0);
#endif
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
  ASSERT_EQ(plan.method_not_allowed_response.response.buf, RESP_405);
  ASSERT_EQ(plan.method_not_allowed_response.response.len, RESP_405_len);
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
  ASSERT_EQ(p403.response.buf, RESP_403);
  ASSERT_EQ(p403.response.len, RESP_403_len);

  struct request_response_plan p404 = request_build_static_fallback_plan(ENOENT);
  ASSERT_EQ(p404.kind, RK_404);
  ASSERT_EQ(p404.keepalive, 0);
  ASSERT_EQ(p404.drain_after_headers, 0);
  ASSERT_EQ(p404.close_after_send, 1);
  ASSERT_EQ(p404.response.buf, RESP_404);
  ASSERT_EQ(p404.response.len, RESP_404_len);
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

SUITE(s_request_handlers) {
  RUN_TEST(t_ok_variants_have_no_fixed_response_mapping);
  RUN_TEST(t_errors_map);
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
}

GREATEST_MAIN_DEFS();
int main(int argc, char **argv) {
  GREATEST_MAIN_BEGIN();
  RUN_SUITE(s_request_handlers);
  GREATEST_MAIN_END();
}
