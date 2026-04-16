#include "include/config.h"
#include "include/conn.h"
#include "include/request_handlers.h"
#include "include/http_pipeline.h"
#include "include/static_serve_utils.h"
#include "include/itest_echo.h"
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

  struct request_route_plan route_plan = request_build_route_plan(c);
  int static_open_err = 0;

  const struct vhost_t *vh = c->vhost;
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
