# riglink — native build + test (non-Zephyr). Compiles the riglink sources plus
# a vendored jcon, builds one executable per tests/test_*.c, runs them all.
#
#   make            build + run all native tests
#   make test       same
#   make clean      remove build/
#   make native-sim-test
#                   run Zephyr native_sim integration tests in Docker

CC      ?= cc
CSTD    ?= -std=c11
WARN    ?= -Wall -Wextra -Wpedantic
INC     := -Iinclude -Ithird_party/jcon/include
DEFS    ?= -DJCON_ENABLE_FLOAT -DRIGLINK_MEM_ACCESS
CFLAGS  ?= $(CSTD) $(WARN) $(INC) $(DEFS) -g -O0 -Itests

RIG_SRC  := $(wildcard src/*.c)
JCON_SRC := third_party/jcon/src/jcon.c
LIB_SRC  := $(RIG_SRC) $(JCON_SRC)

TEST_SRC := $(wildcard tests/test_*.c)
TEST_BIN := $(patsubst tests/%.c,build/%,$(TEST_SRC))

BUILD_DIR := build

.PHONY: all test native-sim-test clean
all: test

$(BUILD_DIR):
	@mkdir -p $(BUILD_DIR)

build/%: tests/%.c $(LIB_SRC) include/riglink.h tests/riglink_test.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) $< $(LIB_SRC) -o $@

test: $(TEST_BIN)
	@set -e; for t in $(TEST_BIN); do echo "--- $$t ---"; ./$$t; done
	@echo "All native tests passed."

native-sim-test:
	@./scripts/native-sim-test.sh $(PYTEST_ARGS)

clean:
	rm -rf $(BUILD_DIR)
