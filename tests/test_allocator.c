/*
 * test_allocator.c — Phase 4 comprehensive test suite for libmalloc.
 *
 * Tests:
 *   --- Basic correctness (Phase 1 carried forward) ---
 *   1.  Basic int allocation
 *   2.  String allocation and strcpy
 *   3.  Struct allocation
 *   4.  lm_calloc — zero initialisation
 *   5.  lm_realloc — grow a buffer
 *   6.  Free then reallocate — block reuse verified
 *   7.  Multi-allocation — no overlap, no corruption
 *   8.  lm_malloc(0) returns NULL
 *   9.  lm_free(NULL) is a no-op
 *  10.  Coalescing of adjacent free blocks
 *
 *   --- Phase 4: edge cases ---
 *  11.  Very large single allocation (1 MB)
 *  12.  Realloc to zero behaves like free
 *  13.  Realloc NULL behaves like malloc
 *  14.  Calloc overflow guard (nmemb * size would overflow)
 *  15.  Double-free detection — DELIBERATELY CRASHES the process with a
 *       clear error message.  Run separately with:
 *         ./build/test_allocator double_free
 *  16.  Stress test — 10,000 random alloc/free cycles, verify no corruption
 *
 * Compile:
 *   make tests
 * Run (all tests except double-free):
 *   ./build/test_allocator
 * Run double-free test (should print error + abort):
 *   ./build/test_allocator double_free
 * Run under Valgrind (Linux):
 *   valgrind --leak-check=full --error-exitcode=1 ./build/test_allocator
 * Run under AddressSanitizer (macOS M2 / Linux):
 *   make asan
 */

#include "../src/allocator.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* -------------------------------------------------------------------------
 * Minimal test framework
 * ------------------------------------------------------------------------- */
static int tests_run = 0;
static int tests_passed = 0;

#define RUN_TEST(name, fn)                                                     \
  do {                                                                         \
    printf("  [TEST %2d] %-50s", ++tests_run, name);                           \
    fn();                                                                      \
    printf("PASS\n");                                                          \
    tests_passed++;                                                            \
  } while (0)

/* -------------------------------------------------------------------------
 * LCG for the stress test — deterministic random sizes / order
 * ------------------------------------------------------------------------- */
static uint32_t lcg_state = 0xCAFE1234;

static uint32_t lcg_next(void) {
  lcg_state = lcg_state * 1664525u + 1013904223u;
  return lcg_state;
}

/* =========================================================================
 * Phase 1 tests (carried forward)
 * ========================================================================= */

typedef struct {
  int id;
  float score;
  char label[32];
} record_t;

static void test_alloc_int(void) {
  int *p = lm_malloc(sizeof(int));
  assert(p != NULL);
  *p = 42;
  assert(*p == 42);
  lm_free(p);
}

static void test_alloc_string(void) {
  const char *src = "Hello, libmalloc!";
  char *buf = lm_malloc(strlen(src) + 1);
  assert(buf != NULL);
  strcpy(buf, src);
  assert(strcmp(buf, src) == 0);
  lm_free(buf);
}

static void test_alloc_struct(void) {
  record_t *r = lm_malloc(sizeof(record_t));
  assert(r != NULL);
  r->id = 7;
  r->score = 99.5f;
  strcpy(r->label, "systems");
  assert(r->id == 7 && r->score == 99.5f && strcmp(r->label, "systems") == 0);
  lm_free(r);
}

static void test_calloc_zero(void) {
  int *arr = lm_calloc(10, sizeof(int));
  assert(arr != NULL);
  for (int i = 0; i < 10; i++)
    assert(arr[i] == 0);
  lm_free(arr);
}

static void test_realloc_grow(void) {
  char *buf = lm_malloc(16);
  assert(buf != NULL);
  strcpy(buf, "Intel");
  buf = lm_realloc(buf, 64);
  assert(buf != NULL);
  assert(strncmp(buf, "Intel", 5) == 0);
  lm_free(buf);
}

static void test_free_reuse(void) {
  void *p1 = lm_malloc(64);
  assert(p1 != NULL);
  lm_free(p1);
  void *p2 = lm_malloc(64);
  assert(p2 != NULL);
  assert(p2 == p1); /* first-fit must reuse the freed block */
  lm_free(p2);
}

static void test_no_overlap(void) {
  const int N = 20;
  int *ptrs[20];
  for (int i = 0; i < N; i++) {
    ptrs[i] = lm_malloc(sizeof(int));
    assert(ptrs[i] != NULL);
    *ptrs[i] = i * 100;
  }
  for (int i = 0; i < N; i++)
    assert(*ptrs[i] == i * 100);
  for (int i = N - 1; i >= 0; i--)
    lm_free(ptrs[i]);
}

static void test_alignment(void) {
  size_t sizes[] = {1, 3, 8, 17, 24, 100, 1000, 4097};
  void *ptrs[sizeof(sizes) / sizeof(sizes[0])];
  for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
    ptrs[i] = lm_malloc(sizes[i]);
    assert(ptrs[i] != NULL);
    assert(((uintptr_t)ptrs[i] % 16) == 0);
  }
  for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++)
    lm_free(ptrs[i]);
}

static void test_malloc_zero(void) { assert(lm_malloc(0) == NULL); }

static void test_free_null(void) { lm_free(NULL); /* must not crash */ }

static void test_coalescing(void) {
  void *a = lm_malloc(32);
  void *b = lm_malloc(32);
  void *c = lm_malloc(32);
  assert(a && b && c);
  lm_free(b);
  lm_free(a);
  lm_free(c);
  void *combined = lm_malloc(90);
  assert(combined != NULL);
  assert(combined == a); /* merged region reused from start */
  lm_free(combined);
}

/* =========================================================================
 * Phase 4 edge-case tests
 * ========================================================================= */

/* Test 11 — very large allocation (1 MB) */
static void test_large_alloc(void) {
  size_t one_mb = 1024 * 1024;
  char *buf = lm_malloc(one_mb);
  assert(buf != NULL);

  /* Write to first and last byte to confirm the whole range is mapped */
  buf[0] = 0xAA;
  buf[one_mb - 1] = 0xBB;
  assert((unsigned char)buf[0] == 0xAA);
  assert((unsigned char)buf[one_mb - 1] == 0xBB);

  lm_free(buf);
}

/* Test 12 — realloc(ptr, 0) behaves like free and returns NULL */
static void test_realloc_to_zero(void) {
  void *p = lm_malloc(64);
  assert(p != NULL);
  void *result = lm_realloc(p, 0);
  assert(result == NULL); /* block must be freed, NULL returned */
}

/* Test 13 — realloc(NULL, size) behaves like malloc */
static void test_realloc_null(void) {
  int *p = lm_realloc(NULL, sizeof(int));
  assert(p != NULL);
  *p = 99;
  assert(*p == 99);
  lm_free(p);
}

/* Test 14 — calloc overflow guard: nmemb * size wraps around */
static void test_calloc_overflow(void) {
  /*
   * SIZE_MAX / 2 + 1 multiplied by 2 would overflow a size_t.
   * Our calloc must detect this and return NULL, not allocate garbage.
   */
  void *p = lm_calloc((size_t)-1 / 2 + 2, 2);
  assert(p == NULL); /* overflow guard triggered */
}

/* Test 15 — double-free detection
 *
 * This test INTENTIONALLY triggers __builtin_trap() which aborts the process.
 * It validates that freeing the same pointer twice prints an error and dies
 * loudly instead of silently corrupting the heap.
 *
 * Run with:  ./build/test_allocator double_free
 * Expected output:
 *   [libmalloc] ERROR: double-free detected at 0x...
 *   <process aborts>
 */
static void run_double_free_test(void) {
  printf("\n  [DOUBLE-FREE TEST] Allocating and freeing twice...\n");
  printf("  (Process should print error message and abort.)\n\n");
  void *p = lm_malloc(64);
  lm_free(p);
  lm_free(p); /* <-- should trap here */
  /* Never reached */
  printf("  FAIL: double-free was not caught!\n");
}

/* Test 16 — stress test: 10,000 mixed alloc/free cycles
 *
 * Pattern: keep a sliding window of live allocations.  At each step,
 * randomly decide to allocate or free.  Write a canary word into each
 * allocation and verify it on free.  Any heap corruption will either
 * trigger an assert or be caught by AddressSanitizer / Valgrind.
 */
#define STRESS_OPS 10000
#define STRESS_LIVE 64 /* max live allocations at once */
#define CANARY 0xDEADC0DEULL

typedef struct {
  void *ptr;
  uint64_t canary;
  size_t size;
} slot_t;

static void test_stress(void) {
  slot_t slots[STRESS_LIVE];
  int live[STRESS_LIVE];

  memset(slots, 0, sizeof(slots));
  memset(live, 0, sizeof(live));

  int live_count = 0;
  int total_allocs = 0;
  int total_frees = 0;

  for (int op = 0; op < STRESS_OPS; op++) {
    uint32_t rng = lcg_next();

    /*
     * Decide action based on how full the window is:
     *   live_count < 8          → always allocate (too empty)
     *   live_count == STRESS_LIVE → always free (no room)
     *   otherwise               → coin flip
     */
    int do_alloc;
    if (live_count < 8)
      do_alloc = 1;
    else if (live_count == STRESS_LIVE)
      do_alloc = 0;
    else
      do_alloc = (rng & 1);

    if (do_alloc) {
      /* Find first empty slot */
      for (int s = 0; s < STRESS_LIVE; s++) {
        if (!live[s]) {
          size_t sz = 8 + (lcg_next() % 504);
          void *p = lm_malloc(sz);
          assert(p != NULL);
          *(uint64_t *)p = CANARY;
          slots[s].ptr = p;
          slots[s].canary = CANARY;
          slots[s].size = sz;
          live[s] = 1;
          live_count++;
          total_allocs++;
          break;
        }
      }
    } else {
      /* Pick a random live slot to free */
      int start = (int)((lcg_next() % (uint32_t)STRESS_LIVE));
      for (int s = 0; s < STRESS_LIVE; s++) {
        int idx = (start + s) % STRESS_LIVE;
        if (live[idx]) {
          assert(*(uint64_t *)slots[idx].ptr == CANARY);
          lm_free(slots[idx].ptr);
          live[idx] = 0;
          live_count--;
          total_frees++;
          break;
        }
      }
    }
  }

  /* Drain remaining live blocks */
  for (int s = 0; s < STRESS_LIVE; s++) {
    if (live[s]) {
      assert(*(uint64_t *)slots[s].ptr == CANARY);
      lm_free(slots[s].ptr);
      total_frees++;
    }
  }

  /* Must have exercised both paths meaningfully */
  assert(total_allocs > 1000);
  assert(total_frees > 1000);
}

/* =========================================================================
 * main
 * ========================================================================= */
int main(int argc, char *argv[]) {

  /* Special mode: double-free test (run separately, crashes intentionally) */
  if (argc == 2 && strcmp(argv[1], "double_free") == 0) {
    run_double_free_test();
    return 0; /* unreachable — process aborts above */
  }

  printf("\n=================================================\n");
  printf("  libmalloc — Phase 4 Full Test Suite\n");
  printf("=================================================\n\n");

  /* Phase 1 — basic correctness */
  printf("-- Phase 1: Basic Correctness --\n");
  RUN_TEST("alloc int + read/write", test_alloc_int);
  RUN_TEST("alloc string + strcpy", test_alloc_string);
  RUN_TEST("alloc struct + field write", test_alloc_struct);
  RUN_TEST("calloc zero-initialisation", test_calloc_zero);
  RUN_TEST("realloc grow + data preserved", test_realloc_grow);
  RUN_TEST("free then reuse (first-fit)", test_free_reuse);
  RUN_TEST("multi-alloc no overlap", test_no_overlap);
  RUN_TEST("payloads are 16-byte aligned", test_alignment);
  RUN_TEST("malloc(0) returns NULL", test_malloc_zero);
  RUN_TEST("free(NULL) no-op", test_free_null);
  RUN_TEST("coalescing adjacent blocks", test_coalescing);

  printf("\n-- Phase 4: Edge Cases & Stress --\n");
  RUN_TEST("large alloc (1 MB) read/write", test_large_alloc);
  RUN_TEST("realloc(ptr, 0) == free", test_realloc_to_zero);
  RUN_TEST("realloc(NULL, n) == malloc", test_realloc_null);
  RUN_TEST("calloc overflow returns NULL", test_calloc_overflow);
  RUN_TEST("stress: 10,000 alloc/free cycles + canary verify", test_stress);

  printf("\n-------------------------------------------------\n");
  printf("  Results: %d / %d tests passed\n", tests_passed, tests_run);
  printf("-------------------------------------------------\n");

  printf("\n  [SKIPPED] double-free crash test.\n");
  printf("  Run manually:  ./build/test_allocator double_free\n");
  printf("  Expected: error printed, process aborts.\n\n");

  lm_heap_dump();
  lm_stats();

  if (tests_passed == tests_run) {
    printf("ALL TESTS PASSED ✓\n\n");
    return 0;
  } else {
    printf("SOME TESTS FAILED ✗\n\n");
    return 1;
  }
}
