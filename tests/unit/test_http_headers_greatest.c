#include "../vendor/greatest_color.h"
#include "../vendor/greatest.h"

#include "include/http_headers.h"
#include "include/types.h"

TEST t_lookup_known_headers_lower(void) {
  enum http_header_id id;
  ASSERT_EQ(http_header_lookup_lower("host", 4, &id), 0);
  ASSERT_EQ(id, HDR_ID_HOST);

  ASSERT_EQ(http_header_lookup_lower("connection", 10, &id), 0);
  ASSERT_EQ(id, HDR_ID_CONNECTION);

  ASSERT_EQ(http_header_lookup_lower("if-modified-since", 17, &id), 0);
  ASSERT_EQ(id, HDR_ID_IF_MODIFIED_SINCE);

  PASS();
}

TEST t_lookup_unknown_header(void) {
  enum http_header_id id;
  ASSERT_EQ(http_header_lookup_lower("x-nope", 6, &id), -1);
  PASS();
}

TEST t_store_mask_base_always_includes_host_connection(void) {
  uint64_t m = http_headers_store_mask(CFG_FEAT_STATIC);
  ASSERT((m & http_header_bit(HDR_ID_HOST)) != 0);
  ASSERT((m & http_header_bit(HDR_ID_CONNECTION)) != 0);
  PASS();
}

TEST t_store_mask_range_features(void) {
  uint64_t m = http_headers_store_mask(CFG_FEAT_STATIC | CFG_FEAT_RANGE);
  ASSERT((m & http_header_bit(HDR_ID_RANGE)) != 0);
  ASSERT((m & http_header_bit(HDR_ID_IF_RANGE)) != 0);
  PASS();
}

TEST t_store_mask_conditional_features(void) {
  uint64_t m = http_headers_store_mask(CFG_FEAT_STATIC | CFG_FEAT_CONDITIONAL);
  ASSERT((m & http_header_bit(HDR_ID_IF_MODIFIED_SINCE)) != 0);
  ASSERT((m & http_header_bit(HDR_ID_IF_NONE_MATCH)) != 0);
  PASS();
}

TEST t_store_mask_compression_features(void) {
  uint64_t m = http_headers_store_mask(CFG_FEAT_STATIC | CFG_FEAT_COMPRESSION);
  ASSERT((m & http_header_bit(HDR_ID_ACCEPT_ENCODING)) != 0);
  PASS();
}

TEST t_store_mask_auth_features(void) {
  uint64_t m = http_headers_store_mask(CFG_FEAT_STATIC | CFG_FEAT_AUTH);
  ASSERT((m & http_header_bit(HDR_ID_AUTHORIZATION)) != 0);
  ASSERT((m & http_header_bit(HDR_ID_COOKIE)) != 0);
  PASS();
}

SUITE(s_http_headers) {
  RUN_TEST(t_lookup_known_headers_lower);
  RUN_TEST(t_lookup_unknown_header);
  RUN_TEST(t_store_mask_base_always_includes_host_connection);
  RUN_TEST(t_store_mask_range_features);
  RUN_TEST(t_store_mask_conditional_features);
  RUN_TEST(t_store_mask_compression_features);
  RUN_TEST(t_store_mask_auth_features);
}

GREATEST_MAIN_DEFS();
int main(int argc, char **argv) {
  GREATEST_MAIN_BEGIN();
  RUN_SUITE(s_http_headers);
  GREATEST_MAIN_END();
}
