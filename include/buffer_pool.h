#pragma once

#include <stdint.h>
#include <liburing.h>

// Simple per-thread buffer pool wrapper around io_uring provided buffers.
// All state is kept thread-local inside the implementation.

// Initialize the buffer pool for this thread.
// Returns 0 on success, non-zero on failure (error is logged).
int buffer_pool_init(struct io_uring *ring, uint16_t entries, uint16_t group_id);

// Destroy the buffer pool for this thread, freeing buffers and the buf_ring.
// Safe to call even if initialization failed or was skipped.
void buffer_pool_destroy(struct io_uring *ring);

// Return the number of buffer ring entries configured for this thread.
// Returns 0 if the pool is not initialized.
uint16_t buffer_pool_entries(void);

// Return the buffer group id configured for this thread's pool.
// Returns 0 if the pool is not initialized.
uint16_t buffer_pool_group_id(void);

// Lookup a buffer pointer for a given buffer-id.
// If out_valid is non-NULL, it is set to 1 when bid is in-range and
// a non-NULL buffer pointer exists; otherwise set to 0.
// Returns NULL if bid is out-of-range or the slot is empty.
char *buffer_pool_get(unsigned short bid, int *out_valid);

// Return a buffer to the pool. Returns 1 if the buffer-id was in-range
// and a non-NULL buffer was re-added to the ring, 0 otherwise.
int buffer_pool_return(unsigned short bid);
