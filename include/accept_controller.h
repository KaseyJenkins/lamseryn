#pragma once

#include <liburing.h>

struct worker_ctx;
struct op_ctx;

// Per-connection accept-success hook.
// Applies socket tuning, connection initialization, and initial HTTP pipeline setup.
void worker_accept_success(struct worker_ctx *w, int cfd);

// Process one accept CQE (success or failure/backoff/rearm path).
void accept_handle_cqe(struct worker_ctx *w, struct op_ctx *opacc, struct io_uring_cqe *cqe);

// Dispatch accept-related op types from the worker CQE loop.
// Returns non-zero when `op->type` is accept-owned and was handled.
int accept_try_handle_cqe(struct worker_ctx *w, struct op_ctx *op, struct io_uring_cqe *cqe);

// Arm initial accept SQEs for configured listeners; returns 1 if at least one accept was posted.
int accept_arm_startup(struct worker_ctx *w);

// Build per-worker listener set from config vhosts; returns 0 on success, -1 on error.
int accept_setup_listeners(struct worker_ctx *w);

// Close all configured listeners and reset listener bookkeeping.
void accept_close_listeners(struct worker_ctx *w);

// Backoff timeout handler: resume and rearm accepts for all listeners.
void accept_handle_backoff(struct worker_ctx *w);

// Enter shutdown-drain accept policy: pause new accepts, stage cancel for
// in-flight accept SQEs, and avoid rearm while existing connections drain.
void accept_enter_drain(struct worker_ctx *w);

// Returns non-zero when accept-side FD pressure is active so close paths
// can prefer urgent flush behavior.
int accept_is_fd_pressure(const struct worker_ctx *w);
