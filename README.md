# libmalloc — A Custom Memory Allocator in C

A fully working `malloc` / `free` / `realloc` / `calloc` implementation from scratch, built for learning and systems engineering portfolio purposes.

Built to understand: free-list management, boundary tags, coalescing, block splitting, and the tradeoffs between search strategies.

---

## Block Layout

Every allocation is preceded by a header and followed by a footer (boundary tag):

```
+------------------------------------------+  ← block_header_t (32 bytes on 64-bit)
│  .size     usable bytes in this block    │
│  .is_free  1=free / 0=used               │
│  .magic    0xCAFEBABE (used)             │
│            0xDEADBEEF (free)             │
│  .next  →  next block in chain           │
│  .prev  →  previous block in chain       │
+------------------------------------------+
│  user data  (size bytes)                 │  ← pointer returned to caller
+------------------------------------------+  ← block_footer_t (12 bytes)
│  .size     (mirrors header.size)         │
│  .is_free  (mirrors header.is_free)      │
+------------------------------------------+
```

Total heap consumption per allocation: `HEADER_SIZE + size + FOOTER_SIZE`

---

## Architecture

### Heap Growth
Uses `sbrk()` to extend the process data segment on demand. Each new block is appended to a single implicit block list anchored at `heap_start` (blocks carry `next`/`prev` pointers in their headers, and free and used blocks share the same list; there is no separate free list).

### Free-List Strategy
Two strategies, switchable at compile time:

| Strategy | Flag | Behaviour |
|----------|------|-----------|
| First-fit | *(default)* | Returns first block ≥ requested size. O(1) best case. |
| Best-fit | `-DBEST_FIT` | Scans entire list; returns smallest block that fits. Reduces fragmentation. |

### Block Splitting
If a free block is significantly larger than requested, it is split into two blocks: one allocated, one free remainder. Guards against `size_t` underflow before any subtraction.

### Coalescing (Boundary Tag Technique)
On every `lm_free()`, adjacent free blocks are merged immediately:

- **Forward merge**: use `block->next` (O(1) via linked list)
- **Backward merge**: use the footer of the physically preceding block (O(1) via boundary tag)

Physical adjacency is verified before trusting the boundary tag — prevents corruption when the allocator shares the heap with `glibc malloc` (e.g., in benchmarks).

**Four coalescing cases** (Knuth 1973):
```
Case 1: [USED][THIS][USED]  → nothing
Case 2: [USED][THIS][FREE]  → merge with next
Case 3: [FREE][THIS][USED]  → merge with prev
Case 4: [FREE][THIS][FREE]  → three-way merge
```

### Safety
- **Double-free detection**: magic field flips between `0xCAFEBABE` (used) and `0xDEADBEEF` (free). Double-free prints an error and calls `__builtin_trap()`.
- **Overflow guard in calloc**: checks `nmemb * size` before multiplying.
- **Alignment**: all sizes rounded up to a multiple of 16 bytes; returned payloads are 16-byte aligned (covered by a test).

---

## Project Structure

```
libmalloc/
├── src/
│   ├── allocator.h       # block_header_t, block_footer_t, API
│   ├── allocator.c       # malloc / free / realloc / calloc (fully commented)
│   └── utils.c           # lm_heap_dump(), lm_stats()
├── tests/
│   └── test_allocator.c  # 15 tests + stress test + double-free detection
├── bench/
│   └── bench.c           # benchmark vs glibc malloc
├── Makefile
└── README.md
```

---

## Building and Running

```bash
# Build and run tests
make run

# Run with AddressSanitizer (macOS M2 / Linux)
make asan

# Run double-free detection test (crashes intentionally)
./build/test_allocator double_free

# Benchmark: first-fit vs system malloc (Apple libmalloc)
make bench

# Benchmark: best-fit vs system malloc (Apple libmalloc)
make bench_best_fit

# Valgrind (Linux only)
make valgrind

# Clean
make clean
```

---

## Benchmark Results

> Measured on a MacBook with Apple M5 (16 GB), macOS, `clang` at `-O0` (the Makefile's default `CFLAGS`), via `make bench` / `make bench_best_fit`. The baseline "glibc" column is the **system allocator on macOS (Apple libmalloc)**, not glibc; `sbrk()` is deprecated on macOS, so treat the absolute numbers as indicative only. Re-run on Linux for glibc numbers.

**Methodology**: 5 runs per workload, minimum reported. Sizes: 8-4096 bytes (pseudo-random, fixed LCG seed). Timing: `clock_gettime(CLOCK_MONOTONIC)`. The ratio is large because every `lm_malloc` does a linear scan of one block list (O(n)) while the system allocator uses size-segregated bins.

### First-fit + Boundary Tags vs system malloc (Apple libmalloc)

| Ops     | lm_malloc (µs) | System malloc (µs) | Ratio |
|---------|---------------|-------------------|-------|
| 1,000 | 1,736.0 | 79.0 | 22.0x |
| 10,000 | 229,577.0 | 867.0 | 264.8x |
| 100,000 | 3,426,577.0 | 12,448.0 | 275.3x |

### Best-fit + Boundary Tags vs system malloc (Apple libmalloc)

| Ops     | lm_malloc (µs) | System malloc (µs) | Ratio |
|---------|---------------|-------------------|-------|
| 1,000 | 1,163.0 | 65.0 | 17.9x |
| 10,000 | 219,002.0 | 905.0 | 242.0x |
| 100,000 | 3,133,312.0 | 11,390.0 | 275.1x |

---

## Known Limitations vs system malloc (Apple libmalloc)

| Feature | libmalloc | glibc (ptmalloc2) |
|---------|-----------|-------------------|
| Thread safety | ✗ No locks | ✓ Per-thread arenas |
| Heap return to OS | ✗ Never (sbrk only grows) | ✓ `brk()` shrink + `mmap` return |
| Size-class segregation | ✗ Single list | ✓ Bins for 8–512B ranges |
| Mmap for large allocs | ✗ Always sbrk | ✓ `mmap` for allocs > 128KB |
| Search strategy | First-fit / Best-fit | Bin-based O(1) lookup |
| Decades of optimisation | ✗ ~400 lines | ✓ 40+ years |

**Why is glibc faster?** glibc maintains separate *bins* (free lists segregated by size range), giving near-O(1) allocation for most sizes. It also uses `mmap` for large allocations so they can be returned to the OS immediately. libmalloc uses a single unsegregated list — honest, simple, and educational.

---

## What I Learned

- How `sbrk()` extends the process heap and why it can only grow (not shrink)
- The Knuth boundary tag technique for O(1) bidirectional coalescing
- Why `size_t` underflow is a real bug class and how to guard against it
- Why first-fit and best-fit have different fragmentation characteristics
- Why production allocators use size-segregated bins instead of a single list
- How `LD_PRELOAD` can swap allocators at runtime for benchmarking

---

## Valgrind (Linux)

```bash
valgrind --leak-check=full --error-exitcode=1 ./build/test_allocator
# Expected: 0 errors, 0 leaks
```

On macOS (M2), use AddressSanitizer instead:
```bash
make asan
# Expected: no ASAN errors
```
