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
Uses `sbrk()` to extend the process data segment on demand. Each new block is appended to a doubly-linked chain anchored at `heap_start`.

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
- **Alignment**: all sizes rounded up to `sizeof(void*)` (8 bytes on 64-bit).

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

# Benchmark: first-fit vs glibc
make bench

# Benchmark: best-fit vs glibc
make bench_best_fit

# Valgrind (Linux only)
make valgrind

# Clean
make clean
```

---

## Benchmark Results

> **Fill in your actual numbers after running on Linux.**
> These numbers were measured on Ubuntu/Debian x86-64 with `glibc`.
> Do not use numbers from macOS — `sbrk()` is deprecated there and adds overhead.

**Methodology**: 5 runs per workload, minimum reported. Sizes: 8–4096 bytes (pseudo-random, LCG seed fixed for reproducibility). Timing: `clock_gettime(CLOCK_MONOTONIC)`.

### First-fit + Boundary Tags vs glibc

| Ops     | lm_malloc (µs) | glibc malloc (µs) | Ratio |
|---------|---------------|-------------------|-------|
| 1,000   | [YOUR NUMBER] | [YOUR NUMBER]     | Xx    |
| 10,000  | [YOUR NUMBER] | [YOUR NUMBER]     | Xx    |
| 100,000 | [YOUR NUMBER] | [YOUR NUMBER]     | Xx    |

### Best-fit + Boundary Tags vs glibc

| Ops     | lm_malloc (µs) | glibc malloc (µs) | Ratio |
|---------|---------------|-------------------|-------|
| 1,000   | [YOUR NUMBER] | [YOUR NUMBER]     | Xx    |
| 10,000  | [YOUR NUMBER] | [YOUR NUMBER]     | Xx    |
| 100,000 | [YOUR NUMBER] | [YOUR NUMBER]     | Xx    |

---

## Known Limitations vs glibc

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
