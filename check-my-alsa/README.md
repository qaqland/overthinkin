# check-my-alsa

check-my-alsa is a small set of diagnostic tools targeting ALSA (Advanced
Linux Sound Architecture), organised so that the ALSA calls it makes are
the same calls PipeWire makes, in the same order, with the same
parameters. When a desktop audio problem appears, run the matching applet:
if the applet fails the same way PipeWire fails, the problem is in the
ALSA/driver layer; if the applet succeeds while PipeWire fails, the
problem is in the PipeWire layer.

## Applets

```sh
$ check-my-alsa -h
usage: check-my-alsa <applet> [args...]
       check-my-alsa -h | --help

applets:
  caps     probe card hardware capabilities
  jack     monitor jack plug/unplug events
  latency  measure playback clock drift and reported delay
  play     playback smoke test (PipeWire path)
  recover  test PCM XRUN recovery behaviour (PipeWire alsa_recover)
  suspend  test PCM recovery across a system suspend/resume
  xrun     monitor XRUN (under/overrun) events

run 'check-my-alsa <applet> -h' for applet-specific options.
```

## Output

Reports and jack events are written to stdout. Runtime diagnostics are
written to stderr and become progressively more detailed:

- default: warnings and errors only
- `-v`: test lifecycle and important state changes
- `-vv`: negotiated parameters and ALSA state-machine operations
- `-vvv`: relative-time event traces and raw ALSA parameter dumps

Single-PCM applets do not repeat the device name in runtime logs because it
was supplied explicitly with `-D`. Capability probing retains PCM names
because it examines multiple devices.

## Build

```sh
meson setup build
meson compile -C build
```

Requires libasound2-dev and meson.
