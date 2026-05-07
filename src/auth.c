#include "include/auth.h"
#include "include/conn.h"
#include "include/http_headers.h"
#include "include/logger.h"
#include "include/tx.h"
#include "include/types.h"

#include <crypt.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

/* ---------------------------------------------------------------------------
 * Constant-time string comparison — used to compare crypt_r output.
 * Operates over the full length of the longer string to avoid early-exit
 * leaks without reading past the end of the shorter buffer.
 * Returns 0 if equal, non-zero if different.
 * --------------------------------------------------------------------------- */
static int auth_ct_strcmp(const char *a, const char *b) {
  size_t a_len = strlen(a);
  size_t b_len = strlen(b);
  size_t max_len = (a_len > b_len) ? a_len : b_len;
  unsigned diff = (unsigned)(a_len ^ b_len);

  for (size_t i = 0; i < max_len; i++) {
    unsigned char a_ch = (i < a_len) ? (unsigned char)a[i] : 0u;
    unsigned char b_ch = (i < b_len) ? (unsigned char)b[i] : 0u;
    diff |= (unsigned)(a_ch ^ b_ch);
  }

  return (int)diff;
}

/* ---------------------------------------------------------------------------
 * Base64 decode (RFC 4648 §4).
 *
 * Decodes src[0..src_len) into dst[0..dst_cap).
 * Returns the number of decoded bytes on success, -1 on invalid input or if
 * the decoded output would exceed dst_cap.
 * The caller is responsible for NUL-terminating dst if needed.
 * --------------------------------------------------------------------------- */
static int b64_char_val(unsigned char c) {
  if (c >= 'A' && c <= 'Z') {
    return c - 'A';
  }
  if (c >= 'a' && c <= 'z') {
    return c - 'a' + 26;
  }
  if (c >= '0' && c <= '9') {
    return c - '0' + 52;
  }
  if (c == '+') {
    return 62;
  }
  if (c == '/') {
    return 63;
  }
  if (c == '=') {
    return -2; /* padding */
  }
  return -1; /* invalid */
}

int auth_base64_decode(const char *src, size_t src_len, char *dst, size_t dst_cap) {
  size_t out = 0;
  size_t i = 0;

  while (i < src_len) {
    int c_present;
    int d_present;
    int a = b64_char_val((unsigned char)src[i++]);
    int b = (i < src_len) ? b64_char_val((unsigned char)src[i++]) : -1;
    int c;
    int d;

    c_present = (i < src_len) ? 1 : 0;
    c = c_present ? b64_char_val((unsigned char)src[i++]) : -2;
    d_present = (i < src_len) ? 1 : 0;
    d = d_present ? b64_char_val((unsigned char)src[i++]) : -2;

    /* First two chars must be data bytes (not padding, not invalid). */
    if (a < 0 || b < 0) {
      return -1;
    }

    if (out >= dst_cap) {
      return -1;
    }
    dst[out++] = (char)((unsigned)((a << 2) | (b >> 4)) & 0xffu);

    if (c == -2) {
      if (c_present && (!d_present || d != -2)) {
        return -1;
      }
      break; /* padded or short final group: only one output byte */
    }
    if (c < 0) {
      return -1;
    }
    if (out >= dst_cap) {
      return -1;
    }
    dst[out++] = (char)((unsigned)((b << 4) | (c >> 2)) & 0xffu);

    if (d == -2) {
      break; /* padded or short final group: two output bytes */
    }
    if (d < 0) {
      return -1;
    }
    if (out >= dst_cap) {
      return -1;
    }
    dst[out++] = (char)((unsigned)((c << 6) | d) & 0xffu);
  }
  if (i < src_len) {
    return -1; /* junk after padding */
  }
  return (int)out;
}

/* ---------------------------------------------------------------------------
 * Password verification via crypt_r.
 *
 * Passes the stored hash string directly as the settings argument so
 * crypt_r interprets the algorithm prefix ($5$, $6$, etc.) itself.
 * Returns 0 if password matches, -1 if not or if the algorithm is
 * unsupported on this host (crypt_r returns NULL or "*0").
 * --------------------------------------------------------------------------- */
static int auth_verify_password(const char *password, const char *hash) {
  struct crypt_data cd;
  memset(&cd, 0, sizeof(cd));
  char *result = crypt_r(password, hash, &cd);
  if (!result || result[0] == '*') {
    return -1;
  }
  return auth_ct_strcmp(result, hash) == 0 ? 0 : -1;
}

/* Probe whether the host's crypt_r can handle a given hash format.
 * Returns 1 if supported, 0 if not. */
static int auth_probe_hash(const char *hash) {
  struct crypt_data cd;
  memset(&cd, 0, sizeof(cd));
  char *result = crypt_r("", hash, &cd);
  return (result && result[0] != '*') ? 1 : 0;
}

/* ---------------------------------------------------------------------------
 * Credential store — load / free.
 * --------------------------------------------------------------------------- */

struct auth_store *auth_store_load(const char *path) {
  if (!path || !path[0]) {
    return NULL;
  }

  FILE *f = fopen(path, "r");
  if (!f) {
    LOGE(LOGC_CORE, "auth: cannot open credential file '%s': %s", path, strerror(errno));
    return NULL;
  }

  /* Warn if world-readable. */
  struct stat st;
  if (fstat(fileno(f), &st) == 0 && (st.st_mode & 0004)) {
    LOGW(LOGC_CORE, "auth: credential file '%s' is world-readable", path);
  }

  struct auth_store *store = calloc(1, sizeof(*store));
  if (!store) {
    LOGE(LOGC_CORE, "auth: out of memory loading '%s'", path);
    fclose(f);
    return NULL;
  }

  char line[AUTH_USER_MAX + 1 + AUTH_HASH_MAX + 2]; /* user:hash\n\0 */
  unsigned line_no = 0;
  unsigned skipped = 0;

  while (fgets(line, (int)sizeof(line), f)) {
    int line_truncated;
    line_no++;

    line_truncated = 0;

    /* Strip trailing newline / carriage return. */
    size_t len = strlen(line);
    while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
      line[--len] = '\0';
    }

    if (len == sizeof(line) - 1) {
      int ch;

      line_truncated = 1;
      while ((ch = fgetc(f)) != EOF) {
        if (ch == '\n') {
          break;
        }
      }
    }

    if (line_truncated) {
      LOGW(LOGC_CORE, "auth: '%s' line %u: line too long; skipped", path, line_no);
      skipped++;
      continue;
    }

    if (len == 0 || line[0] == '#') {
      continue; /* blank / comment */
    }

    char *colon = strchr(line, ':');
    if (!colon) {
      LOGW(LOGC_CORE, "auth: '%s' line %u: no ':' separator; skipped", path, line_no);
      skipped++;
      continue;
    }

    size_t user_len = (size_t)(colon - line);
    size_t hash_len = strlen(colon + 1);

    if (user_len == 0 || user_len > AUTH_USER_MAX) {
      LOGW(LOGC_CORE, "auth: '%s' line %u: username too long; skipped", path, line_no);
      skipped++;
      continue;
    }
    if (hash_len == 0 || hash_len > AUTH_HASH_MAX) {
      LOGW(LOGC_CORE, "auth: '%s' line %u: hash field too long; skipped", path, line_no);
      skipped++;
      continue;
    }

    /* Probe whether the host crypt_r supports this hash format. */
    if (!auth_probe_hash(colon + 1)) {
      LOGW(LOGC_CORE,
           "auth: '%s' line %u: hash format unsupported on this host; skipped",
           path,
           line_no);
      skipped++;
      continue;
    }

    if (store->count >= AUTH_MAX_USERS) {
      LOGW(LOGC_CORE,
           "auth: '%s': max %u entries reached; remaining entries ignored",
           path,
           AUTH_MAX_USERS);
      break;
    }

    if (store->count == store->cap) {
      unsigned new_cap = store->cap ? (store->cap * 2u) : 8u;
      if (new_cap > AUTH_MAX_USERS || new_cap < store->cap) {
        new_cap = AUTH_MAX_USERS;
      }

      struct auth_entry *grown =
        (struct auth_entry *)realloc(store->entries, (size_t)new_cap * sizeof(struct auth_entry));
      if (!grown) {
        LOGE(LOGC_CORE, "auth: out of memory loading '%s'", path);
        fclose(f);
        free(store->entries);
        free(store);
        return NULL;
      }
      store->entries = grown;
      store->cap = new_cap;
    }

    struct auth_entry *e = &store->entries[store->count];
    memcpy(e->user, line, user_len);
    e->user[user_len] = '\0';
    memcpy(e->hash, colon + 1, hash_len);
    e->hash[hash_len] = '\0';
    store->count++;
  }

  if (ferror(f)) {
    int saved_errno = errno;
    LOGE(LOGC_CORE,
         "auth: read error in credential file '%s': %s",
         path,
         saved_errno ? strerror(saved_errno) : "unknown error");
    fclose(f);
    auth_store_free(store);
    return NULL;
  }

  fclose(f);

  if (store->count == 0) {
    LOGE(LOGC_CORE,
         "auth: credential file '%s' has no usable entries%s",
         path,
         skipped ? " (all entries skipped)" : "");
    auth_store_free(store);
    return NULL;
  }

  LOGI(LOGC_CORE, "auth: loaded %u credential(s) from '%s'", store->count, path);
  return store;
}

void auth_store_free(struct auth_store *store) {
  if (!store) {
    return;
  }
  free(store->entries);
  free(store);
}

/* ---------------------------------------------------------------------------
 * Request-path auth check.
 * --------------------------------------------------------------------------- */

int auth_basic_check(struct conn *c) {
  if (!c || !c->vhost || !c->vhost->auth_store) {
    return 0; /* unprotected */
  }

  const struct vhost_t *vh = c->vhost;
  const struct auth_store *store = vh->auth_store;

  /* --- Retrieve the Authorization header value. --- */
  const struct req_hdr_entry *auth_hdr = NULL;
  for (uint8_t i = 0; i < c->h1.req_hdr_count; ++i) {
    const struct req_hdr_entry *entry = &c->h1.req_hdrs[i];
    if ((enum http_header_id)entry->id == HDR_ID_AUTHORIZATION) {
      auth_hdr = entry;
      break;
    }
  }
  if (!auth_hdr || (auth_hdr->flags & REQ_HDR_F_VALUE_TRUNCATED)) {
    goto deny;
  }

  uint16_t val_len = auth_hdr->value_len;
  const char *hdr = auth_hdr->value;
  if (!hdr || val_len < 6 || strncasecmp(hdr, "Basic", 5) != 0 || hdr[5] != ' ') {
    goto deny;
  }

  /* --- Base64-decode the credential token. ---
   * REQ_HDR_VALUE_MAX=128; after the "Basic" scheme and required space we
   * have at most 122 base64 chars. Additional spaces reduce that further, so
   * the buffer below remains safe. */
  char decoded[96];
  const char *token = hdr + 5;
  size_t token_len = (size_t)(val_len - 5);
  while (token_len > 0 && *token == ' ') {
    token++;
    token_len--;
  }
  if (token_len == 0) {
    goto deny;
  }
  int dec_len = auth_base64_decode(token, token_len, decoded, sizeof(decoded) - 1);
  if (dec_len <= 0) {
    goto deny;
  }
  if (memchr(decoded, '\0', (size_t)dec_len)) {
    goto deny;
  }
  decoded[dec_len] = '\0';

  /* --- Split "user:password" on the first colon. --- */
  char *sep = memchr(decoded, ':', (size_t)dec_len);
  if (!sep) {
    goto deny;
  }
  *sep = '\0';
  const char *user = decoded;
  const char *pass = sep + 1;

  if (store->count == 0 || !store->entries) {
    goto deny;
  }

  /* --- Look up user in store and verify. ---
   * Unknown users still perform one password verification against a loaded
   * hash so the reject path does not reveal user existence through the large
   * crypt_r timing gap. */
  const char *candidate_hash = store->entries[0].hash;
  int user_found = 0;
  for (unsigned i = 0; i < store->count; i++) {
    if (!user_found && strcmp(store->entries[i].user, user) == 0) {
      candidate_hash = store->entries[i].hash;
      user_found = 1;
    }
  }
  if (auth_verify_password(pass, candidate_hash) == 0 && user_found) {
    return 0;
  }

deny:;
  /* --- Build and stage a 401 response. --- */
  char challenge[128];
  int n = snprintf(challenge,
                   sizeof(challenge),
                   "WWW-Authenticate: Basic realm=\"%s\"\r\n",
                   vh->auth_realm[0] ? vh->auth_realm : "Restricted");
  if (n <= 0 || (size_t)n >= sizeof(challenge)) {
    /* Realm string is validated at config load; this should not happen. */
    snprintf(challenge, sizeof(challenge), "WWW-Authenticate: Basic realm=\"Restricted\"\r\n");
  }

  static const char body[] = "Unauthorized\n";
  static const size_t body_len = sizeof(body) - 1;

  const char *buf = NULL;
  size_t len = 0;
  const int ka = c->h1.want_keepalive;
  if (tx_build_headers(&c->tx,
                       "401 Unauthorized",
                       "text/plain",
                       /*emit_content_length=*/1,
                       body_len,
                       body,
                       body_len,
                       ka,
                       /*drain_after_headers=*/0,
                       challenge,
                       &buf,
                       &len)
      != 0) {
    /* tx_build_headers failed (OOM); signal caller to send 500 instead. */
    return -1;
  }

  struct tx_next_io out = {0};
  (void)tx_begin_headers(&c->tx,
                         RK_401,
                         buf,
                         len,
                         ka,
                         /*drain_after_headers=*/0,
                         &out);
  return 1;
}
