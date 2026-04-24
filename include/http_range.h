#pragma once

#include <stddef.h>
#include <stdint.h>

// Parsed single-byte Range header value.
// Invalid or multi-range values leave valid=0.
struct byte_range {
  int valid;   // 1 when parsed successfully
  int suffix;  // 1 for suffix form: bytes=-N
  uint64_t first; // first byte, or suffix length when suffix=1
  uint64_t last;  // last byte, UINT64_MAX for open-ended
};

// Parsed range resolved against file size.
struct resolved_range {
  int satisfiable; // 0 means unsatisfiable (416)
  uint64_t start;  // absolute start byte
  uint64_t end;    // absolute end byte (inclusive)
  uint64_t length; // byte count
};

// Parse a Range header value (example: "bytes=0-499").
// Returns valid=0 on parse failure or multi-range.
struct byte_range http_range_parse(const char *value, size_t value_len);

// Resolve a parsed byte_range against a known file size.
// Returns satisfiable=0 if the range cannot be served.
struct resolved_range http_range_resolve(const struct byte_range *br, uint64_t file_size);

// Write a successful Content-Range header line.
// Returns bytes written (excluding NUL), or 0 on failure.
size_t http_range_format_content_range(char *buf,
                                       size_t cap,
                                       uint64_t start,
                                       uint64_t end,
                                       uint64_t total);

// Write an unsatisfied Content-Range header line.
// Returns bytes written (excluding NUL), or 0 on failure.
size_t http_range_format_content_range_unsatisfied(char *buf,
                                                   size_t cap,
                                                   uint64_t total);
