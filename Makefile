# =============================================================================
# Makefile — libmalloc (Phase 2)
#
# Targets:
#   make / make tests     build default (first-fit + boundary tags)
#   make run              build and run tests
#   make best_fit         build with best-fit search strategy enabled
#   make asan             AddressSanitizer build (macOS M2 / Linux substitute for Valgrind)
#   make valgrind         Linux only: run under valgrind --leak-check=full
#   make clean            remove build artefacts
#
# Strategy flag:
#   Default (no flag):    first-fit
#   -DBEST_FIT:           best-fit
# =============================================================================

CC        = gcc
CFLAGS    = -Wall -Wextra -g -O0 -I./src
SRC_DIR   = src
TEST_DIR  = tests
BUILD_DIR = build

LIB_SRCS  = $(SRC_DIR)/allocator.c $(SRC_DIR)/utils.c
TEST_SRC  = $(TEST_DIR)/test_allocator.c

# =============================================================================
.PHONY: all tests run best_fit asan valgrind clean

all: tests

# --- Default: first-fit + boundary tags ---
tests: $(BUILD_DIR)/test_allocator

$(BUILD_DIR)/test_allocator: $(LIB_SRCS) $(TEST_SRC) | $(BUILD_DIR)
	$(CC) $(CFLAGS) -o $@ $^
	@echo ""
	@echo "Build [first-fit] → $(BUILD_DIR)/test_allocator"
	@echo "Run:  make run"
	@echo ""

# --- Build then immediately run ---
run: $(BUILD_DIR)/test_allocator
	./$(BUILD_DIR)/test_allocator

# --- Best-fit variant ---
best_fit: $(LIB_SRCS) $(TEST_SRC) | $(BUILD_DIR)
	$(CC) $(CFLAGS) -DBEST_FIT -o $(BUILD_DIR)/test_allocator_bestfit $^
	@echo ""
	@echo "Build [best-fit] → $(BUILD_DIR)/test_allocator_bestfit"
	./$(BUILD_DIR)/test_allocator_bestfit

# --- AddressSanitizer (macOS M2 safe; do NOT combine with Valgrind) ---
asan: $(LIB_SRCS) $(TEST_SRC) | $(BUILD_DIR)
	$(CC) $(CFLAGS) -fsanitize=address,undefined -o $(BUILD_DIR)/test_allocator_asan $^
	@echo "Running with AddressSanitizer..."
	./$(BUILD_DIR)/test_allocator_asan

# --- Benchmark: first-fit vs glibc ---
bench: $(LIB_SRCS) bench/bench.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -o $(BUILD_DIR)/bench $^
	@echo ""
	@echo "Running benchmark [first-fit]..."
	./$(BUILD_DIR)/bench

# --- Benchmark: best-fit vs glibc ---
bench_best_fit: $(LIB_SRCS) bench/bench.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -DBEST_FIT -o $(BUILD_DIR)/bench_bestfit $^
	@echo ""
	@echo "Running benchmark [best-fit]..."
	./$(BUILD_DIR)/bench_bestfit

# --- Valgrind target (Linux / Ubuntu only) ---
# Usage: ssh into your Linux box, clone the repo, run: make valgrind
valgrind: $(BUILD_DIR)/test_allocator
	@command -v valgrind >/dev/null 2>&1 || \
	  { echo "ERROR: valgrind not found. Install with: sudo apt install valgrind"; exit 1; }
	valgrind \
	  --leak-check=full \
	  --show-leak-kinds=all \
	  --track-origins=yes \
	  --error-exitcode=1 \
	  ./$(BUILD_DIR)/test_allocator

# --- Directory ---
$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

# --- Clean ---
clean:
	rm -rf $(BUILD_DIR)
	@echo "Cleaned."
