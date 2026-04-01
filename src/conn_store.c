#include <stdlib.h>

#include "include/macros.h"
#include "include/types.h"
#include "include/conn.h"
#include "include/conn_store.h"
#include "include/logger.h"

// Per-thread fd-to-conn map state.
static THREAD_LOCAL int *fd_keys = NULL;
static THREAD_LOCAL struct conn **fd_vals = NULL;
static THREAD_LOCAL size_t fd_cap = 0;
static THREAD_LOCAL size_t fd_size = 0;
static THREAD_LOCAL size_t fd_tombs = 0;

static inline size_t fm_mask(void) {
  return fd_cap - 1;
}

// Open-addressing hash map helpers.
int conn_store_init(size_t cap_hint) {
  size_t cap = 256;
  while (cap < cap_hint) {
    cap <<= 1;
  }

  int *keys = malloc(cap * sizeof(*keys));
  struct conn **vals = calloc(cap, sizeof(*vals));
  if (!keys || !vals) {
    free(keys);
    free(vals);
    return -1;
  }
  for (size_t i = 0; i < cap; ++i) {
    keys[i] = FM_EMPTY;
  }

  fd_keys = keys;
  fd_vals = vals;
  fd_cap = cap;
  fd_size = 0;
  fd_tombs = 0;
  return 0;
}

void conn_store_free(void) {
  free(fd_keys);
  fd_keys = NULL;
  free(fd_vals);
  fd_vals = NULL;
  fd_cap = 0;
  fd_size = 0;
  fd_tombs = 0;
}

static int fm_resize(size_t new_cap) {
  int *old_keys = fd_keys;
  struct conn **old_vals = fd_vals;
  size_t old_cap = fd_cap;

  int *new_keys = malloc(new_cap * sizeof(*new_keys));
  struct conn **new_vals = calloc(new_cap, sizeof(*new_vals));
  if (!new_keys || !new_vals) {
    free(new_keys);
    free(new_vals);
    return -1;
  }
  for (size_t i = 0; i < new_cap; ++i) {
    new_keys[i] = FM_EMPTY;
  }

  fd_keys = new_keys;
  fd_vals = new_vals;
  fd_cap = new_cap;
  fd_size = 0;
  fd_tombs = 0;

  size_t mask = new_cap - 1;
  for (size_t i = 0; i < old_cap; ++i) {
    int k = old_keys[i];
    if (k >= 0) {
      size_t j = (size_t)k & mask;
      while (fd_keys[j] >= 0) {
        j = (j + 1) & mask;
      }
      fd_keys[j] = k;
      fd_vals[j] = old_vals[i];
      fd_size++;
    }
  }

  free(old_keys);
  free(old_vals);
  return 0;
}

struct conn *conn_store_get(int fd) {
  if (fd_cap == 0) {
    return NULL;
  }
  size_t mask = fm_mask();
  size_t i = ((size_t)fd) & mask;
  size_t start = i;
  for (;;) {
    int k = fd_keys[i];
    if (k == FM_EMPTY) {
      return NULL;
    }
    if (k == fd) {
      return fd_vals[i];
    }
    i = (i + 1) & mask;
    if (i == start) {
      LOGD_EVERY_N(LOGC_CORE,
                   4096,
                   "fd→conn full-scan cap=%zu size=%zu tombs=%zu key=%d",
                   fd_cap,
                   fd_size,
                   fd_tombs,
                   fd);
      return NULL;
    }
  }
}

int conn_store_put(int fd, struct conn *c) {
  if (fd_cap == 0) {
    // Store must be initialized by conn_store_init() before use.
    return -1;
  }

  size_t occupancy = fd_size + fd_tombs;
  if (((fd_size + 1) * 10u) > (fd_cap * 7u) // live load > 0.7
      || (fd_tombs * 2u) > fd_cap // many tombstones
      || (occupancy + 1) >= fd_cap) {
    if (fm_resize(fd_cap ? (fd_cap << 1) : 256) != 0) {
      return -1;
    }
  }

  size_t mask = fm_mask();
  size_t i = (size_t)fd & mask;
  size_t tomb_idx = (size_t)-1;

  for (;;) {
    int k = fd_keys[i];

    if (k == FM_EMPTY) {
      size_t ins = (tomb_idx != (size_t)-1) ? tomb_idx : i;
      if (ins == tomb_idx) {
        if (fd_tombs) {
          fd_tombs--;
        }
      }
      fd_keys[ins] = fd;
      fd_vals[ins] = c;
      fd_size++;
      return 0;
    }

    if (k == FM_TOMB) {
      if (tomb_idx == (size_t)-1) {
        tomb_idx = i;
      }
    } else if (k == fd) {
      fd_vals[i] = c;
      return 0;
    }

    i = (i + 1) & mask;
  }
}

void conn_store_del(int fd) {
  if (fd_cap == 0) {
    return;
  }
  size_t i = (size_t)fd & fm_mask();
  for (;;) {
    int k = fd_keys[i];
    if (k == FM_EMPTY) {
      return;
    }
    if (k == fd) {
      fd_keys[i] = FM_TOMB;
      fd_vals[i] = NULL;
      fd_size--;
      fd_tombs++;
      return;
    }
    i = (i + 1) & fm_mask();
  }
}
