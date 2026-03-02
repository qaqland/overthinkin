TARGET ?= resize-test
CC ?= cc
PKG_CONFIG ?= pkg-config

SDL3_CFLAGS := $(shell $(PKG_CONFIG) --cflags sdl3 2>/dev/null)
SDL3_LIBS := $(shell $(PKG_CONFIG) --libs sdl3 2>/dev/null)
HAVE_SDL3 := $(strip $(SDL3_CFLAGS)$(SDL3_LIBS))

CFLAGS ?= -Wall -Wextra -Werror

.PHONY: all test clean

ifeq ($(HAVE_SDL3),)
all test:
	@echo "skip $@: missing sdl3 development package"
else
all: $(TARGET)

test: $(TARGET)

$(TARGET): resize.c
	$(CC) $(CFLAGS) $(SDL3_CFLAGS) -o $@ $< $(SDL3_LIBS)
endif

clean:
	rm -f $(TARGET)
