#include <stdint.h>
#include <string.h>

#include "http_parser.h"

#include "include/config.h"
#include "include/conn.h"
#include "include/http_headers.h"
#include "include/http1_limits.h"
#include "include/url.h"

static int on_message_begin(llhttp_t *p) {
  UNUSED(p);
  return 0;
}

static int on_url(llhttp_t *p, const char *at, size_t length) {
  struct conn *c = (struct conn *)p->data;
  if (!c || !at || length == 0) {
    return 0;
  }

  if (c->h1.target_too_long || c->h1.internal_error) {
    return 0;
  }

  size_t have = (size_t)c->h1.target_len;
  size_t need = have + length;
  if (need > (size_t)REQ_TARGET_MAX) {
    c->h1.target_too_long = 1;
    c->h1.parse_error = 1;
    c->h1.want_keepalive = 0;
    return 0;
  }

  if (!c->h1.target || need > (size_t)c->h1.target_cap) {
    size_t new_cap = (size_t)c->h1.target_cap;
    if (new_cap == 0) {
      new_cap = 64;
    }
    while (new_cap < need) {
      size_t doubled = new_cap * 2;
      if (doubled <= new_cap) {
        break;
      }
      new_cap = doubled;
    }
    if (new_cap > (size_t)REQ_TARGET_MAX) {
      new_cap = (size_t)REQ_TARGET_MAX;
    }
    if (new_cap < need) {
      new_cap = need;
    }

    char *buf = (char *)req_arena_alloc(&c->h1.arena, new_cap + 1, 1);
    if (!buf) {
      c->h1.internal_error = 1;
      c->h1.want_keepalive = 0;
      return 0;
    }
    if (c->h1.target && have) {
      memcpy(buf, c->h1.target, have);
    }
    buf[have] = 0;
    c->h1.target = buf;
    c->h1.target_cap = (uint16_t)new_cap;
  }

  memcpy(c->h1.target + have, at, length);
  c->h1.target_len = (uint16_t)(have + length);
  c->h1.target[c->h1.target_len] = 0;
  return 0;
}

void http_parser_reset_header_state(struct conn *c) {
  if (!c) {
    return;
  }
  c->h1.hdr_state = 0;
  c->h1.hdr_interest = 0;
  c->h1.hdr_name_len = 0;
  c->h1.hdr_val_len = 0;
  c->h1.hdr_name[0] = 0;
  c->h1.hdr_val[0] = 0;
  c->h1.hdr_val_raw_len = 0;
  if (sizeof(c->h1.hdr_val_raw)) {
    c->h1.hdr_val_raw[0] = 0;
  }
}

static inline void h1_hdr_append_raw(char *dst,
                                     uint8_t *dst_len,
                                     size_t dst_cap,
                                     const char *src,
                                     size_t n) {
  if (!dst || !dst_len || dst_cap == 0 || !src || n == 0) {
    return;
  }
  size_t len = (size_t)(*dst_len);
  for (size_t i = 0; i < n && len + 1 < dst_cap; ++i) {
    dst[len++] = src[i];
  }
  dst[len] = 0;
  *dst_len = (uint8_t)len;
}

static inline void h1_hdr_append_lower(char *dst,
                                       uint8_t *dst_len,
                                       size_t dst_cap,
                                       const char *src,
                                       size_t n) {
  if (!dst || !dst_len || dst_cap == 0 || !src || n == 0) {
    return;
  }
  size_t len = (size_t)(*dst_len);
  for (size_t i = 0; i < n && len + 1 < dst_cap; ++i) {
    unsigned char ch = (unsigned char)src[i];
    if (ch >= 'A' && ch <= 'Z') {
      ch = (unsigned char)(ch + 32);
    }
    dst[len++] = (char)ch;
  }
  dst[len] = 0;
  *dst_len = (uint8_t)len;
}

static inline int h1_parse_u64_ows_decimal(const char *s, size_t n, uint64_t *out) {
  if (!s || n == 0 || !out) {
    return -1;
  }
  size_t i = 0;
  while (i < n && (s[i] == ' ' || s[i] == '\t')) {
    i++;
  }
  if (i == n) {
    return -1;
  }

  uint64_t v = 0;
  size_t digits = 0;
  for (; i < n; ++i) {
    char cch = s[i];
    if (cch >= '0' && cch <= '9') {
      uint64_t d = (uint64_t)(cch - '0');
      if (v > (UINT64_MAX - d) / 10) {
        return -1;
      }
      v = v * 10 + d;
      digits++;
      continue;
    }
    break;
  }
  if (digits == 0) {
    return -1;
  }
  while (i < n && (s[i] == ' ' || s[i] == '\t')) {
    i++;
  }
  if (i != n) {
    return -1;
  }
  *out = v;
  return 0;
}

// Accept only token list entries equal to "100-continue".
static inline int h1_expect_value_supported(const char *s, size_t n) {
  if (!s) {
    return 0;
  }

  size_t i = 0;
  int saw_100_continue = 0;

  while (i < n) {
    while (i < n && (s[i] == ' ' || s[i] == '\t' || s[i] == ',')) {
      i++;
    }
    if (i >= n) {
      break;
    }

    size_t start = i;
    while (i < n && s[i] != ',' && s[i] != ' ' && s[i] != '\t') {
      i++;
    }
    size_t tok_len = i - start;
    if (tok_len == 0) {
      continue;
    }

    if (tok_len == 12 && memcmp(s + start, "100-continue", 12) == 0) {
      saw_100_continue = 1;
      continue;
    }

    return 0;
  }

  return saw_100_continue;
}

static inline int h1_hdrs_ensure_cap(struct conn *c, uint8_t need) {
  if (!c) {
    return -1;
  }
  if (c->h1.req_hdr_cap >= need) {
    return 0;
  }

  uint8_t new_cap = c->h1.req_hdr_cap ? (uint8_t)(c->h1.req_hdr_cap * 2) : (uint8_t)8;
  if (new_cap < need) {
    new_cap = need;
  }
  if (new_cap > REQ_HDRS_MAX) {
    new_cap = REQ_HDRS_MAX;
  }

  size_t bytes = (size_t)new_cap * sizeof(struct req_hdr_entry);
  struct req_hdr_entry *arr =
    (struct req_hdr_entry *)req_arena_alloc(&c->h1.arena, bytes, _Alignof(struct req_hdr_entry));
  if (!arr) {
    return -1;
  }

  if (c->h1.req_hdrs && c->h1.req_hdr_count) {
    memcpy(arr, c->h1.req_hdrs, (size_t)c->h1.req_hdr_count * sizeof(struct req_hdr_entry));
  }
  c->h1.req_hdrs = arr;
  c->h1.req_hdr_cap = new_cap;
  return 0;
}

static inline void h1_finalize_interested_header(struct conn *c) {
  if (!c || c->h1.internal_error) {
    return;
  }

  http1_header_field_seen(c);
  if (c->h1.header_too_big) {
    return;
  }

  if (c->h1.hdr_name_len > 0 && c->h1.req_hdr_store_mask != 0) {
    enum http_header_id id;
    if (http_header_lookup_lower(c->h1.hdr_name, (size_t)c->h1.hdr_name_len, &id) == 0) {
      uint64_t bit = http_header_bit(id);
      if ((c->h1.req_hdr_store_mask & bit) != 0 && (c->h1.req_hdr_seen_mask & bit) == 0) {
        c->h1.req_hdr_seen_mask |= bit;
        if (c->h1.req_hdr_count < REQ_HDRS_MAX) {
          if (h1_hdrs_ensure_cap(c, (uint8_t)(c->h1.req_hdr_count + 1)) != 0) {
            c->h1.internal_error = 1;
            c->h1.want_keepalive = 0;
            return;
          }

          uint8_t idx = c->h1.req_hdr_count++;
          struct req_hdr_entry *e = &c->h1.req_hdrs[idx];

          uint8_t nlen = c->h1.hdr_name_len;
          if (nlen >= REQ_HDR_NAME_MAX) {
            nlen = (uint8_t)(REQ_HDR_NAME_MAX - 1);
          }
          uint8_t vlen = c->h1.hdr_val_raw_len;
          if (vlen >= REQ_HDR_VALUE_MAX) {
            vlen = (uint8_t)(REQ_HDR_VALUE_MAX - 1);
          }

          char *nm = req_arena_strndup(&c->h1.arena, c->h1.hdr_name, (size_t)nlen);
          char *vv = req_arena_strndup(&c->h1.arena, c->h1.hdr_val_raw, (size_t)vlen);
          if (!nm || !vv) {
            c->h1.internal_error = 1;
            c->h1.want_keepalive = 0;
            c->h1.req_hdr_count--;
            return;
          }

          e->name = nm;
          e->name_len = nlen;
          e->value = vv;
          e->value_len = (uint16_t)vlen;
        }
      }
    }
  }

  if (c->h1.hdr_interest == 1) {
    c->h1.cl_count++;
    if (c->h1.cl_count > 1) {
      c->h1.cl_invalid = 1;
    }

    uint64_t v = 0;
    if (h1_parse_u64_ows_decimal(c->h1.hdr_val, (size_t)c->h1.hdr_val_len, &v) != 0) {
      c->h1.cl_invalid = 1;
    } else {
      if (c->h1.cl_count == 1) {
        c->h1.cl_value = v;
      } else if (c->h1.cl_value != v) {
        c->h1.cl_invalid = 1;
      }
    }
  } else if (c->h1.hdr_interest == 2) {
    c->h1.te_count++;

    const char *s = c->h1.hdr_val;
    size_t n = (size_t)c->h1.hdr_val_len;
    size_t i = 0;
    while (i < n) {
      while (i < n && (s[i] == ' ' || s[i] == '\t' || s[i] == ',')) {
        i++;
      }
      size_t start = i;
      while (i < n && s[i] != ',' && s[i] != ' ' && s[i] != '\t') {
        i++;
      }
      size_t tok_len = (i > start) ? (i - start) : 0;
      if (tok_len == 0) {
        continue;
      }

      if (tok_len == 7 && memcmp(s + start, "chunked", 7) == 0) {
        c->h1.te_chunked = 1;
      } else {
        c->h1.te_other = 1;
      }
    }
  } else if (c->h1.hdr_interest == 3) {
    c->h1.expect_count++;
    if (h1_expect_value_supported(c->h1.hdr_val, (size_t)c->h1.hdr_val_len)) {
      c->h1.expect_100_continue = 1;
    } else {
      c->h1.expect_unsupported = 1;
    }
  }
}

static int on_header_field(llhttp_t *p, const char *at, size_t length) {
  struct conn *c = (struct conn *)p->data;
  if (!c || !at || length == 0) {
    return 0;
  }

  if (c->h1.header_too_big) {
    return 0;
  }

  if (c->h1.hdr_state == 2) {
    h1_finalize_interested_header(c);
    http_parser_reset_header_state(c);
  }
  c->h1.hdr_state = 1;
  h1_hdr_append_lower(c->h1.hdr_name, &c->h1.hdr_name_len, sizeof(c->h1.hdr_name), at, length);
  return 0;
}

static int on_header_value(llhttp_t *p, const char *at, size_t length) {
  struct conn *c = (struct conn *)p->data;
  if (!c || !at || length == 0) {
    return 0;
  }

  if (c->h1.header_too_big) {
    return 0;
  }

  if (c->h1.hdr_state != 2) {
    c->h1.hdr_interest = 0;
    if (c->h1.hdr_name_len == 14 && memcmp(c->h1.hdr_name, "content-length", 14) == 0) {
      c->h1.hdr_interest = 1;
    } else if (c->h1.hdr_name_len == 17 && memcmp(c->h1.hdr_name, "transfer-encoding", 17) == 0) {
      c->h1.hdr_interest = 2;
    } else if (c->h1.hdr_name_len == 6 && memcmp(c->h1.hdr_name, "expect", 6) == 0) {
      c->h1.hdr_interest = 3;
    }

    c->h1.hdr_state = 2;
    c->h1.hdr_val_len = 0;
    c->h1.hdr_val[0] = 0;
    c->h1.hdr_val_raw_len = 0;
    c->h1.hdr_val_raw[0] = 0;
  }

  h1_hdr_append_raw(c->h1.hdr_val_raw,
                    &c->h1.hdr_val_raw_len,
                    sizeof(c->h1.hdr_val_raw),
                    at,
                    length);

  if (c->h1.hdr_interest != 0) {
    h1_hdr_append_lower(c->h1.hdr_val, &c->h1.hdr_val_len, sizeof(c->h1.hdr_val), at, length);
  }

  return 0;
}

static int on_body(llhttp_t *p, const char *at, size_t length) {
  (void)at;
  struct conn *c = (struct conn *)p->data;
  if (!c || length == 0) {
    return 0;
  }

  uint64_t add = (uint64_t)length;
  if (c->h1.body_bytes > UINT64_MAX - add) {
    c->h1.body_too_big = 1;
    c->h1.want_keepalive = 0;
    return 1;
  }
  c->h1.body_bytes += add;
  if (c->h1.body_bytes > (uint64_t)MAX_BODY_BYTES) {
    c->h1.body_too_big = 1;
    c->h1.want_keepalive = 0;
    return 1;
  }
  return 0;
}

static int on_headers_complete(llhttp_t *p) {
  struct conn *c = (struct conn *)p->data;
  if (c) {
    c->h1.headers_done = 1;
    c->h1.want_keepalive = llhttp_should_keep_alive(p) ? 1 : 0;

    c->h1.method = (llhttp_method_t)p->method;
    c->h1.method_set = 1;

    if (c->h1.hdr_state == 2) {
      h1_finalize_interested_header(c);
    }

    c->h1.unsupported_te = 0;
    c->h1.body_remaining = 0;
    c->h1.body_bytes = 0;
    c->h1.body_too_big = 0;
    c->h1.message_done = 0;

    if (c->h1.cl_invalid || c->h1.cl_count > 1) {
      c->h1.parse_error = 1;
      c->h1.want_keepalive = 0;
      return 0;
    }
    if (c->h1.expect_unsupported) {
      c->h1.parse_error = 1;
      c->h1.want_keepalive = 0;
      return 0;
    }
    if (c->h1.te_count > 0 && c->h1.cl_count > 0) {
      c->h1.parse_error = 1;
      c->h1.want_keepalive = 0;
      return 0;
    }

    if (c->h1.te_count > 0) {
      if (c->h1.te_chunked && !c->h1.te_other) {
        c->h1.message_done = 0;
        c->h1.body_remaining = 0;
      } else {
        c->h1.unsupported_te = 1;
        c->h1.want_keepalive = 0;
        return 0;
      }
    }

    if (c->h1.cl_count == 1) {
      if (c->h1.cl_value > (uint64_t)MAX_BODY_BYTES) {
        c->h1.body_too_big = 1;
        c->h1.want_keepalive = 0;
        return 0;
      }
      c->h1.body_remaining = c->h1.cl_value;
      c->h1.message_done = (c->h1.body_remaining == 0);
    } else {
      c->h1.body_remaining = 0;
      c->h1.message_done = 1;
    }

    c->h1.path_bad = 0;
    c->h1.method_not_allowed = 0;
    c->h1.path_len = 0;
    c->h1.query_off = c->h1.target_len;
    c->h1.query_len = 0;
    c->h1.path_dec_len = 0;
    c->h1.path_norm_len = 0;
    c->h1.path_ends_with_slash = 0;

    if (c->h1.method != HTTP_GET && c->h1.method != HTTP_HEAD) {
      c->h1.method_not_allowed = 1;
      c->h1.want_keepalive = 0;
      c->h1.message_done = 1;
      c->h1.body_remaining = 0;
      return 0;
    }

    if (c->h1.internal_error) {
      c->h1.want_keepalive = 0;
      c->h1.message_done = 1;
      return 0;
    }

    if (c->h1.target_len == 0 || c->h1.target_too_long || !c->h1.target) {
      c->h1.parse_error = 1;
      c->h1.want_keepalive = 0;
      c->h1.message_done = 1;
      return 0;
    }

    size_t path_len = 0, qoff = 0, qlen = 0;
    if (url_split_path_query(c->h1.target, (size_t)c->h1.target_len, &path_len, &qoff, &qlen)
        != 0) {
      c->h1.parse_error = 1;
      c->h1.want_keepalive = 0;
      c->h1.message_done = 1;
      return 0;
    }
    if (path_len == 0 || c->h1.target[0] != '/') {
      c->h1.parse_error = 1;
      c->h1.want_keepalive = 0;
      c->h1.message_done = 1;
      return 0;
    }

    c->h1.path_len = (uint16_t)path_len;
    c->h1.query_off = (uint16_t)qoff;
    c->h1.query_len = (uint16_t)qlen;

    if (path_len > (size_t)REQ_PATH_MAX) {
      c->h1.parse_error = 1;
      c->h1.want_keepalive = 0;
      c->h1.message_done = 1;
      return 0;
    }
    size_t path_cap = path_len;
    if (path_cap < 1) {
      path_cap = 1;
    }

    if (!c->h1.path_dec) {
      char *buf = (char *)req_arena_alloc(&c->h1.arena, path_cap + 1, 1);
      if (!buf) {
        c->h1.internal_error = 1;
        c->h1.want_keepalive = 0;
        c->h1.message_done = 1;
        return 0;
      }
      c->h1.path_dec = buf;
      c->h1.path_dec_cap = (uint16_t)path_cap;
      c->h1.path_dec[0] = 0;
    }
    if (!c->h1.path_norm) {
      char *buf = (char *)req_arena_alloc(&c->h1.arena, path_cap + 1, 1);
      if (!buf) {
        c->h1.internal_error = 1;
        c->h1.want_keepalive = 0;
        c->h1.message_done = 1;
        return 0;
      }
      c->h1.path_norm = buf;
      c->h1.path_norm_cap = (uint16_t)path_cap;
      c->h1.path_norm[0] = 0;
    }

    size_t dec_len = 0;
    int drc = url_percent_decode_strict(c->h1.target,
                                        path_len,
                                        c->h1.path_dec,
                                        (size_t)c->h1.path_dec_cap,
                                        &dec_len,
                                        /*reject_decoded_slashes=*/1);
    if (drc != 0) {
      c->h1.parse_error = 1;
      c->h1.want_keepalive = 0;
      c->h1.message_done = 1;
      return 0;
    }
    c->h1.path_dec_len = (uint16_t)dec_len;
    if (dec_len <= (size_t)c->h1.path_dec_cap) {
      c->h1.path_dec[dec_len] = 0;
    }

    size_t norm_len = 0;
    int ends_slash = 0;
    int nrc = url_path_normalize_absolute(c->h1.path_dec,
                                          dec_len,
                                          c->h1.path_norm,
                                          (size_t)c->h1.path_norm_cap,
                                          &norm_len,
                                          &ends_slash);
    if (nrc != 0) {
      c->h1.parse_error = 1;
      c->h1.want_keepalive = 0;
      c->h1.message_done = 1;
      return 0;
    }
    c->h1.path_norm_len = (uint16_t)norm_len;
    if (norm_len <= (size_t)c->h1.path_norm_cap) {
      c->h1.path_norm[norm_len] = 0;
    }
    c->h1.path_ends_with_slash = (uint8_t)(ends_slash ? 1 : 0);
  }
  return 0;
}

static int on_message_complete(llhttp_t *p) {
  struct conn *c = (struct conn *)p->data;
  if (c) {
    c->h1.message_done = 1;
  }
  return HPE_PAUSED;
}

void http_parser_settings_assign_server(llhttp_settings_t *s) {
  http_parser_settings_assign(s,
                              on_message_begin,
                              on_url,
                              on_header_field,
                              on_header_value,
                              on_body,
                              on_headers_complete,
                              on_message_complete);
}
