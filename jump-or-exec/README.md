# jump-or-exec

Activate a matching Wayland toplevel, or execute the command when no match
exists. This tool requires a wlroots-based compositor. It uses
`zwlr_foreign_toplevel_manager_v1` to list windows and activate one.

## Usage

```bash
jump-or-exec -j REGEX -- COMMAND [ARGS...]
jump-or-exec -l
```

The "REGEX" is matched against two lines:

```text
app_id
title
```

This is intended to be bound to a shortcut in the compositor.

## Examples

```bash
# Jump to foot terminal, or launch if not running
jump-or-exec -j '^foot\n' -- foot

# Jump to Firefox window containing "GitHub" in title, or launch firefox
jump-or-exec -j '^firefox\n.*GitHub' -- firefox

# Inspect available app_id/title pairs (use this to find what to match)
jump-or-exec -l
```

## How it works

The tool connects to the Wayland compositor and listens for toplevel windows
via the `zwlr_foreign_toplevel_manager_v1` protocol. For each window, it
concatenates the `app_id` and `title` (separated by a newline) into a single
string. When you provide a regex pattern with `-j`, it searches through all
windows for a match. If a matching window is found and not currently active,
it activates that window; otherwise, it spawns the specified command. This
allows a single keybinding to either jump to an existing window or launch a
new instance.

```c
regex_t regex = {0};
regcomp(&regex, pattern, REG_EXTENDED | REG_NOSUB);
regexec(regex, "{app_id}\n{title}", 0, NULL, 0);
```
