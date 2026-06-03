#include "include/config.h"
#include "include/conn.h"
#include "include/request_handlers.h"
#include "include/http_pipeline.h"
#include "include/static_serve_utils.h"
#include "include/http_headers.h"
#include "include/policy_headers_shared.h"
#include "include/itest_echo.h"
#include "include/auth.h"
#include "include/tx.h"
#include <stdarg.h>
#include <stdio.h>
#include <errno.h>

// Response buffers are provided by the server translation unit.
extern const char RESP_403[];
extern const size_t RESP_403_len;
extern const char RESP_404[];
extern const size_t RESP_404_len;
extern const char RESP_405[];
extern const size_t RESP_405_len;
extern const char RESP_400[];
extern const size_t RESP_400_len;
extern const char RESP_413[];
extern const size_t RESP_413_len;
extern const char RESP_500[];
extern const size_t RESP_500_len;
extern const char RESP_431[];
extern const size_t RESP_431_len;
extern const char RESP_501[];
extern const size_t RESP_501_len;
extern const char RESP_408[];
extern const size_t RESP_408_len;
#if ENABLE_OVERLOAD_503
extern const char RESP_503[];
extern const size_t RESP_503_len;
#endif

struct request_response_plan request_build_static_fallback_plan(int open_err);

static int request_hdr_appendf(char *buf, size_t cap, size_t *off, const char *fmt, ...) {
  if (!buf || !off || !fmt || *off >= cap) {
    return -1;
  }
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(buf + *off, cap - *off, fmt, ap);
  va_end(ap);
  if (n <= 0 || (size_t)n >= (cap - *off)) {
    return -1;
  }
  *off += (size_t)n;
  return 0;
}

static int request_response_kind_supports_policy(enum resp_kind kind) {
  return (kind == RK_400 || kind == RK_403 || kind == RK_404 || kind == RK_405
          || kind == RK_408 || kind == RK_413 || kind == RK_431 || kind == RK_500
          || kind == RK_501
#if ENABLE_OVERLOAD_503
          || kind == RK_503
#endif
          )
           ? 1
           : 0;
}

int request_build_policy_extra_headers(const struct conn *c,
                                       enum resp_kind kind,
                                       char out[1024]) {
  if (!out) {
    return -1;
  }
  out[0] = '\0';

  if (!c || !c->vhost || !request_response_kind_supports_policy(kind)) {
    return 0;
  }

  struct policy_shared_header_ctx ctx;
  policy_shared_collect_headers(c, c->vhost, &ctx);
  if (ctx.overflow) {
    return -1;
  }
  if (ctx.count == 0) {
    return 0;
  }

  size_t off = 0;
  for (unsigned i = 0; i < ctx.count; ++i) {
    if (!ctx.lines[i] || !ctx.lines[i][0]) {
      continue;
    }
    if (request_hdr_appendf(out, 1024, &off, "%s", ctx.lines[i]) != 0) {
      out[0] = '\0';
      return -1;
    }
  }
  return (off > 0) ? 1 : 0;
}

struct request_response_plan request_policy_fail_closed_response_plan(
  struct request_response_plan plan,
  int policy_extra_headers_rc) {
  if (policy_extra_headers_rc < 0) {
    return request_build_response_plan(RK_500,
                                       /*keepalive=*/0,
                                       /*drain_after_headers=*/0,
                                       /*close_after_send=*/1);
  }
  return plan;
}

static int request_build_cors_preflight_headers(const struct cors_policy *cors,
                                                char out[1024]) {
  if (!cors || !out || !cors->enabled || !cors->allow_origin[0]) {
    return -1;
  }

  size_t off = 0;
  out[0] = '\0';

  if (request_hdr_appendf(out,
                          1024,
                          &off,
                          "Access-Control-Allow-Origin: %s\r\n",
                          cors->allow_origin)
      != 0) {
    return -1;
  }
  if (strcmp(cors->allow_origin, "*") != 0) {
    if (request_hdr_appendf(out, 1024, &off, "Vary: Origin\r\n") != 0) {
      return -1;
    }
  }
  if (cors->allow_methods[0]) {
    if (request_hdr_appendf(out,
                            1024,
                            &off,
                            "Access-Control-Allow-Methods: %s\r\n",
                            cors->allow_methods)
        != 0) {
      return -1;
    }
  }
  if (cors->allow_headers[0]) {
    if (request_hdr_appendf(out,
                            1024,
                            &off,
                            "Access-Control-Allow-Headers: %s\r\n",
                            cors->allow_headers)
        != 0) {
      return -1;
    }
  }
  if (cors->allow_credentials) {
    if (request_hdr_appendf(out,
                            1024,
                            &off,
                            "Access-Control-Allow-Credentials: true\r\n")
        != 0) {
      return -1;
    }
  }
  if (cors->max_age_seconds_set && cors->max_age_seconds > 0) {
    if (request_hdr_appendf(out,
                            1024,
                            &off,
                            "Access-Control-Max-Age: %u\r\n",
                            cors->max_age_seconds)
        != 0) {
      return -1;
    }
  }
  return 0;
}

static int request_try_cors_preflight(struct conn *c) {
  if (!c || c->h1.method != HTTP_OPTIONS) {
    return 0;
  }

  const struct vhost_t *vh = c->vhost;
  if (!vh) {
    return 0;
  }

  struct cors_policy cors;
  policy_shared_resolve_effective_cors(c, vh, &cors);
  if (!cors.enabled) {
    return 0;
  }

  uint8_t origin_len = 0;
  const char *origin =
    http_header_find_value(c->h1.req_hdrs, c->h1.req_hdr_count, HDR_ID_ORIGIN, &origin_len);
  uint8_t req_method_len = 0;
  const char *req_method =
    http_header_find_value(c->h1.req_hdrs,
                           c->h1.req_hdr_count,
                           HDR_ID_ACCESS_CONTROL_REQUEST_METHOD,
                           &req_method_len);
  if (!origin || origin_len == 0 || !req_method || req_method_len == 0) {
    return 0;
  }

  char extra_headers[1024];
  if (request_build_cors_preflight_headers(&cors, extra_headers) != 0) {
    return -1;
  }

  const char *buf = NULL;
  size_t len = 0;
  if (tx_build_headers(&c->tx,
                       "204 No Content",
                       /*content_type=*/NULL,
                       /*emit_content_length=*/1,
                       /*content_len=*/0,
                       /*body=*/NULL,
                       /*body_send_len=*/0,
                       /*keepalive=*/0,
                       /*drain_after_headers=*/0,
                       extra_headers,
                       &buf,
                       &len)
      != 0) {
    return -1;
  }

  struct tx_next_io out = {0};
  (void)tx_begin_headers(&c->tx,
                         RK_OK_CLOSE,
                         buf,
                         len,
                         /*keepalive=*/0,
                         /*drain_after_headers=*/0,
                         &out);
  return 1;
}

static const char *request_response_status_line(enum resp_kind kind) {
  switch (kind) {
  case RK_400:
    return "400 Bad Request";
  case RK_403:
    return "403 Forbidden";
  case RK_404:
    return "404 Not Found";
  case RK_405:
    return "405 Method Not Allowed";
  case RK_408:
    return "408 Request Timeout";
  case RK_413:
    return "413 Payload Too Large";
  case RK_431:
    return "431 Request Header Fields Too Large";
  case RK_500:
    return "500 Internal Server Error";
  case RK_501:
    return "501 Not Implemented";
#if ENABLE_OVERLOAD_503
  case RK_503:
    return "503 Service Unavailable";
#endif
  default:
    return NULL;
  }
}

struct response_view request_select_response(enum resp_kind kind, int keepalive) {
  struct response_view rv = {0};
  (void)keepalive;
  switch (kind) {
  case RK_400:
    rv.buf = RESP_400;
    rv.len = RESP_400_len;
    break;
  case RK_403:
    rv.buf = RESP_403;
    rv.len = RESP_403_len;
    break;
  case RK_404:
    rv.buf = RESP_404;
    rv.len = RESP_404_len;
    break;
  case RK_405:
    rv.buf = RESP_405;
    rv.len = RESP_405_len;
    break;
  case RK_413:
    rv.buf = RESP_413;
    rv.len = RESP_413_len;
    break;
  case RK_431:
    rv.buf = RESP_431;
    rv.len = RESP_431_len;
    break;
  case RK_500:
    rv.buf = RESP_500;
    rv.len = RESP_500_len;
    break;
  case RK_501:
    rv.buf = RESP_501;
    rv.len = RESP_501_len;
    break;
  case RK_408:
    rv.buf = RESP_408;
    rv.len = RESP_408_len;
    break;
#if ENABLE_OVERLOAD_503
  case RK_503:
    rv.buf = RESP_503;
    rv.len = RESP_503_len;
    break;
#endif
  default:
    rv.buf = NULL;
    rv.len = 0;
    break;
  }
  return rv;
}

struct request_response_plan request_build_response_plan(enum resp_kind kind,
                                                         int keepalive,
                                                         int drain_after_headers,
                                                         int close_after_send) {
  struct request_response_plan plan;
  plan.kind = kind;
  plan.keepalive = keepalive;
  plan.drain_after_headers = drain_after_headers;
  plan.close_after_send = close_after_send;
  plan.status_line = request_response_status_line(kind);
  plan.response = request_select_response(kind, keepalive);
  return plan;
}

struct request_route_plan request_build_route_plan(const struct conn *c) {
  struct request_route_plan plan = {0};
  if (!c) {
    return plan;
  }

  if (c->h1.method_not_allowed) {
    plan.has_method_not_allowed_response = 1;
    plan.method_not_allowed_response = request_build_response_plan(RK_405,
                                                                   /*keepalive=*/0,
                                                                   /*drain_after_headers=*/0,
                                                                   /*close_after_send=*/1);
    return plan;
  }

  const struct vhost_t *vh = c->vhost;
  if (vh && (vh->features & CFG_FEAT_STATIC) && vh->docroot[0] != '\0' && vh->docroot_fd >= 0
      && !c->h1.path_bad && c->h1.path_norm && c->h1.path_norm_len > 0) {
    plan.try_static = 1;
  }
  return plan;
}

enum resp_kind request_static_open_error_kind(int open_err) {
  if (open_err == EACCES || open_err == EPERM || open_err == ELOOP) {
    return RK_403;
  }
  return RK_404;
}

int request_static_open_err_merge(int current_err, int attempt_err) {
  if (attempt_err > 0) {
    return attempt_err;
  }
  return current_err;
}

int request_static_open_err_finalize(int current_err) {
  if (current_err > 0) {
    return current_err;
  }
  return ENOENT;
}

struct request_response_plan request_build_static_fallback_plan(int open_err) {
  enum resp_kind kind = request_static_open_error_kind(open_err);
  return request_build_response_plan(kind,
                                     /*keepalive=*/0,
                                     /*drain_after_headers=*/0,
                                     /*close_after_send=*/1);
}

struct request_static_outcome request_build_static_outcome(
  const struct request_route_plan *route_plan,
  int static_open_err) {
  struct request_static_outcome out = {0};
  if (!route_plan) {
    return out;
  }

  out.open_attempted = route_plan->try_static ? 1 : 0;
  out.open_err = out.open_attempted ? static_open_err : 0;
  return out;
}

struct request_static_serve_plan request_build_static_serve_plan(const struct conn *c,
                                                                 size_t file_size) {
  const size_t SENDFILE_MIN_BYTES = (size_t)(256 * 1024);
  struct request_static_serve_plan plan = {0};

  if (c && c->h1.method == HTTP_HEAD) {
    plan.mode = REQUEST_STATIC_SERVE_HEAD;
  } else if (file_size > 0 && file_size < SENDFILE_MIN_BYTES) {
    plan.mode = REQUEST_STATIC_SERVE_BUFFERED;
  } else {
    plan.mode = REQUEST_STATIC_SERVE_SENDFILE;
  }
  return plan;
}

struct request_route_apply_plan request_build_route_apply_plan(
  const struct conn *c,
  struct request_response_plan ok_response,
  struct request_static_outcome static_outcome) {
  struct request_route_apply_plan plan = {0};
  struct request_route_plan route = request_build_route_plan(c);
  (void)ok_response;

  plan.send_terminal_response = 1;
  if (route.has_method_not_allowed_response) {
    plan.terminal_response = route.method_not_allowed_response;
  } else if (static_outcome.open_attempted && route.try_static) {
    plan.terminal_response = request_build_static_fallback_plan(static_outcome.open_err);
  } else {
    plan.terminal_response = request_build_response_plan(RK_404,
                                                         /*keepalive=*/0,
                                                         /*drain_after_headers=*/0,
                                                         /*close_after_send=*/1);
  }
  return plan;
}

struct request_ok_dispatch request_dispatch_ok(struct conn *c,
                                               const struct http_ok_plan *okplan) {
  struct request_ok_dispatch result = {0};
  result.kind = REQUEST_OK_NO_RESPONSE;

  if (!c || !okplan) {
    return result;
  }

#if ENABLE_ITEST_ECHO
  if (itest_echo_try_prepare_response(c)) {
    result.kind = REQUEST_OK_TX_BUFFER;
    return result;
  }
#endif

  int preflight = request_try_cors_preflight(c);
  if (preflight > 0) {
    result.kind = REQUEST_OK_TX_BUFFER;
    return result;
  }
  if (preflight < 0) {
    result.kind = REQUEST_OK_HEADER_RESPONSE;
    result.response = request_build_response_plan(RK_500, 0, 0, 1);
    return result;
  }

  struct request_route_plan route_plan = request_build_route_plan(c);
  int static_open_err = 0;

  const struct vhost_t *vh = c->vhost;
  int auth_result = auth_basic_check(c);
  if (auth_result > 0) {
    result.kind = REQUEST_OK_TX_BUFFER;
    return result;
  }
  if (auth_result < 0) {
    result.kind = REQUEST_OK_HEADER_RESPONSE;
    result.response = request_build_response_plan(RK_500, 0, 0, 1);
    return result;
  }
  if (route_plan.try_static) {
    if (static_serve_try_prepare_docroot_response(c, vh->docroot_fd, &static_open_err)) {
      result.kind = REQUEST_OK_TX_BUFFER;
      return result;
    }
  }

  struct request_response_plan ok_response =
    request_build_response_plan(okplan->kind,
                                okplan->keepalive,
                                /*drain_after_headers=*/0,
                                okplan->close_after_send);
  static_open_err = request_static_open_err_finalize(static_open_err);
  struct request_static_outcome static_outcome =
    request_build_static_outcome(&route_plan, static_open_err);
  struct request_route_apply_plan terminal_plan =
    request_build_route_apply_plan(c, ok_response, static_outcome);

  if (!terminal_plan.send_terminal_response) {
    return result;
  }

  result.kind = REQUEST_OK_HEADER_RESPONSE;
  result.response = terminal_plan.terminal_response;
  return result;
}
