// Unit tests for static_serve_assemble_extra_headers — verifies that each
// feature flag (CONDITIONAL, RANGE, COMPRESSION) produces headers
// independently without requiring any other flag.

#define _GNU_SOURCE
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include "../vendor/greatest_color.h"
#include "../vendor/greatest.h"

#include "include/types.h"
#include "include/config.h"
#include "include/conn.h"
#include "include/http_headers.h"
#include "include/request_handlers.h"
#include "include/static_serve_utils.h"
#include "include/tx.h"

// ---------------------------------------------------------------------------
// Stubs — same pattern as test_static_serve_304_greatest.c
// ---------------------------------------------------------------------------

struct request_static_serve_plan request_build_static_serve_plan(
    const struct conn *c, size_t sz) {
  (void)c; (void)sz;
  struct request_static_serve_plan p = {0};
  return p;
}

int request_static_open_err_merge(int a, int b) { (void)a; return b; }

enum tx_decision tx_begin_headers(struct tx_state_t *tx, enum resp_kind rk,
                                  const char *buf, size_t len, int ka,
                                  int dah, struct tx_next_io *out) {
  (void)tx; (void)rk; (void)buf; (void)len; (void)ka; (void)dah; (void)out;
  return TX_NOOP;
}

int tx_build_headers(struct tx_state_t *tx, const char *s, const char *ct,
                     size_t cl, const void *body, size_t bsl, int ka, int dah,
                     const char *eh, const char **buf, size_t *len) {
  (void)tx; (void)s; (void)ct; (void)cl; (void)body; (void)bsl;
  (void)ka; (void)dah; (void)eh; (void)buf; (void)len;
  return -1;
}

int tx_begin_sendfile(struct tx_state_t *tx, off_t off, size_t sz) {
  (void)tx; (void)off; (void)sz;
  return 0;
}

const char *http_header_find_value(const struct req_hdr_entry *hdrs,
                                   uint8_t hdr_count,
                                   enum http_header_id id,
                                   uint16_t *out_len) {
  (void)hdrs; (void)hdr_count; (void)id;
  if (out_len) *out_len = 0;
  return NULL;
}

uint64_t http_headers_store_mask(uint64_t features) { (void)features; return 0; }
int http_header_lookup_lower(const char *n, size_t l, enum http_header_id *o) {
  (void)n; (void)l; (void)o; return -1;
}

// Include file under test to access the static function.
#include "../../src/static_serve_utils.c"

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
static struct stat make_stat(ino_t ino, off_t size, time_t sec, long nsec) {
  struct stat st;
  memset(&st, 0, sizeof(st));
  st.st_ino = ino;
  st.st_size = size;
  st.st_mtim.tv_sec = sec;
  st.st_mtim.tv_nsec = nsec;
  return st;
}

// ===========================================================================
// ETag/Last-Modified are only emitted when CFG_FEAT_CONDITIONAL is set
// ===========================================================================

TEST t_bare_static_no_etag_no_lastmod(void) {
  struct stat st = make_stat(1, 100, 1000, 0);
  char buf[256], etag[64];
  size_t etag_len = 0;
  unsigned enc = 0;
  size_t n = static_serve_assemble_extra_headers(
    buf, sizeof(buf), &st, CFG_FEAT_STATIC, &enc,
    "text/html", NULL, 0, etag, sizeof(etag), &etag_len);
  ASSERT_EQ(n, 0);
  ASSERT_EQ(etag_len, 0);
  ASSERT_STR_EQ(buf, "");
  PASS();
}

TEST t_conditional_emits_etag_and_lastmod(void) {
  struct stat st = make_stat(0x1234, 4096, 1000, 500000);
  char buf[256], etag[64];
  size_t etag_len = 0;
  unsigned enc = 0;
  size_t n = static_serve_assemble_extra_headers(
    buf, sizeof(buf), &st, CFG_FEAT_STATIC | CFG_FEAT_CONDITIONAL, &enc,
    "text/html", NULL, 0, etag, sizeof(etag), &etag_len);
  ASSERT(n > 0);
  ASSERT(etag_len > 0);
  ASSERT(strstr(buf, "ETag:") != NULL);
  ASSERT(strstr(buf, "Last-Modified:") != NULL);
  PASS();
}

// ===========================================================================
// Accept-Ranges is controlled solely by CFG_FEAT_RANGE
// ===========================================================================

TEST t_range_only_emits_accept_ranges(void) {
  struct stat st = make_stat(1, 100, 1000, 0);
  char buf[256], etag[64];
  size_t etag_len = 0;
  unsigned enc = 0;
  size_t n = static_serve_assemble_extra_headers(
    buf, sizeof(buf), &st, CFG_FEAT_STATIC | CFG_FEAT_RANGE, &enc,
    "text/html", NULL, 0, etag, sizeof(etag), &etag_len);
  ASSERT(n > 0);
  ASSERT_EQ(etag_len, 0);
  ASSERT(strstr(buf, "Accept-Ranges: bytes") != NULL);
  ASSERT(strstr(buf, "ETag:") == NULL);
  PASS();
}

// ===========================================================================
// Content-Encoding and Vary are controlled solely by CFG_FEAT_COMPRESSION
// ===========================================================================

TEST t_compression_gzip_without_conditional(void) {
  struct stat st = make_stat(1, 100, 1000, 0);
  char buf[256], etag[64];
  size_t etag_len = 0;
  unsigned enc = COMP_ENC_GZIP;
  size_t n = static_serve_assemble_extra_headers(
    buf, sizeof(buf), &st, CFG_FEAT_STATIC | CFG_FEAT_COMPRESSION, &enc,
    "text/html", NULL, 0, etag, sizeof(etag), &etag_len);
  ASSERT(n > 0);
  ASSERT(strstr(buf, "Content-Encoding: gzip") != NULL);
  ASSERT(strstr(buf, "Vary: Accept-Encoding") != NULL);
  ASSERT_EQ(etag_len, 0);
  ASSERT(strstr(buf, "ETag:") == NULL);
  PASS();
}

TEST t_compression_brotli_without_conditional(void) {
  struct stat st = make_stat(1, 100, 1000, 0);
  char buf[256], etag[64];
  size_t etag_len = 0;
  unsigned enc = COMP_ENC_BROTLI;
  size_t n = static_serve_assemble_extra_headers(
    buf, sizeof(buf), &st, CFG_FEAT_STATIC | CFG_FEAT_COMPRESSION, &enc,
    "text/html", NULL, 0, etag, sizeof(etag), &etag_len);
  ASSERT(n > 0);
  ASSERT(strstr(buf, "Content-Encoding: br") != NULL);
  ASSERT(strstr(buf, "Vary: Accept-Encoding") != NULL);
  PASS();
}

// Vary: Accept-Encoding is emitted on identity responses for compressible
// MIME when CFG_FEAT_COMPRESSION is set, regardless of other flags
TEST t_vary_identity_without_conditional(void) {
  struct stat st = make_stat(1, 100, 1000, 0);
  char buf[256], etag[64];
  size_t etag_len = 0;
  unsigned enc = 0;
  size_t n = static_serve_assemble_extra_headers(
    buf, sizeof(buf), &st, CFG_FEAT_STATIC | CFG_FEAT_COMPRESSION, &enc,
    "text/html", NULL, 0, etag, sizeof(etag), &etag_len);
  ASSERT(n > 0);
  ASSERT(strstr(buf, "Vary: Accept-Encoding") != NULL);
  ASSERT(strstr(buf, "Content-Encoding:") == NULL);
  PASS();
}

// Non-compressible MIME should NOT get Vary even with COMPRESSION flag
TEST t_compression_noncompressible_no_vary(void) {
  struct stat st = make_stat(1, 100, 1000, 0);
  char buf[256], etag[64];
  size_t etag_len = 0;
  unsigned enc = 0;
  size_t n = static_serve_assemble_extra_headers(
    buf, sizeof(buf), &st, CFG_FEAT_STATIC | CFG_FEAT_COMPRESSION, &enc,
    "image/jpeg", NULL, 0, etag, sizeof(etag), &etag_len);
  ASSERT_EQ(n, 0);
  ASSERT_STR_EQ(buf, "");
  PASS();
}

// ===========================================================================
// Combined: all flags together
// ===========================================================================

TEST t_all_flags_identity(void) {
  struct stat st = make_stat(0x1234, 4096, 1000, 500000);
  char buf[256], etag[64];
  size_t etag_len = 0;
  unsigned enc = 0;
  uint64_t features = CFG_FEAT_STATIC | CFG_FEAT_CONDITIONAL
                    | CFG_FEAT_RANGE | CFG_FEAT_COMPRESSION;
  size_t n = static_serve_assemble_extra_headers(
    buf, sizeof(buf), &st, features, &enc,
    "text/html", NULL, 0, etag, sizeof(etag), &etag_len);
  ASSERT(n > 0);
  ASSERT(etag_len > 0);
  ASSERT(strstr(buf, "ETag:") != NULL);
  ASSERT(strstr(buf, "Last-Modified:") != NULL);
  ASSERT(strstr(buf, "Accept-Ranges: bytes") != NULL);
  ASSERT(strstr(buf, "Vary: Accept-Encoding") != NULL);
  ASSERT(strstr(buf, "Content-Encoding:") == NULL);
  PASS();
}

TEST t_all_flags_gzip(void) {
  struct stat st = make_stat(0x1234, 4096, 1000, 500000);
  char buf[256], etag[64];
  size_t etag_len = 0;
  unsigned enc = COMP_ENC_GZIP;
  uint64_t features = CFG_FEAT_STATIC | CFG_FEAT_CONDITIONAL
                    | CFG_FEAT_RANGE | CFG_FEAT_COMPRESSION;
  size_t n = static_serve_assemble_extra_headers(
    buf, sizeof(buf), &st, features, &enc,
    "text/html", NULL, 0, etag, sizeof(etag), &etag_len);
  ASSERT(n > 0);
  ASSERT(strstr(buf, "ETag:") != NULL);
  ASSERT(strstr(buf, "Accept-Ranges: bytes") != NULL);
  ASSERT(strstr(buf, "Content-Encoding: gzip") != NULL);
  ASSERT(strstr(buf, "Vary: Accept-Encoding") != NULL);
  PASS();
}

// Vary must not appear twice when Content-Encoding already includes it
TEST t_no_duplicate_vary(void) {
  struct stat st = make_stat(1, 100, 1000, 0);
  char buf[256], etag[64];
  size_t etag_len = 0;
  unsigned enc = COMP_ENC_GZIP;
  size_t n = static_serve_assemble_extra_headers(
    buf, sizeof(buf), &st, CFG_FEAT_STATIC | CFG_FEAT_COMPRESSION, &enc,
    "text/html", NULL, 0, etag, sizeof(etag), &etag_len);
  ASSERT(n > 0);
  // Count occurrences of "Vary:" — should be exactly 1
  int count = 0;
  const char *p = buf;
  while ((p = strstr(p, "Vary:")) != NULL) { count++; p++; }
  ASSERT_EQ(count, 1);
  PASS();
}

// Content-Encoding overflow: serving_enc should be cleared
TEST t_ce_overflow_clears_serving_enc(void) {
  struct stat st = make_stat(1, 100, 1000, 0);
  // Use a tiny buffer that can't fit Content-Encoding header.
  char buf[8], etag[64];
  size_t etag_len = 0;
  unsigned enc = COMP_ENC_GZIP;
  (void)static_serve_assemble_extra_headers(
    buf, sizeof(buf), &st, CFG_FEAT_STATIC | CFG_FEAT_COMPRESSION, &enc,
    "text/html", NULL, 0, etag, sizeof(etag), &etag_len);
  ASSERT_EQ(enc, 0);
  PASS();
}

// ===========================================================================
// Custom headers (header_set): emitted verbatim
// ===========================================================================

TEST t_custom_header_emits_verbatim(void) {
  struct stat st = make_stat(1, 100, 1000, 0);
  char buf[512], etag[64];
  size_t etag_len = 0;
  unsigned enc = 0;
  char *hdrs[] = {"Cache-Control: public, max-age=3600\r\n"};
  size_t n = static_serve_assemble_extra_headers(
    buf, sizeof(buf), &st, CFG_FEAT_STATIC, &enc,
    "text/html", hdrs, 1, etag, sizeof(etag), &etag_len);
  ASSERT(n > 0);
  ASSERT(strstr(buf, "Cache-Control: public, max-age=3600\r\n") != NULL);
  PASS();
}

TEST t_custom_header_multiple(void) {
  struct stat st = make_stat(1, 100, 1000, 0);
  char buf[512], etag[64];
  size_t etag_len = 0;
  unsigned enc = 0;
  char *hdrs[] = {
    "Cache-Control: no-cache\r\n",
    "X-Content-Type-Options: nosniff\r\n"
  };
  size_t n = static_serve_assemble_extra_headers(
    buf, sizeof(buf), &st, CFG_FEAT_STATIC, &enc,
    "text/html", hdrs, 2, etag, sizeof(etag), &etag_len);
  ASSERT(n > 0);
  ASSERT(strstr(buf, "Cache-Control: no-cache\r\n") != NULL);
  ASSERT(strstr(buf, "X-Content-Type-Options: nosniff\r\n") != NULL);
  PASS();
}

TEST t_custom_header_null_ptr_zero_count(void) {
  struct stat st = make_stat(1, 100, 1000, 0);
  char buf[256], etag[64];
  size_t etag_len = 0;
  unsigned enc = 0;
  (void)static_serve_assemble_extra_headers(
    buf, sizeof(buf), &st, CFG_FEAT_STATIC, &enc,
    "text/html", NULL, 0, etag, sizeof(etag), &etag_len);
  ASSERT(strstr(buf, "Cache-Control:") == NULL);
  PASS();
}

// ===========================================================================
// Suites
// ===========================================================================

SUITE(s_flag_independence) {
  RUN_TEST(t_bare_static_no_etag_no_lastmod);
  RUN_TEST(t_conditional_emits_etag_and_lastmod);
  RUN_TEST(t_range_only_emits_accept_ranges);
  RUN_TEST(t_compression_gzip_without_conditional);
  RUN_TEST(t_compression_brotli_without_conditional);
  RUN_TEST(t_vary_identity_without_conditional);
  RUN_TEST(t_compression_noncompressible_no_vary);
}

SUITE(s_combined) {
  RUN_TEST(t_all_flags_identity);
  RUN_TEST(t_all_flags_gzip);
  RUN_TEST(t_no_duplicate_vary);
  RUN_TEST(t_ce_overflow_clears_serving_enc);
}

SUITE(s_custom_headers) {
  RUN_TEST(t_custom_header_emits_verbatim);
  RUN_TEST(t_custom_header_multiple);
  RUN_TEST(t_custom_header_null_ptr_zero_count);
}

GREATEST_MAIN_DEFS();
int main(int argc, char **argv) {
  GREATEST_MAIN_BEGIN();
  RUN_SUITE(s_flag_independence);
  RUN_SUITE(s_combined);
  RUN_SUITE(s_custom_headers);
  GREATEST_MAIN_END();
}
