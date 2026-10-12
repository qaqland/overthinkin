# pw-micwatch

PipeWire 麦克风观察工具，持续监听 PipeWire registry，打印哪些 client 请求使用麦克风
（`media.class = Stream/Input/Audio` 的采集流，即 PulseAudio 里的 source-output）、
实时状态、以及何时结束。

属性来自绑定的 Node proxy（node info 事件），比 registry global 上的属性更完整，
且能捕捉注册后才补齐或后续变化的属性。

## 示例输出

默认级别：

```text
[11:05:32 INFO ] mic capture request: pw-record (id=85, client.id=96)
[11:05:35 INFO ] mic capture state: pw-record (id=85) -> running
[11:05:39 INFO ] mic recording stopped: pw-record (id=85)
```

`-v` 时可区分尖峰探测与真实录音：

```text
  media.name = Peak detect
  resample.peaks = true
  stream.monitor = true    <<< HERE
  node.latency = 1/25
```
