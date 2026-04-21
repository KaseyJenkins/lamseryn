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
#include "include/buffer_pool.h"
#include "include/conn_close.h"
#include "include/conn_lifecycle.h"
#include "include/http_boundary.h"
#include "include/rx_stash.h"
#include "include/request_handlers.h"
#include "include/access_log.h"

#include "instrumentation/counters_update.h"

static inline void worker_loop_neutralize_sqe(struct io_uring_sqe *sqe) {
  io_uring_prep_nop(sqe);
  io_uring_sqe_set_data64(sqe, 0);
  sqe->flags = 0;
  sqe->buf_group = 0;
}

static void write_handle_cqe(struct worker_ctx *w, struct conn *c, int cfd, int res);
static void write_ready_handle_cqe(struct worker_ctx *w, struct conn *c, int cfd);
static void read_handle_cqe(struct worker_ctx *w,
                             struct conn *c,
                             int cfd,
                             struct io_uring_cqe *cqe);

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
        int did_submit_w = 0;
        struct io_uring_sqe *sqe = get_sqe_retry_once(&w->ring, &did_submit_w);
        if (did_submit_w) {
          CTR_INC_DEV(w, cnt_submit_inline);
        }
        if (sqe) {
          io_uring_prep_poll_add(sqe, pipe_rd, POLLIN);
          io_uring_sqe_set_data64(sqe, (uint64_t)(uintptr_t)&w->wake_static);
          mark_post(w);
        } else {
          LOGE(LOGC_CORE, "OP_WAKE: failed to re-arm wake poll, shutdown signals may be lost");
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

    int did_submit_t = 0;
    struct io_uring_sqe *sqe_t = get_sqe_retry_once(&w->ring, &did_submit_t);
    if (did_submit_t) {
      CTR_INC_DEV(w, cnt_submit_inline);
    }
    if (sqe_t) {
      io_uring_prep_timeout(sqe_t, &ts, 0, 0);
      io_uring_sqe_set_data64(sqe_t, (uint64_t)(uintptr_t)&w->sweep_static);
      mark_post(w);
    } else {
      LOGE(LOGC_CORE, "OP_SWEEP: failed to re-arm sweep timer, timing wheel will stop");
    }
    maybe_flush(w, /*urgent=*/0);
    io_uring_cqe_seen(&w->ring, cqe);
    return 1;
  }

  return 0;
}

int worker_loop_try_handle_conn(struct worker_ctx *w,
                                struct op_ctx *op,
                                struct io_uring_cqe *cqe) {
  if (!w || !op || !cqe) {
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
      conn_put(c);
    }
    io_uring_cqe_seen(&w->ring, cqe);
    return 1;
  }

  case OP_WRITE: {
    struct conn *c = op->c;
    int cfd = c ? c->fd : -1;

    if (!c || cfd < 0) {
      if (c) {
        conn_put(c);
      }
      io_uring_cqe_seen(&w->ring, cqe);
      return 1;
    }

    write_handle_cqe(w, c, cfd, cqe->res);

    conn_put(c);
    io_uring_cqe_seen(&w->ring, cqe);
    return 1;
  }

  case OP_WRITE_READY: {
    struct conn *c = op->c;
    int cfd = c ? c->fd : -1;

    if (!c || cfd < 0) {
      if (c) {
        conn_put(c);
      }
      io_uring_cqe_seen(&w->ring, cqe);
      return 1;
    }

    if (!(c->dl.closing && c->tx.resp_kind == RK_NONE)) {
      write_ready_handle_cqe(w, c, cfd);
    }

    conn_put(c);
    io_uring_cqe_seen(&w->ring, cqe);
    return 1;
  }

  case OP_READ: {
    struct conn *c = op->c;
    int cfd = c ? c->fd : -1;
    read_handle_cqe(w, c, cfd, cqe);
    return 1;
  }

  default:
    return 0;
  }
}

int worker_loop_process_cqe_batch(struct worker_ctx *w,
                                  struct io_uring_cqe **cqes,
                                  unsigned cqe_count,
                                  int is_running) {
  if (!w || !cqes || cqe_count == 0) {
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
    if (worker_loop_try_handle_conn(w, op, cqe)) {
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
    c->dl.header_start_us = time_now_us_monotonic();
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
    c->dl.header_start_us = time_now_us_monotonic();
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

// ---------------------------------------------------------------------------
// CQE dispatch helpers
// ---------------------------------------------------------------------------

void post_recv_ptr(struct worker_ctx *w, struct conn *c) {
  if (!w || !c) {
    return;
  }
  if (c->fd < 0) {
    return;
  }
  if (conn_is_closing_no_deadline(c)) {
    return;
  }
  if (c->tx.recv_armed) {
    return;
  }

  struct io_uring_sqe *sqe = get_sqe_batching(w);
  if (!sqe) {
    return;
  }

  if (c->tls_enabled) {
    conn_ref(c);
    io_uring_prep_poll_add(sqe, c->fd, POLLIN);
    io_uring_sqe_set_data64(sqe, (uint64_t)(uintptr_t)&c->op_read);
    conn_arm_recv(c);
    mark_post(w);
    CTR_INC_DEV(w, cnt_read_ptr_posts);
    return;
  }

  conn_ref(c);
  io_uring_prep_recv(sqe, c->fd, NULL, RBUF_SZ, 0);
  sqe->flags = 0;
  sqe->buf_group = buffer_pool_group_id();
  sqe->flags |= IOSQE_BUFFER_SELECT;
  io_uring_sqe_set_data64(sqe, (uint64_t)(uintptr_t)&c->op_read);

  conn_arm_recv(c);

  mark_post(w);
  CTR_INC_DEV(w, cnt_read_ptr_posts);
}

void stage_header_response_send(struct worker_ctx *w,
                                struct conn *c,
                                int cfd,
                                struct io_uring_sqe *sqe_w,
                                enum resp_kind kind,
                                int keepalive,
                                int drain_after_headers,
                                int close_after_send) {
  if (!w || !c) {
    return;
  }

  // During graceful drain, do not allow keep-alive reuse for terminal responses.
  const int force_close = w->is_draining;
  const int effective_keepalive = force_close ? 0 : keepalive;
  const int effective_close_after_send = force_close ? 1 : close_after_send;
  if (force_close && keepalive) {
    CTR_INC_DEV(w, cnt_shutdown_drain_ka_suppressed);
  }

  struct request_response_plan plan = request_build_response_plan(kind,
                                                                  effective_keepalive,
                                                                  drain_after_headers,
                                                                  effective_close_after_send);
  struct tx_next_io out = {0};
  (void)tx_begin_headers(&c->tx,
                         plan.kind,
                         plan.response.buf,
                         plan.response.len,
                         plan.keepalive,
                         plan.drain_after_headers,
                         &out);

  if (plan.close_after_send) {
    conn_prepare_close_after_send(w, c);
  }

  if (c->tls_enabled) {
    struct worker_loop_tls_write_ops tops = worker_loop_build_tls_write_ops();
    (void)worker_loop_tls_try_send_pending(w, c, cfd, &tops);
    return;
  }

  if (!sqe_w) {
    return;
  }

  conn_ref(c);
  io_uring_prep_send(sqe_w, cfd, out.buf, out.len, MSG_NOSIGNAL);
  io_uring_sqe_set_data64(sqe_w, (uint64_t)(uintptr_t)&c->op_write);
  mark_post(w);
}

void stage_tx_buffer_send(struct worker_ctx *w,
                          struct conn *c,
                          int cfd,
                          struct io_uring_sqe *sqe_w) {
  if (!w || !c) {
    return;
  }

  if (c->tls_enabled) {
    struct worker_loop_tls_write_ops tops = worker_loop_build_tls_write_ops();
    (void)worker_loop_tls_try_send_pending(w, c, cfd, &tops);
    return;
  }

  if (!sqe_w) {
    return;
  }

  conn_ref(c);
  io_uring_prep_send(sqe_w, cfd, c->tx.write_buf, c->tx.write_len, MSG_NOSIGNAL);
  io_uring_sqe_set_data64(sqe_w, (uint64_t)(uintptr_t)&c->op_write);
  mark_post(w);
}

void http_apply_action(struct worker_ctx *w,
                       struct conn *c,
                       int cfd,
                       struct http_pipeline_result hres,
                       const char *chunk,
                       size_t chunk_len) {
  struct http_apply_plan plan = http_pipeline_build_apply_plan(c, &hres);

  // Internal server error (e.g. OOM during request capture): respond 500 and close.
  // This should take precedence over client-induced parse errors.
  if (plan.kind == HP_APPLY_INTERNAL_ERROR) {
    struct http_error_plan ieplan = plan.error;

    struct io_uring_sqe *sqe_w = c->tls_enabled ? NULL : get_sqe_batching(w);
    if (!c->tls_enabled && !sqe_w) {
      CTR_INC_DEV(w, cnt_sqe_starvation_close);
      schedule_or_sync_close(w, cfd);
      return;
    }

    stage_header_response_send(w,
                               c,
                               cfd,
                               sqe_w,
                               ieplan.kind,
                               ieplan.keepalive,
                               ieplan.drain_after_headers,
                               ieplan.close_after_send);
    return;
  }

  // Instrumentation counters for HTTP pipeline transitions
  if (hres.header_too_big_transition) {
    CTR_INC_DEV(w, cnt_431);
  }
  if (hres.parse_error_transition) {
    CTR_INC_DEV(w, cnt_400_llhttp);
  }
  if (hres.headers_complete_transition) {
    CTR_INC_DEV(w, cnt_headers_complete);
  }

  // Diagnostic logging for parse transitions
  http_pipeline_log_transitions(c, &hres, chunk, chunk_len);

  // For successful requests, clear any old KA/header deadline from the wheel.
  // (State becomes NONE once headers_done is set.)
  if (plan.reschedule_on_ok) {
    tw_reschedule(w, c, w->now_cached_ms);
  }

  if (plan.kind == HP_APPLY_ERROR) {
    struct http_error_plan eplan = plan.error;
    struct io_uring_sqe *sqe_w = c->tls_enabled ? NULL : get_sqe_batching(w);
    if (!c->tls_enabled && !sqe_w) {
      CTR_INC_DEV(w, cnt_sqe_starvation_close);
      schedule_or_sync_close(w, cfd);
    } else {
      if (eplan.kind == RK_431) {
        conn_prepare_431_draining(w, c, c->dl.last_active_ms, w->now_cached_ms);
      }

      stage_header_response_send(w,
                                 c,
                                 cfd,
                                 sqe_w,
                                 eplan.kind,
                                 eplan.keepalive,
                                 eplan.drain_after_headers,
                                 eplan.close_after_send);
    }
  } else if (plan.kind == HP_APPLY_CONTINUE) {
    if (!conn_is_closing_no_deadline(c)) {
      // Do NOT reset pending_line_error here; keep it sticky until LF or headers_complete
      post_recv_ptr(w, c);
    }
    // refresh header-related deadline (initial idle or header timeout)
    tw_reschedule(w, c, w->now_cached_ms);
  } else {
    struct io_uring_sqe *sqe_w = c->tls_enabled ? NULL : get_sqe_batching(w);
    if (!c->tls_enabled && !sqe_w) {
      CTR_INC_DEV(w, cnt_sqe_starvation_close);
      schedule_or_sync_close(w, cfd);
    } else {
      struct http_ok_plan okplan = plan.ok;

      // During drain we finish one in-flight request and then close.
      if (w->is_draining) {
        if (okplan.keepalive) {
          CTR_INC_DEV(w, cnt_shutdown_drain_ka_suppressed);
        }
        okplan.keepalive = 0;
        c->h1.want_keepalive = 0;
      }

      struct request_ok_dispatch dispatch = request_dispatch_ok(c, &okplan);
      switch (dispatch.kind) {
      case REQUEST_OK_TX_BUFFER:
        stage_tx_buffer_send(w, c, cfd, sqe_w);
        break;
      case REQUEST_OK_HEADER_RESPONSE:
        c->h1.want_keepalive = dispatch.response.keepalive;
        stage_header_response_send(w,
                                   c,
                                   cfd,
                                   sqe_w,
                                   dispatch.response.kind,
                                   dispatch.response.keepalive,
                                   dispatch.response.drain_after_headers,
                                   dispatch.response.close_after_send);
        break;
      case REQUEST_OK_NO_RESPONSE:
        break;
      }
    }
  }
}

int conn_try_process_stash(struct worker_ctx *w, struct conn *c, int cfd) {
  if (!w || !c || cfd < 0) {
    return 0;
  }
  if (c->rx_stash_len == 0) {
    return 0;
  }
  if (!c->rx_stash) {
    return 0;
  }
  if (c->dl.closing || c->dl.draining) {
    return 0;
  }

  // Treat stash as already-received bytes becoming visible now.
  conn_mark_activity(c, w->now_cached_ms);
  if (!c->h1.headers_done && c->h1.parser_bytes == 0) {
    c->dl.header_start_ms = w->now_cached_ms;
    c->dl.header_start_us = time_now_us_monotonic();
  }

  size_t n = (size_t)c->rx_stash_len;
  struct http_pipeline_result agg;
  memset(&agg, 0, sizeof(agg));
  agg.action = HP_ACTION_CONTINUE;
  agg.err = HPE_OK;

  size_t consumed = 0;

  // Stage 1: headers (boundary-limited)
  if (!c->h1.headers_done && !c->h1.parse_error && !c->h1.header_too_big && !c->h1.unsupported_te) {
    size_t feed_len = n;
    size_t unused_tail = 0;
    conn_split_on_header_end(c, c->rx_stash, n, &feed_len, &unused_tail);

    if (feed_len > 0) {
      struct http_pipeline_result r1 = http_pipeline_feed(c, c->rx_stash, feed_len);
      agg.header_too_big_transition |= r1.header_too_big_transition;
      agg.parse_error_transition |= r1.parse_error_transition;
      agg.headers_complete_transition |= r1.headers_complete_transition;
      agg.tolerated_error |= r1.tolerated_error;
      if (r1.err != HPE_OK) {
        agg.err = r1.err;
      }

      conn_rx_tail_update_after_feed(c, c->rx_stash, feed_len);

      // If llhttp paused at the boundary, consume only up to error_pos so
      // pipelined bytes remain for the next request.
      size_t used = feed_len;
      if (r1.err == HPE_PAUSED) {
        const char *pos = llhttp_get_error_pos(&c->h1.parser);
        const char *base = c->rx_stash;
        if (pos && pos >= base && pos <= base + feed_len) {
          used = (size_t)(pos - base);
        }
      }
      consumed = used;
    }
  }

  // Stage 2: body bytes (Content-Length or chunked)
  if (consumed < n && c->h1.headers_done && !c->h1.message_done && !c->h1.parse_error
      && !c->h1.header_too_big && !c->h1.unsupported_te) {
    size_t avail = n - consumed;
    bool is_chunked = (c->h1.te_count > 0 && c->h1.te_chunked && !c->h1.te_other);
    if (is_chunked) {
      size_t body_feed = avail;
      if (body_feed > 0) {
        struct http_pipeline_result r2 = http_pipeline_feed(c, c->rx_stash + consumed, body_feed);
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
          const char *base = c->rx_stash + consumed;
          // llhttp records `error_pos` as the current pointer when pausing.
          // For on_message_complete pause this is effectively the first byte
          // of the next pipelined request (or endp), so do NOT +1.
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
        struct http_pipeline_result r2 = http_pipeline_feed(c, c->rx_stash + consumed, body_feed);
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

  agg.action = http_pipeline_classify_action(c);

  http_apply_action(w, c, cfd, agg, c->rx_stash, consumed ? consumed : n);

  conn_rx_stash_consume(c, consumed);
  return (agg.action != HP_ACTION_CONTINUE);
}

// ---------------------------------------------------------------------------
// Vtable builders
// ---------------------------------------------------------------------------

struct worker_loop_read_ops worker_loop_build_read_ops(void) {
  return (struct worker_loop_read_ops){
    .is_closing_no_deadline = conn_is_closing_no_deadline,
    .post_recv_ptr = post_recv_ptr,
    .schedule_or_sync_close = schedule_or_sync_close,
    .mark_activity = conn_mark_activity,
    .split_on_header_end = conn_split_on_header_end,
    .rx_tail_update_after_feed = conn_rx_tail_update_after_feed,
    .rx_stash_append = conn_rx_stash_append,
    .apply_action = http_apply_action,
    .conn_put = conn_put,
  };
}

struct worker_loop_write_ops worker_loop_build_write_ops(void) {
  return (struct worker_loop_write_ops){
    .maybe_flush = maybe_flush,
    .arm_write_timeout = conn_arm_write_timeout,
    .clear_write_timeout = conn_clear_write_timeout,
    .tx_close_file = tx_close_file,
    .emit_access_log = access_log_emit_from_conn,
    .post_recv_ptr = post_recv_ptr,
    .conn_reset_request = conn_reset_request,
    .conn_try_process_stash = conn_try_process_stash,
    .tw_reschedule = tw_reschedule,
    .conn_ref = conn_ref,
    .conn_put = conn_put,
    .schedule_or_sync_close = schedule_or_sync_close,
  };
}

struct worker_loop_tls_write_ops worker_loop_build_tls_write_ops(void) {
  return (struct worker_loop_tls_write_ops){
    .arm_write_timeout = conn_arm_write_timeout,
    .post_tls_pollout_ptr = worker_loop_post_tls_pollout,
    .is_closing_no_deadline = conn_is_closing_no_deadline,
    .post_recv_ptr = post_recv_ptr,
    .schedule_or_sync_close = schedule_or_sync_close,
    .write_handle_cqe = write_handle_cqe,
  };
}

struct worker_loop_tls_hs_ops worker_loop_build_tls_hs_ops(void) {
  return (struct worker_loop_tls_hs_ops){
    .is_closing_no_deadline = conn_is_closing_no_deadline,
    .post_recv_ptr = post_recv_ptr,
    .post_tls_pollout_ptr = worker_loop_post_tls_pollout,
    .tw_reschedule = tw_reschedule,
    .schedule_or_sync_close = schedule_or_sync_close,
  };
}

static void write_handle_cqe(struct worker_ctx *w, struct conn *c, int cfd, int res) {
  const struct worker_loop_write_ops wops = worker_loop_build_write_ops();
  worker_loop_write_handle_cqe(w, c, cfd, res, &wops);
}

static void write_ready_handle_cqe(struct worker_ctx *w, struct conn *c, int cfd) {
  if (conn_tls_handshake_pending(c)) {
    tx_notify_poll_disarmed_staged(&c->tx);
    conn_mark_activity(c, w->now_cached_ms);
    const struct worker_loop_tls_hs_ops hs_ops = worker_loop_build_tls_hs_ops();
    (void)worker_loop_tls_handshake_progress(w, c, cfd, &hs_ops);
    return;
  }

  if (c && c->tls_enabled) {
    struct worker_loop_tls_write_ops tops = worker_loop_build_tls_write_ops();
    worker_loop_tls_write_ready_handle_cqe(w, c, cfd, conn_mark_activity, &tops);
    return;
  }

  const struct worker_loop_write_ops wops = worker_loop_build_write_ops();
  worker_loop_write_ready_handle_cqe(w, c, cfd, &wops);
}

static void read_handle_cqe(struct worker_ctx *w,
                            struct conn *c,
                            int cfd,
                            struct io_uring_cqe *cqe) {
  const struct worker_loop_read_ops rops = worker_loop_build_read_ops();

  if (!w || !cqe) {
    return;
  }

  if (!c || cfd < 0 || c->dl.closing) {
    if (c) {
      conn_put(c);
    }
    io_uring_cqe_seen(&w->ring, cqe);
    return;
  }

  if (conn_tls_handshake_pending(c)) {
    conn_disarm_recv(c);
    const struct worker_loop_tls_hs_ops hs_ops = worker_loop_build_tls_hs_ops();

    if (cqe->res < 0 && cqe->res != -EAGAIN && cqe->res != -EINTR) {
      schedule_or_sync_close(w, cfd);
    } else {
      conn_mark_activity(c, w->now_cached_ms);
      (void)worker_loop_tls_handshake_progress(w, c, cfd, &hs_ops);
    }

    io_uring_cqe_seen(&w->ring, cqe);
    conn_put(c);
    return;
  }

  if (c->tls_enabled) {
    const struct worker_loop_tls_write_ops tops = worker_loop_build_tls_write_ops();
    worker_loop_tls_read_handle_cqe(w, c, cfd, cqe, &rops, &tops);
    return;
  }

  worker_loop_read_handle_cqe(w, c, cfd, cqe, &rops);
}
