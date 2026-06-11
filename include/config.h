#pragma once

// Centralized server configuration defaults.
// Runtime policy defaults here are internal fallbacks used when INI keys are absent.

enum {
	DEFAULT_LISTEN_BACKLOG = 1024u,
	DEFAULT_IOURING_QUEUE_DEPTH = 2048u,
	DEFAULT_INITIAL_IDLE_TIMEOUT_MS = 1000u,
	DEFAULT_KEEPALIVE_IDLE_CLOSE_MS = 5000u,
	DEFAULT_HEADER_TIMEOUT_MS = 30000u,
	DEFAULT_BODY_TIMEOUT_MS = 30000u,
	DEFAULT_WRITE_TIMEOUT_MS = 10000u,
	DEFAULT_DRAIN_TIMEOUT_MS = 2000u,
	DEFAULT_ACCEPT_BACKOFF_MS = 5u,
	DEFAULT_MAX_HEADER_BYTES = 64u * 1024u,
	DEFAULT_MAX_BODY_BYTES = 1024u * 1024u,
	DEFAULT_TCP_DEFER_ACCEPT_SEC = 1u,
	DEFAULT_PRE_ACCEPTS = 1u,
};

// Socket buffer tuning (per-connection kernel memory clamps).
#ifndef CONFIG_SOCK_SND_BUF
#define CONFIG_SOCK_SND_BUF (32 * 1024)
#endif

#ifndef CONFIG_SOCK_RCV_BUF
#define CONFIG_SOCK_RCV_BUF (32 * 1024)
#endif

// TCP notsent low-watermark threshold.
#ifndef CONFIG_TCP_NOTSENT_LOWAT
#define CONFIG_TCP_NOTSENT_LOWAT (16 * 1024)
#endif

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
