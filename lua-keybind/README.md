# lua-keybind

A tiny SDL3 + Lua 5.4 keybinding host. It opens a window, loads `user.lua` from
the working directory, and lets you register global shortcuts and temporary
modifier-held contexts through a small Lua API. Core binding logic is embedded
at compile time via `key.lua`; user configuration lives in `user.lua`.
