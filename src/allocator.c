/*
 * allocator.c — Core implementation of libmalloc, Phase 2.
 *
 * Phase 2 additions over Phase 1:
 *   1. BOUNDARY TAGS: a block_footer_t is written at the END of every block.
 *      This makes it possible to find the physically adjacent PREVIOUS block
 *      in O(1) without a list traversal — see coalesce() for details.
 *
 *   2. BEST-FIT STRATEGY: compile with -DBEST_FIT to switch the search from
 *      first-fit to best-fit.  Best-fit finds the smallest block that fits the
 *      request, reducing wasted space at the cost of scanning the entire list.
 *
 *      Compile-time switch (default = first-fit):
 *        First-fit:  gcc -o ... allocator.c
 *        Best-fit:   gcc -DBEST_FIT -o ... allocator.c
 *
 *   3. BLOCK SPLITTING: carried over from Phase 1, now updated to write a
 *      footer on both the shrunken block AND the new split-off block.
 *
 * Updated layout (per block):
 *
 *   +--------------------------------------+  <- block_header_t (HEADER_SIZE)
 *   | .size  .is_free  .magic  .next .prev |
 *   +--------------------------------------+
 *   |  user data  (size bytes)             |  <- ptr returned to caller
 *   +--------------------------------------+  <- block_footer_t (FOOTER_SIZE)
 *   | .size   .is_free                     |
 *   +--------------------------------------+
 *
 *   Total bytes consumed on heap = HEADER_SIZE + size + FOOTER_SIZE
 */

#include "allocator.h"
#include <stdio.h>  /* fprintf()          */
#include <string.h> /* memset(), memcpy() */
#include <unistd.h> /* sbrk()             */

/* -------------------------------------------------------------------------
 * Global state — visible to utils.c via extern
 * ------------------------------------------------------------------------- */
block_header_t *heap_start = NULL;

/* =========================================================================
 * Footer helpers
 * ========================================================================= */

/* footer_of — the footer sits right after the user-data payload */
static inline block_footer_t *footer_of(block_header_t *block) {
  return (block_footer_t *)((char *)block + HEADER_SIZE + block->size);
}

/* prev_phys_footer — the footer of the physically preceding block,
 * which sits immediately before our header in memory.
 * ONLY call this after verifying physical adjacency (see coalesce()). */
static inline block_footer_t *prev_phys_footer(block_header_t *block) {
  return (block_footer_t *)((char *)block - FOOTER_SIZE);
}

/* write_footer — keep the footer in sync with the header.
 * Must be called whenever .size or .is_free changes. */
static void write_footer(block_header_t *block) {
  block_footer_t *footer = footer_of(block);
  footer->size = block->size;
  footer->is_free = block->is_free;
}

/* -------------------------------------------------------------------------
 * align_size — round up to the nearest multiple of sizeof(void*)
 * Also ensure the size is at least FOOTER_SIZE (so a footer always fits).
 * ------------------------------------------------------------------------- */
static size_t align_size(size_t size) {
  size_t alignment = sizeof(void *);
  size = (size + alignment - 1) & ~(alignment - 1);
  if (size < FOOTER_SIZE)
    size = FOOTER_SIZE;
  return size;
}

/* -------------------------------------------------------------------------
 * ptr_to_header / header_to_ptr
 * ------------------------------------------------------------------------- */
static inline block_header_t *ptr_to_header(void *ptr) {
  return (block_header_t *)((char *)ptr - HEADER_SIZE);
}

static inline void *header_to_ptr(block_header_t *block) {
  return (char *)block + HEADER_SIZE;
}

/* =========================================================================
 * Search strategies
 * ========================================================================= */

/* First-fit: return first free block large enough */
static block_header_t *find_first_fit(size_t size) {
  block_header_t *cur = heap_start;
  while (cur != NULL) {
    if (cur->is_free && cur->size >= size)
      return cur;
    cur = cur->next;
  }
  return NULL;
}

/* Best-fit: return smallest free block that still fits.
 * Scans entire list — O(n) always, but minimises wasted payload. */
static block_header_t *find_best_fit(size_t size) {
  block_header_t *cur = heap_start;
  block_header_t *best = NULL;
  while (cur != NULL) {
    if (cur->is_free && cur->size >= size) {
      if (best == NULL || cur->size < best->size) {
        best = cur;
        if (best->size == size)
          break; /* perfect fit */
      }
    }
    cur = cur->next;
  }
  return best;
}

static block_header_t *find_free_block(size_t size) {
#ifdef BEST_FIT
  return find_best_fit(size);
#else
  return find_first_fit(size);
#endif
}

/* =========================================================================
 * split_block — carve a smaller allocation out of a larger free block
 *
 * Phase 2: both halves get fresh footers after the split.
 *
 * Size accounting:
 *   block->size is the user-data payload of the ORIGINAL block (including the
 *   space where our footer lives, since the footer is "inside" size).
 *
 *   After splitting at `size` bytes:
 *     - First half:   header | size bytes of data | FOOTER_SIZE bytes
 * (footer_a)
 *     - Gap:          HEADER_SIZE bytes for the new header (new_block)
 *     - Second half:  new_block payload | FOOTER_SIZE bytes (footer_b)
 *
 *   leftover = block->size - size - FOOTER_SIZE - HEADER_SIZE
 *
 * UNSIGNED UNDERFLOW GUARD:
 *   size_t is unsigned.  If block->size - size < FOOTER_SIZE + HEADER_SIZE the
 *   subtraction wraps to a huge number, bypassing the MIN_BLOCK_SIZE check and
 *   writing new_block at a wildly wrong address.  We guard by checking in the
 *   additive direction first.
 * ========================================================================= */
static void split_block(block_header_t *block, size_t size) {
  /*
   * Minimum total block->size needed to make splitting worthwhile:
   *   size         - payload for the first half
   *   FOOTER_SIZE  - footer for the first half
   *   HEADER_SIZE  - header for the new second block
   *   MIN_BLOCK_SIZE - minimum usable payload for the second block
   *
   * Computed with addition only (no risk of underflow).
   */
  size_t min_to_split = size + FOOTER_SIZE + HEADER_SIZE + MIN_BLOCK_SIZE;

  if (block->size < min_to_split) {
    /* Not enough room — hand the entire block to the caller.
     * Slight internal fragmentation, but avoids tiny useless blocks. */
    write_footer(block);
    return;
  }

  /* Guaranteed safe: block->size >= min_to_split > size + FOOTER_SIZE +
   * HEADER_SIZE */
  size_t leftover = block->size - size - FOOTER_SIZE - HEADER_SIZE;

  /* Shrink the original block */
  block->size = size;
  write_footer(block); /* footer_a immediately after `size` bytes of data */

  /* Place new_block header immediately after footer_a */
  block_header_t *new_block =
      (block_header_t *)((char *)block + HEADER_SIZE + size + FOOTER_SIZE);
  new_block->size = leftover;
  new_block->is_free = 1;
  new_block->magic = MAGIC_FREE;
  new_block->next = block->next;
  new_block->prev = block;
  write_footer(new_block); /* footer_b */

  if (block->next != NULL) {
    block->next->prev = new_block;
  }
  block->next = new_block;
}

/* =========================================================================
 * request_space — extend the heap via sbrk() and append a new block
 * ========================================================================= */
static block_header_t *request_space(block_header_t *last, size_t size) {
  block_header_t *block = (block_header_t *)sbrk(0);

  void *request = sbrk((intptr_t)(HEADER_SIZE + size + FOOTER_SIZE));
  if (request == (void *)-1)
    return NULL;

  block->size = size;
  block->is_free = 0;
  block->magic = MAGIC_USED;
  block->next = NULL;
  block->prev = last;
  write_footer(block);

  if (last != NULL)
    last->next = block;
  return block;
}

/* =========================================================================
 * coalesce — merge `block` with its physically adjacent free neighbours
 *
 * Boundary tag technique: the footer of the preceding physical block sits
 * immediately before our header, giving O(1) access to its size and is_free.
 *
 * PHYSICAL ADJACENCY GUARD:
 *   This allocator shares the process heap with glibc malloc (used in bench.c
 *   for bookkeeping arrays).  glibc also calls sbrk(), so after bench_glibc()
 *   runs, the break advances past our tail.  The next request_space() gets a
 *   block NOT physically adjacent to our previous tail.
 *
 *   Guard: only trust the boundary tag if block->prev ends exactly where
 *   `block` begins.  If there's a gap (another allocator moved sbrk between
 *   our blocks), skip backward coalescing.
 *
 * Four textbook cases:
 *   Case 1: neither neighbour is free  → do nothing
 *   Case 2: only next is free          → absorb next into block
 *   Case 3: only prev is free          → absorb block into prev
 *   Case 4: both are free              → three-way merge into prev
 * ========================================================================= */
static block_header_t *coalesce(block_header_t *block) {

  /* ----- Back neighbour ----- */
  block_footer_t *prev_footer = NULL;
  block_header_t *prev_block = NULL;
  int prev_is_free = 0;

  if (block->prev != NULL) {
    block_header_t *candidate = block->prev;
    /*
     * Are `candidate` and `block` physically contiguous?
     * candidate occupies: [header | size bytes | footer]
     * Expected start of next block: (char*)candidate + HEADER_SIZE
     *                               + candidate->size + FOOTER_SIZE
     */
    char *expected_next =
        (char *)candidate + HEADER_SIZE + candidate->size + FOOTER_SIZE;
    if (expected_next == (char *)block) {
      prev_footer = prev_phys_footer(block);
      prev_block = candidate;
      prev_is_free = prev_footer->is_free;
    }
    /* else: non-contiguous gap — skip backward merge safely */
  }

  /* ----- Forward neighbour ----- */
  block_header_t *next_block = block->next;
  int next_is_free = (next_block != NULL && next_block->is_free);

  /* Case 1 */
  if (!prev_is_free && !next_is_free)
    return block;

  /* Case 2: absorb next into block */
  if (!prev_is_free && next_is_free) {
    block->size += FOOTER_SIZE + HEADER_SIZE + next_block->size;
    block->next = next_block->next;
    if (next_block->next != NULL)
      next_block->next->prev = block;
    write_footer(block);
    return block;
  }

  /* Case 3: absorb block into prev */
  if (prev_is_free && !next_is_free) {
    prev_block->size += FOOTER_SIZE + HEADER_SIZE + block->size;
    prev_block->next = block->next;
    if (block->next != NULL)
      block->next->prev = prev_block;
    write_footer(prev_block);
    return prev_block;
  }

  /* Case 4: three-way merge into prev */
  prev_block->size += (FOOTER_SIZE + HEADER_SIZE + block->size) +
                      (FOOTER_SIZE + HEADER_SIZE + next_block->size);
  prev_block->next = next_block->next;
  if (next_block->next != NULL)
    next_block->next->prev = prev_block;
  write_footer(prev_block);
  return prev_block;
}

/* =========================================================================
 * PUBLIC API
 * ========================================================================= */

void *lm_malloc(size_t size) {
  if (size == 0)
    return NULL;
  size = align_size(size);

  block_header_t *block;

  if (heap_start == NULL) {
    block = request_space(NULL, size);
    if (block == NULL)
      return NULL;
    heap_start = block;
  } else {
    block_header_t *last = heap_start;
    block = find_free_block(size);

    if (block == NULL) {
      while (last->next != NULL)
        last = last->next;
      block = request_space(last, size);
      if (block == NULL)
        return NULL;
    } else {
      split_block(block, size);
      block->is_free = 0;
      block->magic = MAGIC_USED;
      write_footer(block);
    }
  }

  return header_to_ptr(block);
}

void lm_free(void *ptr) {
  if (ptr == NULL)
    return;

  block_header_t *block = ptr_to_header(ptr);

  if (block->magic == MAGIC_FREE) {
    fprintf(stderr, "[libmalloc] ERROR: double-free at %p\n", ptr);
    __builtin_trap();
  }
  if (block->magic != MAGIC_USED) {
    fprintf(stderr, "[libmalloc] ERROR: invalid free at %p (magic=0x%08X)\n",
            ptr, block->magic);
    __builtin_trap();
  }

  block->is_free = 1;
  block->magic = MAGIC_FREE;
  write_footer(block);

  coalesce(block);
}

void *lm_calloc(size_t nmemb, size_t size) {
  if (nmemb == 0 || size == 0)
    return NULL;
  if (nmemb > (size_t)-1 / size)
    return NULL;

  size_t total = nmemb * size;
  void *ptr = lm_malloc(total);
  if (ptr != NULL)
    memset(ptr, 0, total);
  return ptr;
}

void *lm_realloc(void *ptr, size_t new_size) {
  if (ptr == NULL)
    return lm_malloc(new_size);
  if (new_size == 0) {
    lm_free(ptr);
    return NULL;
  }

  block_header_t *block = ptr_to_header(ptr);
  if (block->magic != MAGIC_USED) {
    fprintf(stderr, "[libmalloc] ERROR: realloc on invalid pointer %p\n", ptr);
    return NULL;
  }

  new_size = align_size(new_size);

  if (new_size <= block->size) {
    split_block(block, new_size);
    return ptr;
  }

  void *new_ptr = lm_malloc(new_size);
  if (new_ptr == NULL)
    return NULL;

  memcpy(new_ptr, ptr, block->size);
  lm_free(ptr);
  return new_ptr;
}
