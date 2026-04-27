// Unit tests for compression.c: Accept-Encoding parsing, MIME compressibility,
// and encoding metadata helpers.

#include <string.h>
#include "../vendor/greatest_color.h"
#include "../vendor/greatest.h"

#include "include/compression.h"

// ---------------------------------------------------------------------------
// compress_parse_accept_encoding: basic tokens
// ---------------------------------------------------------------------------

TEST ae_empty(void) {
  ASSERT_EQ(compress_parse_accept_encoding("", 0), 0u);
  ASSERT_EQ(compress_parse_accept_encoding(NULL, 0), 0u);
  PASS();
}

TEST ae_gzip_only(void) {
  unsigned r = compress_parse_accept_encoding("gzip", 4);
  ASSERT(r & COMP_ENC_GZIP);
  ASSERT(!(r & COMP_ENC_BROTLI));
  PASS();
}

TEST ae_br_only(void) {
  unsigned r = compress_parse_accept_encoding("br", 2);
  ASSERT(r & COMP_ENC_BROTLI);
  ASSERT(!(r & COMP_ENC_GZIP));
  PASS();
}

TEST ae_gzip_and_br(void) {
  const char *v = "gzip, br";
  unsigned r = compress_parse_accept_encoding(v, strlen(v));
  ASSERT(r & COMP_ENC_GZIP);
  ASSERT(r & COMP_ENC_BROTLI);
  PASS();
}

TEST ae_case_insensitive(void) {
  const char *v = "GZIP, BR";
  unsigned r = compress_parse_accept_encoding(v, strlen(v));
  ASSERT(r & COMP_ENC_GZIP);
  ASSERT(r & COMP_ENC_BROTLI);
  PASS();
}

TEST ae_identity_ignored(void) {
  // identity is not a compressible encoding we serve; it should not set any bit
  const char *v = "identity";
  unsigned r = compress_parse_accept_encoding(v, strlen(v));
  ASSERT_EQ(r, 0u);
  PASS();
}

TEST ae_deflate_ignored(void) {
  // deflate is parsed but not served (ambiguous format)
  const char *v = "deflate";
  unsigned r = compress_parse_accept_encoding(v, strlen(v));
  ASSERT_EQ(r, 0u);
  PASS();
}

// ---------------------------------------------------------------------------
// q-value handling
// ---------------------------------------------------------------------------

TEST ae_q1_accepted(void) {
  const char *v = "gzip;q=1.0";
  unsigned r = compress_parse_accept_encoding(v, strlen(v));
  ASSERT(r & COMP_ENC_GZIP);
  PASS();
}

TEST ae_q0_excluded(void) {
  const char *v = "gzip;q=0";
  unsigned r = compress_parse_accept_encoding(v, strlen(v));
  ASSERT(!(r & COMP_ENC_GZIP));
  PASS();
}

TEST ae_q0_decimal_excluded(void) {
  const char *v = "gzip;q=0.0";
  unsigned r = compress_parse_accept_encoding(v, strlen(v));
  ASSERT(!(r & COMP_ENC_GZIP));
  PASS();
}

TEST ae_q0_000_excluded(void) {
  const char *v = "gzip;q=0.000";
  unsigned r = compress_parse_accept_encoding(v, strlen(v));
  ASSERT(!(r & COMP_ENC_GZIP));
  PASS();
}

TEST ae_q0_001_accepted(void) {
  // q=0.001 is non-zero: should be accepted
  const char *v = "gzip;q=0.001";
  unsigned r = compress_parse_accept_encoding(v, strlen(v));
  ASSERT(r & COMP_ENC_GZIP);
  PASS();
}

TEST ae_q_uppercase(void) {
  const char *v = "gzip;Q=0";
  unsigned r = compress_parse_accept_encoding(v, strlen(v));
  ASSERT(!(r & COMP_ENC_GZIP));
  PASS();
}

TEST ae_mixed_q(void) {
  // br accepted, gzip excluded
  const char *v = "br;q=1.0, gzip;q=0";
  unsigned r = compress_parse_accept_encoding(v, strlen(v));
  ASSERT(r & COMP_ENC_BROTLI);
  ASSERT(!(r & COMP_ENC_GZIP));
  PASS();
}

// ---------------------------------------------------------------------------
// Wildcard handling (RFC 7231 §5.3.4)
// ---------------------------------------------------------------------------

TEST ae_wildcard_accepts_all(void) {
  const char *v = "*";
  unsigned r = compress_parse_accept_encoding(v, strlen(v));
  ASSERT(r & COMP_ENC_GZIP);
  ASSERT(r & COMP_ENC_BROTLI);
  PASS();
}

TEST ae_wildcard_then_explicit_exclude(void) {
  // '*' first sets all, then explicit gzip;q=0 excludes gzip.
  // Per RFC: explicit entries take precedence over wildcard regardless of order.
  const char *v = "*, gzip;q=0";
  unsigned r = compress_parse_accept_encoding(v, strlen(v));
  ASSERT(!(r & COMP_ENC_GZIP));
  ASSERT(r & COMP_ENC_BROTLI);
  PASS();
}

TEST ae_explicit_exclude_then_wildcard(void) {
  // gzip;q=0 first, then '*'. Explicit still wins.
  const char *v = "gzip;q=0, *";
  unsigned r = compress_parse_accept_encoding(v, strlen(v));
  ASSERT(!(r & COMP_ENC_GZIP));
  ASSERT(r & COMP_ENC_BROTLI);
  PASS();
}

TEST ae_wildcard_q0(void) {
  // '*;q=0' plus explicit 'br': only br should be accepted.
  const char *v = "br, *;q=0";
  unsigned r = compress_parse_accept_encoding(v, strlen(v));
  ASSERT(r & COMP_ENC_BROTLI);
  ASSERT(!(r & COMP_ENC_GZIP));
  PASS();
}

// ---------------------------------------------------------------------------
// Whitespace tolerance
// ---------------------------------------------------------------------------

TEST ae_leading_trailing_spaces(void) {
  const char *v = "  gzip  ,  br  ";
  unsigned r = compress_parse_accept_encoding(v, strlen(v));
  ASSERT(r & COMP_ENC_GZIP);
  ASSERT(r & COMP_ENC_BROTLI);
  PASS();
}

TEST ae_tabs_between_tokens(void) {
  const char *v = "gzip\t,\tbr";
  unsigned r = compress_parse_accept_encoding(v, strlen(v));
  ASSERT(r & COMP_ENC_GZIP);
  ASSERT(r & COMP_ENC_BROTLI);
  PASS();
}

// ---------------------------------------------------------------------------
// Typical browser Accept-Encoding values
// ---------------------------------------------------------------------------

TEST ae_chrome_typical(void) {
  // Chrome sends: gzip, deflate, br
  const char *v = "gzip, deflate, br";
  unsigned r = compress_parse_accept_encoding(v, strlen(v));
  ASSERT(r & COMP_ENC_GZIP);
  ASSERT(r & COMP_ENC_BROTLI);
  PASS();
}

TEST ae_firefox_typical(void) {
  // Firefox sends: gzip, deflate, br
  const char *v = "gzip, deflate, br";
  unsigned r = compress_parse_accept_encoding(v, strlen(v));
  ASSERT(r & COMP_ENC_GZIP);
  ASSERT(r & COMP_ENC_BROTLI);
  PASS();
}

TEST ae_curl_default(void) {
  // curl --compressed sends: deflate, gzip, br, zstd
  const char *v = "deflate, gzip, br, zstd";
  unsigned r = compress_parse_accept_encoding(v, strlen(v));
  ASSERT(r & COMP_ENC_GZIP);
  ASSERT(r & COMP_ENC_BROTLI);
  PASS();
}

TEST ae_gzip_only_no_br(void) {
  const char *v = "gzip, deflate";
  unsigned r = compress_parse_accept_encoding(v, strlen(v));
  ASSERT(r & COMP_ENC_GZIP);
  ASSERT(!(r & COMP_ENC_BROTLI));
  PASS();
}

// ---------------------------------------------------------------------------
// compress_mime_is_compressible
// ---------------------------------------------------------------------------

TEST mime_html_compressible(void) {
  ASSERT_EQ(compress_mime_is_compressible("text/html; charset=utf-8"), 1);
  PASS();
}

TEST mime_css_compressible(void) {
  ASSERT_EQ(compress_mime_is_compressible("text/css; charset=utf-8"), 1);
  PASS();
}

TEST mime_js_compressible(void) {
  ASSERT_EQ(compress_mime_is_compressible("application/javascript; charset=utf-8"), 1);
  PASS();
}

TEST mime_json_compressible(void) {
  ASSERT_EQ(compress_mime_is_compressible("application/json; charset=utf-8"), 1);
  PASS();
}

TEST mime_svg_compressible(void) {
  ASSERT_EQ(compress_mime_is_compressible("image/svg+xml"), 1);
  PASS();
}

TEST mime_jpeg_not_compressible(void) {
  ASSERT_EQ(compress_mime_is_compressible("image/jpeg"), 0);
  PASS();
}

TEST mime_png_not_compressible(void) {
  ASSERT_EQ(compress_mime_is_compressible("image/png"), 0);
  PASS();
}

TEST mime_webp_not_compressible(void) {
  ASSERT_EQ(compress_mime_is_compressible("image/webp"), 0);
  PASS();
}

TEST mime_avif_not_compressible(void) {
  ASSERT_EQ(compress_mime_is_compressible("image/avif"), 0);
  PASS();
}

TEST mime_woff2_not_compressible(void) {
  ASSERT_EQ(compress_mime_is_compressible("font/woff2"), 0);
  PASS();
}

TEST mime_zip_not_compressible(void) {
  ASSERT_EQ(compress_mime_is_compressible("application/zip"), 0);
  PASS();
}

TEST mime_octet_stream_not_compressible(void) {
  ASSERT_EQ(compress_mime_is_compressible("application/octet-stream"), 0);
  PASS();
}

TEST mime_video_not_compressible(void) {
  ASSERT_EQ(compress_mime_is_compressible("video/mp4"), 0);
  PASS();
}

TEST mime_audio_not_compressible(void) {
  ASSERT_EQ(compress_mime_is_compressible("audio/mpeg"), 0);
  PASS();
}

TEST mime_null_not_compressible(void) {
  ASSERT_EQ(compress_mime_is_compressible(NULL), 0);
  PASS();
}

// ---------------------------------------------------------------------------
// compress_enc_ext / compress_enc_name
// ---------------------------------------------------------------------------

TEST enc_ext_gzip(void) {
  ASSERT_STR_EQ(compress_enc_ext(COMP_ENC_GZIP), ".gz");
  PASS();
}

TEST enc_ext_brotli(void) {
  ASSERT_STR_EQ(compress_enc_ext(COMP_ENC_BROTLI), ".br");
  PASS();
}

TEST enc_ext_unknown(void) {
  ASSERT_EQ(compress_enc_ext(0), NULL);
  PASS();
}

TEST enc_name_gzip(void) {
  ASSERT_STR_EQ(compress_enc_name(COMP_ENC_GZIP), "gzip");
  PASS();
}

TEST enc_name_brotli(void) {
  ASSERT_STR_EQ(compress_enc_name(COMP_ENC_BROTLI), "br");
  PASS();
}

TEST enc_name_unknown(void) {
  ASSERT_EQ(compress_enc_name(0), NULL);
  PASS();
}

// ---------------------------------------------------------------------------
// Suites
// ---------------------------------------------------------------------------

SUITE(ae_basic) {
  RUN_TEST(ae_empty);
  RUN_TEST(ae_gzip_only);
  RUN_TEST(ae_br_only);
  RUN_TEST(ae_gzip_and_br);
  RUN_TEST(ae_case_insensitive);
  RUN_TEST(ae_identity_ignored);
  RUN_TEST(ae_deflate_ignored);
}

SUITE(ae_q_values) {
  RUN_TEST(ae_q1_accepted);
  RUN_TEST(ae_q0_excluded);
  RUN_TEST(ae_q0_decimal_excluded);
  RUN_TEST(ae_q0_000_excluded);
  RUN_TEST(ae_q0_001_accepted);
  RUN_TEST(ae_q_uppercase);
  RUN_TEST(ae_mixed_q);
}

SUITE(ae_wildcard) {
  RUN_TEST(ae_wildcard_accepts_all);
  RUN_TEST(ae_wildcard_then_explicit_exclude);
  RUN_TEST(ae_explicit_exclude_then_wildcard);
  RUN_TEST(ae_wildcard_q0);
}

SUITE(ae_whitespace) {
  RUN_TEST(ae_leading_trailing_spaces);
  RUN_TEST(ae_tabs_between_tokens);
}

SUITE(ae_browser_values) {
  RUN_TEST(ae_chrome_typical);
  RUN_TEST(ae_firefox_typical);
  RUN_TEST(ae_curl_default);
  RUN_TEST(ae_gzip_only_no_br);
}

SUITE(mime_compressibility) {
  RUN_TEST(mime_html_compressible);
  RUN_TEST(mime_css_compressible);
  RUN_TEST(mime_js_compressible);
  RUN_TEST(mime_json_compressible);
  RUN_TEST(mime_svg_compressible);
  RUN_TEST(mime_jpeg_not_compressible);
  RUN_TEST(mime_png_not_compressible);
  RUN_TEST(mime_webp_not_compressible);
  RUN_TEST(mime_avif_not_compressible);
  RUN_TEST(mime_woff2_not_compressible);
  RUN_TEST(mime_zip_not_compressible);
  RUN_TEST(mime_octet_stream_not_compressible);
  RUN_TEST(mime_video_not_compressible);
  RUN_TEST(mime_audio_not_compressible);
  RUN_TEST(mime_null_not_compressible);
}

SUITE(enc_metadata) {
  RUN_TEST(enc_ext_gzip);
  RUN_TEST(enc_ext_brotli);
  RUN_TEST(enc_ext_unknown);
  RUN_TEST(enc_name_gzip);
  RUN_TEST(enc_name_brotli);
  RUN_TEST(enc_name_unknown);
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv) {
  GREATEST_MAIN_BEGIN();
  RUN_SUITE(ae_basic);
  RUN_SUITE(ae_q_values);
  RUN_SUITE(ae_wildcard);
  RUN_SUITE(ae_whitespace);
  RUN_SUITE(ae_browser_values);
  RUN_SUITE(mime_compressibility);
  RUN_SUITE(enc_metadata);
  GREATEST_MAIN_END();
}
