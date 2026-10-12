# tty2wayland

A minimal root-only display-path demo, not a general-purpose Wayland compositor:

```text
TTY -> Wayland wl-shm -> CPU copy -> DRM dumb buffer -> Atomic KMS
```

The program creates a temporary runtime directory, sets `XDG_RUNTIME_DIR` and
`WAYLAND_DISPLAY`, and starts the client(default is `weston-simple-shm`). Then
it copies XRGB8888 shared-memory frames into two DRM buffers, and sends each
frame callback after the page flip. There is one output and no input.

```
$ ./build/tty2wayland -h
usage: ./build/tty2wayland [-h] [-c N] [-- command args...]
  -c N    open /dev/dri/cardN (default: 0)
  -h      show this help
```

