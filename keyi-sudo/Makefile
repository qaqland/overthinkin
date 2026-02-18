
CFLAGS	= -Wall -Wextra -g
LDFLAGS	=

CFLAGS	+= $(shell pkgconf --cflags cunit)
LDLIBS	+= $(shell pkgconf --libs cunit)

all: test

test: test.c keyi.c
	$(CC) $(CFLAGS) $(LDFLAGS) $< $(LDLIBS) -o $@
	./test

keyi: keyi.c
	cc -g $< -o $@
	chmod u+s,o-x $@

install: keyi
	install -m 4750 -o root -g wheel keyi /usr/local/bin/

clean:
	rm -f test

.PHONY: all clean

