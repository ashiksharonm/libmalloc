/*
 * allocator.h — Public API and internal block structure for libmalloc.
 *
 * Phase 2 upgrade: adds a BOUNDARY TAG (footer) at the end of each block.
 *
 * === Why Boundary Tags? ===
 * In Phase 1, we coalesced using the `prev` pointer in the header — which works
 * because we track the logical list.  But the `prev` pointer points to the list
 * predecessor, not necessarily the PHYSICALLY adjacent block in memory.
 *
 * With boundary tags (Knuth, 1973), we place a small footer at the END of every
 * block.  The footer mirrors the header's `size` and `is_free` fields.  Now, to
 * find the block that physically precedes us in memory, we just peek at the
 * footer sitting one word below our own header — O(1), no traversal.
 *
 * === Updated Memory Layout ===
 *
 *   ┌──────────────────────────────────────┐
 *   │  block_header_t  (HEADER_SIZE bytes) │  ← header at low address
 *   │   .size, .is_free, .magic, .next/.prev│
 *   ├──────────────────────────────────────┤
 *   │  user data  (size bytes)             │  ← pointer returned to caller
 *   ├──────────────────────────────────────┤
 *   │  block_footer_t  (FOOTER_SIZE bytes) │  ← footer at high address
 *   │   .size (must == header.size)        │
 *   │   .is_free (must == header.is_free)  │
 *   └──────────────────────────────────────┘
 *
 * Total block in memory = HEADER_SIZE + size + FOOTER_SIZE
 */

#ifndef ALLOCATOR_H
#define ALLOCATOR_H

#include <stddef.h> /* size_t  */
#include <stdint.h> /* uint32_t */

/* -------------------------------------------------------------------------
 * Magic sentinels
 * Written into both header and footer to detect corruption and double-free.
 * ------------------------------------------------------------------------- */
#define MAGIC_FREE 0xDEADBEEF
#define MAGIC_USED 0xCAFEBABE

/* -------------------------------------------------------------------------
 * block_header_t — sits at the START of every allocated block
 * ------------------------------------------------------------------------- */
typedef struct block_header {
  size_t size;               /* usable bytes between header and footer   */
  int is_free;               /* 1 = free, 0 = in use                     */
  uint32_t magic;            /* MAGIC_FREE or MAGIC_USED                 */
  struct block_header *next; /* next block in chain (explicit free list) */
  struct block_header *prev; /* previous block in chain                  */
} block_header_t;

_Static_assert(sizeof(block_header_t) % 16 == 0,
               "header size must keep payloads 16-byte aligned");

/* -------------------------------------------------------------------------
 * block_footer_t — sits at the END of every allocated block (boundary tag)
 * The footer makes it possible to find the preceding physical block in O(1).
 * ------------------------------------------------------------------------- */
typedef struct {
  size_t size; /* must always equal the header's .size  */
  int is_free; /* must always equal the header's is_free */
} block_footer_t;

/* Payload alignment guaranteed by lm_malloc (matches max_align_t on x86-64
 * and arm64). */
#define ALIGNMENT 16

/* Sizes */
#define HEADER_SIZE (sizeof(block_header_t))
#define FOOTER_SIZE (sizeof(block_footer_t))

/* Minimum usable block size (avoids splitting into unusably tiny pieces).
 * Must accommodate at least a block_footer_t worth of payload. */
#define MIN_BLOCK_SIZE (FOOTER_SIZE + 8)

/* -------------------------------------------------------------------------
 * Public API
 * ------------------------------------------------------------------------- */
void *lm_malloc(size_t size);
void lm_free(void *ptr);
void *lm_realloc(void *ptr, size_t new_size);
void *lm_calloc(size_t nmemb, size_t size);

/* -------------------------------------------------------------------------
 * Debug helpers (utils.c)
 * ------------------------------------------------------------------------- */
void lm_heap_dump(void);
void lm_stats(void);

#endif /* ALLOCATOR_H */
