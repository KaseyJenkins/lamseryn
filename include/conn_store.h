#pragma once

#include <stddef.h>

struct conn;

// Per-thread fd-to-conn map operations.
// Each worker maintains its own map; all functions operate on thread-local state.

int conn_store_init(size_t cap_hint);
void conn_store_free(void);
struct conn *conn_store_get(int fd);
int conn_store_put(int fd, struct conn *c);
void conn_store_del(int fd);
