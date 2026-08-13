# dde-clip-wink — 任务栏剪贴板抖动提示插件

本项目参与 [deepin 社区活动](https://bbs.deepin.org/post/300665)，为 deepin/UOS 开发的任务栏托盘插件。

## 功能

- 复制内容后，任务栏图标 QQ 式闪烁（消失/出现交替，约 800 ms）。
- 鼠标悬停图标，显示最新剪贴板文本的前 32 个字符。
- 左键点击图标，弹出系统历史剪贴板面板（同 Win+V）。
- 图标为纯黑代码绘制，悬停时不变色不跳动，不依赖外部图标资源。

## 构建与安装

```bash
sudo apt install -y cmake qt6-base-dev dde-tray-loader-dev
cmake -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr
cmake --build build
sudo cmake --install build
```

编译产物为 `build/libdde-clip-wink.so`，会安装到 `/usr/lib/dde-dock/plugins/system-trays/`。

重启后，任务栏托盘区即出现剪贴板图标。

```bash
systemctl --user restart dde-shell@DDE.service
```

## 卸载

```bash
sudo rm /usr/lib/dde-dock/plugins/system-trays/libdde-clip-wink.so
systemctl --user restart dde-shell@DDE.service
```

## 注意事项

- 需要 `dde-clipboard-daemon`（剪贴板守护进程）正常运行。
- 图片显示“剪贴板图片 宽x高”，文件显示“剪贴板文件 N 个”。
