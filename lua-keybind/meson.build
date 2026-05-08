project(
    'lua-keybind',
    'c',
    version: '0.1.0',
    default_options: ['warning_level=3', 'c_std=c11'],
)

sdl3_dep = dependency('sdl3', required: true)
lua_dep = dependency('lua5.4', required: true)

srcs = files('main.c')

executable('lua-keybind', srcs, dependencies: [sdl3_dep, lua_dep])
