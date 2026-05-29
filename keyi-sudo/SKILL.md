---
name: keyi-sudo
description: >
  Use keyi as a non-interactive sudo replacement and bootstrap it on
  authorized SSH targets.
---

# keyi sudo replacement

Use `keyi` when an authorized Linux target needs root privileges and the
agent should avoid interactive `sudo` prompts.

This skill is only for trusted, isolated, authorized targets. Do not use it
on production systems, shared hosts, or systems where the user has not
explicitly authorized privileged access.

## Scope

Use `keyi` for basic non-interactive CLI privilege elevation:

```bash
keyi COMMAND [ARG]...
```

Do not use interactive flows by default. Avoid `sudo -i`, `sudoedit`,
`sudo -e`, `keyi -i`, and `keyi -e` unless the user explicitly asks and
the agent can safely handle the interaction.

## Replace sudo

When `keyi` is installed and verified on the target, replace simple
`sudo` commands with `keyi`:

```bash
sudo COMMAND [ARG]...
keyi COMMAND [ARG]...
```

```bash
sudo NAME=VALUE COMMAND [ARG]...
keyi NAME=VALUE COMMAND [ARG]...
```

```bash
sudo sh -c 'COMMAND'
keyi sh -c 'COMMAND'
```

Examples:

```bash
keyi id -u
keyi whoami
keyi apt update
keyi systemctl restart nginx
keyi chown root:root /path/to/file
```

For shell redirection, keep the redirection inside the elevated shell:

```bash
keyi sh -c 'printf "%s\n" value > /etc/example.conf'
```

`keyi` starts from a clean root environment. Pass required variables as
`NAME=VALUE` arguments before the command.

After `keyi` is installed and verified, do not run `sudo` unless the user
specifically asks for it or `keyi` is broken.

## Verify keyi

Before relying on `keyi`, verify that it exists and elevates to root:

```bash
KEYI="$(command -v keyi || command -v /usr/local/bin/keyi)"
"$KEYI" id -u
"$KEYI" whoami
```

Expected output includes:

```text
0
root
```

If `keyi` is not in `PATH`, prefer the absolute path:

```bash
/usr/local/bin/keyi id -u
```

## SSH bootstrap inputs

If `keyi` is not installed on a remote target, the user may provide the SSH
connection command and sudo password in natural language, for example:

```text
Use ssh -p 2222 dev@10.0.0.5.
The sudo password is hunter2.
```

Interpret these as conceptual placeholders:

- `<SSH>` is the full SSH command from the user's message.
- Example: `ssh -p 2222 dev@10.0.0.5`.
- `<SUDO_PASSWORD>` is the sudo password from the user's message.

They are not required to be environment variables. Extract them from the
conversation. If either value is missing or ambiguous, ask one short
clarifying question before bootstrapping.

Treat `<SUDO_PASSWORD>` as a secret:

- Do not print it.
- Do not write it to files.
- Do not include it in summaries.
- Feed it to `sudo -S` through stdin only when needed.
- After bootstrap succeeds, stop using the password and use `keyi`.

## SSH bootstrap process

Use this process when the current local directory is the `keyi` source tree
and the remote target has authorized SSH and sudo access.

First, check the target and whether `keyi` already exists:

```bash
<SSH> '
uname -a
id
command -v keyi || command -v /usr/local/bin/keyi || true
'
```

Copy the source tree to the target without build artifacts:

```bash
tar --exclude=.git --exclude=build -czf - . \
  | <SSH> '
rm -rf "$HOME/keyi-sudo"
mkdir -p "$HOME/keyi-sudo"
tar -xzf - -C "$HOME/keyi-sudo"
'
```

Build on the target:

```bash
<SSH> '
cmake \
  -S "$HOME/keyi-sudo" \
  -B "$HOME/keyi-sudo/build" \
  -DCMAKE_BUILD_TYPE=Release
cmake --build "$HOME/keyi-sudo/build"
'
```

Install and configure `keyi` with setuid-root. The example below shows
`<SUDO_PASSWORD>` as a placeholder. Replace it internally with the password
extracted from the user's natural-language message, without printing it:

```bash
<SSH> '
set -eu
read -r sudo_pass
repo="$HOME/keyi-sudo"
keyi_group="$(id -gn)"

printf "%s\n" "$sudo_pass" |
  sudo -S -p "" cmake --install "$repo/build"
printf "%s\n" "$sudo_pass" |
  sudo -S -p "" chown root:"$keyi_group" /usr/local/bin/keyi
printf "%s\n" "$sudo_pass" |
  sudo -S -p "" chmod 4750 /usr/local/bin/keyi
unset sudo_pass
' <<'KEYI_SUDO_PASSWORD'
<SUDO_PASSWORD>
KEYI_SUDO_PASSWORD
```

Verify the installed binary:

```bash
<SSH> '
/usr/local/bin/keyi id -u
/usr/local/bin/keyi whoami
ls -l /usr/local/bin/keyi
'
```

Expected output includes:

```text
0
root
```

After verification, use `keyi` for future privileged remote commands:

```bash
<SSH> '/usr/local/bin/keyi COMMAND [ARG]...'
```

## Installing missing build dependencies

If the remote target lacks `cmake`, a C compiler, or build tools, use the
provided sudo password only to install those dependencies. Then continue the
bootstrap.

Common package commands:

```bash
# Debian/Ubuntu
apt update
apt install -y cmake build-essential

# Alpine
apk add cmake build-base

# Fedora/RHEL-like
dnf install -y cmake gcc make
```

Example for Debian/Ubuntu through SSH:

```bash
<SSH> '
set -eu
read -r sudo_pass
printf "%s\n" "$sudo_pass" | sudo -S -p "" apt update
printf "%s\n" "$sudo_pass" |
  sudo -S -p "" apt install -y cmake build-essential
unset sudo_pass
' <<'KEYI_SUDO_PASSWORD'
<SUDO_PASSWORD>
KEYI_SUDO_PASSWORD
```

Run the needed package commands through `sudo -S` using the same stdin
pattern used in the bootstrap. Do not persist scripts or command history
that contain the password.

## Troubleshooting

`operation requires root EUID` means the binary is not running with
effective UID 0. Check that `/usr/local/bin/keyi` is owned by root, has the
setuid bit, and is not on a filesystem mounted with `nosuid`.

`other permissions must not be set` means the binary has unsafe world
permissions. Fix the mode:

```bash
chmod 4750 /usr/local/bin/keyi
```

`Permission denied` when executing `keyi` usually means the SSH user is not
in the binary's group. Set the group to a narrow group that includes the
SSH user, then keep mode `4750`.

`sudo: a password is required` or `Sorry, try again` means the extracted
password is missing or wrong. Ask the user for the correct sudo password
instead of guessing.

If `keyi id -u` does not print `0`, do not use `keyi` for privileged work
until the installation is fixed.
