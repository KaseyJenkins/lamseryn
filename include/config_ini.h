#pragma once
struct config_t;

// Initialize configuration with built-in defaults.
int config_set_defaults(struct config_t *cfg);

// Load INI configuration from path into cfg.
// Returns 0 on success, -1 on parse/load failure.
int config_load_ini(const char *path, struct config_t *cfg, char err[256]);