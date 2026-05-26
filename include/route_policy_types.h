#pragma once

#include <stdint.h>

#include "policy_types.h"

enum {
  ROUTE_PATH_PREFIX_MAX = 256,
  CFG_MAX_ROUTES = 256,
};

struct route_policy_rule {
  char path_prefix[ROUTE_PATH_PREFIX_MAX];
  uint16_t path_prefix_len;

  uint8_t inherit_security_headers;
  uint8_t inherit_security_headers_set;

  struct security_headers_policy security_headers;
  struct cors_policy cors;
};
