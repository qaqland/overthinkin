# PipeWire Suspend

PipeWire 无法像 PulseAudio 一样全局暂停播放（触发命令见下），
某些声卡在播放时进入 S3 又会有问题。

```
$ pactl suspend-sink @DEFAULT_SINK@ 1
```

因此，这里获取输出设备的 ID 然后指定暂停，并希望 S3 后再恢复。

```
$ pipewire-suspend 1
$ pipewire-suspend 0
```

`prepare-listener.sh` 是根据 D-Bus 信号触发的示例。

