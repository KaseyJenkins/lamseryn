#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <stdint.h>

#include "include/conn.h"
#include "include/tls.h"

#if ENABLE_TLS
#include <openssl/crypto.h>
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#endif

struct tls_ctx {
#if ENABLE_TLS
  SSL_CTX *ssl_ctx;
#endif
};

#if ENABLE_TLS
#define TLS_SNI_MAX_CTX 64

struct tls_sni_entry {
  char server_name[sizeof(((struct vhost_t *)0)->name)];
  uint16_t port;
  SSL_CTX *ssl_ctx;
};

static struct tls_sni_entry g_tls_sni_entries[TLS_SNI_MAX_CTX];
static size_t g_tls_sni_count = 0;

static void tls_set_err(char err[256], const char *msg) {
  if (!err) {
    return;
  }
  if (!msg) {
    msg = "tls error";
  }
  snprintf(err, 256, "%s", msg);
}

static void tls_set_err_openssl(char err[256], const char *prefix) {
  if (!err) {
    return;
  }

  unsigned long e = ERR_get_error();
  if (e == 0) {
    snprintf(err, 256, "%s", prefix ? prefix : "openssl error");
    return;
  }

  char b[160] = {0};
  ERR_error_string_n(e, b, sizeof(b));
  if (prefix && prefix[0]) {
    snprintf(err, 256, "%s: %s", prefix, b);
  } else {
    snprintf(err, 256, "%s", b);
  }
}

static void tls_sni_reset_registry(void) {
  g_tls_sni_count = 0;
  memset(g_tls_sni_entries, 0, sizeof(g_tls_sni_entries));
}

static void tls_sni_unregister_ctx(SSL_CTX *ctx) {
  if (!ctx) {
    return;
  }

  for (size_t i = 0; i < g_tls_sni_count; ++i) {
    if (g_tls_sni_entries[i].ssl_ctx != ctx) {
      continue;
    }
    if (i + 1 < g_tls_sni_count) {
      g_tls_sni_entries[i] = g_tls_sni_entries[g_tls_sni_count - 1];
    }
    memset(&g_tls_sni_entries[g_tls_sni_count - 1],
           0,
           sizeof(g_tls_sni_entries[g_tls_sni_count - 1]));
    g_tls_sni_count--;
    return;
  }
}

static int tls_sni_register_ctx(const struct vhost_t *vh, SSL_CTX *ctx, char err[256]) {
  if (!vh || !ctx) {
    return -1;
  }

  if (!vh->name[0]) {
    return 0;
  }

  for (size_t i = 0; i < g_tls_sni_count; ++i) {
    if (g_tls_sni_entries[i].ssl_ctx == ctx) {
      return 0;
    }
  }

  if (g_tls_sni_count >= TLS_SNI_MAX_CTX) {
    tls_set_err(err, "tls sni registry capacity exceeded");
    return -1;
  }

  struct tls_sni_entry *dst = &g_tls_sni_entries[g_tls_sni_count++];
  memset(dst, 0, sizeof(*dst));
  snprintf(dst->server_name, sizeof(dst->server_name), "%s", vh->name);
  dst->port = vh->port;
  dst->ssl_ctx = ctx;
  return 0;
}

static uint16_t tls_ssl_local_port(SSL *ssl) {
  if (!ssl) {
    return 0;
  }

  int fd = SSL_get_fd(ssl);
  if (fd < 0) {
    return 0;
  }

  struct sockaddr_storage ss;
  socklen_t slen = (socklen_t)sizeof(ss);
  memset(&ss, 0, sizeof(ss));
  if (getsockname(fd, (struct sockaddr *)&ss, &slen) != 0) {
    return 0;
  }

  if (ss.ss_family == AF_INET) {
    const struct sockaddr_in *sin = (const struct sockaddr_in *)&ss;
    return ntohs(sin->sin_port);
  }
  if (ss.ss_family == AF_INET6) {
    const struct sockaddr_in6 *sin6 = (const struct sockaddr_in6 *)&ss;
    return ntohs(sin6->sin6_port);
  }
  return 0;
}

static SSL_CTX *tls_sni_find_ctx(const char *server_name, uint16_t port) {
  if (!server_name || !*server_name) {
    return NULL;
  }

  for (size_t i = 0; i < g_tls_sni_count; ++i) {
    const struct tls_sni_entry *e = &g_tls_sni_entries[i];
    if (!e->ssl_ctx) {
      continue;
    }
    if (strcasecmp(e->server_name, server_name) != 0) {
      continue;
    }
    if (port != 0 && e->port != 0 && e->port != port) {
      continue;
    }
    return e->ssl_ctx;
  }
  return NULL;
}

static int tls_servername_cb(SSL *ssl, int *ad, void *arg) {
  (void)ad;
  (void)arg;

  if (!ssl) {
    return SSL_TLSEXT_ERR_NOACK;
  }

  const char *server_name = SSL_get_servername(ssl, TLSEXT_NAMETYPE_host_name);
  if (!server_name || !*server_name) {
    return SSL_TLSEXT_ERR_NOACK;
  }

  uint16_t local_port = tls_ssl_local_port(ssl);
  SSL_CTX *target = tls_sni_find_ctx(server_name, local_port);
  if (!target) {
    return SSL_TLSEXT_ERR_NOACK; // deterministic fallback: keep current ctx
  }

  if (!SSL_set_SSL_CTX(ssl, target)) {
    return SSL_TLSEXT_ERR_ALERT_FATAL;
  }

  return SSL_TLSEXT_ERR_OK;
}

static int tls_apply_ctx_policy(const struct vhost_t *vh, SSL_CTX *ctx, char err[256]) {
  if (!vh || !ctx) {
    return -1;
  }

  // Hardened defaults: disable legacy renegotiation and compression.
  {
    long opts = 0;
#ifdef SSL_OP_NO_RENEGOTIATION
    opts |= SSL_OP_NO_RENEGOTIATION;
#endif
#ifdef SSL_OP_NO_COMPRESSION
    opts |= SSL_OP_NO_COMPRESSION;
#endif
    if (opts) {
      (void)SSL_CTX_set_options(ctx, opts);
    }
  }

  int minv = TLS1_2_VERSION;
  if (vh->tls_min_version[0]) {
    if (strcmp(vh->tls_min_version, "tls1.2") == 0) {
      minv = TLS1_2_VERSION;
    } else if (strcmp(vh->tls_min_version, "tls1.3") == 0) {
      minv = TLS1_3_VERSION;
    } else {
      tls_set_err(err, "unsupported tls_min_version");
      return -1;
    }
  }

  if (SSL_CTX_set_min_proto_version(ctx, minv) != 1) {
    tls_set_err_openssl(err, "SSL_CTX_set_min_proto_version failed");
    return -1;
  }

  if (vh->tls_ciphers[0]) {
    if (SSL_CTX_set_cipher_list(ctx, vh->tls_ciphers) != 1) {
      tls_set_err_openssl(err, "SSL_CTX_set_cipher_list failed");
      return -1;
    }
  }

  if (vh->tls_ciphersuites[0]) {
    if (SSL_CTX_set_ciphersuites(ctx, vh->tls_ciphersuites) != 1) {
      tls_set_err_openssl(err, "SSL_CTX_set_ciphersuites failed");
      return -1;
    }
  }

  if (vh->tls_session_cache_set) {
    long mode = vh->tls_session_cache ? SSL_SESS_CACHE_SERVER : SSL_SESS_CACHE_OFF;
    (void)SSL_CTX_set_session_cache_mode(ctx, mode);
  } else {
    (void)SSL_CTX_set_session_cache_mode(ctx, SSL_SESS_CACHE_SERVER);
  }

#ifdef SSL_OP_NO_TICKET
  if (vh->tls_session_tickets_set) {
    if (vh->tls_session_tickets) {
      (void)SSL_CTX_clear_options(ctx, SSL_OP_NO_TICKET);
    } else {
      (void)SSL_CTX_set_options(ctx, SSL_OP_NO_TICKET);
    }
  }
#endif

  return 0;
}

static enum tls_hs_status tls_ssl_err_to_hs(int ssl_err, char err[256]) {
  switch (ssl_err) {
  case SSL_ERROR_WANT_READ:
    return TLS_HS_WANT_READ;
  case SSL_ERROR_WANT_WRITE:
    return TLS_HS_WANT_WRITE;
  default:
    tls_set_err_openssl(err, "SSL_accept failed");
    return TLS_HS_ERROR;
  }
}

static ssize_t tls_ssl_rw_map(int rc, SSL *ssl, int *want_read, int *want_write, const char *op) {
  if (want_read) {
    *want_read = 0;
  }
  if (want_write) {
    *want_write = 0;
  }

  if (rc > 0) {
    return (ssize_t)rc;
  }

  int se = SSL_get_error(ssl, rc);
  if (se == SSL_ERROR_WANT_READ) {
    if (want_read) {
      *want_read = 1;
    }
    errno = EAGAIN;
    return -1;
  }
  if (se == SSL_ERROR_WANT_WRITE) {
    if (want_write) {
      *want_write = 1;
    }
    errno = EAGAIN;
    return -1;
  }

  if (op) {
    (void)op;
  }
  errno = EIO;
  return -1;
}
#endif

int tls_global_init(void) {
#if ENABLE_TLS
  if (OPENSSL_init_ssl(0, NULL) != 1) {
    return -1;
  }
  tls_sni_reset_registry();
#endif
  return 0;
}

void tls_global_cleanup(void) {
#if ENABLE_TLS
  // OpenSSL 1.1+ manages global teardown; keep registry cleanup only.
  tls_sni_reset_registry();
#endif
}

const char *tls_runtime_version(void) {
#if ENABLE_TLS
  // Touch SSL symbol so runtime linkage stays explicit.
  (void)TLS_method();
  return OpenSSL_version(OPENSSL_VERSION);
#else
  return "tls-disabled";
#endif
}

int tls_ctx_build_for_vhost(const struct vhost_t *vh, struct tls_ctx **out_ctx, char err[256]) {
  if (err) {
    err[0] = '\0';
  }
  if (!out_ctx) {
    return -1;
  }

  *out_ctx = NULL;

#if ENABLE_TLS
  if (!vh) {
    tls_set_err(err, "invalid vhost");
    return -1;
  }

  if (!vh->tls_cert_file[0] || !vh->tls_key_file[0]) {
    tls_set_err(err, "tls_cert_file and tls_key_file are required on TLS vhosts");
    return -1;
  }

  SSL_CTX *ssl_ctx = SSL_CTX_new(TLS_server_method());
  if (!ssl_ctx) {
    tls_set_err_openssl(err, "SSL_CTX_new failed");
    return -1;
  }

  if (tls_apply_ctx_policy(vh, ssl_ctx, err) != 0) {
    SSL_CTX_free(ssl_ctx);
    return -1;
  }

  if (SSL_CTX_use_certificate_chain_file(ssl_ctx, vh->tls_cert_file) != 1) {
    tls_set_err_openssl(err, "SSL_CTX_use_certificate_chain_file failed");
    SSL_CTX_free(ssl_ctx);
    return -1;
  }

  if (SSL_CTX_use_PrivateKey_file(ssl_ctx, vh->tls_key_file, SSL_FILETYPE_PEM) != 1) {
    tls_set_err_openssl(err, "SSL_CTX_use_PrivateKey_file failed");
    SSL_CTX_free(ssl_ctx);
    return -1;
  }

  if (SSL_CTX_check_private_key(ssl_ctx) != 1) {
    tls_set_err_openssl(err, "SSL_CTX_check_private_key failed");
    SSL_CTX_free(ssl_ctx);
    return -1;
  }

  SSL_CTX_set_tlsext_servername_callback(ssl_ctx, tls_servername_cb);
  SSL_CTX_set_tlsext_servername_arg(ssl_ctx, NULL);

  struct tls_ctx *ctx = (struct tls_ctx *)calloc(1, sizeof(*ctx));
  if (!ctx) {
    SSL_CTX_free(ssl_ctx);
    return -1;
  }

  ctx->ssl_ctx = ssl_ctx;
  if (tls_sni_register_ctx(vh, ssl_ctx, err) != 0) {
    SSL_CTX_free(ssl_ctx);
    free(ctx);
    return -1;
  }
  *out_ctx = ctx;
  return 0;
#else
  (void)vh;
  *out_ctx = (struct tls_ctx *)calloc(1, sizeof(struct tls_ctx));
  return (*out_ctx) ? 0 : -1;
#endif
}

void tls_ctx_free(struct tls_ctx *ctx) {
#if ENABLE_TLS
  if (ctx && ctx->ssl_ctx) {
    tls_sni_unregister_ctx(ctx->ssl_ctx);
    SSL_CTX_free(ctx->ssl_ctx);
  }
#endif
  free(ctx);
}

int tls_conn_init(struct conn *c, struct tls_ctx *ctx, int fd, char err[256]) {
  if (err) {
    err[0] = '\0';
  }
  if (!c) {
    return -1;
  }

  c->tls_enabled = 1;
  c->tls_handshake_done = 0;
  c->tls_want_read = 0;
  c->tls_want_write = 0;
  c->tls_handle = NULL;

#if ENABLE_TLS
  if (!ctx || !ctx->ssl_ctx || fd < 0) {
    c->tls_enabled = 0;
    tls_set_err(err, "invalid tls connection init parameters");
    return -1;
  }

  SSL *ssl = SSL_new(ctx->ssl_ctx);
  if (!ssl) {
    c->tls_enabled = 0;
    tls_set_err_openssl(err, "SSL_new failed");
    return -1;
  }
  if (SSL_set_fd(ssl, fd) != 1) {
    SSL_free(ssl);
    c->tls_enabled = 0;
    tls_set_err_openssl(err, "SSL_set_fd failed");
    return -1;
  }

  SSL_set_accept_state(ssl);
  c->tls_handle = ssl;
#else
  (void)ctx;
  (void)fd;
  c->tls_enabled = 0;
#endif
  return 0;
}

enum tls_hs_status tls_conn_handshake_step(struct conn *c, char err[256]) {
  if (err) {
    err[0] = '\0';
  }

  if (!c || !c->tls_enabled) {
    return TLS_HS_ERROR;
  }

  if (c->tls_handshake_done) {
    return TLS_HS_DONE;
  }

#if ENABLE_TLS
  SSL *ssl = (SSL *)c->tls_handle;
  if (!ssl) {
    return TLS_HS_ERROR;
  }

  int rc = SSL_accept(ssl);
  if (rc == 1) {
    c->tls_handshake_done = 1;
    c->tls_want_read = 0;
    c->tls_want_write = 0;
    return TLS_HS_DONE;
  }

  enum tls_hs_status hs = tls_ssl_err_to_hs(SSL_get_error(ssl, rc), err);
  c->tls_want_read = (hs == TLS_HS_WANT_READ) ? 1 : 0;
  c->tls_want_write = (hs == TLS_HS_WANT_WRITE) ? 1 : 0;
  return hs;
#else
  return TLS_HS_ERROR;
#endif
}

ssize_t tls_conn_recv(struct conn *c, void *buf, size_t len, int *want_read, int *want_write) {
  if (want_read) {
    *want_read = 0;
  }
  if (want_write) {
    *want_write = 0;
  }

  if (!c || !buf || len == 0 || !c->tls_enabled) {
    errno = EINVAL;
    return -1;
  }

#if ENABLE_TLS
  SSL *ssl = (SSL *)c->tls_handle;
  if (!ssl) {
    errno = EINVAL;
    return -1;
  }

  int n = SSL_read(ssl, buf, (int)len);
  return tls_ssl_rw_map(n, ssl, want_read, want_write, "SSL_read");
#else
  errno = ENOTSUP;
  return -1;
#endif
}

ssize_t tls_conn_send(struct conn *c,
                      const void *buf,
                      size_t len,
                      int *want_read,
                      int *want_write) {
  if (want_read) {
    *want_read = 0;
  }
  if (want_write) {
    *want_write = 0;
  }

  if (!c || !buf || len == 0 || !c->tls_enabled) {
    errno = EINVAL;
    return -1;
  }

#if ENABLE_TLS
  SSL *ssl = (SSL *)c->tls_handle;
  if (!ssl) {
    errno = EINVAL;
    return -1;
  }

  int n = SSL_write(ssl, buf, (int)len);
  return tls_ssl_rw_map(n, ssl, want_read, want_write, "SSL_write");
#else
  errno = ENOTSUP;
  return -1;
#endif
}

int tls_conn_shutdown(struct conn *c) {
#if ENABLE_TLS
  if (c && c->tls_handle) {
    (void)SSL_shutdown((SSL *)c->tls_handle);
  }
#else
  (void)c;
#endif
  return 0;
}

void tls_conn_destroy(struct conn *c) {
  if (!c) {
    return;
  }

#if ENABLE_TLS
  if (c->tls_handle) {
    SSL_free((SSL *)c->tls_handle);
  }
#endif

  c->tls_enabled = 0;
  c->tls_handshake_done = 0;
  c->tls_want_read = 0;
  c->tls_want_write = 0;
  c->tls_handle = NULL;
}
