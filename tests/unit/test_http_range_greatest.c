// Unit tests for http_range.c: Range header parsing, resolution, and
// Content-Range formatting.

#include <string.h>
#include <stdint.h>
#include "../vendor/greatest_color.h"
#include "../vendor/greatest.h"

#include "include/http_range.h"

// ---------------------------------------------------------------------------
// Parsing tests
// ---------------------------------------------------------------------------

TEST parse_closed_range(void) {
  struct byte_range br = http_range_parse("bytes=0-499", 11);
  ASSERT_EQ(br.valid, 1);
  ASSERT_EQ(br.suffix, 0);
  ASSERT_EQ(br.first, 0ULL);
  ASSERT_EQ(br.last, 499ULL);
  PASS();
}

TEST parse_open_ended_range(void) {
  struct byte_range br = http_range_parse("bytes=500-", 10);
  ASSERT_EQ(br.valid, 1);
  ASSERT_EQ(br.suffix, 0);
  ASSERT_EQ(br.first, 500ULL);
  ASSERT_EQ(br.last, UINT64_MAX);
  PASS();
}

TEST parse_suffix_range(void) {
  struct byte_range br = http_range_parse("bytes=-500", 10);
  ASSERT_EQ(br.valid, 1);
  ASSERT_EQ(br.suffix, 1);
  ASSERT_EQ(br.first, 500ULL);
  PASS();
}

TEST parse_single_byte(void) {
  struct byte_range br = http_range_parse("bytes=0-0", 9);
  ASSERT_EQ(br.valid, 1);
  ASSERT_EQ(br.suffix, 0);
  ASSERT_EQ(br.first, 0ULL);
  ASSERT_EQ(br.last, 0ULL);
  PASS();
}

TEST parse_case_insensitive(void) {
  struct byte_range br = http_range_parse("Bytes=10-20", 11);
  ASSERT_EQ(br.valid, 1);
  ASSERT_EQ(br.first, 10ULL);
  ASSERT_EQ(br.last, 20ULL);
  PASS();
}

TEST parse_multirange_rejected(void) {
  struct byte_range br = http_range_parse("bytes=0-100, 200-300", 20);
  ASSERT_EQ(br.valid, 0);
  PASS();
}

TEST parse_wrong_unit(void) {
  struct byte_range br = http_range_parse("chars=0-100", 11);
  ASSERT_EQ(br.valid, 0);
  PASS();
}

TEST parse_empty(void) {
  struct byte_range br = http_range_parse("bytes=", 6);
  ASSERT_EQ(br.valid, 0);
  PASS();
}

TEST parse_null(void) {
  struct byte_range br = http_range_parse(NULL, 0);
  ASSERT_EQ(br.valid, 0);
  PASS();
}

TEST parse_first_greater_than_last(void) {
  struct byte_range br = http_range_parse("bytes=500-499", 13);
  ASSERT_EQ(br.valid, 0);
  PASS();
}

TEST parse_non_digits(void) {
  struct byte_range br = http_range_parse("bytes=abc-def", 13);
  ASSERT_EQ(br.valid, 0);
  PASS();
}

TEST parse_suffix_zero(void) {
  // bytes=-0 is invalid (suffix length 0)
  struct byte_range br = http_range_parse("bytes=-0", 8);
  ASSERT_EQ(br.valid, 0);
  PASS();
}

TEST parse_trailing_junk(void) {
  struct byte_range br = http_range_parse("bytes=0-499xyz", 14);
  ASSERT_EQ(br.valid, 0);
  PASS();
}

TEST parse_ows_after_equals(void) {
  struct byte_range br = http_range_parse("bytes= 0-499", 12);
  ASSERT_EQ(br.valid, 1);
  ASSERT_EQ(br.first, 0ULL);
  ASSERT_EQ(br.last, 499ULL);
  PASS();
}

// ---------------------------------------------------------------------------
// Resolution tests
// ---------------------------------------------------------------------------

TEST resolve_closed_within_file(void) {
  struct byte_range br = {.valid = 1, .suffix = 0, .first = 0, .last = 499};
  struct resolved_range rr = http_range_resolve(&br, 1000);
  ASSERT_EQ(rr.satisfiable, 1);
  ASSERT_EQ(rr.start, 0ULL);
  ASSERT_EQ(rr.end, 499ULL);
  ASSERT_EQ(rr.length, 500ULL);
  PASS();
}

TEST resolve_closed_clamped(void) {
  struct byte_range br = {.valid = 1, .suffix = 0, .first = 0, .last = 499};
  struct resolved_range rr = http_range_resolve(&br, 100);
  ASSERT_EQ(rr.satisfiable, 1);
  ASSERT_EQ(rr.start, 0ULL);
  ASSERT_EQ(rr.end, 99ULL);
  ASSERT_EQ(rr.length, 100ULL);
  PASS();
}

TEST resolve_open_ended(void) {
  struct byte_range br = {.valid = 1, .suffix = 0, .first = 500, .last = UINT64_MAX};
  struct resolved_range rr = http_range_resolve(&br, 1000);
  ASSERT_EQ(rr.satisfiable, 1);
  ASSERT_EQ(rr.start, 500ULL);
  ASSERT_EQ(rr.end, 999ULL);
  ASSERT_EQ(rr.length, 500ULL);
  PASS();
}

TEST resolve_suffix(void) {
  struct byte_range br = {.valid = 1, .suffix = 1, .first = 500, .last = 0};
  struct resolved_range rr = http_range_resolve(&br, 1000);
  ASSERT_EQ(rr.satisfiable, 1);
  ASSERT_EQ(rr.start, 500ULL);
  ASSERT_EQ(rr.end, 999ULL);
  ASSERT_EQ(rr.length, 500ULL);
  PASS();
}

TEST resolve_suffix_larger_than_file(void) {
  struct byte_range br = {.valid = 1, .suffix = 1, .first = 500, .last = 0};
  struct resolved_range rr = http_range_resolve(&br, 100);
  ASSERT_EQ(rr.satisfiable, 1);
  ASSERT_EQ(rr.start, 0ULL);
  ASSERT_EQ(rr.end, 99ULL);
  ASSERT_EQ(rr.length, 100ULL);
  PASS();
}

TEST resolve_unsatisfiable_past_eof(void) {
  struct byte_range br = {.valid = 1, .suffix = 0, .first = 1000, .last = UINT64_MAX};
  struct resolved_range rr = http_range_resolve(&br, 1000);
  ASSERT_EQ(rr.satisfiable, 0);
  PASS();
}

TEST resolve_zero_length_file(void) {
  struct byte_range br = {.valid = 1, .suffix = 0, .first = 0, .last = 0};
  struct resolved_range rr = http_range_resolve(&br, 0);
  ASSERT_EQ(rr.satisfiable, 0);
  PASS();
}

TEST resolve_single_byte_at_end(void) {
  struct byte_range br = {.valid = 1, .suffix = 0, .first = 999, .last = 999};
  struct resolved_range rr = http_range_resolve(&br, 1000);
  ASSERT_EQ(rr.satisfiable, 1);
  ASSERT_EQ(rr.start, 999ULL);
  ASSERT_EQ(rr.end, 999ULL);
  ASSERT_EQ(rr.length, 1ULL);
  PASS();
}

// ---------------------------------------------------------------------------
// Content-Range formatting tests
// ---------------------------------------------------------------------------

TEST format_content_range_206(void) {
  char buf[128];
  size_t n = http_range_format_content_range(buf, sizeof(buf), 0, 499, 1000);
  ASSERT(n > 0);
  ASSERT_STR_EQ(buf, "Content-Range: bytes 0-499/1000\r\n");
  PASS();
}

TEST format_content_range_416(void) {
  char buf[128];
  size_t n = http_range_format_content_range_unsatisfied(buf, sizeof(buf), 1000);
  ASSERT(n > 0);
  ASSERT_STR_EQ(buf, "Content-Range: bytes */1000\r\n");
  PASS();
}

TEST format_content_range_small_buf(void) {
  char buf[5];
  size_t n = http_range_format_content_range(buf, sizeof(buf), 0, 499, 1000);
  ASSERT_EQ(n, 0ULL);
  PASS();
}

// ---------------------------------------------------------------------------
// Test suites
// ---------------------------------------------------------------------------

SUITE(parse_suite) {
  RUN_TEST(parse_closed_range);
  RUN_TEST(parse_open_ended_range);
  RUN_TEST(parse_suffix_range);
  RUN_TEST(parse_single_byte);
  RUN_TEST(parse_case_insensitive);
  RUN_TEST(parse_multirange_rejected);
  RUN_TEST(parse_wrong_unit);
  RUN_TEST(parse_empty);
  RUN_TEST(parse_null);
  RUN_TEST(parse_first_greater_than_last);
  RUN_TEST(parse_non_digits);
  RUN_TEST(parse_suffix_zero);
  RUN_TEST(parse_trailing_junk);
  RUN_TEST(parse_ows_after_equals);
}

SUITE(resolve_suite) {
  RUN_TEST(resolve_closed_within_file);
  RUN_TEST(resolve_closed_clamped);
  RUN_TEST(resolve_open_ended);
  RUN_TEST(resolve_suffix);
  RUN_TEST(resolve_suffix_larger_than_file);
  RUN_TEST(resolve_unsatisfiable_past_eof);
  RUN_TEST(resolve_zero_length_file);
  RUN_TEST(resolve_single_byte_at_end);
}

SUITE(format_suite) {
  RUN_TEST(format_content_range_206);
  RUN_TEST(format_content_range_416);
  RUN_TEST(format_content_range_small_buf);
}

GREATEST_MAIN_DEFS();

int main(int argc, char *argv[]) {
  GREATEST_MAIN_BEGIN();
  RUN_SUITE(parse_suite);
  RUN_SUITE(resolve_suite);
  RUN_SUITE(format_suite);
  GREATEST_MAIN_END();
}
