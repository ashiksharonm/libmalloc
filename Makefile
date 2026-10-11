# libmalloc
#
#   make run        build and run the single-threaded test suite
#   make asan       tests under AddressSanitizer
#   make bench      build -O2 benchmarks and write results/bench.json
#   make clean

CC      ?= cc
OPT     ?= -O0
CFLAGS  ?= -Wall -Wextra -Werror -g $(OPT) -std=gnu11
CFLAGS  += -I./src
BUILD   = build

LIB_SRCS = src/allocator.c
TEST_SRC = tests/test_allocator.c

.PHONY: all tests run asan bench clean
all: tests

tests: $(BUILD)/test_allocator
$(BUILD)/test_allocator: $(LIB_SRCS) $(TEST_SRC) src/allocator.h | $(BUILD)
	$(CC) $(CFLAGS) -o $@ $(LIB_SRCS) $(TEST_SRC)

run: $(BUILD)/test_allocator
	./$(BUILD)/test_allocator
	@./$(BUILD)/test_allocator double_free 2>/dev/null && { echo "double-free not trapped"; exit 1; } || echo "double-free trapped as expected"

asan: $(LIB_SRCS) $(TEST_SRC) | $(BUILD)
	$(CC) $(CFLAGS) -fsanitize=address -fno-omit-frame-pointer -o $(BUILD)/test_asan $^
	./$(BUILD)/test_asan

bench:
	sh bench/build.sh
	python3 bench/run.py

$(BUILD):
	mkdir -p $(BUILD)

clean:
	rm -rf $(BUILD)
