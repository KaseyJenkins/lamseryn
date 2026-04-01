#include "instrumentation/instrumentation.h"
#include "instrumentation/counters_update.h"

#include "include/logger.h"
#include "include/buffer_pool.h"
#include "include/worker_ctx.h"
#include "include/rx_buffers.h"

void rx_buf_from_cqe(struct worker_ctx *w, struct io_uring_cqe *cqe, struct rx_buf_view *out) {
  if (!out) {
    return;
  }

  out->have_buf = 0;
  out->bid = 0;
  out->valid_bid = 0;
  out->buf_ptr = NULL;

  if (!w || !cqe) {
    return;
  }

  out->have_buf = (cqe->flags & IORING_CQE_F_BUFFER) ? 1 : 0;
  out->bid = out->have_buf ? (unsigned short)(cqe->flags >> IORING_CQE_BUFFER_SHIFT) : 0;

  if (!out->have_buf) {
    return;
  }

  out->buf_ptr = buffer_pool_get(out->bid, &out->valid_bid);
  if (out->valid_bid && out->buf_ptr) {
    CTR_INC_DEV(w, buf_in_use);
    CTR_UPDATE_PEAK_DEV(w, buf_in_use, buf_in_use_max);
  } else if (!out->valid_bid) {
    CTR_INC_DEV(w, cnt_buf_badid);
    LOGW(LOGC_BUF,
         "OP_READ bad buf id bid=%u entries=%u flags=0x%x",
         (unsigned)out->bid,
         (unsigned)buffer_pool_entries(),
         cqe->flags);
  }
}

void rx_buf_return(struct worker_ctx *w, unsigned short bid) {
  if (!w) {
    return;
  }

  if (buffer_pool_return(bid)) {
    CTR_DEC_DEV_IF(w, buf_in_use);
  }
}
