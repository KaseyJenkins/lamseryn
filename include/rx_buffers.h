#pragma once

#include <liburing.h>

struct worker_ctx;

struct rx_buf_view {
  int have_buf;
  unsigned short bid;
  int valid_bid;
  char *buf_ptr;
};

void rx_buf_from_cqe(struct worker_ctx *w, struct io_uring_cqe *cqe, struct rx_buf_view *out);

void rx_buf_return(struct worker_ctx *w, unsigned short bid);
