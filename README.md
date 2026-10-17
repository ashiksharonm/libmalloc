# libmalloc

A from-scratch `malloc` / `free` / `realloc` / `calloc` in C (~600 lines): segregated size-class free lists, boundary-tag coalescing, an mmap-backed arena, a global mutex, and a drop-in shim that can replace the system allocator in real programs. Built to learn allocator design, not to replace glibc.

## Design

- **Arena**: memory is obtained in 256 KiB chunks via `mmap` (macOS and Linux). On Linux, `-DLM_USE_SBRK` switches chunks to `sbrk`. Each chunk is fenced by a prologue flag and a zero-size epilogue block, so coalescing never leaves a chunk. Requests >= 128 KiB get a dedicated mapping that is `munmap`ed on free.
- **Blocks**: dlmalloc-style 8-byte overhead. A size word carries `USED`/`PREV_USED`/`MMAPPED` flags; the previous block's footer is stored in the next header only while that block is free. Payloads are 16-byte aligned; minimum block is 32 bytes.
- **Free lists**: explicit doubly-linked lists (O(1) unlink when coalescing), segregated into 62 exact bins (32..1008 bytes) plus four bins per power of two above that. A bitmap of non-empty bins finds the next fit with a count-trailing-zeros. Inside a large bin the smallest fitting block is chosen.
- **Coalescing**: immediate, both directions, O(1) via the boundary tags.
- **realloc**: shrinks in place (returning the tail), grows in place into a free right neighbour, otherwise allocate + copy + free.
- **Alignment**: `posix_memalign` / `aligned_alloc` over-allocate and split off the front. Aligned blocks needing >= 128 KiB are carved from arena chunks and are not returned to the OS.
- **Safety**: double-free and misaligned-free are detected and trap. `lm_check()` verifies every invariant (flags, footers, bin membership, bitmap, counts) and is used throughout the tests.
- **Threads**: one global `pthread` mutex. Correct, but it is the bottleneck under contention (see numbers).

Known limits: no `fork` handler (a fork while another thread holds the lock can deadlock the child), no per-thread caches, chunks are never returned to the OS (only large mappings are).

## Build and test

```
make run        # tests + 8-thread test + double-free trap check
make asan ubsan # sanitizers (tests and thread test)
make tsan       # thread test under ThreadSanitizer
make valgrind   # Linux
make shim       # build/libmalloc_shim.{so,dylib}
make shim-test  # run real programs under the shim
make bench      # -O2 benchmark matrix -> results/bench.json
```

CI (ubuntu + macOS): `-Wall -Wextra -Werror` at -O0 and -O2, ASAN, UBSAN, shim test; plus TSAN, valgrind and an sbrk-arena job on Linux.

## Using it as a malloc replacement

```
LD_PRELOAD=$PWD/build/libmalloc_shim.so ls -la                      # Linux
DYLD_INSERT_LIBRARIES=$PWD/build/libmalloc_shim.dylib ./yourprog    # macOS
LM_SHIM_REPORT=/tmp/report.txt ...   # append call counts at exit
```

`tests/shim_test.sh` runs `ls -la`, `python3 -c "print(sum(range(10**6)))"` and a shell pipeline under the Linux shim in CI, and requires the report file to show allocator calls (so a silently unloaded shim fails). On macOS, SIP-protected system binaries ignore `DYLD_INSERT_LIBRARIES`; the test uses its own probe program and Homebrew python3. Verified locally on macOS (Apple M5) and in Linux CI / an Ubuntu 24.04 container.

## Benchmarks

Apple M5, 10 cores, 16 GB, macOS, Apple clang 21, `-O2`, one run on 2026-10-11, median of 5 runs after a discarded warm-up (raw data and min/max in `results/bench.json`). "baseline" is the previous implementation (single implicit block list, first fit, sbrk), kept in `bench/baseline/`. "system" is Apple's allocator. Workloads are synthetic, fixed seed, one live-set of slots with random alloc/free (or realloc), sizes 8-4096 B (random), 8-4096 B (realloc, 128 slots), 8-128 B (small, 20k slots); bigrandom = random with 16k slots (~17 MB live); mt4 = bigrandom on 4 threads.

Throughput, ops/s (higher is better):

| Workload | baseline | current | system |
|---|---|---|---|
| random | 1.65 M | 12.8 M | 15.6 M |
| realloc-heavy | failed* | 7.6 M | 6.1 M |
| small-object | 66.5 k | 24.6 M | 16.4 M |
| bigrandom | n/a | 8.0 M | 11.3 M |
| mt4 (4 threads) | n/a | 1.15 M | 12.1 M |

Fragmentation = peak heap bytes / peak requested bytes (system allocator does not report heap size here):

| Workload | baseline | current |
|---|---|---|
| random | 1.12 | 1.26 |
| small-object | 1.97 | 1.50 |
| realloc-heavy | failed* | 1.63 |
| bigrandom | n/a | 1.04 |

Caveats: the baseline ran 200k ops per run (the others 1M) because its list scans are O(n); it is single-threaded and only run on the first three workloads. *The baseline aborted on the realloc workload with an allocation failure; its heap stopped growing near 4 MiB, which is consistent with macOS limiting `sbrk`, but I did not isolate the cause. Fragmentation at small live sizes is inflated by the 256 KiB chunk granularity. Run-to-run noise on this laptop is large (an earlier run of the same binaries gave different absolute numbers), so read the ratios loosely. With the single global lock, 4-thread throughput is about 10x below the system allocator.

## Layout

```
src/allocator.{h,c}  allocator          src/shim.c  malloc interposer
tests/               unit, thread and shim tests
bench/               harness, baseline/ (old implementation), run.py
results/bench.json   measured data
```
