// Unit tests for static_serve_assemble_extra_headers — verifies that each
// feature flag (CONDITIONAL, RANGE, COMPRESSION) produces headers
// independently without requiring any other flag.

#define _GNU_SOURCE
#include <fcntl.h>
#include <limits.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include <unistd.h>
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

static enum resp_kind g_last_begin_rk = RK_404;
static unsigned g_begin_headers_calls = 0;
static const char g_stub_headers[] = "HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n";

struct request_static_serve_plan request_build_static_serve_plan(
    const struct conn *c, size_t sz) {
  (void)c; (void)sz;
  struct request_static_serve_plan p = {0};
  p.mode = REQUEST_STATIC_SERVE_HEAD;
  return p;
}

int request_static_open_err_merge(int a, int b) { (void)a; return b; }

struct request_response_plan request_build_response_plan(enum resp_kind kind,
                                                         int keepalive,
                                                         int drain_after_headers,
                                                         int close_after_send) {
  struct request_response_plan plan;
  memset(&plan, 0, sizeof(plan));
  plan.kind = kind;
  plan.keepalive = keepalive;
  plan.drain_after_headers = drain_after_headers;
  plan.close_after_send = close_after_send;
  plan.status_line = "500 Internal Server Error";
  return plan;
}

enum tx_decision tx_begin_headers(struct tx_state_t *tx, enum resp_kind rk,
                                  const char *buf, size_t len, int ka,
                                  int dah, struct tx_next_io *out) {
  (void)tx; (void)rk; (void)buf; (void)len; (void)ka; (void)dah; (void)out;
  g_last_begin_rk = rk;
  g_begin_headers_calls++;
  return TX_NOOP;
}

int tx_build_headers(struct tx_state_t *tx, const char *s, const char *ct,
                     int emit_cl, size_t cl, const void *body, size_t bsl,
                     int ka, int dah, const char *eh, const char **buf,
                     size_t *len) {
  (void)tx;
  (void)s;
  (void)ct;
  (void)emit_cl;
  (void)cl;
  (void)body;
  (void)bsl;
  (void)ka;
  (void)dah;
  (void)eh;
  if (buf) {
    *buf = g_stub_headers;
  }
  if (len) {
    *len = sizeof(g_stub_headers) - 1;
  }
  return 0;
}

int tx_begin_sendfile(struct tx_state_t *tx, off_t off, size_t sz) {
  (void)tx; (void)off; (void)sz;
  return 0;
}

const char *http_header_find_value(const struct req_hdr_entry *hdrs,
                                   uint8_t hdr_count,
                                   enum http_header_id id,
                                   uint8_t *out_len) {
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
  size_t n = static_serve_assemble_extra_headers_ex(
    buf, sizeof(buf), &st, CFG_FEAT_STATIC, &enc,
    "text/html", NULL, 0, etag, sizeof(etag), &etag_len, NULL);
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
  size_t n = static_serve_assemble_extra_headers_ex(
    buf, sizeof(buf), &st, CFG_FEAT_STATIC | CFG_FEAT_CONDITIONAL, &enc,
    "text/html", NULL, 0, etag, sizeof(etag), &etag_len, NULL);
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
  size_t n = static_serve_assemble_extra_headers_ex(
    buf, sizeof(buf), &st, CFG_FEAT_STATIC | CFG_FEAT_RANGE, &enc,
    "text/html", NULL, 0, etag, sizeof(etag), &etag_len, NULL);
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
  size_t n = static_serve_assemble_extra_headers_ex(
    buf, sizeof(buf), &st, CFG_FEAT_STATIC | CFG_FEAT_COMPRESSION, &enc,
    "text/html", NULL, 0, etag, sizeof(etag), &etag_len, NULL);
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
  size_t n = static_serve_assemble_extra_headers_ex(
    buf, sizeof(buf), &st, CFG_FEAT_STATIC | CFG_FEAT_COMPRESSION, &enc,
    "text/html", NULL, 0, etag, sizeof(etag), &etag_len, NULL);
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
  size_t n = static_serve_assemble_extra_headers_ex(
    buf, sizeof(buf), &st, CFG_FEAT_STATIC | CFG_FEAT_COMPRESSION, &enc,
    "text/html", NULL, 0, etag, sizeof(etag), &etag_len, NULL);
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
  size_t n = static_serve_assemble_extra_headers_ex(
    buf, sizeof(buf), &st, CFG_FEAT_STATIC | CFG_FEAT_COMPRESSION, &enc,
    "image/jpeg", NULL, 0, etag, sizeof(etag), &etag_len, NULL);
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
  size_t n = static_serve_assemble_extra_headers_ex(
    buf, sizeof(buf), &st, features, &enc,
    "text/html", NULL, 0, etag, sizeof(etag), &etag_len, NULL);
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
  size_t n = static_serve_assemble_extra_headers_ex(
    buf, sizeof(buf), &st, features, &enc,
    "text/html", NULL, 0, etag, sizeof(etag), &etag_len, NULL);
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
  size_t n = static_serve_assemble_extra_headers_ex(
    buf, sizeof(buf), &st, CFG_FEAT_STATIC | CFG_FEAT_COMPRESSION, &enc,
    "text/html", NULL, 0, etag, sizeof(etag), &etag_len, NULL);
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
  (void)static_serve_assemble_extra_headers_ex(
    buf, sizeof(buf), &st, CFG_FEAT_STATIC | CFG_FEAT_COMPRESSION, &enc,
    "text/html", NULL, 0, etag, sizeof(etag), &etag_len, NULL);
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
  const char *hdrs[] = {"Cache-Control: public, max-age=3600\r\n"};
  size_t n = static_serve_assemble_extra_headers_ex(
    buf, sizeof(buf), &st, CFG_FEAT_STATIC, &enc,
    "text/html", hdrs, 1, etag, sizeof(etag), &etag_len, NULL);
  ASSERT(n > 0);
  ASSERT(strstr(buf, "Cache-Control: public, max-age=3600\r\n") != NULL);
  PASS();
}

TEST t_custom_header_multiple(void) {
  struct stat st = make_stat(1, 100, 1000, 0);
  char buf[512], etag[64];
  size_t etag_len = 0;
  unsigned enc = 0;
  const char *hdrs[] = {
    "Cache-Control: no-cache\r\n",
    "X-Content-Type-Options: nosniff\r\n"
  };
  size_t n = static_serve_assemble_extra_headers_ex(
    buf, sizeof(buf), &st, CFG_FEAT_STATIC, &enc,
    "text/html", hdrs, 2, etag, sizeof(etag), &etag_len, NULL);
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
  (void)static_serve_assemble_extra_headers_ex(
    buf, sizeof(buf), &st, CFG_FEAT_STATIC, &enc,
    "text/html", NULL, 0, etag, sizeof(etag), &etag_len, NULL);
  ASSERT(strstr(buf, "Cache-Control:") == NULL);
  PASS();
}

TEST t_custom_header_overflow_sets_flag(void) {
  struct stat st = make_stat(1, 100, 1000, 0);
  char buf[48], etag[64];
  size_t etag_len = 0;
  unsigned enc = 0;
  int hdr_overflow = 0;
  const char *hdrs[] = {
    "X-Long-One: 123456789012345678901234567890\r\n",
    "X-Long-Two: 123456789012345678901234567890\r\n"
  };

  (void)static_serve_assemble_extra_headers_ex(
    buf, sizeof(buf), &st, CFG_FEAT_STATIC, &enc,
    "text/html", hdrs, 2, etag, sizeof(etag), &etag_len, &hdr_overflow);

  ASSERT_EQ(hdr_overflow, 1);
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
  RUN_TEST(t_custom_header_overflow_sets_flag);
}

// ===========================================================================
// Typed policy + route precedence + header_set de-dup
// ===========================================================================

TEST t_policy_route_override_and_custom_dedup(void) {
  struct conn c;
  memset(&c, 0, sizeof(c));

  struct vhost_t vh;
  memset(&vh, 0, sizeof(vh));
  struct security_headers_policy vh_sec;
  const struct route_policy_rule *route_rules[2];
  memset(&vh_sec, 0, sizeof(vh_sec));
  memset(route_rules, 0, sizeof(route_rules));

  struct route_policy_rule r_api;
  memset(&r_api, 0, sizeof(r_api));
  snprintf(r_api.path_prefix, sizeof(r_api.path_prefix), "%s", "/api");
  r_api.path_prefix_len = (uint16_t)strlen(r_api.path_prefix);
  r_api.security_headers.enabled = 1;
  r_api.security_headers.header_count = 1;
  snprintf(r_api.security_headers.headers[0].name,
           sizeof(r_api.security_headers.headers[0].name),
           "%s",
           "X-Frame-Options");
  snprintf(r_api.security_headers.headers[0].value,
           sizeof(r_api.security_headers.headers[0].value),
           "%s",
           "SAMEORIGIN");

  struct route_policy_rule r_api_v2;
  memset(&r_api_v2, 0, sizeof(r_api_v2));
  snprintf(r_api_v2.path_prefix, sizeof(r_api_v2.path_prefix), "%s", "/api/v2");
  r_api_v2.path_prefix_len = (uint16_t)strlen(r_api_v2.path_prefix);
  r_api_v2.security_headers.enabled = 1;
  r_api_v2.security_headers.header_count = 1;
  snprintf(r_api_v2.security_headers.headers[0].name,
           sizeof(r_api_v2.security_headers.headers[0].name),
           "%s",
           "X-Frame-Options");
  snprintf(r_api_v2.security_headers.headers[0].value,
           sizeof(r_api_v2.security_headers.headers[0].value),
           "%s",
           "DENY");

  vh.route_rule_count = 2;
  vh.route_rule_cap = 2;
  route_rules[0] = &r_api_v2;
  route_rules[1] = &r_api;
  vh.route_rules = route_rules;
  vh.security_headers = &vh_sec;
  vh.security_headers->enabled = 1;
  vh.security_headers->header_count = 1;
  snprintf(vh.security_headers->headers[0].name,
           sizeof(vh.security_headers->headers[0].name),
           "%s",
           "X-Frame-Options");
  snprintf(vh.security_headers->headers[0].value,
           sizeof(vh.security_headers->headers[0].value),
           "%s",
           "ALLOW-FROM https://old.example");

  static const char h0[] = "X-Frame-Options: SAMEORIGIN\r\n";
  static const char h1[] = "X-Custom: one\r\n";
  vh.custom_headers[0] = (char *)h0;
  vh.custom_headers[1] = (char *)h1;
  vh.custom_headers_count = 2;

  c.vhost = &vh;
  c.h1.path_norm = "/api/v2/users";
  c.h1.path_norm_len = (uint16_t)strlen(c.h1.path_norm);

  struct policy_shared_header_ctx ctx;
  policy_shared_collect_headers(&c, &vh, &ctx);

  int saw_deny = 0;
  int saw_sameorigin = 0;
  int saw_custom = 0;
  for (unsigned i = 0; i < ctx.count; ++i) {
    if (strstr(ctx.lines[i], "X-Frame-Options: DENY") != NULL) {
      saw_deny = 1;
    }
    if (strstr(ctx.lines[i], "X-Frame-Options: SAMEORIGIN") != NULL) {
      saw_sameorigin = 1;
    }
    if (strstr(ctx.lines[i], "X-Custom: one") != NULL) {
      saw_custom = 1;
    }
  }

  ASSERT(saw_deny);
  ASSERT(!saw_sameorigin);
  ASSERT(saw_custom);
  PASS();
}

TEST t_policy_route_cors_override_and_vary_origin(void) {
  struct conn c;
  memset(&c, 0, sizeof(c));

  struct vhost_t vh;
  memset(&vh, 0, sizeof(vh));
  struct cors_policy vh_cors;
  const struct route_policy_rule *route_rules[1];
  memset(&vh_cors, 0, sizeof(vh_cors));
  memset(route_rules, 0, sizeof(route_rules));

  vh.cors = &vh_cors;
  vh.cors->enabled = 1;
  vh.cors->enabled_set = 1;
  snprintf(vh.cors->allow_origin, sizeof(vh.cors->allow_origin), "%s", "*");
  vh.cors->allow_origin_set = 1;

  struct route_policy_rule rr;
  memset(&rr, 0, sizeof(rr));
  snprintf(rr.path_prefix, sizeof(rr.path_prefix), "%s", "/api");
  rr.path_prefix_len = (uint16_t)strlen(rr.path_prefix);
  rr.cors.enabled = 1;
  rr.cors.enabled_set = 1;
  snprintf(rr.cors.allow_origin, sizeof(rr.cors.allow_origin), "%s", "https://app.example.com");
  rr.cors.allow_origin_set = 1;

  vh.route_rule_count = 1;
  vh.route_rule_cap = 1;
  route_rules[0] = &rr;
  vh.route_rules = route_rules;

  static const char dup_origin[] = "Access-Control-Allow-Origin: *\r\n";
  vh.custom_headers[0] = (char *)dup_origin;
  vh.custom_headers_count = 1;

  c.vhost = &vh;
  c.h1.path_norm = "/api/items";
  c.h1.path_norm_len = (uint16_t)strlen(c.h1.path_norm);

  struct policy_shared_header_ctx ctx;
  policy_shared_collect_headers(&c, &vh, &ctx);

  int saw_route_origin = 0;
  int saw_dup_origin = 0;
  int saw_vary_origin = 0;
  for (unsigned i = 0; i < ctx.count; ++i) {
    if (strstr(ctx.lines[i], "Access-Control-Allow-Origin: https://app.example.com") != NULL) {
      saw_route_origin = 1;
    }
    if (strstr(ctx.lines[i], "Access-Control-Allow-Origin: *") != NULL) {
      saw_dup_origin = 1;
    }
    if (strstr(ctx.lines[i], "Vary: Origin") != NULL) {
      saw_vary_origin = 1;
    }
  }

  ASSERT(saw_route_origin);
  ASSERT(!saw_dup_origin);
  ASSERT(saw_vary_origin);
  PASS();
}

TEST t_policy_collects_documented_max_without_overflow(void) {
  struct conn c;
  struct vhost_t vh;
  struct security_headers_policy vh_sec;
  struct cors_policy vh_cors;
  struct route_policy_rule rr;
  const struct route_policy_rule *route_rules[1];
  char custom_headers[16][64];
  char path[] = "/api/items";

  memset(&c, 0, sizeof(c));
  memset(&vh, 0, sizeof(vh));
  memset(&vh_sec, 0, sizeof(vh_sec));
  memset(&vh_cors, 0, sizeof(vh_cors));
  memset(&rr, 0, sizeof(rr));
  memset(route_rules, 0, sizeof(route_rules));
  memset(custom_headers, 0, sizeof(custom_headers));

  c.vhost = &vh;
  c.h1.path_norm = path;
  c.h1.path_norm_len = (uint16_t)strlen(path);

  vh.security_headers = &vh_sec;
  vh.security_headers->enabled = 1;
  for (unsigned i = 0; i < 16u; ++i) {
    snprintf(vh.security_headers->headers[i].name,
             sizeof(vh.security_headers->headers[i].name),
             "X-Vhost-%02u",
             i);
    snprintf(vh.security_headers->headers[i].value,
             sizeof(vh.security_headers->headers[i].value),
             "vh-%02u",
             i);
  }
  vh.security_headers->header_count = 16u;

  vh.cors = &vh_cors;
  vh.cors->enabled = 1;
  snprintf(vh.cors->allow_origin, sizeof(vh.cors->allow_origin), "%s", "https://app.example");
  snprintf(vh.cors->allow_methods, sizeof(vh.cors->allow_methods), "%s", "GET,POST,OPTIONS");
  snprintf(vh.cors->allow_headers, sizeof(vh.cors->allow_headers), "%s", "Content-Type");
  vh.cors->allow_credentials = 1;
  vh.cors->max_age_seconds = 3600u;
  vh.cors->max_age_seconds_set = 1;

  snprintf(rr.path_prefix, sizeof(rr.path_prefix), "%s", "/api");
  rr.path_prefix_len = (uint16_t)strlen(rr.path_prefix);
  for (unsigned i = 0; i < 16u; ++i) {
    snprintf(rr.security_headers.headers[i].name,
             sizeof(rr.security_headers.headers[i].name),
             "X-Route-%02u",
             i);
    snprintf(rr.security_headers.headers[i].value,
             sizeof(rr.security_headers.headers[i].value),
             "route-%02u",
             i);
  }
  rr.security_headers.header_count = 16u;
  route_rules[0] = &rr;
  vh.route_rules = route_rules;
  vh.route_rule_count = 1;
  vh.route_rule_cap = 1;

  for (unsigned i = 0; i < 16u; ++i) {
    snprintf(custom_headers[i], sizeof(custom_headers[i]), "X-Custom-%02u: custom\r\n", i);
    vh.custom_headers[i] = custom_headers[i];
  }
  vh.custom_headers_count = 16u;

  struct policy_shared_header_ctx ctx;
  policy_shared_collect_headers(&c, &vh, &ctx);

  ASSERT_EQ(ctx.overflow, 0);
  ASSERT_EQ(ctx.count, 54u);
  ASSERT(policy_shared_header_find(&ctx, "X-Vhost-15") >= 0);
  ASSERT(policy_shared_header_find(&ctx, "X-Route-15") >= 0);
  ASSERT(policy_shared_header_find(&ctx, "X-Custom-15") >= 0);
  ASSERT(policy_shared_header_find(&ctx, "Access-Control-Max-Age") >= 0);
  PASS();
}

SUITE(s_policy) {
  RUN_TEST(t_policy_route_override_and_custom_dedup);
  RUN_TEST(t_policy_route_cors_override_and_vary_origin);
  RUN_TEST(t_policy_collects_documented_max_without_overflow);
}

// ===========================================================================
// build_docroot_relpath: index file resolution
// ===========================================================================

TEST t_relpath_root_default_index(void) {
  char out[PATH_MAX];
  ASSERT_EQ(static_serve_build_docroot_relpath(out, "/", 1, 0, NULL), 0);
  ASSERT_STR_EQ(out, "index.html");
  PASS();
}

TEST t_relpath_root_custom_index(void) {
  char out[PATH_MAX];
  ASSERT_EQ(static_serve_build_docroot_relpath(out, "/", 1, 0, "main.htm"), 0);
  ASSERT_STR_EQ(out, "main.htm");
  PASS();
}

TEST t_relpath_subdir_slash_default(void) {
  char out[PATH_MAX];
  ASSERT_EQ(static_serve_build_docroot_relpath(out, "/sub", 4, 1, NULL), 0);
  ASSERT_STR_EQ(out, "sub/index.html");
  PASS();
}

TEST t_relpath_subdir_slash_custom(void) {
  char out[PATH_MAX];
  ASSERT_EQ(static_serve_build_docroot_relpath(out, "/sub", 4, 1, "home.xhtml"), 0);
  ASSERT_STR_EQ(out, "sub/home.xhtml");
  PASS();
}

TEST t_relpath_no_slash_returns_bare(void) {
  char out[PATH_MAX];
  ASSERT_EQ(static_serve_build_docroot_relpath(out, "/sub", 4, 0, NULL), 0);
  ASSERT_STR_EQ(out, "sub");
  PASS();
}

TEST t_relpath_empty_index_uses_default(void) {
  char out[PATH_MAX];
  ASSERT_EQ(static_serve_build_docroot_relpath(out, "/", 1, 0, ""), 0);
  ASSERT_STR_EQ(out, "index.html");
  PASS();
}

SUITE(s_relpath) {
  RUN_TEST(t_relpath_root_default_index);
  RUN_TEST(t_relpath_root_custom_index);
  RUN_TEST(t_relpath_subdir_slash_default);
  RUN_TEST(t_relpath_subdir_slash_custom);
  RUN_TEST(t_relpath_no_slash_returns_bare);
  RUN_TEST(t_relpath_empty_index_uses_default);
}

GREATEST_MAIN_DEFS();
int main(int argc, char **argv) {
  GREATEST_MAIN_BEGIN();
  RUN_SUITE(s_flag_independence);
  RUN_SUITE(s_combined);
  RUN_SUITE(s_custom_headers);
  RUN_SUITE(s_policy);
  RUN_SUITE(s_relpath);
  GREATEST_MAIN_END();
}
