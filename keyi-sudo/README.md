# keyi

`keyi` is a small tool for running commands as root.

It is built to stay simple:

- simple to use, with no user config
- small codebase (less than 500 lines)
- clear and safe permission model
- supports sudoedit-style edit mode

Access is based on normal Unix owner, group, and setuid bits.
There is no policy file and no password prompt.
If a user can run `keyi`, no extra setup is needed.

For build and install notes, see [CONTRIBUTING.md](CONTRIBUTING.md).

## Usage

Run a command as root:

```bash
$ keyi id -u
0
```

Pass environment variables to the command:

```bash
$ keyi FOO=BAR printenv FOO
BAR
```

Edit a file:

```bash
$ keyi -e /etc/apk/world
```

Open a root shell:

```bash
$ keyi -i
```
