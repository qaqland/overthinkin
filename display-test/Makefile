TARGET ?= display-test
VERSION ?= 0.0.2
CC ?= cc
PKG_CONFIG ?= pkg-config

SDL2_CFLAGS := $(shell $(PKG_CONFIG) --cflags sdl2 2>/dev/null)
SDL2_LIBS := $(shell $(PKG_CONFIG) --libs sdl2 2>/dev/null)

CFLAGS ?= -Wall -Wextra -Werror -std=c99
CPPFLAGS += -DVERSION=\"$(VERSION)\"

.PHONY: all test clean install

all: $(TARGET)

test: $(TARGET)

$(TARGET): main.c
	$(CC) $(CFLAGS) $(CPPFLAGS) $(SDL2_CFLAGS) -o $@ $< $(SDL2_LIBS)

install:
	@echo "Please move $(TARGET) to your PATH manually."

clean:
	rm -f $(TARGET)
