#include "../vendor/greatest_color.h"
#include "../vendor/greatest.h"

#include "include/config_ini.h"
#include "include/types.h"
#include "include/logger.h"
#include "include/net_utils.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>

// Stubs for auth functions: config_ini tests never set auth_basic_file so
// auth_store_load is never called, but the symbol must be resolvable.
#include "include/auth.h"
static struct auth_store *g_auth_store_load_result = NULL;
static unsigned g_auth_store_load_calls = 0;
static char g_auth_store_load_last_path[PATH_MAX];

static void reset_auth_store_stub(void) {
  g_auth_store_load_result = NULL;
  g_auth_store_load_calls = 0;
  g_auth_store_load_last_path[0] = '\0';
}

struct auth_store *auth_store_load(const char *path) {
  g_auth_store_load_calls++;
  if (path) {
    snprintf(g_auth_store_load_last_path, sizeof(g_auth_store_load_last_path), "%s", path);
  } else {
    g_auth_store_load_last_path[0] = '\0';
  }
  return g_auth_store_load_result;
}
void auth_store_free(struct auth_store *s) {
  (void)s;
}

enum config_ini_alloc_failpoint {
  CONFIG_INI_ALLOC_FAIL_NONE = 0,
  CONFIG_INI_ALLOC_FAIL_CANDIDATE,
  CONFIG_INI_ALLOC_FAIL_PARSE_CTX,
  CONFIG_INI_ALLOC_FAIL_ROUTE_PARSE,
  CONFIG_INI_ALLOC_FAIL_ROUTE_RESOLVED,
  CONFIG_INI_ALLOC_FAIL_ROUTE_INDEX,
  CONFIG_INI_ALLOC_FAIL_VHOST_ROUTE_LIST,
};

static enum config_ini_alloc_failpoint g_config_ini_alloc_failpoint = CONFIG_INI_ALLOC_FAIL_NONE;

static void reset_config_ini_alloc_failpoint(void) {
  g_config_ini_alloc_failpoint = CONFIG_INI_ALLOC_FAIL_NONE;
}

void *test_config_ini_alloc_candidate(size_t size) {
  if (g_config_ini_alloc_failpoint == CONFIG_INI_ALLOC_FAIL_CANDIDATE) {
    return NULL;
  }
  return malloc(size);
}

void *test_config_ini_alloc_parse_ctx(size_t nmemb, size_t size) {
  if (g_config_ini_alloc_failpoint == CONFIG_INI_ALLOC_FAIL_PARSE_CTX) {
    return NULL;
  }
  return calloc(nmemb, size);
}

void *test_config_ini_alloc_route_parse(size_t nmemb, size_t size) {
  if (g_config_ini_alloc_failpoint == CONFIG_INI_ALLOC_FAIL_ROUTE_PARSE) {
    return NULL;
  }
  return calloc(nmemb, size);
}

void *test_config_ini_alloc_route_resolved(size_t nmemb, size_t size) {
  if (g_config_ini_alloc_failpoint == CONFIG_INI_ALLOC_FAIL_ROUTE_RESOLVED) {
    return NULL;
  }
  return calloc(nmemb, size);
}

void *test_config_ini_alloc_route_index(size_t nmemb, size_t size) {
  if (g_config_ini_alloc_failpoint == CONFIG_INI_ALLOC_FAIL_ROUTE_INDEX) {
    return NULL;
  }
  return calloc(nmemb, size);
}

void *test_config_ini_alloc_vhost_route_list(size_t nmemb, size_t size) {
  if (g_config_ini_alloc_failpoint == CONFIG_INI_ALLOC_FAIL_VHOST_ROUTE_LIST) {
    return NULL;
  }
  return calloc(nmemb, size);
}

static int write_temp_ini(const char *content, char out_path[256]) {
  if (!content || !out_path) {
    return -1;
  }
  snprintf(out_path, 256, "/tmp/ini_testXXXXXX");
  int fd = mkstemp(out_path);
  if (fd < 0) {
    return -1;
  }
  size_t len = strlen(content);
  ssize_t wr = write(fd, content, len);
  close(fd);
  return (wr == (ssize_t)len) ? 0 : -1;
}

static int capture_stderr_start(int *saved_fd, int *read_fd) {
  int fds[2];
  if (pipe(fds) != 0) {
    return -1;
  }
  *saved_fd = dup(STDERR_FILENO);
  if (*saved_fd < 0) {
    close(fds[0]);
    close(fds[1]);
    return -1;
  }
  if (dup2(fds[1], STDERR_FILENO) < 0) {
    close(fds[0]);
    close(fds[1]);
    close(*saved_fd);
    return -1;
  }
  close(fds[1]);
  *read_fd = fds[0];
  return 0;
}

static int is_numeric_literal(const char *s) {
  if (!s || !*s) {
    return 0;
  }
  char addr[128], zone[128];
  split_zone_id(s, addr, sizeof(addr), zone, sizeof(zone));
  if (inet_pton(AF_INET, addr, &(struct in_addr){0}) == 1) {
    return 1;
  }
  if (inet_pton(AF_INET6, addr, &(struct in6_addr){0}) == 1) {
    return 1;
  }
  return 0;
}

static int capture_stderr_end(int saved_fd, int read_fd, char *buf, size_t bufsz) {
  if (!buf || bufsz == 0) {
    return -1;
  }
  buf[0] = '\0';
  (void)dup2(saved_fd, STDERR_FILENO);
  close(saved_fd);
  size_t off = 0;
  for (;;) {
    ssize_t n = read(read_fd, buf + off, bufsz - off - 1);
    if (n <= 0) {
      break;
    }
    off += (size_t)n;
    if (off + 1 >= bufsz) {
      break;
    }
  }
  close(read_fd);
  buf[off] = '\0';
  return 0;
}

static int init_cfg(struct config_t *cfg) {
  reset_auth_store_stub();
  reset_config_ini_alloc_failpoint();
  return config_set_defaults(cfg);
}

static void free_loaded_cfg_heap(struct config_t *cfg) {
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

    free(vh->route_rules);
    vh->route_rules = NULL;
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

static int route_ptr_in_cfg_storage(const struct config_t *cfg,
                                    const struct route_policy_rule *rr) {
  if (!cfg || !rr || cfg->route_rule_count <= 0) {
    return 0;
  }

  uintptr_t base = (uintptr_t)(const void *)cfg->route_rules;
  uintptr_t ptr = (uintptr_t)(const void *)rr;
  size_t bytes = (size_t)cfg->route_rule_count * sizeof(cfg->route_rules[0]);

  if (ptr < base || (ptr - base) >= bytes) {
    return 0;
  }

  return ((ptr - base) % sizeof(cfg->route_rules[0])) == 0;
}

TEST t_config_ini_parses_globals_and_vhost(void) {
  const char *ini = "[globals]\n"
                    "log_level = debug\n"
                    "log_categories = core,io\n"
                    "queue_depth = 4096\n"
                    "pre_accepts = 32\n"
                    "default_max_header_fields = 77\n"
                    "\n"
                    "[vhost main]\n"
                    "bind = 127.0.0.1\n"
                    "port = 8080\n"
                    "docroot = /tmp\n"
                    "static = true\n"
                    "range = false\n"
                    "conditional = true\n"
                    "compression = false\n"
                    "auth = true\n"
                    "max_header_fields = 55\n";

  char path[256];
  ASSERT_EQ(write_temp_ini(ini, path), 0);

  struct config_t cfg;
  char err[256];
  ASSERT_EQ(init_cfg(&cfg), 0);
  ASSERT_EQ(config_load_ini(path, &cfg, err), 0);

  ASSERT_EQ(cfg.vhost_count, 1);
  ASSERT_EQ(cfg.vhosts[0].port, (uint16_t)8080);
  ASSERT_EQ(strcmp(cfg.vhosts[0].bind, "127.0.0.1"), 0);
  ASSERT_EQ(cfg.vhosts[0].max_header_fields, (uint16_t)55);
  ASSERT_EQ(cfg.vhosts[0].auth_enabled, 1u);
  ASSERT_EQ((cfg.vhosts[0].features & CFG_FEAT_STATIC), CFG_FEAT_STATIC);
  ASSERT_EQ((cfg.vhosts[0].features & CFG_FEAT_CONDITIONAL), CFG_FEAT_CONDITIONAL);
  ASSERT_EQ((cfg.vhosts[0].features & CFG_FEAT_AUTH), CFG_FEAT_AUTH);
  ASSERT_EQ((cfg.vhosts[0].features & CFG_FEAT_RANGE), 0);
  ASSERT_EQ((cfg.vhosts[0].features & CFG_FEAT_COMPRESSION), 0);

  ASSERT((cfg.g.present & GF_LOG_LEVEL) != 0);
  ASSERT((cfg.g.present & GF_LOG_CATEGORIES) != 0);
  ASSERT((cfg.g.present & GF_QUEUE_DEPTH) != 0);
  ASSERT((cfg.g.present & GF_PRE_ACCEPTS) != 0);
  ASSERT((cfg.g.present & GF_DEFAULT_MAX_HDR_FIELDS) != 0);
  ASSERT_EQ(cfg.g.queue_depth, (unsigned)4096);
  ASSERT_EQ(cfg.g.pre_accepts, (unsigned)32);
  ASSERT_EQ(cfg.g.default_max_header_fields, (unsigned)77);

  unlink(path);
  PASS();
}

TEST t_warns_linklocal_without_zone(void) {
  const char *ini = "[vhost ll]\n"
                    "bind = fe80::1\n"
                    "port = 8081\n";

  char path[256];
  ASSERT_EQ(write_temp_ini(ini, path), 0);

  log_set_level(LOG_WARN);
  log_set_categories(LOGC_CORE);
  log_set_thread_id(0);

  int saved_fd = -1, read_fd = -1;
  ASSERT_EQ(capture_stderr_start(&saved_fd, &read_fd), 0);

  struct config_t cfg;
  char err[256];
  ASSERT_EQ(init_cfg(&cfg), 0);
  ASSERT_EQ(config_load_ini(path, &cfg, err), 0);

  char out[1024];
  ASSERT_EQ(capture_stderr_end(saved_fd, read_fd, out, sizeof(out)), 0);
  ASSERT(strstr(out, "link-local bind without zone id") != NULL);

  unlink(path);
  PASS();
}

TEST t_warns_multiple_wildcard_vhosts(void) {
  const char *ini = "[vhost a]\n"
                    "bind = 0.0.0.0\n"
                    "port = 8082\n"
                    "\n"
                    "[vhost b]\n"
                    "bind = *\n"
                    "port = 8082\n";

  char path[256];
  ASSERT_EQ(write_temp_ini(ini, path), 0);

  log_set_level(LOG_WARN);
  log_set_categories(LOGC_CORE);
  log_set_thread_id(0);

  int saved_fd = -1, read_fd = -1;
  ASSERT_EQ(capture_stderr_start(&saved_fd, &read_fd), 0);

  struct config_t cfg;
  char err[256];
  ASSERT_EQ(init_cfg(&cfg), 0);
  ASSERT_EQ(config_load_ini(path, &cfg, err), 0);

  char out[1024];
  ASSERT_EQ(capture_stderr_end(saved_fd, read_fd, out, sizeof(out)), 0);
  ASSERT(strstr(out, "multiple wildcard vhosts on port 8082") != NULL);

  unlink(path);
  PASS();
}

TEST t_applies_default_max_header_fields(void) {
  const char *ini = "[globals]\n"
                    "default_max_header_fields = 77\n"
                    "\n"
                    "[vhost a]\n"
                    "bind = 127.0.0.1\n"
                    "port = 8083\n";

  char path[256];
  ASSERT_EQ(write_temp_ini(ini, path), 0);

  struct config_t cfg;
  char err[256];
  ASSERT_EQ(init_cfg(&cfg), 0);
  ASSERT_EQ(config_load_ini(path, &cfg, err), 0);

  ASSERT_EQ(cfg.vhost_count, 1);
  ASSERT_EQ(cfg.vhosts[0].max_header_fields, (uint16_t)77);

  unlink(path);
  PASS();
}

TEST t_invalid_port_fails(void) {
  const char *ini = "[vhost bad]\n"
                    "bind = 127.0.0.1\n"
                    "port = abc\n";

  char path[256];
  ASSERT_EQ(write_temp_ini(ini, path), 0);

  struct config_t cfg;
  char err[256];
  ASSERT_EQ(init_cfg(&cfg), 0);
  ASSERT_EQ(config_load_ini(path, &cfg, err), -1);

  unlink(path);
  PASS();
}

TEST t_invalid_bool_warns_and_ignores(void) {
  const char *ini = "[vhost bad]\n"
                    "bind = 127.0.0.1\n"
                    "port = 8084\n"
                    "static = maybe\n";

  char path[256];
  ASSERT_EQ(write_temp_ini(ini, path), 0);

  log_set_level(LOG_WARN);
  log_set_categories(LOGC_CORE);
  log_set_thread_id(0);

  int saved_fd = -1, read_fd = -1;
  ASSERT_EQ(capture_stderr_start(&saved_fd, &read_fd), 0);

  struct config_t cfg;
  char err[256];
  ASSERT_EQ(init_cfg(&cfg), 0);
  ASSERT_EQ(config_load_ini(path, &cfg, err), 0);

  char out[1024];
  ASSERT_EQ(capture_stderr_end(saved_fd, read_fd, out, sizeof(out)), 0);
  ASSERT(strstr(out, "invalid boolean for vhost key") != NULL);

  unlink(path);
  PASS();
}

TEST t_unknown_globals_ignored(void) {
  const char *ini = "[globals]\n"
                    "nonsense = 123\n"
                    "queue_depth = 2048\n"
                    "\n"
                    "[vhost a]\n"
                    "bind = 127.0.0.1\n"
                    "port = 8085\n";

  char path[256];
  ASSERT_EQ(write_temp_ini(ini, path), 0);

  struct config_t cfg;
  char err[256];
  ASSERT_EQ(init_cfg(&cfg), 0);
  ASSERT_EQ(config_load_ini(path, &cfg, err), 0);

  ASSERT((cfg.g.present & GF_QUEUE_DEPTH) != 0);
  ASSERT_EQ(cfg.g.queue_depth, (unsigned)2048);

  unlink(path);
  PASS();
}

TEST t_shutdown_grace_ms_parsed(void) {
  const char *ini = "[globals]\n"
                    "shutdown_grace_ms = 9000\n"
                    "\n"
                    "[vhost a]\n"
                    "bind = 127.0.0.1\n"
                    "port = 8085\n";

  char path[256];
  ASSERT_EQ(write_temp_ini(ini, path), 0);

  struct config_t cfg;
  char err[256];
  ASSERT_EQ(init_cfg(&cfg), 0);
  ASSERT_EQ(config_load_ini(path, &cfg, err), 0);

  ASSERT((cfg.g.present & GF_SHUTDOWN_GRACE_MS) != 0);
  ASSERT_EQ(cfg.g.shutdown_grace_ms, (unsigned)9000);

  unlink(path);
  PASS();
}

TEST t_shutdown_grace_ms_invalid_fails(void) {
  const char *ini = "[globals]\n"
                    "shutdown_grace_ms = nope\n"
                    "\n"
                    "[vhost a]\n"
                    "bind = 127.0.0.1\n"
                    "port = 8085\n";

  char path[256];
  ASSERT_EQ(write_temp_ini(ini, path), 0);

  struct config_t cfg;
  char err[256];
  ASSERT_EQ(init_cfg(&cfg), 0);
  ASSERT_EQ(config_load_ini(path, &cfg, err), -1);
  ASSERT(strstr(err, "shutdown_grace_ms") != NULL);

  unlink(path);
  PASS();
}

TEST t_workers_parsed(void) {
  const char *ini = "[globals]\n"
                    "workers = 7\n"
                    "\n"
                    "[vhost a]\n"
                    "bind = 127.0.0.1\n"
                    "port = 8085\n";

  char path[256];
  ASSERT_EQ(write_temp_ini(ini, path), 0);

  struct config_t cfg;
  char err[256];
  ASSERT_EQ(init_cfg(&cfg), 0);
  ASSERT_EQ(config_load_ini(path, &cfg, err), 0);

  ASSERT((cfg.g.present & GF_WORKERS) != 0);
  ASSERT_EQ(cfg.g.workers, (unsigned)7);

  unlink(path);
  PASS();
}

TEST t_workers_invalid_fails(void) {
  const char *ini = "[globals]\n"
                    "workers = 0\n"
                    "\n"
                    "[vhost a]\n"
                    "bind = 127.0.0.1\n"
                    "port = 8085\n";

  char path[256];
  ASSERT_EQ(write_temp_ini(ini, path), 0);

  struct config_t cfg;
  char err[256];
  ASSERT_EQ(init_cfg(&cfg), 0);
  ASSERT_EQ(config_load_ini(path, &cfg, err), -1);
  ASSERT(strstr(err, "workers") != NULL);

  unlink(path);
  PASS();
}

TEST t_wake_pipe_mode_parsed(void) {
  const char *ini = "[globals]\n"
                    "wake_pipe_mode = per-worker\n"
                    "\n"
                    "[vhost a]\n"
                    "bind = 127.0.0.1\n"
                    "port = 8085\n";

  char path[256];
  ASSERT_EQ(write_temp_ini(ini, path), 0);

  struct config_t cfg;
  char err[256];
  ASSERT_EQ(init_cfg(&cfg), 0);
  ASSERT_EQ(config_load_ini(path, &cfg, err), 0);

  ASSERT((cfg.g.present & GF_WAKE_PIPE_MODE) != 0);
  ASSERT_EQ(cfg.g.wake_pipe_mode, (unsigned)1);

  unlink(path);
  PASS();
}

TEST t_wake_pipe_mode_alias_rejected(void) {
  const char *ini = "[globals]\n"
                    "wake_pipe_mode = per_worker\n"
                    "\n"
                    "[vhost a]\n"
                    "bind = 127.0.0.1\n"
                    "port = 8085\n";

  char path[256];
  ASSERT_EQ(write_temp_ini(ini, path), 0);

  struct config_t cfg;
  char err[256];
  ASSERT_EQ(init_cfg(&cfg), 0);
  ASSERT_EQ(config_load_ini(path, &cfg, err), 0);

  ASSERT((cfg.g.present & GF_WAKE_PIPE_MODE) == 0);

  unlink(path);
  PASS();
}

TEST t_access_log_globals_parsed(void) {
  const char *ini = "[globals]\n"
                    "access_log_enabled = true\n"
                    "access_log_path = /tmp/access.log\n"
                    "access_log_format = text\n"
                    "access_log_sample = 10\n"
                    "access_log_min_status = 400\n"
                    "\n"
                    "[vhost a]\n"
                    "bind = 127.0.0.1\n"
                    "port = 8085\n";

  char path[256];
  ASSERT_EQ(write_temp_ini(ini, path), 0);

  struct config_t cfg;
  char err[256];
  ASSERT_EQ(init_cfg(&cfg), 0);
  ASSERT_EQ(config_load_ini(path, &cfg, err), 0);

  ASSERT((cfg.g.present & GF_ACCESS_LOG_ENABLED) != 0);
  ASSERT((cfg.g.present & GF_ACCESS_LOG_PATH) != 0);
  ASSERT((cfg.g.present & GF_ACCESS_LOG_FORMAT) != 0);
  ASSERT((cfg.g.present & GF_ACCESS_LOG_SAMPLE) != 0);
  ASSERT((cfg.g.present & GF_ACCESS_LOG_MIN_STATUS) != 0);

  ASSERT_EQ(cfg.g.access_log_enabled, (unsigned)1);
  ASSERT_EQ(strcmp(cfg.g.access_log_path, "/tmp/access.log"), 0);
  ASSERT_EQ(strcmp(cfg.g.access_log_format, "text"), 0);
  ASSERT_EQ(cfg.g.access_log_sample, (unsigned)10);
  ASSERT_EQ(cfg.g.access_log_min_status, (unsigned)400);

  unlink(path);
  PASS();
}

TEST t_access_log_sample_invalid_fails(void) {
  const char *ini = "[globals]\n"
                    "access_log_sample = 0\n"
                    "\n"
                    "[vhost a]\n"
                    "bind = 127.0.0.1\n"
                    "port = 8085\n";

  char path[256];
  ASSERT_EQ(write_temp_ini(ini, path), 0);

  struct config_t cfg;
  char err[256];
  ASSERT_EQ(init_cfg(&cfg), 0);
  ASSERT_EQ(config_load_ini(path, &cfg, err), -1);
  ASSERT(strstr(err, "access_log_sample") != NULL);

  unlink(path);
  PASS();
}

TEST t_access_log_min_status_invalid_fails(void) {
  const char *ini = "[globals]\n"
                    "access_log_min_status = 42\n"
                    "\n"
                    "[vhost a]\n"
                    "bind = 127.0.0.1\n"
                    "port = 8085\n";

  char path[256];
  ASSERT_EQ(write_temp_ini(ini, path), 0);

  struct config_t cfg;
  char err[256];
  ASSERT_EQ(init_cfg(&cfg), 0);
  ASSERT_EQ(config_load_ini(path, &cfg, err), -1);
  ASSERT(strstr(err, "access_log_min_status") != NULL);

  unlink(path);
  PASS();
}

TEST t_docroot_open_failure_nonfatal(void) {
  const char *ini = "[vhost a]\n"
                    "bind = 127.0.0.1\n"
                    "port = 8086\n"
                    "docroot = /this/does/not/exist\n"
                    "static = true\n";

  char path[256];
  ASSERT_EQ(write_temp_ini(ini, path), 0);

  log_set_level(LOG_WARN);
  log_set_categories(LOGC_CORE);
  log_set_thread_id(0);

  int saved_fd = -1, read_fd = -1;
  ASSERT_EQ(capture_stderr_start(&saved_fd, &read_fd), 0);

  struct config_t cfg;
  char err[256];
  ASSERT_EQ(init_cfg(&cfg), 0);
  ASSERT_EQ(config_load_ini(path, &cfg, err), 0);

  char out[1024];
  ASSERT_EQ(capture_stderr_end(saved_fd, read_fd, out, sizeof(out)), 0);
  ASSERT(strstr(out, "docroot open failed") != NULL);

  ASSERT_EQ(cfg.vhost_count, 1);
  ASSERT_EQ(cfg.vhosts[0].docroot_fd, -1);

  unlink(path);
  PASS();
}

TEST t_unknown_vhost_key_warns_and_ignores(void) {
  const char *ini = "[vhost a]\n"
                    "bind = 127.0.0.1\n"
                    "port = 8087\n"
                    "nonsense = 1\n";

  char path[256];
  ASSERT_EQ(write_temp_ini(ini, path), 0);

  log_set_level(LOG_WARN);
  log_set_categories(LOGC_CORE);
  log_set_thread_id(0);

  int saved_fd = -1, read_fd = -1;
  ASSERT_EQ(capture_stderr_start(&saved_fd, &read_fd), 0);

  struct config_t cfg;
  char err[256];
  ASSERT_EQ(init_cfg(&cfg), 0);
  ASSERT_EQ(config_load_ini(path, &cfg, err), 0);

  char out[1024];
  ASSERT_EQ(capture_stderr_end(saved_fd, read_fd, out, sizeof(out)), 0);
  ASSERT(strstr(out, "unknown vhost key") != NULL);

  unlink(path);
  PASS();
}

TEST t_hostname_bind_normalizes_to_numeric(void) {
  const char *ini = "[vhost host]\n"
                    "bind = localhost\n"
                    "port = 8088\n";

  char path[256];
  ASSERT_EQ(write_temp_ini(ini, path), 0);

  struct config_t cfg;
  char err[256];
  ASSERT_EQ(init_cfg(&cfg), 0);
  ASSERT_EQ(config_load_ini(path, &cfg, err), 0);

  ASSERT_EQ(cfg.vhost_count, 1);
  ASSERT(is_numeric_literal(cfg.vhosts[0].bind));
  ASSERT(strcmp(cfg.vhosts[0].bind, "localhost") != 0);

  unlink(path);
  PASS();
}

TEST t_hostname_bind_failure_returns_error(void) {
  const char *ini = "[vhost bad]\n"
                    "bind = no-such-host.invalid.example\n"
                    "port = 8089\n";

  char path[256];
  ASSERT_EQ(write_temp_ini(ini, path), 0);

  struct config_t cfg;
  char err[256];
  ASSERT_EQ(init_cfg(&cfg), 0);
  ASSERT_EQ(config_load_ini(path, &cfg, err), -1);
  ASSERT(err[0] != '\0');

  unlink(path);
  PASS();
}

TEST t_all_vhosts_preserved_over_16(void) {
  char ini[4096];
  size_t off = 0;
  for (int i = 0; i < 20; ++i) {
    off += (size_t)snprintf(ini + off,
                            sizeof(ini) - off,
                            "[vhost v%d]\n"
                            "bind = 127.0.0.1\n"
                            "port = %d\n\n",
                            i,
                            9000 + i);
    if (off + 64 >= sizeof(ini)) {
      break;
    }
  }

  char path[256];
  ASSERT_EQ(write_temp_ini(ini, path), 0);

  struct config_t cfg;
  char err[256];
  ASSERT_EQ(init_cfg(&cfg), 0);
  ASSERT_EQ(config_load_ini(path, &cfg, err), 0);

  ASSERT_EQ(cfg.vhost_count, 20);
  ASSERT_EQ(cfg.vhosts[0].port, (uint16_t)9000);
  ASSERT_EQ(cfg.vhosts[19].port, (uint16_t)9019);

  unlink(path);
  PASS();
}

TEST t_tls_globals_parsed(void) {
  const char *ini = "[globals]\n"
                    "tls = true\n"
                    "tls_cert_file = /tmp/server.crt\n"
                    "tls_key_file = /tmp/server.key\n"
                    "tls_min_version = tls1.3\n"
                    "tls_ciphers = HIGH:!aNULL\n"
                    "tls_ciphersuites = TLS_AES_128_GCM_SHA256\n"
                    "tls_session_tickets = false\n"
                    "tls_session_cache = false\n"
                    "\n"
                    "[vhost a]\n"
                    "bind = 127.0.0.1\n"
                    "port = 8091\n";

  char path[256];
  ASSERT_EQ(write_temp_ini(ini, path), 0);

  struct config_t cfg;
  char err[256];
  ASSERT_EQ(init_cfg(&cfg), 0);
  ASSERT_EQ(config_load_ini(path, &cfg, err), 0);

  ASSERT((cfg.g.present & GF_TLS_ENABLED) != 0);
  ASSERT((cfg.g.present & GF_TLS_CERT_FILE) != 0);
  ASSERT((cfg.g.present & GF_TLS_KEY_FILE) != 0);
  ASSERT((cfg.g.present & GF_TLS_MIN_VERSION) != 0);
  ASSERT((cfg.g.present & GF_TLS_CIPHERS) != 0);
  ASSERT((cfg.g.present & GF_TLS_CIPHERSUITES) != 0);
  ASSERT((cfg.g.present & GF_TLS_SESSION_TICKETS) != 0);
  ASSERT((cfg.g.present & GF_TLS_SESSION_CACHE) != 0);

  ASSERT_EQ(cfg.g.tls_enabled, (unsigned)1);
  ASSERT_EQ(strcmp(cfg.g.tls_cert_file, "/tmp/server.crt"), 0);
  ASSERT_EQ(strcmp(cfg.g.tls_key_file, "/tmp/server.key"), 0);
  ASSERT_EQ(strcmp(cfg.g.tls_min_version, "tls1.3"), 0);
  ASSERT_EQ(strcmp(cfg.g.tls_ciphers, "HIGH:!aNULL"), 0);
  ASSERT_EQ(strcmp(cfg.g.tls_ciphersuites, "TLS_AES_128_GCM_SHA256"), 0);
  ASSERT_EQ(cfg.g.tls_session_tickets, (unsigned)0);
  ASSERT_EQ(cfg.g.tls_session_cache, (unsigned)0);

  unlink(path);
  PASS();
}

TEST t_tls_vhost_enable_inherits_global_cert_key(void) {
  const char *ini = "[globals]\n"
                    "tls = false\n"
                    "tls_cert_file = /tmp/global.crt\n"
                    "tls_key_file = /tmp/global.key\n"
                    "\n"
                    "[vhost a]\n"
                    "bind = 127.0.0.1\n"
                    "port = 8092\n"
                    "tls = true\n";

  char path[256];
  ASSERT_EQ(write_temp_ini(ini, path), 0);

  struct config_t cfg;
  char err[256];
  ASSERT_EQ(init_cfg(&cfg), 0);
  ASSERT_EQ(config_load_ini(path, &cfg, err), 0);

  ASSERT_EQ(cfg.vhost_count, 1);
  ASSERT_EQ(cfg.vhosts[0].tls_enabled_set, (uint8_t)1);
  ASSERT_EQ(cfg.vhosts[0].tls_enabled, (uint8_t)1);

  unlink(path);
  PASS();
}

TEST t_tls_vhost_overrides_ticket_cache_flags(void) {
  const char *ini = "[globals]\n"
                    "tls = true\n"
                    "tls_cert_file = /tmp/global.crt\n"
                    "tls_key_file = /tmp/global.key\n"
                    "tls_session_tickets = false\n"
                    "tls_session_cache = false\n"
                    "\n"
                    "[vhost a]\n"
                    "bind = 127.0.0.1\n"
                    "port = 8094\n"
                    "tls = true\n"
                    "tls_session_tickets = true\n"
                    "tls_session_cache = true\n";

  char path[256];
  ASSERT_EQ(write_temp_ini(ini, path), 0);

  struct config_t cfg;
  char err[256];
  ASSERT_EQ(init_cfg(&cfg), 0);
  ASSERT_EQ(config_load_ini(path, &cfg, err), 0);

  ASSERT_EQ(cfg.vhost_count, 1);
  ASSERT_EQ(cfg.vhosts[0].tls_session_tickets_set, (uint8_t)1);
  ASSERT_EQ(cfg.vhosts[0].tls_session_tickets, (uint8_t)1);
  ASSERT_EQ(cfg.vhosts[0].tls_session_cache_set, (uint8_t)1);
  ASSERT_EQ(cfg.vhosts[0].tls_session_cache, (uint8_t)1);

  unlink(path);
  PASS();
}

TEST t_tls_enabled_without_key_fails(void) {
  const char *ini = "[vhost bad]\n"
                    "bind = 127.0.0.1\n"
                    "port = 8093\n"
                    "tls = true\n"
                    "tls_cert_file = /tmp/only-cert.crt\n";

  char path[256];
  ASSERT_EQ(write_temp_ini(ini, path), 0);

  struct config_t cfg;
  char err[256];
  ASSERT_EQ(init_cfg(&cfg), 0);
  ASSERT_EQ(config_load_ini(path, &cfg, err), -1);
  ASSERT(strstr(err, "tls=true requires tls_cert_file and tls_key_file") != NULL);

  unlink(path);
  PASS();
}

// ===========================================================================
// header_set parser validation
// ===========================================================================

TEST t_header_set_valid_stored(void) {
  const char *ini = "[vhost hs]\n"
                    "bind = 127.0.0.1\n"
                    "port = 8094\n"
                    "header_set = Cache-Control: no-cache\n"
                    "header_set = X-Frame-Options: DENY\n";

  char path[256];
  ASSERT_EQ(write_temp_ini(ini, path), 0);

  struct config_t cfg;
  char err[256];
  ASSERT_EQ(config_set_defaults(&cfg), 0);
  ASSERT_EQ(config_load_ini(path, &cfg, err), 0);
  ASSERT_EQ(cfg.vhosts[0].custom_headers_count, 2u);
  ASSERT(strstr(cfg.vhosts[0].custom_headers[0], "Cache-Control: no-cache\r\n") != NULL);
  ASSERT(strstr(cfg.vhosts[0].custom_headers[1], "X-Frame-Options: DENY\r\n") != NULL);

  for (unsigned i = 0; i < cfg.vhosts[0].custom_headers_count; i++)
    free(cfg.vhosts[0].custom_headers[i]);
  unlink(path);
  PASS();
}

TEST t_header_set_rejects_no_colon(void) {
  const char *ini = "[vhost hs2]\n"
                    "bind = 127.0.0.1\n"
                    "port = 8095\n"
                    "header_set = InvalidNoColon\n";

  char path[256];
  ASSERT_EQ(write_temp_ini(ini, path), 0);

  struct config_t cfg;
  char err[256];
  ASSERT_EQ(config_set_defaults(&cfg), 0);
  ASSERT_EQ(config_load_ini(path, &cfg, err), 0);
  // Should be rejected — count stays 0.
  ASSERT_EQ(cfg.vhosts[0].custom_headers_count, 0u);

  unlink(path);
  PASS();
}

TEST t_header_set_rejects_bad_token_char(void) {
  const char *ini = "[vhost hs3]\n"
                    "bind = 127.0.0.1\n"
                    "port = 8096\n"
                    "header_set = Bad Name: value\n";

  char path[256];
  ASSERT_EQ(write_temp_ini(ini, path), 0);

  struct config_t cfg;
  char err[256];
  ASSERT_EQ(config_set_defaults(&cfg), 0);
  ASSERT_EQ(config_load_ini(path, &cfg, err), 0);
  // Space in header name is not a valid token character.
  ASSERT_EQ(cfg.vhosts[0].custom_headers_count, 0u);

  unlink(path);
  PASS();
}

TEST t_header_set_rejects_crlf_injection(void) {
  // Bare \r in middle of value — inih delivers it, our validator rejects.
  const char *ini = "[vhost hs5]\n"
                    "bind = 127.0.0.1\n"
                    "port = 8098\n"
                    "header_set = X-Injected: foo\rbar\n";

  char path[256];
  ASSERT_EQ(write_temp_ini(ini, path), 0);

  struct config_t cfg;
  char err[256];
  ASSERT_EQ(config_set_defaults(&cfg), 0);
  ASSERT_EQ(config_load_ini(path, &cfg, err), 0);
  // Embedded CR must be rejected.
  ASSERT_EQ(cfg.vhosts[0].custom_headers_count, 0u);

  unlink(path);
  PASS();
}

TEST t_header_set_rejects_overlength(void) {
  // Build a header_set value > 1024 bytes.
  char ini[2048];
  size_t off = 0;
  off += (size_t)snprintf(ini + off, sizeof(ini) - off,
                          "[vhost hs6]\nbind = 127.0.0.1\nport = 8099\nheader_set = X-Long: ");
  // Fill with 'A' to exceed 1024 total.
  while (off < 1100 && off < sizeof(ini) - 2) {
    ini[off++] = 'A';
  }
  ini[off++] = '\n';
  ini[off] = '\0';

  char path[256];
  ASSERT_EQ(write_temp_ini(ini, path), 0);

  struct config_t cfg;
  char err[256];
  ASSERT_EQ(config_set_defaults(&cfg), 0);
  ASSERT_EQ(config_load_ini(path, &cfg, err), 0);
  // Over-length must be rejected.
  ASSERT_EQ(cfg.vhosts[0].custom_headers_count, 0u);

  unlink(path);
  PASS();
}

TEST t_header_set_max_16_enforced(void) {
  char ini[4096];
  size_t off = 0;
  off += (size_t)snprintf(ini + off, sizeof(ini) - off,
                          "[vhost hs4]\nbind = 127.0.0.1\nport = 8097\n");
  for (int i = 0; i < 18; i++) {
    off += (size_t)snprintf(ini + off, sizeof(ini) - off,
                            "header_set = X-Hdr-%d: val\n", i);
  }

  char path[256];
  ASSERT_EQ(write_temp_ini(ini, path), 0);

  struct config_t cfg;
  char err[256];
  ASSERT_EQ(config_set_defaults(&cfg), 0);
  ASSERT_EQ(config_load_ini(path, &cfg, err), 0);
  // Only 16 stored, the rest ignored.
  ASSERT_EQ(cfg.vhosts[0].custom_headers_count, 16u);

  for (unsigned i = 0; i < cfg.vhosts[0].custom_headers_count; i++)
    free(cfg.vhosts[0].custom_headers[i]);
  unlink(path);
  PASS();
}

TEST t_header_set_rejects_over_budget(void) {
  // Fill budget to just over 1536 bytes with multiple valid entries.
  // Each stored entry = prefix ("X-B0: " = 6 bytes) + 400 value bytes + "\r\n" = 408 bytes.
  // 3 entries: 3 * 408 = 1224 bytes (fits).  4th: 4 * 408 = 1632 bytes (exceeds 1536).
  char ini[8192];
  size_t off = 0;
  off += (size_t)snprintf(ini + off, sizeof(ini) - off,
                          "[vhost hs7]\nbind = 127.0.0.1\nport = 8100\n");
  for (int i = 0; i < 4; i++) {
    off += (size_t)snprintf(ini + off, sizeof(ini) - off, "header_set = X-B%d: ", i);
    // 400 'A' characters as value payload.
    for (int j = 0; j < 400 && off < sizeof(ini) - 2; j++) {
      ini[off++] = 'A';
    }
    ini[off++] = '\n';
  }
  ini[off] = '\0';

  char path[256];
  ASSERT_EQ(write_temp_ini(ini, path), 0);

  struct config_t cfg;
  char err[256];
  ASSERT_EQ(config_set_defaults(&cfg), 0);
  ASSERT_EQ(config_load_ini(path, &cfg, err), 0);
  // First 3 entries fit (3 * 408 = 1224 ≤ 1536).  4th pushes total to 1632 > 1536.
  ASSERT_EQ(cfg.vhosts[0].custom_headers_count, 3u);

  for (unsigned i = 0; i < cfg.vhosts[0].custom_headers_count; i++)
    free(cfg.vhosts[0].custom_headers[i]);
  unlink(path);
  PASS();
}

// ===========================================================================
// index filename configuration
// ===========================================================================

TEST t_index_default_is_empty(void) {
  const char *ini = "[vhost idx]\n"
                    "bind = 127.0.0.1\n"
                    "port = 8094\n";

  char path[256];
  ASSERT_EQ(write_temp_ini(ini, path), 0);

  struct config_t cfg;
  char err[256];
  ASSERT_EQ(config_set_defaults(&cfg), 0);
  ASSERT_EQ(config_load_ini(path, &cfg, err), 0);
  ASSERT_EQ(cfg.vhosts[0].index_file[0], '\0');

  unlink(path);
  PASS();
}

TEST t_index_custom_stored(void) {
  const char *ini = "[vhost idx2]\n"
                    "bind = 127.0.0.1\n"
                    "port = 8095\n"
                    "index = main.html\n";

  char path[256];
  ASSERT_EQ(write_temp_ini(ini, path), 0);

  struct config_t cfg;
  char err[256];
  ASSERT_EQ(config_set_defaults(&cfg), 0);
  ASSERT_EQ(config_load_ini(path, &cfg, err), 0);
  ASSERT_STR_EQ(cfg.vhosts[0].index_file, "main.html");

  unlink(path);
  PASS();
}

TEST t_index_rejects_slash(void) {
  const char *ini = "[vhost idx3]\n"
                    "bind = 127.0.0.1\n"
                    "port = 8096\n"
                    "index = sub/index.html\n";

  char path[256];
  ASSERT_EQ(write_temp_ini(ini, path), 0);

  struct config_t cfg;
  char err[256];
  ASSERT_EQ(config_set_defaults(&cfg), 0);
  ASSERT_EQ(config_load_ini(path, &cfg, err), 0);
  // Rejected — must be a plain filename.
  ASSERT_EQ(cfg.vhosts[0].index_file[0], '\0');

  unlink(path);
  PASS();
}

TEST t_index_rejects_dotdot(void) {
  const char *ini = "[vhost idx4]\n"
                    "bind = 127.0.0.1\n"
                    "port = 8097\n"
                    "index = ..\n";

  char path[256];
  ASSERT_EQ(write_temp_ini(ini, path), 0);

  struct config_t cfg;
  char err[256];
  ASSERT_EQ(init_cfg(&cfg), 0);
  ASSERT_EQ(config_load_ini(path, &cfg, err), 0);
  ASSERT_EQ(cfg.vhosts[0].index_file[0], '\0');

  unlink(path);
  PASS();
}

TEST t_auth_basic_file_and_realm_parsed(void) {
  static struct auth_store fake_store;
  const char *ini = "[vhost auth]\n"
                    "bind = 127.0.0.1\n"
                    "port = 8098\n"
                    "auth = true\n"
                    "auth_basic_file = /tmp/auth.htpasswd\n"
                    "auth_realm = Admin Area\n";

  char path[256];
  ASSERT_EQ(write_temp_ini(ini, path), 0);

  struct config_t cfg;
  char err[256];
  ASSERT_EQ(init_cfg(&cfg), 0);
  g_auth_store_load_result = &fake_store;
  ASSERT_EQ(config_load_ini(path, &cfg, err), 0);

  ASSERT_EQ(cfg.vhost_count, 1);
  ASSERT_EQ(strcmp(cfg.vhosts[0].auth_basic_file, "/tmp/auth.htpasswd"), 0);
  ASSERT_EQ(strcmp(cfg.vhosts[0].auth_realm, "Admin Area"), 0);
  ASSERT_EQ(g_auth_store_load_calls, 1u);
  ASSERT_EQ(strcmp(g_auth_store_load_last_path, "/tmp/auth.htpasswd"), 0);
  ASSERT_EQ(cfg.vhosts[0].auth_store, &fake_store);

  unlink(path);
  PASS();
}

TEST t_auth_realm_invalid_rejected(void) {
  const char *ini = "[vhost auth]\n"
                    "bind = 127.0.0.1\n"
                    "port = 8099\n"
                    "auth = true\n"
                    "auth_realm = Bad\"Realm\n";

  char path[256];
  ASSERT_EQ(write_temp_ini(ini, path), 0);

  log_set_level(LOG_WARN);
  log_set_categories(LOGC_CORE);
  log_set_thread_id(0);

  int saved_fd = -1, read_fd = -1;
  ASSERT_EQ(capture_stderr_start(&saved_fd, &read_fd), 0);

  struct config_t cfg;
  char err[256];
  ASSERT_EQ(init_cfg(&cfg), 0);
  ASSERT_EQ(config_load_ini(path, &cfg, err), 0);

  char out[1024];
  ASSERT_EQ(capture_stderr_end(saved_fd, read_fd, out, sizeof(out)), 0);
  ASSERT(strstr(out, "auth_realm contains invalid character") != NULL);
  ASSERT_EQ(cfg.vhosts[0].auth_realm[0], '\0');

  unlink(path);
  PASS();
}

TEST t_auth_basic_file_ignored_when_auth_disabled(void) {
  const char *ini = "[vhost auth]\n"
                    "bind = 127.0.0.1\n"
                    "port = 8100\n"
                    "auth = false\n"
                    "auth_basic_file = /tmp/ignored.htpasswd\n";

  char path[256];
  ASSERT_EQ(write_temp_ini(ini, path), 0);

  log_set_level(LOG_WARN);
  log_set_categories(LOGC_CORE);
  log_set_thread_id(0);

  int saved_fd = -1, read_fd = -1;
  ASSERT_EQ(capture_stderr_start(&saved_fd, &read_fd), 0);

  struct config_t cfg;
  char err[256];
  ASSERT_EQ(init_cfg(&cfg), 0);
  ASSERT_EQ(config_load_ini(path, &cfg, err), 0);

  char out[1024];
  ASSERT_EQ(capture_stderr_end(saved_fd, read_fd, out, sizeof(out)), 0);
  ASSERT(strstr(out, "auth_basic_file set but auth = false; file ignored") != NULL);
  ASSERT_EQ(g_auth_store_load_calls, 0u);
  ASSERT_EQ(cfg.vhosts[0].auth_store, NULL);

  unlink(path);
  PASS();
}

TEST t_auth_basic_file_load_failure_returns_error(void) {
  const char *ini = "[vhost auth]\n"
                    "bind = 127.0.0.1\n"
                    "port = 8101\n"
                    "auth = true\n"
                    "auth_basic_file = /tmp/missing.htpasswd\n";

  char path[256];
  ASSERT_EQ(write_temp_ini(ini, path), 0);

  struct config_t cfg;
  char err[256];
  ASSERT_EQ(init_cfg(&cfg), 0);
  g_auth_store_load_result = NULL;
  ASSERT_EQ(config_load_ini(path, &cfg, err), -1);
  ASSERT_EQ(g_auth_store_load_calls, 1u);
  ASSERT_EQ(strcmp(g_auth_store_load_last_path, "/tmp/missing.htpasswd"), 0);

  unlink(path);
  PASS();
}

TEST t_auth_load_failure_keeps_cfg_unchanged(void) {
  const char *ini = "[vhost first]\n"
                    "bind = 127.0.0.1\n"
                    "port = 8102\n"
                    "docroot = /tmp\n"
                    "header_set = X-Test: one\n"
                    "\n"
                    "[vhost second]\n"
                    "bind = 127.0.0.1\n"
                    "port = 8103\n"
                    "auth = true\n"
                    "auth_basic_file = /tmp/missing.htpasswd\n";

  char path[256];
  ASSERT_EQ(write_temp_ini(ini, path), 0);

  struct config_t cfg;
  char err[256];
  ASSERT_EQ(init_cfg(&cfg), 0);
  g_auth_store_load_result = NULL;
  ASSERT_EQ(config_load_ini(path, &cfg, err), -1);

  ASSERT_EQ(cfg.vhost_count, 0);
  ASSERT_EQ(cfg.route_rule_count, 0);
  ASSERT_EQ(g_auth_store_load_calls, 1u);
  ASSERT_EQ(strcmp(g_auth_store_load_last_path, "/tmp/missing.htpasswd"), 0);

  unlink(path);
  PASS();
}

TEST t_route_auth_modes_parsed(void) {
  const char *ini = "[vhost protected]\n"
                    "bind = 127.0.0.1\n"
                    "port = 8104\n"
                    "auth = true\n"
                    "auth_basic_file = /tmp/users.htpasswd\n"
                    "\n"
                    "[route root]\n"
                    "vhost = protected\n"
                    "path_prefix = /\n"
                    "auth = inherit\n"
                    "\n"
                    "[route admin]\n"
                    "vhost = protected\n"
                    "path_prefix = /admin\n"
                    "auth = require\n"
                    "\n"
                    "[route health]\n"
                    "vhost = protected\n"
                    "path_prefix = /healthz\n"
                    "auth = disable\n";

  char path[256];
  ASSERT_EQ(write_temp_ini(ini, path), 0);

  struct config_t cfg;
  char err[256];
  ASSERT_EQ(init_cfg(&cfg), 0);
  g_auth_store_load_result = (struct auth_store *)0x1;
  ASSERT_EQ(config_load_ini(path, &cfg, err), 0);

  ASSERT_EQ(cfg.route_rule_count, 3);
  ASSERT_EQ(cfg.vhosts[0].route_rule_count, 3u);
  ASSERT_EQ(g_auth_store_load_calls, 1u);

  ASSERT_STR_EQ(cfg.vhosts[0].route_rules[0]->path_prefix, "/healthz");
  ASSERT_EQ(cfg.vhosts[0].route_rules[0]->auth_mode, ROUTE_AUTH_DISABLE);

  ASSERT_STR_EQ(cfg.vhosts[0].route_rules[1]->path_prefix, "/admin");
  ASSERT_EQ(cfg.vhosts[0].route_rules[1]->auth_mode, ROUTE_AUTH_REQUIRE);

  ASSERT_STR_EQ(cfg.vhosts[0].route_rules[2]->path_prefix, "/");
  ASSERT_EQ(cfg.vhosts[0].route_rules[2]->auth_mode, ROUTE_AUTH_INHERIT);

  free_loaded_cfg_heap(&cfg);

  unlink(path);
  PASS();
}

TEST t_route_auth_invalid_mode_fails(void) {
  const char *ini = "[vhost main]\n"
                    "bind = 127.0.0.1\n"
                    "port = 8104\n"
                    "\n"
                    "[route bad]\n"
                    "vhost = main\n"
                    "path_prefix = /admin\n"
                    "auth = maybe\n";

  char path[256];
  ASSERT_EQ(write_temp_ini(ini, path), 0);

  struct config_t cfg;
  char err[256];
  ASSERT_EQ(init_cfg(&cfg), 0);
  ASSERT_EQ(config_load_ini(path, &cfg, err), -1);
  ASSERT(strstr(err, "invalid route auth mode") != NULL);

  unlink(path);
  PASS();
}

TEST t_route_auth_require_without_store_fails(void) {
  const char *ini = "[vhost main]\n"
                    "bind = 127.0.0.1\n"
                    "port = 8104\n"
                    "\n"
                    "[route admin]\n"
                    "vhost = main\n"
                    "path_prefix = /admin\n"
                    "auth = require\n";

  char path[256];
  ASSERT_EQ(write_temp_ini(ini, path), 0);

  struct config_t cfg;
  char err[256];
  ASSERT_EQ(init_cfg(&cfg), 0);
  ASSERT_EQ(config_load_ini(path, &cfg, err), -1);
  ASSERT(strstr(err, "auth=require") != NULL);

  unlink(path);
  PASS();
}

TEST t_route_auth_require_on_public_vhost_loads_store_and_captures_auth(void) {
  const char *ini = "[vhost public]\n"
                    "bind = 127.0.0.1\n"
                    "port = 8104\n"
                    "auth = false\n"
                    "auth_basic_file = /tmp/users.htpasswd\n"
                    "\n"
                    "[route admin]\n"
                    "vhost = public\n"
                    "path_prefix = /admin\n"
                    "auth = require\n";

  char path[256];
  ASSERT_EQ(write_temp_ini(ini, path), 0);

  struct config_t cfg;
  char err[256];
  ASSERT_EQ(init_cfg(&cfg), 0);
  g_auth_store_load_result = (struct auth_store *)0x1;
  ASSERT_EQ(config_load_ini(path, &cfg, err), 0);

  ASSERT_EQ(cfg.vhost_count, 1);
  ASSERT_EQ(cfg.vhosts[0].auth_enabled, 0u);
  ASSERT_EQ((cfg.vhosts[0].features & CFG_FEAT_AUTH), CFG_FEAT_AUTH);
  ASSERT_EQ(cfg.vhosts[0].auth_store, (struct auth_store *)0x1);
  ASSERT_EQ(g_auth_store_load_calls, 1u);
  ASSERT_STR_EQ(g_auth_store_load_last_path, "/tmp/users.htpasswd");
  ASSERT_EQ(cfg.vhosts[0].route_rule_count, 1u);
  ASSERT_EQ(cfg.vhosts[0].route_rules[0]->auth_mode, ROUTE_AUTH_REQUIRE);

  free_loaded_cfg_heap(&cfg);

  unlink(path);
  PASS();
}

TEST t_route_max_body_bytes_parsed(void) {
  const char *ini = "[vhost main]\n"
                    "bind = 127.0.0.1\n"
                    "port = 8104\n"
                    "\n"
                    "[route upload]\n"
                    "vhost = main\n"
                    "path_prefix = /upload\n"
                    "max_body_bytes = 104857600\n";

  char path[256];
  ASSERT_EQ(write_temp_ini(ini, path), 0);

  struct config_t cfg;
  char err[256];
  ASSERT_EQ(init_cfg(&cfg), 0);
  ASSERT_EQ(config_load_ini(path, &cfg, err), 0);

  ASSERT_EQ(cfg.route_rule_count, 1);
  ASSERT_EQ(cfg.vhosts[0].route_rule_count, 1u);
  ASSERT_EQ(cfg.vhosts[0].route_rules[0]->max_body_bytes_set, 1u);
  ASSERT_EQ(cfg.vhosts[0].route_rules[0]->max_body_bytes, (uint64_t)104857600u);

  free_loaded_cfg_heap(&cfg);

  unlink(path);
  PASS();
}

TEST t_route_max_body_bytes_absent_inherits_default(void) {
  const char *ini = "[vhost main]\n"
                    "bind = 127.0.0.1\n"
                    "port = 8104\n"
                    "\n"
                    "[route upload]\n"
                    "vhost = main\n"
                    "path_prefix = /upload\n";

  char path[256];
  ASSERT_EQ(write_temp_ini(ini, path), 0);

  struct config_t cfg;
  char err[256];
  ASSERT_EQ(init_cfg(&cfg), 0);
  ASSERT_EQ(config_load_ini(path, &cfg, err), 0);

  ASSERT_EQ(cfg.route_rule_count, 1);
  ASSERT_EQ(cfg.vhosts[0].route_rules[0]->max_body_bytes_set, 0u);
  ASSERT_EQ(cfg.vhosts[0].route_rules[0]->max_body_bytes, (uint64_t)0u);

  free_loaded_cfg_heap(&cfg);

  unlink(path);
  PASS();
}

TEST t_route_max_body_bytes_invalid_values_fail(void) {
  const char *values[] = {
    "0",
    "+5",
    "nope",
    "18446744073709551616",
  };

  for (size_t i = 0; i < sizeof(values) / sizeof(values[0]); ++i) {
    char ini[512];
    snprintf(ini,
             sizeof(ini),
             "[vhost main]\n"
             "bind = 127.0.0.1\n"
             "port = 8104\n"
             "\n"
             "[route upload]\n"
             "vhost = main\n"
             "path_prefix = /upload\n"
             "max_body_bytes = %s\n",
             values[i]);

    char path[256];
    ASSERT_EQ(write_temp_ini(ini, path), 0);

    struct config_t cfg;
    char err[256];
    ASSERT_EQ(init_cfg(&cfg), 0);
    ASSERT_EQ(config_load_ini(path, &cfg, err), -1);
    ASSERT(strstr(err, "invalid route max_body_bytes") != NULL);

    unlink(path);
  }
  PASS();
}

TEST t_cors_and_security_headers_parsed(void) {
  const char *ini = "[vhost policy]\n"
                    "bind = 127.0.0.1\n"
                    "port = 8104\n"
                    "security_headers = true\n"
                    "security_header_set = X-Frame-Options: DENY\n"
                    "security_header_set = X-Content-Type-Options: nosniff\n"
                    "cors = true\n"
                    "cors_allow_origin = https://app.example.com\n"
                    "cors_allow_methods = GET,POST,OPTIONS\n"
                    "cors_allow_headers = Content-Type,Authorization\n"
                    "cors_allow_credentials = true\n"
                    "cors_max_age_seconds = 3600\n";

  char path[256];
  ASSERT_EQ(write_temp_ini(ini, path), 0);

  struct config_t cfg;
  char err[256];
  ASSERT_EQ(init_cfg(&cfg), 0);
  ASSERT_EQ(config_load_ini(path, &cfg, err), 0);

  ASSERT_EQ(cfg.vhost_count, 1);
  ASSERT(cfg.vhosts[0].security_headers != NULL);
  ASSERT_EQ(cfg.vhosts[0].security_headers->enabled, 1);
  ASSERT_EQ(cfg.vhosts[0].security_headers->header_count, 2u);
  ASSERT_STR_EQ(cfg.vhosts[0].security_headers->headers[0].name, "X-Frame-Options");
  ASSERT_STR_EQ(cfg.vhosts[0].security_headers->headers[0].value, "DENY");
  ASSERT_STR_EQ(cfg.vhosts[0].security_headers->headers[1].name, "X-Content-Type-Options");
  ASSERT_STR_EQ(cfg.vhosts[0].security_headers->headers[1].value, "nosniff");

  ASSERT(cfg.vhosts[0].cors != NULL);
  ASSERT_EQ(cfg.vhosts[0].cors->enabled, 1);
  ASSERT_STR_EQ(cfg.vhosts[0].cors->allow_origin, "https://app.example.com");
  ASSERT_STR_EQ(cfg.vhosts[0].cors->allow_methods, "GET,POST,OPTIONS");
  ASSERT_STR_EQ(cfg.vhosts[0].cors->allow_headers, "Content-Type,Authorization");
  ASSERT_EQ(cfg.vhosts[0].cors->allow_credentials, 1);
  ASSERT_EQ(cfg.vhosts[0].cors->max_age_seconds, 3600u);
  ASSERT((cfg.vhosts[0].features & CFG_FEAT_CORS) != 0);

  free_loaded_cfg_heap(&cfg);

  unlink(path);
  PASS();
}

TEST t_cors_wildcard_with_credentials_fails(void) {
  const char *ini = "[vhost badcors]\n"
                    "bind = 127.0.0.1\n"
                    "port = 8105\n"
                    "cors = true\n"
                    "cors_allow_origin = *\n"
                    "cors_allow_credentials = true\n";

  char path[256];
  ASSERT_EQ(write_temp_ini(ini, path), 0);

  struct config_t cfg;
  char err[256];
  ASSERT_EQ(init_cfg(&cfg), 0);
  ASSERT_EQ(config_load_ini(path, &cfg, err), -1);
  ASSERT(strstr(err, "cannot be combined") != NULL);

  unlink(path);
  PASS();
}

TEST t_route_inherited_cors_wildcard_with_credentials_fails(void) {
  const char *ini = "[vhost inherited_badcors]\n"
                    "bind = 127.0.0.1\n"
                    "port = 8106\n"
                    "cors = true\n"
                    "cors_allow_origin = *\n"
                    "\n"
                    "[route api]\n"
                    "vhost = inherited_badcors\n"
                    "path_prefix = /api\n"
                    "cors_allow_credentials = true\n";

  char path[256];
  ASSERT_EQ(write_temp_ini(ini, path), 0);

  struct config_t cfg;
  char err[256];
  ASSERT_EQ(init_cfg(&cfg), 0);
  ASSERT_EQ(config_load_ini(path, &cfg, err), -1);
  ASSERT(strstr(err, "cannot be combined") != NULL);

  unlink(path);
  PASS();
}

TEST t_route_index_longest_prefix_order(void) {
  const char *ini = "[vhost main]\n"
                    "bind = 127.0.0.1\n"
                    "port = 8106\n"
                    "\n"
                    "[route api]\n"
                    "vhost = main\n"
                    "path_prefix = /api\n"
                    "\n"
                    "[route api_v2]\n"
                    "vhost = main\n"
                    "path_prefix = /api/v2\n"
                    "cors = true\n"
                    "\n"
                    "[route root]\n"
                    "vhost = main\n"
                    "path_prefix = /\n";

  char path[256];
  ASSERT_EQ(write_temp_ini(ini, path), 0);

  struct config_t cfg;
  char err[256];
  ASSERT_EQ(init_cfg(&cfg), 0);
  ASSERT_EQ(config_load_ini(path, &cfg, err), 0);

  ASSERT_EQ(cfg.route_rule_count, 3);
  ASSERT_EQ(cfg.vhosts[0].route_rule_count, 3u);

  const struct route_policy_rule *r0 = cfg.vhosts[0].route_rules[0];
  const struct route_policy_rule *r1 = cfg.vhosts[0].route_rules[1];
  const struct route_policy_rule *r2 = cfg.vhosts[0].route_rules[2];

  ASSERT(route_ptr_in_cfg_storage(&cfg, r0));
  ASSERT(route_ptr_in_cfg_storage(&cfg, r1));
  ASSERT(route_ptr_in_cfg_storage(&cfg, r2));

  ASSERT_STR_EQ(r0->path_prefix, "/api/v2");
  ASSERT_STR_EQ(r1->path_prefix, "/api");
  ASSERT_STR_EQ(r2->path_prefix, "/");

  ASSERT((cfg.vhosts[0].features & CFG_FEAT_CORS) != 0);

  free_loaded_cfg_heap(&cfg);

  unlink(path);
  PASS();
}

TEST t_route_index_tie_uses_declaration_order(void) {
  const char *ini = "[vhost main]\n"
                    "bind = 127.0.0.1\n"
                    "port = 8107\n"
                    "\n"
                    "[route first]\n"
                    "vhost = main\n"
                    "path_prefix = /foo\n"
                    "\n"
                    "[route second]\n"
                    "vhost = main\n"
                    "path_prefix = /bar\n";

  char path[256];
  ASSERT_EQ(write_temp_ini(ini, path), 0);

  struct config_t cfg;
  char err[256];
  ASSERT_EQ(init_cfg(&cfg), 0);
  ASSERT_EQ(config_load_ini(path, &cfg, err), 0);

  ASSERT_EQ(cfg.route_rule_count, 2);
  ASSERT_EQ(cfg.vhosts[0].route_rule_count, 2u);

  const struct route_policy_rule *r0 = cfg.vhosts[0].route_rules[0];
  const struct route_policy_rule *r1 = cfg.vhosts[0].route_rules[1];

  ASSERT(route_ptr_in_cfg_storage(&cfg, r0));
  ASSERT(route_ptr_in_cfg_storage(&cfg, r1));

  ASSERT_STR_EQ(r0->path_prefix, "/foo");
  ASSERT_STR_EQ(r1->path_prefix, "/bar");

  free_loaded_cfg_heap(&cfg);

  unlink(path);
  PASS();
}

TEST t_route_parse_capacity_grows_with_route_count(void) {
  const char *ini = "[vhost main]\n"
                    "bind = 127.0.0.1\n"
                    "port = 8115\n"
                    "\n"
                    "[route r1]\n"
                    "vhost = main\n"
                    "path_prefix = /a\n"
                    "\n"
                    "[route r2]\n"
                    "vhost = main\n"
                    "path_prefix = /ab\n"
                    "\n"
                    "[route r3]\n"
                    "vhost = main\n"
                    "path_prefix = /abc\n"
                    "\n"
                    "[route r4]\n"
                    "vhost = main\n"
                    "path_prefix = /abcd\n"
                    "\n"
                    "[route r5]\n"
                    "vhost = main\n"
                    "path_prefix = /abcde\n"
                    "\n"
                    "[route r6]\n"
                    "vhost = main\n"
                    "path_prefix = /abcdef\n"
                    "\n"
                    "[route r7]\n"
                    "vhost = main\n"
                    "path_prefix = /abcdefg\n"
                    "\n"
                    "[route r8]\n"
                    "vhost = main\n"
                    "path_prefix = /abcdefgh\n"
                    "\n"
                    "[route r9]\n"
                    "vhost = main\n"
                    "path_prefix = /abcdefghi\n";

  char path[256];
  ASSERT_EQ(write_temp_ini(ini, path), 0);

  struct config_t cfg;
  char err[256];
  ASSERT_EQ(init_cfg(&cfg), 0);
  ASSERT_EQ(config_load_ini(path, &cfg, err), 0);

  ASSERT_EQ(cfg.route_rule_count, 9);
  ASSERT_EQ(cfg.vhosts[0].route_rule_count, 9u);
  ASSERT_EQ(cfg.vhosts[0].route_rule_cap, 9u);

  for (uint16_t i = 0; i < cfg.vhosts[0].route_rule_count; ++i) {
    ASSERT(route_ptr_in_cfg_storage(&cfg, cfg.vhosts[0].route_rules[i]));
  }
  ASSERT_STR_EQ(cfg.vhosts[0].route_rules[0]->path_prefix, "/abcdefghi");
  ASSERT_STR_EQ(cfg.vhosts[0].route_rules[8]->path_prefix, "/a");

  free_loaded_cfg_heap(&cfg);
  unlink(path);
  PASS();
}

TEST t_route_unknown_vhost_fails(void) {
  const char *ini = "[vhost main]\n"
                    "bind = 127.0.0.1\n"
                    "port = 8108\n"
                    "\n"
                    "[route bad]\n"
                    "vhost = missing\n"
                    "path_prefix = /api\n";

  char path[256];
  ASSERT_EQ(write_temp_ini(ini, path), 0);

  struct config_t cfg;
  char err[256];
  ASSERT_EQ(init_cfg(&cfg), 0);
  ASSERT_EQ(config_load_ini(path, &cfg, err), -1);
  ASSERT(strstr(err, "unknown vhost") != NULL);

  unlink(path);
  PASS();
}

TEST t_route_missing_required_fields_fail(void) {
  const char *ini = "[vhost main]\n"
                    "bind = 127.0.0.1\n"
                    "port = 8109\n"
                    "\n"
                    "[route missing_path]\n"
                    "vhost = main\n";

  char path[256];
  ASSERT_EQ(write_temp_ini(ini, path), 0);

  struct config_t cfg;
  char err[256];
  ASSERT_EQ(init_cfg(&cfg), 0);
  ASSERT_EQ(config_load_ini(path, &cfg, err), -1);
  ASSERT(strstr(err, "missing required key 'path_prefix'") != NULL);

  unlink(path);
  PASS();
}

TEST t_alloc_fail_candidate_keeps_cfg_unchanged(void) {
  const char *ini = "[vhost main]\n"
                    "bind = 127.0.0.1\n"
                    "port = 8110\n";

  char path[256];
  ASSERT_EQ(write_temp_ini(ini, path), 0);

  struct config_t cfg;
  char err[256];
  ASSERT_EQ(init_cfg(&cfg), 0);
  cfg.vhost_count = 7;
  cfg.route_rule_count = 3;
  cfg.g.present = GF_QUEUE_DEPTH;
  cfg.g.queue_depth = 4242u;
  struct config_t before = cfg;

  g_config_ini_alloc_failpoint = CONFIG_INI_ALLOC_FAIL_CANDIDATE;
  ASSERT_EQ(config_load_ini(path, &cfg, err), -1);
  ASSERT(strstr(err, "candidate config") != NULL);
  ASSERT_EQ(memcmp(&cfg, &before, sizeof(cfg)), 0);

  unlink(path);
  PASS();
}

TEST t_alloc_fail_parse_ctx_keeps_cfg_unchanged(void) {
  const char *ini = "[vhost main]\n"
                    "bind = 127.0.0.1\n"
                    "port = 8111\n";

  char path[256];
  ASSERT_EQ(write_temp_ini(ini, path), 0);

  struct config_t cfg;
  char err[256];
  ASSERT_EQ(init_cfg(&cfg), 0);
  cfg.vhost_count = 5;
  cfg.route_rule_count = 2;
  cfg.g.present = GF_WORKERS;
  cfg.g.workers = 9u;
  struct config_t before = cfg;

  g_config_ini_alloc_failpoint = CONFIG_INI_ALLOC_FAIL_PARSE_CTX;
  ASSERT_EQ(config_load_ini(path, &cfg, err), -1);
  ASSERT(strstr(err, "parse context") != NULL);
  ASSERT_EQ(memcmp(&cfg, &before, sizeof(cfg)), 0);

  unlink(path);
  PASS();
}

TEST t_alloc_fail_route_index_keeps_cfg_unchanged(void) {
  const char *ini = "[vhost main]\n"
                    "bind = 127.0.0.1\n"
                    "port = 8112\n"
                    "\n"
                    "[route api]\n"
                    "vhost = main\n"
                    "path_prefix = /api\n";

  char path[256];
  ASSERT_EQ(write_temp_ini(ini, path), 0);

  struct config_t cfg;
  char err[256];
  ASSERT_EQ(init_cfg(&cfg), 0);
  cfg.vhost_count = 2;
  cfg.route_rule_count = 1;
  cfg.g.present = GF_PRE_ACCEPTS;
  cfg.g.pre_accepts = 77u;
  struct config_t before = cfg;

  g_config_ini_alloc_failpoint = CONFIG_INI_ALLOC_FAIL_ROUTE_INDEX;
  ASSERT_EQ(config_load_ini(path, &cfg, err), -1);
  ASSERT(err[0] != '\0');
  ASSERT_EQ(memcmp(&cfg, &before, sizeof(cfg)), 0);

  unlink(path);
  PASS();
}

TEST t_alloc_fail_route_parse_keeps_cfg_unchanged(void) {
  const char *ini = "[vhost main]\n"
                    "bind = 127.0.0.1\n"
                    "port = 8113\n"
                    "\n"
                    "[route api]\n"
                    "vhost = main\n"
                    "path_prefix = /api\n";

  char path[256];
  ASSERT_EQ(write_temp_ini(ini, path), 0);

  struct config_t cfg;
  char err[256];
  ASSERT_EQ(init_cfg(&cfg), 0);
  cfg.vhost_count = 4;
  cfg.route_rule_count = 1;
  cfg.g.present = GF_WORKERS;
  cfg.g.workers = 5u;
  struct config_t before = cfg;

  g_config_ini_alloc_failpoint = CONFIG_INI_ALLOC_FAIL_ROUTE_PARSE;
  ASSERT_EQ(config_load_ini(path, &cfg, err), -1);
  ASSERT(err[0] != '\0');
  ASSERT_EQ(memcmp(&cfg, &before, sizeof(cfg)), 0);

  unlink(path);
  PASS();
}

TEST t_alloc_fail_route_resolved_keeps_cfg_unchanged(void) {
  const char *ini = "[vhost main]\n"
                    "bind = 127.0.0.1\n"
                    "port = 8114\n"
                    "\n"
                    "[route api]\n"
                    "vhost = main\n"
                    "path_prefix = /api\n";

  char path[256];
  ASSERT_EQ(write_temp_ini(ini, path), 0);

  struct config_t cfg;
  char err[256];
  ASSERT_EQ(init_cfg(&cfg), 0);
  cfg.vhost_count = 3;
  cfg.route_rule_count = 2;
  cfg.g.present = GF_PRE_ACCEPTS;
  cfg.g.pre_accepts = 101u;
  struct config_t before = cfg;

  g_config_ini_alloc_failpoint = CONFIG_INI_ALLOC_FAIL_ROUTE_RESOLVED;
  ASSERT_EQ(config_load_ini(path, &cfg, err), -1);
  ASSERT(err[0] != '\0');
  ASSERT_EQ(memcmp(&cfg, &before, sizeof(cfg)), 0);

  unlink(path);
  PASS();
}

TEST t_alloc_fail_vhost_route_list_keeps_cfg_unchanged(void) {
  const char *ini = "[vhost main]\n"
                    "bind = 127.0.0.1\n"
                    "port = 8116\n"
                    "\n"
                    "[route api]\n"
                    "vhost = main\n"
                    "path_prefix = /api\n";

  char path[256];
  ASSERT_EQ(write_temp_ini(ini, path), 0);

  struct config_t cfg;
  char err[256];
  ASSERT_EQ(init_cfg(&cfg), 0);
  cfg.vhost_count = 6;
  cfg.route_rule_count = 4;
  cfg.g.present = GF_QUEUE_DEPTH;
  cfg.g.queue_depth = 3333u;
  struct config_t before = cfg;

  g_config_ini_alloc_failpoint = CONFIG_INI_ALLOC_FAIL_VHOST_ROUTE_LIST;
  ASSERT_EQ(config_load_ini(path, &cfg, err), -1);
  ASSERT(strstr(err, "resolving route list") != NULL);
  ASSERT_EQ(memcmp(&cfg, &before, sizeof(cfg)), 0);

  unlink(path);
  PASS();
}

SUITE(config_ini_greatest) {
  RUN_TEST(t_config_ini_parses_globals_and_vhost);
  RUN_TEST(t_warns_linklocal_without_zone);
  RUN_TEST(t_warns_multiple_wildcard_vhosts);
  RUN_TEST(t_applies_default_max_header_fields);
  RUN_TEST(t_invalid_port_fails);
  RUN_TEST(t_invalid_bool_warns_and_ignores);
  RUN_TEST(t_unknown_globals_ignored);
  RUN_TEST(t_shutdown_grace_ms_parsed);
  RUN_TEST(t_shutdown_grace_ms_invalid_fails);
  RUN_TEST(t_workers_parsed);
  RUN_TEST(t_workers_invalid_fails);
  RUN_TEST(t_wake_pipe_mode_parsed);
  RUN_TEST(t_wake_pipe_mode_alias_rejected);
  RUN_TEST(t_access_log_globals_parsed);
  RUN_TEST(t_access_log_sample_invalid_fails);
  RUN_TEST(t_access_log_min_status_invalid_fails);
  RUN_TEST(t_docroot_open_failure_nonfatal);
  RUN_TEST(t_all_vhosts_preserved_over_16);
  RUN_TEST(t_unknown_vhost_key_warns_and_ignores);
  RUN_TEST(t_hostname_bind_normalizes_to_numeric);
  RUN_TEST(t_hostname_bind_failure_returns_error);
  RUN_TEST(t_tls_globals_parsed);
  RUN_TEST(t_tls_vhost_enable_inherits_global_cert_key);
  RUN_TEST(t_tls_vhost_overrides_ticket_cache_flags);
  RUN_TEST(t_tls_enabled_without_key_fails);
  RUN_TEST(t_header_set_valid_stored);
  RUN_TEST(t_header_set_rejects_no_colon);
  RUN_TEST(t_header_set_rejects_bad_token_char);
  RUN_TEST(t_header_set_rejects_crlf_injection);
  RUN_TEST(t_header_set_rejects_overlength);
  RUN_TEST(t_header_set_max_16_enforced);
  RUN_TEST(t_header_set_rejects_over_budget);
  RUN_TEST(t_index_default_is_empty);
  RUN_TEST(t_index_custom_stored);
  RUN_TEST(t_index_rejects_slash);
  RUN_TEST(t_index_rejects_dotdot);
  RUN_TEST(t_auth_basic_file_and_realm_parsed);
  RUN_TEST(t_auth_realm_invalid_rejected);
  RUN_TEST(t_auth_basic_file_ignored_when_auth_disabled);
  RUN_TEST(t_auth_basic_file_load_failure_returns_error);
  RUN_TEST(t_auth_load_failure_keeps_cfg_unchanged);
  RUN_TEST(t_route_auth_modes_parsed);
  RUN_TEST(t_route_auth_invalid_mode_fails);
  RUN_TEST(t_route_auth_require_without_store_fails);
  RUN_TEST(t_route_auth_require_on_public_vhost_loads_store_and_captures_auth);
  RUN_TEST(t_route_max_body_bytes_parsed);
  RUN_TEST(t_route_max_body_bytes_absent_inherits_default);
  RUN_TEST(t_route_max_body_bytes_invalid_values_fail);
  RUN_TEST(t_cors_and_security_headers_parsed);
  RUN_TEST(t_cors_wildcard_with_credentials_fails);
  RUN_TEST(t_route_inherited_cors_wildcard_with_credentials_fails);
  RUN_TEST(t_route_index_longest_prefix_order);
  RUN_TEST(t_route_index_tie_uses_declaration_order);
  RUN_TEST(t_route_parse_capacity_grows_with_route_count);
  RUN_TEST(t_route_unknown_vhost_fails);
  RUN_TEST(t_route_missing_required_fields_fail);
  RUN_TEST(t_alloc_fail_candidate_keeps_cfg_unchanged);
  RUN_TEST(t_alloc_fail_parse_ctx_keeps_cfg_unchanged);
  RUN_TEST(t_alloc_fail_route_parse_keeps_cfg_unchanged);
  RUN_TEST(t_alloc_fail_route_resolved_keeps_cfg_unchanged);
  RUN_TEST(t_alloc_fail_route_index_keeps_cfg_unchanged);
  RUN_TEST(t_alloc_fail_vhost_route_list_keeps_cfg_unchanged);
}

GREATEST_MAIN_DEFS();
int main(int argc, char **argv) {
  GREATEST_MAIN_BEGIN();
  RUN_SUITE(config_ini_greatest);
  GREATEST_MAIN_END();
}
