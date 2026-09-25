# SPDX-License-Identifier: GPL-2.0-only
# Copyright (C) 2026 Ctrl IQ, Inc.
#
# Normal, sanitizer, and AFL builds use separate object directories so objects
# compiled with different flags cannot be mixed.

CC ?= cc
PYTHON ?= python3
export PYTHON

# V=1 prints full commands; otherwise, show the build action and target
ifeq ($(V),1)
Q :=
show =
else
Q := @
show = @printf '  %-9s %s\n' '[$(1)]' '$(subst ','"'"',$(2))'
endif

# make -s must also suppress the action labels
ifneq ($(findstring s,$(firstword -$(MAKEFLAGS))),)
show =
endif

INSTALL = install
INSTALL_PROGRAM = $(INSTALL)
prefix = /usr/local
exec_prefix = $(prefix)
bindir = $(exec_prefix)/bin

# AFL builds use the same sources and flags with an instrumenting compiler
AFL_CC ?= afl-clang-fast

# A dependency prefix may also contain liburcu, so prefer the bundled headers
override CPPFLAGS := -Isrc/include -Ivendor $(CPPFLAGS)

# Keep required flags even under make -e. User flags can override optimization,
# but -funsigned-char must remain last: byte comparisons and ctype calls
# throughout the program rely on unsigned char.
override CFLAGS := -std=gnu17 -O2 -g -Wall -Wextra -Werror \
	-Wdeclaration-after-statement $(CFLAGS) -funsigned-char

# Sanitizer instrumentation requires the flags at both compilation and linking
ASAN_FLAGS := -fsanitize=address,undefined -fno-omit-frame-pointer

TOOL_SRCS := main.c assemble.c cli.c display.c gitread.c gittree.c highlight.c \
	html.c iomem.c patch.c reader.c render.c review.c treediff.c udiff.c util.c
DRIVER_SRCS := iomem.c reader.c udiff.c util.c

# Object-reader tests link the tree differ and patch assembler without the CLI.
# Both object access and line comparison require libgit2.
GITREAD_DRIVER_SRCS := assemble.c gitread.c iomem.c treediff.c udiff.c util.c
GIT2_LDLIBS := -lgit2

# Rebuild on any header change, including indirect dependencies, rather than
# maintaining a separate dependency list for every object.
HEADERS := $(wildcard src/include/*.h) \
	vendor/urcu/compiler.h vendor/urcu/list.h

build/html-assets.h: scripts/embed-assets.py src/html.css src/html.js src/html-controls.html
	$(call show,GEN,$@)
	$(Q)"$(PYTHON)" scripts/embed-assets.py $@ src/html.css src/html.js src/html-controls.html

build/plain/html.o build/asan/html.o build/afl/html.o: build/html-assets.h

PLAIN_TOOL_OBJS := $(TOOL_SRCS:%.c=build/plain/%.o)
PLAIN_DRIVER_OBJS := $(DRIVER_SRCS:%.c=build/plain/%.o)
PLAIN_GITREAD_OBJS := $(GITREAD_DRIVER_SRCS:%.c=build/plain/%.o)
ASAN_TOOL_OBJS := $(TOOL_SRCS:%.c=build/asan/%.o)
ASAN_DRIVER_OBJS := $(DRIVER_SRCS:%.c=build/asan/%.o)
ASAN_GITREAD_OBJS := $(GITREAD_DRIVER_SRCS:%.c=build/asan/%.o)
AFL_TOOL_OBJS := $(TOOL_SRCS:%.c=build/afl/%.o)

PLAIN_BINS := diffofdiffs
ASAN_BINS := diffofdiffs-asan
AFL_BINS := diffofdiffs-afl

# Each spec selects a support binary or the tool. The runner resolves that name
# to the normal or sanitizer executable for its target.
TEST_SPECS := $(wildcard tests/*/spec)
TEST_DIRS := $(patsubst tests/%/spec,%,$(TEST_SPECS))
PLAIN_RUN_GOALS := $(addprefix run-,$(TEST_DIRS))
ASAN_RUN_GOALS := $(addprefix run-asan-,$(TEST_DIRS))

# Run timeout fixtures serially to avoid resource contention. Derive membership
# from the specs, and guard the empty list so grep cannot wait for stdin.
TIMEOUT_SPECS := $(if $(TEST_SPECS),$(shell grep -l '^timeout:' $(TEST_SPECS) 2>/dev/null))
# Make's bytewise sort gives the dependency chain the same order in every locale
TIMEOUT_DIRS := $(sort $(patsubst tests/%/spec,%,$(TIMEOUT_SPECS)))
TIMEOUT_N := $(words $(TIMEOUT_DIRS))

UDIFF_DRIVER_BINS := tests/support/udiff_driver tests/support/udiff_driver-asan
GITREAD_UNIT_BINS := tests/support/gitread-unit tests/support/gitread-unit-asan
MATCH_DRIVER_BINS := tests/support/match-driver tests/support/match-driver-asan
HIGHLIGHT_DRIVER_BINS := tests/support/highlight-driver tests/support/highlight-driver-asan
HIGHLIGHT_DRIVER_OBJS := display.o highlight.o iomem.o udiff.o util.o

# This shared preload configures the tested process's dumpability and signal
# disposition. It is test setup rather than code under test.
TOOL_SETUP_LIB := tests/support/tool-setup.so

.DEFAULT_GOAL := all
.PHONY: all install uninstall clean check check-asan \
	$(PLAIN_RUN_GOALS) $(ASAN_RUN_GOALS)

all: $(PLAIN_BINS)

install: diffofdiffs
	$(call show,MKDIR,$(DESTDIR)$(bindir))
	$(Q)$(INSTALL) -d "$(DESTDIR)$(bindir)"
	$(call show,INSTALL,$(DESTDIR)$(bindir)/diffofdiffs)
	$(Q)$(INSTALL_PROGRAM) diffofdiffs "$(DESTDIR)$(bindir)/diffofdiffs"

uninstall:
	$(call show,RM,$(DESTDIR)$(bindir)/diffofdiffs)
	$(Q)$(RM) "$(DESTDIR)$(bindir)/diffofdiffs"

diffofdiffs: $(PLAIN_TOOL_OBJS)
	$(call show,LD,$@)
	$(Q)$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(GIT2_LDLIBS)

diffofdiffs-asan: $(ASAN_TOOL_OBJS)
	$(call show,LD,$@)
	$(Q)$(CC) $(CFLAGS) $(ASAN_FLAGS) $(LDFLAGS) -o $@ $^ $(GIT2_LDLIBS)

diffofdiffs-afl: $(AFL_TOOL_OBJS)
	$(call show,LD,$@)
	$(Q)$(AFL_CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(GIT2_LDLIBS)

tests/support/highlight-driver: tests/support/highlight-driver.c $(HEADERS) $(addprefix build/plain/,$(HIGHLIGHT_DRIVER_OBJS))
	$(call show,LD,$@)
	$(Q)$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ $< $(addprefix build/plain/,$(HIGHLIGHT_DRIVER_OBJS)) $(GIT2_LDLIBS)

tests/support/highlight-driver-asan: tests/support/highlight-driver.c $(HEADERS) $(addprefix build/asan/,$(HIGHLIGHT_DRIVER_OBJS))
	$(call show,LD,$@)
	$(Q)$(CC) $(CPPFLAGS) $(CFLAGS) $(ASAN_FLAGS) $(LDFLAGS) -o $@ $< $(addprefix build/asan/,$(HIGHLIGHT_DRIVER_OBJS)) $(GIT2_LDLIBS)

# Creating an object must wait for its directory, but a directory timestamp
# change must not trigger recompilation.
build/plain/%.o: src/%.c $(HEADERS) | build/plain
	$(call show,CC,$@)
	$(Q)$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

build/asan/%.o: src/%.c $(HEADERS) | build/asan
	$(call show,CC,$@)
	$(Q)$(CC) $(CPPFLAGS) $(CFLAGS) $(ASAN_FLAGS) -c -o $@ $<

build/afl/%.o: src/%.c $(HEADERS) | build/afl
	$(call show,CC,$@)
	$(Q)$(AFL_CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

build/plain build/asan build/afl:
	$(call show,MKDIR,$@)
	$(Q)mkdir -p $@

# Explicit target lists let these pattern rules remain phony and keep run-asan-*
# out of the normal run-* rule. The spec chooses its executable at runtime, so
# every target depends on its variant's full binary set.
#
# Each fixture waits for checkstyle even in a parallel make invocation.
$(PLAIN_RUN_GOALS): run-%: tests/%/spec $(PLAIN_BINS) tests/support/udiff_driver tests/support/gitread-unit tests/support/derive-patch $(TOOL_SETUP_LIB) | checkstyle
	$(Q)tests/run-one.sh tests/$*

$(ASAN_RUN_GOALS): run-asan-%: tests/%/spec $(ASAN_BINS) tests/support/udiff_driver-asan tests/support/gitread-unit-asan tests/support/derive-patch $(TOOL_SETUP_LIB) | checkstyle
	$(Q)tests/run-one.sh tests/$*

# Serialize timeout fixtures within each variant using order-only dependencies
ifneq ($(filter-out 0 1,$(TIMEOUT_N)),)
$(foreach i,$(shell seq 2 $(TIMEOUT_N)), \
	$(eval run-$(word $i,$(TIMEOUT_DIRS)): | run-$(word $(shell expr $i - 1),$(TIMEOUT_DIRS))) \
	$(eval run-asan-$(word $i,$(TIMEOUT_DIRS)): | run-asan-$(word $(shell expr $i - 1),$(TIMEOUT_DIRS))))
endif

# If both suites were requested, finish normal timeout fixtures before starting
# sanitizer timeout fixtures. Repeated copies of one goal do not count as both.
ifeq (2,$(words $(sort $(filter check check-asan,$(MAKECMDGOALS)))))
ifneq ($(TIMEOUT_DIRS),)
run-asan-$(firstword $(TIMEOUT_DIRS)): | run-$(lastword $(TIMEOUT_DIRS))
endif
endif

# Support binaries share production objects and stay out of the all target
build/plain/support/%.o: tests/support/%.c $(HEADERS) | build/plain/support
	$(call show,CC,$@)
	$(Q)$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

build/asan/support/%.o: tests/support/%.c $(HEADERS) | build/asan/support
	$(call show,CC,$@)
	$(Q)$(CC) $(CPPFLAGS) $(CFLAGS) $(ASAN_FLAGS) -c -o $@ $<

build/plain/support build/asan/support:
	$(call show,MKDIR,$@)
	$(Q)mkdir -p $@

# A standalone driver for the line comparison API
tests/support/udiff_driver: $(PLAIN_DRIVER_OBJS) build/plain/support/udiff-driver.o
	$(call show,LD,$@)
	$(Q)$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(GIT2_LDLIBS)

tests/support/udiff_driver-asan: $(ASAN_DRIVER_OBJS) build/asan/support/udiff-driver.o
	$(call show,LD,$@)
	$(Q)$(CC) $(CFLAGS) $(ASAN_FLAGS) $(LDFLAGS) -o $@ $^ $(GIT2_LDLIBS)

tests/support/match-driver: build/plain/iomem.o build/plain/udiff.o build/plain/util.o build/plain/support/match-driver.o
	$(call show,LD,$@)
	$(Q)$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(GIT2_LDLIBS)

tests/support/match-driver-asan: build/asan/iomem.o build/asan/udiff.o build/asan/util.o build/asan/support/match-driver.o
	$(call show,LD,$@)
	$(Q)$(CC) $(CFLAGS) $(ASAN_FLAGS) $(LDFLAGS) -o $@ $^ $(GIT2_LDLIBS)

.PHONY: check-matches check-matches-asan
check-matches: tests/support/match-driver
	$(Q)"$(PYTHON)" tests/check-matches.py $<

check-matches-asan: tests/support/match-driver-asan
	$(Q)"$(PYTHON)" tests/check-matches.py $<

# Libraries follow objects so static linking sees the references first
tests/support/derive-patch: $(PLAIN_GITREAD_OBJS) build/plain/support/derive-patch.o
	$(call show,LD,$@)
	$(Q)$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(GIT2_LDLIBS)

tests/support/gitread-unit: $(PLAIN_GITREAD_OBJS) build/plain/support/gitread-unit.o
	$(call show,LD,$@)
	$(Q)$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(GIT2_LDLIBS)

tests/support/gitread-unit-asan: $(ASAN_GITREAD_OBJS) build/asan/support/gitread-unit.o
	$(call show,LD,$@)
	$(Q)$(CC) $(CFLAGS) $(ASAN_FLAGS) $(LDFLAGS) -o $@ $^ $(GIT2_LDLIBS)

$(TOOL_SETUP_LIB): tests/support/tool-setup.c
	$(call show,LD,$@)
	$(Q)$(CC) $(CPPFLAGS) $(CFLAGS) -shared -fPIC $(LDFLAGS) -o $@ $<

tests/support/no-locale.so: tests/support/no-locale.c src/include/util.h
	$(call show,LD,$@)
	$(Q)$(CC) $(CPPFLAGS) $(CFLAGS) -shared -fPIC $(LDFLAGS) -o $@ $<

# clang-format checks layout when installed. The remaining checks enforce rules
# it cannot express; keep the formatter optional for the correctness suites.
STYLE_SRCS := $(wildcard src/*.c) $(wildcard src/include/*.h) $(wildcard tests/support/*.c)

# Only types.h declares the short integer aliases. Passing /dev/null to checks
# of filtered lists prevents grep from waiting for stdin when a list is empty.
STYLE_STDINT_SRCS := $(filter-out src/include/types.h,$(STYLE_SRCS))

# Raw allocation calls belong only in their wrappers in util.c
STYLE_ALLOC_SRCS := $(filter-out src/util.c,$(STYLE_SRCS))

.PHONY: checkstyle
# Empty file lists must not make grep or clang-format wait for stdin
ifeq ($(STYLE_SRCS),)
checkstyle:
	$(Q)echo "checkstyle: no sources found, skipping"
else
checkstyle:
	$(Q)"$(PYTHON)" tests/check-comments.py $(STYLE_SRCS)
	$(Q)if command -v clang-format >/dev/null 2>&1; then \
		clang-format --dry-run -Werror $(STYLE_SRCS); \
	else \
		echo "checkstyle: clang-format not found, skipping the format check"; \
	fi
	$(Q)! grep -n 'unsigned char' $(STYLE_SRCS) || { \
		echo "checkstyle: unsigned char is banned; plain char is unsigned and numeric bytes are u8"; \
		exit 1; }
	$(Q)! grep -nE '\bu?int(8|16|32|64)_t\b' $(STYLE_STDINT_SRCS) /dev/null || { \
		echo "checkstyle: the long stdint spellings live only in src/include/types.h"; \
		exit 1; }
	$(Q)! grep -nE '/\*[[:space:]]*[^ *]' $(STYLE_SRCS) | grep -v '\*/' || { \
		echo "checkstyle: a multi-line comment opens with /* alone on its line"; \
		exit 1; }
	$(Q)! awk 'function w(s,    i, c, r) { \
			r = 0; \
			for (i = 1; i <= length(s); i++) { \
				c = substr(s, i, 1); \
				if (c == "\t") \
					r += 8 - r % 8; \
				else \
					r++; \
			} \
			return r; \
		} \
		FNR == 1 { inc = 0 } \
		!inc { \
			if ($$0 ~ /\/\*/ && $$0 !~ /\*\//) { \
				inc = 1; \
				pl = 0; \
			} \
			next; \
		} \
		/^[[:blank:]]*\*\/[[:blank:]]*$$/ { inc = 0; next } \
		{ \
			t = ""; \
			if (match($$0, /^[[:blank:]]*\* [^[:blank:]]/)) \
				t = substr($$0, RSTART + RLENGTH - 1); \
			if (t != "" && pl && \
			    t !~ /^(Copyright \(C\)|Author: )/) { \
				split(t, a, " "); \
				f = a[1]; \
				if (f != "-" && f !~ /^[0-9]+[.)]$$/ && \
				    w(pv) + 1 + length(f) <= 80) { \
					printf "%s:%d: %s\n", FILENAME, \
					       FNR - 1, pv; \
					bad = 1; \
				} \
			} \
			pl = (t != ""); \
			pv = $$0; \
		} \
		END { exit !bad }' $(STYLE_SRCS) || { \
		echo "checkstyle: the next word still fits the comment line" \
		     "above (see CODING_STYLE.md Comments)"; \
		exit 1; }
	$(Q)! awk 'FNR == 1 { inc = 0 } \
		!inc { \
			if ($$0 ~ /\/\*/ && $$0 !~ /\*\//) { \
				inc = 1; \
				last = ""; \
				lastln = 0; \
			} \
			next; \
		} \
		/^[[:blank:]]*\*\/[[:blank:]]*$$/ { \
			if (lastln && last !~ /\.[")]*$$/ && \
			    last !~ /^Author: .* <.*>$$/ && \
			    last !~ /^[[:blank:]]/) { \
				printf "%s:%d: %s\n", FILENAME, lastln, last; \
				bad = 1; \
			} \
			inc = 0; \
			next; \
		} \
		{ \
			if (match($$0, /^[[:blank:]]*\* /)) { \
				t = substr($$0, RSTART + RLENGTH); \
				sub(/[[:blank:]]+$$/, "", t); \
				if (t != "") { \
					last = t; \
					lastln = FNR; \
				} \
			} \
		} \
		END { exit !bad }' $(STYLE_SRCS) || { \
		echo "checkstyle: a multi-line comment ends in a period" \
		     "(see CODING_STYLE.md Comments)"; \
		exit 1; }
	$(Q)! grep -inE 'load.bearing|\bshape\b|adversar|attack|\bhostile\b|\bmalicious\b|\bevil\b' $(STYLE_SRCS) || { \
		echo "checkstyle: banned vocabulary (see CODING_STYLE.md Comments)"; \
		exit 1; }
	$(Q)! grep -nE '"(output buffer|record table|freed table|image payload|image line table|image charge|operand image|change script|cleanup action table|classifier (hash|class) table|reference index|matcher diagonal vectors|the (split patch|delta diff|context diff))"' $(STYLE_SRCS) || { \
		echo "checkstyle: failure reports must identify the requested operation (see CODING_STYLE.md Failure reports)"; \
		exit 1; }
	$(Q)! grep -nE '\b(malloc|calloc|realloc|reallocarray|strdup|strndup|asprintf|vasprintf|getline|getdelim|open_memstream|fmemopen)\(' \
		$(STYLE_ALLOC_SRCS) /dev/null | \
		grep -vE ':[[:space:]]* \*([[:space:]]|/|$$)' || { \
		echo "checkstyle: raw allocators are wrapped in src/util.c (see CODING_STYLE.md)"; \
		exit 1; }
	$(Q)! awk 'FNR == 1 { prev = "" } \
		prev ~ /,$$/ && /^[[:space:]]*\}/ { \
			printf "%s:%d: %s\n", FILENAME, FNR - 1, prev; bad = 1 } \
		{ prev = $$0 } END { exit !bad }' $(STYLE_SRCS) || { \
		echo "checkstyle: no trailing comma before a closing brace (see CODING_STYLE.md)"; \
		exit 1; }
	$(Q)viol=$$(for f in $(STYLE_SRCS); do \
		expand -t8 $$f | grep -n '.\{81\}' | grep -v '"' | sed "s|^|$$f:|"; \
	done); \
	if [ -n "$$viol" ]; then \
		echo "$$viol"; \
		echo "checkstyle: lines over 80 columns (tabs at 8)"; \
		exit 1; \
	fi
# Recognize function definitions by an unindented signature followed by '{'.
# Declarations, indented calls, and preprocessor directives do not qualify.
	$(Q)! awk 'function pdelta(s,    i, c, d) { \
			d = 0; \
			for (i = 1; i <= length(s); i++) { \
				c = substr(s, i, 1); \
				if (c == "(") d++; \
				else if (c == ")") d--; \
			} \
			return d; \
		} \
		FNR == 1 { st = 0 } \
		st == 2 { \
			s = $$0; \
			sub(/^[[:blank:]]*/, "", s); \
			if (substr(s, 1, 1) == "{") { \
				n = split(id, T, "_"); \
				hit = 0; \
				for (i = 1; i <= n; i++) { \
					if (T[i] ~ \
					    /^(destroy|release|clear|cleanup|dispose)$$/) \
						hit = 1; \
					if (T[i] == "free" && id !~ /_free$$/) \
						hit = 1; \
				} \
				if (hit) { \
					printf "%s:%d: %s\n", FILENAME, ln, id; \
					bad = 1; \
				} \
			} \
			st = 0; \
		} \
		st == 1 { \
			depth += pdelta($$0); \
			if (depth <= 0) \
				st = ($$0 ~ /;/) ? 0 : 2; \
			next; \
		} \
		st == 0 && /^[A-Za-z_]/ && /\(/ { \
			pre = $$0; \
			sub(/\(.*/, "", pre); \
			if (match(pre, /[A-Za-z_][A-Za-z0-9_]*$$/)) { \
				id = substr(pre, RSTART, RLENGTH); \
				ln = FNR; \
				depth = pdelta($$0); \
				if (depth > 0) \
					st = 1; \
				else if ($$0 !~ /;/) \
					st = 2; \
			} \
		} \
		END { exit !bad }' $(STYLE_SRCS) || { \
		echo "checkstyle: a teardown definition is named <type>_free" \
		     "(see CODING_STYLE.md Teardown naming)"; \
		exit 1; }
	$(Q)echo "checkstyle: clean"
endif

# Bind each test's variant on its run target. Timeout dependencies cross
# between the normal and sanitizer targets, and target-specific variables
# propagate to prerequisites. A binding on check/check-asan could therefore
# run a normal fixture with the sanitizer binary.
#
# Export on the assignment so make parses it as a target-specific variable,
# not as extra prerequisite names. Each run target keeps its own binding
# regardless of which aggregate target reaches it first. RUN_VARIANT selects
# timeout-asan and is checked against the binary by the runner.
$(PLAIN_RUN_GOALS): export DIFFOFDIFFS := $(CURDIR)/diffofdiffs
$(PLAIN_RUN_GOALS): export UDIFF_DRIVER := $(CURDIR)/tests/support/udiff_driver
$(PLAIN_RUN_GOALS): export GITREAD_UNIT := $(CURDIR)/tests/support/gitread-unit
$(PLAIN_RUN_GOALS): export TOOL_SETUP_SO := $(CURDIR)/$(TOOL_SETUP_LIB)
$(PLAIN_RUN_GOALS): export RUN_VARIANT := plain

$(ASAN_RUN_GOALS): export DIFFOFDIFFS := $(CURDIR)/diffofdiffs-asan
$(ASAN_RUN_GOALS): export UDIFF_DRIVER := $(CURDIR)/tests/support/udiff_driver-asan
$(ASAN_RUN_GOALS): export GITREAD_UNIT := $(CURDIR)/tests/support/gitread-unit-asan
$(ASAN_RUN_GOALS): export TOOL_SETUP_SO := $(CURDIR)/$(TOOL_SETUP_LIB)
$(ASAN_RUN_GOALS): export RUN_VARIANT := asan

check: checkstyle check-matches check-review check-highlight $(PLAIN_BINS) tests/support/udiff_driver tests/support/gitread-unit $(TOOL_SETUP_LIB) $(PLAIN_RUN_GOALS)

check-asan: checkstyle check-matches-asan check-review-asan check-highlight-asan $(ASAN_BINS) tests/support/udiff_driver-asan tests/support/gitread-unit-asan $(TOOL_SETUP_LIB) $(ASAN_RUN_GOALS)

clean:
	$(call show,CLEAN,build files)
	$(Q)rm -rf build $(PLAIN_BINS) $(ASAN_BINS) $(AFL_BINS) $(UDIFF_DRIVER_BINS) $(MATCH_DRIVER_BINS) $(GITREAD_UNIT_BINS) $(HIGHLIGHT_DRIVER_BINS) tests/support/derive-patch $(TOOL_SETUP_LIB) tests/support/no-locale.so

.PHONY: check-highlight check-highlight-asan
check-highlight: diffofdiffs tests/support/highlight-driver
	$(Q)"$(PYTHON)" tests/check-highlight.py ./diffofdiffs tests/support/highlight-driver

check-highlight-asan: diffofdiffs-asan tests/support/highlight-driver-asan
	$(Q)"$(PYTHON)" tests/check-highlight.py ./diffofdiffs-asan tests/support/highlight-driver-asan

.PHONY: check-review check-review-asan
check-review: diffofdiffs tests/support/derive-patch tests/support/gitread-unit tests/support/no-locale.so
	$(Q)"$(PYTHON)" tests/check-review.py ./diffofdiffs

check-review-asan: diffofdiffs-asan tests/support/derive-patch tests/support/gitread-unit tests/support/no-locale.so
	$(Q)"$(PYTHON)" tests/check-review.py ./diffofdiffs-asan
