# libmalloc
#
#   make run        build and run the single-threaded test suite
#   make asan       tests under AddressSanitizer
#   make ubsan      tests under UndefinedBehaviorSanitizer
#   make tsan       multithreaded test under ThreadSanitizer
#   make valgrind   tests under valgrind (Linux)
#   make bench      build -O2 benchmarks and write results/bench.json
#   make clean

CC      ?= cc
OPT     ?= -O0
CFLAGS  ?= -Wall -Wextra -Werror -g $(OPT) -std=gnu11
CFLAGS  += -I./src -pthread
BUILD   = build

LIB_SRCS = src/allocator.c
TEST_SRC = tests/test_allocator.c
MT_SRC   = tests/test_threads.c

.PHONY: all tests run asan ubsan tsan valgrind bench check clean
all: tests

tests: $(BUILD)/test_allocator $(BUILD)/test_threads
$(BUILD)/test_allocator: $(LIB_SRCS) $(TEST_SRC) src/allocator.h | $(BUILD)
	$(CC) $(CFLAGS) -o $@ $(LIB_SRCS) $(TEST_SRC)

$(BUILD)/test_threads: $(LIB_SRCS) $(MT_SRC) src/allocator.h | $(BUILD)
	$(CC) $(CFLAGS) -o $@ $(LIB_SRCS) $(MT_SRC)

run: $(BUILD)/test_allocator $(BUILD)/test_threads
	./$(BUILD)/test_allocator
	./$(BUILD)/test_threads
	@./$(BUILD)/test_allocator double_free 2>/dev/null && { echo "double-free not trapped"; exit 1; } || echo "double-free trapped as expected"

asan: | $(BUILD)
	$(CC) $(CFLAGS) -fsanitize=address -fno-omit-frame-pointer -o $(BUILD)/test_asan $(LIB_SRCS) $(TEST_SRC)
	./$(BUILD)/test_asan
	$(CC) $(CFLAGS) -fsanitize=address -fno-omit-frame-pointer -o $(BUILD)/test_threads_asan $(LIB_SRCS) $(MT_SRC)
	./$(BUILD)/test_threads_asan

ubsan: | $(BUILD)
	$(CC) $(CFLAGS) -fsanitize=undefined -fno-sanitize-recover=undefined -o $(BUILD)/test_ubsan $(LIB_SRCS) $(TEST_SRC)
	./$(BUILD)/test_ubsan
	$(CC) $(CFLAGS) -fsanitize=undefined -fno-sanitize-recover=undefined -o $(BUILD)/test_threads_ubsan $(LIB_SRCS) $(MT_SRC)
	./$(BUILD)/test_threads_ubsan

tsan: | $(BUILD)
	$(CC) $(CFLAGS) -fsanitize=thread -o $(BUILD)/test_threads_tsan $(LIB_SRCS) $(MT_SRC)
	./$(BUILD)/test_threads_tsan

# valgrind cannot run the sanitizer builds; use the plain -O1 binaries.
valgrind: | $(BUILD)
	$(CC) $(CFLAGS) -O1 -o $(BUILD)/test_valgrind $(LIB_SRCS) $(TEST_SRC)
	$(CC) $(CFLAGS) -O1 -o $(BUILD)/test_threads_valgrind $(LIB_SRCS) $(MT_SRC)
	valgrind --error-exitcode=1 --leak-check=full ./$(BUILD)/test_valgrind
	valgrind --error-exitcode=1 --leak-check=full ./$(BUILD)/test_threads_valgrind

bench:
	sh bench/build.sh
	python3 bench/run.py

$(BUILD):
	mkdir -p $(BUILD)

clean:
	rm -rf $(BUILD)
