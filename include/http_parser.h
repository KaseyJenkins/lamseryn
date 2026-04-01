#pragma once
#include <llhttp.h>
#include <string.h>

struct conn;

// Initialize an llhttp parser instance for server use.

static inline void http_parser_init(llhttp_t *parser, const llhttp_settings_t *settings) {
  llhttp_init(parser, HTTP_BOTH, settings);
}

// Feed bytes into llhttp and return parser status.
static inline enum llhttp_errno http_parser_feed(llhttp_t *parser, const char *data, size_t len) {
  if (len == 0 || data == NULL) {
    return HPE_OK;
  }
  return llhttp_execute(parser, data, len);
}

// Assign the callback set used by this server.
static inline void http_parser_settings_assign(llhttp_settings_t *s,
                                               llhttp_cb on_message_begin,
                                               llhttp_data_cb on_url,
                                               llhttp_data_cb on_header_field,
                                               llhttp_data_cb on_header_value,
                                               llhttp_data_cb on_body,
                                               llhttp_cb on_headers_complete,
                                               llhttp_cb on_message_complete) {
  memset(s, 0, sizeof(*s));
  s->on_message_begin = on_message_begin;
  s->on_url = on_url;
  s->on_header_field = on_header_field;
  s->on_header_value = on_header_value;
  s->on_body = on_body;
  s->on_headers_complete = on_headers_complete;
  s->on_message_complete = on_message_complete;
}

// Reset per-request header-accumulator state used by callbacks.
void http_parser_reset_header_state(struct conn *c);

// Assign the server's llhttp callback set.
void http_parser_settings_assign_server(llhttp_settings_t *s);
