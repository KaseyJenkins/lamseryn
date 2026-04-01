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
  ASSERT_EQ(config_set_defaults(&cfg), 0);
  ASSERT_EQ(config_load_ini(path, &cfg, err), 0);

  ASSERT_EQ(cfg.vhost_count, 1);
  ASSERT_EQ(cfg.vhosts[0].port, (uint16_t)8080);
  ASSERT_EQ(strcmp(cfg.vhosts[0].bind, "127.0.0.1"), 0);
  ASSERT_EQ(cfg.vhosts[0].max_header_fields, (uint16_t)55);
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
  ASSERT_EQ(config_set_defaults(&cfg), 0);
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
  ASSERT_EQ(config_set_defaults(&cfg), 0);
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
  ASSERT_EQ(config_set_defaults(&cfg), 0);
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
  ASSERT_EQ(config_set_defaults(&cfg), 0);
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
  ASSERT_EQ(config_set_defaults(&cfg), 0);
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
  ASSERT_EQ(config_set_defaults(&cfg), 0);
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
  ASSERT_EQ(config_set_defaults(&cfg), 0);
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
  ASSERT_EQ(config_set_defaults(&cfg), 0);
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
  ASSERT_EQ(config_set_defaults(&cfg), 0);
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
  ASSERT_EQ(config_set_defaults(&cfg), 0);
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
  ASSERT_EQ(config_set_defaults(&cfg), 0);
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
  ASSERT_EQ(config_set_defaults(&cfg), 0);
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
  ASSERT_EQ(config_set_defaults(&cfg), 0);
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
  ASSERT_EQ(config_set_defaults(&cfg), 0);
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
  ASSERT_EQ(config_set_defaults(&cfg), 0);
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
  ASSERT_EQ(config_set_defaults(&cfg), 0);
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
  ASSERT_EQ(config_set_defaults(&cfg), 0);
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
  ASSERT_EQ(config_set_defaults(&cfg), 0);
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
  ASSERT_EQ(config_set_defaults(&cfg), 0);
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
  ASSERT_EQ(config_set_defaults(&cfg), 0);
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
  ASSERT_EQ(config_set_defaults(&cfg), 0);
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
  ASSERT_EQ(config_set_defaults(&cfg), 0);
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
  ASSERT_EQ(config_set_defaults(&cfg), 0);
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
  ASSERT_EQ(config_set_defaults(&cfg), 0);
  ASSERT_EQ(config_load_ini(path, &cfg, err), -1);
  ASSERT(strstr(err, "tls=true requires tls_cert_file and tls_key_file") != NULL);

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
}

GREATEST_MAIN_DEFS();
int main(int argc, char **argv) {
  GREATEST_MAIN_BEGIN();
  RUN_SUITE(config_ini_greatest);
  GREATEST_MAIN_END();
}
