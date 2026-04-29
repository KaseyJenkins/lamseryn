// Unit tests for compress_gzip() and compress_brotli() (dynamic compression).
// Tests the compression engines in isolation: round-trips, expansion guard,
// edge inputs, and error paths.

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

#ifdef HAVE_BROTLI
#include <brotli/decode.h>
#endif

#include "../vendor/greatest_color.h"
#include "../vendor/greatest.h"

#include "include/compression.h"

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// Decompress a gzip buffer into a newly malloc'd buffer.
// Returns allocated buffer (caller frees) and sets *out_len.
// Returns NULL on failure.
static void *gunzip(const void *src, size_t src_len, size_t *out_len) {
  if (!src || src_len == 0 || !out_len) return NULL;

  // Allocate a generous output buffer (10x input is safe for test payloads).
  size_t buf_size = src_len * 10 + 4096;
  void *buf = malloc(buf_size);
  if (!buf) return NULL;

  z_stream strm = {0};
  // MAX_WBITS + 16 = gzip framing decode.
  if (inflateInit2(&strm, MAX_WBITS + 16) != Z_OK) {
    free(buf);
    return NULL;
  }

  strm.next_in   = (Bytef *)(uintptr_t)src;
  strm.avail_in  = (uInt)src_len;
  strm.next_out  = (Bytef *)buf;
  strm.avail_out = (uInt)buf_size;

  int rc = inflate(&strm, Z_FINISH);
  uLong decompressed = buf_size - strm.avail_out;
  inflateEnd(&strm);

  if (rc != Z_STREAM_END) {
    free(buf);
    return NULL;
  }

  *out_len = (size_t)decompressed;
  return buf;
}

// ---------------------------------------------------------------------------
// Round-trip: compress then decompress and verify byte-exact match
// ---------------------------------------------------------------------------

TEST gzip_roundtrip_text(void) {
  const char *input = "Hello, world! This is a compressible text string. "
                      "Repeating it makes it compress well. "
                      "Repeating it makes it compress well. "
                      "Repeating it makes it compress well.";
  size_t in_len = strlen(input);

  void *comp_buf = NULL;
  size_t comp_len = 0;
  enum compress_result cr = compress_gzip(input, in_len, &comp_buf, &comp_len, 1);
  ASSERT_EQ(cr, COMPRESS_OK);
  ASSERT(comp_buf != NULL);
  ASSERT(comp_len > 0);
  ASSERT(comp_len < in_len);  // should actually compress

  size_t decomp_len = 0;
  void *decomp = gunzip(comp_buf, comp_len, &decomp_len);
  ASSERT(decomp != NULL);
  ASSERT_EQ(decomp_len, in_len);
  ASSERT_EQ(memcmp(decomp, input, in_len), 0);

  free(comp_buf);
  free(decomp);
  PASS();
}

TEST gzip_roundtrip_binary(void) {
  // A compressible binary payload: 1024 bytes with a repeating pattern.
  size_t in_len = 1024;
  uint8_t *input = malloc(in_len);
  ASSERT(input != NULL);
  for (size_t i = 0; i < in_len; i++) {
    input[i] = (uint8_t)(i % 17);
  }

  void *comp_buf = NULL;
  size_t comp_len = 0;
  enum compress_result cr = compress_gzip(input, in_len, &comp_buf, &comp_len, 1);
  ASSERT_EQ(cr, COMPRESS_OK);
  ASSERT(comp_buf != NULL);

  size_t decomp_len = 0;
  void *decomp = gunzip(comp_buf, comp_len, &decomp_len);
  ASSERT(decomp != NULL);
  ASSERT_EQ(decomp_len, in_len);
  ASSERT_EQ(memcmp(decomp, input, in_len), 0);

  free(comp_buf);
  free(decomp);
  free(input);
  PASS();
}

// Level 9 should also produce valid gzip output.
TEST gzip_roundtrip_level9(void) {
  const char *input = "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
                      "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA";
  size_t in_len = strlen(input);

  void *comp_buf = NULL;
  size_t comp_len = 0;
  enum compress_result cr = compress_gzip(input, in_len, &comp_buf, &comp_len, 9);
  ASSERT_EQ(cr, COMPRESS_OK);
  ASSERT(comp_buf != NULL);

  size_t decomp_len = 0;
  void *decomp = gunzip(comp_buf, comp_len, &decomp_len);
  ASSERT(decomp != NULL);
  ASSERT_EQ(decomp_len, in_len);
  ASSERT_EQ(memcmp(decomp, input, in_len), 0);

  free(comp_buf);
  free(decomp);
  PASS();
}

// ---------------------------------------------------------------------------
// Expansion guard: tiny input that gzip framing overhead makes larger
// ---------------------------------------------------------------------------

TEST gzip_expansion_guard_tiny(void) {
  // 5 random bytes: gzip overhead (~18 bytes) guarantees expansion.
  const uint8_t input[5] = {0xDE, 0xAD, 0xBE, 0xEF, 0x00};
  void *comp_buf = NULL;
  size_t comp_len = 0;
  enum compress_result cr = compress_gzip(input, sizeof(input), &comp_buf, &comp_len, 1);
  ASSERT_EQ(cr, COMPRESS_EXPANDED);
  ASSERT_EQ(comp_buf, NULL);
  ASSERT_EQ(comp_len, 0);
  PASS();
}

// ---------------------------------------------------------------------------
// Empty / zero-byte input
// ---------------------------------------------------------------------------

TEST gzip_empty_input(void) {
  void *comp_buf = NULL;
  size_t comp_len = 0;
  // Empty input: gzip framing bytes always > 0 bytes, so should expand.
  enum compress_result cr = compress_gzip("", 0, &comp_buf, &comp_len, 1);
  // Either COMPRESS_EXPANDED (framing > 0 bytes) or COMPRESS_ERROR is acceptable.
  ASSERT(cr == COMPRESS_EXPANDED || cr == COMPRESS_ERROR);
  ASSERT_EQ(comp_buf, NULL);
  PASS();
}

TEST gzip_null_src(void) {
  void *comp_buf = NULL;
  size_t comp_len = 0;
  enum compress_result cr = compress_gzip(NULL, 10, &comp_buf, &comp_len, 1);
  ASSERT_EQ(cr, COMPRESS_ERROR);
  ASSERT_EQ(comp_buf, NULL);
  PASS();
}

TEST gzip_null_out_buf(void) {
  const char *input = "hello";
  size_t comp_len = 0;
  enum compress_result cr = compress_gzip(input, 5, NULL, &comp_len, 1);
  ASSERT_EQ(cr, COMPRESS_ERROR);
  PASS();
}

TEST gzip_null_out_len(void) {
  const char *input = "hello";
  void *comp_buf = NULL;
  enum compress_result cr = compress_gzip(input, 5, &comp_buf, NULL, 1);
  ASSERT_EQ(cr, COMPRESS_ERROR);
  ASSERT_EQ(comp_buf, NULL);
  PASS();
}

// ---------------------------------------------------------------------------
// Output is valid gzip (RFC 1952): magic bytes check
// ---------------------------------------------------------------------------

TEST gzip_output_has_magic(void) {
  // Build a 512-byte compressible buffer.
  char input[512];
  memset(input, 'A', sizeof(input));

  void *comp_buf = NULL;
  size_t comp_len = 0;
  enum compress_result cr = compress_gzip(input, sizeof(input), &comp_buf, &comp_len, 1);
  ASSERT_EQ(cr, COMPRESS_OK);
  ASSERT(comp_len >= 2);

  // gzip magic: 0x1f 0x8b
  const uint8_t *bytes = (const uint8_t *)comp_buf;
  ASSERT_EQ(bytes[0], 0x1fu);
  ASSERT_EQ(bytes[1], 0x8bu);

  free(comp_buf);
  PASS();
}

// ---------------------------------------------------------------------------
// Compress result: compressed size < src_len on COMPRESS_OK
// ---------------------------------------------------------------------------

TEST gzip_ok_output_smaller(void) {
  // 2048 bytes of repeated pattern: definitely compressible.
  char input[2048];
  memset(input, 'Z', sizeof(input));

  void *comp_buf = NULL;
  size_t comp_len = 0;
  enum compress_result cr = compress_gzip(input, sizeof(input), &comp_buf, &comp_len, 1);
  ASSERT_EQ(cr, COMPRESS_OK);
  ASSERT(comp_len < sizeof(input));

  free(comp_buf);
  PASS();
}

// ---------------------------------------------------------------------------
// Suites
// ---------------------------------------------------------------------------

SUITE(gzip_roundtrip) {
  RUN_TEST(gzip_roundtrip_text);
  RUN_TEST(gzip_roundtrip_binary);
  RUN_TEST(gzip_roundtrip_level9);
}

SUITE(gzip_expansion_guard) {
  RUN_TEST(gzip_expansion_guard_tiny);
}

SUITE(gzip_edge_inputs) {
  RUN_TEST(gzip_empty_input);
  RUN_TEST(gzip_null_src);
  RUN_TEST(gzip_null_out_buf);
  RUN_TEST(gzip_null_out_len);
}

SUITE(gzip_output_validity) {
  RUN_TEST(gzip_output_has_magic);
  RUN_TEST(gzip_ok_output_smaller);
}

// ---------------------------------------------------------------------------
// Brotli tests (only compiled when HAVE_BROTLI is defined)
// ---------------------------------------------------------------------------

#ifdef HAVE_BROTLI

// Decompress a brotli buffer into a newly malloc'd buffer.
// Returns allocated buffer (caller frees) and sets *out_len, or NULL on error.
static void *bunzip(const void *src, size_t src_len, size_t *out_len) {
  if (!src || src_len == 0 || !out_len) return NULL;
  size_t buf_size = src_len * 10 + 4096;
  void *buf = malloc(buf_size);
  if (!buf) return NULL;
  size_t decoded_size = buf_size;
  BrotliDecoderResult r = BrotliDecoderDecompress(
    src_len, (const uint8_t *)src, &decoded_size, (uint8_t *)buf);
  if (r != BROTLI_DECODER_RESULT_SUCCESS) {
    free(buf);
    return NULL;
  }
  *out_len = decoded_size;
  return buf;
}

TEST brotli_roundtrip_text(void) {
  const char *input = "Hello, brotli! This is a compressible text string. "
                      "Repeating it makes it compress well. "
                      "Repeating it makes it compress well. "
                      "Repeating it makes it compress well.";
  size_t in_len = strlen(input);

  void *comp_buf = NULL;
  size_t comp_len = 0;
  enum compress_result cr = compress_brotli(input, in_len, &comp_buf, &comp_len, 4);
  ASSERT_EQ(cr, COMPRESS_OK);
  ASSERT(comp_buf != NULL);
  ASSERT(comp_len > 0);
  ASSERT(comp_len < in_len);

  size_t decomp_len = 0;
  void *decomp = bunzip(comp_buf, comp_len, &decomp_len);
  ASSERT(decomp != NULL);
  ASSERT_EQ(decomp_len, in_len);
  ASSERT_EQ(memcmp(decomp, input, in_len), 0);

  free(comp_buf);
  free(decomp);
  PASS();
}

TEST brotli_roundtrip_binary(void) {
  size_t in_len = 1024;
  uint8_t *input = malloc(in_len);
  ASSERT(input != NULL);
  for (size_t i = 0; i < in_len; i++) {
    input[i] = (uint8_t)(i % 17);
  }

  void *comp_buf = NULL;
  size_t comp_len = 0;
  enum compress_result cr = compress_brotli(input, in_len, &comp_buf, &comp_len, 4);
  ASSERT_EQ(cr, COMPRESS_OK);
  ASSERT(comp_buf != NULL);

  size_t decomp_len = 0;
  void *decomp = bunzip(comp_buf, comp_len, &decomp_len);
  ASSERT(decomp != NULL);
  ASSERT_EQ(decomp_len, in_len);
  ASSERT_EQ(memcmp(decomp, input, in_len), 0);

  free(comp_buf);
  free(decomp);
  free(input);
  PASS();
}

TEST brotli_expansion_guard_tiny(void) {
  // 5 random bytes: brotli framing overhead guarantees expansion.
  const uint8_t input[5] = {0xDE, 0xAD, 0xBE, 0xEF, 0x00};
  void *comp_buf = NULL;
  size_t comp_len = 0;
  enum compress_result cr = compress_brotli(input, sizeof(input), &comp_buf, &comp_len, 4);
  ASSERT_EQ(cr, COMPRESS_EXPANDED);
  ASSERT_EQ(comp_buf, NULL);
  ASSERT_EQ(comp_len, 0);
  PASS();
}

TEST brotli_null_src(void) {
  void *comp_buf = NULL;
  size_t comp_len = 0;
  enum compress_result cr = compress_brotli(NULL, 10, &comp_buf, &comp_len, 4);
  ASSERT_EQ(cr, COMPRESS_ERROR);
  ASSERT_EQ(comp_buf, NULL);
  PASS();
}

TEST brotli_null_out_buf(void) {
  const char *input = "hello";
  size_t comp_len = 0;
  enum compress_result cr = compress_brotli(input, 5, NULL, &comp_len, 4);
  ASSERT_EQ(cr, COMPRESS_ERROR);
  PASS();
}

TEST brotli_null_out_len(void) {
  const char *input = "hello";
  void *comp_buf = NULL;
  enum compress_result cr = compress_brotli(input, 5, &comp_buf, NULL, 4);
  ASSERT_EQ(cr, COMPRESS_ERROR);
  ASSERT_EQ(comp_buf, NULL);
  PASS();
}

TEST brotli_ok_output_smaller(void) {
  char input[2048];
  memset(input, 'Z', sizeof(input));

  void *comp_buf = NULL;
  size_t comp_len = 0;
  enum compress_result cr = compress_brotli(input, sizeof(input), &comp_buf, &comp_len, 4);
  ASSERT_EQ(cr, COMPRESS_OK);
  ASSERT(comp_len < sizeof(input));

  free(comp_buf);
  PASS();
}

SUITE(brotli_roundtrip) {
  RUN_TEST(brotli_roundtrip_text);
  RUN_TEST(brotli_roundtrip_binary);
}

SUITE(brotli_expansion_guard) {
  RUN_TEST(brotli_expansion_guard_tiny);
}

SUITE(brotli_edge_inputs) {
  RUN_TEST(brotli_null_src);
  RUN_TEST(brotli_null_out_buf);
  RUN_TEST(brotli_null_out_len);
}

SUITE(brotli_output_validity) {
  RUN_TEST(brotli_ok_output_smaller);
}

#endif /* HAVE_BROTLI */

GREATEST_MAIN_DEFS();

int main(int argc, char **argv) {
  GREATEST_MAIN_BEGIN();
  RUN_SUITE(gzip_roundtrip);
  RUN_SUITE(gzip_expansion_guard);
  RUN_SUITE(gzip_edge_inputs);
  RUN_SUITE(gzip_output_validity);
#ifdef HAVE_BROTLI
  RUN_SUITE(brotli_roundtrip);
  RUN_SUITE(brotli_expansion_guard);
  RUN_SUITE(brotli_edge_inputs);
  RUN_SUITE(brotli_output_validity);
#endif
  GREATEST_MAIN_END();
}
