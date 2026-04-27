#include "include/compression.h"

#include <string.h>

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
