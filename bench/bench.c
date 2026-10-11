/*
 * bench.c - allocator benchmark.
 *
 *   bench <impl> <workload>     impl: lm | sys     workload: random | bigrandom
 *                                         | realloc | small | mt4
 *
 * Prints one JSON object (throughput = median of REPS runs after one warm-up run, plus peak-heap /
 * peak-requested fragmentation where the allocator can report its heap size).
 *
 * "lm" resolves to whichever allocator this binary was linked with: the
 * current src/allocator.c, or the pre-rewrite baseline in bench/baseline/
 * (built with -DBASELINE, which measures heap growth through sbrk).
 * Bookkeeping lives in static arrays so it never touches the allocator
 * under test.
 */
#include "allocator.h"
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define REPS 5
#define MAX_THREADS 4
#define MAX_SLOTS 32768

static int use_sys;

static void *do_malloc(size_t n) { return use_sys ? malloc(n) : lm_malloc(n); }
static void do_free(void *p) { use_sys ? free(p) : lm_free(p); }
static void *do_realloc(void *p, size_t n) {
  return use_sys ? realloc(p, n) : lm_realloc(p, n);
}

typedef struct {
  uint64_t s;
} rng_t;
static inline uint32_t rnd(rng_t *r) { /* xorshift64* */
  r->s ^= r->s >> 12;
  r->s ^= r->s << 25;
  r->s ^= r->s >> 27;
  return (uint32_t)((r->s * 2685821657736338717ULL) >> 32);
}

static uint64_t now_ns(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static void die(void) {
  fprintf(stderr, "bench: allocation failed\n");
  exit(1);
}

typedef struct {
  void *p;
  size_t n;
} slot_t;

static slot_t slots[MAX_THREADS][MAX_SLOTS];

typedef struct {
  int tid;
  const char *workload;
  long ops;
  size_t peak_req;
} job_t;

/* Each worker keeps its own live set; peak_req tracks live requested bytes. */
static void run_job(job_t *j) {
  slot_t *sl = slots[j->tid];
  rng_t r = {0x9E3779B97F4A7C15ULL + (uint64_t)j->tid * 7919};
  size_t live = 0, peak = 0;
  int nslots = MAX_SLOTS;
  uint32_t lo = 8, hi = 4096;
  int is_realloc = !strcmp(j->workload, "realloc");
  int is_small = !strcmp(j->workload, "small");
  if (is_realloc) {
    nslots = 128;
    hi = 4096;
  }
  if (is_small) {
    nslots = 20000;
    hi = 128;
  }
  if (!is_small && !is_realloc)
    nslots = !strcmp(j->workload, "bigrandom") ? 16384 : 1024;

  memset(sl, 0, sizeof(slot_t) * MAX_SLOTS);
  for (long i = 0; i < j->ops; i++) {
    uint32_t k = rnd(&r) % (uint32_t)nslots;
    size_t n = lo + rnd(&r) % (hi - lo + 1);
    slot_t *s = &sl[k];
    if (is_realloc) {
      void *q = do_realloc(s->p, n);
      if (!q)
        die();
      live = live - s->n + n;
      s->p = q;
      s->n = n;
    } else if (s->p) {
      do_free(s->p);
      live -= s->n;
      s->p = NULL;
      s->n = 0;
    } else {
      s->p = do_malloc(n);
      if (!s->p)
        die();
      s->n = n;
      live += n;
    }
    if (s->p)
      *(volatile char *)s->p = 1; /* touch so the call cannot be elided */
    if (live > peak)
      peak = live;
  }
  j->peak_req = peak;
  for (int k = 0; k < nslots; k++)
    if (sl[k].p)
      do_free(sl[k].p);
}

static void *thread_main(void *a) {
  run_job((job_t *)a);
  return NULL;
}

static size_t heap_now(void) {
#ifdef BASELINE
  return (size_t)(uintptr_t)sbrk(0);
#else
  return lm_peak_heap_bytes();
#endif
}

static int cmp_d(const void *a, const void *b) {
  double x = *(const double *)a, y = *(const double *)b;
  return (x > y) - (x < y);
}

int main(int argc, char **argv) {
  if (argc != 3) {
    fprintf(stderr, "usage: %s <lm|sys> <random|bigrandom|realloc|small|mt4>\n", argv[0]);
    return 2;
  }
  use_sys = !strcmp(argv[1], "sys");
  const char *wl = argv[2];
  int threads = !strcmp(wl, "mt4") ? 4 : 1;
  const char *inner = threads > 1 ? "bigrandom" : wl;
  long ops = 1000000;
#ifdef BASELINE
  if (!use_sys)
    ops = 200000; /* O(n) list scans: keep the baseline run short */
#endif

  double ops_per_s[REPS];
  double frag = -1;
  size_t peak_req_max = 0, heap_base = 0;
#ifdef BASELINE
  heap_base = heap_now();
#endif
  for (int rep = -1; rep < REPS; rep++) { /* rep -1 is a discarded warm-up */
    job_t jobs[MAX_THREADS];
    pthread_t th[MAX_THREADS];
    uint64_t t0 = now_ns();
    for (int t = 0; t < threads; t++) {
      jobs[t] = (job_t){t, inner, ops / threads, 0};
      if (threads > 1)
        pthread_create(&th[t], NULL, thread_main, &jobs[t]);
    }
    if (threads == 1)
      run_job(&jobs[0]);
    else
      for (int t = 0; t < threads; t++)
        pthread_join(th[t], NULL);
    uint64_t t1 = now_ns();
    if (rep >= 0)
      ops_per_s[rep] = (double)ops / ((double)(t1 - t0) / 1e9);
    size_t req = 0;
    for (int t = 0; t < threads; t++)
      req += jobs[t].peak_req;
    if (req > peak_req_max)
      peak_req_max = req;
  }
  qsort(ops_per_s, REPS, sizeof(double), cmp_d);
  if (!use_sys) {
    size_t heap = heap_now() - heap_base;
    frag = (double)heap / (double)peak_req_max;
    printf("{\"impl\":\"%s\",\"workload\":\"%s\",\"ops\":%ld,\"threads\":%d,"
           "\"ops_per_s_median\":%.0f,\"ops_per_s_min\":%.0f,\"ops_per_s_max\":%.0f,"
           "\"peak_heap_bytes\":%zu,\"peak_requested_bytes\":%zu,\"fragmentation\":%.3f}\n",
           argv[1], wl, ops, threads, ops_per_s[REPS / 2], ops_per_s[0],
           ops_per_s[REPS - 1], heap, peak_req_max, frag);
  } else {
    printf("{\"impl\":\"sys\",\"workload\":\"%s\",\"ops\":%ld,\"threads\":%d,"
           "\"ops_per_s_median\":%.0f,\"ops_per_s_min\":%.0f,\"ops_per_s_max\":%.0f,"
           "\"peak_requested_bytes\":%zu}\n",
           wl, ops, threads, ops_per_s[REPS / 2], ops_per_s[0], ops_per_s[REPS - 1],
           peak_req_max);
  }
  return 0;
}
