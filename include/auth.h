#pragma once

#include <stddef.h>

/* Maximum credential entries per vhost. */
#define AUTH_MAX_USERS  256
/* Maximum username length in bytes (excluding NUL). */
#define AUTH_USER_MAX    64
/* Maximum stored hash length in bytes (excluding NUL).
 * SHA-512 crypt ($6$) output is 106 chars; 128 gives headroom. */
#define AUTH_HASH_MAX   128

struct auth_entry {
  char user[AUTH_USER_MAX + 1];
  char hash[AUTH_HASH_MAX + 1];
};

struct auth_store {
  struct auth_entry *entries;
  unsigned count;
  unsigned cap;
};

struct conn;

/* Decode a Basic-auth base64 token into dst. */
int auth_base64_decode(const char *src, size_t src_len, char *dst, size_t dst_cap);

/* Load credentials from an htpasswd-format file.
 * Returns a heap-allocated auth_store on success, NULL on failure (logs
 * reason).  Fails if the file is unreadable, has zero parseable entries, or
 * every entry has a hash format unsupported by the host crypt_r (returns
 * "*0"). */
struct auth_store *auth_store_load(const char *path);

/* Free a previously loaded auth_store. */
void auth_store_free(struct auth_store *store);

/* Check HTTP Basic credentials for an incoming request.
 * Reads the Authorization header captured in c->h1.req_hdrs.
 * Authorization capture is still bounded by REQ_HDR_VALUE_MAX in conn.h,
 * so credentials whose full header value exceeds that limit are rejected
 * before this function sees them.
 *
 * Returns 0 — authorized (or vhost has no auth_store; proceed to serving).
 * Returns 1 — unauthorized; a 401 response has been built and staged in
 *             c->tx.  The caller must return immediately without serving.
 * Returns -1 — internal failure while building the 401 response; the caller
 *              should send a 500 instead. */
int auth_basic_check(struct conn *c);
