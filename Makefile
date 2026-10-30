# SPDX-License-Identifier: GPL-3.0-or-later
# ale-core: plain make, C99 + libm, no other dependency.
#   make            shared + static library and tools in $(BUILD)
#   make test       unit tests and the bit-exact vector check (needs only the checked-in vectors)
#   make asan       the same under AddressSanitizer + UndefinedBehaviorSanitizer
#   make ref        build the reference programs (ref/build_ref.sh, needs the reference sources)
#   make vectors    regenerate vectors/ with the reference programs (scripts/make_vectors.py)
CC ?= cc
BUILD ?= build
CFLAGS ?= -O2
ALE_CFLAGS = -std=c99 -Wall -Wextra -pedantic -fPIC -ffp-contract=off -fno-fast-math -Iinclude
LDLIBS = -lm

UNAME := $(shell uname)
ifeq ($(UNAME),Darwin)
  SHLIB = libale.dylib
  SHFLAGS = -dynamiclib -install_name @rpath/libale.dylib
else
  SHLIB = libale.so
  SHFLAGS = -shared
endif

SRCS = src/ale_rx.c src/ale_tx.c src/ale_tables.c
OBJS = $(SRCS:src/%.c=$(BUILD)/%.o)

all: shared static $(BUILD)/ale_dump

shared: $(BUILD)/$(SHLIB)
static: $(BUILD)/libale.a

$(BUILD)/%.o: src/%.c include/ale.h src/ale_internal.h
	@mkdir -p $(BUILD)
	$(CC) $(ALE_CFLAGS) $(CFLAGS) -c $< -o $@

$(BUILD)/$(SHLIB): $(OBJS)
	$(CC) $(SHFLAGS) -o $@ $(OBJS) $(LDLIBS)

$(BUILD)/libale.a: $(OBJS)
	ar rcs $@ $(OBJS)

$(BUILD)/ale_dump: tools/ale_dump.c $(BUILD)/libale.a
	$(CC) $(ALE_CFLAGS) $(CFLAGS) $< $(BUILD)/libale.a $(LDLIBS) -o $@

$(BUILD)/test_ale: tests/test_ale.c $(BUILD)/libale.a
	$(CC) $(ALE_CFLAGS) $(CFLAGS) $< $(BUILD)/libale.a $(LDLIBS) -o $@

test: $(BUILD)/test_ale $(BUILD)/ale_dump
	$(BUILD)/test_ale vectors
	sh scripts/check_vectors.sh $(BUILD)/ale_dump

asan:
	$(MAKE) BUILD=build-asan CFLAGS="-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer" \
	    LDLIBS="-lm -fsanitize=address,undefined" test

ref:
	sh ref/build_ref.sh

vectors: ref $(BUILD)/ale_dump
	python3 scripts/make_vectors.py --ref ref/build --dump $(BUILD)/ale_dump --out vectors

clean:
	rm -rf build build-asan

.PHONY: all shared static test asan ref vectors clean
