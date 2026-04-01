#include "include/http1_limits.h"

#include <llhttp.h>

#include "include/conn.h"

void http1_header_field_seen(struct conn *c) {
  if (!c) {
    return;
  }
  if (c->h1.header_too_big) {
    return;
  }

  if (c->h1.hdr_name_len == 0) {
    return;
  }

  if (c->h1.hdr_fields_count != UINT16_MAX) {
    c->h1.hdr_fields_count++;
  }

  if (c->h1.hdr_fields_max != 0 && c->h1.hdr_fields_count > c->h1.hdr_fields_max) {
    c->h1.header_too_big = 1;
    c->h1.header_fields_too_many = 1;
    c->h1.want_keepalive = 0;
    llhttp_pause(&c->h1.parser);
  }
}
