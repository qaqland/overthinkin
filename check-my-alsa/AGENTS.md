# AGENTS.md

Guidance for AI coding agents working on check-my-alsa.

## Project

check-my-alsa is a set of ALSA diagnostic applets whose ALSA call
sequences mirror PipeWire's `spa/plugins/alsa` (uos-pipewire
1.6.0-1deepin9), so an applet failing the same way PipeWire fails
localises the fault to the driver/ALSA layer rather than PipeWire.

## Build / format

    meson setup build
    meson compile -C build

Requires libasound2-dev and meson. There is no test suite.

After modifying any source file, reformat before committing:

    clang-format -i *.c *.h

The tree has a `.clang-format` (tabs, 80-column limit). Do not add
comments unless asked. Pipewire source references belong only in each
file's header comment (name the pipewire file(s) mirrored); do not put
`file.c:line` references in the file body. Deviation markers (prose
comments with no line number, e.g. `/* deviation: ... */`) may stay at
the site.

## Architecture

Layout mirrors PipeWire's `spa/plugins/alsa`:

| file                  | pipewire counterpart                       |
|-----------------------|--------------------------------------------|
| `alsa-pcm.c`/`.h`     | `spa/plugins/alsa/alsa-pcm.c`/`.h`         |
| `alsa-pcm-sink.c`     | `spa/plugins/alsa/alsa-pcm-sink.c`         |
| `app-common.c`/`.h`   | applet registry + CLI/report helpers       |
| `app-caps.c`          | `alsa-pcm-device.c` + `test-hw-params.c`   |
| `app-jack.c`          | `bind_ctl` machinery in `alsa-pcm.c`       |
| `app-play`/`app-xrun`/`app-latency`/`app-recover` | `alsa-pcm-sink.c` node process side |

`alsa-pcm.c` is a function-for-function port: same function names
(`spa_alsa_open`, `spa_alsa_set_format`, `set_swparams`, `do_prepare`,
`do_start`, `do_drop`, `alsa_recover`, `get_avail`, `get_status`,
`update_time`, `alsa_write_frames`, ...), same `snd_*` call sequence,
same log text. Each file's header names the pipewire source(s) it
mirrors; deviations (graph plumbing, capture, IRQ mode, dll rate
correction) are marked at the site. The two files can be diffed against
`uos-pipewire/spa/plugins/alsa/alsa-pcm.c` — the diff *is* the
deviation list. Only the playback + tsched (timer-scheduled) path is
ported.

## Conventions

- **Applet registration**: applets self-register via the
  `APPLET_REGISTER()` constructor macro (`app-common.h`); `main.c`
  only dispatches and prints help. To add an applet, define an
  `applet_t` and register it.
- **Exit codes**: `0` success, `1` failure, `2` usage error
  (aplay-style). Applet mains return pipewire-style negative errno;
  `main.c` normalises any negative return to `1`.
- **Errors**: the `CHECK(s, msg, ...)` macro (`app-common.h`) logs the
  ALSA error and returns the negative errno to the caller — do not
  exit from inside an applet.
- **Option parsing** follows alsa-utils conventions (aplay/amixer):
  options before positional args, `getopt` (short options only), strict
  numeric validation. PCM applets use `-D`; `caps`/`jack` use
  `-c` (device `hw:N`).
- **Logging** mirrors `enum spa_log_level` (ERROR/WARN/INFO/DEBUG/TRACE,
  `app-common.h`); default WARN, raised by `-v`/`-vv`/`-vvv`. Log goes
  to stderr; each log line in `alsa-pcm.c` uses the same level as the
  corresponding pipewire `spa_log_*` call.
- **Report output**: summaries go to stdout (never gated) via
  `report_section()` / `report_tab_*` / `report_ok/fail/warn/note()`
  (`app-common.c`). Key column width is the fixed `REPORT_KEY_WIDTH`
  (32). Verdict lines are prefixed `OK:`/`FAIL:`/`WARNING:`/`note:`,
  colored only on a tty (disabled by `NO_COLOR`).
- **No `typedef struct`**: structs are declared with the raw
  `struct` keyword (`struct foo { ... };`) and referenced as
  `struct foo`; do not hide the tag behind a typedef.
- **No new tests**: the project has no test framework; verify changes
  by building and running the relevant applet against a device.
