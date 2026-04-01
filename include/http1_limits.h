#pragma once

struct conn;

// Count ALL parsed header fields (not just stored subset) and enforce
// a LimitRequestFields-style cap.
//
// When the cap is exceeded, this sets:
//   - c->h1.header_too_big = 1 (maps to 431)
//   - c->h1.header_fields_too_many = 1
//   - c->h1.want_keepalive = 0
//   - pauses llhttp parser
//
// The caller is expected to call this once per completed header field.
void http1_header_field_seen(struct conn *c);
