#include "ini.h"
#include "include/types.h"
#include "include/auth.h"
#include "include/logger.h"
#include "include/policy_headers_shared.h"

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
#include "config_ini_alloc_hooks.h"

#define VHOST_NAME_MAX 64
#define ROUTE_CFG_NAME_MAX 64
#define CFG_MAX_VHOSTS 32
#define INIH_MAX_SECTION 50
#define INIH_SECTION_LIMIT ((size_t)(INIH_MAX_SECTION - 1))

static char ini_err_reason[256] = {0};
static int ini_fatal = 0;

struct route_cfg_entry {
  char name[ROUTE_CFG_NAME_MAX];
  char vhost_name[64];
  uint16_t decl_ordinal;
};

struct config_parse_ctx {
  struct config_t *cfg;
  struct route_policy_rule *route_rules;
  struct route_cfg_entry *routes;
  int route_count;
  int route_cap;
};

struct vhost_route_index_scratch {
  uint16_t route_indices[CFG_MAX_ROUTES];
  uint16_t route_count;
};

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

static struct security_headers_policy *ensure_vhost_security_headers(struct vhost_t *vh) {
  if (!vh) {
    return NULL;
  }
  if (vh->security_headers) {
    return vh->security_headers;
  }

  vh->security_headers = config_ini_alloc_route_parse(1, sizeof(*vh->security_headers));
  return vh->security_headers;
}

static struct cors_policy *ensure_vhost_cors(struct vhost_t *vh) {
  if (!vh) {
    return NULL;
  }
  if (vh->cors) {
    return vh->cors;
  }

  vh->cors = config_ini_alloc_route_parse(1, sizeof(*vh->cors));
  return vh->cors;
}

static int ensure_route_capacity(struct config_parse_ctx *pctx, int need_count) {
  if (!pctx || need_count <= 0) {
    return -1;
  }
  if (need_count <= pctx->route_cap) {
    return 0;
  }

  int new_cap = pctx->route_cap > 0 ? pctx->route_cap : 8;
  while (new_cap < need_count && new_cap < CFG_MAX_ROUTES) {
    new_cap *= 2;
  }
  if (new_cap > CFG_MAX_ROUTES) {
    new_cap = CFG_MAX_ROUTES;
  }
  if (new_cap < need_count) {
    return -1;
  }

  struct route_cfg_entry *new_routes =
    config_ini_alloc_route_parse((size_t)new_cap, sizeof(*new_routes));
  struct route_policy_rule *new_rules =
    config_ini_alloc_route_parse((size_t)new_cap, sizeof(*new_rules));
  if (!new_routes || !new_rules) {
    free(new_routes);
    free(new_rules);
    return -1;
  }

  if (pctx->route_count > 0) {
    memcpy(new_routes,
           pctx->routes,
           (size_t)pctx->route_count * sizeof(*new_routes));
    memcpy(new_rules,
           pctx->route_rules,
           (size_t)pctx->route_count * sizeof(*new_rules));
  }

  free(pctx->routes);
  free(pctx->route_rules);
  pctx->routes = new_routes;
  pctx->route_rules = new_rules;
  pctx->route_cap = new_cap;
  return 0;
}

static struct route_cfg_entry *ensure_route(struct config_parse_ctx *pctx, const char *section) {
  if (ini_fatal) {
    return NULL;
  }
  if (!pctx || !section) {
    return NULL;
  }
  if (strncasecmp(section, "route", 5) != 0) {
    return NULL;
  }

  const char *p = section + 5;
  while (*p == ' ' || *p == ':' || *p == '.') {
    p++;
  }
  const char *name = (*p ? p : "default");

  size_t consumed = (size_t)(p - section);
  size_t namelen = strlen(name);

  if (consumed >= INIH_SECTION_LIMIT) {
    ini_fatal = 1;
    snprintf(
      ini_err_reason,
      sizeof(ini_err_reason),
      "route section too long or truncated by parser (prefix len=%zu >= %zu). Keep names short",
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
      "route section too long or truncated by parser (len=%zu >= %zu). Keep names < %zu chars",
      consumed + namelen,
      INIH_SECTION_LIMIT,
      cap_parser);
    LOGE(LOGC_CORE, "%s", ini_err_reason);
    return NULL;
  }

  if (namelen >= ROUTE_CFG_NAME_MAX) {
    ini_fatal = 1;
    snprintf(ini_err_reason,
             sizeof(ini_err_reason),
             "route name exceeds buffer (%zu >= %d)",
             namelen,
             ROUTE_CFG_NAME_MAX);
    LOGE(LOGC_CORE, "%s", ini_err_reason);
    return NULL;
  }

  for (int i = 0; i < pctx->route_count; ++i) {
    if (strcasecmp(pctx->routes[i].name, name) == 0) {
      return &pctx->routes[i];
    }
  }

  if (pctx->route_count >= CFG_MAX_ROUTES) {
    ini_fatal = 1;
    snprintf(ini_err_reason, sizeof(ini_err_reason), "too many route rules (max %d)", CFG_MAX_ROUTES);
    LOGE(LOGC_CORE, "%s", ini_err_reason);
    return NULL;
  }

  if (ensure_route_capacity(pctx, pctx->route_count + 1) != 0) {
    ini_fatal = 1;
    snprintf(ini_err_reason,
             sizeof(ini_err_reason),
             "out of memory while allocating route parse buffer");
    LOGE(LOGC_CORE, "%s", ini_err_reason);
    return NULL;
  }

  int route_idx = pctx->route_count;
  struct route_cfg_entry *re = &pctx->routes[route_idx];
  struct route_policy_rule *rr = &pctx->route_rules[route_idx];
  memset(re, 0, sizeof(*re));
  memset(rr, 0, sizeof(*rr));
  re->decl_ordinal = (uint16_t)pctx->route_count;
  rr->inherit_security_headers = 1u;
  snprintf(re->name, sizeof(re->name), "%s", name);
  pctx->route_count++;
  return re;
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

static int config_find_vhost_index_by_name(const struct config_t *cfg, const char *name) {
  if (!cfg || !name || !name[0]) {
    return -1;
  }
  for (int i = 0; i < cfg->vhost_count; ++i) {
    if (strcasecmp(cfg->vhosts[i].name, name) == 0) {
      return i;
    }
  }
  return -1;
}

static int route_has_cors_policy(const struct route_policy_rule *rr) {
  if (!rr) {
    return 0;
  }
  return rr->cors.enabled
         || rr->cors.enabled_set
         || rr->cors.allow_origin_set
         || rr->cors.allow_methods_set
         || rr->cors.allow_headers_set
         || rr->cors.allow_credentials_set
         || rr->cors.max_age_seconds_set;
}

static void sort_vhost_route_index(const struct config_t *cfg,
                                   const struct config_parse_ctx *pctx,
                                   struct vhost_route_index_scratch route_index[CFG_MAX_VHOSTS],
                                   int vhost_idx) {
  if (!cfg || !pctx || !pctx->route_rules || !pctx->routes || !route_index || vhost_idx < 0
      || vhost_idx >= CFG_MAX_VHOSTS) {
    return;
  }

  struct vhost_route_index_scratch *idx = &route_index[vhost_idx];
  uint16_t n = idx->route_count;
  if (n > (uint16_t)CFG_MAX_ROUTES) {
    n = (uint16_t)CFG_MAX_ROUTES;
  }
  if (n <= 1) {
    return;
  }

  for (uint16_t i = 1; i < n; ++i) {
    uint16_t key = idx->route_indices[i];
    const struct route_cfg_entry *rk = &pctx->routes[key];
    const struct route_policy_rule *rrk = &pctx->route_rules[key];
    uint16_t j = i;
    while (j > 0) {
      uint16_t prev = idx->route_indices[j - 1];
      const struct route_cfg_entry *rp = &pctx->routes[prev];
      const struct route_policy_rule *rrp = &pctx->route_rules[prev];

      int swap = 0;
      if (rrp->path_prefix_len < rrk->path_prefix_len) {
        swap = 1;
      } else if (rrp->path_prefix_len == rrk->path_prefix_len
                 && rp->decl_ordinal > rk->decl_ordinal) {
        swap = 1;
      }
      if (!swap) {
        break;
      }
      idx->route_indices[j] = idx->route_indices[j - 1];
      --j;
    }
    idx->route_indices[j] = key;
  }

  for (uint16_t i = 1; i < n; ++i) {
    const struct route_cfg_entry *a = &pctx->routes[idx->route_indices[i - 1]];
    const struct route_cfg_entry *b = &pctx->routes[idx->route_indices[i]];
    const struct route_policy_rule *rra = &pctx->route_rules[idx->route_indices[i - 1]];
    const struct route_policy_rule *rrb = &pctx->route_rules[idx->route_indices[i]];
    if (rra->path_prefix_len == rrb->path_prefix_len) {
      LOGW(LOGC_CORE,
           "vhost '%s': route prefix length tie (%u) between '%s' and '%s'; declaration order wins",
           cfg->vhosts[vhost_idx].name,
           (unsigned)rra->path_prefix_len,
           a->name,
           b->name);
    }
  }
}

static int config_resolve_routes(struct config_t *cfg,
                                 const struct config_parse_ctx *pctx,
                                 char err[256]) {
  if (!cfg || !pctx) {
    return -1;
  }
  if (pctx->route_count > 0 && (!pctx->route_rules || !pctx->routes)) {
    return -1;
  }

  uint8_t vhost_route_has_cors[CFG_MAX_VHOSTS] = {0};
  struct route_policy_rule *resolved_routes = NULL;
  struct vhost_route_index_scratch *route_index =
    config_ini_alloc_route_index((size_t)CFG_MAX_VHOSTS, sizeof(*route_index));
  int rc = -1;
  if (!route_index) {
    if (err) {
      snprintf(err, 256, "out of memory while resolving route index");
    }
    goto out;
  }

  if (pctx->route_count > 0) {
    resolved_routes = config_ini_alloc_route_resolved((size_t)pctx->route_count,
                                                      sizeof(*resolved_routes));
    if (!resolved_routes) {
      if (err) {
        snprintf(err, 256, "out of memory while allocating resolved route storage");
      }
      goto out;
    }
  }

  for (int i = 0; i < pctx->route_count; ++i) {
    const struct route_cfg_entry *src = &pctx->routes[i];
    struct route_policy_rule *rr = &resolved_routes[i];
    const char *route_name = src->name[0] ? src->name : "(unnamed)";

    *rr = pctx->route_rules[i];

    if (!src->vhost_name[0]) {
      if (err) {
        snprintf(err,
                 256,
                 "route '%s': missing required key 'vhost'",
                 route_name);
      }
      LOGE(LOGC_CORE,
           "route '%s': missing required key 'vhost'",
           route_name);
       goto out;
    }

    if (!rr->path_prefix[0]) {
      if (err) {
        snprintf(err,
                 256,
                 "route '%s': missing required key 'path_prefix'",
                 route_name);
      }
      LOGE(LOGC_CORE,
           "route '%s': missing required key 'path_prefix'",
           route_name);
       goto out;
    }

    if (rr->path_prefix[0] != '/') {
      if (err) {
        snprintf(err,
                 256,
                 "route '%s': path_prefix must start with '/'",
                 route_name);
      }
      LOGE(LOGC_CORE,
           "route '%s': path_prefix must start with '/'",
           route_name);
       goto out;
    }

    rr->path_prefix_len = (uint16_t)strlen(rr->path_prefix);

    int vhost_idx = config_find_vhost_index_by_name(cfg, src->vhost_name);
    if (vhost_idx < 0) {
      if (err) {
        snprintf(err,
                 256,
                 "route '%s': unknown vhost '%s'",
                 route_name,
                 src->vhost_name);
      }
      LOGE(LOGC_CORE,
           "route '%s': unknown vhost '%s'",
           route_name,
           src->vhost_name);
       goto out;
    }

    struct vhost_route_index_scratch *idx = &route_index[vhost_idx];
    if (idx->route_count >= CFG_MAX_ROUTES) {
      if (err) {
        snprintf(err,
                 256,
                 "vhost '%s': too many route entries",
                 cfg->vhosts[vhost_idx].name);
      }
      LOGE(LOGC_CORE,
           "vhost '%s': too many route entries",
           cfg->vhosts[vhost_idx].name);
       goto out;
    }

    idx->route_indices[idx->route_count++] = (uint16_t)i;

    if (route_has_cors_policy(rr)) {
      vhost_route_has_cors[vhost_idx] = 1u;
    }

    struct cors_policy effective_cors;
    policy_shared_resolve_effective_cors_for_route(&cfg->vhosts[vhost_idx], rr, &effective_cors);
    if (effective_cors.enabled
        && effective_cors.allow_credentials
        && strcmp(effective_cors.allow_origin, "*") == 0) {
      if (err) {
        snprintf(err,
                 256,
                 "route '%s': cors_allow_origin='*' cannot be combined with cors_allow_credentials=true",
                 route_name);
      }
      LOGE(LOGC_CORE,
           "route '%s': cors_allow_origin='*' cannot be combined with cors_allow_credentials=true",
           route_name);
      goto out;
    }
  }

  for (int i = 0; i < cfg->vhost_count; ++i) {
    sort_vhost_route_index(cfg, pctx, route_index, i);
  }

  cfg->route_rules = resolved_routes;
  resolved_routes = NULL;
  cfg->route_rule_count = pctx->route_count;

  for (int i = 0; i < cfg->vhost_count; ++i) {
    const struct vhost_route_index_scratch *idx = &route_index[i];
    cfg->vhosts[i].route_rule_count = idx->route_count;
    cfg->vhosts[i].route_rule_cap = idx->route_count;
    cfg->vhosts[i].route_rules = NULL;

    if (idx->route_count > 0) {
      cfg->vhosts[i].route_rules = config_ini_alloc_vhost_route_list(
        (size_t)idx->route_count,
        sizeof(*cfg->vhosts[i].route_rules));
      if (!cfg->vhosts[i].route_rules) {
        if (err) {
          snprintf(err,
                   256,
                   "out of memory while resolving route list for vhost '%s'",
                   cfg->vhosts[i].name);
        }
        goto out;
      }
    }

    for (uint16_t j = 0; j < idx->route_count; ++j) {
      cfg->vhosts[i].route_rules[j] = &cfg->route_rules[idx->route_indices[j]];
    }
    if (vhost_route_has_cors[i]) {
      cfg->vhosts[i].features |= CFG_FEAT_CORS;
    }
  }

  rc = 0;

out:
  free(resolved_routes);
  free(route_index);
  return rc;
}

static int http_header_name_char_is_valid(unsigned char ch) {
  if (ch <= 32 || ch == 127) {
    return 0;
  }
  if (ch == '(' || ch == ')' || ch == '<' || ch == '>' || ch == '@'
      || ch == ',' || ch == ';' || ch == '\\' || ch == '"'
      || ch == '/' || ch == '[' || ch == ']' || ch == '?'
      || ch == '=' || ch == '{' || ch == '}') {
    return 0;
  }
  return 1;
}

static int parse_header_name_value(const char *line,
                                   char *name,
                                   size_t name_cap,
                                   char *value,
                                   size_t value_cap) {
  if (!line || !name || !value || name_cap == 0 || value_cap == 0) {
    return 0;
  }

  for (const char *p = line; *p; ++p) {
    if (*p == '\r' || *p == '\n') {
      return 0;
    }
  }

  const char *colon = strchr(line, ':');
  if (!colon) {
    return 0;
  }

  const char *n0 = line;
  while (*n0 == ' ' || *n0 == '\t') {
    n0++;
  }
  const char *n1 = colon;
  while (n1 > n0 && (n1[-1] == ' ' || n1[-1] == '\t')) {
    n1--;
  }
  size_t nlen = (size_t)(n1 - n0);
  if (nlen == 0 || nlen >= name_cap) {
    return 0;
  }

  for (size_t i = 0; i < nlen; ++i) {
    if (!http_header_name_char_is_valid((unsigned char)n0[i])) {
      return 0;
    }
  }

  const char *v0 = colon + 1;
  while (*v0 == ' ' || *v0 == '\t') {
    v0++;
  }
  const char *v1 = line + strlen(line);
  while (v1 > v0 && (v1[-1] == ' ' || v1[-1] == '\t')) {
    v1--;
  }
  size_t vlen = (size_t)(v1 - v0);
  if (vlen >= value_cap) {
    return 0;
  }

  memcpy(name, n0, nlen);
  name[nlen] = '\0';
  memcpy(value, v0, vlen);
  value[vlen] = '\0';
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
  struct config_parse_ctx *pctx = (struct config_parse_ctx *)user;
  struct config_t *cfg = pctx ? pctx->cfg : NULL;

  if (!cfg) {
    return 0;
  }

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

  struct route_cfg_entry *re = ensure_route(pctx, section);
  struct route_policy_rule *rr = NULL;
  if (re) {
    ptrdiff_t route_idx = re - pctx->routes;
    if (route_idx >= 0 && route_idx < pctx->route_count) {
      rr = &pctx->route_rules[route_idx];
    }
  }

  if (ini_fatal) {
    return 0;
  }
  if (rr) {
    if (!name) {
      return 1;
    }

    if (!strcasecmp(name, "vhost")) {
      if (!value || !value[0]) {
        LOGW(LOGC_CORE, "empty route vhost value; ignored");
        return 1;
      }
      size_t vlen = strlen(value);
      if (vlen >= sizeof(re->vhost_name)) {
        LOGW(LOGC_CORE, "route vhost name too long; ignored");
        return 1;
      }
      memcpy(re->vhost_name, value, vlen + 1);
      return 1;
    }
    if (!strcasecmp(name, "path_prefix")) {
      if (!value || !value[0]) {
        LOGW(LOGC_CORE, "empty route path_prefix value; ignored");
        return 1;
      }
      size_t vlen = strlen(value);
      if (vlen >= sizeof(rr->path_prefix)) {
        LOGW(LOGC_CORE, "route path_prefix too long; ignored");
        return 1;
      }
      if (value[0] != '/') {
        LOGW(LOGC_CORE, "invalid route path_prefix '%s': must start with '/'", value);
        return 1;
      }
      memcpy(rr->path_prefix, value, vlen + 1);
      rr->path_prefix_len = (uint16_t)vlen;
      return 1;
    }
    if (!strcasecmp(name, "inherit_security_headers")) {
      bool b;
      if (!parse_bool(value, &b)) {
        LOGW(LOGC_CORE, "invalid boolean for route key '%s': %s", name, value ? value : "(null)");
        return 1;
      }
      rr->inherit_security_headers = b ? 1u : 0u;
      rr->inherit_security_headers_set = 1u;
      return 1;
    }
    if (!strcasecmp(name, "security_headers")) {
      bool b;
      if (!parse_bool(value, &b)) {
        LOGW(LOGC_CORE, "invalid boolean for route key '%s': %s", name, value ? value : "(null)");
        return 1;
      }
      rr->security_headers.enabled = b ? 1u : 0u;
      rr->security_headers.enabled_set = 1u;
      return 1;
    }
    if (!strcasecmp(name, "security_header_set")) {
      if (!value || !value[0]) {
        LOGW(LOGC_CORE, "empty security_header_set value; ignored");
        return 1;
      }
      if (rr->security_headers.header_count >= 16u) {
        LOGW(LOGC_CORE,
             "security_header_set: max 16 entries per route; '%s' ignored",
             value);
        return 1;
      }

      struct security_header_entry *dst = &rr->security_headers.headers[rr->security_headers.header_count];
      if (!parse_header_name_value(value,
                                   dst->name,
                                   sizeof(dst->name),
                                   dst->value,
                                   sizeof(dst->value))) {
        LOGW(LOGC_CORE,
             "invalid security_header_set '%s': expected 'Header-Name: value'",
             value);
        return 1;
      }

      rr->security_headers.header_count++;
      rr->security_headers.enabled = 1u;
      return 1;
    }
    if (!strcasecmp(name, "cors")) {
      bool b;
      if (!parse_bool(value, &b)) {
        LOGW(LOGC_CORE, "invalid boolean for route key '%s': %s", name, value ? value : "(null)");
        return 1;
      }
      rr->cors.enabled = b ? 1u : 0u;
      rr->cors.enabled_set = 1u;
      return 1;
    }
    if (!strcasecmp(name, "cors_allow_origin")) {
      if (!value || !value[0]) {
        LOGW(LOGC_CORE, "empty cors_allow_origin value; ignored");
        return 1;
      }
      size_t vlen = strlen(value);
      if (vlen >= sizeof(rr->cors.allow_origin)) {
        LOGW(LOGC_CORE, "cors_allow_origin too long; ignored");
        return 1;
      }
      memcpy(rr->cors.allow_origin, value, vlen + 1);
      rr->cors.allow_origin_set = 1u;
      return 1;
    }
    if (!strcasecmp(name, "cors_allow_methods")) {
      if (!value || !value[0]) {
        LOGW(LOGC_CORE, "empty cors_allow_methods value; ignored");
        return 1;
      }
      size_t vlen = strlen(value);
      if (vlen >= sizeof(rr->cors.allow_methods)) {
        LOGW(LOGC_CORE, "cors_allow_methods too long; ignored");
        return 1;
      }
      memcpy(rr->cors.allow_methods, value, vlen + 1);
      rr->cors.allow_methods_set = 1u;
      return 1;
    }
    if (!strcasecmp(name, "cors_allow_headers")) {
      if (!value || !value[0]) {
        LOGW(LOGC_CORE, "empty cors_allow_headers value; ignored");
        return 1;
      }
      size_t vlen = strlen(value);
      if (vlen >= sizeof(rr->cors.allow_headers)) {
        LOGW(LOGC_CORE, "cors_allow_headers too long; ignored");
        return 1;
      }
      memcpy(rr->cors.allow_headers, value, vlen + 1);
      rr->cors.allow_headers_set = 1u;
      return 1;
    }
    if (!strcasecmp(name, "cors_allow_credentials")) {
      bool b;
      if (!parse_bool(value, &b)) {
        LOGW(LOGC_CORE, "invalid boolean for route key '%s': %s", name, value ? value : "(null)");
        return 1;
      }
      rr->cors.allow_credentials = b ? 1u : 0u;
      rr->cors.allow_credentials_set = 1u;
      return 1;
    }
    if (!strcasecmp(name, "cors_max_age_seconds")) {
      unsigned v;
      if (!parse_u32(value, &v)) {
        LOGW(LOGC_CORE, "invalid uint for route key '%s': %s", name, value ? value : "(null)");
        return 1;
      }
      rr->cors.max_age_seconds = v;
      rr->cors.max_age_seconds_set = 1u;
      return 1;
    }

    LOGW(LOGC_CORE, "unknown route key '%s'", name ? name : "(null)");
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
  if (!strcasecmp(name, "index")) {
    if (!value || !value[0]) {
      LOGW(LOGC_CORE, "empty index value; ignored");
      return 1;
    }
    if (strchr(value, '/') || strcmp(value, ".") == 0
        || strcmp(value, "..") == 0) {
      LOGW(LOGC_CORE,
           "invalid index '%s': must be a plain filename", value);
      return 1;
    }
    size_t vlen = strlen(value);
    if (vlen >= sizeof(vh->index_file)) {
      LOGW(LOGC_CORE,
           "index filename too long (%zu bytes, max %zu); ignored",
           vlen, sizeof(vh->index_file) - 1);
      return 1;
    }
    memcpy(vh->index_file, value, vlen + 1);
    return 1;
  }
  if (!strcasecmp(name, "auth_basic_file")) {
    if (!value || !value[0]) {
      LOGW(LOGC_CORE, "empty auth_basic_file value; ignored");
      return 1;
    }
    size_t vlen = strlen(value);
    if (vlen >= sizeof(vh->auth_basic_file)) {
      LOGW(LOGC_CORE, "auth_basic_file path too long; ignored");
      return 1;
    }
    memcpy(vh->auth_basic_file, value, vlen + 1);
    return 1;
  }
  if (!strcasecmp(name, "auth_realm")) {
    if (!value || !value[0]) {
      LOGW(LOGC_CORE, "empty auth_realm value; ignored");
      return 1;
    }
    size_t vlen = strlen(value);
    if (vlen >= sizeof(vh->auth_realm)) {
      LOGW(LOGC_CORE, "auth_realm too long (max %zu); ignored", sizeof(vh->auth_realm) - 1);
      return 1;
    }
    /* Reject embedded quotes, backslashes, and control characters — these
     * appear verbatim in the WWW-Authenticate header value. */
    for (size_t k = 0; k < vlen; k++) {
      unsigned char ch = (unsigned char)value[k];
      if (ch < 0x20 || ch == '"' || ch == '\\') {
        LOGW(LOGC_CORE, "auth_realm contains invalid character; ignored");
        return 1;
      }
    }
    memcpy(vh->auth_realm, value, vlen + 1);
    return 1;
  }
  if (!strcasecmp(name, "security_headers")) {
    struct security_headers_policy *vsec = ensure_vhost_security_headers(vh);
    if (!vsec) {
      ini_fatal = 1;
      snprintf(ini_err_reason,
               sizeof(ini_err_reason),
               "out of memory while allocating vhost security policy");
      LOGE(LOGC_CORE, "%s", ini_err_reason);
      return 0;
    }
    bool b;
    if (!parse_bool(value, &b)) {
      LOGW(LOGC_CORE, "invalid boolean for vhost key '%s': %s", name, value ? value : "(null)");
      return 1;
    }
    vsec->enabled = b ? 1u : 0u;
    vsec->enabled_set = 1u;
    return 1;
  }
  if (!strcasecmp(name, "security_header_set")) {
    struct security_headers_policy *vsec = ensure_vhost_security_headers(vh);
    if (!vsec) {
      ini_fatal = 1;
      snprintf(ini_err_reason,
               sizeof(ini_err_reason),
               "out of memory while allocating vhost security policy");
      LOGE(LOGC_CORE, "%s", ini_err_reason);
      return 0;
    }
    if (!value || !value[0]) {
      LOGW(LOGC_CORE, "empty security_header_set value; ignored");
      return 1;
    }
    if (vsec->header_count >= 16u) {
      LOGW(LOGC_CORE,
           "security_header_set: max 16 entries per vhost; '%s' ignored",
           value);
      return 1;
    }

    struct security_header_entry *dst = &vsec->headers[vsec->header_count];
    if (!parse_header_name_value(value,
                                 dst->name,
                                 sizeof(dst->name),
                                 dst->value,
                                 sizeof(dst->value))) {
      LOGW(LOGC_CORE,
           "invalid security_header_set '%s': expected 'Header-Name: value'",
           value);
      return 1;
    }

    vsec->header_count++;
    vsec->enabled = 1u;
    return 1;
  }
  if (!strcasecmp(name, "cors")) {
    struct cors_policy *vcors = ensure_vhost_cors(vh);
    if (!vcors) {
      ini_fatal = 1;
      snprintf(ini_err_reason,
               sizeof(ini_err_reason),
               "out of memory while allocating vhost cors policy");
      LOGE(LOGC_CORE, "%s", ini_err_reason);
      return 0;
    }
    bool b;
    if (!parse_bool(value, &b)) {
      LOGW(LOGC_CORE, "invalid boolean for vhost key '%s': %s", name, value ? value : "(null)");
      return 1;
    }
    vcors->enabled = b ? 1u : 0u;
    vcors->enabled_set = 1u;
    vh->features |= CFG_FEAT_CORS;
    return 1;
  }
  if (!strcasecmp(name, "cors_allow_origin")) {
    struct cors_policy *vcors = ensure_vhost_cors(vh);
    if (!vcors) {
      ini_fatal = 1;
      snprintf(ini_err_reason,
               sizeof(ini_err_reason),
               "out of memory while allocating vhost cors policy");
      LOGE(LOGC_CORE, "%s", ini_err_reason);
      return 0;
    }
    if (!value || !value[0]) {
      LOGW(LOGC_CORE, "empty cors_allow_origin value; ignored");
      return 1;
    }
    size_t vlen = strlen(value);
    if (vlen >= sizeof(vcors->allow_origin)) {
      LOGW(LOGC_CORE, "cors_allow_origin too long; ignored");
      return 1;
    }
    memcpy(vcors->allow_origin, value, vlen + 1);
    vcors->allow_origin_set = 1u;
    vh->features |= CFG_FEAT_CORS;
    return 1;
  }
  if (!strcasecmp(name, "cors_allow_methods")) {
    struct cors_policy *vcors = ensure_vhost_cors(vh);
    if (!vcors) {
      ini_fatal = 1;
      snprintf(ini_err_reason,
               sizeof(ini_err_reason),
               "out of memory while allocating vhost cors policy");
      LOGE(LOGC_CORE, "%s", ini_err_reason);
      return 0;
    }
    if (!value || !value[0]) {
      LOGW(LOGC_CORE, "empty cors_allow_methods value; ignored");
      return 1;
    }
    size_t vlen = strlen(value);
    if (vlen >= sizeof(vcors->allow_methods)) {
      LOGW(LOGC_CORE, "cors_allow_methods too long; ignored");
      return 1;
    }
    memcpy(vcors->allow_methods, value, vlen + 1);
    vcors->allow_methods_set = 1u;
    vh->features |= CFG_FEAT_CORS;
    return 1;
  }
  if (!strcasecmp(name, "cors_allow_headers")) {
    struct cors_policy *vcors = ensure_vhost_cors(vh);
    if (!vcors) {
      ini_fatal = 1;
      snprintf(ini_err_reason,
               sizeof(ini_err_reason),
               "out of memory while allocating vhost cors policy");
      LOGE(LOGC_CORE, "%s", ini_err_reason);
      return 0;
    }
    if (!value || !value[0]) {
      LOGW(LOGC_CORE, "empty cors_allow_headers value; ignored");
      return 1;
    }
    size_t vlen = strlen(value);
    if (vlen >= sizeof(vcors->allow_headers)) {
      LOGW(LOGC_CORE, "cors_allow_headers too long; ignored");
      return 1;
    }
    memcpy(vcors->allow_headers, value, vlen + 1);
    vcors->allow_headers_set = 1u;
    vh->features |= CFG_FEAT_CORS;
    return 1;
  }
  if (!strcasecmp(name, "cors_allow_credentials")) {
    struct cors_policy *vcors = ensure_vhost_cors(vh);
    if (!vcors) {
      ini_fatal = 1;
      snprintf(ini_err_reason,
               sizeof(ini_err_reason),
               "out of memory while allocating vhost cors policy");
      LOGE(LOGC_CORE, "%s", ini_err_reason);
      return 0;
    }
    bool b;
    if (!parse_bool(value, &b)) {
      LOGW(LOGC_CORE, "invalid boolean for vhost key '%s': %s", name, value ? value : "(null)");
      return 1;
    }
    vcors->allow_credentials = b ? 1u : 0u;
    vcors->allow_credentials_set = 1u;
    vh->features |= CFG_FEAT_CORS;
    return 1;
  }
  if (!strcasecmp(name, "cors_max_age_seconds")) {
    struct cors_policy *vcors = ensure_vhost_cors(vh);
    if (!vcors) {
      ini_fatal = 1;
      snprintf(ini_err_reason,
               sizeof(ini_err_reason),
               "out of memory while allocating vhost cors policy");
      LOGE(LOGC_CORE, "%s", ini_err_reason);
      return 0;
    }
    unsigned v;
    if (!parse_u32(value, &v)) {
      LOGW(LOGC_CORE, "invalid uint for vhost key '%s': %s", name, value ? value : "(null)");
      return 1;
    }
    vcors->max_age_seconds = v;
    vcors->max_age_seconds_set = 1u;
    vh->features |= CFG_FEAT_CORS;
    return 1;
  }
  if (!strcasecmp(name, "header_set")) {
    if (!value || !value[0]) {
      LOGW(LOGC_CORE, "empty header_set value; ignored");
      return 1;
    }
    // Validate: must contain ':', no CR/LF injection.
    const char *colon = strchr(value, ':');
    if (!colon || colon == value) {
      LOGW(LOGC_CORE,
           "invalid header_set '%s': expected 'Header-Name: value'",
           value);
      return 1;
    }
    for (const char *p = value; *p; p++) {
      if (*p == '\r' || *p == '\n') {
        LOGW(LOGC_CORE,
             "invalid header_set: CR/LF in value; ignored");
        return 1;
      }
    }
    // Validate header name chars (token per RFC 7230).
    for (const char *p = value; p < colon; p++) {
      unsigned char ch = (unsigned char)*p;
      if (ch <= 32 || ch == 127 || ch == '(' || ch == ')' || ch == '<'
          || ch == '>' || ch == '@' || ch == ',' || ch == ';'
          || ch == '\\' || ch == '"' || ch == '/' || ch == '['
          || ch == ']' || ch == '?' || ch == '=' || ch == '{'
          || ch == '}') {
        LOGW(LOGC_CORE,
             "invalid header_set: bad character in header name; ignored");
        return 1;
      }
    }
    if (vh->custom_headers_count >= 16) {
      LOGW(LOGC_CORE,
           "header_set: max 16 custom headers per vhost; '%s' ignored",
           value);
      return 1;
    }
    size_t vlen = strlen(value);
    if (vlen > 1024) {
      LOGW(LOGC_CORE,
           "header_set value too long (%zu bytes, max 1024); ignored", vlen);
      return 1;
    }
    // Enforce total byte budget: all custom headers must fit in the
    // response emit buffer alongside built-in headers (~200 bytes).
    // Budget of 1536 bytes ensures they always fit in the 2048-byte buffer.
    size_t emit_len = vlen + 2; // this entry after \r\n appended
    for (unsigned i = 0; i < vh->custom_headers_count; i++) {
      emit_len += strlen(vh->custom_headers[i]);
    }
    if (emit_len > 1536) {
      LOGW(LOGC_CORE,
           "header_set: total custom header bytes (%zu) exceeds budget (1536); '%s' ignored",
           emit_len, value);
      return 1;
    }
    // Store as "Header-Name: value\r\n" for direct emission.
    size_t alloc_len = vlen + 2 + 1; // \r\n + NUL
    char *entry = malloc(alloc_len);
    if (!entry) {
      return 1;
    }
    memcpy(entry, value, vlen);
    entry[vlen] = '\r';
    entry[vlen + 1] = '\n';
    entry[vlen + 2] = '\0';
    vh->custom_headers[vh->custom_headers_count++] = entry;
    return 1;
  }

  LOGW(LOGC_CORE, "unknown vhost key '%s'", name ? name : "(null)");
  return 1;
}

static void cleanup_vhost_runtime_state(struct config_t *cfg) {
  if (!cfg) {
    return;
  }

  for (int i = 0; i < cfg->vhost_count; ++i) {
    struct vhost_t *vh = &cfg->vhosts[i];

    if (vh->docroot_fd >= 0) {
      close(vh->docroot_fd);
      vh->docroot_fd = -1;
    }

    for (unsigned h = 0; h < vh->custom_headers_count; ++h) {
      free(vh->custom_headers[h]);
      vh->custom_headers[h] = NULL;
    }
    vh->custom_headers_count = 0;

    auth_store_free(vh->auth_store);
    vh->auth_store = NULL;

    if (vh->route_rules) {
      for (uint16_t j = 0; j < vh->route_rule_count; ++j) {
        vh->route_rules[j] = NULL;
      }
      free(vh->route_rules);
      vh->route_rules = NULL;
    }
    vh->route_rule_count = 0;
    vh->route_rule_cap = 0;

    free(vh->security_headers);
    vh->security_headers = NULL;

    free(vh->cors);
    vh->cors = NULL;
  }

  free(cfg->route_rules);
  cfg->route_rules = NULL;
  cfg->route_rule_count = 0;
}

static void rebind_vhost_route_rule_ptrs_after_commit(struct config_t *dst,
                                                      const struct config_t *src) {
  if (!dst || !src) {
    return;
  }

  for (int i = 0; i < dst->vhost_count; ++i) {
    struct vhost_t *dst_vh = &dst->vhosts[i];
    const struct vhost_t *src_vh = &src->vhosts[i];

    if (dst_vh->route_rule_count > dst_vh->route_rule_cap) {
      dst_vh->route_rule_count = dst_vh->route_rule_cap;
    }
    if (dst_vh->route_rule_count > (uint16_t)dst->route_rule_count) {
      dst_vh->route_rule_count = (uint16_t)dst->route_rule_count;
    }
    if (!dst_vh->route_rules || !src_vh->route_rules) {
      dst_vh->route_rule_count = 0;
      continue;
    }

    for (uint16_t j = 0; j < dst_vh->route_rule_count; ++j) {
      const struct route_policy_rule *src_ptr = src_vh->route_rules[j];
      if (!src_ptr) {
        dst_vh->route_rules[j] = NULL;
        continue;
      }
      if (!src->route_rules || !dst->route_rules) {
        dst_vh->route_rules[j] = NULL;
        continue;
      }

      ptrdiff_t route_idx = src_ptr - src->route_rules;
      if (route_idx < 0 || route_idx >= (ptrdiff_t)dst->route_rule_count) {
        LOGE(LOGC_CORE,
             "vhost '%s': route pointer rebind failed at index %u",
             dst_vh->name,
             (unsigned)j);
        dst_vh->route_rules[j] = NULL;
        continue;
      }

      dst_vh->route_rules[j] = &dst->route_rules[route_idx];
    }
  }
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

  // Build candidate configuration off to the side and commit only on success.
  struct config_t *next = config_ini_alloc_candidate(sizeof(*next));
  if (!next) {
    if (err) {
      snprintf(err, 256, "out of memory while allocating candidate config");
    }
    return -1;
  }

  if (config_set_defaults(next) != 0) {
    if (err) {
      snprintf(err, 256, "failed to initialize candidate config");
    }
    free(next);
    return -1;
  }

  struct config_parse_ctx *pctx = config_ini_alloc_parse_ctx(1, sizeof(*pctx));
  if (!pctx) {
    if (err) {
      snprintf(err, 256, "out of memory while allocating parse context");
    }
    free(next);
    return -1;
  }
  pctx->cfg = next;

  int rc = ini_parse(path, on_kv, pctx);

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
    goto fail;
  }

  if (normalize_vhost_binds(next, err) != 0) {
    goto fail;
  }

  config_warn_vhost_ambiguity(next);

  // Validate effective TLS configuration (globals + per-vhost overrides).
  for (int i = 0; i < next->vhost_count; ++i) {
    struct vhost_t *vh = &next->vhosts[i];

    int tls_enabled = 0;
    if (vh->tls_enabled_set) {
      tls_enabled = vh->tls_enabled ? 1 : 0;
    } else if (next->g.present & GF_TLS_ENABLED) {
      tls_enabled = next->g.tls_enabled ? 1 : 0;
    }

    if (tls_enabled) {
      const char *cert = vh->tls_cert_file[0]
                           ? vh->tls_cert_file
                           : ((next->g.present & GF_TLS_CERT_FILE) ? next->g.tls_cert_file : "");
      const char *key = vh->tls_key_file[0]
                          ? vh->tls_key_file
                          : ((next->g.present & GF_TLS_KEY_FILE) ? next->g.tls_key_file : "");
      if (!cert[0] || !key[0]) {
        if (err) {
          snprintf(err,
                   256,
                   "vhost '%s': tls=true requires tls_cert_file and tls_key_file",
                   vh->name);
        }
        goto fail;
      }
    }

    if ((vh->vf_present & VF_COMP_DYN_MIN) && (vh->vf_present & VF_COMP_DYN_MAX)
        && vh->comp_dynamic_min_bytes > vh->comp_dynamic_max_bytes) {
      LOGW(LOGC_CORE,
           "vhost '%s': compression_dynamic_min_bytes (%u) > compression_dynamic_max_bytes (%u); "
           "dynamic compression will never fire",
           vh->name, vh->comp_dynamic_min_bytes, vh->comp_dynamic_max_bytes);
    }

    if (vh->cors
        && vh->cors->enabled
        && vh->cors->allow_credentials
        && strcmp(vh->cors->allow_origin, "*") == 0) {
      if (err) {
        snprintf(err,
                 256,
                 "vhost '%s': cors_allow_origin='*' cannot be combined with cors_allow_credentials=true",
                 vh->name);
      }
      LOGE(LOGC_CORE,
           "vhost '%s': cors_allow_origin='*' cannot be combined with cors_allow_credentials=true",
           vh->name);
      goto fail;
    }
  }

  if (config_resolve_routes(next, pctx, err) != 0) {
    goto fail;
  }

  for (int i = 0; i < next->vhost_count; ++i) {
    if (next->vhosts[i].docroot[0]) {
      int fd = open(next->vhosts[i].docroot, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
      if (fd >= 0) {
        next->vhosts[i].docroot_fd = fd;
      } else {
        LOGW(LOGC_CORE, "docroot open failed: %s: %s", next->vhosts[i].docroot, strerror(errno));
      }
    }
    if (next->vhosts[i].auth_basic_file[0]) {
      if (!(next->vhosts[i].features & CFG_FEAT_AUTH)) {
        LOGW(LOGC_CORE,
             "vhost '%s': auth_basic_file set but auth = false; file ignored",
             next->vhosts[i].name);
      } else {
        next->vhosts[i].auth_store = auth_store_load(next->vhosts[i].auth_basic_file);
        if (!next->vhosts[i].auth_store) {
          LOGE(LOGC_CORE,
               "vhost '%s': failed to load auth_basic_file '%s'",
               next->vhosts[i].name,
               next->vhosts[i].auth_basic_file);
          if (err) {
            snprintf(err,
                     256,
                     "vhost '%s': failed to load auth_basic_file '%s'",
                     next->vhosts[i].name,
                     next->vhosts[i].auth_basic_file);
          }
          goto fail;
        }
      }
    }
  }

  cleanup_vhost_runtime_state(cfg);
  *cfg = *next;
  rebind_vhost_route_rule_ptrs_after_commit(cfg, next);
  free(next);
  free(pctx->routes);
  free(pctx->route_rules);
  free(pctx);
  return 0;

fail:
  free(pctx->routes);
  free(pctx->route_rules);
  free(pctx);
  cleanup_vhost_runtime_state(next);
  free(next);
  return -1;
}
