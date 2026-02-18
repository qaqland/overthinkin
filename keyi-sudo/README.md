# keyi

keyi 可以 - unsafe(\*not rust, no passwd) sudo/doas

Allow users in a specific group to execute a command as root.

## Usage

```bash
# suid
$ keyi id -u
0
```

```bash
# env
$ keyi FOO=BAR printenv FOO
BAR
```

```bash
# edit
$ keyi -e /etc/apk/world
```

```bash
# shell
$ keyi -i
```

