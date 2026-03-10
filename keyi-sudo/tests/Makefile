
CFLAGS	= -Wall -Wextra -g
LDFLAGS	=

CFLAGS	+= $(shell pkgconf --cflags cunit)
LDLIBS	+= $(shell pkgconf --libs cunit)

all: integration-test unit-test

integration-test: keyi integration.bats
	bats integration.bats

unit-test: unit-test.c ../keyi.c
	$(CC) $(CFLAGS) $(LDFLAGS) $< $(LDLIBS) -o $@
	./$@

keyi: ../keyi.c
	$(CC) $(CFLAGS) -g $< -o $@
	chmod u+s,o-x $@

install: keyi
	install -m 4750 -o root -g wheel keyi /usr/local/bin/

clean:
	rm -f unit-test keyi
	rm -f fixtures/*.got

.PHONY: all clean integration-test unit-test

