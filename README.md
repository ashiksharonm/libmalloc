# libmalloc

A from-scratch `malloc` / `free` / `realloc` / `calloc` in C (~600 lines): segregated size-class free lists, boundary-tag coalescing, an mmap-backed arena, a global mutex, and a drop-in shim that can replace the system allocator in real programs. Built to learn allocator design, not to replace glibc. Design notes: [docs/DESIGN.md](docs/DESIGN.md).

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
make bench      # -O2 benchmark matrix -> results/bench.json (OPT=-O0 for unoptimised)
make json-test  # heap snapshot test (JSON totals vs lm_get_stats)
```

CI (ubuntu + macOS): `-Wall -Wextra -Werror` at -O0 and -O2, ASAN, UBSAN, heap-snapshot test, shim test; plus TSAN, valgrind, an sbrk-arena job and a benchmark job on Linux.

## Using it as a malloc replacement

```
LD_PRELOAD=$PWD/build/libmalloc_shim.so ls -la                      # Linux
DYLD_INSERT_LIBRARIES=$PWD/build/libmalloc_shim.dylib ./yourprog    # macOS
LM_SHIM_REPORT=/tmp/report.txt ...   # append call counts at exit
```

`tests/shim_test.sh` runs `ls -la`, `python3 -c "print(sum(range(10**6)))"` and a shell pipeline under the Linux shim in CI, and requires the report file to show allocator calls (so a silently unloaded shim fails). On macOS, SIP-protected system binaries ignore `DYLD_INSERT_LIBRARIES`; the test uses its own probe program and Homebrew python3. Verified locally on macOS (Apple M5) and in Linux CI / an Ubuntu 24.04 container.

## Heap visualizer

Set `LM_HEAP_JSON=<path>` (a `%p` in the path becomes the pid) and the shim writes a heap snapshot when the process exits; `lm_dump_json(path)` does the same from code. The snapshot has free-list occupancy per bin, a histogram of live block sizes, a 64-cell occupancy strip per arena chunk, the largest free block and an external-fragmentation figure. `tools/render_heap.py` prints it as text and can write an SVG:

```
LM_HEAP_JSON=/tmp/heap_%p.json LD_PRELOAD=$PWD/build/libmalloc_shim.so python3 -c "print(sum(range(10**6)))"
python3 tools/render_heap.py /tmp/heap_*.json --svg heap.svg
```

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

### CI runner numbers (Linux)

The `bench` CI job builds the same harness and runs it against glibc's allocator on a GitHub-hosted runner (`ubuntu-latest`): AMD EPYC 7763, 4 vCPU, 15 GiB, Linux 6.17, gcc 13.3, one run (CI run 39051454189, medians of 5). Ratio = system ops/s divided by current ops/s, so 1.80x means glibc is 1.80x faster. Runner hardware is shared and varies between runs; treat these as indicative.

| Workload | -O2 current | -O2 system | -O2 ratio | -O0 current | -O0 system | -O0 ratio |
|---|---|---|---|---|---|---|
| random | 19.0 M | 20.5 M | 1.08x | 7.9 M | 14.2 M | 1.80x |
| realloc-heavy | 13.3 M | 10.3 M | 0.78x | 5.5 M | 8.5 M | 1.54x |
| small-object | 26.2 M | 32.3 M | 1.23x | 11.1 M | 19.9 M | 1.80x |
| bigrandom | 11.9 M | 13.9 M | 1.17x | 5.9 M | 9.0 M | 1.52x |
| mt4 (4 threads) | 2.1 M | 31.8 M | 15.1x | 1.4 M | 24.3 M | 16.9x |

The earlier 265-275x gap at -O0 was the previous implementation (implicit list, first fit); the current allocator is within about 1.5-1.8x of glibc single-threaded at -O0 (the system allocator is always optimised; only this harness and the allocator are built at -O0) and about 15-17x behind with four threads, which is the cost of the single global lock. `-O2 realloc-heavy` is the one workload where the current allocator is ahead (0.78x). The job's step summary and `bench-results` artifact carry the raw JSON.

## Layout

```
src/allocator.{h,c}  allocator          src/shim.c  malloc interposer
tests/               unit, thread, snapshot and shim tests
tools/render_heap.py heap snapshot renderer
docs/DESIGN.md       allocator design notes
bench/               harness, baseline/ (old implementation), run.py
results/bench.json   measured data
```
