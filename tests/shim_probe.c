/* Small program that uses every allocation entry point the shim exports. */
#define _DEFAULT_SOURCE
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(void) {
  char *s = strdup("hello"); /* libc-internal malloc */
  assert(s && !strcmp(s, "hello"));
  free(s);

  void *z = malloc(0);
  assert(z);
  free(z);

  int *a = calloc(1000, sizeof *a);
  for (int i = 0; i < 1000; i++)
    assert(a[i] == 0);
  a = realloc(a, 100000 * sizeof *a);
  for (int i = 0; i < 1000; i++)
    assert(a[i] == 0);
  free(a);

  void *p;
  assert(posix_memalign(&p, 4096, 1000) == 0 && ((uintptr_t)p & 4095) == 0);
  free(p);
  p = aligned_alloc(64, 640);
  assert(p && ((uintptr_t)p & 63) == 0);
  free(p);

  FILE *f = fopen("/dev/null", "w"); /* FILE buffers come from malloc */
  fprintf(f, "%d\n", 42);
  fclose(f);
  puts("probe ok");
  return 0;
}
