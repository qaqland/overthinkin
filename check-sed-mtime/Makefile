TARGET ?= check_sed_mtime
CC ?= cc
UNAME_S := $(shell uname -s)

CFLAGS ?= -Wall -Wextra -Werror -std=c99

.PHONY: all test clean

all: $(TARGET)

$(TARGET): check_sed_mtime.c
	$(CC) $(CFLAGS) -o $@ $<

test: $(TARGET) check_sed_mtime.sh
ifeq ($(UNAME_S),Linux)
	./$(TARGET) 20 >/dev/null
	./check_sed_mtime.sh 20 >/dev/null
else
	@echo "skip test on $(UNAME_S): check-sed-mtime requires GNU sed/stat behavior"
endif

clean:
	rm -f $(TARGET)
