#include <errno.h>
#include <string.h>
#include <sys/poll.h>
#include <sys/sendfile.h>
#include <time.h>
#include <unistd.h>

#include "include/worker_loop.h"
#include "include/accept_controller.h"
#include "include/logger.h"
#include "include/conn_deadline.h"
#include "include/ring_ops.h"
#include "include/rx_buffers.h"
#include "include/tls.h"
#include "include/time_utils.h"
#include "include/timing_wheel.h"
#include "include/tx.h"

static inline void worker_loop_neutralize_sqe(struct io_uring_sqe *sqe) {
  io_uring_prep_nop(sqe);
  io_uring_sqe_set_data64(sqe, 0);
  sqe->flags = 0;
  sqe->buf_group = 0;
}

bool worker_loop_cancel_write_poll_if_armed(struct worker_ctx *w, struct conn *c) {
  if (!w || !c || !tx_pollout_is_armed(&c->tx)) {
    return false;
  }

  struct io_uring_sqe *sqe_rm = get_sqe_batching(w);
  if (!sqe_rm) {
    return false;
  }

  io_uring_prep_poll_remove(sqe_rm, (uint64_t)(uintptr_t)&c->op_write_ready);
  io_uring_sqe_set_data64(sqe_rm, 0);
  mark_post(w);
  tx_notify_poll_disarmed_staged(&c->tx);
  return true;
}

int worker_loop_post_tls_pollout(struct worker_ctx *w, struct conn *c) {
  if (!w || !c || c->fd < 0) {
    return -1;
  }
  if (conn_is_closing_no_deadline(c)) {
    return -1;
  }
  if (tx_pollout_is_armed(&c->tx)) {
    return 0;
  }

  struct io_uring_sqe *sqe = get_sqe_batching(w);
  if (!sqe) {
    return -1;
  }

  c->refcnt++;
  io_uring_prep_poll_add(sqe, c->fd, POLLOUT);
  io_uring_sqe_set_data64(sqe, (uint64_t)(uintptr_t)&c->op_write_ready);
  mark_post(w);
  tx_notify_poll_armed(&c->tx);
  return 0;
}

void worker_loop_write_post_remaining(struct worker_ctx *w,
                                      struct conn *c,
                                      int cfd,
                                      int guard_if_closing,
                                      const struct worker_loop_write_ops *ops) {
  if (!w || !c || !ops) {
    return;
  }

  struct tx_next_io next = {0};
  if (!tx_pending_headers(&c->tx, &next)) {
    return;
  }

  struct io_uring_sqe *sqe = get_sqe_batching(w);
  if (!sqe) {
    CTR_INC_DEV(w, cnt_sqe_starvation_close);
    if ((!guard_if_closing || !c->dl.closing) && ops->schedule_or_sync_close) {
      ops->schedule_or_sync_close(w, cfd);
    }
    return;
  }

  if (ops->conn_ref) {
    ops->conn_ref(c);
  }
  io_uring_prep_send(sqe, cfd, next.buf, next.len, MSG_NOSIGNAL);
  io_uring_sqe_set_data64(sqe, (uint64_t)(uintptr_t)&c->op_write);
  mark_post(w);
}

static void worker_loop_write_on_full_response(struct worker_ctx *w,
                                               struct conn *c,
                                               int cfd,
                                               const struct worker_loop_write_ops *ops) {
  if (ops->emit_access_log) {
    ops->emit_access_log(w, c);
  }

  enum tx_after_action action = tx_after_full_response(&c->tx, c->dl.draining);

  if (action == TX_AFTER_SHUT_WR_AND_READ) {
    if (ops->clear_write_timeout) {
      ops->clear_write_timeout(w, c);
    }
    shutdown(cfd, SHUT_WR);
    if (!c->tx.recv_armed && ops->post_recv_ptr) {
      ops->post_recv_ptr(w, c);
    }
    return;
  }

  if (action == TX_AFTER_KEEPALIVE_RESET) {
    int staged = 0;
    if (ops->clear_write_timeout) {
      ops->clear_write_timeout(w, c);
    }
    if (ops->conn_reset_request) {
      ops->conn_reset_request(w, c);
    }
    if (ops->conn_try_process_stash) {
      staged = ops->conn_try_process_stash(w, c, cfd);
    }

    if (!staged) {
      if (!c->tx.recv_armed && ops->post_recv_ptr) {
        ops->post_recv_ptr(w, c);
      }
      if (ops->tw_reschedule) {
        ops->tw_reschedule(w, c, w->now_cached_ms);
      }
    }
    return;
  }

  if (ops->clear_write_timeout) {
    ops->clear_write_timeout(w, c);
  }
  if (ops->schedule_or_sync_close) {
    ops->schedule_or_sync_close(w, cfd);
  }
}

static void worker_loop_write_arm_pollout(struct worker_ctx *w,
                                          struct conn *c,
                                          int cfd,
                                          const struct worker_loop_write_ops *ops) {
  if (!w || !c || cfd < 0 || !ops) {
    return;
  }
  if (!tx_should_arm_pollout(&c->tx)) {
    return;
  }

  if (ops->arm_write_timeout) {
    ops->arm_write_timeout(w, c);
  }

  int did_submit_p = 0;
  struct io_uring_sqe *sqe_p = get_sqe_retry_once(&w->ring, &did_submit_p);
  if (did_submit_p) {
    CTR_INC_DEV(w, cnt_submit_inline);
  }
  if (!sqe_p) {
    CTR_INC_DEV(w, cnt_sqe_starvation_close);
    if (!c->dl.closing && ops->schedule_or_sync_close) {
      ops->schedule_or_sync_close(w, cfd);
    }
    return;
  }

  if (ops->conn_ref) {
    ops->conn_ref(c);
  }
  io_uring_prep_poll_add(sqe_p, cfd, POLLOUT);
  io_uring_sqe_set_data64(sqe_p, (uint64_t)(uintptr_t)&c->op_write_ready);
  CTR_INC_DEV(w, cnt_sqes_direct_posted);
  int rc2 = io_uring_submit(&w->ring);
  CTR_INC_DEV(w, cnt_submit_direct);
  if (rc2 < 0) {
    worker_loop_neutralize_sqe(sqe_p);
    if (ops->conn_put) {
      ops->conn_put(c);
    }
    if (!c->dl.closing && ops->schedule_or_sync_close) {
      ops->schedule_or_sync_close(w, cfd);
    }
    return;
  }
  tx_notify_poll_armed(&c->tx);
}

static void worker_loop_tx_sendfile_progress(struct worker_ctx *w,
                                             struct conn *c,
                                             int cfd,
                                             const struct worker_loop_write_ops *ops) {
  if (!w || !c || cfd < 0 || !ops) {
    return;
  }

  if (c->tx.file_fd < 0 || c->tx.file_rem == 0) {
    if (ops->tx_close_file) {
      ops->tx_close_file(c);
    }
    worker_loop_write_on_full_response(w, c, cfd, ops);
    return;
  }

  while (c->tx.file_rem > 0) {
    size_t want = tx_next_sendfile_chunk(&c->tx);
    if (want == 0) {
      break;
    }

    ssize_t n = sendfile(cfd, c->tx.file_fd, &c->tx.file_off, want);
    ssize_t sent = n;
    if (n < 0) {
      sent = -(ssize_t)errno;
    }

    struct tx_next_io out = {0};
    enum tx_decision d = tx_on_io_result(&c->tx, TX_IO_SENDFILE, sent, &out);
    enum tx_sendfile_step step = tx_sendfile_step_from_decision(d);

    if (step == TX_SF_STEP_RESUME) {
      c->dl.last_active_ms = w->now_cached_ms;
      if (ops->arm_write_timeout) {
        ops->arm_write_timeout(w, c);
      }
      continue;
    }
    if (step == TX_SF_STEP_RETRY) {
      continue;
    }
    if (step == TX_SF_STEP_ARM_POLLOUT) {
      worker_loop_write_arm_pollout(w, c, cfd, ops);
      return;
    }

    if (step == TX_SF_STEP_ERROR_CLOSE) {
      if (ops->clear_write_timeout) {
        ops->clear_write_timeout(w, c);
      }
      if (ops->schedule_or_sync_close) {
        ops->schedule_or_sync_close(w, cfd);
      }
      if (ops->tx_close_file) {
        ops->tx_close_file(c);
      }
      return;
    }

    if (ops->tx_close_file) {
      ops->tx_close_file(c);
    }
    if (worker_loop_cancel_write_poll_if_armed(w, c) && ops->maybe_flush) {
      ops->maybe_flush(w, 0);
    }

    if (step == TX_SF_STEP_SHUT_WR_AND_READ) {
      if (ops->emit_access_log) {
        ops->emit_access_log(w, c);
      }
      if (ops->clear_write_timeout) {
        ops->clear_write_timeout(w, c);
      }
      shutdown(cfd, SHUT_WR);
      if (!c->tx.recv_armed && ops->post_recv_ptr) {
        ops->post_recv_ptr(w, c);
      }
      return;
    }

    worker_loop_write_on_full_response(w, c, cfd, ops);
    return;
  }

  if (ops->tx_close_file) {
    ops->tx_close_file(c);
  }
  if (worker_loop_cancel_write_poll_if_armed(w, c) && ops->maybe_flush) {
    ops->maybe_flush(w, 0);
  }
  worker_loop_write_on_full_response(w, c, cfd, ops);
}

int worker_loop_fetch_cqes(struct worker_ctx *w,
                           struct io_uring_cqe **cqes,
                           unsigned max_cqes,
                           unsigned *out_count) {
  if (!w || !cqes || max_cqes == 0 || !out_count) {
    return -EINVAL;
  }

  *out_count = io_uring_peek_batch_cqe(&w->ring, cqes, max_cqes);
  if (*out_count > 0) {
    return 0;
  }

  submit_if_pending(w);

  struct io_uring_cqe *first = NULL;
  int ret = io_uring_wait_cqe(&w->ring, &first);
  if (ret < 0) {
    return ret;
  }

  cqes[0] = first;
  *out_count = 1;
  return 0;
}

struct op_ctx *worker_loop_decode_op(const struct io_uring_cqe *cqe) {
  if (!cqe || cqe->user_data == 0) {
    return NULL;
  }

  struct op_ctx *op = (struct op_ctx *)(uintptr_t)cqe->user_data;
  if (!op || op->magic != OP_MAGIC) {
    return NULL;
  }
  return op;
}

int worker_loop_prepare_dispatch(struct worker_ctx *w,
                                 struct io_uring_cqe *cqe,
                                 struct op_ctx **out_op) {
  if (!w || !cqe || !out_op) {
    return 1;
  }

  *out_op = worker_loop_decode_op(cqe);
  if (!*out_op) {
    io_uring_cqe_seen(&w->ring, cqe);
    return 1;
  }

  if (accept_try_handle_cqe(w, *out_op, cqe)) {
    io_uring_cqe_seen(&w->ring, cqe);
    return 1;
  }

  return 0;
}

int worker_loop_try_handle_nonconn(struct worker_ctx *w,
                                   struct op_ctx *op,
                                   struct io_uring_cqe *cqe,
                                   int is_running) {
  if (!w || !op || !cqe) {
    return 0;
  }

  if (op->type == OP_WAKE) {
    if (!is_running) {
      // On shutdown transition, stop new accepts and drain in-flight connections.
      accept_enter_drain(w);
    }

    int pipe_rd = w->cfg.wake_rd;
    if (pipe_rd >= 0) {
      char buf[256];
      int pipe_closed = 0;
      for (;;) {
        ssize_t n = read(pipe_rd, buf, sizeof(buf));
        if (n > 0) {
          continue;
        }
        if (n == 0) {
          pipe_closed = 1;
          break;
        }
        if (errno == EINTR) {
          continue;
        }
        if (errno == EAGAIN) {
          break;
        }
        LOGE(LOGC_CORE, "wake read error: %s", strerror(errno));
        break;
      }
      // Keep wake poll armed in running and drain states.
      if (!pipe_closed) {
        struct io_uring_sqe *sqe = get_sqe_batching(w);
        if (sqe) {
          io_uring_prep_poll_add(sqe, pipe_rd, POLLIN);
          io_uring_sqe_set_data64(sqe, (uint64_t)(uintptr_t)&w->wake_static);
          mark_post(w);
        }
      } else if (pipe_closed) {
        w->cfg.wake_rd = -1;
      }
    }
    io_uring_cqe_seen(&w->ring, cqe);
    return 1;
  }

  if (op->type == OP_SWEEP) {
    uint64_t now = w->now_cached_ms ? w->now_cached_ms : time_now_ms_monotonic();

    while (w->next_tick_deadline_ms <= now) {
      tw_process_tick(w, now);
      w->next_tick_deadline_ms += w->tw_tick_ms;
    }

    uint64_t delta_ms =
      (w->next_tick_deadline_ms > now) ? (w->next_tick_deadline_ms - now) : w->tw_tick_ms;
    struct __kernel_timespec ts;
    ts.tv_sec = (time_t)(delta_ms / 1000);
    ts.tv_nsec = (long)((delta_ms % 1000) * 1000000);

    struct io_uring_sqe *sqe_t = get_sqe_batching(w);
    if (sqe_t) {
      io_uring_prep_timeout(sqe_t, &ts, 0, 0);
      io_uring_sqe_set_data64(sqe_t, (uint64_t)(uintptr_t)&w->sweep_static);
      mark_post(w);
    }
    maybe_flush(w, /*urgent=*/0);
    io_uring_cqe_seen(&w->ring, cqe);
    return 1;
  }

  return 0;
}

int worker_loop_try_handle_conn(struct worker_ctx *w,
                                struct op_ctx *op,
                                struct io_uring_cqe *cqe,
                                const struct worker_loop_conn_handlers *handlers) {
  if (!w || !op || !cqe || !handlers) {
    return 0;
  }

  switch (op->type) {
  case OP_CLOSE: {
    struct conn *c = op->c;
    if (cqe->res < 0) {
      CTR_INC_OPS(w, cnt_close_err);
      LOGE(LOGC_IO, "close failed res=%d", cqe->res);
    }
    if (c) {
      c->fd = -1;
      if (handlers->on_conn_put) {
        handlers->on_conn_put(c);
      }
    }
    io_uring_cqe_seen(&w->ring, cqe);
    return 1;
  }

  case OP_WRITE: {
    struct conn *c = op->c;
    int cfd = c ? c->fd : -1;

    if (!c || cfd < 0) {
      if (c && handlers->on_conn_put) {
        handlers->on_conn_put(c);
      }
      io_uring_cqe_seen(&w->ring, cqe);
      return 1;
    }

    if (handlers->on_write) {
      handlers->on_write(w, c, cfd, cqe->res);
    }

    if (handlers->on_conn_put) {
      handlers->on_conn_put(c);
    }
    io_uring_cqe_seen(&w->ring, cqe);
    return 1;
  }

  case OP_WRITE_READY: {
    struct conn *c = op->c;
    int cfd = c ? c->fd : -1;

    if (!c || cfd < 0) {
      if (c && handlers->on_conn_put) {
        handlers->on_conn_put(c);
      }
      io_uring_cqe_seen(&w->ring, cqe);
      return 1;
    }

    if (!(c->dl.closing && c->tx.resp_kind == RK_NONE) && handlers->on_write_ready) {
      handlers->on_write_ready(w, c, cfd);
    }

    if (handlers->on_conn_put) {
      handlers->on_conn_put(c);
    }
    io_uring_cqe_seen(&w->ring, cqe);
    return 1;
  }

  case OP_READ: {
    struct conn *c = op->c;
    int cfd = c ? c->fd : -1;
    if (handlers->on_read) {
      handlers->on_read(w, c, cfd, cqe);
    }
    return 1;
  }

  default:
    return 0;
  }
}

int worker_loop_process_cqe_batch(struct worker_ctx *w,
                                  struct io_uring_cqe **cqes,
                                  unsigned cqe_count,
                                  const struct worker_loop_conn_handlers *handlers,
                                  int is_running) {
  if (!w || !cqes || cqe_count == 0 || !handlers) {
    return -EINVAL;
  }

  for (unsigned i = 0; i < cqe_count; ++i) {
    struct io_uring_cqe *cqe = cqes[i];
    struct op_ctx *op = NULL;

    if (worker_loop_prepare_dispatch(w, cqe, &op)) {
      continue;
    }
    if (worker_loop_try_handle_nonconn(w, op, cqe, is_running)) {
      continue;
    }
    if (worker_loop_try_handle_conn(w, op, cqe, handlers)) {
      continue;
    }

    io_uring_cqe_seen(&w->ring, cqe);
  }

  return 0;
}

void worker_loop_read_handle_cqe(struct worker_ctx *w,
                                 struct conn *c,
                                 int cfd,
                                 struct io_uring_cqe *cqe,
                                 const struct worker_loop_read_ops *ops) {
  if (!w || !cqe || !ops) {
    return;
  }

  int res = cqe->res;

  struct rx_buf_view rxb;
  rx_buf_from_cqe(w, cqe, &rxb);
  int have_buf = rxb.have_buf;
  unsigned short bid = rxb.bid;
  char *buf_ptr = rxb.buf_ptr;

  if (c) {
    c->tx.recv_armed = 0;
  }

  if (!c || cfd < 0 || c->dl.closing) {
    if (have_buf) {
      rx_buf_return(w, bid);
    }
    if (ops->conn_put) {
      ops->conn_put(c);
    }
    io_uring_cqe_seen(&w->ring, cqe);
    return;
  }

  if (!have_buf) {
    CTR_INC_DEV(w, cnt_buf_noselect);
    LOGD_RL(LOGC_BUF,
            1000,
            "OP_READ noselect fd=%d gen=%u res=%d",
            cfd,
            (unsigned)(c ? c->generation : 0u),
            res);
    c->noselect_streak++;
    int closed_due_to_noselect = 0;
    if (c->noselect_streak >= 2) {
      if (ops->schedule_or_sync_close) {
        ops->schedule_or_sync_close(w, cfd);
      }
      closed_due_to_noselect = 1;
    }
    if (!closed_due_to_noselect && ops->is_closing_no_deadline && !ops->is_closing_no_deadline(c)
        && ops->post_recv_ptr) {
      ops->post_recv_ptr(w, c);
    }
    if (ops->conn_put) {
      ops->conn_put(c);
    }
    io_uring_cqe_seen(&w->ring, cqe);
    return;
  }

  if (have_buf && !buf_ptr) {
    if (ops->is_closing_no_deadline && !ops->is_closing_no_deadline(c) && ops->post_recv_ptr) {
      ops->post_recv_ptr(w, c);
    }
    rx_buf_return(w, bid);
    if (ops->conn_put) {
      ops->conn_put(c);
    }
    io_uring_cqe_seen(&w->ring, cqe);
    return;
  }

  if (res <= 0) {
    if (res == -EAGAIN || res == -EINTR) {
      if (ops->is_closing_no_deadline && !ops->is_closing_no_deadline(c) && ops->post_recv_ptr) {
        ops->post_recv_ptr(w, c);
      }
      rx_buf_return(w, bid);
      if (ops->conn_put) {
        ops->conn_put(c);
      }
      io_uring_cqe_seen(&w->ring, cqe);
      return;
    }

    if (res == 0) {
      if (c->h1.parser_bytes == 0) {
        CTR_INC_DEV(w, cnt_eof_idle);
        LOGD_EVERY_N(LOGC_IO,
                     128,
                     "EOF idle fd=%u gen=%u bytes=%zu headers_done=%d",
                     (unsigned)cfd,
                     (unsigned)c->generation,
                     c->h1.parser_bytes,
                     c->h1.headers_done);
      } else if (!c->h1.headers_done) {
        CTR_INC_DEV(w, cnt_eof_midreq);
        LOGD_EVERY_N(LOGC_IO,
                     128,
                     "EOF midreq fd=%u gen=%u bytes=%zu headers_done=%d",
                     (unsigned)cfd,
                     (unsigned)c->generation,
                     c->h1.parser_bytes,
                     c->h1.headers_done);
      }
    }

    if (ops->schedule_or_sync_close) {
      ops->schedule_or_sync_close(w, cfd);
    }
    rx_buf_return(w, bid);
    if (ops->conn_put) {
      ops->conn_put(c);
    }
    io_uring_cqe_seen(&w->ring, cqe);
    return;
  }

  size_t n = (size_t)res;
  if (ops->mark_activity) {
    ops->mark_activity(c, w->now_cached_ms);
  }

  if (!c->h1.headers_done && c->h1.parser_bytes == 0) {
    c->dl.header_start_ms = w->now_cached_ms;
    LOGD_EVERY_N(LOGC_HTTP,
                 128,
                 "READ_FIRSTBYTE fd=%u gen=%u header_start_ms=%llu",
                 (unsigned)cfd,
                 (unsigned)c->generation,
                 (unsigned long long)c->dl.header_start_ms);
  }

  c->noselect_streak = 0;

  struct http_pipeline_result agg;
  memset(&agg, 0, sizeof(agg));
  agg.action = HP_ACTION_CONTINUE;
  agg.err = HPE_OK;

  size_t consumed = 0;

  if (!c->h1.headers_done && !c->h1.parse_error && !c->h1.header_too_big && !c->h1.unsupported_te) {
    size_t feed_len = n;
    size_t tail_len = 0;
    if (ops->split_on_header_end) {
      ops->split_on_header_end(c, buf_ptr, n, &feed_len, &tail_len);
    }

    if (feed_len > 0) {
      struct http_pipeline_result r1 = http_pipeline_feed(c, buf_ptr, feed_len);
      agg.header_too_big_transition |= r1.header_too_big_transition;
      agg.parse_error_transition |= r1.parse_error_transition;
      agg.headers_complete_transition |= r1.headers_complete_transition;
      agg.tolerated_error |= r1.tolerated_error;
      if (r1.err != HPE_OK) {
        agg.err = r1.err;
      }

      if (ops->rx_tail_update_after_feed) {
        ops->rx_tail_update_after_feed(c, buf_ptr, feed_len);
      }

      size_t used = feed_len;
      if (r1.err == HPE_PAUSED) {
        const char *pos = llhttp_get_error_pos(&c->h1.parser);
        const char *base = buf_ptr;
        if (pos && pos >= base && pos <= base + feed_len) {
          used = (size_t)(pos - base);
        }
      }
      consumed = used;
    }
  }

  if (consumed < n && c->h1.headers_done && !c->h1.message_done && !c->h1.parse_error
      && !c->h1.header_too_big && !c->h1.unsupported_te) {
    size_t avail = n - consumed;
    bool is_chunked = (c->h1.te_count > 0 && c->h1.te_chunked && !c->h1.te_other);
    if (is_chunked) {
      size_t body_feed = avail;
      if (body_feed > 0) {
        struct http_pipeline_result r2 = http_pipeline_feed(c, buf_ptr + consumed, body_feed);
        agg.header_too_big_transition |= r2.header_too_big_transition;
        agg.parse_error_transition |= r2.parse_error_transition;
        agg.headers_complete_transition |= r2.headers_complete_transition;
        agg.tolerated_error |= r2.tolerated_error;
        if (r2.err != HPE_OK) {
          agg.err = r2.err;
        }

        size_t used = body_feed;
        if (r2.err == HPE_PAUSED) {
          const char *pos = llhttp_get_error_pos(&c->h1.parser);
          const char *base = buf_ptr + consumed;
          if (pos && pos >= base && pos <= base + body_feed) {
            used = (size_t)(pos - base);
          }
        }
        consumed += used;
      }
    } else {
      uint64_t rem = c->h1.body_remaining;
      size_t body_feed = (rem < (uint64_t)avail) ? (size_t)rem : avail;
      if (body_feed > 0) {
        struct http_pipeline_result r2 = http_pipeline_feed(c, buf_ptr + consumed, body_feed);
        agg.header_too_big_transition |= r2.header_too_big_transition;
        agg.parse_error_transition |= r2.parse_error_transition;
        agg.headers_complete_transition |= r2.headers_complete_transition;
        agg.tolerated_error |= r2.tolerated_error;
        if (r2.err != HPE_OK) {
          agg.err = r2.err;
        }

        if (c->h1.body_remaining >= (uint64_t)body_feed) {
          c->h1.body_remaining -= (uint64_t)body_feed;
        } else {
          c->h1.body_remaining = 0;
        }
        consumed += body_feed;
      }
    }
  }

  if (consumed < n) {
    size_t leftover = n - consumed;
    if (ops->rx_stash_append && ops->rx_stash_append(c, buf_ptr + consumed, leftover) != 0) {
      LOGW(LOGC_HTTP,
           "rx_stash_append failed: fd=%u gen=%u leftover=%zu stash_len=%u parser_bytes=%zu "
           "headers_done=%d",
           (unsigned)cfd,
           (unsigned)c->generation,
           leftover,
           (unsigned)c->rx_stash_len,
           c->h1.parser_bytes,
           c->h1.headers_done);
      if (ops->schedule_or_sync_close) {
        ops->schedule_or_sync_close(w, cfd);
      }
    }
  }

  agg.action = http_pipeline_classify_action(c);
  if (ops->apply_action) {
    ops->apply_action(w, c, cfd, agg, buf_ptr, consumed ? consumed : n);
  }

  rx_buf_return(w, bid);
  io_uring_cqe_seen(&w->ring, cqe);
  if (ops->conn_put) {
    ops->conn_put(c);
  }
}

void worker_loop_write_handle_cqe(struct worker_ctx *w,
                                  struct conn *c,
                                  int cfd,
                                  int res,
                                  const struct worker_loop_write_ops *ops) {
  if (!w || !c || !ops) {
    return;
  }

  if (res > 0) {
    c->dl.last_active_ms = w->now_cached_ms;
  }
  if (res > 0 && ops->arm_write_timeout) {
    ops->arm_write_timeout(w, c);
  }

  struct tx_next_io out = {0};
  enum tx_decision d = tx_on_io_result(&c->tx, TX_IO_HEADERS, (ssize_t)res, &out);

  switch (d) {
  case TX_SEND_HEADERS:
    worker_loop_write_post_remaining(w, c, cfd, 1, ops);
    return;

  case TX_START_SENDFILE:
    if (worker_loop_cancel_write_poll_if_armed(w, c) && ops->maybe_flush) {
      ops->maybe_flush(w, 0);
    }
    tx_discard(&c->tx);
    worker_loop_tx_sendfile_progress(w, c, cfd, ops);
    return;

  case TX_ARM_POLLOUT:
    worker_loop_write_arm_pollout(w, c, cfd, ops);
    return;

  case TX_SHUT_WR_AND_READ:
    if (worker_loop_cancel_write_poll_if_armed(w, c) && ops->maybe_flush) {
      ops->maybe_flush(w, 0);
    }
    if (ops->emit_access_log) {
      ops->emit_access_log(w, c);
    }
    if (ops->clear_write_timeout) {
      ops->clear_write_timeout(w, c);
    }
    shutdown(cfd, SHUT_WR);
    if (!c->tx.recv_armed && ops->post_recv_ptr) {
      ops->post_recv_ptr(w, c);
    }
    return;

  case TX_DONE_KEEPALIVE:
  case TX_DONE_CLOSE:
    if (worker_loop_cancel_write_poll_if_armed(w, c) && ops->maybe_flush) {
      ops->maybe_flush(w, 0);
    }
    worker_loop_write_on_full_response(w, c, cfd, ops);
    return;

  case TX_ERROR_CLOSE:
    if (res < 0) {
      CTR_INC_OPS(w, cnt_send_err);
    }
    if (res < 0) {
      int err = -res;
      LOGW(LOGC_IO,
           "send error: fd=%u gen=%u err=%d(%s) write_off=%zu write_len=%zu headers_done=%d "
           "parser_bytes=%zu",
           (unsigned)cfd,
           (unsigned)c->generation,
           err,
           strerror(err),
           c->tx.write_off,
           c->tx.write_len,
           c->h1.headers_done,
           c->h1.parser_bytes);
    }
    if (ops->clear_write_timeout) {
      ops->clear_write_timeout(w, c);
    }
    if (ops->schedule_or_sync_close) {
      ops->schedule_or_sync_close(w, cfd);
    }
    return;

  case TX_NOOP:
  default:
    return;
  }
}

void worker_loop_write_ready_handle_cqe(struct worker_ctx *w,
                                        struct conn *c,
                                        int cfd,
                                        const struct worker_loop_write_ops *ops) {
  if (!w || !c || cfd < 0 || !ops) {
    return;
  }

  tx_notify_poll_disarmed_staged(&c->tx);

  struct tx_next_io out = {0};
  enum tx_decision d = tx_on_pollout(&c->tx, &out);

  if (d == TX_RESUME_SENDFILE) {
    worker_loop_tx_sendfile_progress(w, c, cfd, ops);
    return;
  }

  if (d == TX_SEND_HEADERS) {
    worker_loop_write_post_remaining(w, c, cfd, 0, ops);
    return;
  }
}

void worker_loop_process_rx_bytes(struct worker_ctx *w,
                                  struct conn *c,
                                  int cfd,
                                  const char *buf_ptr,
                                  size_t n,
                                  const struct worker_loop_read_ops *ops) {
  if (!w || !c || cfd < 0 || !buf_ptr || n == 0 || !ops) {
    return;
  }

  if (c->dl.closing) {
    return;
  }

  if (c->h1.parser_bytes == 0) {
    c->dl.header_start_ms = w->now_cached_ms;
  }
  if (ops->mark_activity) {
    ops->mark_activity(c, w->now_cached_ms);
  }
  c->noselect_streak = 0;

  struct http_pipeline_result agg;
  memset(&agg, 0, sizeof(agg));
  agg.action = HP_ACTION_CONTINUE;
  agg.err = HPE_OK;

  size_t consumed = 0;

  if (!c->h1.headers_done && !c->h1.parse_error && !c->h1.header_too_big && !c->h1.unsupported_te) {
    size_t feed_len = n;
    size_t tail_len = 0;
    if (ops->split_on_header_end) {
      ops->split_on_header_end(c, buf_ptr, n, &feed_len, &tail_len);
    }

    if (feed_len > 0) {
      struct http_pipeline_result r1 = http_pipeline_feed(c, buf_ptr, feed_len);
      agg.header_too_big_transition |= r1.header_too_big_transition;
      agg.parse_error_transition |= r1.parse_error_transition;
      agg.headers_complete_transition |= r1.headers_complete_transition;
      agg.tolerated_error |= r1.tolerated_error;
      if (r1.err != HPE_OK) {
        agg.err = r1.err;
      }

      if (ops->rx_tail_update_after_feed) {
        ops->rx_tail_update_after_feed(c, buf_ptr, feed_len);
      }

      size_t used = feed_len;
      if (r1.err == HPE_PAUSED) {
        const char *pos = llhttp_get_error_pos(&c->h1.parser);
        const char *base = buf_ptr;
        if (pos && pos >= base && pos <= base + feed_len) {
          used = (size_t)(pos - base);
        }
      }
      consumed = used;
    }
  }

  if (consumed < n && c->h1.headers_done && !c->h1.message_done && !c->h1.parse_error
      && !c->h1.header_too_big && !c->h1.unsupported_te) {
    size_t avail = n - consumed;
    bool is_chunked = (c->h1.te_count > 0 && c->h1.te_chunked && !c->h1.te_other);
    if (is_chunked) {
      size_t body_feed = avail;
      if (body_feed > 0) {
        struct http_pipeline_result r2 = http_pipeline_feed(c, buf_ptr + consumed, body_feed);
        agg.header_too_big_transition |= r2.header_too_big_transition;
        agg.parse_error_transition |= r2.parse_error_transition;
        agg.headers_complete_transition |= r2.headers_complete_transition;
        agg.tolerated_error |= r2.tolerated_error;
        if (r2.err != HPE_OK) {
          agg.err = r2.err;
        }

        size_t used = body_feed;
        if (r2.err == HPE_PAUSED) {
          const char *pos = llhttp_get_error_pos(&c->h1.parser);
          const char *base = buf_ptr + consumed;
          if (pos && pos >= base && pos <= base + body_feed) {
            used = (size_t)(pos - base);
          }
        }
        consumed += used;
      }
    } else {
      uint64_t rem = c->h1.body_remaining;
      size_t body_feed = (rem < (uint64_t)avail) ? (size_t)rem : avail;
      if (body_feed > 0) {
        struct http_pipeline_result r2 = http_pipeline_feed(c, buf_ptr + consumed, body_feed);
        agg.header_too_big_transition |= r2.header_too_big_transition;
        agg.parse_error_transition |= r2.parse_error_transition;
        agg.headers_complete_transition |= r2.headers_complete_transition;
        agg.tolerated_error |= r2.tolerated_error;
        if (r2.err != HPE_OK) {
          agg.err = r2.err;
        }

        if (c->h1.body_remaining >= (uint64_t)body_feed) {
          c->h1.body_remaining -= (uint64_t)body_feed;
        } else {
          c->h1.body_remaining = 0;
        }
        consumed += body_feed;
      }
    }
  }

  if (consumed < n) {
    size_t leftover = n - consumed;
    if (ops->rx_stash_append && ops->rx_stash_append(c, buf_ptr + consumed, leftover) != 0) {
      if (ops->schedule_or_sync_close) {
        ops->schedule_or_sync_close(w, cfd);
      }
      return;
    }
  }

  agg.action = http_pipeline_classify_action(c);
  if (ops->apply_action) {
    ops->apply_action(w, c, cfd, agg, buf_ptr, consumed ? consumed : n);
  }
}

int worker_loop_tls_try_send_pending(struct worker_ctx *w,
                                     struct conn *c,
                                     int cfd,
                                     const struct worker_loop_tls_write_ops *ops) {
  if (!w || !c || cfd < 0 || !c->tls_enabled || !ops) {
    return -1;
  }

  struct tx_next_io next = {0};
  if (!tx_pending_headers(&c->tx, &next)) {
    return 0;
  }

  int want_read = 0;
  int want_write = 0;
  ssize_t n = tls_conn_send(c, next.buf, next.len, &want_read, &want_write);
  if (n > 0) {
    if (ops->write_handle_cqe) {
      ops->write_handle_cqe(w, c, cfd, (int)n);
    }
    return 0;
  }

  if (n < 0 && errno == EAGAIN) {
    if (ops->arm_write_timeout) {
      ops->arm_write_timeout(w, c);
    }
    if (want_write && ops->post_tls_pollout_ptr && ops->post_tls_pollout_ptr(w, c) != 0) {
      CTR_INC_DEV(w, cnt_sqe_starvation_close);
      if (ops->schedule_or_sync_close) {
        ops->schedule_or_sync_close(w, cfd);
      }
      return -1;
    }
    if (want_read && !c->tx.recv_armed && ops->is_closing_no_deadline
        && !ops->is_closing_no_deadline(c) && ops->post_recv_ptr) {
      ops->post_recv_ptr(w, c);
    }
    return 0;
  }

  if (ops->schedule_or_sync_close) {
    ops->schedule_or_sync_close(w, cfd);
  }
  return -1;
}

void worker_loop_tls_read_handle_cqe(struct worker_ctx *w,
                                     struct conn *c,
                                     int cfd,
                                     struct io_uring_cqe *cqe,
                                     const struct worker_loop_read_ops *read_ops,
                                     const struct worker_loop_tls_write_ops *tls_ops) {
  if (!w || !c || !cqe || !read_ops || !tls_ops || cfd < 0) {
    return;
  }

  c->tx.recv_armed = 0;

  int spins = 0;
  while (!c->dl.closing && spins < 8) {
    spins++;
    char tbuf[8192];
    int want_read = 0;
    int want_write = 0;
    ssize_t n = tls_conn_recv(c, tbuf, sizeof(tbuf), &want_read, &want_write);

    if (n > 0) {
      worker_loop_process_rx_bytes(w, c, cfd, tbuf, (size_t)n, read_ops);
      continue;
    }

    if (n == 0) {
      if (tls_ops->schedule_or_sync_close) {
        tls_ops->schedule_or_sync_close(w, cfd);
      }
      break;
    }

    if (errno == EAGAIN) {
      if (want_write && tls_ops->post_tls_pollout_ptr && tls_ops->post_tls_pollout_ptr(w, c) != 0) {
        CTR_INC_DEV(w, cnt_sqe_starvation_close);
        if (tls_ops->schedule_or_sync_close) {
          tls_ops->schedule_or_sync_close(w, cfd);
        }
        break;
      }
      if (want_read && !c->tx.recv_armed && tls_ops->is_closing_no_deadline
          && !tls_ops->is_closing_no_deadline(c) && tls_ops->post_recv_ptr) {
        tls_ops->post_recv_ptr(w, c);
      }
      break;
    }

    if (tls_ops->schedule_or_sync_close) {
      tls_ops->schedule_or_sync_close(w, cfd);
    }
    break;
  }

  if (!c->dl.closing && !c->tx.recv_armed && tls_ops->is_closing_no_deadline
      && !tls_ops->is_closing_no_deadline(c) && tls_ops->post_recv_ptr) {
    tls_ops->post_recv_ptr(w, c);
  }

  io_uring_cqe_seen(&w->ring, cqe);
  if (read_ops->conn_put) {
    read_ops->conn_put(c);
  }
}

void worker_loop_tls_write_ready_handle_cqe(struct worker_ctx *w,
                                            struct conn *c,
                                            int cfd,
                                            void (*mark_activity)(struct conn *c, uint64_t now_ms),
                                            const struct worker_loop_tls_write_ops *tls_ops) {
  if (!w || !c || cfd < 0 || !tls_ops) {
    return;
  }

  tx_notify_poll_disarmed_staged(&c->tx);
  if (mark_activity) {
    mark_activity(c, w->now_cached_ms);
  }
  (void)worker_loop_tls_try_send_pending(w, c, cfd, tls_ops);
}

int worker_loop_tls_handshake_progress(struct worker_ctx *w,
                                       struct conn *c,
                                       int cfd,
                                       const struct worker_loop_tls_hs_ops *ops) {
  if (!w || !c || cfd < 0 || !ops) {
    return -1;
  }
  if (!conn_tls_handshake_pending(c)) {
    return 1;
  }

  char terr[256] = {0};
  enum tls_hs_status hs = tls_conn_handshake_step(c, terr);

  if (hs == TLS_HS_DONE) {
    c->tls_handshake_done = 1;
    c->tls_want_read = 0;
    c->tls_want_write = 0;
    if (!c->tx.recv_armed && ops->is_closing_no_deadline && !ops->is_closing_no_deadline(c)
        && ops->post_recv_ptr) {
      ops->post_recv_ptr(w, c);
    }
    if (ops->tw_reschedule) {
      ops->tw_reschedule(w, c, w->now_cached_ms);
    }
    return 1;
  }

  if (hs == TLS_HS_WANT_READ) {
    c->tls_want_read = 1;
    c->tls_want_write = 0;
    if (!c->tx.recv_armed && ops->is_closing_no_deadline && !ops->is_closing_no_deadline(c)
        && ops->post_recv_ptr) {
      ops->post_recv_ptr(w, c);
    }
    if (ops->tw_reschedule) {
      ops->tw_reschedule(w, c, w->now_cached_ms);
    }
    return 0;
  }

  if (hs == TLS_HS_WANT_WRITE) {
    c->tls_want_read = 0;
    c->tls_want_write = 1;
    if (ops->post_tls_pollout_ptr && ops->post_tls_pollout_ptr(w, c) != 0) {
      CTR_INC_DEV(w, cnt_sqe_starvation_close);
      if (ops->schedule_or_sync_close) {
        ops->schedule_or_sync_close(w, cfd);
      }
      return -1;
    }
    if (ops->tw_reschedule) {
      ops->tw_reschedule(w, c, w->now_cached_ms);
    }
    return 0;
  }

  LOGW(LOGC_IO,
       "TLS handshake failed fd=%u gen=%u%s%s",
       (unsigned)cfd,
       (unsigned)c->generation,
       terr[0] ? ": " : "",
       terr[0] ? terr : "");
  if (ops->schedule_or_sync_close) {
    ops->schedule_or_sync_close(w, cfd);
  }
  return -1;
}
