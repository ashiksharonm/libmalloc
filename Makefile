# libmalloc
#
#   make run        build and run the single-threaded test suite
#   make asan       tests under AddressSanitizer
#   make bench      benchmark (see bench/)
#   make clean

CC      ?= cc
OPT     ?= -O0
CFLAGS  ?= -Wall -Wextra -Werror -g $(OPT) -std=gnu11
CFLAGS  += -I./src
BUILD   = build

LIB_SRCS = src/allocator.c
TEST_SRC = tests/test_allocator.c

.PHONY: all tests run asan clean
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

$(BUILD):
	mkdir -p $(BUILD)

clean:
	rm -rf $(BUILD)
