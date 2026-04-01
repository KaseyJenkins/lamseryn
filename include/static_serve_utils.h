#pragma once

#include <limits.h>
#include <stddef.h>

struct conn;

// Return MIME type for a normalized request path.
const char *static_serve_mime_type_for_path(const char *path);

// Build a docroot-relative path from normalized URL path components.
int static_serve_build_docroot_relpath(char out[PATH_MAX],
                                       const char *path_norm,
                                       size_t path_norm_len,
                                       int path_ends_with_slash);

// Open a path beneath root_dirfd with traversal-resistant semantics.
int static_serve_openat_beneath_nofollow(int root_dirfd, const char *relpath);

// Prepare dynamic TX headers/body for a static response.
int static_serve_tx_set_dynamic_response_ex(struct conn *c,
                                            const char *status_line,
                                            const char *content_type,
                                            size_t content_len,
                                            const void *body,
                                            size_t body_send_len,
                                            int keepalive);

// Try preparing a static-file response from docroot for the current request.
// Returns 1 when c->tx is staged for header send (and optional sendfile body),
// 0 when not served via static path, and leaves fallback planning to caller.
int static_serve_try_prepare_docroot_response(struct conn *c, int docroot_fd, int *static_open_err);
