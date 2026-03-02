TARGET ?= mkdot
CC ?= cc
BATS ?= bats
TEST_BIN := t/fixtures/$(TARGET)
HAVE_BATS := $(shell command -v $(BATS) >/dev/null 2>&1 && echo 1)

CFLAGS ?= -Wall -Wextra -Werror -std=c99

.PHONY: all test clean
.PHONY: test-smoke

all: $(TARGET)

$(TARGET): mkdot.c
	$(CC) $(CFLAGS) -o $@ $<

test: test-smoke
ifeq ($(HAVE_BATS),1)
	@set -e; \
	cp "$(TARGET)" "$(TEST_BIN)"; \
	trap 'rm -f "$(TEST_BIN)"' EXIT; \
	$(BATS) t/test.bats
else
	@echo "skip test: bats is not installed"
endif

test-smoke: $(TARGET)
	@set -e; \
	tmp1="$$(mktemp -d)"; \
	tmp2="$$(mktemp -d)"; \
	trap 'rm -rf "$$tmp1" "$$tmp2"' EXIT; \
	( cd t/fixtures && ../../$(TARGET) -t "$$tmp1" my-topic >/dev/null ); \
	[ -f "$$tmp1/my-config" ]; \
	( cd t/fixtures && ../../$(TARGET) -s -t "$$tmp2" my-topic >/dev/null ); \
	[ -L "$$tmp2/my-config" ]; \
	( cd t/fixtures && ../../$(TARGET) -t "$$tmp1" dot-topic >/dev/null ); \
	[ -f "$$tmp1/dot-" ]; \
	[ -f "$$tmp1/.vimrc" ]

clean:
	rm -f $(TARGET) $(TEST_BIN)
