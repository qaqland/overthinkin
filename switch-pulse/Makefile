SCRIPT := switch-pulse.sh

.PHONY: all test clean install

all: test

test: $(SCRIPT)
	bash -n $(SCRIPT)

install: $(SCRIPT)
	install -m 0755 $(SCRIPT) /usr/local/bin/switch-pulse

clean:
	@:
