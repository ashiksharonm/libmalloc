# libmalloc design

## Problem and scope

A general-purpose `malloc`/`free`/`realloc`/`calloc`/`posix_memalign` in C that is correct, inspectable and usable as a drop-in replacement (`LD_PRELOAD` / `DYLD_INSERT_LIBRARIES`) for ordinary programs. It is a study allocator: it favours clarity and verifiability (`lm_check()`, sanitizers, a heap snapshot) over peak performance.

Requirements

- Functional: 16-byte aligned payloads; `realloc` that shrinks and grows in place when it can; aligned allocation; double-free and misaligned-free detection; introspection (stats, dump, JSON snapshot).
- Non-functional: O(1) free and coalescing; near-O(1) allocation via a bin bitmap; thread safe; no allocation inside the allocator's own introspection paths (so it can sit behind the shim); portable across Linux and macOS.
- Non-goals: per-thread caches, returning arena chunks to the OS, `fork` safety, hardening against heap exploitation.

## Memory layout

```
 arena chunk (256 KiB, mmap)
 +--------+----------------+----------------+-----+----------------+-----------+
 | chunk  | block          | block          | ... | block          | epilogue  |
 | header | hdr|payload    | hdr|payload    |     | hdr|payload    | size=0,   |
 | 16 B   |                |                |     |                | USED      |
 +--------+----------------+----------------+-----+----------------+-----------+

 block header (16 B in front of a 16-byte aligned payload)
   +0   prev_size  footer of the previous block, valid only while that block is FREE
   +8   size|flags USED=1  PREV_USED=2  MMAPPED=4   (size is a multiple of 16, min block 32)
   +16  payload    (free blocks store next/prev list pointers here)
```

Because `prev_size` overlaps the previous block's payload tail while it is in use, the effective per-block overhead is 8 bytes. The epilogue (size 0, USED) stops forward coalescing at the chunk end; `PREV_USED` set on the first block stops backward coalescing at the chunk start.

Requests >= 128 KiB bypass the arena and get a dedicated mapping (`MMAPPED`), `munmap`ed on free.

## Free lists and bins

```
 g_bitmap  (192 bits)   bit i set  <=>  g_bins[i] non-empty
 g_bins[0..61]   exact sizes   32, 48, 64, ... 1008        (one bin per 16 bytes)
 g_bins[62..191] 4 bins per power of two from 1 KiB:
                 [1024,1280) [1280,1536) [1536,1792) [1792,2048) [2048,2560) ...
 each bin: doubly linked list of free blocks (LIFO insert)

 malloc(n):  bs = round_up(n+8, 16)  ->  bin_index(bs)
             ctz over the bitmap from that bin upward -> first non-empty bin
             exact bins: any block fits;  range bins: smallest fitting block in the list
             split if the remainder >= 32 B, remainder goes back to its bin
             nothing found -> grow() a new 256 KiB chunk
 free(p):    clear USED, merge with free left neighbour (via prev_size) and right neighbour,
             insert the merged block into its bin           (all O(1))
```

The bin ranges and occupancy can be seen for a real run with the snapshot below.

## Thread-safety model

One global `pthread_mutex_t` guards all allocator state (chunk list, bins, bitmap, large-mapping list, counters). Every public entry point takes it exactly once; internal `_core` functions assume it is held. Rules that keep the shim safe:

- The lock is never held across a call that can allocate (no `printf`; introspection formats into stack buffers and uses `write(2)`; the JSON snapshot prints integers only because libc floating-point formatting may call `malloc`).
- No lock-order issues: there is a single lock.
- Verified with ThreadSanitizer (`make tsan`) and an 8-thread stress test.

Trade-off: correctness over scalability. Measured 4-thread throughput is well below the system allocator (see README); per-thread arenas or caches are the next step.

## Fragmentation trade-offs

| Choice | Effect |
|---|---|
| Immediate bidirectional coalescing | keeps external fragmentation low, costs a few loads per free |
| Exact 16-byte bins below 1 KiB | no internal waste from bin rounding for small objects; many bins (62) |
| 4 bins per power of two above | bounds internal waste of a size class to ~25% of the class, best-fit within a bin |
| LIFO bins | hot blocks reused first (cache locality) but can scatter long-lived objects |
| 256 KiB chunks never returned | simple and fast; peak heap is retained, and small programs show inflated fragmentation ratios |
| mmap threshold 128 KiB | big blocks cannot fragment the arena and go back to the OS on free |

`external_fragmentation` in the snapshot is `1 - largest_free_block / total_free_bytes` (0 = one big hole, ->1 = many small holes). It is a diagnostic, not the same as the benchmark's peak-heap / peak-requested ratio.

## Heap visualizer

`LM_HEAP_JSON=<path>` (the `%p` in the path expands to the pid) makes the shim write a snapshot at process exit; `lm_dump_json(path)` does the same from code. The JSON holds: totals, largest free block, external fragmentation, per-bin free block counts and bytes, a log2 histogram of live block sizes, and a 64-cell occupancy strip per chunk.

```
LM_HEAP_JSON=/tmp/heap_%p.json LD_PRELOAD=build/libmalloc_shim.so python3 -c "..."
python3 tools/render_heap.py /tmp/heap_1234.json --svg heap.svg
```

`make json-test` fragments a heap on purpose and checks that the JSON totals equal `lm_get_stats()`.

## Failure modes

| Failure | Behaviour |
|---|---|
| double free / misaligned free | detected by the flag bits and alignment check, prints and aborts |
| overflow of a request size | `req_to_bsize` returns 0, `malloc` returns NULL; `calloc` checks `nmemb*size` |
| `mmap` fails | `malloc` returns NULL (errno untouched by the library itself) |
| heap corruption by the caller | not detected in the fast path; `lm_check()` finds boundary-tag and list inconsistencies in tests |
| `fork` while another thread holds the lock | child can deadlock (no atfork handler) |

## Decisions

- ADR-1 segregated explicit lists over an implicit list: the previous implementation scanned all blocks first-fit (see the baseline in `bench/baseline/` and the README table).
- ADR-2 bitmap of non-empty bins: next-fit search in a few word operations instead of walking empty bins.
- ADR-3 one global lock: smallest thing that is provably correct and TSAN-clean; scalability deferred.
- ADR-4 `mmap` chunks (with an optional `sbrk` mode): works identically on macOS and Linux and does not fight other users of the program break.
- ADR-5 JSON snapshot written with `write(2)` and integer formatting: usable from the shim destructor without re-entering the allocator.
