#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "include/conn.h"
#include "include/request_handlers.h"
#include "include/static_serve_utils.h"
#include "include/tx.h"

const char *static_serve_mime_type_for_path(const char *path) {
  if (!path) {
    return "application/octet-stream";
  }
  const char *dot = strrchr(path, '.');
  if (!dot || dot == path) {
    return "application/octet-stream";
  }

  if (strcmp(dot, ".html") == 0 || strcmp(dot, ".htm") == 0) {
    return "text/html; charset=utf-8";
  }
  if (strcmp(dot, ".css") == 0) {
    return "text/css; charset=utf-8";
  }
  if (strcmp(dot, ".js") == 0) {
    return "application/javascript; charset=utf-8";
  }
  if (strcmp(dot, ".json") == 0) {
    return "application/json; charset=utf-8";
  }
  if (strcmp(dot, ".txt") == 0) {
    return "text/plain; charset=utf-8";
  }
  if (strcmp(dot, ".png") == 0) {
    return "image/png";
  }
  if (strcmp(dot, ".jpg") == 0 || strcmp(dot, ".jpeg") == 0) {
    return "image/jpeg";
  }
  if (strcmp(dot, ".gif") == 0) {
    return "image/gif";
  }
  if (strcmp(dot, ".svg") == 0) {
    return "image/svg+xml";
  }

  return "application/octet-stream";
}

int static_serve_build_docroot_relpath(char out[PATH_MAX],
                                       const char *path_norm,
                                       size_t path_norm_len,
                                       int path_ends_with_slash) {
  if (!out || !path_norm || path_norm_len == 0) {
    return -1;
  }
  if (path_norm[0] != '/') {
    return -1;
  }

  const char *rel = path_norm + 1;
  size_t rel_len = path_norm_len - 1;

  size_t off = 0;
  if (rel_len == 0 || path_ends_with_slash) {
    if (rel_len > 0) {
      if (rel[rel_len - 1] == '/') {
        rel_len--;
      }
      if (rel_len > 0) {
        if (rel_len >= PATH_MAX) {
          return -1;
        }
        memcpy(out + off, rel, rel_len);
        off += rel_len;
        if (off + 1 >= PATH_MAX) {
          return -1;
        }
        out[off++] = '/';
      }
    }
    static const char INDEX[] = "index.html";
    size_t ilen = sizeof(INDEX) - 1;
    if (off + ilen >= PATH_MAX) {
      return -1;
    }
    memcpy(out + off, INDEX, ilen);
    off += ilen;
  } else {
    if (rel_len >= PATH_MAX) {
      return -1;
    }
    memcpy(out + off, rel, rel_len);
    off += rel_len;
  }

  out[off] = 0;
  return 0;
}

#ifndef NAME_MAX
#define NAME_MAX 255
#endif

int static_serve_openat_beneath_nofollow(int root_dirfd, const char *relpath) {
  if (root_dirfd < 0 || !relpath || relpath[0] == '\0') {
    return -1;
  }
  if (relpath[0] == '/') {
    errno = EINVAL;
    return -1;
  }

  int cur = root_dirfd;
  const char *p = relpath;
  while (*p) {
    while (*p == '/') {
      p++;
    }
    if (*p == '\0') {
      break;
    }

    const char *slash = strchr(p, '/');
    size_t seg_len = slash ? (size_t)(slash - p) : strlen(p);
    if (seg_len == 0) {
      errno = EINVAL;
      if (cur != root_dirfd) {
        close(cur);
      }
      return -1;
    }
    if (seg_len > (size_t)NAME_MAX) {
      errno = ENAMETOOLONG;
      if (cur != root_dirfd) {
        close(cur);
      }
      return -1;
    }

    char seg[NAME_MAX + 1];
    memcpy(seg, p, seg_len);
    seg[seg_len] = 0;

    if (strcmp(seg, ".") == 0 || strcmp(seg, "..") == 0) {
      errno = EACCES;
      if (cur != root_dirfd) {
        close(cur);
      }
      return -1;
    }

    int is_last = (slash == NULL);
    if (!is_last) {
      int next = openat(cur, seg, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
      if (cur != root_dirfd) {
        close(cur);
      }
      if (next < 0) {
        return -1;
      }
      cur = next;
      p = slash + 1;
      continue;
    }

    int fd = openat(cur, seg, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (cur != root_dirfd) {
      close(cur);
    }
    return fd;
  }

  errno = EINVAL;
  if (cur != root_dirfd) {
    close(cur);
  }
  return -1;
}

int static_serve_tx_set_dynamic_response_ex(struct conn *c,
                                            const char *status_line,
                                            const char *content_type,
                                            size_t content_len,
                                            const void *body,
                                            size_t body_send_len,
                                            int keepalive) {
  if (!c || !status_line) {
    return -1;
  }

  if (body_send_len > 0 && !body) {
    return -1;
  }

  const char *buf = NULL;
  size_t len = 0;
  if (tx_build_headers(&c->tx,
                       status_line,
                       content_type,
                       content_len,
                       body,
                       body_send_len,
                       keepalive,
                       /*drain_after_headers=*/0,
                       &buf,
                       &len)
      != 0) {
    return -1;
  }

  struct tx_next_io out = {0};
  (void)tx_begin_headers(&c->tx,
                         keepalive ? RK_OK_KA : RK_OK_CLOSE,
                         buf,
                         len,
                         keepalive,
                         /*drain_after_headers=*/0,
                         &out);
  return 0;
}

int static_serve_try_prepare_docroot_response(struct conn *c,
                                              int docroot_fd,
                                              int *static_open_err) {
  if (!c || docroot_fd < 0 || !static_open_err) {
    return 0;
  }

  char relpath[PATH_MAX];
  int fd = -1;
  if (static_serve_build_docroot_relpath(relpath,
                                         c->h1.path_norm,
                                         (size_t)c->h1.path_norm_len,
                                         (int)c->h1.path_ends_with_slash)
      == 0) {
    fd = static_serve_openat_beneath_nofollow(docroot_fd, relpath);
    if (fd < 0) {
      *static_open_err = request_static_open_err_merge(*static_open_err, errno);
    }
  }

  if (fd < 0) {
    return 0;
  }

  struct stat st;
  if (fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_size >= 0
      && (uint64_t)st.st_size <= (uint64_t)SIZE_MAX) {
    size_t fsz = (size_t)st.st_size;
    const int keep = c->h1.want_keepalive ? 1 : 0;
    const char *ctype = static_serve_mime_type_for_path(relpath);
    struct request_static_serve_plan static_serve = request_build_static_serve_plan(c, fsz);
    int attempt_sendfile = 0;

    enum request_static_serve_mode mode = static_serve.mode;
    if (c->tls_enabled && mode == REQUEST_STATIC_SERVE_SENDFILE) {
      mode = REQUEST_STATIC_SERVE_BUFFERED;
    }

    switch (mode) {
    case REQUEST_STATIC_SERVE_HEAD:
      close(fd);
      fd = -1;
      if (static_serve_tx_set_dynamic_response_ex(c, "200 OK", ctype, fsz, NULL, 0, keep) == 0) {
        return 1;
      }
      break;

    case REQUEST_STATIC_SERVE_BUFFERED: {
      char *file_buf = (char *)malloc(fsz);
      if (file_buf) {
        size_t got = 0;
        while (got < fsz) {
          ssize_t r = read(fd, file_buf + got, fsz - got);
          if (r < 0) {
            if (errno == EINTR) {
              continue;
            }
            break;
          }
          if (r == 0) {
            break;
          }
          got += (size_t)r;
        }

        close(fd);
        fd = -1;

        if (got == fsz) {
#if ENABLE_ITEST_ECHO
          c->tx.itest_static_mode = "buffered";
#endif
          if (static_serve_tx_set_dynamic_response_ex(c, "200 OK", ctype, fsz, file_buf, fsz, keep)
              == 0) {
            free(file_buf);
            return 1;
          }
        }

        free(file_buf);
      }

      fd = static_serve_openat_beneath_nofollow(docroot_fd, relpath);
      if (fd < 0) {
        *static_open_err = request_static_open_err_merge(*static_open_err, errno);
      }
      attempt_sendfile = 1;
      break;
    }

    case REQUEST_STATIC_SERVE_SENDFILE:
      attempt_sendfile = 1;
      break;
    }

    if (attempt_sendfile && fd >= 0) {
#if ENABLE_ITEST_ECHO
      c->tx.itest_static_mode = "sendfile";
#endif
      if (static_serve_tx_set_dynamic_response_ex(c, "200 OK", ctype, fsz, NULL, 0, keep) == 0) {
        if (c->tx.file_fd >= 0) {
          close(c->tx.file_fd);
          c->tx.file_fd = -1;
        }
        if (fsz > 0) {
          c->tx.file_fd = fd;
          (void)tx_begin_sendfile(&c->tx, fsz);
          fd = -1;
        } else {
          close(fd);
          fd = -1;
        }
        return 1;
      }
    }
  }

  if (fd >= 0) {
    close(fd);
  }
  return 0;
}
