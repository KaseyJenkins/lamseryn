#pragma once
#include <stdint.h>

/*
 * Create a non-blocking listening socket bound to (bind_addr, port).
 * bind_addr may be numeric (for example "127.0.0.1", "::") or wildcard
 * ("0.0.0.0", "::", "*"). Returns fd >= 0 on success, -1 on failure.
 */
int create_listening_socket_bind_port(const char *bind_addr, uint16_t port, int type, int backlog);

// Apply socket buffer and TCP tuning for accepted connections.
void net_tune_socket_buffers(int fd);

// Set O_NONBLOCK on a file descriptor; returns 0 on success, -1 on error.
int net_set_nonblock(int fd);
