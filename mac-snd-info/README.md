# macOS 音频设备信息工具

> [!WARNING]
> 纯粹的 LLM 制品

这是一个用于 macOS 的命令行工具，用于获取和显示系统中所有音频设备的硬件属性信息。

## 功能特性

- 列出系统中所有音频设备（声卡）
- 显示每个设备的详细硬件属性
- 包括音量信息（标量和 dB 值）
- 支持输入和输出设备
- 显示采样率、通道数、延迟等关键参数
- 支持 USB、蓝牙、HDMI 等多种传输类型

## 使用方法

### 编译
```bash
swiftc AudioDeviceInfo.swift -o snd-info
```

### 运行
```bash
./snd-info
```

程序将输出所有音频设备的详细信息，包括：
- 设备名称和制造商
- 设备 UID 和型号 UID
- 传输类型（USB、蓝牙等）
- 输入/输出通道数
- 采样率和可用采样率范围
- 延迟和安全偏移
- 缓冲区大小
- 音量信息（每通道和主音量）
- 数据源信息
- 流信息等

## 输出示例
```
========================================
  macOS Audio Device Inspector
========================================

Default Input Device ID:         81
Default Output Device ID:        86
Default System Output Device ID: 86

Total devices found: 4

────────────────────────────────────────
Device #1  (AudioObjectID: 86)
────────────────────────────────────────
  Name:             SIMGOT GEW1 ◀ Default Output ◀ Default System
  Manufacturer:     HP
  Device UID:       AppleUSBAudioEngine:HP:SIMGOT GEW1:0123456789AB:1
  Model UID:        SIMGOT GEW1:3206:0798
  Transport Type:   USB
  Input Channels:   0
  Output Channels:  2
  Nominal Sample Rate: 48000.0 Hz
  Available Sample Rates: 44100.0, 48000.0 Hz
  ...
```

