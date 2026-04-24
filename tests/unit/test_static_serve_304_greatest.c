// Unit tests for the 304 Not Modified conditional-request helpers in
// static_serve_utils.c (format_etag, format_last_modified, parse_http_date,
// check_not_modified, build_validator_headers).
//
// Because the helpers are `static`, we include the implementation file
// directly and stub out external dependencies that we don't exercise.

#define _GNU_SOURCE
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include "../vendor/greatest_color.h"
#include "../vendor/greatest.h"

// ---------------------------------------------------------------------------
// Include real headers so types and signatures match. The #pragma once guards
// prevent double-include when the .c file includes them again.
// ---------------------------------------------------------------------------
#include "include/types.h"
#include "include/config.h"
#include "include/conn.h"
#include "include/http_headers.h"
#include "include/request_handlers.h"
#include "include/static_serve_utils.h"
#include "include/tx.h"

// ---------------------------------------------------------------------------
// Stubs for external symbols referenced by static_serve_utils.c that we
// don't exercise in these tests.
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

// ---------------------------------------------------------------------------
// Controllable stub for http_header_find_value used by check_not_modified.
// ---------------------------------------------------------------------------
static const char *g_stub_inm_value = NULL;
static uint16_t g_stub_inm_len = 0;
static const char *g_stub_ims_value = NULL;
static uint16_t g_stub_ims_len = 0;

const char *http_header_find_value(const struct req_hdr_entry *hdrs,
                                   uint8_t hdr_count,
                                   enum http_header_id id,
                                   uint16_t *out_len) {
  (void)hdrs;
  (void)hdr_count;
  if (id == HDR_ID_IF_NONE_MATCH) {
    if (out_len) *out_len = g_stub_inm_len;
    return g_stub_inm_value;
  }
  if (id == HDR_ID_IF_MODIFIED_SINCE) {
    if (out_len) *out_len = g_stub_ims_len;
    return g_stub_ims_value;
  }
  if (out_len) *out_len = 0;
  return NULL;
}

// Stubs for remaining http_headers symbols (never called, but linked).
uint64_t http_headers_store_mask(uint64_t features) { (void)features; return 0; }
int http_header_lookup_lower(const char *n, size_t l, enum http_header_id *o) {
  (void)n; (void)l; (void)o; return -1;
}

// Now include the file under test — gets access to the static helpers.
#include "../../src/static_serve_utils.c"

// ---------------------------------------------------------------------------
// Helper: build a struct stat with known inode/size/mtime.
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
// format_etag tests
// ===========================================================================

TEST t_format_etag_basic(void) {
  struct stat st = make_stat(0x1234, 4096, 1000, 500000); // 500us = 500000ns
  char buf[64];
  size_t n = static_serve_format_etag(buf, sizeof(buf), &st);
  ASSERT_GT(n, 0);
  // Verify it's quoted.
  ASSERT_EQ(buf[0], '"');
  ASSERT_EQ(buf[n - 1], '"');
  // Verify it contains the hex fields separated by dashes.
  ASSERT(strstr(buf, "1234-") != NULL);
  ASSERT(strstr(buf, "-1000-") != NULL);
  PASS();
}

TEST t_format_etag_null_buf(void) {
  struct stat st = make_stat(1, 1, 1, 0);
  ASSERT_EQ(static_serve_format_etag(NULL, 64, &st), 0);
  PASS();
}

TEST t_format_etag_null_stat(void) {
  char buf[64];
  ASSERT_EQ(static_serve_format_etag(buf, sizeof(buf), NULL), 0);
  PASS();
}


// ===========================================================================
// format_last_modified tests
// ===========================================================================

TEST t_format_last_mod_known_date(void) {
  // 2024-01-15 12:30:45 UTC = Monday
  struct stat st = make_stat(1, 1, 1705322445, 0);
  char buf[64];
  size_t n = static_serve_format_last_modified(buf, sizeof(buf), &st);
  ASSERT_GT(n, 0);
  // IMF-fixdate: "Mon, 15 Jan 2024 12:40:45 GMT"
  ASSERT_STR_EQ(buf, "Mon, 15 Jan 2024 12:40:45 GMT");
  PASS();
}

TEST t_format_last_mod_epoch(void) {
  struct stat st = make_stat(1, 1, 0, 0);
  char buf[64];
  size_t n = static_serve_format_last_modified(buf, sizeof(buf), &st);
  ASSERT_GT(n, 0);
  // 1970-01-01 00:00:00 UTC = Thursday
  ASSERT_STR_EQ(buf, "Thu, 01 Jan 1970 00:00:00 GMT");
  PASS();
}

TEST t_format_last_mod_null_guards(void) {
  struct stat st = make_stat(1, 1, 0, 0);
  char buf[64];
  ASSERT_EQ(static_serve_format_last_modified(NULL, 64, &st), 0);
  ASSERT_EQ(static_serve_format_last_modified(buf, sizeof(buf), NULL), 0);
  PASS();
}

// ===========================================================================
// parse_http_date tests
// ===========================================================================

TEST t_parse_date_valid_imf(void) {
  time_t out = 0;
  ASSERT_EQ(static_serve_parse_http_date("Sun, 06 Nov 1994 08:49:37 GMT", &out), 0);
  // 1994-11-06 08:49:37 UTC
  struct tm tm;
  gmtime_r(&out, &tm);
  ASSERT_EQ(tm.tm_year, 1994 - 1900);
  ASSERT_EQ(tm.tm_mon, 10); // November = 10
  ASSERT_EQ(tm.tm_mday, 6);
  ASSERT_EQ(tm.tm_hour, 8);
  ASSERT_EQ(tm.tm_min, 49);
  ASSERT_EQ(tm.tm_sec, 37);
  PASS();
}

TEST t_parse_date_epoch(void) {
  time_t out = 99;
  ASSERT_EQ(static_serve_parse_http_date("Thu, 01 Jan 1970 00:00:00 GMT", &out), 0);
  ASSERT_EQ(out, 0);
  PASS();
}

TEST t_parse_date_roundtrip(void) {
  // Format a known date, then parse it back.
  struct stat st = make_stat(1, 1, 1705322445, 0);
  char buf[64];
  size_t n = static_serve_format_last_modified(buf, sizeof(buf), &st);
  ASSERT_GT(n, 0);

  time_t out = 0;
  ASSERT_EQ(static_serve_parse_http_date(buf, &out), 0);
  ASSERT_EQ(out, 1705322445);
  PASS();
}

TEST t_parse_date_null_guards(void) {
  time_t out;
  ASSERT_EQ(static_serve_parse_http_date(NULL, &out), -1);
  ASSERT_EQ(static_serve_parse_http_date("Sun, 06 Nov 1994 08:49:37 GMT", NULL), -1);
  PASS();
}

TEST t_parse_date_empty_string(void) {
  time_t out;
  ASSERT_EQ(static_serve_parse_http_date("", &out), -1);
  PASS();
}

TEST t_parse_date_short_string(void) {
  time_t out;
  ASSERT_EQ(static_serve_parse_http_date("Sun", &out), -1);
  ASSERT_EQ(static_serve_parse_http_date("Sun, ", &out), -1);
  ASSERT_EQ(static_serve_parse_http_date("Sun, 06", &out), -1);
  ASSERT_EQ(static_serve_parse_http_date("Sun, 06 Nov", &out), -1);
  ASSERT_EQ(static_serve_parse_http_date("Sun, 06 Nov 1994", &out), -1);
  ASSERT_EQ(static_serve_parse_http_date("Sun, 06 Nov 1994 08:49:37", &out), -1);
  PASS();
}

TEST t_parse_date_missing_gmt(void) {
  time_t out;
  ASSERT_EQ(static_serve_parse_http_date("Sun, 06 Nov 1994 08:49:37 UTC", &out), -1);
  ASSERT_EQ(static_serve_parse_http_date("Sun, 06 Nov 1994 08:49:37 EST", &out), -1);
  PASS();
}

TEST t_parse_date_trailing_garbage(void) {
  time_t out;
  ASSERT_EQ(static_serve_parse_http_date("Sun, 06 Nov 1994 08:49:37 GMTextra", &out), -1);
  ASSERT_EQ(static_serve_parse_http_date("Sun, 06 Nov 1994 08:49:37 GMT ", &out), -1);
  PASS();
}

TEST t_parse_date_bad_month(void) {
  time_t out;
  ASSERT_EQ(static_serve_parse_http_date("Sun, 06 Foo 1994 08:49:37 GMT", &out), -1);
  PASS();
}

TEST t_parse_date_out_of_range_hour(void) {
  time_t out;
  ASSERT_EQ(static_serve_parse_http_date("Sun, 06 Nov 1994 25:49:37 GMT", &out), -1);
  PASS();
}

TEST t_parse_date_out_of_range_minute(void) {
  time_t out;
  ASSERT_EQ(static_serve_parse_http_date("Sun, 06 Nov 1994 08:60:37 GMT", &out), -1);
  PASS();
}

TEST t_parse_date_out_of_range_second(void) {
  time_t out;
  ASSERT_EQ(static_serve_parse_http_date("Sun, 06 Nov 1994 08:49:61 GMT", &out), -1);
  PASS();
}

TEST t_parse_date_leap_second(void) {
  time_t out;
  // sec=60 is valid (leap second per RFC 7231).
  ASSERT_EQ(static_serve_parse_http_date("Sun, 06 Nov 1994 08:49:60 GMT", &out), 0);
  PASS();
}

TEST t_parse_date_day_zero_rejected(void) {
  time_t out;
  ASSERT_EQ(static_serve_parse_http_date("Sun, 00 Nov 1994 08:49:37 GMT", &out), -1);
  PASS();
}

TEST t_parse_date_day_32_rejected(void) {
  time_t out;
  ASSERT_EQ(static_serve_parse_http_date("Sun, 32 Nov 1994 08:49:37 GMT", &out), -1);
  PASS();
}

// ===========================================================================
// check_not_modified tests
// ===========================================================================

// Helper: set up stubs and call check_not_modified with given INM/IMS values.
static int do_check(const char *etag, size_t etag_len,
                    const struct stat *st,
                    const char *inm, const char *ims) {
  g_stub_inm_value = inm;
  g_stub_inm_len = inm ? (uint16_t)strlen(inm) : 0;
  g_stub_ims_value = ims;
  g_stub_ims_len = ims ? (uint16_t)strlen(ims) : 0;

  struct conn c;
  memset(&c, 0, sizeof(c));

  return static_serve_check_not_modified(&c, st, etag, etag_len);
}

TEST t_cnm_inm_exact_match(void) {
  struct stat st = make_stat(0x1234, 4096, 1000, 500000);
  char etag[64];
  size_t elen = static_serve_format_etag(etag, sizeof(etag), &st);

  ASSERT_EQ(do_check(etag, elen, &st, etag, NULL), 1);
  PASS();
}

TEST t_cnm_inm_no_match(void) {
  struct stat st = make_stat(0x1234, 4096, 1000, 500000);
  char etag[64];
  size_t elen = static_serve_format_etag(etag, sizeof(etag), &st);

  ASSERT_EQ(do_check(etag, elen, &st, "\"no-match\"", NULL), 0);
  PASS();
}

TEST t_cnm_inm_wildcard(void) {
  struct stat st = make_stat(1, 1, 1, 0);
  ASSERT_EQ(do_check("\"any\"", 5, &st, "*", NULL), 1);
  PASS();
}

TEST t_cnm_inm_weak_prefix(void) {
  struct stat st = make_stat(0x1234, 4096, 1000, 500000);
  char etag[64];
  size_t elen = static_serve_format_etag(etag, sizeof(etag), &st);

  // Prefix with W/ — should still match via weak comparison.
  char weak_inm[80];
  snprintf(weak_inm, sizeof(weak_inm), "W/%s", etag);
  ASSERT_EQ(do_check(etag, elen, &st, weak_inm, NULL), 1);
  PASS();
}

TEST t_cnm_inm_comma_list_match(void) {
  struct stat st = make_stat(0x1234, 4096, 1000, 500000);
  char etag[64];
  size_t elen = static_serve_format_etag(etag, sizeof(etag), &st);

  // Build a comma-separated list with the real ETag in the middle.
  char list[256];
  snprintf(list, sizeof(list), "\"aaa\", %s, \"zzz\"", etag);
  ASSERT_EQ(do_check(etag, elen, &st, list, NULL), 1);
  PASS();
}

TEST t_cnm_inm_comma_list_no_match(void) {
  struct stat st = make_stat(0x1234, 4096, 1000, 500000);
  char etag[64];
  size_t elen = static_serve_format_etag(etag, sizeof(etag), &st);

  ASSERT_EQ(do_check(etag, elen, &st, "\"aaa\", \"bbb\", \"ccc\"", NULL), 0);
  PASS();
}

TEST t_cnm_inm_with_ows(void) {
  struct stat st = make_stat(0x1234, 4096, 1000, 500000);
  char etag[64];
  size_t elen = static_serve_format_etag(etag, sizeof(etag), &st);

  // Extra whitespace around the matching ETag.
  char list[256];
  snprintf(list, sizeof(list), "  %s  ", etag);
  ASSERT_EQ(do_check(etag, elen, &st, list, NULL), 1);
  PASS();
}

TEST t_cnm_inm_precedence_over_ims(void) {
  // INM present but no match => should return 0, even if IMS would match.
  struct stat st = make_stat(1, 1, 1000, 0);
  ASSERT_EQ(do_check("\"real\"", 6, &st,
                      "\"no-match\"",
                      "Thu, 01 Jan 1970 00:00:00 GMT"), // IMS would match (mtime <= ims)
            0);
  PASS();
}

TEST t_cnm_ims_match(void) {
  // mtime=1000, IMS date is far in the future => match.
  struct stat st = make_stat(1, 1, 1000, 0);
  ASSERT_EQ(do_check("\"x\"", 3, &st,
                      NULL,
                      "Mon, 15 Jan 2024 12:30:45 GMT"), 1);
  PASS();
}

TEST t_cnm_ims_no_match(void) {
  // mtime far in the future, IMS date is epoch => no match.
  struct stat st = make_stat(1, 1, 1705322445, 0);
  ASSERT_EQ(do_check("\"x\"", 3, &st,
                      NULL,
                      "Thu, 01 Jan 1970 00:00:00 GMT"), 0);
  PASS();
}

TEST t_cnm_ims_invalid_date_ignored(void) {
  struct stat st = make_stat(1, 1, 1000, 0);
  ASSERT_EQ(do_check("\"x\"", 3, &st,
                      NULL,
                      "not-a-date"), 0);
  PASS();
}

TEST t_cnm_both_null(void) {
  struct stat st = make_stat(1, 1, 1000, 0);
  ASSERT_EQ(do_check("\"x\"", 3, &st, NULL, NULL), 0);
  PASS();
}

TEST t_cnm_null_conn(void) {
  struct stat st = make_stat(1, 1, 1000, 0);
  ASSERT_EQ(static_serve_check_not_modified(NULL, &st, "\"x\"", 3), 0);
  PASS();
}

// ===========================================================================
// build_validator_headers tests
// ===========================================================================

TEST t_build_validator_hdrs_basic(void) {
  char buf[256];
  size_t n = static_serve_build_validator_headers(buf, sizeof(buf),
      "\"abc\"", 5, "Sun, 06 Nov 1994 08:49:37 GMT", 29);
  ASSERT_GT(n, 0);
  ASSERT(strstr(buf, "ETag: \"abc\"\r\n") != NULL);
  ASSERT(strstr(buf, "Last-Modified: Sun, 06 Nov 1994 08:49:37 GMT\r\n") != NULL);
  PASS();
}

TEST t_build_validator_hdrs_null_buf(void) {
  ASSERT_EQ(static_serve_build_validator_headers(NULL, 256, "\"a\"", 3, "d", 1), 0);
  PASS();
}

// ===========================================================================
// Suites
// ===========================================================================

SUITE(s_format_etag) {
  RUN_TEST(t_format_etag_basic);
  RUN_TEST(t_format_etag_null_buf);
  RUN_TEST(t_format_etag_null_stat);
}

SUITE(s_format_last_modified) {
  RUN_TEST(t_format_last_mod_known_date);
  RUN_TEST(t_format_last_mod_epoch);
  RUN_TEST(t_format_last_mod_null_guards);
}

SUITE(s_parse_http_date) {
  RUN_TEST(t_parse_date_valid_imf);
  RUN_TEST(t_parse_date_epoch);
  RUN_TEST(t_parse_date_roundtrip);
  RUN_TEST(t_parse_date_null_guards);
  RUN_TEST(t_parse_date_empty_string);
  RUN_TEST(t_parse_date_short_string);
  RUN_TEST(t_parse_date_missing_gmt);
  RUN_TEST(t_parse_date_trailing_garbage);
  RUN_TEST(t_parse_date_bad_month);
  RUN_TEST(t_parse_date_out_of_range_hour);
  RUN_TEST(t_parse_date_out_of_range_minute);
  RUN_TEST(t_parse_date_out_of_range_second);
  RUN_TEST(t_parse_date_leap_second);
  RUN_TEST(t_parse_date_day_zero_rejected);
  RUN_TEST(t_parse_date_day_32_rejected);
}

SUITE(s_check_not_modified) {
  RUN_TEST(t_cnm_inm_exact_match);
  RUN_TEST(t_cnm_inm_no_match);
  RUN_TEST(t_cnm_inm_wildcard);
  RUN_TEST(t_cnm_inm_weak_prefix);
  RUN_TEST(t_cnm_inm_comma_list_match);
  RUN_TEST(t_cnm_inm_comma_list_no_match);
  RUN_TEST(t_cnm_inm_with_ows);
  RUN_TEST(t_cnm_inm_precedence_over_ims);
  RUN_TEST(t_cnm_ims_match);
  RUN_TEST(t_cnm_ims_no_match);
  RUN_TEST(t_cnm_ims_invalid_date_ignored);
  RUN_TEST(t_cnm_both_null);
  RUN_TEST(t_cnm_null_conn);
}

SUITE(s_validator_headers) {
  RUN_TEST(t_build_validator_hdrs_basic);
  RUN_TEST(t_build_validator_hdrs_null_buf);
}

GREATEST_MAIN_DEFS();
int main(int argc, char **argv) {
  GREATEST_MAIN_BEGIN();
  RUN_SUITE(s_format_etag);
  RUN_SUITE(s_format_last_modified);
  RUN_SUITE(s_parse_http_date);
  RUN_SUITE(s_check_not_modified);
  RUN_SUITE(s_validator_headers);
  GREATEST_MAIN_END();
}
