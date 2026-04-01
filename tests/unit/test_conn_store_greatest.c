// Greatest tests for the per-thread fd→conn map (open addressing, linear probe).

#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h> // isatty, fileno
#include <time.h>

#include "include/conn_store.h"
#include "include/conn.h"

#include "../vendor/greatest_color.h"
#include "../vendor/greatest.h"

static struct conn *make_conn(int fd) {
  struct conn *c = (struct conn *)calloc(1, sizeof(*c));
  c->fd = fd;
  return c;
}

TEST t_get_before_init(void) {
  // Safe: get should return NULL when map not initialized
  ASSERT_EQ(conn_store_get(12345), NULL);
  PASS();
}

TEST t_init_put_get_del(void) {
  ASSERT_EQ(conn_store_init(2), 0); // min cap is 256, grows as needed

  struct conn *c10 = make_conn(10);
  struct conn *c11 = make_conn(11);
  struct conn *c12 = make_conn(12);

  ASSERT_EQ(conn_store_put(10, c10), 0);
  ASSERT_EQ(conn_store_put(11, c11), 0);
  ASSERT_EQ(conn_store_put(12, c12), 0);

  ASSERT_EQ(conn_store_get(10), c10);
  ASSERT_EQ(conn_store_get(11), c11);
  ASSERT_EQ(conn_store_get(12), c12);

  conn_store_del(11);
  ASSERT_EQ(conn_store_get(11), NULL);

  // Reinsert after delete (tombstone reuse)
  struct conn *c11b = make_conn(11);
  ASSERT_EQ(conn_store_put(11, c11b), 0);
  ASSERT_EQ(conn_store_get(11), c11b);

  conn_store_free();
  free(c10);
  free(c11);
  free(c12);
  free(c11b);
  PASS();
}

// Overwrite value for same fd
TEST t_overwrite_updates_value(void) {
  ASSERT_EQ(conn_store_init(256), 0);

  int fd = 42;
  struct conn *c1 = make_conn(fd);
  struct conn *c2 = make_conn(fd);

  ASSERT_EQ(conn_store_put(fd, c1), 0);
  ASSERT_EQ(conn_store_get(fd), c1);

  ASSERT_EQ(conn_store_put(fd, c2), 0);
  ASSERT_EQ(conn_store_get(fd), c2);

  conn_store_free();
  free(c1);
  free(c2);
  PASS();
}

// Collisions: linear probing — keys i, i+256, i+512 share the same initial slot (cap is power of two)
TEST t_linear_probe_collisions(void) {
  ASSERT_EQ(conn_store_init(256), 0);

  int base = 10;
  struct conn *c0 = make_conn(base);
  struct conn *c1 = make_conn(base + 256);
  struct conn *c2 = make_conn(base + 512);

  ASSERT_EQ(conn_store_put(base, c0), 0);
  ASSERT_EQ(conn_store_put(base + 256, c1), 0);
  ASSERT_EQ(conn_store_put(base + 512, c2), 0);

  ASSERT_EQ(conn_store_get(base), c0);
  ASSERT_EQ(conn_store_get(base + 256), c1);
  ASSERT_EQ(conn_store_get(base + 512), c2);

  conn_store_free();
  free(c0);
  free(c1);
  free(c2);
  PASS();
}

// Force high load to trigger resize; verify retrieval after growth
TEST t_resize_pressure(void) {
  ASSERT_EQ(conn_store_init(256), 0);

  enum {
    N = 600
  }; // > 0.7 load of 256; triggers growth paths during inserts
  struct conn *cs[N] = {0};

  for (int i = 0; i < N; ++i) {
    cs[i] = make_conn(i);
    ASSERT_EQm("put failed", conn_store_put(i, cs[i]), 0);
  }
  for (int i = 0; i < N; ++i) {
    ASSERT_EQm("get mismatch after growth", conn_store_get(i), cs[i]);
  }

  conn_store_free();
  for (int i = 0; i < N; ++i) {
    free(cs[i]);
  }
  PASS();
}

// Deleting missing keys should be safe (no-op)
TEST t_delete_nonexistent_safe(void) {
  ASSERT_EQ(conn_store_init(256), 0);
  conn_store_del(999999);
  conn_store_free();
  PASS();
}

// Stress: random mixed workload; validates functional correctness only
TEST t_stress_random_mixed(void) {
  ASSERT_EQ(conn_store_init(256), 0);

  enum {
    MAXFD = 4096,
    OPS = 20000
  };
  struct conn *expect[MAXFD] = {0};

  unsigned seed = (unsigned)time(NULL);
  for (int i = 0; i < OPS; ++i) {
    int op = rand_r(&seed) % 3; // 0=put,1=get,2=del
    int fd = rand_r(&seed) % MAXFD;

    if (op == 0) {
      struct conn *prev = expect[fd];
      struct conn *c = make_conn(fd);
      ASSERT_EQ(conn_store_put(fd, c), 0);
      if (prev) {
        free(prev); // replaced pointer
      }
      expect[fd] = c;
    } else if (op == 1) {
      struct conn *got = conn_store_get(fd);
      ASSERT_EQ(got, expect[fd]);
    } else {
      conn_store_del(fd);
      if (expect[fd]) {
        free(expect[fd]);
        expect[fd] = NULL;
      }
    }
  }

  // Cleanup any remaining
  for (int fd = 0; fd < MAXFD; ++fd) {
    if (expect[fd]) {
      conn_store_del(fd);
      free(expect[fd]);
    }
  }
  conn_store_free();
  PASS();
}

TEST t_update_with_tomb_does_not_duplicate(void) {
  // Start with a small map to keep probe chains short and deterministic.
  ASSERT_EQ(conn_store_init(256), 0);

  // Choose three FDs that hash to the same home slot with cap=256:
  // base, base+256, base+512 collide due to (fd & 255).
  int base = 1000; // arbitrary
  int fdY = base; // slot s
  int fdZ = base + 256; // slot s+1
  int fdX = base + 512; // slot s+2 (the “target” key we’ll update)

  struct conn *cY = (struct conn *)calloc(1, sizeof(*cY));
  cY->fd = fdY;
  struct conn *cZ = (struct conn *)calloc(1, sizeof(*cZ));
  cZ->fd = fdZ;
  struct conn *cX1 = (struct conn *)calloc(1, sizeof(*cX1));
  cX1->fd = fdX; // original X
  struct conn *cX2 = (struct conn *)calloc(1, sizeof(*cX2));
  cX2->fd = fdX; // update X

  // 1) Build the cluster: Y -> Z -> X
  ASSERT_EQ(conn_store_put(fdY, cY), 0);
  ASSERT_EQ(conn_store_put(fdZ, cZ), 0);
  ASSERT_EQ(conn_store_put(fdX, cX1), 0);
  ASSERT_EQ(conn_store_get(fdX), cX1);

  // 2) Delete the front element to create a tombstone before X.
  conn_store_del(fdY);

  // 3) Update X while a tombstone is before it in the probe chain.
  //    Fixed put(): scans past tomb, finds X, updates in place.
  //    Old put():  inserts at tomb → creates a duplicate of X.
  ASSERT_EQ(conn_store_put(fdX, cX2), 0);
  ASSERT_EQ(conn_store_get(fdX), cX2); // should see the updated pointer

  // 4) Delete X once; if a duplicate exists (old bug), one X remains.
  conn_store_del(fdX);
  ASSERT_EQ(conn_store_get(fdX), NULL); // with the fix, it must be gone

  // Cleanup
  conn_store_free();
  free(cY);
  free(cZ);
  free(cX1);
  free(cX2);
  PASS();
}

TEST t_wraparound_cluster_insertion_and_reuse_tomb(void) {
  ASSERT_EQ(conn_store_init(256), 0);

  // Choose keys that all map to the same home slot (mask=255),
  // and build a cluster that wraps around the end of the table.
  int fdA = 255; // home slot s=255
  int fdB = 255 + 256; // s -> wraps to 0
  int fdC = 255 + 512; // s -> wraps to 1

  struct conn *cA = (struct conn *)calloc(1, sizeof(*cA));
  cA->fd = fdA;
  struct conn *cB = (struct conn *)calloc(1, sizeof(*cB));
  cB->fd = fdB;
  struct conn *cC = (struct conn *)calloc(1, sizeof(*cC));
  cC->fd = fdC;

  ASSERT_EQ(conn_store_put(fdA, cA), 0); // index 255
  ASSERT_EQ(conn_store_put(fdB, cB), 0); // index 0
  ASSERT_EQ(conn_store_put(fdC, cC), 0); // index 1

  ASSERT_EQ(conn_store_get(fdA), cA);
  ASSERT_EQ(conn_store_get(fdB), cB);
  ASSERT_EQ(conn_store_get(fdC), cC);

  // Delete the head to create a tombstone at index 255
  conn_store_del(fdA);
  ASSERT_EQ(conn_store_get(fdA), NULL);

  // Insert a new colliding key; it should reuse the tombstone slot at 255
  int fdD = 255 + 768; // also maps to slot 255
  struct conn *cD = (struct conn *)calloc(1, sizeof(*cD));
  cD->fd = fdD;
  ASSERT_EQ(conn_store_put(fdD, cD), 0);

  ASSERT_EQ(conn_store_get(fdD), cD);
  ASSERT_EQ(conn_store_get(fdB), cB);
  ASSERT_EQ(conn_store_get(fdC), cC);

  conn_store_free();
  free(cA);
  free(cB);
  free(cC);
  free(cD);
  PASS();
}

SUITE(s_conn_store) {
  RUN_TEST(t_get_before_init);
  RUN_TEST(t_init_put_get_del);
  RUN_TEST(t_overwrite_updates_value);
  RUN_TEST(t_linear_probe_collisions);
  RUN_TEST(t_resize_pressure);
  RUN_TEST(t_delete_nonexistent_safe);
  RUN_TEST(t_update_with_tomb_does_not_duplicate);
  RUN_TEST(t_wraparound_cluster_insertion_and_reuse_tomb);
  RUN_TEST(t_stress_random_mixed);
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv) {
  GREATEST_MAIN_BEGIN();
  RUN_SUITE(s_conn_store);
  GREATEST_MAIN_END();
}
