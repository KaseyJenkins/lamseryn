#pragma once

#include <stddef.h>

// Encoding bitmask values returned by compress_parse_accept_encoding().
#define COMP_ENC_GZIP   (1u << 0)
#define COMP_ENC_BROTLI (1u << 1)

// Parse an Accept-Encoding header value into an encoding bitmask.
// Follows RFC 7231 §5.3.4: explicit per-token q values take precedence over
// wildcards; q=0 is an explicit exclusion; '*' fills in encodings not explicitly
// listed.
// Returns a bitmask of accepted encodings (COMP_ENC_* bits). Returns 0 when
// value is NULL, empty, or no known encodings are accepted.
unsigned compress_parse_accept_encoding(const char *value, size_t value_len);

// Returns 1 if the MIME type is a candidate for precompressed serving.
// Returns 0 for already-compressed types (JPEG, PNG, WebP, AVIF, WOFF2,
// archives, etc.) where compression adds size overhead with no benefit.
int compress_mime_is_compressible(const char *mime_type);

// Returns the file extension for a given encoding (e.g. ".gz" for COMP_ENC_GZIP).
// Returns NULL for unknown encodings.
const char *compress_enc_ext(unsigned enc);

// Returns the Content-Encoding token for a given encoding (e.g. "gzip").
// Returns NULL for unknown encodings.
const char *compress_enc_name(unsigned enc);

// ---------------------------------------------------------------------------
// Dynamic (on-the-fly) compression
// ---------------------------------------------------------------------------

// Return status for compress_gzip() / compress_brotli().
enum compress_result {
  COMPRESS_OK,       // *out_buf and *out_len set; caller must free(*out_buf).
  COMPRESS_EXPANDED, // Compressed form >= src_len; implementation freed buf internally.
  COMPRESS_ERROR,    // Library failure; no buffer allocated.
};

// Compress src (src_len bytes) using gzip (RFC 1952).
// On COMPRESS_OK, *out_buf is a malloc'd buffer and *out_len is its size.
// Caller must free(*out_buf) after COMPRESS_OK.
// level must be in [1,9]; pass 0 to use the default (Z_DEFAULT_COMPRESSION).
enum compress_result compress_gzip(const void *src, size_t src_len,
                                   void **out_buf, size_t *out_len,
                                   int level);

#ifdef HAVE_BROTLI
// Compress src (src_len bytes) using Brotli.
// Same ownership contract as compress_gzip().
// quality must be in [0,11]; pass -1 to use the default (4).
enum compress_result compress_brotli(const void *src, size_t src_len,
                                     void **out_buf, size_t *out_len,
                                     int quality);
#endif
