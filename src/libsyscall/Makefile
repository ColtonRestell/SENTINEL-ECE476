# src/libsyscall — Lab 1.
#
#   make         build $(BUILD)/libsyscall.a and $(BUILD)/demo_lab1
#   make test    build and run each tests/test_*.c; stop at the first failure
#   make clean   remove only the files that this Makefile built
#
# The top-level Makefile gives CC, CFLAGS and BUILD. The defaults below make
# this file work alone too, after one run of make at the top level, which
# builds $(BUILD)/libcommon.a. This file uses that library, and never builds
# or removes it.

CC     ?= gcc
CFLAGS ?= -std=c11 -D_GNU_SOURCE -Wall -Wextra -Werror -g -O1
BUILD  ?= $(abspath ../../build)

CPPFLAGS := -Iinclude -I../../common
# A .d file for each object, so that a change to a header rebuilds each
# object that includes it.
DEPFLAGS := -MMD -MP

OBJDIR := $(BUILD)/libsyscall
LIB    := $(BUILD)/libsyscall.a
COMMON := $(BUILD)/libcommon.a
DEMO   := $(BUILD)/demo_lab1

SRCS   := $(wildcard src/*.c)
OBJS   := $(patsubst src/%.c,$(OBJDIR)/%.o,$(SRCS))
TESTS  := $(patsubst tests/%.c,$(OBJDIR)/%,$(wildcard tests/test_*.c))
DEPS   := $(OBJS:.o=.d) $(addsuffix .d,$(TESTS)) $(OBJDIR)/demo_lab1.d

# The linker reads from left to right, and keeps only the objects that an
# earlier file needs. libsyscall comes first, because a program calls it, and
# libcommon after it.
LDLIBS := $(LIB) $(COMMON) -lm

.PHONY: all test clean

all: $(LIB) $(DEMO)

$(LIB): $(OBJS)
	rm -f $@
	ar rcs $@ $^

$(OBJDIR)/%.o: src/%.c | $(OBJDIR)
	$(CC) $(CFLAGS) $(CPPFLAGS) $(DEPFLAGS) -c -o $@ $<

$(DEMO): tests/demo_lab1.c $(LIB) $(COMMON) | $(OBJDIR)
	$(CC) $(CFLAGS) $(CPPFLAGS) $(DEPFLAGS) -MF $(OBJDIR)/demo_lab1.d $(LDFLAGS) -o $@ $< $(LDLIBS)

$(OBJDIR)/test_%: tests/test_%.c $(LIB) $(COMMON) | $(OBJDIR)
	$(CC) $(CFLAGS) $(CPPFLAGS) $(DEPFLAGS) $(LDFLAGS) -o $@ $< $(LDLIBS)

test: $(TESTS)
	@for t in $(TESTS); do \
		echo "  RUN   $$(basename $$t)"; \
		$$t || { echo "  FAIL  $$(basename $$t)"; exit 1; }; \
		echo "  PASS  $$(basename $$t)"; \
	done

$(COMMON):
	@echo "$@ does not exist. Run make at the top of the repository first."
	@exit 1

$(OBJDIR):
	mkdir -p $@

clean:
	rm -f $(OBJS) $(DEPS) $(TESTS) $(LIB) $(DEMO)
	rmdir $(OBJDIR) 2>/dev/null || true

-include $(DEPS)
