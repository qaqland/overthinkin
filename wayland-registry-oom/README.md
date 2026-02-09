# Wayland Register OOM

Wayland 协议的接口注册会用到 `wl_display_get_registry` 函数

```
wl_display::get_registry - get global registry object

registry
    id for the new wl_registry - global registry object
```

文档中有说：

> This request creates a registry object that allows the client to list and
> bind the global objects available from the compositor.
>
> It should be noted that the server side resources consumed in response to a
> `get_registry` request can only be released when the client disconnects,
> not when the client side proxy is destroyed. Therefore, clients should invoke
> `get_registry` as infrequently as possible to avoid wasting memory.

于是做了个小程序测试一下，在 Ubuntu 下从 16:35 开始运行，观察到：

- `gnome-shell` 每秒内存占用提高 1MB 左右，20 分钟后占用 1.5 G，也不太大
- `oom` 本体的占用也缓慢增长，速度低于每秒 0.1 MB，可能是为了分配 ID 创建的 `wl_array`

结束 `oom` 进程后不知道为什么 `gnome-shell` 的内存占用没有下降，是设计如此吗？

