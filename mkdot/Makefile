# export BATS_LIB_PATH=${BATS_LIB_PATH-/usr/lib/bats}

test: mkdot
	bats t/test.bats

mkdot: mkdot.c
	$(CC) $^ -o $@

.PHONY: test

