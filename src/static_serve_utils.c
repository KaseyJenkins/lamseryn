#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "include/compression.h"
#include "include/conn.h"
#include "include/http_headers.h"
#include "include/http_range.h"
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

// ---------------------------------------------------------------------------
// Conditional request helpers (304 Not Modified).
// ---------------------------------------------------------------------------

// Produces a strong-looking quoted ETag: "inode-size-mtimeUs".
// Returns the number of bytes written (excluding NUL), or 0 on failure.
static size_t static_serve_format_etag(char *buf, size_t cap, const struct stat *st) {
  if (!buf || cap == 0 || !st) {
    return 0;
  }
  uint64_t mtime_us =
    (uint64_t)st->st_mtim.tv_sec * 1000000ULL + (uint64_t)(st->st_mtim.tv_nsec / 1000);
  int n = snprintf(buf, cap, "\"%" PRIx64 "-" "%" PRIx64 "-" "%" PRIx64 "\"",
                   (uint64_t)st->st_ino,
                   (uint64_t)st->st_size,
                   mtime_us);
  if (n <= 0 || (size_t)n >= cap) {
    return 0;
  }
  return (size_t)n;
}

// Produces: "Sun, 06 Nov 1994 08:49:37 GMT"
// Uses static English name arrays to be locale-independent (RFC 7231 §7.1.1.1).
// Returns bytes written (excluding NUL), or 0 on failure.
static size_t static_serve_format_last_modified(char *buf, size_t cap, const struct stat *st) {
  if (!buf || cap == 0 || !st) {
    return 0;
  }
  static const char *const days[]   = {"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};
  static const char *const months[] = {"Jan","Feb","Mar","Apr","May","Jun",
                                        "Jul","Aug","Sep","Oct","Nov","Dec"};
  struct tm tm;
  if (!gmtime_r(&st->st_mtim.tv_sec, &tm)) {
    return 0;
  }
  if (tm.tm_wday < 0 || tm.tm_wday > 6 || tm.tm_mon < 0 || tm.tm_mon > 11) {
    return 0;
  }
  int n = snprintf(buf, cap, "%s, %02d %s %04d %02d:%02d:%02d GMT",
                   days[tm.tm_wday], tm.tm_mday, months[tm.tm_mon],
                   1900 + tm.tm_year, tm.tm_hour, tm.tm_min, tm.tm_sec);
  if (n <= 0 || (size_t)n >= cap) {
    return 0;
  }
  return (size_t)n;
}

// Only handles the preferred IMF-fixdate format: "Sun, 06 Nov 1994 08:49:37 GMT".
// Locale-independent: uses static English month names instead of strptime.
// Returns 0 on success, -1 on parse failure.
static int static_serve_parse_http_date(const char *str, time_t *out) {
  if (!str || !out) {
    return -1;
  }
  static const char *const months[] = {"Jan","Feb","Mar","Apr","May","Jun",
                                        "Jul","Aug","Sep","Oct","Nov","Dec"};
  const char *p = str;

  // Skip 3-char day name and ", " (we don't validate the day name).
  if (p[0] == '\0' || p[1] == '\0' || p[2] == '\0' || p[3] != ',' || p[4] != ' ') {
    return -1;
  }
  p += 5;

  // 2-digit day-of-month.
  if (p[0] < '0' || p[0] > '9' || p[1] < '0' || p[1] > '9' || p[2] != ' ') {
    return -1;
  }
  int mday = (p[0] - '0') * 10 + (p[1] - '0');
  p += 3;

  // 3-char month name.
  int mon = -1;
  for (int i = 0; i < 12; ++i) {
    if (p[0] == months[i][0] && p[1] == months[i][1] && p[2] == months[i][2]) {
      mon = i;
      break;
    }
  }
  if (mon < 0 || p[3] != ' ') {
    return -1;
  }
  p += 4;

  // 4-digit year.
  if (p[0] < '0' || p[0] > '9' || p[1] < '0' || p[1] > '9' ||
      p[2] < '0' || p[2] > '9' || p[3] < '0' || p[3] > '9' || p[4] != ' ') {
    return -1;
  }
  int year = (p[0]-'0')*1000 + (p[1]-'0')*100 + (p[2]-'0')*10 + (p[3]-'0');
  p += 5;

  // HH:MM:SS GMT
  if (p[0] < '0' || p[0] > '9' || p[1] < '0' || p[1] > '9' || p[2] != ':' ||
      p[3] < '0' || p[3] > '9' || p[4] < '0' || p[4] > '9' || p[5] != ':' ||
      p[6] < '0' || p[6] > '9' || p[7] < '0' || p[7] > '9' ||
      p[8] != ' ' || p[9] != 'G' || p[10] != 'M' || p[11] != 'T' || p[12] != '\0') {
    return -1;
  }
  int hour = (p[0]-'0')*10 + (p[1]-'0');
  int min  = (p[3]-'0')*10 + (p[4]-'0');
  int sec  = (p[6]-'0')*10 + (p[7]-'0');

  // RFC 7232 §3.3: ignore If-Modified-Since with an invalid HTTP-date.
  if (mday < 1 || mday > 31 || hour > 23 || min > 59 || sec > 60) {
    return -1;
  }

  struct tm tm;
  memset(&tm, 0, sizeof(tm));
  tm.tm_mday = mday;
  tm.tm_mon  = mon;
  tm.tm_year = year - 1900;
  tm.tm_hour = hour;
  tm.tm_min  = min;
  tm.tm_sec  = sec;

  *out = timegm(&tm);
  return 0;
}

// Evaluate RFC 7232 conditional-request logic for a static file.
// Returns 1 if the response should be 304 Not Modified, 0 otherwise.
// Caller must ensure the request method is GET or HEAD.
static int static_serve_check_not_modified(const struct conn *c,
                                           const struct stat *st,
                                           const char *etag,
                                           size_t etag_len) {
  if (!c || !st) {
    return 0;
  }

  // RFC 7232 §3.2: If-None-Match takes precedence over If-Modified-Since.
  uint16_t inm_len = 0;
  const char *inm =
    http_header_find_value(c->h1.req_hdrs, c->h1.req_hdr_count, HDR_ID_IF_NONE_MATCH, &inm_len);
  if (inm && inm_len > 0) {
    // Walk the comma-separated ETag list and compare each token as a whole
    // quoted string with OWS trimming (RFC 7232 §3.2).
    // Wildcard "*" matches any ETag.
    const char *p = inm;
    const char *end = inm + inm_len;
    while (p < end) {
      // Skip OWS before token.
      while (p < end && (*p == ' ' || *p == '\t')) p++;
      if (p >= end) break;

      // Find end of token (up to next comma or end of string).
      const char *tok = p;
      while (p < end && *p != ',') p++;
      const char *tok_end = p;

      // Trim trailing OWS from token.
      while (tok_end > tok && (tok_end[-1] == ' ' || tok_end[-1] == '\t')) tok_end--;

      size_t tok_len = (size_t)(tok_end - tok);

      // Wildcard.
      if (tok_len == 1 && tok[0] == '*') {
        return 1;
      }
      // Weak comparison (RFC 7232 §2.3.2): strip optional W/ prefix, then
      // compare the quoted opaque-tag. Our server emits strong ETags, but
      // clients or intermediaries may add the weak prefix.
      const char *cmp = tok;
      size_t cmp_len = tok_len;
      if (cmp_len >= 2 && cmp[0] == 'W' && cmp[1] == '/') {
        cmp += 2;
        cmp_len -= 2;
      }
      if (etag && etag_len > 0 && cmp_len == etag_len
          && memcmp(cmp, etag, etag_len) == 0) {
        return 1;
      }

      // Advance past comma.
      if (p < end) p++;
    }
    // If-None-Match was present but no token matched — do NOT fall through to
    // If-Modified-Since (RFC 7232 §6 precedence rule).
    return 0;
  }

  // RFC 7232 §3.3: If-Modified-Since (only when If-None-Match is absent).
  uint16_t ims_len = 0;
  const char *ims = http_header_find_value(
    c->h1.req_hdrs, c->h1.req_hdr_count, HDR_ID_IF_MODIFIED_SINCE, &ims_len);
  if (ims && ims_len > 0) {
    time_t ims_time;
    if (static_serve_parse_http_date(ims, &ims_time) == 0) {
      if (st->st_mtim.tv_sec <= ims_time) {
        return 1;
      }
    }
  }

  return 0;
}

// Build the combined ETag + Last-Modified header string for inclusion in
// both 200 and 304 responses. Returns bytes written (excluding NUL), 0 on failure.
static size_t static_serve_build_validator_headers(char *buf,
                                                   size_t cap,
                                                   const char *etag,
                                                   size_t etag_len,
                                                   const char *last_mod,
                                                   size_t last_mod_len) {
  if (!buf || cap == 0) {
    return 0;
  }
  int n = snprintf(buf, cap, "ETag: %.*s\r\nLast-Modified: %.*s\r\n",
                   (int)etag_len, etag ? etag : "",
                   (int)last_mod_len, last_mod ? last_mod : "");
  if (n <= 0 || (size_t)n >= cap) {
    return 0;
  }
  return (size_t)n;
}

// Evaluate If-Range (RFC 7233 §3.2).
// Returns 1 to apply Range, 0 to ignore Range and serve full entity.
static int static_serve_if_range_matches(const struct conn *c,
                                         const struct stat *st,
                                         const char *etag,
                                         size_t etag_len) {
  if (!c) {
    return 1; // no request headers
  }

  uint16_t ir_len = 0;
  const char *ir =
    http_header_find_value(c->h1.req_hdrs, c->h1.req_hdr_count, HDR_ID_IF_RANGE, &ir_len);
  if (!ir || ir_len == 0) {
    return 1; // no If-Range header
  }

  // Quoted value: strong ETag comparison.
  if (ir_len >= 2 && ir[0] == '"') {
    // RFC 7233 §3.2: If-Range with ETag uses strong comparison.
    // Weak ETags do not satisfy If-Range.
    if (etag && etag_len > 0 && ir_len == (uint16_t)etag_len
        && memcmp(ir, etag, etag_len) == 0) {
      return 1;
    }
    return 0;
  }

  // Weak ETags never satisfy If-Range.
  if (ir_len >= 2 && ir[0] == 'W' && ir[1] == '/') {
    return 0;
  }

  // Otherwise treat value as HTTP-date.
  if (st) {
    // Parse helper needs a NUL-terminated buffer.
    char date_buf[64];
    size_t copy_len = ir_len < sizeof(date_buf) - 1 ? ir_len : sizeof(date_buf) - 1;
    memcpy(date_buf, ir, copy_len);
    date_buf[copy_len] = '\0';

    time_t ir_time;
    if (static_serve_parse_http_date(date_buf, &ir_time) == 0) {
      if (st->st_mtim.tv_sec == ir_time) {
        return 1;
      }
    }
  }

  return 0;
}

int static_serve_tx_set_dynamic_response_ex(struct conn *c,
                                            const char *status_line,
                                            const char *content_type,
                                            size_t content_len,
                                            const void *body,
                                            size_t body_send_len,
                                            int keepalive,
                                            const char *extra_headers) {
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
                       extra_headers,
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

// Builds optional static-response headers (validators, Accept-Ranges, Content-Encoding, Vary, custom headers).
// Returns bytes written and exports ETag for later conditional checks.
// If Content-Encoding cannot fit, clears serving_enc so caller falls back to identity file.
static size_t static_serve_assemble_extra_headers(
    char *buf, size_t bufsz,
    const struct stat *st,
    uint64_t features,
    unsigned *serving_enc,
    const char *ctype,
    char *const *custom_headers, unsigned custom_headers_count,
    char *etag_out, size_t etag_outsz, size_t *etag_out_len) {
  char last_mod_buf[64];
  size_t last_mod_len = 0;
  size_t etag_len = 0;

  if (features & CFG_FEAT_CONDITIONAL) {
    etag_len = static_serve_format_etag(etag_out, etag_outsz, st);
    last_mod_len =
      static_serve_format_last_modified(last_mod_buf, sizeof(last_mod_buf), st);
  }
  *etag_out_len = etag_len;

  size_t vhdr_len = 0;
  if (etag_len > 0 || last_mod_len > 0) {
    vhdr_len = static_serve_build_validator_headers(
      buf, bufsz, etag_out, etag_len, last_mod_buf, last_mod_len);
  }
  if (vhdr_len == 0) {
    buf[0] = '\0';
  }

  if (features & CFG_FEAT_RANGE) {
    static const char ar[] = "Accept-Ranges: bytes\r\n";
    size_t ar_len = sizeof(ar) - 1;
    if (vhdr_len + ar_len < bufsz) {
      memcpy(buf + vhdr_len, ar, ar_len);
      vhdr_len += ar_len;
      buf[vhdr_len] = '\0';
    }
  }

  int vary_needed = (features & CFG_FEAT_COMPRESSION)
                    && compress_mime_is_compressible(ctype);
  if (*serving_enc) {
    const char *enc_name = compress_enc_name(*serving_enc);
    char ce_vary[64];
    int n = snprintf(ce_vary, sizeof(ce_vary),
                     "Content-Encoding: %s\r\nVary: Accept-Encoding\r\n",
                     enc_name);
    if (n > 0 && (size_t)n < sizeof(ce_vary)
        && vhdr_len + (size_t)n < bufsz) {
      memcpy(buf + vhdr_len, ce_vary, (size_t)n);
      vhdr_len += (size_t)n;
      buf[vhdr_len] = '\0';
      vary_needed = 0;
    } else {
      *serving_enc = 0;
    }
  }
  if (vary_needed) {
    static const char vary_hdr[] = "Vary: Accept-Encoding\r\n";
    size_t vary_len = sizeof(vary_hdr) - 1;
    if (vhdr_len + vary_len < bufsz) {
      memcpy(buf + vhdr_len, vary_hdr, vary_len);
      vhdr_len += vary_len;
      buf[vhdr_len] = '\0';
    }
  }
  for (unsigned i = 0; i < custom_headers_count; i++) {
    const char *hdr = custom_headers[i];
    if (!hdr) {
      continue;
    }
    size_t hlen = strlen(hdr);
    if (vhdr_len + hlen < bufsz) {
      memcpy(buf + vhdr_len, hdr, hlen);
      vhdr_len += hlen;
      buf[vhdr_len] = '\0';
    }
  }
  return vhdr_len;
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
    const struct vhost_t *vh = c->vhost;
    const char *ctype = static_serve_mime_type_for_path(relpath);

    // Precompressed sibling probe: look for a .br or .gz file next to the
    // requested path and serve it if the client advertises the encoding.
    // Skip Range requests: byte offsets are defined on identity bytes, so
    // serving compressed bytes would break the requested positions.
    unsigned serving_enc = 0;
    // accepted is hoisted so the dynamic compression path can reuse it.
    unsigned accepted = 0;
    if (vh && (vh->features & CFG_FEAT_COMPRESSION)
        && compress_mime_is_compressible(ctype)
        && (c->h1.method == HTTP_GET || c->h1.method == HTTP_HEAD)) {
      int has_range_hdr =
        (vh->features & CFG_FEAT_RANGE) != 0
        && http_header_find_value(c->h1.req_hdrs, c->h1.req_hdr_count,
                                  HDR_ID_RANGE, NULL) != NULL;
      if (!has_range_hdr) {
        uint16_t ae_len = 0;
        const char *ae = http_header_find_value(
          c->h1.req_hdrs, c->h1.req_hdr_count, HDR_ID_ACCEPT_ENCODING, &ae_len);
        if (ae && ae_len > 0) {
          accepted = compress_parse_accept_encoding(ae, ae_len);
          // Probe in preference order: brotli first, gzip fallback.
          static const unsigned pref[] = {COMP_ENC_BROTLI, COMP_ENC_GZIP, 0};
          for (int pi = 0; pref[pi] && !serving_enc; pi++) {
            if (!(accepted & pref[pi])) {
              continue;
            }
            const char *ext = compress_enc_ext(pref[pi]);
            char comp_relpath[PATH_MAX];
            int n = snprintf(comp_relpath, sizeof(comp_relpath), "%s%s", relpath, ext);
            if (n <= 0 || (size_t)n >= sizeof(comp_relpath)) {
              continue;
            }
            int comp_fd = static_serve_openat_beneath_nofollow(docroot_fd, comp_relpath);
            if (comp_fd >= 0) {
              struct stat comp_st;
              if (fstat(comp_fd, &comp_st) == 0
                  && S_ISREG(comp_st.st_mode)
                  && comp_st.st_size >= 0
                  && (uint64_t)comp_st.st_size <= (uint64_t)SIZE_MAX) {
                close(fd);
                fd = comp_fd;
                comp_fd = -1;
                st   = comp_st;
                fsz  = (size_t)comp_st.st_size;
                serving_enc = pref[pi];
              }
              if (comp_fd >= 0) {
                close(comp_fd);
              }
            }
          }
        }
      }
    }

    char etag_buf[64];
    size_t etag_len = 0;
    char validator_hdrs[2048];
    unsigned orig_serving_enc = serving_enc;
    char *const *custom_hdrs = vh ? (char *const *)vh->custom_headers : NULL;
    unsigned custom_hdrs_n = vh ? vh->custom_headers_count : 0;
    (void)static_serve_assemble_extra_headers(
      validator_hdrs, sizeof(validator_hdrs),
      &st, vh ? vh->features : 0, &serving_enc, ctype,
      custom_hdrs, custom_hdrs_n,
      etag_buf, sizeof(etag_buf), &etag_len);
    if (orig_serving_enc && !serving_enc) {
      // Content-Encoding header didn't fit — reopen the original
      // uncompressed file so we never send raw compressed bytes.
      close(fd);
      fd = static_serve_openat_beneath_nofollow(docroot_fd, relpath);
      if (fd < 0) {
        *static_open_err = request_static_open_err_merge(*static_open_err, errno);
        goto done;
      }
      if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)
          || st.st_size < 0 || (uint64_t)st.st_size > (uint64_t)SIZE_MAX) {
        goto done;
      }
      fsz = (size_t)st.st_size;
    }
    if (vh && (vh->features & CFG_FEAT_CONDITIONAL)
        && (c->h1.method == HTTP_GET || c->h1.method == HTTP_HEAD)
        && etag_len > 0) {
      if (static_serve_check_not_modified(c, &st, etag_buf, etag_len)) {
        close(fd);
        fd = -1;
        // 304 must not contain a message body (RFC 7232 §4.1).
        // Content-Type and Content-Length are omitted; we send only validators.
        const char *buf_304 = NULL;
        size_t len_304 = 0;
        if (tx_build_headers(&c->tx,
                             "304 Not Modified",
                             /*content_type=*/NULL,
                             /*content_len=*/0,
                             /*body=*/NULL,
                             /*body_send_len=*/0,
                             keep,
                             /*drain_after_headers=*/0,
                             validator_hdrs,
                             &buf_304,
                             &len_304)
            == 0) {
          struct tx_next_io out304 = {0};
          (void)tx_begin_headers(&c->tx,
                                 RK_304,
                                 buf_304,
                                 len_304,
                                 keep,
                                 /*drain_after_headers=*/0,
                                 &out304);
          return 1;
        }
        // If header build failed, fall through to normal close-fd path below.
        goto done;
      }
    }

    // Range handling runs after 304 checks and only for GET.
    if (vh && (vh->features & CFG_FEAT_RANGE) && c->h1.method == HTTP_GET) {
      uint16_t range_hdr_len = 0;
      const char *range_hdr =
        http_header_find_value(c->h1.req_hdrs, c->h1.req_hdr_count, HDR_ID_RANGE, &range_hdr_len);
      if (range_hdr && range_hdr_len > 0) {
        struct byte_range br = http_range_parse(range_hdr, range_hdr_len);
        if (br.valid) {
          // Apply range only when If-Range matches.
          int apply_range = static_serve_if_range_matches(c, &st, etag_buf, etag_len);
          if (apply_range) {
            struct resolved_range rr = http_range_resolve(&br, (uint64_t)fsz);
            if (!rr.satisfiable) {
              close(fd);
              fd = -1;
              char cr_buf[128];
              size_t cr_len = http_range_format_content_range_unsatisfied(
                cr_buf, sizeof(cr_buf), (uint64_t)fsz);
              // Return 416 with Content-Range, Content-Length: 0, and validators.
              char extra_416[512];
              int elen = snprintf(extra_416, sizeof(extra_416),
                                  "%.*sContent-Length: 0\r\n%s",
                                  (int)cr_len, cr_buf, validator_hdrs);
              if (elen <= 0 || (size_t)elen >= sizeof(extra_416)) {
                goto done;
              }
              const char *buf_416 = NULL;
              size_t len_416 = 0;
              if (tx_build_headers(&c->tx,
                                   "416 Range Not Satisfiable",
                                   /*content_type=*/NULL,
                                   /*content_len=*/0,
                                   /*body=*/NULL,
                                   /*body_send_len=*/0,
                                   keep,
                                   /*drain_after_headers=*/0,
                                   extra_416,
                                   &buf_416,
                                   &len_416)
                  == 0) {
                struct tx_next_io out416 = {0};
                (void)tx_begin_headers(&c->tx,
                                       RK_416,
                                       buf_416,
                                       len_416,
                                       keep,
                                       /*drain_after_headers=*/0,
                                       &out416);
                return 1;
              }
              goto done;
            }

            char cr_buf[128];
            size_t cr_len = http_range_format_content_range(
              cr_buf, sizeof(cr_buf), rr.start, rr.end, (uint64_t)fsz);
            // Build extra headers for 206.
            char extra_206[sizeof(validator_hdrs) + 256];
            int elen = snprintf(extra_206, sizeof(extra_206), "%.*s%s",
                                (int)cr_len, cr_buf, validator_hdrs);
            if (elen <= 0 || (size_t)elen >= sizeof(extra_206)) {
              goto done;
            }

            struct request_static_serve_plan rs = request_build_static_serve_plan(c, (size_t)rr.length);
            enum request_static_serve_mode rmode = rs.mode;
            if (c->tls_enabled && rmode == REQUEST_STATIC_SERVE_SENDFILE) {
              rmode = REQUEST_STATIC_SERVE_BUFFERED;
            }

            if (rmode == REQUEST_STATIC_SERVE_BUFFERED && rr.length <= (uint64_t)SIZE_MAX) {
              size_t range_len = (size_t)rr.length;
              char *rbuf = (char *)malloc(range_len);
              if (rbuf) {
                if (lseek(fd, (off_t)rr.start, SEEK_SET) != (off_t)-1) {
                  size_t got = 0;
                  while (got < range_len) {
                    ssize_t r = read(fd, rbuf + got, range_len - got);
                    if (r < 0) {
                      if (errno == EINTR) continue;
                      break;
                    }
                    if (r == 0) break;
                    got += (size_t)r;
                  }
                  close(fd);
                  fd = -1;
                  if (got == range_len) {
                    const char *buf_206 = NULL;
                    size_t len_206 = 0;
                    if (tx_build_headers(&c->tx,
                                         "206 Partial Content",
                                         ctype,
                                         range_len,
                                         rbuf,
                                         range_len,
                                         keep,
                                         /*drain_after_headers=*/0,
                                         extra_206,
                                         &buf_206,
                                         &len_206)
                        == 0) {
                      struct tx_next_io out206 = {0};
                      (void)tx_begin_headers(&c->tx,
                                             RK_206,
                                             buf_206,
                                             len_206,
                                             keep,
                                             /*drain_after_headers=*/0,
                                             &out206);
                      free(rbuf);
                      return 1;
                    }
                  }
                }
                free(rbuf);
              }
              // TLS cannot fall back to kernel sendfile.
              if (c->tls_enabled) {
                goto done;
              }
              // Non-TLS can retry with sendfile.
              if (fd < 0) {
                fd = static_serve_openat_beneath_nofollow(docroot_fd, relpath);
                if (fd < 0) {
                  *static_open_err = request_static_open_err_merge(*static_open_err, errno);
                  goto done;
                }
              }
            }

            // Sendfile path for 206.
            if (fd >= 0) {
              const char *buf_206 = NULL;
              size_t len_206 = 0;
              if (tx_build_headers(&c->tx,
                                   "206 Partial Content",
                                   ctype,
                                   (size_t)rr.length,
                                   /*body=*/NULL,
                                   /*body_send_len=*/0,
                                   keep,
                                   /*drain_after_headers=*/0,
                                   extra_206,
                                   &buf_206,
                                   &len_206)
                  == 0) {
                struct tx_next_io out206 = {0};
                (void)tx_begin_headers(&c->tx,
                                       RK_206,
                                       buf_206,
                                       len_206,
                                       keep,
                                       /*drain_after_headers=*/0,
                                       &out206);
                if (c->tx.file_fd >= 0) {
                  close(c->tx.file_fd);
                  c->tx.file_fd = -1;
                }
                c->tx.file_fd = fd;
                (void)tx_begin_sendfile(&c->tx, (off_t)rr.start, (size_t)rr.length);
                fd = -1;
                return 1;
              }
            }
            goto done;
          }
          // If-Range mismatch: serve normal 200.
        }
        // Invalid or multi-range: serve normal 200.
      }
    }

    struct request_static_serve_plan static_serve = request_build_static_serve_plan(c, fsz);
    int attempt_sendfile = 0;

    const unsigned dyn_comp_max =
      (vh && (vh->vf_present & VF_COMP_DYN_MAX)) ? vh->comp_dynamic_max_bytes : (1u << 20);
    const unsigned dyn_comp_min =
      (vh && (vh->vf_present & VF_COMP_DYN_MIN)) ? vh->comp_dynamic_min_bytes : 256u;
    // Encodings we can actually produce dynamically in this build.
    static const unsigned dyn_supported =
      COMP_ENC_GZIP
#ifdef HAVE_BROTLI
      | COMP_ENC_BROTLI
#endif
      ;
    int dyn_eligible =
      serving_enc == 0
      && vh && (vh->features & CFG_FEAT_COMPRESSION)
      && (vh->vf_present & VF_COMP_DYNAMIC) && vh->comp_dynamic
      && compress_mime_is_compressible(ctype)
      && c->h1.method == HTTP_GET
      && (accepted & dyn_supported) != 0
      && fsz >= (size_t)dyn_comp_min
      && fsz <= (size_t)dyn_comp_max;

    enum request_static_serve_mode mode = static_serve.mode;
    if (c->tls_enabled && mode == REQUEST_STATIC_SERVE_SENDFILE) {
      mode = REQUEST_STATIC_SERVE_BUFFERED;
    }
    // Dynamic compression requires reading into userspace; force buffered.
    if (dyn_eligible && mode == REQUEST_STATIC_SERVE_SENDFILE) {
      mode = REQUEST_STATIC_SERVE_BUFFERED;
    }

    switch (mode) {
    case REQUEST_STATIC_SERVE_HEAD:
      close(fd);
      fd = -1;
      if (static_serve_tx_set_dynamic_response_ex(c, "200 OK", ctype, fsz, NULL, 0, keep,
                                                   validator_hdrs) == 0) {
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
          if (dyn_eligible) {
            const int level =
              (vh && (vh->vf_present & VF_COMP_DYN_LEVEL)) ? (int)vh->comp_dynamic_effort : 1;
#ifdef HAVE_BROTLI
            static const unsigned try_enc[] = {COMP_ENC_BROTLI, COMP_ENC_GZIP, 0};
#else
            static const unsigned try_enc[] = {COMP_ENC_GZIP, 0};
#endif
            for (int i = 0; try_enc[i] && !serving_enc; i++) {
              if (!(accepted & try_enc[i])) {
                continue;
              }
              void *comp_buf = NULL;
              size_t comp_len = 0;
              enum compress_result cr;
#ifdef HAVE_BROTLI
              if (try_enc[i] == COMP_ENC_BROTLI) {
                // effort maps 1:1 to brotli quality (valid range 0-11).
                int q = (level <= 11) ? level : 4;
                cr = compress_brotli(file_buf, fsz, &comp_buf, &comp_len, q);
              } else
#endif
              {
                cr = compress_gzip(file_buf, fsz, &comp_buf, &comp_len, level);
              }
              if (cr == COMPRESS_OK) {
                free(file_buf);
                file_buf = (char *)comp_buf;
                fsz = comp_len;
                serving_enc = try_enc[i];
              }
            }
          }

          // Build the extra-headers string, prepending Content-Encoding / Vary
          // for dynamically compressed responses (validator_hdrs was assembled
          // with serving_enc == 0 and does not include them).
          const char *resp_extra = validator_hdrs;
          // Sized to always hold CE line (~30 bytes) + full validator_hdrs buffer.
          char dyn_extra[sizeof(validator_hdrs) + 64];
          if (serving_enc && !orig_serving_enc) {
            // validator_hdrs was built with serving_enc==0; for a compressible
            // MIME type it already contains "Vary: Accept-Encoding".  We only
            // need to prepend Content-Encoding — do not add a second Vary.
            int n = snprintf(dyn_extra, sizeof(dyn_extra),
                             "Content-Encoding: %s\r\n%s",
                             compress_enc_name(serving_enc), validator_hdrs);
            if (n > 0 && (size_t)n < sizeof(dyn_extra)) {
              resp_extra = dyn_extra;
            } else {
              // Unreachable: buffer is sized to always fit.  If somehow
              // hit, abort this response rather than serve compressed
              // bytes without Content-Encoding.
              free(file_buf);
              break;
            }
          }

#if ENABLE_ITEST_ECHO
          c->tx.itest_static_mode = "buffered";
#endif
          if (static_serve_tx_set_dynamic_response_ex(c, "200 OK", ctype, fsz, file_buf, fsz, keep,
                                                       resp_extra) == 0) {
            free(file_buf);
            return 1;
          }
        }

        free(file_buf);
      }

      // TLS cannot fall back to kernel sendfile.
      if (c->tls_enabled) {
        goto done;
      }
      // Non-TLS can reopen and continue with sendfile.
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
      if (static_serve_tx_set_dynamic_response_ex(c, "200 OK", ctype, fsz, NULL, 0, keep,
                                                   validator_hdrs) == 0) {
        if (c->tx.file_fd >= 0) {
          close(c->tx.file_fd);
          c->tx.file_fd = -1;
        }
        if (fsz > 0) {
          c->tx.file_fd = fd;
          (void)tx_begin_sendfile(&c->tx, 0, fsz);
          fd = -1;
        } else {
          close(fd);
          fd = -1;
        }
        return 1;
      }
    }
  }

done:
  if (fd >= 0) {
    close(fd);
  }
  return 0;
}
