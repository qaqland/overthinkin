CC ?= cc
PKG_CONFIG ?= pkg-config
BATS ?= bats
UNAME_S := $(shell uname -s)
HAVE_BATS := $(shell command -v $(BATS) >/dev/null 2>&1 && echo 1)

TARGET ?= keyi
TEST_KEYI := t/$(TARGET)

CFLAGS ?= -Wall -Wextra -g
LDFLAGS ?=

CUNIT_CFLAGS := $(shell $(PKG_CONFIG) --cflags cunit 2>/dev/null)
CUNIT_LIBS := $(shell $(PKG_CONFIG) --libs cunit 2>/dev/null)

.PHONY: all test test-c clean install

ifeq ($(UNAME_S),Linux)
all: $(TARGET)

$(TARGET): keyi.c
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $<
	chmod u+s,go-x $@

test: $(TARGET) test.bats
ifeq ($(HAVE_BATS),1)
	@set -e; \
	cp "$(TARGET)" "$(TEST_KEYI)"; \
	chmod u+s,go-x "$(TEST_KEYI)"; \
	trap 'rm -f "$(TEST_KEYI)"' EXIT; \
	$(BATS) test.bats
else
	@echo "skip test: bats is not installed"
endif

test-c: test.c keyi.c
	$(CC) $(CFLAGS) $(CUNIT_CFLAGS) $(LDFLAGS) -o $@ $< $(CUNIT_LIBS)
	./$@

install: $(TARGET)
	install -m 4750 -o root -g wheel $(TARGET) /usr/local/bin/
else
all test test-c install:
	@echo "skip $@: keyi-sudo is Linux-only (current: $(UNAME_S))"
endif

clean:
	rm -f test-c $(TARGET) $(TEST_KEYI)
	rm -f t/*.got
