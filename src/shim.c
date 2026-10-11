/*
 * shim.c - drop-in malloc replacement built on libmalloc.
 *
 * Linux:  LD_PRELOAD=build/libmalloc_shim.so ./program
 * macOS:  DYLD_INSERT_LIBRARIES=build/libmalloc_shim.dylib ./program
 *         (uses dyld interposing; SIP-protected system binaries ignore
 *          DYLD_INSERT_LIBRARIES, so this works on your own / Homebrew
 *          binaries only)
 *
 * Set LM_SHIM_REPORT=<file> to append call counts and peak heap to that file at exit,
 * which is how the tests prove the shim was actually in the loop.
 */
#define _DEFAULT_SOURCE
#include "allocator.h"
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifdef __APPLE__
#include <malloc/malloc.h>
#define SHIM(name) shim_##name
#define INTERPOSE(name)                                                        \
  __attribute__((used)) static struct {                                        \
    const void *replacement;                                                   \
    const void *original;                                                      \
  } interpose_##name __attribute__((section("__DATA,__interpose"))) = {        \
      (const void *)shim_##name, (const void *)name}
#else
#define SHIM(name) name
#define INTERPOSE(name) /* symbol interposition via LD_PRELOAD */
#endif

static unsigned long n_malloc, n_free, n_realloc, n_calloc, n_memalign;
#define COUNT(c) __atomic_fetch_add(&(c), 1, __ATOMIC_RELAXED)

#ifdef __APPLE__
/* Pointers handed out by the system allocator before the shim took over
 * (dyld, libSystem initialisers) must still be freed by the system. */
static int foreign(void *p) { return malloc_zone_from_ptr(p) != NULL; }
#else
static int foreign(void *p) {
  (void)p;
  return 0;
}
#endif

void *SHIM(malloc)(size_t n) {
  COUNT(n_malloc);
  return lm_malloc(n ? n : 1); /* malloc(0) must return a unique pointer */
}
INTERPOSE(malloc);

void SHIM(free)(void *p) {
  if (!p)
    return;
  COUNT(n_free);
  if (foreign(p))
    free(p);
  else
    lm_free(p);
}
INTERPOSE(free);

void *SHIM(calloc)(size_t n, size_t sz) {
  COUNT(n_calloc);
  if (n == 0 || sz == 0)
    return lm_malloc(1);
  return lm_calloc(n, sz);
}
INTERPOSE(calloc);

void *SHIM(realloc)(void *p, size_t n) {
  COUNT(n_realloc);
#ifdef __APPLE__
  if (p && foreign(p)) {
    if (n == 0) {
      free(p);
      return NULL;
    }
    size_t old = malloc_size(p);
    void *q = lm_malloc(n);
    if (q) {
      memcpy(q, p, old < n ? old : n);
      free(p);
    }
    return q;
  }
#endif
  return lm_realloc(p, n);
}
INTERPOSE(realloc);

#ifdef __APPLE__
/* The Objective-C runtime and others ask malloc_size() about every block. */
size_t shim_malloc_size(const void *p) {
  if (foreign((void *)p))
    return malloc_size(p);
  return lm_owns(p) ? lm_usable_size((void *)p) : 0;
}
INTERPOSE(malloc_size);
#endif

int SHIM(posix_memalign)(void **out, size_t align, size_t n) {
  COUNT(n_memalign);
  return lm_posix_memalign(out, align, n);
}
INTERPOSE(posix_memalign);

void *SHIM(aligned_alloc)(size_t align, size_t n) {
  COUNT(n_memalign);
  return lm_aligned_alloc(align, n);
}
INTERPOSE(aligned_alloc);

#ifdef __linux__
void *memalign(size_t align, size_t n) {
  COUNT(n_memalign);
  return lm_aligned_alloc(align, n);
}

void *valloc(size_t n) {
  COUNT(n_memalign);
  return lm_aligned_alloc((size_t)sysconf(_SC_PAGESIZE), n);
}

void *pvalloc(size_t n) {
  size_t ps = (size_t)sysconf(_SC_PAGESIZE);
  COUNT(n_memalign);
  return lm_aligned_alloc(ps, (n + ps - 1) & ~(ps - 1));
}

size_t malloc_usable_size(void *p) { return lm_usable_size(p); }
#endif

__attribute__((destructor)) static void report(void) {
  /* Appends to the file named by LM_SHIM_REPORT.  stderr is not used because
   * coreutils closes it in an atexit handler that runs before destructors. */
  const char *path = getenv("LM_SHIM_REPORT");
  if (!path || !*path)
    return;
  int fd = open(path, O_WRONLY | O_APPEND | O_CREAT, 0644);
  if (fd < 0)
    return;
  char buf[200];
  int n = snprintf(buf, sizeof buf,
                   "[libmalloc-shim] malloc=%lu calloc=%lu realloc=%lu free=%lu "
                   "memalign=%lu peak_heap_bytes=%zu\n",
                   n_malloc, n_calloc, n_realloc, n_free, n_memalign,
                   lm_peak_heap_bytes());
  if (n > 0 && write(fd, buf, (size_t)n) < 0)
    return;
  close(fd);
}
