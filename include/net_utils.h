#pragma once

#include <stddef.h>
#include <string.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#include <stdio.h>

static inline int is_wildcard_bind(const char *s) {
  if (!s || !*s) {
    return 1;
  }
  return (strcmp(s, "0.0.0.0") == 0 || strcmp(s, "::") == 0 || strcmp(s, "*") == 0);
}

static inline void strip_zone_id(const char *in, char *out, size_t outsz) {
  if (!out || outsz == 0) {
    return;
  }
  if (!in) {
    out[0] = '\0';
    return;
  }
  size_t i = 0;
  for (; in[i] && in[i] != '%' && i + 1 < outsz; ++i) {
    out[i] = in[i];
  }
  out[i] = '\0';
}

static inline int split_zone_id(const char *in,
                                char *addr,
                                size_t addrsz,
                                char *zone,
                                size_t zonesz) {
  if (!addr || addrsz == 0) {
    return 0;
  }
  if (!in) {
    addr[0] = '\0';
    if (zone && zonesz) {
      zone[0] = '\0';
    }
    return 0;
  }
  size_t i = 0;
  for (; in[i] && in[i] != '%' && i + 1 < addrsz; ++i) {
    addr[i] = in[i];
  }
  addr[i] = '\0';

  if (!zone || zonesz == 0) {
    return 0;
  }
  if (in[i] != '%') {
    zone[0] = '\0';
    return 0;
  }

  size_t j = 0;
  i++; // skip '%'
  for (; in[i] && j + 1 < zonesz; ++i, ++j) {
    zone[j] = in[i];
  }
  zone[j] = '\0';
  return (zone[0] != '\0');
}

// Resolve a bind host to a numeric literal (preserves wildcard and already-numeric inputs).
// Returns 0 on success; on failure fills errbuf (if provided) and returns -1.
static inline int resolve_bind_host_numeric(const char *in,
                                            char *out,
                                            size_t outsz,
                                            char *errbuf,
                                            size_t errsz) {
  if (!out || outsz == 0) {
    return -1;
  }
  if (errbuf && errsz) {
    errbuf[0] = '\0';
  }

  // Empty or wildcard: keep as-is
  if (!in || !*in || is_wildcard_bind(in)) {
    out[0] = '\0';
    if (in && *in) {
      snprintf(out, outsz, "%s", in);
    }
    return 0;
  }

  char addr[128];
  char zone[128];
  split_zone_id(in, addr, sizeof(addr), zone, sizeof(zone));

  // Already numeric?
  if (inet_pton(AF_INET, addr, &(struct in_addr){0}) == 1
      || inet_pton(AF_INET6, addr, &(struct in6_addr){0}) == 1) {
    // Copy addr first, then append "%zone" if present while keeping bounds.
    if (snprintf(out, outsz, "%s", addr) >= (int)outsz) {
      if (errbuf && errsz) {
        snprintf(errbuf, errsz, "bind address too long");
      }
      return -1;
    }
    if (zone[0]) {
      size_t used = strnlen(out, outsz);
      if (used + 1 < outsz) { // room for '%'
        out[used++] = '%';
        out[used] = '\0';
        size_t avail = (outsz > used) ? outsz - used - 1 : 0;
        size_t zlen = strnlen(zone, sizeof(zone));
        size_t copy = (zlen < avail) ? zlen : avail;
        if (copy > 0) {
          memcpy(out + used, zone, copy);
          out[used + copy] = '\0';
        }
      }
    }
    return 0;
  }

  struct addrinfo hints, *res = NULL;
  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_flags = AI_NUMERICSERV | AI_ADDRCONFIG;

  int rc = getaddrinfo(in, "0", &hints, &res);
  if (rc != 0) {
    if (errbuf && errsz) {
      snprintf(errbuf, errsz, "getaddrinfo failed for %s: %s", in, gai_strerror(rc));
    }
    if (res) {
      freeaddrinfo(res);
    }
    return -1;
  }

  int ok = -1;
  for (struct addrinfo *rp = res; rp; rp = rp->ai_next) {
    char host[NI_MAXHOST];
    if (getnameinfo(rp->ai_addr, rp->ai_addrlen, host, sizeof(host), NULL, 0, NI_NUMERICHOST)
        == 0) {
      snprintf(out, outsz, "%.*s", (int)(outsz > 0 ? outsz - 1 : 0), host);
      ok = 0;
      break;
    }
  }
  if (res) {
    freeaddrinfo(res);
  }

  if (ok != 0 && errbuf && errsz) {
    snprintf(errbuf, errsz, "failed to format numeric host for %s", in);
  }

  return ok;
}
