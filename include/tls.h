#pragma once

#include <stddef.h>
#include <sys/types.h>

struct conn;
struct vhost_t;
struct tls_ctx;

enum tls_hs_status {
  TLS_HS_DONE = 0,
  TLS_HS_WANT_READ,
  TLS_HS_WANT_WRITE,
  TLS_HS_ERROR,
};

int tls_global_init(void);
void tls_global_cleanup(void);

const char *tls_runtime_version(void);

int tls_ctx_build_for_vhost(const struct vhost_t *vh, struct tls_ctx **out_ctx, char err[256]);
void tls_ctx_free(struct tls_ctx *ctx);

// Vhost-level TLS context lifecycle.
struct config_t;
int tls_vhost_effective_enabled(const struct config_t *cfg, const struct vhost_t *vh);
int tls_init_vhost_contexts(struct config_t *cfg, char err[256]);
int tls_reload_vhost_contexts(struct config_t *cfg, char err[256]);

// TLS context read-lock for concurrent accept paths.
void tls_ctx_rdlock(void);
void tls_ctx_unlock(void);

int tls_conn_init(struct conn *c, struct tls_ctx *ctx, int fd, char err[256]);
enum tls_hs_status tls_conn_handshake_step(struct conn *c, char err[256]);
ssize_t tls_conn_recv(struct conn *c, void *buf, size_t len, int *want_read, int *want_write);
ssize_t tls_conn_send(struct conn *c, const void *buf, size_t len, int *want_read, int *want_write);
int tls_conn_shutdown(struct conn *c);
void tls_conn_destroy(struct conn *c);
