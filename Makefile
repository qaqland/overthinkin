TOOLS := \
	check-sed-mtime \
	display-test \
	keyi-sudo \
	mkdot \
	resize-test \
	switch-pulse \
	wayland-registry-oom

.PHONY: all test clean help

help:
	@printf "Targets:\n"
	@printf "  all   Build every tool directory\n"
	@printf "  test  Run each tool's test target\n"
	@printf "  clean Clean build outputs in every tool directory\n"

all test clean:
	@set -e; \
	for d in $(TOOLS); do \
		printf "\n==> [%s] %s\n" "$$d" "$@"; \
		$(MAKE) -C "$$d" "$@"; \
	done
