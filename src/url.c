#include "url.h"

#include <ctype.h>
#include <string.h>

int url_split_path_query(const char *target,
                         size_t target_len,
                         size_t *path_len,
                         size_t *query_off,
                         size_t *query_len) {
  if (!target || !path_len || !query_off || !query_len) {
    return -1;
  }

  size_t q = target_len;
  for (size_t i = 0; i < target_len; ++i) {
    if (target[i] == '?') {
      q = i;
      break;
    }
  }

  *path_len = q;
  if (q == target_len) {
    *query_off = target_len;
    *query_len = 0;
    return 0;
  }

  *query_off = q + 1;
  *query_len = (q + 1 <= target_len) ? (target_len - (q + 1)) : 0;
  return 0;
}

static int hexval(unsigned char c) {
  if (c >= '0' && c <= '9') {
    return (int)(c - '0');
  }
  if (c >= 'a' && c <= 'f') {
    return 10 + (int)(c - 'a');
  }
  if (c >= 'A' && c <= 'F') {
    return 10 + (int)(c - 'A');
  }
  return -1;
}

int url_percent_decode_strict(const char *in,
                              size_t in_len,
                              char *out,
                              size_t out_cap,
                              size_t *out_len,
                              int reject_decoded_slashes) {
  if (!in || !out || out_cap == 0 || !out_len) {
    return -1;
  }

  size_t o = 0;
  for (size_t i = 0; i < in_len; ++i) {
    unsigned char c = (unsigned char)in[i];

    if (c == '%') {
      if (i + 2 >= in_len) {
        return -1;
      }
      int hi = hexval((unsigned char)in[i + 1]);
      int lo = hexval((unsigned char)in[i + 2]);
      if (hi < 0 || lo < 0) {
        return -1;
      }
      unsigned char d = (unsigned char)((hi << 4) | lo);
      i += 2;

      if (d == 0) {
        return -1;
      }
      if (reject_decoded_slashes && (d == '/' || d == '\\')) {
        return -1;
      }

      if (o + 1 > out_cap) {
        return -2;
      }
      out[o++] = (char)d;
      continue;
    }

    if (o + 1 > out_cap) {
      return -2;
    }
    out[o++] = (char)c;
  }

  *out_len = o;
  return 0;
}

int url_path_normalize_absolute(const char *path,
                                size_t path_len,
                                char *out,
                                size_t out_cap,
                                size_t *out_len,
                                int *ends_with_slash) {
  if (!path || !out || out_cap == 0 || !out_len || !ends_with_slash) {
    return -1;
  }
  if (path_len == 0 || path[0] != '/') {
    return -1;
  }

  *ends_with_slash = (path_len > 0 && path[path_len - 1] == '/');

  // Build normalized absolute path and track segment starts for '..' handling.
  size_t seg_starts[256];
  size_t seg_count = 0;
  const size_t seg_cap = sizeof(seg_starts) / sizeof(seg_starts[0]);

  size_t o = 0;
  if (out_cap < 1) {
    return -2;
  }
  out[o++] = '/';

  size_t i = 1;
  while (i < path_len) {
    while (i < path_len && path[i] == '/') {
      i++;
    }
    if (i >= path_len) {
      break;
    }

    size_t start = i;
    while (i < path_len && path[i] != '/') {
      i++;
    }
    size_t len = i - start;

    if (len == 1 && path[start] == '.') {
      continue;
    }
    if (len == 2 && path[start] == '.' && path[start + 1] == '.') {
      if (seg_count > 0) {
        o = seg_starts[seg_count - 1];
        seg_count--;
        if (o > 1 && out[o - 1] == '/') {
          o--;
        }
        out[o] = 0;
      }
      continue;
    }

    if (o > 1) {
      if (o + 1 > out_cap) {
        return -2;
      }
      out[o++] = '/';
    }

    if (seg_count >= seg_cap) {
      return -1;
    }
    seg_starts[seg_count++] = o;

    if (o + len > out_cap) {
      return -2;
    }
    memcpy(out + o, path + start, len);
    o += len;
  }

  if (o == 0) {
    return -1;
  }

  if (o == 1) {
    out[0] = '/';
    *out_len = 1;
    return 0;
  }

  *out_len = o;
  return 0;
}
