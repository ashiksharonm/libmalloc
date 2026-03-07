/*
 * bench.c — Benchmark: libmalloc vs glibc malloc
 *
 * Measures wall-clock time for three workload sizes:
 *   1,000 / 10,000 / 100,000 mixed alloc+free operations
 *
 * Methodology
 * -----------
 *   For each N:
 *     - Allocate N blocks of pseudo-random sizes (8–4096 bytes)
 *     - Write one byte to each block (prevents the compiler from
 *       optimising the allocation away)
 *     - Free all N blocks in random order
 *     - Repeat RUNS times; report the MINIMUM time (reduces OS scheduler noise)
 *   Run the same workload with lm_malloc and then with glibc malloc.
 *   The SAME pseudo-random sequence is used for both (reproducibility).
 *
 * Build
 * -----
 *   Compiled by: make bench           (first-fit libmalloc vs glibc)
 *                make bench_best_fit  (best-fit  libmalloc vs glibc)
 *
 * Run
 * ---
 *   ./build/bench
 *
 * Important note on LD_PRELOAD
 * ----------------------------
 *   The LD_PRELOAD trick (swapping symbols at runtime) requires building a
 *   shared library — that is Phase 3b.  This binary instead calls lm_malloc
 *   and glibc malloc explicitly in the same process, which is cleaner to
 *   understand and just as honest for the benchmark.
 */

#include "../src/allocator.h"
#include <stdint.h> /* uint64_t                        */
#include <stdio.h>
#include <stdlib.h> /* malloc / free / rand            */
#include <string.h> /* memset                          */
#include <time.h>   /* clock_gettime, CLOCK_MONOTONIC  */

/* -------------------------------------------------------------------------
 * Configuration
 * ------------------------------------------------------------------------- */
#define RUNS 5        /* repeat each measurement; take minimum       */
#define SEED 0xDEAD42 /* fixed RNG seed for reproducibility          */
#define MIN_SIZE 8    /* smallest allocation (bytes)                 */
#define MAX_SIZE 4096 /* largest  allocation (bytes)                 */

static const int WORKLOADS[] = {1000, 10000, 100000};
#define NUM_WORKLOADS (int)(sizeof(WORKLOADS) / sizeof(WORKLOADS[0]))

/* -------------------------------------------------------------------------
 * Tiny LCG pseudo-random number generator
 *
 * We roll our own (not rand()) so we can reset the seed deterministically
 * before EACH allocator's run, guaranteeing identical size sequences.
 *
 * LCG parameters from Numerical Recipes:
 *   x_{n+1} = (1664525 * x_n + 1013904223) mod 2^32
 * ------------------------------------------------------------------------- */
static uint32_t lcg_state;

static void lcg_seed(uint32_t seed) { lcg_state = seed; }

static uint32_t lcg_next(void) {
  lcg_state = lcg_state * 1664525u + 1013904223u;
  return lcg_state;
}

/* Random allocation size in [MIN_SIZE, MAX_SIZE] */
static size_t rand_size(void) {
  return MIN_SIZE + (lcg_next() % (MAX_SIZE - MIN_SIZE + 1));
}

/* -------------------------------------------------------------------------
 * Timing helpers — nanosecond resolution
 * ------------------------------------------------------------------------- */
static uint64_t now_ns(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

/* Convert nanoseconds to microseconds (float) */
static double ns_to_us(uint64_t ns) { return (double)ns / 1000.0; }

/* -------------------------------------------------------------------------
 * Workload functions
 *
 * Both return elapsed time in nanoseconds.
 *
 * Pattern: batch-allocate N random-sized blocks, write one byte to each
 *          (so the compiler cannot elide the call), then free all.
 *
 * We use a fixed-size pointer array on the heap (ptrs is malloc'd with
 * the SYSTEM malloc regardless of which allocator is under test — it is
 * just bookkeeping, not part of the timed region).
 * ------------------------------------------------------------------------- */

/* --- libmalloc --- */
static uint64_t bench_lm(int n) {
  void **ptrs = malloc((size_t)n * sizeof(void *)); /* bookkeeping only */
  if (!ptrs) {
    perror("malloc ptrs");
    exit(1);
  }

  lcg_seed(SEED); /* deterministic size sequence */

  uint64_t start = now_ns();

  for (int i = 0; i < n; i++) {
    size_t sz = rand_size();
    ptrs[i] = lm_malloc(sz);
    if (ptrs[i]) {
      ((volatile char *)ptrs[i])[0] = 0xAB; /* defeat dead-code elim */
    }
  }
  for (int i = 0; i < n; i++) {
    lm_free(ptrs[i]);
  }

  uint64_t elapsed = now_ns() - start;
  free(ptrs);
  return elapsed;
}

/* --- glibc malloc --- */
static uint64_t bench_glibc(int n) {
  void **ptrs = malloc((size_t)n * sizeof(void *));
  if (!ptrs) {
    perror("malloc ptrs");
    exit(1);
  }

  lcg_seed(SEED); /* same seed → same sizes as libmalloc */

  uint64_t start = now_ns();

  for (int i = 0; i < n; i++) {
    size_t sz = rand_size();
    ptrs[i] = malloc(sz);
    if (ptrs[i]) {
      ((volatile char *)ptrs[i])[0] = 0xAB;
    }
  }
  for (int i = 0; i < n; i++) {
    free(ptrs[i]);
  }

  uint64_t elapsed = now_ns() - start;
  free(ptrs);
  return elapsed;
}

/* -------------------------------------------------------------------------
 * Run one workload N times, return the MINIMUM elapsed nanoseconds.
 * (Minimum, not average, because outliers are caused by scheduler preemption;
 *  the minimum reflects best-achievable hardware throughput.)
 * ------------------------------------------------------------------------- */
static uint64_t run_min(uint64_t (*bench_fn)(int), int n) {
  uint64_t best = UINT64_MAX;
  for (int r = 0; r < RUNS; r++) {
    uint64_t t = bench_fn(n);
    if (t < best)
      best = t;
  }
  return best;
}

/* -------------------------------------------------------------------------
 * Pretty-print helpers
 * ------------------------------------------------------------------------- */
static void print_separator(void) {
  printf("+%-10s+%-20s+%-20s+%-10s+\n", "----------", "--------------------",
         "--------------------", "----------");
}

static void print_header(void) {
  print_separator();
  printf("| %-8s | %-18s | %-18s | %-8s |\n", "Ops",
#ifdef BEST_FIT
         "lm (best-fit) µs",
#else
         "lm (first-fit) µs",
#endif
         "glibc malloc µs", "Ratio");
  print_separator();
}

/* =========================================================================
 * main
 * ========================================================================= */
int main(void) {
  printf("\n");
  printf("=============================================================\n");
  printf("  libmalloc Benchmark — lm_malloc vs glibc malloc\n");
  printf("  Strategy : %s\n",
#ifdef BEST_FIT
         "BEST-FIT + BOUNDARY TAGS"
#else
         "FIRST-FIT + BOUNDARY TAGS"
#endif
  );
  printf("  Timing   : clock_gettime(CLOCK_MONOTONIC), %d runs/min\n", RUNS);
  printf("  Sizes    : %d–%d bytes, pseudo-random (LCG seed=0x%X)\n", MIN_SIZE,
         MAX_SIZE, SEED);
  printf("=============================================================\n");

  printf("\n");
  print_header();

  for (int w = 0; w < NUM_WORKLOADS; w++) {
    int n = WORKLOADS[w];

    uint64_t lm_ns = run_min(bench_lm, n);
    uint64_t glibc_ns = run_min(bench_glibc, n);

    double lm_us = ns_to_us(lm_ns);
    double glibc_us = ns_to_us(glibc_ns);
    double ratio = lm_us / glibc_us; /* >1.0 means lm is slower */

    printf("| %-8d | %-18.1f | %-18.1f | %-8.2fx|\n", n, lm_us, glibc_us,
           ratio);
  }

  print_separator();

  printf("\n");
  printf("Ratio < 1.0  → libmalloc is faster than glibc for this workload\n");
  printf("Ratio > 1.0  → glibc is faster (expected — glibc is highly "
         "optimised)\n");
  printf("\n");
  printf(
      "NOTE: Fill in your ACTUAL numbers above in README.md for the resume.\n");
  printf("      Do NOT copy numbers from this source file.\n");
  printf("\n");

  return 0;
}
