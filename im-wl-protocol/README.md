# im-wl-protocol

A simple multi-user chat system built on Wayland's client-server IPC mechanism.

基于 Wayland 协议的本地聊天室

## Protocol

Defines a custom Wayland interface `im_chat_manager_v1`:

| Requests | |
|---|---|
| `set_nickname(nickname)` | Set or change nickname (must be unique) |
| `send_message(content)` | Send a message to the room |
| `destroy` | Leave the room |

| Events | |
|---|---|
| `nickname_accepted(nickname)` | Nickname was set |
| `nickname_rejected(reason)` | Nickname was rejected |
| `message(nickname, content)` | Chat message received |
| `user_joined(nickname)` | A user joined |
| `user_left(nickname)` | A user left |

## Build & Run

```bash
meson setup build
meson compile -C build

# set XDG_RUNTIME_DIR

# terminal 1
./build/im-server

# terminal 2
./build/im-client
```
