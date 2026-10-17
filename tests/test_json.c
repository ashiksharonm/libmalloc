/* Builds a fragmented heap, dumps the JSON snapshot, and records what
 * lm_get_stats() reports so tools/render_heap.py --check can compare. */
#include "allocator.h"
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv) {
  const char *out = argc > 1 ? argv[1] : "build/heap.json";
  enum { N = 2000 };
  void *p[N];
  srand(7);
  for (int i = 0; i < N; i++)
    p[i] = lm_malloc((size_t)(16 + rand() % 2000));
  for (int i = 0; i < N; i += 2) { /* free every other block: fragmentation */
    lm_free(p[i]);
    p[i] = NULL;
  }
  void *big = lm_malloc(300 << 10); /* dedicated mapping */
  if (lm_check()) return 1;
  if (lm_dump_json(out)) { fprintf(stderr, "dump failed\n"); return 1; }
  lm_stats_t s;
  lm_get_stats(&s);
  FILE *f = fopen("build/heap.expect", "w");
  if (!f) return 1;
  fprintf(f, "%zu %zu %zu %zu\n", s.used_blocks, s.free_blocks, s.large_blocks, s.used_bytes);
  fclose(f);
  lm_free(big);
  for (int i = 1; i < N; i += 2) lm_free(p[i]);
  return lm_check();
}
