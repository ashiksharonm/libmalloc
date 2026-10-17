/*
 * allocator.h - public API of libmalloc.
 *
 * All block layout details live in allocator.c; callers only see the lm_*
 * functions and a few introspection helpers.
 */
#ifndef ALLOCATOR_H
#define ALLOCATOR_H

#include <stddef.h>

/* Every payload is aligned to this many bytes. */
#define ALIGNMENT 16

void *lm_malloc(size_t size);
void lm_free(void *ptr);
void *lm_realloc(void *ptr, size_t new_size);
void *lm_calloc(size_t nmemb, size_t size);

/* Returns 0 on success, or EINVAL / ENOMEM (errno is left untouched). */
int lm_posix_memalign(void **out, size_t alignment, size_t size);
void *lm_aligned_alloc(size_t alignment, size_t size);

/* Number of bytes the caller may use at ptr (>= the size requested). */
size_t lm_usable_size(void *ptr);

/* Non-zero if ptr is a live payload pointer returned by this allocator
 * (a lookup over the chunks and large mappings, not a fast path). */
int lm_owns(const void *ptr);

/* Bytes currently obtained from the OS, and the high-water mark. */
size_t lm_heap_bytes(void);
size_t lm_peak_heap_bytes(void);

typedef struct {
  size_t chunks;       /* arena chunks obtained from the OS              */
  size_t large_blocks; /* live dedicated mappings for big allocations    */
  size_t used_blocks;
  size_t free_blocks;
  size_t used_bytes; /* block bytes incl. headers, arena chunks only   */
  size_t free_bytes;
  size_t mapped_bytes; /* == lm_heap_bytes()                             */
} lm_stats_t;

void lm_get_stats(lm_stats_t *out);
void lm_stats(void);     /* print a summary to stdout */
void lm_heap_dump(void); /* print every arena block to stdout */

/* Write a JSON snapshot (bin occupancy, size histogram, per-chunk occupancy
 * map, fragmentation figures) to fd / to a file.  Allocation-free, so it is
 * safe behind the malloc shim.  "%p" in the path expands to the pid.
 * The shim calls lm_dump_json() at exit when LM_HEAP_JSON=<path> is set. */
int lm_dump_json_fd(int fd);
int lm_dump_json(const char *path);

/* Walk the whole heap and verify every invariant (boundary tags, flags,
 * free-list links, no adjacent free blocks).  Returns 0 if consistent;
 * otherwise prints the first violation to stderr and returns non-zero. */
int lm_check(void);

#endif /* ALLOCATOR_H */
