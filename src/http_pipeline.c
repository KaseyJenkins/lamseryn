#include <string.h>

#include <llhttp.h>

#include "include/macros.h"
#include "include/conn.h"
#include "http_parser.h"
#include "include/http_pipeline.h"
#include "include/http1_limits.h"
#include "include/logger.h"

static inline int has_lf(const char *buf, size_t n) {
  for (size_t i = 0; i < n; ++i) {
    if (buf[i] == '\n') {
      return 1;
    }
  }
  return 0;
}

// Decide whether to tolerate an llhttp error during header parsing.
// Sticky behavior: once we flag a bad header line, tolerate until LF appears, then escalate to 400.
static inline int llhttp_should_tolerate(struct conn *c,
                                         enum llhttp_errno err,
                                         const char *buf,
                                         size_t n) {
  if (c->h1.headers_done) {
    return 0;
  }

  if (err == HPE_CR_EXPECTED || err == HPE_LF_EXPECTED) {
    return 1;
  }

  if (err == HPE_INVALID_HEADER_TOKEN
#ifdef HPE_INVALID_HEADER_VALUE_CHAR
      || err == HPE_INVALID_HEADER_VALUE_CHAR
#endif
  ) {
    int haslf = has_lf(buf, n);
    if (c->h1.pending_line_error == 0) {
      c->h1.pending_line_error = 1;
      return 1;
    }
    return !haslf;
  }

  return 0;
}

enum http_action http_pipeline_classify_action(const struct conn *c) {
  if (c->h1.header_too_big && !c->dl.draining) {
    return HP_ACTION_RESP_431;
  }
  if (c->h1.unsupported_te) {
    return HP_ACTION_RESP_501;
  }
  if (c->h1.body_too_big) {
    return HP_ACTION_RESP_413;
  }
  if (c->h1.parse_error) {
    return HP_ACTION_RESP_400;
  }
  if (!c->h1.message_done) {
    return HP_ACTION_CONTINUE;
  }
  return HP_ACTION_RESP_OK;
}

int http_pipeline_error_plan(enum http_action action, struct http_error_plan *out) {
  if (!out) {
    return 0;
  }

  memset(out, 0, sizeof(*out));

  switch (action) {
  case HP_ACTION_RESP_431:
    out->kind = RK_431;
    out->keepalive = 0;
    out->drain_after_headers = 1;
    out->close_after_send = 0;
    return 1;
  case HP_ACTION_RESP_400:
    out->kind = RK_400;
    out->keepalive = 0;
    out->drain_after_headers = 0;
    out->close_after_send = 1;
    return 1;
  case HP_ACTION_RESP_413:
    out->kind = RK_413;
    out->keepalive = 0;
    out->drain_after_headers = 0;
    out->close_after_send = 1;
    return 1;
  case HP_ACTION_RESP_501:
    out->kind = RK_501;
    out->keepalive = 0;
    out->drain_after_headers = 0;
    out->close_after_send = 1;
    return 1;
  default:
    return 0;
  }
}

void http_pipeline_internal_error_plan(struct http_error_plan *out) {
  if (!out) {
    return;
  }

  memset(out, 0, sizeof(*out));
  out->kind = RK_500;
  out->keepalive = 0;
  out->drain_after_headers = 0;
  out->close_after_send = 1;
}

struct http_ok_plan http_pipeline_ok_plan(const struct conn *c) {
  struct http_ok_plan out;
  memset(&out, 0, sizeof(out));

  if (c && c->h1.method_not_allowed) {
    out.kind = RK_405;
    out.keepalive = 0;
    out.close_after_send = 1;
    return out;
  }

  out.keepalive = (c && c->h1.want_keepalive) ? 1 : 0;
  out.kind = out.keepalive ? RK_OK_KA : RK_OK_CLOSE;
  out.close_after_send = 0;
  return out;
}

struct http_apply_plan http_pipeline_build_apply_plan(const struct conn *c,
                                                      const struct http_pipeline_result *hres) {
  struct http_apply_plan out;
  memset(&out, 0, sizeof(out));
  out.kind = HP_APPLY_CONTINUE;

  if (c && c->h1.internal_error) {
    out.kind = HP_APPLY_INTERNAL_ERROR;
    http_pipeline_internal_error_plan(&out.error);
    return out;
  }

  enum http_action act = hres ? hres->action : HP_ACTION_CONTINUE;
  out.reschedule_on_ok = (act == HP_ACTION_RESP_OK) ? 1 : 0;

  if (http_pipeline_error_plan(act, &out.error)) {
    out.kind = HP_APPLY_ERROR;
    return out;
  }

  if (act == HP_ACTION_CONTINUE) {
    out.kind = HP_APPLY_CONTINUE;
    return out;
  }

  out.kind = HP_APPLY_OK;
  out.ok = http_pipeline_ok_plan(c);
  return out;
}

struct http_pipeline_result http_pipeline_feed(struct conn *c, const char *buf, size_t n) {
  struct http_pipeline_result r;
  memset(&r, 0, sizeof(r));
  r.action = HP_ACTION_CONTINUE;
  r.err = HPE_OK;

  if (!c || !buf || n == 0) {
    return r;
  }

  int had_header_too_big = c->h1.header_too_big;
  int had_parse_error = c->h1.parse_error;
  int had_headers_done = c->h1.headers_done;

  // Stop parsing once this request reaches terminal state.
  if (!c->h1.parse_error && !c->h1.header_too_big && !c->h1.unsupported_te && !c->h1.message_done) {
    if (!c->h1.headers_done) {
      c->h1.parser_bytes += n;
      if (c->h1.parser_bytes > HEADER_CAP) {
        c->h1.header_too_big = 1;
        r.header_too_big_transition = !had_header_too_big;
      }
    }

    if (!c->h1.header_too_big) {
      enum llhttp_errno err = http_parser_feed(&c->h1.parser, buf, n);
      r.err = err;
      if (err != HPE_OK && err != HPE_PAUSED) {
        int tolerate = llhttp_should_tolerate(c, err, buf, n);
        if (tolerate) {
          r.tolerated_error = 1;
        } else if (!c->h1.body_too_big) {
          c->h1.parse_error = 1;
          r.parse_error_transition = !had_parse_error;
        }
      }
    }

    if (!had_headers_done && c->h1.headers_done) {
      c->h1.pending_line_error = 0;
      r.headers_complete_transition = 1;
    }
  }

  r.action = http_pipeline_classify_action(c);

  return r;
}

void http_pipeline_log_transitions(const struct conn *c,
                                   const struct http_pipeline_result *hres,
                                   const char *chunk,
                                   size_t chunk_len) {
  if (!c || !hres) {
    return;
  }

  if (hres->header_too_big_transition) {
    const char *reason = c->h1.header_fields_too_many ? "fields" : "bytes";
    size_t cap = c->h1.header_fields_too_many ? (size_t)c->h1.hdr_fields_max : (size_t)HEADER_CAP;
    size_t parsed =
      c->h1.header_fields_too_many ? (size_t)c->h1.hdr_fields_count : c->h1.parser_bytes;
    LOGW(LOGC_HTTP,
         "fd=%u gen=%u: 431 reason=%s cap=%zu parsed=%zu headers_done=%d",
         (unsigned)c->fd,
         (unsigned)c->generation,
         reason,
         cap,
         parsed,
         c->h1.headers_done);
  }

  if (hres->parse_error_transition) {
    const char *ename = llhttp_errno_name(hres->err);
    const char *reason = llhttp_get_error_reason(&c->h1.parser);
    size_t preview = chunk_len < 64 ? chunk_len : 64;
    LOGW(LOGC_HTTP,
         "400 llhttp_err=%d(%s) reason=\"%s\" bytes=%zu headers_done=%d "
         "chunk_len=%zu preview=\"%.*s\" rx_stash_len=%u rx_tail_len=%u "
         "closing=%d draining=%d keepalive=%d resp_kind=%d",
         (int)hres->err,
         ename ? ename : "?",
         reason ? reason : "?",
         c->h1.parser_bytes,
         c->h1.headers_done,
         (size_t)chunk_len,
         (int)preview,
         chunk ? chunk : "",
         (unsigned)c->rx_stash_len,
         (unsigned)c->rx_tail_len,
         c->dl.closing,
         c->dl.draining,
         c->tx.keepalive,
         (int)c->tx.resp_kind);
  } else if (hres->tolerated_error && hres->err != HPE_OK) {
    const char *ename = llhttp_errno_name(hres->err);
    const char *reason = llhttp_get_error_reason(&c->h1.parser);
    LOGD_EVERY_N(LOGC_HTTP,
                 64,
                 "llhttp transient err=%d(%s) reason=\"%s\" bytes=%zu",
                 (int)hres->err,
                 ename ? ename : "?",
                 reason ? reason : "?",
                 c->h1.parser_bytes);
  }
}
