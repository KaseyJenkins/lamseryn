#include "include/http_boundary.h"

#include <string.h>

ssize_t http_find_header_terminator_end(const char *prefix,
                                        size_t prefix_len,
                                        const char *buf,
                                        size_t n) {
  // Scan combined stream using a 4-byte sliding window.
  if ((!prefix || prefix_len == 0) && (!buf || n == 0)) {
    return -1;
  }

  const size_t total = prefix_len + n;
  if (total < 4) {
    return -1;
  }

  char w0 = 0, w1 = 0, w2 = 0, w3 = 0;
  for (size_t i = 0; i < total; ++i) {
    char c;
    if (i < prefix_len) {
      c = prefix[i];
    } else {
      c = buf[i - prefix_len];
    }

    w0 = w1;
    w1 = w2;
    w2 = w3;
    w3 = c;

    if (i >= 3) {
      if (w0 == '\r' && w1 == '\n' && w2 == '\r' && w3 == '\n') {
        return (ssize_t)(i + 1);
      }
    }
  }

  return -1;
}
