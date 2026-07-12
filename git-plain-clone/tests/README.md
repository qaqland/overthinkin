# Testing

Integration tests live in this directory.

## Run

Build the required binaries and run the tests:

```bash
go build -o build/gitp ./cmd/gitp
go build -o build/compatcheck compatcheck.go
go build -o build/gitserver gitserver.go
go test ./tests/...
```

Tests hard-code the paths `../build/gitp`, `../build/compatcheck`, and
`../build/gitserver`. If a binary is missing, the test fails with the build
command.

## Compatibility check

`compatcheck.go` compares `gitp` output against the official `git clone` output:

```bash
go build -o build/gitp ./cmd/gitp
go build -o build/compatcheck compatcheck.go
./build/compatcheck https://github.com/qaqland/bushi.git
```

`compatcheck` looks for `gitp` in the same directory as itself, so both binaries
should be built into `build/` together.

It clones the repository with both tools and compares gitp's working tree
against the official git clone's index using `GIT_DIR`/`GIT_WORK_TREE`. Use it
to verify behavior on a specific repository or platform before reporting an
issue.

## Local server

`gitserver.go` is a standalone smart-HTTP server used by the tests. It can also
be run manually:

```bash
go build -o build/gitserver gitserver.go
./build/gitserver --root /path/to/bare/repos --timeout 30s
```

The server prints its base URL on stdout and exits automatically after the
timeout.
