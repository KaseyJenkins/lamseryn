// Centralized accept policy (success, errors, backoff, rearm).

#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <liburing.h>

#include "instrumentation/instrumentation.h"
#include "instrumentation/counters_update.h"
#include "include/config.h"
#include "include/macros.h"
#include "include/logger.h"
#include "include/net_server.h"
#include "include/worker_ctx.h"
#include "include/accept_controller.h"
#include "include/ring_ops.h"

static inline int accept_can_rearm(const struct worker_ctx *w) {
  // Do not rearm while accepts are paused or while cancel is in flight.
  return !w->accept_paused && !w->accept_cancel_inflight;
}

static inline void accept_mark_paused(struct worker_ctx *w) {
  if (w && !w->accept_paused) {
    w->accept_paused = 1;
  }
}

static inline void accept_backoff_init_op(struct worker_ctx *w) {
  if (!w) {
    return;
  }
  if (w->accept_backoff_static.magic == OP_MAGIC) {
    return;
  }
  w->accept_backoff_static =
    (struct op_ctx){.magic = OP_MAGIC, .type = OP_ACCEPT_BACKOFF, .c = NULL, .fd = -1};
}

// Arm a short accept-backoff timer (batched, urgent flush).
// Returns 1 when newly armed.
static inline int accept_arm_backoff_timer(struct worker_ctx *w) {
  if (!w || w->accept_backoff_armed) {
    return 0;
  }

  accept_backoff_init_op(w);

  unsigned backoff_ms = ACCEPT_BACKOFF_MS;
  if (w->cfg.config && w->cfg.config->g.present & GF_ACCEPT_BACKOFF_MS) {
    backoff_ms = w->cfg.config->g.accept_backoff_ms;
  }

  struct __kernel_timespec ts = {.tv_sec = 0, .tv_nsec = (long)backoff_ms * 1000000};
  struct io_uring_sqe *sqe_t = get_sqe_batching(w);
  if (!sqe_t) {
    return 0;
  }

  io_uring_prep_timeout(sqe_t, &ts, 0, 0);
  io_uring_sqe_set_data64(sqe_t, (uint64_t)(uintptr_t)&w->accept_backoff_static);
  w->accept_backoff_armed = 1;
  mark_post(w);
  maybe_flush(w, /*urgent=*/1);
  return 1;
}

// Cancel in-flight accept SQEs across listeners (batched, urgent flush).
// Returns 1 if we consider cancel-inflight active after this call.
static inline int accept_maybe_stage_cancel_all(struct worker_ctx *w) {
  if (!w) {
    return 0;
  }
  if (w->accept_cancel_inflight) {
    return 1;
  }

  int posted = 0;
  for (int i = 0; i < w->num_listeners; ++i) {
    int lfd2 = w->listen_fds[i];
    struct io_uring_sqe *sqe_c = get_sqe_batching(w);
    if (!sqe_c) {
      break;
    }
#ifdef IORING_ASYNC_CANCEL_ALL
    io_uring_prep_cancel_fd(sqe_c, lfd2, IORING_ASYNC_CANCEL_ALL);
#else
    io_uring_prep_cancel_fd(sqe_c, lfd2, 0);
#endif
    io_uring_sqe_set_data64(sqe_c, 0);
    mark_post(w);
    posted++;
  }
  maybe_flush(w, /*urgent=*/1);
  if (posted == w->num_listeners) {
    w->accept_cancel_inflight = 1;
  }
  return w->accept_cancel_inflight;
}

static inline void accept_resume_from_backoff(struct worker_ctx *w) {
  if (!w) {
    return;
  }
  w->accept_backoff_armed = 0;
  w->accept_paused = 0;
  w->accept_cancel_inflight = 0;
}

// Post one accept re-arm SQE for the given listener/op context.
// Returns 1 if an SQE was posted, 0 otherwise.
static inline int accept_post_one(struct worker_ctx *w, int lfd, struct op_ctx *opacc) {
  if (!w || !opacc) {
    return 0;
  }

  struct io_uring_sqe *sqe_a = get_sqe_batching(w);
  if (!sqe_a) {
    return 0;
  }

#if ENABLE_MULTISHOT_ACCEPT && defined(IORING_ACCEPT_MULTISHOT)
  if (w->accept_multishot) {
    io_uring_prep_multishot_accept(sqe_a, lfd, NULL, NULL, ACCEPT_FLAGS);
  } else
#endif
  {
    io_uring_prep_accept(sqe_a, lfd, NULL, NULL, ACCEPT_FLAGS);
  }
  io_uring_sqe_set_data64(sqe_a, (uint64_t)(uintptr_t)opacc);
  mark_post(w);
  return 1;
}

int accept_arm_startup(struct worker_ctx *w) {
  if (!w) {
    return 0;
  }

  int accept_armed = 0;
  for (int i = 0; i < w->num_listeners; ++i) {
    int lfd = w->listen_fds[i];

    if (!w->accept_inited[i]) {
      w->accept_static[i] =
        (struct op_ctx){.magic = OP_MAGIC, .type = OP_ACCEPT, .c = NULL, .fd = lfd};
      w->accept_inited[i] = 1;
    }

#if ENABLE_MULTISHOT_ACCEPT && defined(IORING_ACCEPT_MULTISHOT)
    struct io_uring_sqe *sqe = get_sqe_batching(w);
    if (sqe) {
      io_uring_prep_multishot_accept(sqe, lfd, NULL, NULL, ACCEPT_FLAGS);
      io_uring_sqe_set_data64(sqe, (uint64_t)(uintptr_t)&w->accept_static[i]);
      w->accept_multishot = 1;
      accept_armed = 1;
      mark_post(w);
    }
#else
    int posted = 0;
    for (int k = 0; k < PRE_ACCEPTS; ++k) {
      struct io_uring_sqe *sqe1 = get_sqe_batching(w);
      if (!sqe1) {
        break;
      }
      io_uring_prep_accept(sqe1, lfd, NULL, NULL, ACCEPT_FLAGS);
      io_uring_sqe_set_data64(sqe1, (uint64_t)(uintptr_t)&w->accept_static[i]);
      mark_post(w);
      posted++;
    }
    if (posted) {
      accept_armed = 1;
    }
#endif
  }

  return accept_armed;
}

void accept_close_listeners(struct worker_ctx *w) {
  if (!w) {
    return;
  }

  for (int i = 0; i < w->num_listeners; ++i) {
    if (w->listen_fds[i] >= 0) {
      close(w->listen_fds[i]);
      w->listen_fds[i] = -1;
    }
    w->accept_inited[i] = 0;
  }
  w->num_listeners = 0;
}

int accept_setup_listeners(struct worker_ctx *w) {
  if (!w || !w->cfg.config) {
    return -1;
  }

  w->num_listeners = 0;
  for (int i = 0; i < w->cfg.config->vhost_count; ++i) {
    const struct vhost_t *vh = &w->cfg.config->vhosts[i];
    if (!vh->port) {
      continue;
    }

    const char *bind_addr = (vh->bind[0] ? vh->bind : "0.0.0.0");
    int lfd = create_listening_socket_bind_port(bind_addr, vh->port, SOCK_STREAM, SERVER_BACKLOG);
    if (lfd < 0) {
      LOGE(LOGC_ACCEPT, "listen socket failed on %s:%u", bind_addr, (unsigned)vh->port);
      accept_close_listeners(w);
      return -1;
    }

    if (w->num_listeners >= (int)(sizeof(w->listen_fds) / sizeof(w->listen_fds[0]))) {
      LOGE(LOGC_ACCEPT,
           "Too many listeners (max %zu)",
           sizeof(w->listen_fds) / sizeof(w->listen_fds[0]));
      close(lfd);
      accept_close_listeners(w);
      return -1;
    }

    w->listen_fds[w->num_listeners++] = lfd;
    LOGI(LOGC_CORE,
         "listening on %s:%u (core %d), lfd=%d",
         bind_addr,
         (unsigned)vh->port,
         w->cfg.cpu_core,
         lfd);
  }

  return 0;
}

void accept_handle_cqe(struct worker_ctx *w, struct op_ctx *opacc, struct io_uring_cqe *cqe) {
  int lfd = opacc->fd;
  int cfd = cqe->res;
  int more = (cqe->flags & IORING_CQE_F_MORE) ? 1 : 0;

  if (cfd >= 0) {
    CTR_INC_OPS(w, cnt_accept_ok);

    worker_accept_success(w, cfd);

    // Rearm accepts (batched, urgent flush for single-shot mode).
#if ENABLE_MULTISHOT_ACCEPT && defined(IORING_ACCEPT_MULTISHOT)
    if (accept_can_rearm(w) && (!w->accept_multishot || !more)) {
      (void)accept_post_one(w, lfd, opacc);
      if (!w->accept_multishot) {
        maybe_flush(w, /*urgent=*/1);
      }
    }
#else
    if (accept_can_rearm(w)) {
      (void)accept_post_one(w, lfd, opacc);
      maybe_flush(w, /*urgent=*/1);
    }
#endif
    return;
  }

  // Error path.
  int err = -cfd;
  CTR_INC_OPS(w, cnt_accept_err);
  if (err == EAGAIN) {
      CTR_INC_DEV(w, cnt_accept_eagain);
  } else if (err == EMFILE) {
    CTR_INC_OPS(w, cnt_accept_emfile);
  } else if (err == ENFILE) {
    CTR_INC_OPS(w, cnt_accept_enfile);
  } else {
    CTR_INC_OPS(w, cnt_accept_other);
  }

  // Downgrade to single-shot when multishot is unsupported.
  if (w->accept_multishot && (err == EINVAL || err == EOPNOTSUPP)) {
    w->accept_multishot = 0;
  }

  if (err == EMFILE || err == ENFILE) {
    // Pause, arm backoff, then stage cancel-all for accepts.
    accept_mark_paused(w);
    (void)accept_arm_backoff_timer(w);
    (void)accept_maybe_stage_cancel_all(w);
  } else {
    // Normal rearm on error (batched, urgent for single-shot mode).
    if (err == ECANCELED && w->accept_paused) {
    } else if (accept_can_rearm(w) && (!w->accept_multishot || !more)) {
      (void)accept_post_one(w, lfd, opacc);
      maybe_flush(w, /*urgent=*/1);
    }
  }
}

void accept_handle_backoff(struct worker_ctx *w) {
  accept_resume_from_backoff(w);

  for (int i = 0; i < w->num_listeners; ++i) {
    int lfd = w->listen_fds[i];
    if (!accept_post_one(w, lfd, &w->accept_static[i])) {
      break;
    }
  }
  // Promptly flush batched rearms.
  maybe_flush(w, /*urgent=*/1);
}

void accept_enter_drain(struct worker_ctx *w) {
  if (!w) {
    return;
  }

  // Stop new accept rearms immediately.
  accept_mark_paused(w);
  w->accept_backoff_armed = 0;

  // Best effort cancel of any in-flight accept SQEs.
  (void)accept_maybe_stage_cancel_all(w);
}

int accept_try_handle_cqe(struct worker_ctx *w, struct op_ctx *op, struct io_uring_cqe *cqe) {
  if (!w || !op || !cqe) {
    return 0;
  }

  if (op->type == OP_ACCEPT) {
    accept_handle_cqe(w, op, cqe);
    return 1;
  }
  if (op->type == OP_ACCEPT_BACKOFF) {
    accept_handle_backoff(w);
    return 1;
  }
  return 0;
}

int accept_is_fd_pressure(const struct worker_ctx *w) {
  if (!w) {
    return 0;
  }
  return w->accept_paused || w->accept_cancel_inflight;
}
