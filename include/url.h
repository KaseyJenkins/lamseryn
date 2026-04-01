#pragma once

#include <stddef.h>
#include <stdint.h>

// Split an origin-form request target into path and query.
// target is not required to be NUL-terminated.
//
// Outputs:
// - *path_len: number of bytes in the path (before '?')
// - *query_off: offset of query string (after '?') or == target_len if none
// - *query_len: length of query string (0 if none)
int url_split_path_query(const char *target,
                         size_t target_len,
                         size_t *path_len,
                         size_t *query_off,
                         size_t *query_len);

// Strict percent-decode of a URL path segment.
// - Accepts only valid %HH sequences.
// - Decodes '+' literally (does NOT turn it into space).
// - Rejects decoded NUL.
// - Optionally rejects decoded '/' and '\\' (to prevent encoded-slash traversal tricks).
//
// Returns 0 on success, -1 on invalid encoding or forbidden decoded byte,
// -2 on output overflow.
int url_percent_decode_strict(const char *in,
                              size_t in_len,
                              char *out,
                              size_t out_cap,
                              size_t *out_len,
                              int reject_decoded_slashes);

// Normalize an absolute URL path (must start with '/').
// - Collapses multiple '/'
// - Resolves '.' and '..' segments
// - Preserves whether the original path ended with '/' via *ends_with_slash
//
// Output is always absolute (starts with '/'), with no trailing slash unless
// the normalized path is '/'.
//
// Returns 0 on success, -1 on invalid input, -2 on output overflow.
int url_path_normalize_absolute(const char *path,
                                size_t path_len,
                                char *out,
                                size_t out_cap,
                                size_t *out_len,
                                int *ends_with_slash);
