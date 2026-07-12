# git-plain-clone

`git-plain-clone` is a small Go command-line tool that fetches a repository
snapshot over HTTP(S) Git smart protocol and writes a plain working tree.

It does not run `git clone` and does not create a `.git` directory. Use it
when you only need source files, not history, an index, or later Git
operations.

The implementation uses only the Go standard library. It supports repositories
whose native object format is either SHA-1 or SHA-256.

## Usage

```bash
./gitp [clone] [-q] [-b REF] URL [DIRECTORY]
```

`clone` is optional, so these are equivalent:

```bash
./gitp https://github.com/qaqland/bushi.git bushi
./gitp clone https://github.com/qaqland/bushi.git bushi
```

`-b, --branch` accepts a branch name or tag name. Full `refs/...` syntax is not
supported. When `stderr` is an interactive terminal, `gitp` shows lightweight
progress by default; `-q, --quiet` disables it.

## Build

```bash
go build -o gitp ./cmd/gitp
```

## Test

See [`tests/README.md`](tests/README.md) for how to run the integration tests
and use the compatibility check tool.

## Limitations

It is not a full `git clone` replacement. It currently does **not** support:

- SSH, git-daemon, or dumb HTTP.
- Authentication.
- Submodule initialization.
- A local object database.
- Incremental fetch after the first checkout.
- Git porcelain operations such as merge, pull, or status.
- Cross-format translation between SHA-1 and SHA-256 repositories.

Each clone session uses the single object format advertised by the remote
repository.

The output directory must either not exist or already be empty.

## License

MIT License.
