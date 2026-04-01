#pragma once

#include <stddef.h>
#include <stdint.h>

// Feature flags are stored in config_t::features (see include/types.h).

enum http_header_id {
  HDR_ID_HOST = 0,
  HDR_ID_CONNECTION,
  HDR_ID_ACCEPT_ENCODING,
  HDR_ID_RANGE,
  HDR_ID_IF_RANGE,
  HDR_ID_IF_MODIFIED_SINCE,
  HDR_ID_IF_NONE_MATCH,
  HDR_ID_AUTHORIZATION,
  HDR_ID_COOKIE,

  HDR_ID__COUNT
};

// Bitset for header ids (supports up to 64 ids).
static inline uint64_t http_header_bit(enum http_header_id id) {
  return (id < 64) ? (1ULL << (unsigned)id) : 0ULL;
}

// Returns a bitset of headers the server should store for the given enabled features.
// Notes:
// - This is only about what we *store for later use*.
// - Protocol-safety headers (Content-Length/Transfer-Encoding) are still parsed/enforced elsewhere.
uint64_t http_headers_store_mask(uint64_t features);

// Lookup a header id by name.
// Expects header names in lowercase ASCII (the parser already lowercases).
// Returns 0 on match, -1 if unknown.
int http_header_lookup_lower(const char *name, size_t name_len, enum http_header_id *out_id);
