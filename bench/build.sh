#!/bin/sh
# Builds the benchmark binaries into build/bin/.  OPT=-O0 for an unoptimised build (default -O2).
set -e
CC=${CC:-cc}
F="${OPT:--O2} -std=gnu11 -Wall -Wextra -pthread"
mkdir -p build/bin
$CC $F -Isrc -o build/bin/bench_new bench/bench.c src/allocator.c
$CC $F -Wno-deprecated-declarations -DBASELINE -Ibench/baseline \
  -o build/bin/bench_old bench/bench.c bench/baseline/allocator.c
