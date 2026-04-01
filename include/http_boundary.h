#pragma once

#include <stddef.h>
#include <sys/types.h>

// Find the end of HTTP/1 headers ("\r\n\r\n") in a byte stream that is the
// concatenation of:
//   prefix[0..prefix_len) + buf[0..n)
//
// Returns:
//  - >= 0: the combined-stream index (0-based) of the first byte AFTER the
//          terminator (i.e. exclusive end offset).
//  - -1:   not found.
//
// This is used to boundary-limit llhttp feeds so it cannot consume bytes from
// the next pipelined request.
ssize_t http_find_header_terminator_end(const char *prefix,
                                        size_t prefix_len,
                                        const char *buf,
                                        size_t n);
