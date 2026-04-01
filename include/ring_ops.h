#pragma once

#include <liburing.h>

#include "instrumentation/instrumentation.h"
#include "instrumentation/counters_update.h"
#include "include/macros.h"
#include "include/worker_ctx.h"

// Toggle: 1 = inline in the header (default), 0 = out-of-line in src/ring_ops.c
#ifndef RING_OPS_INLINE
#define RING_OPS_INLINE 1
#endif

#if RING_OPS_INLINE

// Single-submit SQE helper: get an SQE; if full, submit once and retry.
static inline struct io_uring_sqe *get_sqe_retry_once(struct io_uring *ring, int *did_submit) {
  if (did_submit) {
    *did_submit = 0; // ensure defined
  }
  struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
  if (!sqe) {
    (void)io_uring_submit(ring);
    if (did_submit) {
      *did_submit = 1;
    }
    sqe = io_uring_get_sqe(ring);
  }
  return sqe;
}

// Get an SQE without submitting; if full, do a single submit to free space and retry.
static inline struct io_uring_sqe *get_sqe_batching(struct worker_ctx *w) {
  struct io_uring_sqe *sqe = io_uring_get_sqe(&w->ring);
  if (!sqe) {
    if (w->need_submit) {
      CTR_ADD_DEV(w, cnt_sqes_flushed, w->sqes_pending);
      CTR_INC_DEV(w, cnt_submit_calls);
    } else {
      CTR_INC_DEV(w, cnt_submit_calls);
    }
    (void)io_uring_submit(&w->ring);
    w->need_submit = 0;
    w->sqes_pending = 0;

    sqe = io_uring_get_sqe(&w->ring);
  }
  return sqe;
}

// Mark that an SQE has been staged and should be flushed later.
static inline void mark_post(struct worker_ctx *w) {
  w->need_submit = 1;
  w->sqes_pending++;
  CTR_INC_DEV(w, cnt_sqes_posted);
}

// Flush staged SQEs based on urgency, batch size, or SQ-space pressure.
static inline void maybe_flush(struct worker_ctx *w, int urgent) {
  if (!w->need_submit) {
    return;
  }
  if (urgent || w->sqes_pending >= SUBMIT_BATCH_SZ
      || io_uring_sq_space_left(&w->ring) < SUBMIT_LOW_SPACE) {
    CTR_ADD_DEV(w, cnt_sqes_flushed, w->sqes_pending);
    CTR_INC_DEV(w, cnt_submit_calls);
    if (urgent) {
      CTR_INC_DEV(w, cnt_submit_urgent);
    }
    (void)io_uring_submit(&w->ring);
    w->need_submit = 0;
    w->sqes_pending = 0;
  }
}

// Unconditionally submit staged SQEs if any are pending.
static inline void submit_if_pending(struct worker_ctx *w) {
  if (!w || !w->need_submit) {
    return;
  }
  CTR_ADD_DEV(w, cnt_sqes_flushed, w->sqes_pending);
  CTR_INC_DEV(w, cnt_submit_calls);
  (void)io_uring_submit(&w->ring);
  w->need_submit = 0;
  w->sqes_pending = 0;
}

#else // RING_OPS_INLINE == 0

struct io_uring_sqe *get_sqe_retry_once(struct io_uring *ring, int *did_submit);
struct io_uring_sqe *get_sqe_batching(struct worker_ctx *w);
void mark_post(struct worker_ctx *w);
void maybe_flush(struct worker_ctx *w, int urgent);
void submit_if_pending(struct worker_ctx *w);

#endif
