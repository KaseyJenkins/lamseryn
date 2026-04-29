#include "include/compression.h"

#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

#ifdef HAVE_BROTLI
#include <brotli/encode.h>
#endif

// ---------------------------------------------------------------------------
// Accept-Encoding parsing
// ---------------------------------------------------------------------------

// Skip ASCII horizontal whitespace.
static const char *ae_skip_ws(const char *p, const char *end) {
  while (p < end && (*p == ' ' || *p == '\t')) {
    p++;
  }
  return p;
}

// Case-insensitive comparison of a length-bounded token against a literal.
static int ae_token_ieq(const char *tok, size_t tok_len, const char *s) {
  size_t slen = strlen(s);
  if (tok_len != slen) {
    return 0;
  }
  for (size_t i = 0; i < slen; i++) {
    unsigned char c = (unsigned char)tok[i];
    if (c >= 'A' && c <= 'Z') {
      c |= 0x20u;
    }
    if (c != (unsigned char)s[i]) {
      return 0;
    }
  }
  return 1;
}

// Returns 1 when the q-value starting at *p (up to end) is exactly zero.
// Advances *p past the q-value token.
static int ae_q_is_zero(const char **p, const char *end) {
  const char *q = ae_skip_ws(*p, end);
  *p = q;

  if (q >= end) {
    return 0;
  }

  // Consume leading '0'.
  if (*q != '0') {
    while (*p < end && **p != ',' && **p != ';') {
      (*p)++;
    }
    return 0;
  }
  (*p)++; // consume '0'

  // Optional fractional part.
  if (*p < end && **p == '.') {
    (*p)++; // consume '.'
    while (*p < end && **p >= '0' && **p <= '9') {
      if (**p != '0') {
        // Non-zero digit: q > 0.
        while (*p < end && **p != ',' && **p != ';') {
          (*p)++;
        }
        return 0;
      }
      (*p)++;
    }
  }

  // Advance to end of value.
  while (*p < end && **p != ',' && **p != ';') {
    (*p)++;
  }
  return 1;
}

unsigned compress_parse_accept_encoding(const char *value, size_t value_len) {
  if (!value || value_len == 0) {
    return 0;
  }

  // Per RFC 7231 §5.3.4, explicit coding entries override wildcard entries;
  // wildcard applies only to codings not explicitly listed.

  // Tracking per known encoding: 0=not seen, 1=accepted, -1=rejected.
  int gzip_explicit   = 0; // 0 = not seen
  int brotli_explicit = 0;
  int gzip_val        = 0;
  int brotli_val      = 0;
  int wildcard_seen   = 0;
  int wildcard_val    = 0;

  const char *p   = value;
  const char *end = value + value_len;

  while (p < end) {
    p = ae_skip_ws(p, end);
    if (p >= end) {
      break;
    }

    // Scan the coding token up to ';' or ','.
    const char *tok_start = p;
    while (p < end && *p != ',' && *p != ';') {
      p++;
    }
    const char *tok_end = p;

    // Trim trailing whitespace.
    while (tok_end > tok_start
           && (tok_end[-1] == ' ' || tok_end[-1] == '\t')) {
      tok_end--;
    }
    size_t tok_len = (size_t)(tok_end - tok_start);

    // Classify the coding token.
    int is_gzip     = ae_token_ieq(tok_start, tok_len, "gzip");
    int is_brotli   = ae_token_ieq(tok_start, tok_len, "br");
    int is_wildcard = (tok_len == 1 && tok_start[0] == '*');

    // Parse optional parameters; only q matters.
    int q_zero = 0;
    while (p < end && *p == ';') {
      p++; // skip ';'
      p = ae_skip_ws(p, end);

      const char *param = p;
      while (p < end && *p != '=' && *p != ',' && *p != ';') {
        p++;
      }
      size_t param_len = (size_t)(p - param);

      if (p < end && *p == '=') {
        p++; // skip '='
        if (param_len == 1 && (param[0] == 'q' || param[0] == 'Q')) {
          q_zero = ae_q_is_zero(&p, end);
        } else {
          // Unknown parameter value: skip.
          while (p < end && *p != ',' && *p != ';') {
            p++;
          }
        }
      }
    }

    // Record accept/reject decision.
    if (is_gzip) {
      gzip_explicit = 1;
      gzip_val      = q_zero ? -1 : 1;
    } else if (is_brotli) {
      brotli_explicit = 1;
      brotli_val      = q_zero ? -1 : 1;
    } else if (is_wildcard) {
      wildcard_seen = 1;
      wildcard_val  = q_zero ? -1 : 1;
    }
    // deflate, identity, zstd, x-gzip: parsed but not acted on.

    p = ae_skip_ws(p, end);
    if (p < end && *p == ',') {
      p++;
    }
  }

  // Resolve final bitmask: explicit entries win over wildcard.
  unsigned result = 0;

  int gzip_accept = gzip_explicit
                      ? (gzip_val > 0 ? 1 : 0)
                      : (wildcard_seen ? (wildcard_val > 0 ? 1 : 0) : 0);

  int brotli_accept = brotli_explicit
                        ? (brotli_val > 0 ? 1 : 0)
                        : (wildcard_seen ? (wildcard_val > 0 ? 1 : 0) : 0);

  if (gzip_accept)   { result |= COMP_ENC_GZIP;   }
  if (brotli_accept) { result |= COMP_ENC_BROTLI; }

  return result;
}

// ---------------------------------------------------------------------------
// MIME type compressibility
// ---------------------------------------------------------------------------

int compress_mime_is_compressible(const char *mime_type) {
  if (!mime_type) {
    return 0;
  }
  // Already-compressed or binary types that gain nothing from compression.
  // Match on prefix to handle "; charset=utf-8" suffixes.
  static const char *const skip[] = {
    "image/jpeg",
    "image/png",
    "image/gif",
    "image/webp",
    "image/avif",
    "image/bmp",
    "image/tiff",
    "font/woff",   // covers woff and woff2
    "application/zip",
    "application/gzip",
    "application/x-gzip",
    "application/x-bzip2",
    "application/x-xz",
    "application/x-rar",
    "application/x-7z",
    "application/octet-stream",
    "video/",
    "audio/",
    NULL,
  };

  for (int i = 0; skip[i]; i++) {
    if (strncmp(mime_type, skip[i], strlen(skip[i])) == 0) {
      return 0;
    }
  }
  return 1;
}

// ---------------------------------------------------------------------------
// Encoding metadata
// ---------------------------------------------------------------------------

const char *compress_enc_ext(unsigned enc) {
  switch (enc) {
    case COMP_ENC_GZIP:   return ".gz";
    case COMP_ENC_BROTLI: return ".br";
    default:              return NULL;
  }
}

const char *compress_enc_name(unsigned enc) {
  switch (enc) {
    case COMP_ENC_GZIP:   return "gzip";
    case COMP_ENC_BROTLI: return "br";
    default:              return NULL;
  }
}

// ---------------------------------------------------------------------------
// Dynamic (on-the-fly) compression
// ---------------------------------------------------------------------------

enum compress_result compress_gzip(const void *src, size_t src_len,
                                   void **out_buf, size_t *out_len,
                                   int level) {
  if (!src || !out_buf || !out_len) {
    return COMPRESS_ERROR;
  }
  *out_buf = NULL;
  *out_len = 0;

  // zlib's avail_in/avail_out are uInt (32-bit); reject oversized inputs.
  if (src_len > (size_t)UINT_MAX) {
    return COMPRESS_ERROR;
  }

  // Use Z_DEFAULT_COMPRESSION when caller passes 0.
  if (level == 0) {
    level = Z_DEFAULT_COMPRESSION;
  }

  z_stream strm = {0};
  // MAX_WBITS + 16 selects gzip framing (RFC 1952).
  int rc = deflateInit2(&strm, level, Z_DEFLATED,
                        MAX_WBITS + 16, 8, Z_DEFAULT_STRATEGY);
  if (rc != Z_OK) {
    return COMPRESS_ERROR;
  }

  uLong bound = deflateBound(&strm, (uLong)src_len);
  void *buf = malloc((size_t)bound);
  if (!buf) {
    deflateEnd(&strm);
    return COMPRESS_ERROR;
  }

  strm.next_in   = (Bytef *)(uintptr_t)src;
  strm.avail_in  = (uInt)src_len;
  strm.next_out  = (Bytef *)buf;
  strm.avail_out = (uInt)bound;

  rc = deflate(&strm, Z_FINISH);
  uLong comp_len = bound - strm.avail_out;
  deflateEnd(&strm);

  if (rc != Z_STREAM_END) {
    free(buf);
    return COMPRESS_ERROR;
  }

  // Expansion guard: if compressed form is not smaller, discard it.
  if (comp_len >= (uLong)src_len) {
    free(buf);
    return COMPRESS_EXPANDED;
  }

  *out_buf = buf;
  *out_len = (size_t)comp_len;
  return COMPRESS_OK;
}

#ifdef HAVE_BROTLI
enum compress_result compress_brotli(const void *src, size_t src_len,
                                     void **out_buf, size_t *out_len,
                                     int quality) {
  if (!src || !out_buf || !out_len) {
    return COMPRESS_ERROR;
  }
  *out_buf = NULL;
  *out_len = 0;

  if (quality < 0) {
    quality = 4;
  }

  size_t enc_size = BrotliEncoderMaxCompressedSize(src_len);
  if (enc_size == 0) {
    return COMPRESS_ERROR;
  }

  void *buf = malloc(enc_size);
  if (!buf) {
    return COMPRESS_ERROR;
  }

  size_t out_size = enc_size;
  BROTLI_BOOL ok = BrotliEncoderCompress(
    quality, BROTLI_DEFAULT_WINDOW, BROTLI_MODE_GENERIC,
    src_len, (const uint8_t *)src,
    &out_size, (uint8_t *)buf);

  if (!ok) {
    free(buf);
    return COMPRESS_ERROR;
  }

  if (out_size >= src_len) {
    free(buf);
    return COMPRESS_EXPANDED;
  }

  *out_buf = buf;
  *out_len = out_size;
  return COMPRESS_OK;
}
#endif
