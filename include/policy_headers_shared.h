#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "include/conn.h"

enum {
  POLICY_SHARED_HDR_MAX = 64,
  POLICY_SHARED_HDR_LINE_MAX = 384,
  POLICY_SHARED_HDR_NAME_MAX = 64,
};

struct policy_shared_header_ctx {
  const char *lines[POLICY_SHARED_HDR_MAX];
  char owned_lines[POLICY_SHARED_HDR_MAX][POLICY_SHARED_HDR_LINE_MAX];
  char names[POLICY_SHARED_HDR_MAX][POLICY_SHARED_HDR_NAME_MAX];
  unsigned count;
  int overflow;
};

static inline int policy_shared_path_prefix_matches(const char *path,
                                                    const char *prefix,
                                                    size_t prefix_len) {
  if (!path || !prefix || prefix_len == 0) {
    return 0;
  }
  if (prefix_len == 1 && prefix[0] == '/') {
    return path[0] == '/';
  }
  if (strncmp(path, prefix, prefix_len) != 0) {
    return 0;
  }

  char next = path[prefix_len];
  if (next == '\0') {
    return 1;
  }
  if (prefix[prefix_len - 1] == '/') {
    return 1;
  }
  return next == '/';
}

static inline const struct route_policy_rule *policy_shared_resolve_route_rule(
  const struct conn *c,
  const struct vhost_t *vh) {
  if (!c || !vh || !vh->route_rules || vh->route_rule_count == 0
      || !c->h1.path_norm || c->h1.path_norm_len == 0) {
    return NULL;
  }

  for (uint16_t i = 0; i < vh->route_rule_count; ++i) {
    const struct route_policy_rule *rr = vh->route_rules[i];
    if (!rr || rr->path_prefix_len == 0) {
      continue;
    }
    if (policy_shared_path_prefix_matches(c->h1.path_norm,
                                          rr->path_prefix,
                                          (size_t)rr->path_prefix_len)) {
      return rr;
    }
  }
  return NULL;
}

static inline void policy_shared_resolve_effective_cors_for_route(
  const struct vhost_t *vh,
  const struct route_policy_rule *rr,
  struct cors_policy *out) {
  if (!out) {
    return;
  }
  memset(out, 0, sizeof(*out));
  if (!vh) {
    return;
  }

  if (vh->cors) {
    *out = *vh->cors;
  }
  if (!rr) {
    return;
  }

  if (rr->cors.enabled_set) {
    out->enabled = rr->cors.enabled;
    out->enabled_set = rr->cors.enabled_set;
  }
  if (rr->cors.allow_origin_set) {
    snprintf(out->allow_origin, sizeof(out->allow_origin), "%s", rr->cors.allow_origin);
    out->allow_origin_set = 1u;
  }
  if (rr->cors.allow_methods_set) {
    snprintf(out->allow_methods, sizeof(out->allow_methods), "%s", rr->cors.allow_methods);
    out->allow_methods_set = 1u;
  }
  if (rr->cors.allow_headers_set) {
    snprintf(out->allow_headers, sizeof(out->allow_headers), "%s", rr->cors.allow_headers);
    out->allow_headers_set = 1u;
  }
  if (rr->cors.allow_credentials_set) {
    out->allow_credentials = rr->cors.allow_credentials;
    out->allow_credentials_set = 1u;
  }
  if (rr->cors.max_age_seconds_set) {
    out->max_age_seconds = rr->cors.max_age_seconds;
    out->max_age_seconds_set = 1u;
  }
}

static inline void policy_shared_resolve_effective_cors(const struct conn *c,
                                                        const struct vhost_t *vh,
                                                        struct cors_policy *out) {
  const struct route_policy_rule *rr = policy_shared_resolve_route_rule(c, vh);
  policy_shared_resolve_effective_cors_for_route(vh, rr, out);
}

static inline int policy_shared_header_find(const struct policy_shared_header_ctx *ctx,
                                            const char *name) {
  if (!ctx || !name || !name[0]) {
    return -1;
  }
  for (unsigned i = 0; i < ctx->count; ++i) {
    if (strcasecmp(ctx->names[i], name) == 0) {
      return (int)i;
    }
  }
  return -1;
}

static inline int policy_shared_header_extract_name(const char *line,
                                                    char out[POLICY_SHARED_HDR_NAME_MAX]) {
  if (!line || !out) {
    return -1;
  }
  const char *colon = strchr(line, ':');
  if (!colon || colon == line) {
    return -1;
  }
  size_t len = (size_t)(colon - line);
  if (len == 0 || len >= POLICY_SHARED_HDR_NAME_MAX) {
    return -1;
  }
  memcpy(out, line, len);
  out[len] = '\0';
  return 0;
}

static inline void policy_shared_header_upsert(struct policy_shared_header_ctx *ctx,
                                               const char *name,
                                               const char *value) {
  if (!ctx || !name || !name[0] || !value) {
    return;
  }

  int idx = policy_shared_header_find(ctx, name);
  unsigned out_idx = (idx >= 0) ? (unsigned)idx : ctx->count;
  if (idx < 0) {
    if (ctx->count >= POLICY_SHARED_HDR_MAX) {
      ctx->overflow = 1;
      return;
    }
    snprintf(ctx->names[out_idx], sizeof(ctx->names[out_idx]), "%s", name);
    ctx->count++;
  }

  int n = snprintf(ctx->owned_lines[out_idx],
                   sizeof(ctx->owned_lines[out_idx]),
                   "%s: %s\r\n",
                   name,
                   value);
  if (n <= 0 || (size_t)n >= sizeof(ctx->owned_lines[out_idx])) {
    if (idx < 0 && ctx->count > 0) {
      ctx->count--;
    }
    ctx->overflow = 1;
    return;
  }

  ctx->lines[out_idx] = ctx->owned_lines[out_idx];
}

static inline void policy_shared_append_custom_if_unique(struct policy_shared_header_ctx *ctx,
                                                         const char *line) {
  if (!ctx || !line || !line[0]) {
    return;
  }
  if (ctx->count >= POLICY_SHARED_HDR_MAX) {
    ctx->overflow = 1;
    return;
  }

  char name[POLICY_SHARED_HDR_NAME_MAX];
  if (policy_shared_header_extract_name(line, name) != 0) {
    return;
  }
  if (policy_shared_header_find(ctx, name) >= 0) {
    return;
  }

  snprintf(ctx->names[ctx->count], sizeof(ctx->names[ctx->count]), "%s", name);
  ctx->lines[ctx->count] = line;
  ctx->count++;
}

static inline void policy_shared_collect_headers(const struct conn *c,
                                                 const struct vhost_t *vh,
                                                 struct policy_shared_header_ctx *ctx) {
  if (!ctx) {
    return;
  }
  memset(ctx, 0, sizeof(*ctx));
  if (!vh) {
    return;
  }

  const struct route_policy_rule *rr = policy_shared_resolve_route_rule(c, vh);

  int inherit_security = 1;
  if (rr && rr->inherit_security_headers_set) {
    inherit_security = rr->inherit_security_headers ? 1 : 0;
  }

  if (inherit_security && vh->security_headers && vh->security_headers->enabled) {
    for (unsigned i = 0; i < vh->security_headers->header_count; ++i) {
      const struct security_header_entry *h = &vh->security_headers->headers[i];
      if (h->name[0] && h->value[0]) {
        policy_shared_header_upsert(ctx, h->name, h->value);
      }
    }
  }

  if (rr) {
    if (rr->security_headers.enabled_set && !rr->security_headers.enabled) {
      ctx->count = 0;
    } else if (!rr->security_headers.enabled_set || rr->security_headers.enabled) {
      for (unsigned i = 0; i < rr->security_headers.header_count; ++i) {
        const struct security_header_entry *h = &rr->security_headers.headers[i];
        if (h->name[0] && h->value[0]) {
          policy_shared_header_upsert(ctx, h->name, h->value);
        }
      }
    }
  }

  struct cors_policy cors;
  policy_shared_resolve_effective_cors(c, vh, &cors);
  if (cors.enabled) {
    if (cors.allow_origin[0]) {
      policy_shared_header_upsert(ctx, "Access-Control-Allow-Origin", cors.allow_origin);
      if (strcmp(cors.allow_origin, "*") != 0) {
        policy_shared_header_upsert(ctx, "Vary", "Origin");
      }
    }
    if (cors.allow_methods[0]) {
      policy_shared_header_upsert(ctx, "Access-Control-Allow-Methods", cors.allow_methods);
    }
    if (cors.allow_headers[0]) {
      policy_shared_header_upsert(ctx, "Access-Control-Allow-Headers", cors.allow_headers);
    }
    if (cors.allow_credentials) {
      policy_shared_header_upsert(ctx, "Access-Control-Allow-Credentials", "true");
    }
    if (cors.max_age_seconds_set && cors.max_age_seconds > 0) {
      char max_age[32];
      int n = snprintf(max_age, sizeof(max_age), "%u", cors.max_age_seconds);
      if (n > 0 && (size_t)n < sizeof(max_age)) {
        policy_shared_header_upsert(ctx, "Access-Control-Max-Age", max_age);
      }
    }
  }

  for (unsigned i = 0; i < vh->custom_headers_count; ++i) {
    policy_shared_append_custom_if_unique(ctx, vh->custom_headers[i]);
  }
}