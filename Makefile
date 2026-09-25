# SPDX-License-Identifier: GPL-2.0-only
# Copyright (C) 2026 Ctrl IQ, Inc.

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

# A dependency prefix may also contain liburcu, so prefer the bundled headers
override CPPFLAGS := -Isrc/include -Ivendor $(CPPFLAGS)
override CFLAGS := -std=gnu17 -O2 -g -Wall -Wextra -Werror -Wdeclaration-after-statement $(CFLAGS) -funsigned-char
TOOL_SRCS := $(notdir $(wildcard src/*.c))
HEADERS := $(wildcard src/include/*.h) vendor/urcu/compiler.h vendor/urcu/list.h
PLAIN_TOOL_OBJS := $(TOOL_SRCS:%.c=build/plain/%.o)
GIT2_LDLIBS := -lgit2

.DEFAULT_GOAL := all
.PHONY: all install uninstall clean

ifneq ($(wildcard src/main.c),)
all: diffofdiffs
else
all: $(PLAIN_TOOL_OBJS)
endif

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

build/plain/%.o: src/%.c $(HEADERS)
	$(call show,MKDIR,$@)
	$(Q)mkdir -p $(@D)
	$(call show,CC,$@)
	$(Q)$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

build/plain/support/%.o: tests/support/%.c $(HEADERS)
	$(call show,MKDIR,$@)
	$(Q)mkdir -p $(@D)
	$(call show,CC,$@)
	$(Q)$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

build/html-assets.h: scripts/embed-assets.py src/html.css src/html.js src/html-controls.html
	$(call show,GEN,$@)
	$(Q)"$(PYTHON)" scripts/embed-assets.py $@ src/html.css src/html.js src/html-controls.html

build/plain/html.o: build/html-assets.h

DRIVER_OBJS := $(addprefix build/plain/,iomem.o reader.o udiff.o util.o)
GITREAD_OBJS := $(addprefix build/plain/,assemble.o gitread.o iomem.o treediff.o udiff.o util.o)

tests/support/udiff_driver: $(DRIVER_OBJS) build/plain/support/udiff-driver.o
	$(call show,LD,$@)
	$(Q)$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(GIT2_LDLIBS)

tests/support/gitread-unit: $(GITREAD_OBJS) build/plain/support/gitread-unit.o
	$(call show,LD,$@)
	$(Q)$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(GIT2_LDLIBS)

tests/support/derive-patch: $(GITREAD_OBJS) build/plain/support/derive-patch.o
	$(call show,LD,$@)
	$(Q)$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(GIT2_LDLIBS)

tests/support/match-driver: build/plain/iomem.o build/plain/udiff.o build/plain/util.o build/plain/support/match-driver.o
	$(call show,LD,$@)
	$(Q)$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(GIT2_LDLIBS)

tests/support/highlight-driver: build/plain/display.o build/plain/highlight.o build/plain/iomem.o build/plain/udiff.o build/plain/util.o build/plain/support/highlight-driver.o
	$(call show,LD,$@)
	$(Q)$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(GIT2_LDLIBS)

tests/support/tool-setup.so: tests/support/tool-setup.c $(HEADERS)
	$(call show,LD,$@)
	$(Q)$(CC) $(CPPFLAGS) $(CFLAGS) -shared -fPIC $(LDFLAGS) -o $@ $<

tests/support/no-locale.so: tests/support/no-locale.c $(HEADERS)
	$(call show,LD,$@)
	$(Q)$(CC) $(CPPFLAGS) $(CFLAGS) -shared -fPIC $(LDFLAGS) -o $@ $<

clean:
	$(call show,CLEAN,build files)
	$(Q)rm -rf build diffofdiffs

.PHONY: check check-matches
check: check-matches

check-matches: tests/support/match-driver
	$(Q)"$(PYTHON)" tests/check-matches.py tests/support/match-driver
