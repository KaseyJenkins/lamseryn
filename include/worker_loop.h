#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <liburing.h>

#include "include/http_pipeline.h"

struct worker_ctx;
struct op_ctx;

// Collect a CQE batch for the worker loop.
// Returns 0 on success and fills `out_count` (>=1 if a CQE is available).
// Returns a negative liburing-style error code from wait path on failure.
int worker_loop_fetch_cqes(struct worker_ctx *w,
                           struct io_uring_cqe **cqes,
                           unsigned max_cqes,
                           unsigned *out_count);

// Decode CQE user_data into an op context pointer.
// Returns NULL when user_data is zero or the op context fails magic validation.
struct op_ctx *worker_loop_decode_op(const struct io_uring_cqe *cqe);

// Prepare one CQE for non-accept dispatch.
// Returns non-zero when the CQE was fully handled (seen + continue path).
// Returns 0 and stores a valid op context in `out_op` when caller should continue switch dispatch.
int worker_loop_prepare_dispatch(struct worker_ctx *w,
                                 struct io_uring_cqe *cqe,
                                 struct op_ctx **out_op);

// Handle non-connection op types owned by worker loop helpers.
// Returns non-zero when the op was handled (including CQE seen).
int worker_loop_try_handle_nonconn(struct worker_ctx *w,
                                   struct op_ctx *op,
                                   struct io_uring_cqe *cqe,
                                   int is_running);

// Handle connection-bound op types owned by worker loop helpers.
// Returns non-zero when the op was handled (including CQE seen when applicable).
int worker_loop_try_handle_conn(struct worker_ctx *w,
                                struct op_ctx *op,
                                struct io_uring_cqe *cqe);

// Process one fetched CQE batch through decode/dispatch helpers.
// Returns 0 after processing all entries.
int worker_loop_process_cqe_batch(struct worker_ctx *w,
                                  struct io_uring_cqe **cqes,
                                  unsigned cqe_count,
                                  int is_running);

struct worker_loop_read_ops {
  int (*is_closing_no_deadline)(const struct conn *c);
  void (*post_recv_ptr)(struct worker_ctx *w, struct conn *c);
  int (*schedule_or_sync_close)(struct worker_ctx *w, int cfd);
  void (*mark_activity)(struct conn *c, uint64_t now_ms);
  void (*split_on_header_end)(struct conn *c,
                              const char *buf,
                              size_t len,
                              size_t *feed_len,
                              size_t *tail_len);
  void (*rx_tail_update_after_feed)(struct conn *c, const char *buf, size_t len);
  int (*rx_stash_append)(struct conn *c, const char *buf, size_t len);
  void (*apply_action)(struct worker_ctx *w,
                       struct conn *c,
                       int cfd,
                       struct http_pipeline_result agg,
                       const char *buf,
                       size_t n);
  void (*conn_put)(struct conn *c);
};

// Process received bytes through HTTP pipeline/stash/apply path.
void worker_loop_process_rx_bytes(struct worker_ctx *w,
                                  struct conn *c,
                                  int cfd,
                                  const char *buf_ptr,
                                  size_t n,
                                  const struct worker_loop_read_ops *ops);

// Read CQE handler.
void worker_loop_read_handle_cqe(struct worker_ctx *w,
                                 struct conn *c,
                                 int cfd,
                                 struct io_uring_cqe *cqe,
                                 const struct worker_loop_read_ops *ops);

struct worker_loop_write_ops {
  void (*maybe_flush)(struct worker_ctx *w, int urgent);
  void (*arm_write_timeout)(struct worker_ctx *w, struct conn *c);
  void (*clear_write_timeout)(struct worker_ctx *w, struct conn *c);
  void (*tx_close_file)(struct conn *c);
  void (*emit_access_log)(struct worker_ctx *w, struct conn *c);
  void (*post_recv_ptr)(struct worker_ctx *w, struct conn *c);
  void (*conn_reset_request)(struct worker_ctx *w, struct conn *c);
  int (*conn_try_process_stash)(struct worker_ctx *w, struct conn *c, int cfd);
  void (*tw_reschedule)(struct worker_ctx *w, struct conn *c, uint64_t now_ms);
  void (*conn_ref)(struct conn *c);
  void (*conn_put)(struct conn *c);
  int (*schedule_or_sync_close)(struct worker_ctx *w, int cfd);
};

struct worker_loop_tls_write_ops {
  void (*arm_write_timeout)(struct worker_ctx *w, struct conn *c);
  int (*post_tls_pollout_ptr)(struct worker_ctx *w, struct conn *c);
  int (*is_closing_no_deadline)(const struct conn *c);
  void (*post_recv_ptr)(struct worker_ctx *w, struct conn *c);
  int (*schedule_or_sync_close)(struct worker_ctx *w, int cfd);
  void (*write_handle_cqe)(struct worker_ctx *w, struct conn *c, int cfd, int res);
};

struct worker_loop_tls_hs_ops {
  int (*is_closing_no_deadline)(const struct conn *c);
  void (*post_recv_ptr)(struct worker_ctx *w, struct conn *c);
  int (*post_tls_pollout_ptr)(struct worker_ctx *w, struct conn *c);
  void (*tw_reschedule)(struct worker_ctx *w, struct conn *c, uint64_t now_ms);
  int (*schedule_or_sync_close)(struct worker_ctx *w, int cfd);
};

// Advance TLS handshake state machine and perform transport readiness actions.
// Returns 1 on handshake completion, 0 when handshake remains in progress,
// and -1 on fatal error (close path scheduled).
int worker_loop_tls_handshake_progress(struct worker_ctx *w,
                                       struct conn *c,
                                       int cfd,
                                       const struct worker_loop_tls_hs_ops *ops);

// Post a POLLOUT watcher for TLS transport progression.
int worker_loop_post_tls_pollout(struct worker_ctx *w, struct conn *c);

// Try to push pending TX header bytes over TLS transport.
// Returns 0 when progression was handled, -1 on fatal path/invalid input.
int worker_loop_tls_try_send_pending(struct worker_ctx *w,
                                     struct conn *c,
                                     int cfd,
                                     const struct worker_loop_tls_write_ops *ops);

// Handle post-handshake TLS read CQEs using TLS recv wrappers.
void worker_loop_tls_read_handle_cqe(struct worker_ctx *w,
                                     struct conn *c,
                                     int cfd,
                                     struct io_uring_cqe *cqe,
                                     const struct worker_loop_read_ops *read_ops,
                                     const struct worker_loop_tls_write_ops *tls_ops);

// Handle TLS write-ready progression by disarming pollout and advancing
// pending TLS writes.
void worker_loop_tls_write_ready_handle_cqe(struct worker_ctx *w,
                                            struct conn *c,
                                            int cfd,
                                            void (*mark_activity)(struct conn *c, uint64_t now_ms),
                                            const struct worker_loop_tls_write_ops *tls_ops);

// Shared write-path utility helpers.
bool worker_loop_cancel_write_poll_if_armed(struct worker_ctx *w, struct conn *c);

void worker_loop_write_post_remaining(struct worker_ctx *w,
                                      struct conn *c,
                                      int cfd,
                                      int guard_if_closing,
                                      const struct worker_loop_write_ops *ops);

// Write CQE handlers.
void worker_loop_write_handle_cqe(struct worker_ctx *w,
                                  struct conn *c,
                                  int cfd,
                                  int res,
                                  const struct worker_loop_write_ops *ops);

void worker_loop_write_ready_handle_cqe(struct worker_ctx *w,
                                        struct conn *c,
                                        int cfd,
                                        const struct worker_loop_write_ops *ops);

// ---------------------------------------------------------------------------
// CQE dispatch helpers
// ---------------------------------------------------------------------------

// Post a recv (or POLLIN for TLS) SQE for the given connection.
void post_recv_ptr(struct worker_ctx *w, struct conn *c);

// Stage a header-only response send (e.g. 400, 404, 431, 500).
void stage_header_response_send(struct worker_ctx *w,
                                struct conn *c,
                                int cfd,
                                struct io_uring_sqe *sqe_w,
                                enum resp_kind kind,
                                int keepalive,
                                int drain_after_headers,
                                int close_after_send);

// Stage a TX-buffer response send (static file header path).
void stage_tx_buffer_send(struct worker_ctx *w,
                          struct conn *c,
                          int cfd,
                          struct io_uring_sqe *sqe_w);

// Central HTTP action dispatcher: converts an http_pipeline_result into a
// response send or continuation.
void http_apply_action(struct worker_ctx *w,
                       struct conn *c,
                       int cfd,
                       struct http_pipeline_result hres,
                       const char *chunk,
                       size_t chunk_len);

// Process pipelined stash bytes through the HTTP parser and dispatch actions.
// Returns non-zero when a response was staged.
int conn_try_process_stash(struct worker_ctx *w, struct conn *c, int cfd);

// Vtable builders: construct ops structs for the CQE dispatch handlers.
struct worker_loop_read_ops worker_loop_build_read_ops(void);
struct worker_loop_write_ops worker_loop_build_write_ops(void);
struct worker_loop_tls_write_ops worker_loop_build_tls_write_ops(void);
struct worker_loop_tls_hs_ops worker_loop_build_tls_hs_ops(void);


