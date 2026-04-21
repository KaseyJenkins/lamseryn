#include "include/http_headers.h"

#include "include/conn.h"
#include "include/types.h"

#include <string.h>

struct hdr_spec {
  const char *name;
  uint8_t name_len;
  enum http_header_id id;
};

static const struct hdr_spec g_specs[] = {
  {"host", 4, HDR_ID_HOST},
  {"connection", 10, HDR_ID_CONNECTION},
  {"accept-encoding", 15, HDR_ID_ACCEPT_ENCODING},
  {"range", 5, HDR_ID_RANGE},
  {"if-range", 8, HDR_ID_IF_RANGE},
  {"if-modified-since", 17, HDR_ID_IF_MODIFIED_SINCE},
  {"if-none-match", 13, HDR_ID_IF_NONE_MATCH},
  {"authorization", 13, HDR_ID_AUTHORIZATION},
  {"cookie", 6, HDR_ID_COOKIE},
};

uint64_t http_headers_store_mask(uint64_t features) {
  uint64_t mask = http_header_bit(HDR_ID_HOST) | http_header_bit(HDR_ID_CONNECTION);

  if (features & CFG_FEAT_COMPRESSION) {
    mask |= http_header_bit(HDR_ID_ACCEPT_ENCODING);
  }
  if (features & CFG_FEAT_RANGE) {
    mask |= http_header_bit(HDR_ID_RANGE) | http_header_bit(HDR_ID_IF_RANGE);
  }
  if (features & CFG_FEAT_CONDITIONAL) {
    mask |= http_header_bit(HDR_ID_IF_MODIFIED_SINCE) | http_header_bit(HDR_ID_IF_NONE_MATCH);
  }
  if (features & CFG_FEAT_AUTH) {
    mask |= http_header_bit(HDR_ID_AUTHORIZATION) | http_header_bit(HDR_ID_COOKIE);
  }

  return mask;
}

int http_header_lookup_lower(const char *name, size_t name_len, enum http_header_id *out_id) {
  if (!name || name_len == 0 || !out_id) {
    return -1;
  }

  for (size_t i = 0; i < (sizeof(g_specs) / sizeof(g_specs[0])); ++i) {
    const struct hdr_spec *s = &g_specs[i];
    if ((size_t)s->name_len == name_len && memcmp(name, s->name, name_len) == 0) {
      *out_id = s->id;
      return 0;
    }
  }
  return -1;
}

const char *http_header_find_value(const struct req_hdr_entry *hdrs,
                                   uint8_t hdr_count,
                                   enum http_header_id id,
                                   uint16_t *out_len) {
  if (!hdrs || hdr_count == 0) {
    return NULL;
  }

  for (uint8_t i = 0; i < hdr_count; ++i) {
    const struct req_hdr_entry *e = &hdrs[i];
    if ((enum http_header_id)e->id == id) {
      if (out_len) {
        *out_len = e->value_len;
      }
      return e->value;
    }
  }
  return NULL;
}
