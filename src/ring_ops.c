#include "include/ring_ops.h"

// Get an SQE; if full, submit once and retry.
struct io_uring_sqe *get_sqe_retry_once(struct io_uring *ring, int *did_submit) {
  if (did_submit) {
    *did_submit = 0;
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

// Get an SQE from batched path; submit once and retry if full.
struct io_uring_sqe *get_sqe_batching(struct worker_ctx *w) {
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

// Mark staged SQE for later submission.
void mark_post(struct worker_ctx *w) {
  w->need_submit = 1;
  w->sqes_pending++;
  CTR_INC_DEV(w, cnt_sqes_posted);
}

// Flush staged SQEs based on urgency, batch size, or SQ-space pressure.
void maybe_flush(struct worker_ctx *w, int urgent) {
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

// Submit pending staged SQEs.
void submit_if_pending(struct worker_ctx *w) {
  if (!w || !w->need_submit) {
    return;
  }
  CTR_ADD_DEV(w, cnt_sqes_flushed, w->sqes_pending);
  CTR_INC_DEV(w, cnt_submit_calls);
  (void)io_uring_submit(&w->ring);
  w->need_submit = 0;
  w->sqes_pending = 0;
}
