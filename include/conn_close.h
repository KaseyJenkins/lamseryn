#pragma once

struct worker_ctx;

// Stage an asynchronous close via io_uring when an SQE is available;
// otherwise perform a synchronous close while ownership is still held.
// Returns:
//   0: async close staged
//   1: sync close performed, or fd was already detached/not found
int schedule_or_sync_close(struct worker_ctx *w, int fd);
