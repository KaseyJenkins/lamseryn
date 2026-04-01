#include "../vendor/greatest_color.h"
#include "../vendor/greatest.h"

#include "include/url.h"

#include <string.h>

static int str_contains(const char *s, const char *needle) {
  return strstr(s, needle) != NULL;
}

static int normalized_looks_sane(const char *s) {
  // Absolute
  if (!s || s[0] != '/') {
    return 0;
  }
  // No double slashes (except the first char)
  if (str_contains(s, "//")) {
    return 0;
  }
  // No dot segments
  if (str_contains(s, "/./") || str_contains(s, "/../")) {
    return 0;
  }
  // No trailing "/." or "/.." either
  size_t n = strlen(s);
  if (n >= 2 && strcmp(s + (n - 2), "/.") == 0) {
    return 0;
  }
  if (n >= 3 && strcmp(s + (n - 3), "/..") == 0) {
    return 0;
  }
  return 1;
}

TEST t_split_path_query_basic(void) {
  const char *t = "/a/b?x=y";
  size_t path_len = 0, query_off = 0, query_len = 0;
  ASSERT_EQ(url_split_path_query(t, strlen(t), &path_len, &query_off, &query_len), 0);
  ASSERT_EQ(path_len, (size_t)4);
  ASSERT_EQ(query_off, (size_t)5);
  ASSERT_EQ(query_len, (size_t)3);
  PASS();
}

TEST t_split_path_query_empty_query(void) {
  const char *t = "/a/b?";
  size_t path_len = 0, query_off = 0, query_len = 0;
  ASSERT_EQ(url_split_path_query(t, strlen(t), &path_len, &query_off, &query_len), 0);
  ASSERT_EQ(path_len, (size_t)4);
  ASSERT_EQ(query_off, (size_t)5);
  ASSERT_EQ(query_len, (size_t)0);
  PASS();
}

TEST t_split_path_query_none(void) {
  const char *t = "/a/b";
  size_t path_len = 0, query_off = 0, query_len = 0;
  ASSERT_EQ(url_split_path_query(t, strlen(t), &path_len, &query_off, &query_len), 0);
  ASSERT_EQ(path_len, strlen(t));
  ASSERT_EQ(query_off, strlen(t));
  ASSERT_EQ(query_len, (size_t)0);
  PASS();
}

TEST t_split_path_query_multiple_qmark(void) {
  const char *t = "/p?a?b";
  size_t path_len = 0, query_off = 0, query_len = 0;
  ASSERT_EQ(url_split_path_query(t, strlen(t), &path_len, &query_off, &query_len), 0);
  ASSERT_EQ(path_len, (size_t)2);
  ASSERT_EQ(query_off, (size_t)3);
  ASSERT_EQ(query_len, (size_t)3);
  ASSERT_STR_EQ("a?b", t + query_off);
  PASS();
}

TEST t_percent_decode_ok_and_plus_literal(void) {
  char out[64];
  size_t out_len = 0;

  memset(out, 0, sizeof(out));
  ASSERT_EQ(url_percent_decode_strict("a%20b", 5, out, sizeof(out) - 1, &out_len, 1), 0);
  ASSERT_EQ(out_len, (size_t)3);
  out[out_len] = '\0';
  ASSERT_STR_EQ("a b", out);

  memset(out, 0, sizeof(out));
  ASSERT_EQ(url_percent_decode_strict("a+b", 3, out, sizeof(out) - 1, &out_len, 1), 0);
  ASSERT_EQ(out_len, (size_t)3);
  out[out_len] = '\0';
  ASSERT_STR_EQ("a+b", out);

  PASS();
}

TEST t_percent_decode_rejects_encoded_slashes_and_nul(void) {
  char out[16];
  size_t out_len = 0;

  ASSERT_EQ(url_percent_decode_strict("%2f", 3, out, sizeof(out), &out_len, 1), -1);
  ASSERT_EQ(url_percent_decode_strict("%5C", 3, out, sizeof(out), &out_len, 1), -1);
  ASSERT_EQ(url_percent_decode_strict("%00", 3, out, sizeof(out), &out_len, 0), -1);

  PASS();
}

TEST t_percent_decode_allows_encoded_slash_when_configured(void) {
  char out[16];
  size_t out_len = 0;

  ASSERT_EQ(url_percent_decode_strict("%2f", 3, out, sizeof(out), &out_len, 0), 0);
  ASSERT_EQ(out_len, (size_t)1);
  ASSERT_EQ(out[0], '/');

  ASSERT_EQ(url_percent_decode_strict("%5c", 3, out, sizeof(out), &out_len, 0), 0);
  ASSERT_EQ(out_len, (size_t)1);
  ASSERT_EQ(out[0], '\\');

  PASS();
}

TEST t_percent_decode_hex_case_insensitive(void) {
  char out[16];
  size_t out_len = 0;

  ASSERT_EQ(url_percent_decode_strict("%2F", 3, out, sizeof(out), &out_len, 0), 0);
  ASSERT_EQ(out_len, (size_t)1);
  ASSERT_EQ(out[0], '/');
  PASS();
}

TEST t_percent_decode_invalid_sequences(void) {
  char out[16];
  size_t out_len = 0;

  ASSERT_EQ(url_percent_decode_strict("%", 1, out, sizeof(out), &out_len, 1), -1);
  ASSERT_EQ(url_percent_decode_strict("%0", 2, out, sizeof(out), &out_len, 1), -1);
  ASSERT_EQ(url_percent_decode_strict("%G0", 3, out, sizeof(out), &out_len, 1), -1);

  PASS();
}

TEST t_percent_decode_overflow(void) {
  char out[2];
  size_t out_len = 0;

  // Needs 3 bytes but cap is 2.
  ASSERT_EQ(url_percent_decode_strict("abc", 3, out, sizeof(out), &out_len, 1), -2);
  PASS();
}

TEST t_normalize_absolute_basic(void) {
  char out[128];
  size_t out_len = 0;
  int ends_with_slash = 0;

  const char *in = "/a//b/./c/../d";
  ASSERT_EQ(
    url_path_normalize_absolute(in, strlen(in), out, sizeof(out) - 1, &out_len, &ends_with_slash),
    0);
  ASSERT_EQ(ends_with_slash, 0);
  out[out_len] = '\0';
  ASSERT_STR_EQ("/a/b/d", out);
  ASSERT(normalized_looks_sane(out));

  PASS();
}

TEST t_normalize_absolute_preserves_input_slash_flag(void) {
  char out[128];
  size_t out_len = 0;
  int ends_with_slash = 0;

  const char *in = "/a/b/";
  ASSERT_EQ(
    url_path_normalize_absolute(in, strlen(in), out, sizeof(out) - 1, &out_len, &ends_with_slash),
    0);
  ASSERT_EQ(ends_with_slash, 1);
  out[out_len] = '\0';
  ASSERT_STR_EQ("/a/b", out);
  ASSERT(normalized_looks_sane(out));

  PASS();
}

TEST t_normalize_absolute_clamps_at_root(void) {
  char out[128];
  size_t out_len = 0;
  int ends_with_slash = 0;

  const char *in = "/a/../../b";
  ASSERT_EQ(
    url_path_normalize_absolute(in, strlen(in), out, sizeof(out) - 1, &out_len, &ends_with_slash),
    0);
  out[out_len] = '\0';
  ASSERT_STR_EQ("/b", out);
  ASSERT(normalized_looks_sane(out));

  PASS();
}

TEST t_normalize_root_variants(void) {
  char out[32];
  size_t out_len = 0;
  int ends_with_slash = 0;

  ASSERT_EQ(url_path_normalize_absolute("/", 1, out, sizeof(out) - 1, &out_len, &ends_with_slash),
            0);
  ASSERT_EQ(ends_with_slash, 1);
  out[out_len] = '\0';
  ASSERT_STR_EQ("/", out);

  ASSERT_EQ(url_path_normalize_absolute("///", 3, out, sizeof(out) - 1, &out_len, &ends_with_slash),
            0);
  ASSERT_EQ(ends_with_slash, 1);
  out[out_len] = '\0';
  ASSERT_STR_EQ("/", out);

  ASSERT_EQ(
    url_path_normalize_absolute("/././", 5, out, sizeof(out) - 1, &out_len, &ends_with_slash),
    0);
  ASSERT_EQ(ends_with_slash, 1);
  out[out_len] = '\0';
  ASSERT_STR_EQ("/", out);

  ASSERT_EQ(
    url_path_normalize_absolute("/../", 4, out, sizeof(out) - 1, &out_len, &ends_with_slash),
    0);
  ASSERT_EQ(ends_with_slash, 1);
  out[out_len] = '\0';
  ASSERT_STR_EQ("/", out);

  PASS();
}

TEST t_normalize_overflow(void) {
  char out[8];
  size_t out_len = 0;
  int ends_with_slash = 0;

  // Would need 10 bytes including leading '/'.
  ASSERT_EQ(
    url_path_normalize_absolute("/abcdefghi", 10, out, sizeof(out), &out_len, &ends_with_slash),
    -2);
  PASS();
}

TEST t_normalize_idempotent(void) {
  char out1[128];
  char out2[128];
  size_t out1_len = 0;
  size_t out2_len = 0;
  int ends_with_slash1 = 0;
  int ends_with_slash2 = 0;

  const char *in = "/a//b/./c/../d";
  ASSERT_EQ(url_path_normalize_absolute(in,
                                        strlen(in),
                                        out1,
                                        sizeof(out1) - 1,
                                        &out1_len,
                                        &ends_with_slash1),
            0);
  out1[out1_len] = '\0';

  ASSERT_EQ(url_path_normalize_absolute(out1,
                                        out1_len,
                                        out2,
                                        sizeof(out2) - 1,
                                        &out2_len,
                                        &ends_with_slash2),
            0);
  out2[out2_len] = '\0';

  ASSERT_STR_EQ(out1, out2);
  ASSERT(normalized_looks_sane(out2));
  PASS();
}

TEST t_normalize_too_many_segments_rejected(void) {
  // url_path_normalize_absolute keeps a fixed segment stack; exceeding it should fail closed.
  char in[4096];
  size_t n = 0;
  in[n++] = '/';
  for (int i = 0; i < 300; ++i) {
    in[n++] = 'a';
    in[n++] = '/';
  }
  // make it not end with slash to avoid relying on ends_with_slash behavior
  if (n > 1) {
    n--; // drop last '/'
  }

  char out[4096];
  size_t out_len = 0;
  int ends_with_slash = 0;

  ASSERT_EQ(url_path_normalize_absolute(in, n, out, sizeof(out) - 1, &out_len, &ends_with_slash),
            -1);
  PASS();
}

TEST t_normalize_rejects_non_absolute(void) {
  char out[16];
  size_t out_len = 0;
  int ends_with_slash = 0;

  ASSERT_EQ(url_path_normalize_absolute("a/b", 3, out, sizeof(out), &out_len, &ends_with_slash),
            -1);
  PASS();
}

SUITE(s_url) {
  RUN_TEST(t_split_path_query_basic);
  RUN_TEST(t_split_path_query_empty_query);
  RUN_TEST(t_split_path_query_none);
  RUN_TEST(t_split_path_query_multiple_qmark);
  RUN_TEST(t_percent_decode_ok_and_plus_literal);
  RUN_TEST(t_percent_decode_rejects_encoded_slashes_and_nul);
  RUN_TEST(t_percent_decode_allows_encoded_slash_when_configured);
  RUN_TEST(t_percent_decode_hex_case_insensitive);
  RUN_TEST(t_percent_decode_invalid_sequences);
  RUN_TEST(t_percent_decode_overflow);
  RUN_TEST(t_normalize_absolute_basic);
  RUN_TEST(t_normalize_absolute_preserves_input_slash_flag);
  RUN_TEST(t_normalize_absolute_clamps_at_root);
  RUN_TEST(t_normalize_root_variants);
  RUN_TEST(t_normalize_overflow);
  RUN_TEST(t_normalize_idempotent);
  RUN_TEST(t_normalize_too_many_segments_rejected);
  RUN_TEST(t_normalize_rejects_non_absolute);
}

GREATEST_MAIN_DEFS();
int main(int argc, char **argv) {
  GREATEST_MAIN_BEGIN();
  RUN_SUITE(s_url);
  GREATEST_MAIN_END();
}
