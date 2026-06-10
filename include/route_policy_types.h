#pragma once

#include <stdint.h>

#include "policy_types.h"

enum {
  ROUTE_PATH_PREFIX_MAX = 256,
  CFG_MAX_ROUTES = 256,
};

enum route_auth_mode {
  ROUTE_AUTH_INHERIT = 0,
  ROUTE_AUTH_REQUIRE,
  ROUTE_AUTH_DISABLE,
};

struct route_policy_rule {
  char path_prefix[ROUTE_PATH_PREFIX_MAX];
  uint16_t path_prefix_len;

  enum route_auth_mode auth_mode;
  uint64_t max_body_bytes;
  uint8_t max_body_bytes_set;

  uint8_t inherit_security_headers;
  uint8_t inherit_security_headers_set;

  struct security_headers_policy security_headers;
  struct cors_policy cors;
};
