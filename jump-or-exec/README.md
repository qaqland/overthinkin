# jump-or-exec

Activate a matching Wayland toplevel, or start a command when no match exists.

Uses `zwlr_foreign_toplevel_manager_v1` to list windows and activate one.

## Usage

```bash
jump-or-exec -j REGEX -- COMMAND [ARGS...]
jump-or-exec -l
```

## Examples

```bash
jump-or-exec -j '^foot\n' -- foot
jump-or-exec -j '^firefox\n.*GitHub' -- firefox

# Inspect available app_id/title pairs
jump-or-exec -l
```

The regex is matched against two lines:

```text
app_id
title
```

This is intended to be bound to a shortcut in the compositor.
