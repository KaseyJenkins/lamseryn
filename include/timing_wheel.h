#pragma once

#include <stdint.h>

struct worker_ctx;
struct conn;

// Initialize timing-wheel state on the worker.
void tw_init(struct worker_ctx *w, uint32_t slots, uint32_t tick_ms, uint64_t now_ms);

// Compute and rearm the single active deadline for a connection.
void tw_reschedule(struct worker_ctx *w, struct conn *c, uint64_t now_ms);

// Remove an active deadline and clear deadline metadata.
void tw_cancel(struct worker_ctx *w, struct conn *c);

// Process one wheel tick and handle expired deadlines.
void tw_process_tick(struct worker_ctx *w, uint64_t now_ms);
