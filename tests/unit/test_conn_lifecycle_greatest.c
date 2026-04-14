// Unit tests for src/conn_lifecycle.c
//
// Covers: atomic conn counters, freelist alloc/recycle/drain,
//         refcount transitions (conn_ref/conn_put), tx_close_file,
//         and conn_freelist_log_stats smoke.
//
// Heavy-weight functions (conn_init, conn_reset_request,
// schedule_or_sync_close) are exercised by integration tests.
#include "../vendor/greatest_color.h"
#include "../vendor/greatest.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "include/macros.h"
#include "include/conn.h"
#include "include/conn_lifecycle.h"

// ------------------------------------------------------------------
// Stubs for symbols pulled in transitively by conn_lifecycle.c
// ------------------------------------------------------------------

// tls_conn_destroy: called by conn_recycle when tls_enabled/tls_handle.
void tls_conn_destroy(struct conn *c) {
  if (c) {
    c->tls_enabled = 0;
    c->tls_handle = NULL;
  }
}

// rx_stash_free: called by conn_recycle to release stash block.
void rx_stash_free(char *p) {
  free(p);
}

// ==================================================================
// Suite: active_conns atomic counters
// ==================================================================

TEST t_active_conns_inc_dec(void) {
  // Reset to known state: do a few decs to drain any leftover.
  while (active_conns_total() > 0) {
    active_conns_dec();
  }
  ASSERT_EQ(active_conns_total(), 0u);

  ASSERT_EQ(active_conns_inc(), 1u);
  ASSERT_EQ(active_conns_inc(), 2u);
  ASSERT_EQ(active_conns_total(), 2u);

  ASSERT_EQ(active_conns_dec(), 1u);
  ASSERT_EQ(active_conns_dec(), 0u);
  ASSERT_EQ(active_conns_total(), 0u);
  PASS();
}

TEST t_active_conns_dec_underflow_guard(void) {
  while (active_conns_total() > 0) {
    active_conns_dec();
  }
  // Decrementing from zero must stay at zero (no wrap-around).
  ASSERT_EQ(active_conns_dec(), 0u);
  ASSERT_EQ(active_conns_dec(), 0u);
  ASSERT_EQ(active_conns_total(), 0u);
  PASS();
}

SUITE(s_active_conns) {
  RUN_TEST(t_active_conns_inc_dec);
  RUN_TEST(t_active_conns_dec_underflow_guard);
}

// ==================================================================
// Suite: freelist alloc / recycle / drain
// ==================================================================

TEST t_alloc_from_empty_returns_zeroed(void) {
  // Start from a drained freelist.
  conn_freelist_drain();
  struct conn *c = conn_alloc();
  ASSERT(c != NULL);
  ASSERT_EQ(c->fd, 0);       // calloc zeroes everything
  ASSERT_EQ(c->refcnt, 0);
  free(c);
  PASS();
}

TEST t_recycle_then_alloc_reuses(void) {
  conn_freelist_drain();

  // Allocate, mark, recycle.
  struct conn *c = conn_alloc();
  ASSERT(c != NULL);
  c->fd = 42;
  c->refcnt = 1;
  conn_recycle(c);

  // Next alloc should return the same pointer (from freelist).
  struct conn *c2 = conn_alloc();
  ASSERT(c2 == c);
  // memset(0) happens inside conn_alloc on freelist hit.
  ASSERT_EQ(c2->fd, 0);
  free(c2);
  PASS();
}

TEST t_drain_empties_freelist(void) {
  conn_freelist_drain();

  // Recycle three conns into the freelist.
  for (int i = 0; i < 3; ++i) {
    struct conn *c = (struct conn *)calloc(1, sizeof(*c));
    c->fd = -1;
    conn_recycle(c);
  }

  // Drain should free them all.
  conn_freelist_drain();

  // Next alloc must go to calloc (no freelist hit).
  struct conn *c = conn_alloc();
  ASSERT(c != NULL);
  free(c);
  PASS();
}

SUITE(s_freelist) {
  RUN_TEST(t_alloc_from_empty_returns_zeroed);
  RUN_TEST(t_recycle_then_alloc_reuses);
  RUN_TEST(t_drain_empties_freelist);
}

// ==================================================================
// Suite: refcount (conn_ref / conn_put)
// ==================================================================

TEST t_ref_increments(void) {
  conn_freelist_drain();

  struct conn *c = conn_alloc();
  c->refcnt = 1;
  conn_ref(c);
  ASSERT_EQ(c->refcnt, 2);
  conn_ref(c);
  ASSERT_EQ(c->refcnt, 3);
  free(c);
  PASS();
}

TEST t_put_decrements(void) {
  conn_freelist_drain();

  struct conn *c = conn_alloc();
  c->refcnt = 3;
  conn_put(c);
  ASSERT_EQ(c->refcnt, 2);
  conn_put(c);
  ASSERT_EQ(c->refcnt, 1);
  // Don't call conn_put again: refcnt 0 triggers conn_recycle and the
  // pointer moves to the freelist. Drain after.
  free(c);
  PASS();
}

TEST t_put_to_zero_recycles(void) {
  conn_freelist_drain();

  struct conn *c = conn_alloc();
  ASSERT(c != NULL);
  c->fd = -1;
  c->refcnt = 1;

  // conn_put with refcnt==1 should call conn_recycle internally.
  conn_put(c);

  // The conn should now be on the freelist: verify by allocating it back.
  struct conn *c2 = conn_alloc();
  ASSERT_EQ(c2, c);
  free(c2);
  PASS();
}

TEST t_put_null_safe(void) {
  conn_put(NULL);   // must not crash
  conn_ref(NULL);   // must not crash
  PASS();
}

SUITE(s_refcount) {
  RUN_TEST(t_ref_increments);
  RUN_TEST(t_put_decrements);
  RUN_TEST(t_put_to_zero_recycles);
  RUN_TEST(t_put_null_safe);
}

// ==================================================================
// Suite: tx_close_file
// ==================================================================

TEST t_tx_close_file_closes_fd(void) {
  int pfd[2];
  if (pipe(pfd) != 0) {
    SKIPm("pipe() failed");
  }
  struct conn c;
  memset(&c, 0, sizeof(c));
  c.tx.file_fd = pfd[0];
  c.tx.file_off = 100;
  c.tx.file_rem = 200;
  int closed_fd = c.tx.file_fd;

  tx_close_file(&c);

  ASSERT_EQ(c.tx.file_fd, -1);
  ASSERT_EQ(c.tx.file_off, 0);
  ASSERT_EQ((int)c.tx.file_rem, 0);

  errno = 0;
  ASSERT_EQ(fcntl(closed_fd, F_GETFD), -1);
  ASSERT_EQ(errno, EBADF);

  // Close the write end; this is just cleanup.
  close(pfd[1]);
  PASS();
}

TEST t_tx_close_file_noop_when_no_fd(void) {
  struct conn c;
  memset(&c, 0, sizeof(c));
  c.tx.file_fd = -1;
  c.tx.file_off = 10;
  c.tx.file_rem = 20;

  tx_close_file(&c);

  ASSERT_EQ(c.tx.file_fd, -1);
  ASSERT_EQ(c.tx.file_off, 0);
  ASSERT_EQ((int)c.tx.file_rem, 0);
  PASS();
}

TEST t_tx_close_file_null_safe(void) {
  tx_close_file(NULL);   // must not crash
  PASS();
}

SUITE(s_tx_close_file) {
  RUN_TEST(t_tx_close_file_closes_fd);
  RUN_TEST(t_tx_close_file_noop_when_no_fd);
  RUN_TEST(t_tx_close_file_null_safe);
}

// ==================================================================
// Suite: freelist log stats (smoke)
// ==================================================================

TEST t_log_stats_smoke(void) {
  conn_freelist_log_stats(0);   // must not crash
  conn_freelist_log_stats(99);
  PASS();
}

SUITE(s_log_stats) {
  RUN_TEST(t_log_stats_smoke);
}

// ==================================================================
// Main
// ==================================================================

GREATEST_MAIN_DEFS();

int main(int argc, char **argv) {
  GREATEST_MAIN_BEGIN();
  RUN_SUITE(s_active_conns);
  RUN_SUITE(s_freelist);
  RUN_SUITE(s_refcount);
  RUN_SUITE(s_tx_close_file);
  RUN_SUITE(s_log_stats);
  GREATEST_MAIN_END();
}
