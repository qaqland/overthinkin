# hook-wl-signal

> [!WARNING]
> WIP 包含未经测试的 LLM 代码

A tiny `LD_PRELOAD` hook for Wayland server symbol `wl_signal_emit_mutable`.

It does two things:

- Prints a warning when `wl_signal_emit_mutable` is called with an empty listener list.
- Optionally prints a stack trace (via `libunwind`) each time the function is called.

## Build

Run in this directory:

```bash
gcc -fPIC -shared hook-wl-signal.c -o hook-wl-signal.so \
  $(pkg-config --cflags --libs wayland-server libunwind libdw)
```

If `pkg-config` cannot find `libdw`, install the elfutils development package first
(for example: `libdw-dev` on Debian/Ubuntu, `elfutils-devel` on Fedora).

## Usage

Preload the generated shared library into the target Wayland process:

```bash
$ export HOOK_STACK_DEPTH=10
$ LD_PRELOAD=$PWD/hook-wl-signal.so <wayland-server>
```

