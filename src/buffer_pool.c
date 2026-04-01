#include <stdlib.h>
#include <string.h>

#include <liburing.h>

#include "include/macros.h"
#include "logger.h"
#include "include/buffer_pool.h"

// Thread-local state for the provided-buffer ring and backing storage.
static THREAD_LOCAL struct io_uring_buf_ring *bp_ring = NULL;
static THREAD_LOCAL uint16_t bp_entries = 0;
static THREAD_LOCAL uint16_t bp_mask = 0;
static THREAD_LOCAL uint16_t bp_group_id = 0;
static THREAD_LOCAL char **bp_pool = NULL;
/*
 * Tracks staged descriptors in the current unpublished batch.
 * For io_uring_buf_ring_add(), `pos` is a batch-local offset, not a
 * monotonic producer index. Reset this counter after each
 * io_uring_buf_ring_advance().
 */
static THREAD_LOCAL uint32_t bp_batch_pos = 0;

int buffer_pool_init(struct io_uring *ring, uint16_t entries, uint16_t group_id) {
  if (!ring || entries == 0) {
    LOGE(LOGC_BUF,
         "buffer_pool_init: invalid args (ring=%p entries=%u)",
         (void *)ring,
         (unsigned)entries);
    return -1;
  }

  // If we are re-initializing, tear down any previous state first.
  if (bp_ring || bp_pool) {
    buffer_pool_destroy(ring);
  }

  int err_br = 0;
  bp_entries = entries;
  bp_group_id = group_id;

  bp_ring = io_uring_setup_buf_ring(ring, bp_entries, bp_group_id, 0, &err_br);
  if (!bp_ring) {
    LOGE(LOGC_BUF, "setup buf ring failed: %s", strerror(-err_br));
    bp_entries = 0;
    bp_group_id = 0;
    return -1;
  }

  io_uring_buf_ring_init(bp_ring);
  bp_batch_pos = 0;
  bp_mask = io_uring_buf_ring_mask(bp_entries);

  bp_pool = (char **)calloc((size_t)bp_entries, sizeof(char *));
  if (!bp_pool) {
    LOGE(LOGC_BUF, "buf pool alloc failed");
    int rc_free = io_uring_free_buf_ring(ring, bp_ring, bp_entries, bp_group_id);
    if (rc_free < 0) {
      LOGW(LOGC_BUF, "free_buf_ring (init fail): %s", strerror(-rc_free));
    }
    bp_ring = NULL;
    bp_entries = 0;
    bp_mask = 0;
    bp_group_id = 0;
    return -1;
  }

  unsigned short added = 0;
  for (unsigned short i = 0; i < bp_entries; ++i) {
    char *b = (char *)malloc(RBUF_SZ);
    if (!b) {
      LOGW(LOGC_BUF, "buf alloc failed at %u", (unsigned)i);
      // Continue with a partial pool when one allocation fails.
      continue;
    }
    bp_pool[i] = b;
    io_uring_buf_ring_add(bp_ring, b, RBUF_SZ, i, bp_mask, i);
    added++;
  }

  if (added > 0) {
    io_uring_buf_ring_advance(bp_ring, added);
  } else {
    LOGE(LOGC_BUF, "no buffers added ...");
    for (unsigned short i = 0; i < bp_entries; ++i) {
      free(bp_pool[i]);
    }
    free(bp_pool);
    bp_pool = NULL;

    int rc_free = io_uring_free_buf_ring(ring, bp_ring, bp_entries, bp_group_id);
    if (rc_free < 0) {
      LOGW(LOGC_BUF, "free_buf_ring (added=0): %s", strerror(-rc_free));
    }
    bp_ring = NULL;
    bp_entries = 0;
    bp_mask = 0;
    bp_group_id = 0;
    return -1;
  }

  return 0;
}

void buffer_pool_destroy(struct io_uring *ring) {
  // Free backing buffers.
  if (bp_pool) {
    for (unsigned short i = 0; i < bp_entries; ++i) {
      free(bp_pool[i]);
    }
    free(bp_pool);
    bp_pool = NULL;
  }

  // Free the provided-buffer ring.
  if (bp_ring && ring) {
    int free_buf_ring_r = io_uring_free_buf_ring(ring, bp_ring, bp_entries, bp_group_id);
    if (free_buf_ring_r < 0) {
      LOGW(LOGC_BUF, "free_buf_ring: %s", strerror(-free_buf_ring_r));
    }
    bp_ring = NULL;
  }

  bp_entries = 0;
  bp_mask = 0;
  bp_group_id = 0;
  bp_batch_pos = 0;
}

uint16_t buffer_pool_entries(void) {
  return bp_entries;
}

uint16_t buffer_pool_group_id(void) {
  return bp_group_id;
}

char *buffer_pool_get(unsigned short bid, int *out_valid) {
  if (out_valid) {
    *out_valid = 0;
  }

  if (!bp_pool || bid >= bp_entries) {
    return NULL;
  }

  char *buf = bp_pool[bid];
  if (!buf) {
    return NULL;
  }

  if (out_valid) {
    *out_valid = 1;
  }
  return buf;
}

int buffer_pool_return(unsigned short bid) {
  if (!bp_ring || !bp_pool) {
    return 0;
  }
  if (bid >= bp_entries) {
    return 0;
  }

  char *buf = bp_pool[bid];
  if (!buf) {
    return 0;
  }

  io_uring_buf_ring_add(bp_ring, buf, RBUF_SZ, bid, bp_mask, 0);
  io_uring_buf_ring_advance(bp_ring, 1);
  return 1;
}
