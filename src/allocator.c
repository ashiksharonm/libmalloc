/*
 * allocator.c - libmalloc core.
 *
 * Heap layout
 * -----------
 * Memory comes from the OS in chunks (mmap by default; sbrk is available on
 * Linux with -DLM_USE_SBRK).  A chunk is:
 *
 *   [chunk_t: 16B][ block ... block ][ epilogue: 16B ]
 *
 * The first block has PREV_USED set and the epilogue is a zero-sized USED
 * block, so coalescing never walks off either end of a chunk.
 *
 * Block layout (dlmalloc-style boundary tags, 8 bytes of overhead):
 *
 *   b+0   prev_size   size of the physically previous block, valid only
 *                     while that block is free (it is that block's footer);
 *                     otherwise it is the tail of the previous payload
 *   b+8   size|flags  block size incl. header (multiple of 16), low bits:
 *                     USED, PREV_USED, MMAPPED
 *   b+16  payload     16-byte aligned; free blocks keep next/prev here
 *
 * Allocations >= MMAP_THRESHOLD get a dedicated mapping that is returned to
 * the OS on free.
 */
#define _DEFAULT_SOURCE
#include "allocator.h"
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#ifndef MAP_ANONYMOUS
#define MAP_ANONYMOUS MAP_ANON
#endif

#define USED 1u
#define PREV_USED 2u
#define MMAPPED 4u
#define FLAG_MASK 15u

#define MIN_BLOCK 32u
#define CHUNK_BYTES ((size_t)1 << 20)
#define MMAP_THRESHOLD ((size_t)128 << 10)

typedef struct block {
  size_t prev_size;
  size_t size;
  struct block *next; /* free blocks only */
  struct block *prev;
} block_t;

typedef struct chunk {
  struct chunk *next;
  size_t len;
} chunk_t;

static chunk_t *g_chunks;
static block_t *g_free_head;
static size_t g_mapped, g_peak_mapped, g_large_blocks;

/* ------------------------------------------------------------------ */
/* Block helpers                                                       */
/* ------------------------------------------------------------------ */

static inline size_t bsize(const block_t *b) { return b->size & ~(size_t)FLAG_MASK; }
static inline block_t *next_phys(block_t *b) { return (block_t *)((char *)b + bsize(b)); }
static inline block_t *prev_phys(block_t *b) { return (block_t *)((char *)b - b->prev_size); }
static inline void *payload(block_t *b) { return (char *)b + 16; }
static inline block_t *hdr(void *p) { return (block_t *)((char *)p - 16); }
static inline size_t usable(const block_t *b) {
  return bsize(b) - ((b->size & MMAPPED) ? 16 : 8);
}

/* Block size needed to serve n payload bytes, or 0 on overflow. */
static size_t req_to_bsize(size_t n) {
  if (n > SIZE_MAX / 2)
    return 0;
  size_t s = (n + 8 + 15) & ~(size_t)15;
  return s < MIN_BLOCK ? MIN_BLOCK : s;
}

static size_t page_size(void) {
  static size_t ps;
  if (!ps)
    ps = (size_t)sysconf(_SC_PAGESIZE);
  return ps;
}

static void fatal(const char *what, void *p) {
  fprintf(stderr, "[libmalloc] ERROR: %s at %p\n", what, p);
  __builtin_trap();
}

/* ------------------------------------------------------------------ */
/* OS interface                                                        */
/* ------------------------------------------------------------------ */

static void *os_mmap(size_t len) {
  void *p = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  return p == MAP_FAILED ? NULL : p;
}

/* Arena chunks: mmap, or the program break when built with -DLM_USE_SBRK
 * (Linux only; sbrk is deprecated on macOS). */
static void *os_chunk(size_t len) {
#if defined(LM_USE_SBRK) && defined(__linux__)
  void *cur = sbrk(0);
  size_t pad = (ALIGNMENT - ((uintptr_t)cur % ALIGNMENT)) % ALIGNMENT;
  void *p = sbrk((intptr_t)(len + pad));
  return p == (void *)-1 ? NULL : (char *)p + pad;
#else
  return os_mmap(len);
#endif
}

static void account(size_t add, size_t sub) {
  g_mapped = g_mapped + add - sub;
  if (g_mapped > g_peak_mapped)
    g_peak_mapped = g_mapped;
}

/* ------------------------------------------------------------------ */
/* Free list (explicit, doubly linked)                                 */
/* ------------------------------------------------------------------ */

static void list_insert(block_t *b) {
  b->prev = NULL;
  b->next = g_free_head;
  if (g_free_head)
    g_free_head->prev = b;
  g_free_head = b;
}

static void list_remove(block_t *b) {
  if (b->prev)
    b->prev->next = b->next;
  else
    g_free_head = b->next;
  if (b->next)
    b->next->prev = b->prev;
}

static block_t *find_fit(size_t bs) {
  block_t *best = NULL;
  for (block_t *b = g_free_head; b; b = b->next) {
    if (bsize(b) < bs)
      continue;
#ifdef LM_BEST_FIT
    if (!best || bsize(b) < bsize(best)) {
      best = b;
      if (bsize(b) == bs)
        break;
    }
#else
    return b;
#endif
  }
  return best;
}

/* ------------------------------------------------------------------ */
/* Chunk management                                                    */
/* ------------------------------------------------------------------ */

static int grow(size_t bs) {
  size_t ps = page_size();
  size_t need = bs + 32; /* chunk header + epilogue */
  size_t len = need > CHUNK_BYTES ? need : CHUNK_BYTES;
  len = (len + ps - 1) & ~(ps - 1);
  char *mem = os_chunk(len);
  if (!mem)
    return 0;
  account(len, 0);

  chunk_t *c = (chunk_t *)mem;
  c->len = len;
  c->next = g_chunks;
  g_chunks = c;

  block_t *b = (block_t *)(mem + 16);
  size_t total = len - 32;
  b->size = total | PREV_USED;
  block_t *epi = (block_t *)(mem + len - 16);
  epi->prev_size = total;
  epi->size = USED;
  list_insert(b);
  return 1;
}

/* ------------------------------------------------------------------ */
/* Core operations (caller holds any required lock)                    */
/* ------------------------------------------------------------------ */

/* Mark b (free, already unlinked) as used for bs bytes, splitting off the
 * remainder when it is big enough to stand alone. */
static void place(block_t *b, size_t bs) {
  size_t total = bsize(b);
  size_t rem = total - bs;
  if (rem >= MIN_BLOCK) {
    b->size = bs | USED | (b->size & PREV_USED);
    block_t *r = (block_t *)((char *)b + bs);
    r->size = rem | PREV_USED;
    next_phys(r)->prev_size = rem; /* footer; its PREV_USED is already clear */
    list_insert(r);
  } else {
    b->size |= USED;
    next_phys(b)->size |= PREV_USED;
  }
}

/* Free a block whose USED flag is set; coalesce with free neighbours. */
static void release(block_t *b) {
  size_t size = bsize(b);
  unsigned prev_used = b->size & PREV_USED;
  block_t *next = (block_t *)((char *)b + size);

  if (!(next->size & USED)) {
    list_remove(next);
    size += bsize(next);
  }
  if (!prev_used) {
    block_t *p = prev_phys(b);
    list_remove(p);
    size += bsize(p);
    b = p;
  }
  b->size = size | PREV_USED; /* two free blocks are never adjacent */
  block_t *after = (block_t *)((char *)b + size);
  after->prev_size = size;
  after->size &= ~(size_t)PREV_USED;
  list_insert(b);
}

/* Shrink used block b to bs bytes, returning any big-enough tail. */
static void trim(block_t *b, size_t bs) {
  size_t total = bsize(b);
  if (total - bs < MIN_BLOCK)
    return;
  b->size = bs | (b->size & FLAG_MASK);
  block_t *r = (block_t *)((char *)b + bs);
  r->size = (total - bs) | USED | PREV_USED;
  release(r);
}

static void *alloc_large(size_t bs) {
  size_t ps = page_size();
  size_t len = (bs + ps - 1) & ~(ps - 1);
  if (len < bs)
    return NULL;
  block_t *b = os_mmap(len);
  if (!b)
    return NULL;
  b->size = len | USED | PREV_USED | MMAPPED;
  account(len, 0);
  g_large_blocks++;
  return payload(b);
}

static void free_large(block_t *b) {
  size_t len = bsize(b);
  g_large_blocks--;
  account(0, len);
  munmap(b, len);
}

/* allow_large == 0 forces the arena path (needed by aligned allocation,
 * which relies on carving the block). */
static void *alloc_core(size_t n, int allow_large) {
  size_t bs = req_to_bsize(n);
  if (!bs) {
    return NULL;
  }
  if (allow_large && bs >= MMAP_THRESHOLD)
    return alloc_large(bs);
  block_t *b = find_fit(bs);
  if (!b) {
    if (!grow(bs))
      return NULL;
    b = find_fit(bs);
  }
  list_remove(b);
  place(b, bs);
  return payload(b);
}

static void free_core(void *p) {
  if ((uintptr_t)p & (ALIGNMENT - 1))
    fatal("invalid free (misaligned pointer)", p);
  block_t *b = hdr(p);
  if (!(b->size & USED))
    fatal("double-free or invalid free", p);
  if (b->size & MMAPPED)
    free_large(b);
  else
    release(b);
}

static void *realloc_core(void *p, size_t n) {
  block_t *b = hdr(p);
  if (!(b->size & USED))
    fatal("realloc on freed or invalid pointer", p);
  size_t bs = req_to_bsize(n);
  if (!bs)
    return NULL;

  if (!(b->size & MMAPPED)) {
    if (bs <= bsize(b)) {
      trim(b, bs);
      return p;
    }
    /* Grow in place into a free right-hand neighbour. */
    block_t *next = next_phys(b);
    if (!(next->size & USED) && bsize(b) + bsize(next) >= bs) {
      list_remove(next);
      b->size = (bsize(b) + bsize(next)) | (b->size & FLAG_MASK);
      next_phys(b)->size |= PREV_USED;
      trim(b, bs);
      return p;
    }
  } else if (bs <= bsize(b) && bs > bsize(b) / 2) {
    return p;
  }

  void *q = alloc_core(n, 1);
  if (!q)
    return NULL;
  size_t old = usable(b);
  memcpy(q, p, old < n ? old : n);
  free_core(p);
  return q;
}

static void *memalign_core(size_t align, size_t n) {
  if (align <= ALIGNMENT)
    return alloc_core(n, 1);
  size_t bs = req_to_bsize(n);
  if (!bs || bs > SIZE_MAX - align - MIN_BLOCK)
    return NULL;
  void *p = alloc_core(n + align + MIN_BLOCK, 0);
  if (!p)
    return NULL;
  block_t *b = hdr(p);
  uintptr_t a = ((uintptr_t)p + align - 1) & ~(uintptr_t)(align - 1);
  size_t gap = a - (uintptr_t)p;
  if (gap && gap < MIN_BLOCK) {
    a += align;
    gap += align;
  }
  if (gap) {
    /* Split the front off as its own block and free it. */
    block_t *nb = hdr((void *)a);
    nb->size = (bsize(b) - gap) | USED | PREV_USED;
    b->size = gap | USED | (b->size & PREV_USED);
    release(b);
    b = nb;
  }
  trim(b, bs);
  return (void *)a;
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

void *lm_malloc(size_t size) {
  if (size == 0)
    return NULL;
  void *p = alloc_core(size, 1);
  if (!p)
    errno = ENOMEM;
  return p;
}

void lm_free(void *ptr) {
  if (ptr)
    free_core(ptr);
}

void *lm_calloc(size_t nmemb, size_t size) {
  if (nmemb == 0 || size == 0)
    return NULL;
  if (nmemb > SIZE_MAX / size) {
    errno = ENOMEM;
    return NULL;
  }
  void *p = lm_malloc(nmemb * size);
  if (p)
    memset(p, 0, nmemb * size);
  return p;
}

void *lm_realloc(void *ptr, size_t new_size) {
  if (!ptr)
    return lm_malloc(new_size);
  if (new_size == 0) {
    lm_free(ptr);
    return NULL;
  }
  void *q = realloc_core(ptr, new_size);
  if (!q)
    errno = ENOMEM;
  return q;
}

int lm_posix_memalign(void **out, size_t alignment, size_t size) {
  if (alignment < sizeof(void *) || (alignment & (alignment - 1)))
    return EINVAL;
  void *p = memalign_core(alignment, size ? size : 1);
  if (!p)
    return ENOMEM;
  *out = p;
  return 0;
}

void *lm_aligned_alloc(size_t alignment, size_t size) {
  if (alignment == 0 || (alignment & (alignment - 1))) {
    errno = EINVAL;
    return NULL;
  }
  void *p = memalign_core(alignment, size ? size : 1);
  if (!p)
    errno = ENOMEM;
  return p;
}

size_t lm_usable_size(void *ptr) { return ptr ? usable(hdr(ptr)) : 0; }
size_t lm_heap_bytes(void) { return g_mapped; }
size_t lm_peak_heap_bytes(void) { return g_peak_mapped; }

/* ------------------------------------------------------------------ */
/* Introspection                                                       */
/* ------------------------------------------------------------------ */

void lm_get_stats(lm_stats_t *o) {
  memset(o, 0, sizeof *o);
  for (chunk_t *c = g_chunks; c; c = c->next) {
    o->chunks++;
    for (block_t *b = (block_t *)((char *)c + 16); bsize(b); b = next_phys(b)) {
      if (b->size & USED) {
        o->used_blocks++;
        o->used_bytes += bsize(b);
      } else {
        o->free_blocks++;
        o->free_bytes += bsize(b);
      }
    }
  }
  o->large_blocks = g_large_blocks;
  o->mapped_bytes = g_mapped;
}

void lm_stats(void) {
  lm_stats_t s;
  lm_get_stats(&s);
  printf("\n===== HEAP STATS =====\n");
  printf("  Chunks       : %zu (%zu bytes mapped, peak %zu)\n", s.chunks,
         s.mapped_bytes, g_peak_mapped);
  printf("  Used blocks  : %zu (%zu bytes)\n", s.used_blocks, s.used_bytes);
  printf("  Free blocks  : %zu (%zu bytes)\n", s.free_blocks, s.free_bytes);
  printf("  Large maps   : %zu\n", s.large_blocks);
  printf("======================\n\n");
}

void lm_heap_dump(void) {
  printf("\n===== HEAP DUMP =====\n");
  int idx = 0;
  for (chunk_t *c = g_chunks; c; c = c->next) {
    printf("chunk %p len=%zu\n", (void *)c, c->len);
    for (block_t *b = (block_t *)((char *)c + 16); bsize(b); b = next_phys(b))
      printf("  [%4d] %p size=%-8zu %s\n", idx++, (void *)b, bsize(b),
             (b->size & USED) ? "USED" : "FREE");
  }
  printf("=====================\n\n");
}

#define CHECK(cond, msg)                                                      \
  do {                                                                        \
    if (!(cond)) {                                                            \
      fprintf(stderr, "[libmalloc] heap check failed: %s\n", msg);            \
      return 1;                                                               \
    }                                                                         \
  } while (0)

int lm_check(void) {
  size_t free_seen = 0;
  for (chunk_t *c = g_chunks; c; c = c->next) {
    block_t *b = (block_t *)((char *)c + 16);
    unsigned prev_used = PREV_USED; /* the first block has a virtual used prev */
    size_t prev_size = 0;
    while (bsize(b)) {
      CHECK(bsize(b) >= MIN_BLOCK && bsize(b) % 16 == 0, "bad block size");
      CHECK(((uintptr_t)payload(b) & 15) == 0, "misaligned payload");
      CHECK((b->size & PREV_USED) == prev_used, "PREV_USED flag mismatch");
      if (!prev_used)
        CHECK(b->prev_size == prev_size, "footer (prev_size) mismatch");
      if (b->size & USED) {
        prev_used = PREV_USED;
      } else {
        CHECK(prev_used, "two adjacent free blocks");
        prev_used = 0;
        prev_size = bsize(b);
        free_seen++;
      }
      b = next_phys(b);
    }
    CHECK(b->size & USED, "epilogue not marked used");
    CHECK((char *)b + 16 == (char *)c + c->len, "chunk walk missed the end");
    CHECK((b->size & PREV_USED) == prev_used, "epilogue PREV_USED mismatch");
  }
  size_t listed = 0;
  for (block_t *b = g_free_head; b; b = b->next) {
    CHECK(!(b->size & USED), "used block on free list");
    CHECK(!b->next || b->next->prev == b, "free-list link broken");
    listed++;
  }
  CHECK(listed == free_seen, "free-list count differs from heap walk");
  return 0;
}
