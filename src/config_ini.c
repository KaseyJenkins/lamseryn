#include "ini.h"
#include "include/types.h"
#include "include/logger.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdbool.h>
#include <fcntl.h>
#include <ctype.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netdb.h>
#include <unistd.h>
#include <errno.h>

#include "include/net_utils.h"

#define VHOST_NAME_MAX 64
#define INIH_MAX_SECTION 50
#define INIH_SECTION_LIMIT ((size_t)(INIH_MAX_SECTION - 1))

static char ini_err_reason[256] = {0};
static int ini_fatal = 0;

static int parse_bool(const char *s, bool *out) {
  if (!s || !out) {
    return 0;
  }
  if (!strcasecmp(s, "true") || !strcmp(s, "1") || !strcasecmp(s, "yes") || !strcasecmp(s, "on")) {
    *out = true;
    return 1;
  }
  if (!strcasecmp(s, "false") || !strcmp(s, "0") || !strcasecmp(s, "no") || !strcasecmp(s, "off")) {
    *out = false;
    return 1;
  }
  return 0;
}

static int parse_u16(const char *s, unsigned short *out) {
  if (!s || !out) {
    return 0;
  }
  char *end = NULL;
  long v = strtol(s, &end, 10);
  if (end == s || v < 0 || v > 65535) {
    return 0;
  }
  *out = (unsigned short)v;
  return 1;
}

static struct vhost_t *ensure_vhost(struct config_t *cfg, const char *section) {
  if (ini_fatal) {
    return NULL;
  }
  if (!cfg || !section) {
    return NULL;
  }
  if (strncasecmp(section, "vhost", 5) != 0) {
    return NULL;
  }

  const char *p = section + 5;
  while (*p == ' ' || *p == ':' || *p == '.') {
    p++;
  }
  const char *name = (*p ? p : "default");

  // Enforce parser section limits before copying vhost names.
  size_t consumed = (size_t)(p - section);
  size_t namelen = strlen(name);

  if (consumed >= INIH_SECTION_LIMIT) {
    ini_fatal = 1;
    snprintf(
      ini_err_reason,
      sizeof(ini_err_reason),
      "vhost section too long or truncated by parser (prefix len=%zu >= %zu). Keep names short",
      consumed,
      INIH_SECTION_LIMIT);
    LOGE(LOGC_CORE, "%s", ini_err_reason);
    return NULL;
  }

  size_t cap_parser = INIH_SECTION_LIMIT - consumed;
  if (namelen >= cap_parser) {
    ini_fatal = 1;
    snprintf(
      ini_err_reason,
      sizeof(ini_err_reason),
      "vhost section too long or truncated by parser (len=%zu >= %zu). Keep names < %zu chars",
      consumed + namelen,
      INIH_SECTION_LIMIT,
      cap_parser);
    LOGE(LOGC_CORE, "%s", ini_err_reason);
    return NULL;
  }

  if (namelen >= VHOST_NAME_MAX) {
    ini_fatal = 1;
    snprintf(ini_err_reason,
             sizeof(ini_err_reason),
             "vhost name exceeds buffer (%zu >= %d)",
             namelen,
             VHOST_NAME_MAX);
    LOGE(LOGC_CORE, "%s", ini_err_reason);
    return NULL;
  }

  for (int i = 0; i < cfg->vhost_count; ++i) {
    if (strcasecmp(cfg->vhosts[i].name, name) == 0) {
      return &cfg->vhosts[i];
    }
  }

  if (cfg->vhost_count >= 32) {
    ini_fatal = 1;
    snprintf(ini_err_reason, sizeof(ini_err_reason), "too many vhosts (max 32)");
    LOGE(LOGC_CORE, "%s", ini_err_reason);
    return NULL;
  }

  struct vhost_t *vh = &cfg->vhosts[cfg->vhost_count++];
  memset(vh, 0, sizeof(*vh));
  snprintf(vh->name, sizeof(vh->name), "%s", name);
  vh->max_header_fields = (cfg && (cfg->g.present & GF_DEFAULT_MAX_HDR_FIELDS))
                            ? (uint16_t)cfg->g.default_max_header_fields
                            : 100;
  vh->docroot_fd = -1;
  return vh;
}

static int parse_u32(const char *s, unsigned *out) {
  if (!s || !out) {
    return 0;
  }
  char *end = NULL;
  unsigned long v = strtoul(s, &end, 10);
  if (end == s) {
    return 0;
  }
  if (v > 0xfffffffful) {
    return 0;
  }
  *out = (unsigned)v;
  return 1;
}

static void parse_and_set_global_tls_bool(struct config_t *cfg, const char *value) {
  bool b;
  if (!cfg) {
    return;
  }
  if (!parse_bool(value, &b)) {
    return;
  }
  cfg->g.tls_enabled = b ? 1u : 0u;
  cfg->g.present |= GF_TLS_ENABLED;
}

static int parse_level_str(const char *s, int *out) {
  if (!out) {
    return 0;
  }
  if (!s || !*s) {
    return 0;
  }
  if (!strcasecmp(s, "error")) {
    *out = LOG_ERROR;
    return 1;
  }
  if (!strcasecmp(s, "warn") || !strcasecmp(s, "warning")) {
    *out = LOG_WARN;
    return 1;
  }
  if (!strcasecmp(s, "info")) {
    *out = LOG_INFO;
    return 1;
  }
  if (!strcasecmp(s, "debug")) {
    *out = LOG_DEBUG;
    return 1;
  }
  if (!strcasecmp(s, "trace")) {
    *out = LOG_TRACE;
    return 1;
  }
  return 0;
}

static unsigned parse_categories_str(const char *s, int *ok) {
  if (ok) {
    *ok = 0;
  }
  if (!s || !*s) {
    return 0;
  }
  unsigned m = 0;
  char buf[256];
  size_t n = strnlen(s, sizeof(buf) - 1);
  memcpy(buf, s, n);
  buf[n] = 0;
  for (char *tok = strtok(buf, ","); tok; tok = strtok(NULL, ",")) {
    while (*tok && isspace((unsigned char)*tok)) {
      tok++;
    }
    if (!strcasecmp(tok, "all")) {
      m |= LOGC_ALL;
    } else if (!strcasecmp(tok, "core")) {
      m |= LOGC_CORE;
    } else if (!strcasecmp(tok, "accept")) {
      m |= LOGC_ACCEPT;
    } else if (!strcasecmp(tok, "io")) {
      m |= LOGC_IO;
    } else if (!strcasecmp(tok, "http")) {
      m |= LOGC_HTTP;
    } else if (!strcasecmp(tok, "buf")) {
      m |= LOGC_BUF;
    } else if (!strcasecmp(tok, "timer")) {
      m |= LOGC_TIMER;
    } else if (!strcasecmp(tok, "poll")) {
      m |= LOGC_POLL;
    }
  }
  if (m) {
    if (ok) {
      *ok = 1;
    }
  }
  return m;
}

static int is_linklocal_v6(const char *addr) {
  if (!addr || !*addr) {
    return 0;
  }
  struct in6_addr a6;
  if (inet_pton(AF_INET6, addr, &a6) != 1) {
    return 0;
  }
  return IN6_IS_ADDR_LINKLOCAL(&a6) ? 1 : 0;
}

static void config_warn_vhost_ambiguity(const struct config_t *cfg) {
  if (!cfg) {
    return;
  }

  // Warn on link-local binds without a zone id.
  for (int i = 0; i < cfg->vhost_count; ++i) {
    const struct vhost_t *vh = &cfg->vhosts[i];
    if (!vh->bind[0] || is_wildcard_bind(vh->bind)) {
      continue;
    }
    char addr[INET6_ADDRSTRLEN] = {0};
    char zone[64] = {0};
    int has_zone = split_zone_id(vh->bind, addr, sizeof(addr), zone, sizeof(zone));
    if (addr[0] && is_linklocal_v6(addr) && !has_zone) {
      LOGW(LOGC_CORE, "link-local bind without zone id: %s (ambiguous)", vh->bind);
    }
  }

  // Warn on multiple wildcard vhosts per port.
  uint16_t warned_ports[32];
  int warned_count = 0;
  for (int i = 0; i < cfg->vhost_count; ++i) {
    const struct vhost_t *vh = &cfg->vhosts[i];
    if (!is_wildcard_bind(vh->bind) || vh->port == 0) {
      continue;
    }

    int seen = 0;
    for (int k = 0; k < warned_count; ++k) {
      if (warned_ports[k] == vh->port) {
        seen = 1;
        break;
      }
    }
    if (seen) {
      continue;
    }

    int count = 0;
    char names[256];
    names[0] = '\0';
    size_t off = 0;
    for (int j = 0; j < cfg->vhost_count; ++j) {
      const struct vhost_t *vj = &cfg->vhosts[j];
      if (vj->port != vh->port) {
        continue;
      }
      if (is_wildcard_bind(vj->bind)) {
        count++;
        size_t vlen = strnlen(vj->name, sizeof(vj->name));
        if (vlen > 0 && off + vlen + 2 < sizeof(names)) {
          if (off) {
            names[off++] = ',';
          }
          memcpy(names + off, vj->name, vlen);
          off += vlen;
          names[off] = '\0';
        }
      }
    }
    if (count > 1) {
      warned_ports[warned_count++] = vh->port;
      if (names[0] != '\0') {
        LOGW(LOGC_CORE,
             "multiple wildcard vhosts on port %u: %s (first-match wins)",
             (unsigned)vh->port,
             names);
      } else {
        LOGW(LOGC_CORE,
             "multiple wildcard vhosts on port %u (first-match wins)",
             (unsigned)vh->port);
      }
    }
  }
}

static int normalize_vhost_binds(struct config_t *cfg, char err[256]) {
  if (!cfg) {
    return -1;
  }

  char numeric[128];
  char ebuf[128];

  for (int i = 0; i < cfg->vhost_count; ++i) {
    struct vhost_t *vh = &cfg->vhosts[i];
    if (!vh->bind[0] || is_wildcard_bind(vh->bind)) {
      continue;
    }

    if (resolve_bind_host_numeric(vh->bind, numeric, sizeof(numeric), ebuf, sizeof(ebuf)) != 0) {
      snprintf(ini_err_reason,
               sizeof(ini_err_reason),
               "%s",
               ebuf[0] ? ebuf : "bind resolution failed");
      if (err) {
        snprintf(err, 256, "%s", ini_err_reason);
      }
      LOGE(LOGC_CORE, "%s", ini_err_reason);
      return -1;
    }

    snprintf(vh->bind, sizeof(vh->bind), "%s", numeric);
  }

  return 0;
}

static int on_kv(void *user, const char *section, const char *name, const char *value) {
  struct config_t *cfg = (struct config_t *)user;

  // Return 0 so inih reports line number for fatal errors.
  if (ini_fatal) {
    return 0;
  }

  if (!strcasecmp(section, "globals")) {
    if (!name) {
      return 1;
    }

    if (!strcasecmp(name, "log_level")) {
      int lvl = 0;
      if (parse_level_str(value, &lvl)) {
        cfg->g.log_level = lvl;
        cfg->g.present |= GF_LOG_LEVEL;
      }
      return 1;
    }
    if (!strcasecmp(name, "log_categories")) {
      int ok = 0;
      unsigned mask = parse_categories_str(value, &ok);
      if (ok) {
        cfg->g.log_categories = mask;
        cfg->g.present |= GF_LOG_CATEGORIES;
      }
      return 1;
    }
    if (!strcasecmp(name, "queue_depth")) {
      unsigned v;
      if (parse_u32(value, &v)) {
        cfg->g.queue_depth = v;
        cfg->g.present |= GF_QUEUE_DEPTH;
      }
      return 1;
    }
    if (!strcasecmp(name, "pre_accepts")) {
      unsigned v;
      if (parse_u32(value, &v)) {
        cfg->g.pre_accepts = v;
        cfg->g.present |= GF_PRE_ACCEPTS;
      }
      return 1;
    }
    if (!strcasecmp(name, "workers")) {
      unsigned v;
      if (!parse_u32(value, &v) || v == 0u) {
        ini_fatal = 1;
        snprintf(ini_err_reason,
                 sizeof(ini_err_reason),
                 "invalid [globals].workers: '%s' (expected u32 > 0)",
                 value ? value : "(null)");
        LOGE(LOGC_CORE, "%s", ini_err_reason);
        return 0;
      }
      cfg->g.workers = v;
      cfg->g.present |= GF_WORKERS;
      return 1;
    }
    if (!strcasecmp(name, "initial_idle_timeout_ms")) {
      unsigned v;
      if (parse_u32(value, &v)) {
        cfg->g.initial_idle_timeout_ms = v;
        cfg->g.present |= GF_INITIAL_IDLE_TIMEOUT;
      }
      return 1;
    }
    if (!strcasecmp(name, "keepalive_idle_close_ms")) {
      unsigned v;
      if (parse_u32(value, &v)) {
        cfg->g.keepalive_idle_close_ms = v;
        cfg->g.present |= GF_KA_IDLE_CLOSE;
      }
      return 1;
    }
    if (!strcasecmp(name, "header_timeout_ms")) {
      unsigned v;
      if (parse_u32(value, &v)) {
        cfg->g.header_timeout_ms = v;
        cfg->g.present |= GF_HEADER_TIMEOUT;
      }
      return 1;
    }
    if (!strcasecmp(name, "body_timeout_ms")) {
      unsigned v;
      if (parse_u32(value, &v)) {
        cfg->g.body_timeout_ms = v;
        cfg->g.present |= GF_BODY_TIMEOUT;
      }
      return 1;
    }
    if (!strcasecmp(name, "write_timeout_ms")) {
      unsigned v;
      if (parse_u32(value, &v)) {
        cfg->g.write_timeout_ms = v;
        cfg->g.present |= GF_WRITE_TIMEOUT;
      }
      return 1;
    }
    if (!strcasecmp(name, "drain_timeout_ms")) {
      unsigned v;
      if (parse_u32(value, &v)) {
        cfg->g.drain_timeout_ms = v;
        cfg->g.present |= GF_DRAIN_TIMEOUT;
      }
      return 1;
    }
    if (!strcasecmp(name, "accept_backoff_ms")) {
      unsigned v;
      if (parse_u32(value, &v)) {
        cfg->g.accept_backoff_ms = v;
        cfg->g.present |= GF_ACCEPT_BACKOFF_MS;
      }
      return 1;
    }
    if (!strcasecmp(name, "shutdown_grace_ms")) {
      unsigned v;
      if (!parse_u32(value, &v) || v == 0u) {
        ini_fatal = 1;
        snprintf(ini_err_reason,
                 sizeof(ini_err_reason),
                 "invalid [globals].shutdown_grace_ms: '%s' (expected u32 > 0)",
                 value ? value : "(null)");
        LOGE(LOGC_CORE, "%s", ini_err_reason);
        return 0;
      }
      cfg->g.shutdown_grace_ms = v;
      cfg->g.present |= GF_SHUTDOWN_GRACE_MS;
      return 1;
    }
    if (!strcasecmp(name, "default_max_header_fields")) {
      unsigned v;
      if (parse_u32(value, &v) && v <= 65535u) {
        cfg->g.default_max_header_fields = v;
        cfg->g.present |= GF_DEFAULT_MAX_HDR_FIELDS;
      }
      return 1;
    }
    if (!strcasecmp(name, "tls")) {
      parse_and_set_global_tls_bool(cfg, value);
      return 1;
    }
    if (!strcasecmp(name, "tls_cert_file")) {
      snprintf(cfg->g.tls_cert_file, sizeof(cfg->g.tls_cert_file), "%s", value ? value : "");
      cfg->g.present |= GF_TLS_CERT_FILE;
      return 1;
    }
    if (!strcasecmp(name, "tls_key_file")) {
      snprintf(cfg->g.tls_key_file, sizeof(cfg->g.tls_key_file), "%s", value ? value : "");
      cfg->g.present |= GF_TLS_KEY_FILE;
      return 1;
    }
    if (!strcasecmp(name, "tls_min_version")) {
      snprintf(cfg->g.tls_min_version, sizeof(cfg->g.tls_min_version), "%s", value ? value : "");
      cfg->g.present |= GF_TLS_MIN_VERSION;
      return 1;
    }
    if (!strcasecmp(name, "tls_ciphers")) {
      snprintf(cfg->g.tls_ciphers, sizeof(cfg->g.tls_ciphers), "%s", value ? value : "");
      cfg->g.present |= GF_TLS_CIPHERS;
      return 1;
    }
    if (!strcasecmp(name, "tls_ciphersuites")) {
      snprintf(cfg->g.tls_ciphersuites, sizeof(cfg->g.tls_ciphersuites), "%s", value ? value : "");
      cfg->g.present |= GF_TLS_CIPHERSUITES;
      return 1;
    }
    if (!strcasecmp(name, "tls_session_tickets")) {
      bool b;
      if (parse_bool(value, &b)) {
        cfg->g.tls_session_tickets = b ? 1u : 0u;
        cfg->g.present |= GF_TLS_SESSION_TICKETS;
      }
      return 1;
    }
    if (!strcasecmp(name, "tls_session_cache")) {
      bool b;
      if (parse_bool(value, &b)) {
        cfg->g.tls_session_cache = b ? 1u : 0u;
        cfg->g.present |= GF_TLS_SESSION_CACHE;
      }
      return 1;
    }
    if (!strcasecmp(name, "wake_pipe_mode")) {
      if (value && !strcasecmp(value, "shared")) {
        cfg->g.wake_pipe_mode = 0u;
        cfg->g.present |= GF_WAKE_PIPE_MODE;
      } else if (value && !strcasecmp(value, "per-worker")) {
        cfg->g.wake_pipe_mode = 1u;
        cfg->g.present |= GF_WAKE_PIPE_MODE;
      } else {
        LOGW(LOGC_CORE,
             "invalid [globals].wake_pipe_mode '%s' (expected shared|per-worker)",
             value ? value : "(null)");
      }
      return 1;
    }
    if (!strcasecmp(name, "access_log_enabled")) {
      bool b;
      if (parse_bool(value, &b)) {
        cfg->g.access_log_enabled = b ? 1u : 0u;
        cfg->g.present |= GF_ACCESS_LOG_ENABLED;
      }
      return 1;
    }
    if (!strcasecmp(name, "access_log_path")) {
      snprintf(cfg->g.access_log_path, sizeof(cfg->g.access_log_path), "%s", value ? value : "");
      cfg->g.present |= GF_ACCESS_LOG_PATH;
      return 1;
    }
    if (!strcasecmp(name, "access_log_format")) {
      if (value && !strcasecmp(value, "text")) {
        snprintf(cfg->g.access_log_format, sizeof(cfg->g.access_log_format), "%s", value);
        cfg->g.present |= GF_ACCESS_LOG_FORMAT;
      } else {
        LOGW(LOGC_CORE,
             "invalid [globals].access_log_format '%s' (expected text)",
             value ? value : "(null)");
      }
      return 1;
    }
    if (!strcasecmp(name, "access_log_sample")) {
      unsigned v;
      if (!parse_u32(value, &v) || v == 0u) {
        ini_fatal = 1;
        snprintf(ini_err_reason,
                 sizeof(ini_err_reason),
                 "invalid [globals].access_log_sample: '%s' (expected u32 > 0)",
                 value ? value : "(null)");
        LOGE(LOGC_CORE, "%s", ini_err_reason);
        return 0;
      }
      cfg->g.access_log_sample = v;
      cfg->g.present |= GF_ACCESS_LOG_SAMPLE;
      return 1;
    }
    if (!strcasecmp(name, "access_log_min_status")) {
      unsigned v;
      if (!parse_u32(value, &v) || v < 100u || v > 599u) {
        ini_fatal = 1;
        snprintf(ini_err_reason,
                 sizeof(ini_err_reason),
                 "invalid [globals].access_log_min_status: '%s' (expected 100..599)",
                 value ? value : "(null)");
        LOGE(LOGC_CORE, "%s", ini_err_reason);
        return 0;
      }
      cfg->g.access_log_min_status = v;
      cfg->g.present |= GF_ACCESS_LOG_MIN_STATUS;
      return 1;
    }
    return 1;
  }

  struct vhost_t *vh = ensure_vhost(cfg, section);

  if (ini_fatal) {
    return 0;
  }
  if (!vh) {
    return 1;
  }

  if (!strcasecmp(name, "bind")) {
    snprintf(vh->bind, sizeof(vh->bind), "%s", value ? value : "");
    return 1;
  }
  if (!strcasecmp(name, "port")) {
    unsigned short v;
    return parse_u16(value, &v) ? (vh->port = v, 1) : 0;
  }
  if (!strcasecmp(name, "docroot")) {
    snprintf(vh->docroot, sizeof(vh->docroot), "%s", value ? value : "");
    return 1;
  }
  if (!strcasecmp(name, "max_header_fields")) {
    unsigned short v;
    return parse_u16(value, &v) ? (vh->max_header_fields = v, 1) : 0;
  }
  if (!strcasecmp(name, "tls")) {
    bool b;
    if (!parse_bool(value, &b)) {
      LOGW(LOGC_CORE, "invalid boolean for vhost key '%s': %s", name, value ? value : "(null)");
      return 1;
    }
    vh->tls_enabled = b ? 1u : 0u;
    vh->tls_enabled_set = 1u;
    return 1;
  }
  if (!strcasecmp(name, "tls_cert_file")) {
    snprintf(vh->tls_cert_file, sizeof(vh->tls_cert_file), "%s", value ? value : "");
    return 1;
  }
  if (!strcasecmp(name, "tls_key_file")) {
    snprintf(vh->tls_key_file, sizeof(vh->tls_key_file), "%s", value ? value : "");
    return 1;
  }
  if (!strcasecmp(name, "tls_min_version")) {
    snprintf(vh->tls_min_version, sizeof(vh->tls_min_version), "%s", value ? value : "");
    return 1;
  }
  if (!strcasecmp(name, "tls_ciphers")) {
    snprintf(vh->tls_ciphers, sizeof(vh->tls_ciphers), "%s", value ? value : "");
    return 1;
  }
  if (!strcasecmp(name, "tls_ciphersuites")) {
    snprintf(vh->tls_ciphersuites, sizeof(vh->tls_ciphersuites), "%s", value ? value : "");
    return 1;
  }
  if (!strcasecmp(name, "tls_session_tickets")) {
    bool b;
    if (!parse_bool(value, &b)) {
      LOGW(LOGC_CORE, "invalid boolean for vhost key '%s': %s", name, value ? value : "(null)");
      return 1;
    }
    vh->tls_session_tickets = b ? 1u : 0u;
    vh->tls_session_tickets_set = 1u;
    return 1;
  }
  if (!strcasecmp(name, "tls_session_cache")) {
    bool b;
    if (!parse_bool(value, &b)) {
      LOGW(LOGC_CORE, "invalid boolean for vhost key '%s': %s", name, value ? value : "(null)");
      return 1;
    }
    vh->tls_session_cache = b ? 1u : 0u;
    vh->tls_session_cache_set = 1u;
    return 1;
  }

  uint64_t bit = 0;
  if (!strcasecmp(name, "static")) {
    bit = CFG_FEAT_STATIC;
  } else if (!strcasecmp(name, "compression")) {
    bit = CFG_FEAT_COMPRESSION;
  } else if (!strcasecmp(name, "range")) {
    bit = CFG_FEAT_RANGE;
  } else if (!strcasecmp(name, "conditional")) {
    bit = CFG_FEAT_CONDITIONAL;
  } else if (!strcasecmp(name, "auth")) {
    bit = CFG_FEAT_AUTH;
  }

  if (bit) {
    bool b;
    if (!parse_bool(value, &b)) {
      LOGW(LOGC_CORE, "invalid boolean for vhost key '%s': %s", name, value ? value : "(null)");
      return 1;
    }
    if (b) {
      vh->features |= bit;
    } else {
      vh->features &= ~bit;
    }
    return 1;
  }

  if (!strcasecmp(name, "compression_dynamic")) {
    bool b;
    if (!parse_bool(value, &b)) {
      LOGW(LOGC_CORE, "invalid boolean for vhost key '%s': %s", name, value ? value : "(null)");
      return 1;
    }
    vh->comp_dynamic = b ? 1u : 0u;
    vh->vf_present |= VF_COMP_DYNAMIC;
    return 1;
  }
  if (!strcasecmp(name, "compression_dynamic_max_bytes")) {
    unsigned v;
    if (!parse_u32(value, &v)) {
      LOGW(LOGC_CORE, "invalid uint for vhost key '%s': %s", name, value ? value : "(null)");
      return 1;
    }
    vh->comp_dynamic_max_bytes = v;
    vh->vf_present |= VF_COMP_DYN_MAX;
    return 1;
  }
  if (!strcasecmp(name, "compression_dynamic_min_bytes")) {
    unsigned v;
    if (!parse_u32(value, &v)) {
      LOGW(LOGC_CORE, "invalid uint for vhost key '%s': %s", name, value ? value : "(null)");
      return 1;
    }
    vh->comp_dynamic_min_bytes = v;
    vh->vf_present |= VF_COMP_DYN_MIN;
    return 1;
  }
  if (!strcasecmp(name, "compression_dynamic_effort")) {
    unsigned v;
    if (!parse_u32(value, &v) || v < 1 || v > 9) {
      LOGW(LOGC_CORE,
           "invalid compression_dynamic_effort '%s': expected 1-9",
           value ? value : "(null)");
      return 1;
    }
    vh->comp_dynamic_effort = v;
    vh->vf_present |= VF_COMP_DYN_LEVEL;
    return 1;
  }

  LOGW(LOGC_CORE, "unknown vhost key '%s'", name ? name : "(null)");
  return 1;
}

int config_set_defaults(struct config_t *cfg) {
  if (!cfg) {
    return -1;
  }
  memset(cfg, 0, sizeof(*cfg));
  cfg->g.access_log_sample = 1u;
  cfg->g.access_log_min_status = 100u;
  snprintf(cfg->g.access_log_format, sizeof(cfg->g.access_log_format), "%s", "text");
  return 0;
}

int config_load_ini(const char *path, struct config_t *cfg, char err[256]) {
  if (err) {
    err[0] = '\0';
  }
  ini_err_reason[0] = '\0';
  ini_fatal = 0;

  if (!path || !cfg) {
    return -1;
  }
  int rc = ini_parse(path, on_kv, cfg);

  if (ini_fatal || rc != 0) {
    if (err) {
      if (ini_err_reason[0]) {
        if (rc != 0) {
          snprintf(err, 256, "%s (line=%d)", ini_err_reason, rc);
        } else {
          snprintf(err, 256, "%s", ini_err_reason);
        }
      } else {
        snprintf(err, 256, "ini_parse failed (rc=%d)", rc);
      }
    }
    return -1;
  }

  if (normalize_vhost_binds(cfg, err) != 0) {
    return -1;
  }

  config_warn_vhost_ambiguity(cfg);

  // Validate effective TLS configuration (globals + per-vhost overrides).
  for (int i = 0; i < cfg->vhost_count; ++i) {
    struct vhost_t *vh = &cfg->vhosts[i];

    int tls_enabled = 0;
    if (vh->tls_enabled_set) {
      tls_enabled = vh->tls_enabled ? 1 : 0;
    } else if (cfg->g.present & GF_TLS_ENABLED) {
      tls_enabled = cfg->g.tls_enabled ? 1 : 0;
    }

    if (tls_enabled) {
      const char *cert = vh->tls_cert_file[0]
                           ? vh->tls_cert_file
                           : ((cfg->g.present & GF_TLS_CERT_FILE) ? cfg->g.tls_cert_file : "");
      const char *key = vh->tls_key_file[0]
                          ? vh->tls_key_file
                          : ((cfg->g.present & GF_TLS_KEY_FILE) ? cfg->g.tls_key_file : "");
      if (!cert[0] || !key[0]) {
        if (err) {
          snprintf(err,
                   256,
                   "vhost '%s': tls=true requires tls_cert_file and tls_key_file",
                   vh->name);
        }
        return -1;
      }
    }

    if ((vh->vf_present & VF_COMP_DYN_MIN) && (vh->vf_present & VF_COMP_DYN_MAX)
        && vh->comp_dynamic_min_bytes > vh->comp_dynamic_max_bytes) {
      LOGW(LOGC_CORE,
           "vhost '%s': compression_dynamic_min_bytes (%u) > compression_dynamic_max_bytes (%u); "
           "dynamic compression will never fire",
           vh->name, vh->comp_dynamic_min_bytes, vh->comp_dynamic_max_bytes);
    }
  }

  for (int i = 0; i < cfg->vhost_count; ++i) {
    if (cfg->vhosts[i].docroot[0]) {
      int fd = open(cfg->vhosts[i].docroot, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
      if (fd >= 0) {
        cfg->vhosts[i].docroot_fd = fd;
      } else {
        LOGW(LOGC_CORE, "docroot open failed: %s: %s", cfg->vhosts[i].docroot, strerror(errno));
      }
    }
  }
  return 0;
}
