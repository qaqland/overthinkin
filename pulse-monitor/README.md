# pulse-monitor

PulseAudio event monitor with more details than `pactl subscribe`.

Subscribes to all PulseAudio events and prints each with relevant state
(name, volume, mute, routing, etc.) in real time.

## Build

Dependencies: `libpulse-dev` (Debian).

```sh
$ meson setup build
$ meson compile -C build
```

## Usage

```sh
$ ./build/pulse-monitor
```

`^C` to quit.

## Systemd user service

`meson install -C build` also installs a user service unit to
`/etc/systemd/user/pulse-monitor.service`. To enable and start it:

```sh
$ systemctl --user daemon-reload
$ systemctl --user enable --now pulse-monitor
```

View logs with:

```sh
$ tail -f /tmp/pulse-monitor.log
```

## Sample output

```
[10:52:19.193] new    client        #2776  [com.deepin.SoundEffect]
[10:52:19.220] new    sink-input    #2777  [audio-volume-change]   0% unmuted uncorked client#2776  sink#-
[10:52:19.244] change sink-input    #2777  [audio-volume-change] 100% unmuted uncorked client#2776  sink#2762
[10:52:19.266] change sink          #2762  [USB Audio Device 模拟立体声]  46% unmuted port:analog-output-speaker state:RUNNING
[10:52:19.266] change source        #2762  [Monitor of USB Audio Device 模拟立体声] 100% unmuted state:RUNNING
[10:52:19.431] remove sink-input    #2777  (deleted)
[10:52:19.437] change sink          #2762  [USB Audio Device 模拟立体声]  46% unmuted port:analog-output-speaker state:IDLE
```

