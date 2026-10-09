# ssh-auto-port

`ssh-auto-port` watches listening IPv4 TCP ports on an SSH host and maintains
matching loopback-only local forwards over one persistent SSH connection.
The remote host must provide Linux `/proc/net/tcp`, which is read immediately
on connection and then every 2 seconds.
IPv6 listeners are not discovered or forwarded.

The local client runs on Linux and Windows (ssh-agent: `SSH_AUTH_SOCK` on
Unix, the `\\.\pipe\openssh-ssh-agent` named pipe on Windows).

The positional argument must be a `Host` alias from `~/.ssh/config`. Host keys
are verified strictly against `known_hosts`; password and keyboard-interactive
authentication are intentionally unsupported. Private key files must be
unencrypted; encrypted files are skipped without prompting. SSH-agent keys
remain supported.

```bash
cargo run -- my-host
```

Use `--port-range` and `--exclude` to control which remote ports are exposed
locally. Occupied local ports are remapped to the nearest free port by default;
use `--skip` to skip them instead. `--debug` enables debug logs.

## Development

Use the system Cargo toolchain configured for this project:

```bash
cargo test
cargo fmt --check
cargo clippy --all-targets -- -D warnings
```
