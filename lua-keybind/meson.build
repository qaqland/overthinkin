project(
    'lua-keybind',
    'c',
    version: '0.1.0',
    default_options: ['warning_level=3', 'c_std=c23'],
)

sdl_dep = dependency('sdl3', required: true)
lua_dep = dependency('lua5.4', required: true)

exe = executable('lua-keybind', 'main.c',
    dependencies: [sdl_dep, lua_dep])
