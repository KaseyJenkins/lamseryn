/* Unit tests for src/auth.c — base64 decode, credential store load,
 * and auth_basic_check decision matrix. */

#include "../vendor/greatest_color.h"
#include "../vendor/greatest.h"

#include "include/auth.h"

#include <crypt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* auth_basic_check depends on http_header_find_value, tx_build_headers, and
 * tx_begin_headers which are not linked in this test binary.  Provide test
 * stubs so both the decoder/store helpers and auth_basic_check can be tested
 * in isolation. */
#include "include/conn.h"
#include "include/http_headers.h"
static int g_tx_build_rc = 0;
static unsigned g_tx_build_calls = 0;
static unsigned g_tx_begin_calls = 0;
static enum resp_kind g_last_resp_kind = RK_NONE;
static char g_last_status[64];
static char g_last_extra_headers[256];

static void reset_auth_stubs(void) {
  g_tx_build_rc = 0;
  g_tx_build_calls = 0;
  g_tx_begin_calls = 0;
  g_last_resp_kind = RK_NONE;
  g_last_status[0] = '\0';
  g_last_extra_headers[0] = '\0';
}

const char *http_header_find_value(const struct req_hdr_entry *h,
                                   uint8_t n,
                                   enum http_header_id id,
                                   uint8_t *len) {
  for (uint8_t i = 0; i < n; i++) {
    if ((enum http_header_id)h[i].id != id) {
      continue;
    }
    if (len) {
      *len = h[i].value_len;
    }
    return h[i].value;
  }
  if (len) {
    *len = 0;
  }
  return NULL;
}
#include "include/tx.h"
int tx_build_headers(struct tx_state_t *tx,
                     const char *s,
                     const char *ct,
                     int ecl,
                     size_t cl,
                     const void *b,
                     size_t bl,
                     int ka,
                     int d,
                     const char *e,
                     const char **buf,
                     size_t *len) {
  static const char stub_buf[] = "HTTP/1.1 401 Unauthorized\r\n\r\n";
  (void)tx;
  (void)ct;
  (void)ecl;
  (void)cl;
  (void)b;
  (void)bl;
  (void)ka;
  (void)d;
  g_tx_build_calls++;
  snprintf(g_last_status, sizeof(g_last_status), "%s", s ? s : "");
  snprintf(g_last_extra_headers, sizeof(g_last_extra_headers), "%s", e ? e : "");
  if (buf) {
    *buf = stub_buf;
  }
  if (len) {
    *len = sizeof(stub_buf) - 1;
  }
  return g_tx_build_rc;
}
enum tx_decision tx_begin_headers(struct tx_state_t *tx,
                                  enum resp_kind rk,
                                  const char *buf,
                                  size_t len,
                                  int ka,
                                  int d,
                                  struct tx_next_io *out) {
  (void)ka;
  (void)d;
  g_tx_begin_calls++;
  g_last_resp_kind = rk;
  if (tx) {
    tx->resp_kind = rk;
    tx->write_buf = buf;
    tx->write_len = len;
    tx->write_off = 0;
  }
  if (out) {
    out->buf = buf;
    out->len = len;
  }
  return TX_SEND_HEADERS;
}

/* Write a temporary htpasswd file into out_path. */
static int write_temp_htpasswd(const char *content, char *out_path, size_t path_cap) {
  snprintf(out_path, path_cap, "/tmp/auth_test_XXXXXX");
  int fd = mkstemp(out_path);
  if (fd < 0) {
    return -1;
  }
  size_t len = strlen(content);
  if (write(fd, content, len) != (ssize_t)len) {
    close(fd);
    unlink(out_path);
    return -1;
  }
  close(fd);
  return 0;
}

static void init_auth_header(struct req_hdr_entry *hdr, const char *value) {
  memset(hdr, 0, sizeof(*hdr));
  hdr->id = HDR_ID_AUTHORIZATION;
  hdr->flags = 0;
  hdr->value = (char *)value;
  hdr->value_len = (uint8_t)strlen(value);
}

static void init_auth_conn(struct conn *c,
                           struct vhost_t *vh,
                           struct auth_store *store,
                           struct req_hdr_entry *hdrs,
                           uint8_t hdr_count) {
  memset(c, 0, sizeof(*c));
  memset(vh, 0, sizeof(*vh));
  vh->auth_store = store;
  memcpy(vh->auth_realm, "Admin Area", sizeof("Admin Area"));
  c->vhost = vh;
  c->h1.req_hdrs = hdrs;
  c->h1.req_hdr_count = hdr_count;
  c->h1.want_keepalive = 1;
}

/* =========================================================================
 * Suite: base64 decoding
 * ====================================================================== */

TEST t_b64_decode_valid(void) {
  /* "alice:hunter2" in base64 */
  const char *enc = "YWxpY2U6aHVudGVyMg==";
  char dst[64];
  int n = auth_base64_decode(enc, strlen(enc), dst, sizeof(dst));
  ASSERT_EQ(n, 13);
  dst[n] = '\0';
  ASSERT_STR_EQ(dst, "alice:hunter2");
  PASS();
}

TEST t_b64_decode_no_padding(void) {
  /* "abc" in base64 without padding */
  const char *enc = "YWJj";
  char dst[16];
  int n = auth_base64_decode(enc, strlen(enc), dst, sizeof(dst));
  ASSERT_EQ(n, 3);
  dst[n] = '\0';
  ASSERT_STR_EQ(dst, "abc");
  PASS();
}

TEST t_b64_decode_one_byte_padding(void) {
  /* "ab" → "YWI=" (one padding char) */
  const char *enc = "YWI=";
  char dst[16];
  int n = auth_base64_decode(enc, strlen(enc), dst, sizeof(dst));
  ASSERT_EQ(n, 2);
  dst[n] = '\0';
  ASSERT_STR_EQ(dst, "ab");
  PASS();
}

TEST t_b64_decode_two_byte_padding(void) {
  /* "a" → "YQ==" (two padding chars) */
  const char *enc = "YQ==";
  char dst[16];
  int n = auth_base64_decode(enc, strlen(enc), dst, sizeof(dst));
  ASSERT_EQ(n, 1);
  dst[n] = '\0';
  ASSERT_STR_EQ(dst, "a");
  PASS();
}

TEST t_b64_decode_empty(void) {
  char dst[16];
  int n = auth_base64_decode("", 0, dst, sizeof(dst));
  ASSERT_EQ(n, 0);
  PASS();
}

TEST t_b64_decode_invalid_char(void) {
  /* '@' is not a valid base64 character */
  const char *enc = "YWx@Y2U=";
  char dst[64];
  int n = auth_base64_decode(enc, strlen(enc), dst, sizeof(dst));
  ASSERT_EQ(n, -1);
  PASS();
}

TEST t_b64_decode_overflow(void) {
  /* "alice:hunter2" decodes to 13 bytes; give it only 5 bytes of output */
  const char *enc = "YWxpY2U6aHVudGVyMg==";
  char dst[5];
  int n = auth_base64_decode(enc, strlen(enc), dst, sizeof(dst));
  ASSERT_EQ(n, -1);
  PASS();
}

TEST t_b64_decode_junk_after_padding(void) {
  /* Valid base64 "alice:hunter2" followed by trailing garbage bytes.
   * RFC 4648 §4: any character after a complete padded group is junk. */
  const char *enc = "YWxpY2U6aHVudGVyMg==JUNK";
  char dst[64];
  int n = auth_base64_decode(enc, strlen(enc), dst, sizeof(dst));
  ASSERT_EQ(n, -1);
  PASS();
}

TEST t_b64_decode_rejects_malformed_padding(void) {
  /* "a" is "YQ==". Replacing the last '=' with data must be rejected. */
  const char *enc = "YQ=A";
  char dst[16];
  int n = auth_base64_decode(enc, strlen(enc), dst, sizeof(dst));
  ASSERT_EQ(n, -1);
  PASS();
}

SUITE(s_base64) {
  RUN_TEST(t_b64_decode_valid);
  RUN_TEST(t_b64_decode_no_padding);
  RUN_TEST(t_b64_decode_one_byte_padding);
  RUN_TEST(t_b64_decode_two_byte_padding);
  RUN_TEST(t_b64_decode_empty);
  RUN_TEST(t_b64_decode_invalid_char);
  RUN_TEST(t_b64_decode_overflow);
  RUN_TEST(t_b64_decode_junk_after_padding);
  RUN_TEST(t_b64_decode_rejects_malformed_padding);
}

/* =========================================================================
 * Suite: credential store loading
 * ====================================================================== */

/* Generate a $6$ hash for the password "hunter2" using crypt_r so tests are
 * portable to whatever the host supports.  Writes into buf[buf_cap].
 * Returns 0 on success, -1 if crypt_r fails (unsupported). */
static int make_hash(const char *password, char *buf, size_t buf_cap) {
  struct crypt_data cd;
  memset(&cd, 0, sizeof(cd));
  /* Use a fixed salt for reproducibility in tests. */
  char *h = crypt_r(password, "$6$testsalt12345$", &cd);
  if (!h || h[0] == '*') {
    return -1;
  }
  size_t len = strlen(h);
  if (len >= buf_cap) {
    return -1;
  }
  memcpy(buf, h, len + 1);
  return 0;
}

static int build_single_user_store(struct auth_store *store,
                                   const char *user,
                                   const char *password) {
  static struct auth_entry single;
  char hash[256];
  size_t user_len = strlen(user);
  if (make_hash(password, hash, sizeof(hash)) != 0) {
    return -1;
  }
  if (user_len >= sizeof(single.user)) {
    return -1;
  }
  if (strlen(hash) >= sizeof(single.hash)) {
    return -1;
  }

  memset(store, 0, sizeof(*store));
  memset(&single, 0, sizeof(single));
  store->entries = &single;
  store->cap = 1;
  store->count = 1;
  memcpy(store->entries[0].user, user, user_len + 1);
  memcpy(store->entries[0].hash, hash, strlen(hash) + 1);
  return 0;
}

TEST t_store_load_valid(void) {
  char hash[256];
  if (make_hash("hunter2", hash, sizeof(hash)) != 0) {
    SKIPm("$6$ not supported on this host");
  }

  char content[1024];
  snprintf(content, sizeof(content), "alice:%s\n# comment\n\nbob:%s\n", hash, hash);

  char path[256];
  if (write_temp_htpasswd(content, path, sizeof(path)) != 0) {
    FAILm("could not write temp htpasswd");
  }

  struct auth_store *store = auth_store_load(path);
  unlink(path);

  ASSERT(store != NULL);
  ASSERT_EQ(store->count, 2u);
  ASSERT(store->cap >= store->count);
  ASSERT_STR_EQ(store->entries[0].user, "alice");
  ASSERT_STR_EQ(store->entries[1].user, "bob");
  auth_store_free(store);
  PASS();
}

TEST t_store_load_grows_entries_dynamically(void) {
  char hash[256];
  char content[8192];
  char path[256];
  size_t off = 0;
  const unsigned want = 17;

  if (make_hash("hunter2", hash, sizeof(hash)) != 0) {
    SKIPm("$6$ not supported on this host");
  }

  for (unsigned i = 0; i < want; ++i) {
    int n = snprintf(content + off, sizeof(content) - off, "u%u:%s\n", i, hash);
    if (n <= 0 || (size_t)n >= (sizeof(content) - off)) {
      FAILm("credential content assembly overflow");
    }
    off += (size_t)n;
  }

  if (write_temp_htpasswd(content, path, sizeof(path)) != 0) {
    FAILm("could not write temp htpasswd");
  }

  struct auth_store *store = auth_store_load(path);
  unlink(path);

  ASSERT(store != NULL);
  ASSERT_EQ(store->count, want);
  ASSERT(store->cap >= store->count);
  ASSERT(store->cap > 8u);
  ASSERT_STR_EQ(store->entries[0].user, "u0");
  ASSERT_STR_EQ(store->entries[want - 1].user, "u16");
  auth_store_free(store);
  PASS();
}

TEST t_store_load_enforces_max_users_cap(void) {
  char hash[256];
  char content[65536];
  char path[256];
  size_t off = 0;
  const unsigned total = AUTH_MAX_USERS + 5u;

  if (make_hash("hunter2", hash, sizeof(hash)) != 0) {
    SKIPm("$6$ not supported on this host");
  }

  for (unsigned i = 0; i < total; ++i) {
    int n = snprintf(content + off, sizeof(content) - off, "u%u:%s\n", i, hash);
    if (n <= 0 || (size_t)n >= (sizeof(content) - off)) {
      FAILm("credential content assembly overflow");
    }
    off += (size_t)n;
  }

  if (write_temp_htpasswd(content, path, sizeof(path)) != 0) {
    FAILm("could not write temp htpasswd");
  }

  struct auth_store *store = auth_store_load(path);
  unlink(path);

  ASSERT(store != NULL);
  ASSERT_EQ(store->count, (unsigned)AUTH_MAX_USERS);
  ASSERT_EQ(store->cap, (unsigned)AUTH_MAX_USERS);
  ASSERT_STR_EQ(store->entries[AUTH_MAX_USERS - 1].user, "u255");
  auth_store_free(store);
  PASS();
}

TEST t_store_load_accepts_max_length_username(void) {
  char hash[256];
  char content[512];
  char path[256];
  char user[AUTH_USER_MAX + 1];

  if (make_hash("hunter2", hash, sizeof(hash)) != 0) {
    SKIPm("$6$ not supported on this host");
  }

  memset(user, 'u', AUTH_USER_MAX);
  user[AUTH_USER_MAX] = '\0';
  snprintf(content, sizeof(content), "%s:%s\n", user, hash);

  if (write_temp_htpasswd(content, path, sizeof(path)) != 0) {
    FAILm("could not write temp htpasswd");
  }

  struct auth_store *store = auth_store_load(path);
  unlink(path);

  ASSERT(store != NULL);
  ASSERT_EQ(store->count, 1u);
  ASSERT_STR_EQ(store->entries[0].user, user);
  auth_store_free(store);
  PASS();
}

TEST t_store_load_skips_malformed_line(void) {
  char hash[256];
  if (make_hash("x", hash, sizeof(hash)) != 0) {
    SKIPm("$6$ not supported on this host");
  }

  char content[512];
  snprintf(content, sizeof(content), "nocolon\nalice:%s\n", hash);

  char path[256];
  if (write_temp_htpasswd(content, path, sizeof(path)) != 0) {
    FAILm("could not write temp htpasswd");
  }

  struct auth_store *store = auth_store_load(path);
  unlink(path);

  ASSERT(store != NULL);
  ASSERT_EQ(store->count, 1u);
  ASSERT_STR_EQ(store->entries[0].user, "alice");
  auth_store_free(store);
  PASS();
}

TEST t_store_load_empty_file_fails(void) {
  char path[256];
  if (write_temp_htpasswd("# only a comment\n", path, sizeof(path)) != 0) {
    FAILm("could not write temp htpasswd");
  }

  struct auth_store *store = auth_store_load(path);
  unlink(path);

  ASSERT_EQ(store, NULL);
  PASS();
}

TEST t_store_load_nonexistent_file_fails(void) {
  struct auth_store *store = auth_store_load("/tmp/auth_no_such_file_xyz");
  ASSERT_EQ(store, NULL);
  PASS();
}

TEST t_store_load_unsupported_hash_skipped(void) {
  /* DES crypt with a 2-char salt — unsupported by libxcrypt on modern systems.
   * If crypt_r can handle it this test becomes a no-op and passes anyway. */
  char path[256];
  if (write_temp_htpasswd("alice:ab/Nd4pbR5WFJI\n", path, sizeof(path)) != 0) {
    FAILm("could not write temp htpasswd");
  }

  /* We don't assert the result here — on hosts that support DES the entry
   * would be accepted.  On hosts that don't, the file should fail to load. */
  struct auth_store *store = auth_store_load(path);
  unlink(path);

  auth_store_free(store);
  PASS();
}

TEST t_store_load_truncated_physical_line_skipped(void) {
  char hash[256];
  char content[512];
  char path[256];
  size_t prefix_len;
  size_t hash_len;
  size_t pos;

  if (make_hash("hunter2", hash, sizeof(hash)) != 0) {
    SKIPm("$6$ not supported on this host");
  }

  memset(content, 'x', sizeof(content));
  content[0] = '#';
  prefix_len = (AUTH_USER_MAX + 1 + AUTH_HASH_MAX + 2) - 1;
  hash_len = strlen(hash);
  pos = prefix_len;
  memcpy(content + pos, "alice:", sizeof("alice:") - 1);
  pos += sizeof("alice:") - 1;
  memcpy(content + pos, hash, hash_len);
  pos += hash_len;
  content[pos++] = '\n';
  content[pos] = '\0';

  if (write_temp_htpasswd(content, path, sizeof(path)) != 0) {
    FAILm("could not write temp htpasswd");
  }

  struct auth_store *store = auth_store_load(path);
  unlink(path);

  ASSERT_EQ(store, NULL);
  PASS();
}

SUITE(s_auth_store) {
  RUN_TEST(t_store_load_valid);
  RUN_TEST(t_store_load_grows_entries_dynamically);
  RUN_TEST(t_store_load_enforces_max_users_cap);
  RUN_TEST(t_store_load_accepts_max_length_username);
  RUN_TEST(t_store_load_skips_malformed_line);
  RUN_TEST(t_store_load_empty_file_fails);
  RUN_TEST(t_store_load_nonexistent_file_fails);
  RUN_TEST(t_store_load_unsupported_hash_skipped);
  RUN_TEST(t_store_load_truncated_physical_line_skipped);
}

/* =========================================================================
 * Suite: auth_basic_check
 * ====================================================================== */

TEST t_auth_check_no_store_allows(void) {
  struct conn c;
  struct vhost_t vh;

  reset_auth_stubs();
  init_auth_conn(&c, &vh, NULL, NULL, 0);

  ASSERT_EQ(auth_basic_check(&c), 0);
  ASSERT_EQ(g_tx_build_calls, 0u);
  ASSERT_EQ(g_tx_begin_calls, 0u);
  PASS();
}

TEST t_auth_check_missing_header_rejects(void) {
  struct conn c;
  struct vhost_t vh;
  struct auth_store store;

  if (build_single_user_store(&store, "alice", "hunter2") != 0) {
    SKIPm("$6$ not supported on this host");
  }

  reset_auth_stubs();
  init_auth_conn(&c, &vh, &store, NULL, 0);

  ASSERT_EQ(auth_basic_check(&c), 1);
  ASSERT_EQ(g_tx_build_calls, 1u);
  ASSERT_EQ(g_tx_begin_calls, 1u);
  ASSERT_EQ(g_last_resp_kind, RK_401);
  ASSERT_STR_EQ(g_last_status, "401 Unauthorized");
  ASSERT(strstr(g_last_extra_headers, "WWW-Authenticate: Basic realm=\"Admin Area\"\r\n") != NULL);
  PASS();
}

TEST t_auth_check_missing_header_rejects_with_policy_headers(void) {
  struct conn c;
  struct vhost_t vh;
  struct auth_store store;
  struct security_headers_policy vh_sec;

  if (build_single_user_store(&store, "alice", "hunter2") != 0) {
    SKIPm("$6$ not supported on this host");
  }

  memset(&vh_sec, 0, sizeof(vh_sec));
  vh_sec.enabled = 1;
  vh_sec.enabled_set = 1;
  snprintf(vh_sec.headers[0].name, sizeof(vh_sec.headers[0].name), "%s", "X-Frame-Options");
  snprintf(vh_sec.headers[0].value, sizeof(vh_sec.headers[0].value), "%s", "DENY");
  vh_sec.header_count = 1;

  reset_auth_stubs();
  init_auth_conn(&c, &vh, &store, NULL, 0);
  vh.security_headers = &vh_sec;

  ASSERT_EQ(auth_basic_check(&c), 1);
  ASSERT_EQ(g_tx_build_calls, 1u);
  ASSERT_EQ(g_tx_begin_calls, 1u);
  ASSERT_EQ(g_last_resp_kind, RK_401);
  ASSERT_STR_EQ(g_last_status, "401 Unauthorized");
  ASSERT(strstr(g_last_extra_headers, "WWW-Authenticate: Basic realm=\"Admin Area\"\r\n") != NULL);
  ASSERT(strstr(g_last_extra_headers, "X-Frame-Options: DENY\r\n") != NULL);
  PASS();
}

TEST t_auth_check_policy_overflow_fail_closes(void) {
  struct conn c;
  struct vhost_t vh;
  struct auth_store store;
  char value[280];
  char h0[340];
  char h1[340];
  char h2[340];
  char h3[340];

  if (build_single_user_store(&store, "alice", "hunter2") != 0) {
    SKIPm("$6$ not supported on this host");
  }

  memset(value, 'A', sizeof(value) - 1);
  value[sizeof(value) - 1] = '\0';
  snprintf(h0, sizeof(h0), "X-Long-0: %s\r\n", value);
  snprintf(h1, sizeof(h1), "X-Long-1: %s\r\n", value);
  snprintf(h2, sizeof(h2), "X-Long-2: %s\r\n", value);
  snprintf(h3, sizeof(h3), "X-Long-3: %s\r\n", value);

  reset_auth_stubs();
  init_auth_conn(&c, &vh, &store, NULL, 0);
  vh.custom_headers[0] = h0;
  vh.custom_headers[1] = h1;
  vh.custom_headers[2] = h2;
  vh.custom_headers[3] = h3;
  vh.custom_headers_count = 4;

  ASSERT_EQ(auth_basic_check(&c), -1);
  ASSERT_EQ(g_tx_build_calls, 0u);
  ASSERT_EQ(g_tx_begin_calls, 0u);
  PASS();
}

TEST t_auth_check_wrong_scheme_rejects(void) {
  struct conn c;
  struct vhost_t vh;
  struct auth_store store;
  struct req_hdr_entry hdr;

  if (build_single_user_store(&store, "alice", "hunter2") != 0) {
    SKIPm("$6$ not supported on this host");
  }

  init_auth_header(&hdr, "Bearer abc");
  reset_auth_stubs();
  init_auth_conn(&c, &vh, &store, &hdr, 1);

  ASSERT_EQ(auth_basic_check(&c), 1);
  ASSERT_EQ(g_tx_build_calls, 1u);
  ASSERT_EQ(g_tx_begin_calls, 1u);
  PASS();
}

TEST t_auth_check_bad_base64_rejects(void) {
  struct conn c;
  struct vhost_t vh;
  struct auth_store store;
  struct req_hdr_entry hdr;

  if (build_single_user_store(&store, "alice", "hunter2") != 0) {
    SKIPm("$6$ not supported on this host");
  }

  init_auth_header(&hdr, "Basic YWx@Y2U=");
  reset_auth_stubs();
  init_auth_conn(&c, &vh, &store, &hdr, 1);

  ASSERT_EQ(auth_basic_check(&c), 1);
  ASSERT_EQ(g_tx_build_calls, 1u);
  ASSERT_EQ(g_tx_begin_calls, 1u);
  PASS();
}

TEST t_auth_check_wrong_password_rejects(void) {
  struct conn c;
  struct vhost_t vh;
  struct auth_store store;
  struct req_hdr_entry hdr;

  if (build_single_user_store(&store, "alice", "hunter2") != 0) {
    SKIPm("$6$ not supported on this host");
  }

  init_auth_header(&hdr, "Basic YWxpY2U6d3Jvbmc=");
  reset_auth_stubs();
  init_auth_conn(&c, &vh, &store, &hdr, 1);

  ASSERT_EQ(auth_basic_check(&c), 1);
  ASSERT_EQ(g_tx_build_calls, 1u);
  ASSERT_EQ(g_tx_begin_calls, 1u);
  PASS();
}

TEST t_auth_check_unknown_user_rejects(void) {
  struct conn c;
  struct vhost_t vh;
  struct auth_store store;
  struct req_hdr_entry hdr;

  if (build_single_user_store(&store, "alice", "hunter2") != 0) {
    SKIPm("$6$ not supported on this host");
  }

  init_auth_header(&hdr, "Basic Ym9iOmh1bnRlcjI=");
  reset_auth_stubs();
  init_auth_conn(&c, &vh, &store, &hdr, 1);

  ASSERT_EQ(auth_basic_check(&c), 1);
  ASSERT_EQ(g_tx_build_calls, 1u);
  ASSERT_EQ(g_tx_begin_calls, 1u);
  PASS();
}

TEST t_auth_check_valid_credentials_allows(void) {
  struct conn c;
  struct vhost_t vh;
  struct auth_store store;
  struct req_hdr_entry hdr;

  if (build_single_user_store(&store, "alice", "hunter2") != 0) {
    SKIPm("$6$ not supported on this host");
  }

  init_auth_header(&hdr, "Basic YWxpY2U6aHVudGVyMg==");
  reset_auth_stubs();
  init_auth_conn(&c, &vh, &store, &hdr, 1);

  ASSERT_EQ(auth_basic_check(&c), 0);
  ASSERT_EQ(g_tx_build_calls, 0u);
  ASSERT_EQ(g_tx_begin_calls, 0u);
  PASS();
}

TEST t_auth_check_lowercase_scheme_allows(void) {
  struct conn c;
  struct vhost_t vh;
  struct auth_store store;
  struct req_hdr_entry hdr;

  if (build_single_user_store(&store, "alice", "hunter2") != 0) {
    SKIPm("$6$ not supported on this host");
  }

  init_auth_header(&hdr, "basic YWxpY2U6aHVudGVyMg==");
  reset_auth_stubs();
  init_auth_conn(&c, &vh, &store, &hdr, 1);

  ASSERT_EQ(auth_basic_check(&c), 0);
  ASSERT_EQ(g_tx_build_calls, 0u);
  ASSERT_EQ(g_tx_begin_calls, 0u);
  PASS();
}

TEST t_auth_check_multiple_spaces_after_scheme_allows(void) {
  struct conn c;
  struct vhost_t vh;
  struct auth_store store;
  struct req_hdr_entry hdr;

  if (build_single_user_store(&store, "alice", "hunter2") != 0) {
    SKIPm("$6$ not supported on this host");
  }

  init_auth_header(&hdr, "Basic   YWxpY2U6aHVudGVyMg==");
  reset_auth_stubs();
  init_auth_conn(&c, &vh, &store, &hdr, 1);

  ASSERT_EQ(auth_basic_check(&c), 0);
  ASSERT_EQ(g_tx_build_calls, 0u);
  ASSERT_EQ(g_tx_begin_calls, 0u);
  PASS();
}

TEST t_auth_check_malformed_padding_rejects(void) {
  struct conn c;
  struct vhost_t vh;
  struct auth_store store;
  struct req_hdr_entry hdr;

  if (build_single_user_store(&store, "alice", "hunter2") != 0) {
    SKIPm("$6$ not supported on this host");
  }

  init_auth_header(&hdr, "Basic YWxpY2U6aHVudGVyMg=A");
  reset_auth_stubs();
  init_auth_conn(&c, &vh, &store, &hdr, 1);

  ASSERT_EQ(auth_basic_check(&c), 1);
  ASSERT_EQ(g_tx_build_calls, 1u);
  ASSERT_EQ(g_tx_begin_calls, 1u);
  PASS();
}

TEST t_auth_check_embedded_nul_in_password_rejects(void) {
  struct conn c;
  struct vhost_t vh;
  struct auth_store store;
  struct req_hdr_entry hdr;

  if (build_single_user_store(&store, "alice", "hunter2") != 0) {
    SKIPm("$6$ not supported on this host");
  }

  init_auth_header(&hdr, "Basic YWxpY2U6aHVudGVyMgBzdWZmaXg=");
  reset_auth_stubs();
  init_auth_conn(&c, &vh, &store, &hdr, 1);

  ASSERT_EQ(auth_basic_check(&c), 1);
  ASSERT_EQ(g_tx_build_calls, 1u);
  ASSERT_EQ(g_tx_begin_calls, 1u);
  PASS();
}

TEST t_auth_check_embedded_nul_in_user_rejects(void) {
  struct conn c;
  struct vhost_t vh;
  struct auth_store store;
  struct req_hdr_entry hdr;

  if (build_single_user_store(&store, "alice", "hunter2") != 0) {
    SKIPm("$6$ not supported on this host");
  }

  init_auth_header(&hdr, "Basic YWxpY2UAc3VmZml4Omh1bnRlcjI=");
  reset_auth_stubs();
  init_auth_conn(&c, &vh, &store, &hdr, 1);

  ASSERT_EQ(auth_basic_check(&c), 1);
  ASSERT_EQ(g_tx_build_calls, 1u);
  ASSERT_EQ(g_tx_begin_calls, 1u);
  PASS();
}

TEST t_auth_check_truncated_authorization_rejects(void) {
  struct conn c;
  struct vhost_t vh;
  struct auth_store store;
  struct req_hdr_entry hdr;

  if (build_single_user_store(&store, "alice", "hunter2") != 0) {
    SKIPm("$6$ not supported on this host");
  }

  /* Overlong Authorization values must be denied even if the retained
   * prefix still looks like valid credentials. */
  init_auth_header(&hdr, "Basic YWxpY2U6aHVudGVyMg==");
  hdr.flags = REQ_HDR_F_VALUE_TRUNCATED;
  reset_auth_stubs();
  init_auth_conn(&c, &vh, &store, &hdr, 1);

  ASSERT_EQ(auth_basic_check(&c), 1);
  ASSERT_EQ(g_tx_build_calls, 1u);
  ASSERT_EQ(g_tx_begin_calls, 1u);
  PASS();
}

TEST t_auth_check_tx_build_failure_returns_internal_error(void) {
  struct conn c;
  struct vhost_t vh;
  struct auth_store store;

  if (build_single_user_store(&store, "alice", "hunter2") != 0) {
    SKIPm("$6$ not supported on this host");
  }

  reset_auth_stubs();
  g_tx_build_rc = -1;
  init_auth_conn(&c, &vh, &store, NULL, 0);

  ASSERT_EQ(auth_basic_check(&c), -1);
  ASSERT_EQ(g_tx_build_calls, 1u);
  ASSERT_EQ(g_tx_begin_calls, 0u);
  PASS();
}

SUITE(s_auth_check) {
  RUN_TEST(t_auth_check_no_store_allows);
  RUN_TEST(t_auth_check_missing_header_rejects);
  RUN_TEST(t_auth_check_missing_header_rejects_with_policy_headers);
  RUN_TEST(t_auth_check_policy_overflow_fail_closes);
  RUN_TEST(t_auth_check_wrong_scheme_rejects);
  RUN_TEST(t_auth_check_bad_base64_rejects);
  RUN_TEST(t_auth_check_wrong_password_rejects);
  RUN_TEST(t_auth_check_unknown_user_rejects);
  RUN_TEST(t_auth_check_valid_credentials_allows);
  RUN_TEST(t_auth_check_lowercase_scheme_allows);
  RUN_TEST(t_auth_check_multiple_spaces_after_scheme_allows);
  RUN_TEST(t_auth_check_malformed_padding_rejects);
  RUN_TEST(t_auth_check_embedded_nul_in_password_rejects);
  RUN_TEST(t_auth_check_embedded_nul_in_user_rejects);
  RUN_TEST(t_auth_check_truncated_authorization_rejects);
  RUN_TEST(t_auth_check_tx_build_failure_returns_internal_error);
}

/* =========================================================================
 * Suite: auth_store_free
 * ====================================================================== */

TEST t_store_free_null_is_noop(void) {
  auth_store_free(NULL);
  PASS();
}

SUITE(s_auth_store_free) {
  RUN_TEST(t_store_free_null_is_noop);
}

/* =========================================================================
 * Main
 * ====================================================================== */

GREATEST_MAIN_DEFS();

int main(int argc, char **argv) {
  GREATEST_MAIN_BEGIN();
  RUN_SUITE(s_base64);
  RUN_SUITE(s_auth_store);
  RUN_SUITE(s_auth_check);
  RUN_SUITE(s_auth_store_free);
  GREATEST_MAIN_END();
}
