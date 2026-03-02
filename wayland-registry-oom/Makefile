TARGET ?= oom
CC ?= cc
PKG_CONFIG ?= pkg-config

WAYLAND_CFLAGS := $(shell $(PKG_CONFIG) --cflags wayland-client 2>/dev/null)
WAYLAND_LIBS := $(shell $(PKG_CONFIG) --libs wayland-client 2>/dev/null)
HAVE_WAYLAND := $(strip $(WAYLAND_CFLAGS)$(WAYLAND_LIBS))

CFLAGS ?= -Wall -Wextra -Werror

.PHONY: all test clean

ifeq ($(HAVE_WAYLAND),)
all test:
	@echo "skip $@: missing wayland-client development package"
else
all: $(TARGET)

$(TARGET): oom.c
	$(CC) $(CFLAGS) $(WAYLAND_CFLAGS) -o $@ $< $(WAYLAND_LIBS) -lm

test: $(TARGET)
endif

clean:
	rm -f $(TARGET)
