#pragma once

// Cross-cutting utility macros.
#define UNUSED(x) (void)(x)

// Magic constants.
#define OP_MAGIC 0xC10ECAFEu

// Low-bit tagging relies on allocator alignment.
#define UD_TAG_BIT 1ull

#include <stddef.h>
#include <stdalign.h>
_Static_assert((alignof(max_align_t) & UD_TAG_BIT) == 0,
               "low-bit tag relies on malloc alignment being even");

// fd-to-conn map slot markers.
#define FM_EMPTY (-1)
#define FM_TOMB (-2)

// Thread-local storage macro (portable selection).
#if defined(__cplusplus)
#define THREAD_LOCAL thread_local
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
#define THREAD_LOCAL _Thread_local
#elif defined(__GNUC__) || defined(__clang__)
#define THREAD_LOCAL __thread
#else
#error "No thread-local storage keyword available for this compiler."
#endif

// Shared caps and timeouts.
// Intentionally overridable via build flags (-DNAME=value).
#ifndef HEADER_CAP
#define HEADER_CAP (64 * 1024) // 64 KB
#endif

#ifndef INITIAL_IDLE_TIMEOUT_MS
#define INITIAL_IDLE_TIMEOUT_MS 1000
#endif

#ifndef SWEEP_PERIOD_MS
#define SWEEP_PERIOD_MS 1000
#endif

#ifndef IDLE_CLOSE_MS
#define IDLE_CLOSE_MS 5000
#endif

#ifndef HEADER_TIMEOUT_MS
#define HEADER_TIMEOUT_MS 30000
#endif

// Accept backoff after EMFILE/ENFILE (override via -D).
#ifndef ACCEPT_BACKOFF_MS
#define ACCEPT_BACKOFF_MS 5
#endif

// Request body limits and slow-client defense (override via -D).
#ifndef MAX_BODY_BYTES
#define MAX_BODY_BYTES (1ull * 1024 * 1024) // 1 MiB
#endif

#ifndef BODY_TIMEOUT_MS
#define BODY_TIMEOUT_MS 30000
#endif

// Response write-side timeout/backpressure (override via -D).
// Applies to in-flight header/body sends and sendfile streaming.
#ifndef WRITE_TIMEOUT_MS
#define WRITE_TIMEOUT_MS 10000
#endif

// Timing-wheel defaults (override via -D).
#ifndef TW_TICK_MS
#define TW_TICK_MS 100
#endif
#ifndef TW_SLOTS
#define TW_SLOTS 1024
#endif

// Provided-buffer ring defaults.
#define RBUF_SZ 256
#define BUF_RING_ENTRIES 512
#define BUF_GROUP_BASE 1

// CQE batching and drain thresholds.
#ifndef CQE_BATCH
#define CQE_BATCH 64
#endif
#ifndef DRAIN_TIMEOUT_MS
#define DRAIN_TIMEOUT_MS 2000
#endif
#ifndef SUBMIT_BATCH_SZ
#define SUBMIT_BATCH_SZ 64
#endif
#ifndef SUBMIT_LOW_SPACE
#define SUBMIT_LOW_SPACE 16
#endif

// Accept sockets as non-blocking and close-on-exec.
// Avoids two fcntl() calls per accepted connection.
#ifndef ACCEPT_FLAGS
#define ACCEPT_FLAGS (SOCK_NONBLOCK | SOCK_CLOEXEC)
#endif
