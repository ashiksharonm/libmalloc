/*
 * utils.c — Debug helpers for libmalloc.
 *
 * These are NOT part of the allocator logic.  They inspect the heap chain and
 * print human-readable summaries — useful while developing and testing.
 */

#include "allocator.h"
#include <stdio.h>

/*
 * External reference to the heap start pointer that lives in allocator.c.
 * Declared extern so utils.c can walk the list without duplicating state.
 */
extern block_header_t *heap_start;

/* -------------------------------------------------------------------------
 * lm_heap_dump — print every block currently on the heap
 *
 * Output looks like:
 *   [BLOCK 0] addr=0x... size=64  status=USED  magic=0xCAFEBABE
 *   [BLOCK 1] addr=0x... size=128 status=FREE  magic=0xDEADBEEF
 * ------------------------------------------------------------------------- */
void lm_heap_dump(void) {
  printf("\n===== HEAP DUMP =====\n");

  if (heap_start == NULL) {
    printf("  (heap is empty — no allocation yet)\n");
    printf("=====================\n\n");
    return;
  }

  block_header_t *cur = heap_start;
  int idx = 0;

  while (cur != NULL) {
    /*
     * The user-visible address is one header beyond the block_header_t.
     * Cast to char* first so pointer arithmetic works in bytes.
     */
    void *user_ptr = (char *)cur + HEADER_SIZE;

    printf("[BLOCK %3d]  header=%p  data=%p  size=%-8zu  status=%s  "
           "magic=0x%08X\n",
           idx, (void *)cur, user_ptr, cur->size,
           cur->is_free ? "FREE" : "USED", cur->magic);

    cur = cur->next;
    idx++;
  }

  printf("=====================\n\n");
}

/* -------------------------------------------------------------------------
 * lm_stats — print aggregate heap statistics
 * ------------------------------------------------------------------------- */
void lm_stats(void) {
  size_t total_blocks = 0;
  size_t free_blocks = 0;
  size_t used_blocks = 0;
  size_t free_bytes = 0;
  size_t used_bytes = 0;

  block_header_t *cur = heap_start;
  while (cur != NULL) {
    total_blocks++;
    if (cur->is_free) {
      free_blocks++;
      free_bytes += cur->size;
    } else {
      used_blocks++;
      used_bytes += cur->size;
    }
    cur = cur->next;
  }

  printf("\n===== HEAP STATS =====\n");
  printf("  Total blocks : %zu\n", total_blocks);
  printf("  Used  blocks : %zu  (%zu bytes payload)\n", used_blocks,
         used_bytes);
  printf("  Free  blocks : %zu  (%zu bytes payload)\n", free_blocks,
         free_bytes);
  printf("  Overhead     : %zu bytes (headers)\n", total_blocks * HEADER_SIZE);
  printf("======================\n\n");
}
