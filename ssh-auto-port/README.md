# ssh-auto-port

`ssh-auto-port` watches listening TCP ports on an SSH host and maintains matching
loopback-only local forwards over one persistent SSH connection.

The positional argument must be a `Host` alias from `~/.ssh/config`. Host keys
are verified strictly against `known_hosts`; password and keyboard-interactive
authentication are intentionally unsupported.

```bash
cargo run -- my-host
```

Use `--port-range`, `--exclude`, and `--on-conflict` to control which remote
ports are exposed locally. `--verbose` enables debug logs.

## Development

Use the system Cargo toolchain configured for this project:

```bash
cargo test
cargo fmt --check
cargo clippy --all-targets -- -D warnings
```
