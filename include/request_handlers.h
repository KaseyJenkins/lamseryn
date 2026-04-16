#pragma once
#include <stddef.h>
#include "include/types.h"

struct response_view {
  const char *buf;
  size_t len;
};

struct request_response_plan {
  enum resp_kind kind;
  int keepalive;
  int drain_after_headers;
  int close_after_send;
  struct response_view response;
};

struct request_route_plan {
  int try_static;
  int has_method_not_allowed_response;
  struct request_response_plan method_not_allowed_response;
};

struct request_route_apply_plan {
  int send_terminal_response;
  struct request_response_plan terminal_response;
};

struct request_static_outcome {
  int open_attempted;
  int open_err;
};

enum request_static_serve_mode {
  REQUEST_STATIC_SERVE_HEAD = 1,
  REQUEST_STATIC_SERVE_BUFFERED,
  REQUEST_STATIC_SERVE_SENDFILE,
};

struct request_static_serve_plan {
  enum request_static_serve_mode mode;
};

// Select static response buffer based on kind and keep-alive flag.
struct response_view request_select_response(enum resp_kind kind, int keepalive);

// Build a transport-ready response plan for canned/header-only responses.
struct request_response_plan request_build_response_plan(enum resp_kind kind,
                                                         int keepalive,
                                                         int drain_after_headers,
                                                         int close_after_send);

// Build routing gate decisions for request handling.
struct request_route_plan request_build_route_plan(const struct conn *c);

// Map static-file open errors to fallback response kind.
enum resp_kind request_static_open_error_kind(int open_err);

// Merge raw static-open attempt errors into a stable normalized state.
int request_static_open_err_merge(int current_err, int attempt_err);

// Finalize static-open error state for fallback planning after static attempts.
int request_static_open_err_finalize(int current_err);

// Build static-file fallback response plan from open error mapping.
struct request_response_plan request_build_static_fallback_plan(int open_err);

// Shape static-route open-attempt outcomes for terminal response planning.
struct request_static_outcome request_build_static_outcome(
  const struct request_route_plan *route_plan,
  int static_open_err);

// Build static-file serve mode decision from request method and file size.
struct request_static_serve_plan request_build_static_serve_plan(const struct conn *c,
                                                                 size_t file_size);

// Build terminal response selection for method-not-allowed, static fallback,
// and default OK response handling.
struct request_route_apply_plan request_build_route_apply_plan(
  const struct conn *c,
  struct request_response_plan ok_response,
  struct request_static_outcome static_outcome);

// OK-path dispatch result kinds.
enum request_ok_dispatch_kind {
  REQUEST_OK_NO_RESPONSE = 0,
  REQUEST_OK_TX_BUFFER,
  REQUEST_OK_HEADER_RESPONSE,
};

struct request_ok_dispatch {
  enum request_ok_dispatch_kind kind;
  struct request_response_plan response;
};

struct http_ok_plan;

// Dispatch a successful (HP_APPLY_OK) request: try itest echo, static-serve,
// and terminal response resolution. Returns what the caller should send.
struct request_ok_dispatch request_dispatch_ok(struct conn *c,
                                               const struct http_ok_plan *okplan);
