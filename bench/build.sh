#!/bin/sh
# Builds the three benchmark binaries at -O2 into build/bin/.
set -e
CC=${CC:-cc}
F="-O2 -std=gnu11 -Wall -Wextra -pthread"
mkdir -p build/bin
$CC $F -Isrc -o build/bin/bench_new bench/bench.c src/allocator.c
$CC $F -Wno-deprecated-declarations -DBASELINE -Ibench/baseline \
  -o build/bin/bench_old bench/bench.c bench/baseline/allocator.c
