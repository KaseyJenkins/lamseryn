#pragma once

#include <stdint.h>

struct security_header_entry {
  char name[64];
  char value[256];
};

struct security_headers_policy {
  uint8_t enabled;
  uint8_t enabled_set;
  struct security_header_entry headers[16];
  unsigned header_count;
};

struct cors_policy {
  uint8_t enabled;
  uint8_t enabled_set;
  char allow_origin[256];
  uint8_t allow_origin_set;
  char allow_methods[128];
  uint8_t allow_methods_set;
  char allow_headers[256];
  uint8_t allow_headers_set;
  uint8_t allow_credentials;
  uint8_t allow_credentials_set;
  unsigned max_age_seconds;
  uint8_t max_age_seconds_set;
};
