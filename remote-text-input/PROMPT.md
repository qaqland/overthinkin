# 提示词

实现下面描述的项目，最终行为必须与规格一致。

## 目标

用 Rust 写一个极简"远程输入法"：在支持 `zwp_input_method_v2` 的 Wayland 会话
（Sway / Hyprland）里运行，手机用浏览器打开内嵌网页打字，文字实时显示为桌面目标应
用的预编辑（preedit），点"发送"才提交。不模拟按键（包括 Enter），不依赖 Fcitx /
IBus，只用 `zwp_input_method_v2`。

- 网页用 `include_str!` 嵌入可执行文件，无前端构建。
- 依赖：`axum` 0.8（ws）、`tokio` 1、`calloop` 0.14、`calloop-wayland-source` 0.4、
  `wayland-client` 0.31、`wayland-protocols-misc` 0.3、`serde` / `serde_json`。
- 规模：Rust 约 400 行 + 一个约 120 行的 HTML。不加配置、认证和任何未列出的功能。

## 架构

两个线程：`tokio` 主线程跑 HTTP；`spawn_blocking` 里跑 `calloop` + Wayland 事件循环。
三个通道：HTTP→Wayland 用 `calloop` channel 发命令（更新/清空/停止，更新附带 oneshot
回传结果）；Wayland→HTTP 用 `tokio` watch 广播状态 `{ active: bool, epoch: u64 }`。
启动先 bind TCP 并打印地址；`tokio::select!` 等待 axum、Ctrl+C、后端退出；退出前发
停止命令并等后端清理（清 preedit、destroy、flush）。

## Wayland 后端

- 第一次 roundtrip 从 registry 绑定 `zwp_input_method_manager_v2` 和第一个 `wl_seat`
  （缺一即报错退出），然后 `get_input_method` 再 roundtrip；收到 `unavailable` 即退出。
- 事件：`activate` / `deactivate` 只记 pending 状态并标记重置；`done` 时 serial 加 1、
  pending 落入正式状态、若标记了重置则 epoch 加 1，然后发布状态；`unavailable` 停循环。
- epoch 防串焦点：处理文本命令时校验当前 active 且客户端 epoch 相等，否则报错
  "输入焦点已改变，请重新输入或发送"。
- 文本校验：UTF-8 ≤ 3900 字节、不含 `\0`。
- 草稿：`set_preedit_string(text, len, len)`（光标固定末尾）后 `commit(serial)`。
- 发送：先 `set_preedit_string("", 0, 0)` 再 `commit_string(text)` 后 `commit(serial)`
  （只有 `commit_string` 真正插入文字）。
- 会话结束或退出时若仍 active，清 preedit 并 commit。

## HTTP / WebSocket 服务端

- CLI 第一个参数为监听地址，默认 `0.0.0.0:8080`；`-h` 打印用法。启动提示：手机需换
  局域网 IP、首个会话可输入其余排队、仅限可信局域网（未加密）。
- `GET /` 返回内嵌 HTML，响应头带 `cache-control: no-store`、`referrer-policy:
  no-referrer`、`x-frame-options: DENY`。
- `GET /ws` 升级前校验 `Origin` 等于 `http://{Host}`，否则 403；消息和帧上限 32KB。
- 排队：新会话先发 `{"type":"waiting"}`，然后排队等一把 `tokio` `Mutex`（利用其
  FIFO 公平性），等待期间忽略消息、断开即退队。拿到锁的成为控制者，先收
  `{"type":"state","state":{active,epoch}}`，之后状态变化都推送；断开时锁释放，下一
  个自动接管，同时向 Wayland 发清空命令。
- 客户端消息两种：`{"type":"draft","epoch":N,"text":"..."}` 和
  `{"type":"send","epoch":N,"id":M,"text":"..."}`。回复 `{"type":"ack","id":M}`
  （draft 时 id 为 null）或 `{"type":"error","id":M,"message":"..."}`；无法解析回
  "无效消息"；Wayland 通道断开回 "Wayland 连接已关闭"。

## 前端（内嵌单页，中文 UI）

界面：标题、状态行、textarea、字节计数（`N / 3900 字节`）、发送按钮、错误行、断线
后出现的重连按钮。

- 打开即连 `/ws`（按页面协议选 ws/wss）。收到 `waiting` 显示排队提示，发送键禁用，
  但仍可写本地草稿。收到 `state` 按 active 显示"已接管，可以输入"或"已接管，请在电
  脑上选择输入位置"；epoch 变化时取消未发出的防抖同步。
- 输入后 40ms 防抖，已连接、active、无进行中发送且文本合法时自动发 `draft` 同步；
  不合法时显示错误且不同步。
- 监听 `compositionstart` / `compositionend`：组词期间禁发送、不同步，结束后立即同步。
- 发送：自增 id 发 `send`，期间锁定 textarea 和按钮直到对应 `ack`（成功清空草稿）
  或 `error`；断线时有未确认发送则提示"发送结果未知，请先检查电脑上的内容再重试"。
- 断线：显示"连接已断开，草稿已保留"（草稿留在 textarea）和重连按钮。
- 页面注明："发送只提交文字，不模拟回车。关闭页面后下一会话自动接管。排队或没有输
  入焦点时仍可写草稿。"

## README 边界

写清运行环境、用法和安全警告（无加密无认证，仅限可信局域网，勿暴露公网）。

## 验收标准

1. `cargo run --release` 在 Sway/Hyprland 启动并打印地址。
2. 手机打字桌面实时显示 preedit；点发送文字提交且无回车。
3. 第二个网页显示排队；关闭第一个后自动接管。
4. 桌面切换焦点后，旧草稿/发送被拒绝并提示焦点已改变。
5. 手机断开，桌面残留 preedit 被清空。
6. Ctrl+C 干净退出。
