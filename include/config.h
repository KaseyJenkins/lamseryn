#pragma once

// Centralized server configuration defaults.
// Runtime policy defaults here are internal fallbacks used when INI keys are absent.

#define SERVER_DEFAULT_PORT 8080
#define SERVER_BACKLOG 1024
#define IOURING_QUEUE_DEPTH 2048
#define READ_BUFFER_SIZE 8192
#define WRITE_BUFFER_SIZE 8192

enum {
	DEFAULT_INITIAL_IDLE_TIMEOUT_MS = 1000u,
	DEFAULT_KEEPALIVE_IDLE_CLOSE_MS = 5000u,
	DEFAULT_HEADER_TIMEOUT_MS = 30000u,
	DEFAULT_BODY_TIMEOUT_MS = 30000u,
	DEFAULT_WRITE_TIMEOUT_MS = 10000u,
	DEFAULT_DRAIN_TIMEOUT_MS = 2000u,
	DEFAULT_ACCEPT_BACKOFF_MS = 5u,
};

// Socket buffer tuning (per-connection kernel memory clamps).
#define CONFIG_SOCK_SND_BUF (32 * 1024)
#define CONFIG_SOCK_RCV_BUF (32 * 1024)

// TCP notsent low-watermark threshold.
#define CONFIG_TCP_NOTSENT_LOWAT (16 * 1024)

// Optional listener tuning.
#define CONFIG_TCP_DEFER_ACCEPT_SEC 1

// Initial pre-accept operations per thread.
#define CONFIG_PRE_ACCEPTS 1

// ---- Feature toggles (0/1) ----
// Prefer numeric flags to support direct `#if ENABLE_...` checks.
#ifndef ENABLE_OVERLOAD_503
#define ENABLE_OVERLOAD_503 1
#endif

// Backward compatibility: honor legacy OVERLOAD_503 if it is defined.
#if defined(OVERLOAD_503) && !defined(ENABLE_OVERLOAD_503)
#define ENABLE_OVERLOAD_503 1
#endif

// Optional compile-time feature switch.
#ifndef ENABLE_MULTISHOT_ACCEPT
#define ENABLE_MULTISHOT_ACCEPT 1
#endif
