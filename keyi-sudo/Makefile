
CFLAGS	= -Wall -Wextra -g
LDFLAGS	=

CFLAGS	+= $(shell pkgconf --cflags cunit)
LDLIBS	+= $(shell pkgconf --libs cunit)

all: test

test: keyi test.bats
	bats test.bats

test-c: test.c keyi.c
	$(CC) $(CFLAGS) $(LDFLAGS) $< $(LDLIBS) -o $@
	./$@

keyi: keyi.c
	$(CC) $(CFLAGS) -g $< -o $@
	chmod u+s,o-x $@

install: keyi
	install -m 4750 -o root -g wheel keyi /usr/local/bin/

clean:
	rm -f test keyi
	rm t/*.got

.PHONY: all clean test

