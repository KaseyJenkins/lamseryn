#pragma once

#include <stddef.h>
#include <stdlib.h>

#ifdef CONFIG_INI_TEST_HOOKS
void *test_config_ini_alloc_candidate(size_t size);
void *test_config_ini_alloc_parse_ctx(size_t nmemb, size_t size);
void *test_config_ini_alloc_route_parse(size_t nmemb, size_t size);
void *test_config_ini_alloc_route_resolved(size_t nmemb, size_t size);
void *test_config_ini_alloc_route_index(size_t nmemb, size_t size);
void *test_config_ini_alloc_vhost_route_list(size_t nmemb, size_t size);

static inline void *config_ini_alloc_candidate(size_t size) {
  return test_config_ini_alloc_candidate(size);
}

static inline void *config_ini_alloc_parse_ctx(size_t nmemb, size_t size) {
  return test_config_ini_alloc_parse_ctx(nmemb, size);
}

static inline void *config_ini_alloc_route_parse(size_t nmemb, size_t size) {
  return test_config_ini_alloc_route_parse(nmemb, size);
}

static inline void *config_ini_alloc_route_resolved(size_t nmemb, size_t size) {
  return test_config_ini_alloc_route_resolved(nmemb, size);
}

static inline void *config_ini_alloc_route_index(size_t nmemb, size_t size) {
  return test_config_ini_alloc_route_index(nmemb, size);
}

static inline void *config_ini_alloc_vhost_route_list(size_t nmemb, size_t size) {
  return test_config_ini_alloc_vhost_route_list(nmemb, size);
}
#else
static inline void *config_ini_alloc_candidate(size_t size) {
  return malloc(size);
}

static inline void *config_ini_alloc_parse_ctx(size_t nmemb, size_t size) {
  return calloc(nmemb, size);
}

static inline void *config_ini_alloc_route_parse(size_t nmemb, size_t size) {
  return calloc(nmemb, size);
}

static inline void *config_ini_alloc_route_resolved(size_t nmemb, size_t size) {
  return calloc(nmemb, size);
}

static inline void *config_ini_alloc_route_index(size_t nmemb, size_t size) {
  return calloc(nmemb, size);
}

static inline void *config_ini_alloc_vhost_route_list(size_t nmemb, size_t size) {
  return calloc(nmemb, size);
}
#endif