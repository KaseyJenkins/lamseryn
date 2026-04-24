#include "include/http_range.h"

#include <stdio.h>
#include <string.h>

// Parse an unsigned decimal integer and advance *pos.
// Returns -1 on empty/non-digit/overflow.
static int parse_u64(const char *s, size_t len, size_t *pos, uint64_t *out) {
  size_t start = *pos;
  uint64_t val = 0;

  while (*pos < len && s[*pos] >= '0' && s[*pos] <= '9') {
    uint64_t digit = (uint64_t)(s[*pos] - '0');
    // Overflow check: val > UINT64_MAX / 10 or val * 10 + digit wraps.
    if (val > UINT64_MAX / 10 || (val == UINT64_MAX / 10 && digit > UINT64_MAX % 10)) {
      return -1;
    }
    val = val * 10 + digit;
    (*pos)++;
  }

  if (*pos == start) {
    return -1; // no digits consumed
  }

  *out = val;
  return 0;
}

struct byte_range http_range_parse(const char *value, size_t value_len) {
  struct byte_range br = {0};

  if (!value || value_len == 0) {
    return br;
  }

  // Accept only the "bytes=" unit (RFC 7233 §2.1).
  if (value_len < 6) {
    return br;
  }
  if ((value[0] != 'b' && value[0] != 'B')
      || (value[1] != 'y' && value[1] != 'Y')
      || (value[2] != 't' && value[2] != 'T')
      || (value[3] != 'e' && value[3] != 'E')
      || (value[4] != 's' && value[4] != 'S')
      || value[5] != '=') {
    return br;
  }

  size_t pos = 6;

  // Skip optional whitespace after '='.
  while (pos < value_len && (value[pos] == ' ' || value[pos] == '\t')) {
    pos++;
  }

  if (pos >= value_len) {
    return br;
  }

  // Multi-range is unsupported; caller falls back to full 200.
  for (size_t i = pos; i < value_len; i++) {
    if (value[i] == ',') {
      return br; // multi-range not supported
    }
  }

  // Suffix form: bytes=-N
  if (value[pos] == '-') {
    pos++;
    uint64_t suffix_len = 0;
    if (parse_u64(value, value_len, &pos, &suffix_len) != 0) {
      return br;
    }
    // Reject trailing junk.
    if (pos != value_len) {
      return br;
    }
    // bytes=-0 is unsatisfiable.
    if (suffix_len == 0) {
      return br;
    }
    br.valid = 1;
    br.suffix = 1;
    br.first = suffix_len;
    br.last = 0; // unused for suffix
    return br;
  }

  uint64_t first = 0;
  if (parse_u64(value, value_len, &pos, &first) != 0) {
    return br;
  }

  // Require the '-' separator.
  if (pos >= value_len || value[pos] != '-') {
    return br;
  }
  pos++;

  // Open-ended form: bytes=N-
  if (pos == value_len) {
    br.valid = 1;
    br.suffix = 0;
    br.first = first;
    br.last = UINT64_MAX;
    return br;
  }

  // Closed form: bytes=N-M
  uint64_t last = 0;
  if (parse_u64(value, value_len, &pos, &last) != 0) {
    return br;
  }

  // Reject trailing junk.
  if (pos != value_len) {
    return br;
  }

  // Invalid if first > last.
  if (first > last) {
    return br;
  }

  br.valid = 1;
  br.suffix = 0;
  br.first = first;
  br.last = last;
  return br;
}

struct resolved_range http_range_resolve(const struct byte_range *br, uint64_t file_size) {
  struct resolved_range rr = {0};

  if (!br || !br->valid || file_size == 0) {
    return rr;
  }

  if (br->suffix) {
    // Serve the trailing N bytes.
    if (br->first >= file_size) {
      // Suffix larger than file means full entity.
      rr.satisfiable = 1;
      rr.start = 0;
      rr.end = file_size - 1;
      rr.length = file_size;
    } else {
      rr.satisfiable = 1;
      rr.start = file_size - br->first;
      rr.end = file_size - 1;
      rr.length = rr.end - rr.start + 1;
    }
    return rr;
  }

  // Start offset must be within file bounds.
  if (br->first >= file_size) {
    return rr; // unsatisfiable
  }

  rr.satisfiable = 1;
  rr.start = br->first;

  if (br->last == UINT64_MAX || br->last >= file_size) {
    // Clamp open-ended or oversized end to EOF.
    rr.end = file_size - 1;
  } else {
    rr.end = br->last;
  }

  rr.length = rr.end - rr.start + 1;
  return rr;
}

size_t http_range_format_content_range(char *buf,
                                       size_t cap,
                                       uint64_t start,
                                       uint64_t end,
                                       uint64_t total) {
  if (!buf || cap == 0) {
    return 0;
  }
  int n = snprintf(buf, cap, "Content-Range: bytes %llu-%llu/%llu\r\n",
                   (unsigned long long)start,
                   (unsigned long long)end,
                   (unsigned long long)total);
  if (n <= 0 || (size_t)n >= cap) {
    return 0;
  }
  return (size_t)n;
}

size_t http_range_format_content_range_unsatisfied(char *buf,
                                                   size_t cap,
                                                   uint64_t total) {
  if (!buf || cap == 0) {
    return 0;
  }
  int n = snprintf(buf, cap, "Content-Range: bytes */%llu\r\n",
                   (unsigned long long)total);
  if (n <= 0 || (size_t)n >= cap) {
    return 0;
  }
  return (size_t)n;
}
