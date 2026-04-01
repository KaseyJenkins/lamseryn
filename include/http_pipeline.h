#pragma once

#include <stddef.h>
#include <llhttp.h>

#include "types.h"

struct conn;

// High-level HTTP/1 parsing actions for the read path.
enum http_action {
  HP_ACTION_CONTINUE = 0,
  HP_ACTION_RESP_431,
  HP_ACTION_RESP_400,
  HP_ACTION_RESP_413,
  HP_ACTION_RESP_501,
  HP_ACTION_RESP_OK,
};

// Result of feeding a chunk of bytes into the HTTP pipeline.
// Contains both the classified action and transition hints for instrumentation.
struct http_pipeline_result {
  enum http_action action;

  int header_too_big_transition; // 1 if header_too_big flipped 0->1
  int parse_error_transition; // 1 if parse_error flipped 0->1
  int headers_complete_transition; // 1 if headers_done flipped 0->1

  int tolerated_error; // 1 if an llhttp error occurred but was tolerated
  enum llhttp_errno err; // last llhttp error seen for this chunk (HPE_OK if none)
};

struct http_error_plan {
  enum resp_kind kind;
  int keepalive;
  int drain_after_headers;
  int close_after_send;
};

struct http_ok_plan {
  enum resp_kind kind;
  int keepalive;
  int close_after_send;
};

enum http_apply_kind {
  HP_APPLY_CONTINUE = 0,
  HP_APPLY_ERROR,
  HP_APPLY_OK,
  HP_APPLY_INTERNAL_ERROR,
};

struct http_apply_plan {
  enum http_apply_kind kind;
  int reschedule_on_ok;
  struct http_error_plan error;
  struct http_ok_plan ok;
};

// Classify next action from current connection parser state.
enum http_action http_pipeline_classify_action(const struct conn *c);

// Build response policy for terminal error actions.
// Returns 1 if action is an error action with a plan, 0 otherwise.
int http_pipeline_error_plan(enum http_action action, struct http_error_plan *out);

// Build response policy for internal server errors.
void http_pipeline_internal_error_plan(struct http_error_plan *out);

// Build response policy for HP_ACTION_RESP_OK branch.
struct http_ok_plan http_pipeline_ok_plan(const struct conn *c);

// Build an apply-time plan from connection state and classified action.
// Encodes precedence: internal_error > error actions > continue > ok.
struct http_apply_plan http_pipeline_build_apply_plan(const struct conn *c,
                                                      const struct http_pipeline_result *hres);

// Feed bytes into the HTTP/1 parser associated with this connection and
// classify the next action. Updates struct conn::h1 in-place.
struct http_pipeline_result http_pipeline_feed(struct conn *c, const char *buf, size_t n);
