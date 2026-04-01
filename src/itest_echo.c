#include "include/itest_echo.h"

#include "include/conn.h"
#include "include/static_serve_utils.h"

#include <llhttp.h>
#include <stdio.h>
#include <string.h>

int itest_echo_try_prepare_response(struct conn *c) {
  if (!c) {
    return 0;
  }

  if (c->h1.path_bad || c->h1.path_norm_len != 13
      || memcmp(c->h1.path_norm, "/__itest/echo", 13) != 0) {
    return 0;
  }

  const char *mname = llhttp_method_name(c->h1.method);
  if (!mname) {
    mname = "?";
  }

  char body[4096];
  size_t boff = 0;

  boff += (size_t)snprintf(body + boff, sizeof(body) - boff, "method=%s\n", mname);
  boff += (size_t)snprintf(body + boff,
                           sizeof(body) - boff,
                           "target=%.*s\n",
                           (int)c->h1.target_len,
                           c->h1.target);
  boff += (size_t)snprintf(body + boff,
                           sizeof(body) - boff,
                           "path_norm=%.*s\n",
                           (int)c->h1.path_norm_len,
                           c->h1.path_norm);
  if (c->h1.query_len > 0) {
    boff += (size_t)snprintf(body + boff,
                             sizeof(body) - boff,
                             "query=%.*s\n",
                             (int)c->h1.query_len,
                             c->h1.target + c->h1.query_off);
  } else {
    boff += (size_t)snprintf(body + boff, sizeof(body) - boff, "query=\n");
  }

  boff += (size_t)snprintf(body + boff,
                           sizeof(body) - boff,
                           "hdr_fields_count=%u\n",
                           (unsigned)c->h1.hdr_fields_count);
  boff += (size_t)snprintf(body + boff,
                           sizeof(body) - boff,
                           "stored_hdr_count=%u\n",
                           (unsigned)c->h1.req_hdr_count);

  for (uint8_t i = 0; i < c->h1.req_hdr_count && boff + 64 < sizeof(body); ++i) {
    boff += (size_t)snprintf(body + boff,
                             sizeof(body) - boff,
                             "hdr[%.*s]=%.*s\n",
                             (int)c->h1.req_hdrs[i].name_len,
                             c->h1.req_hdrs[i].name,
                             (int)c->h1.req_hdrs[i].value_len,
                             c->h1.req_hdrs[i].value);
  }

  int keep = c->h1.want_keepalive ? 1 : 0;
  const void *body_ptr = body;
  size_t body_send_len = boff;

  if (c->h1.method == HTTP_HEAD) {
    body_ptr = NULL;
    body_send_len = 0;
  }

  if (static_serve_tx_set_dynamic_response_ex(c,
                                              "200 OK",
                                              "text/plain; charset=utf-8",
                                              boff,
                                              body_ptr,
                                              body_send_len,
                                              keep)
      == 0) {
    return 1;
  }

  return 0;
}
