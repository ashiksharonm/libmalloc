/*
 * test_threads.c - multithreaded stress test.
 *
 * THREADS workers share a table of pointers.  Each step a worker allocates a
 * block stamped with its size and a tag, swaps it into a random table slot
 * with an atomic exchange, then verifies and frees whatever block it got
 * back - usually one allocated by a different thread, so cross-thread frees
 * and concurrent coalescing are exercised.  realloc is mixed in.
 */
#include "allocator.h"
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define THREADS 8
#define TABLE 512
#ifndef ITERS
#define ITERS 20000
#endif

static _Atomic(unsigned char *) table[TABLE];

typedef struct {
  size_t n;
  uint64_t tag;
} stamp_t;

static unsigned char *make(size_t n, uint64_t tag) {
  unsigned char *p = lm_malloc(n);
  assert(p && ((uintptr_t)p & 15) == 0);
  stamp_t st = {n, tag};
  memcpy(p, &st, sizeof st);
  memset(p + sizeof st, (int)(tag & 0xFF), n - sizeof st);
  return p;
}

static void verify(const unsigned char *p) {
  stamp_t st;
  memcpy(&st, p, sizeof st);
  for (size_t i = sizeof st; i < st.n; i += 37)
    assert(p[i] == (unsigned char)(st.tag & 0xFF));
}

static void *worker(void *arg) {
  uint64_t x = 0x9E3779B97F4A7C15ULL * (uint64_t)((uintptr_t)arg + 1);
  for (int i = 0; i < ITERS; i++) {
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    size_t n = sizeof(stamp_t) + (x >> 40) % (((x >> 8) & 7) == 0 ? 20000 : 600);
    unsigned char *mine = make(n, x);
    if ((x >> 20) % 4 == 0) { /* realloc keeps the stamp's tag, so re-stamp */
      size_t n2 = sizeof(stamp_t) + (x >> 44) % 3000;
      mine = lm_realloc(mine, n2);
      assert(mine);
      stamp_t st = {n2, x};
      memcpy(mine, &st, sizeof st);
      memset(mine + sizeof st, (int)(x & 0xFF), n2 - sizeof st);
    }
    unsigned char *old = atomic_exchange(&table[(x >> 16) % TABLE], mine);
    if (old) {
      verify(old);
      lm_free(old);
    }
  }
  return NULL;
}

int main(void) {
  pthread_t th[THREADS];
  for (long t = 0; t < THREADS; t++)
    assert(pthread_create(&th[t], NULL, worker, (void *)t) == 0);
  for (int t = 0; t < THREADS; t++)
    pthread_join(th[t], NULL);
  for (int i = 0; i < TABLE; i++) {
    unsigned char *p = atomic_exchange(&table[i], NULL);
    if (p) {
      verify(p);
      lm_free(p);
    }
  }
  assert(lm_check() == 0);
  lm_stats_t s;
  lm_get_stats(&s);
  assert(s.used_blocks == 0 && s.large_blocks == 0);
  printf("threads test: %d threads x %d iterations OK (%zu chunk(s), heap verified)\n",
         THREADS, ITERS, s.chunks);
  return 0;
}
