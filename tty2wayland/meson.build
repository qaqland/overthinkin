project(
    'tty2wayland',
    'c',
    version: '0.1.0',
    default_options: ['c_std=c11', 'warning_level=3'],
)

add_project_arguments('-D_GNU_SOURCE', '-Wno-unused-parameter', language: 'c')

wayland_server = dependency('wayland-server')
libdrm = dependency('libdrm')
wayland_protocols = dependency('wayland-protocols')
scanner = find_program('wayland-scanner')
xml = join_paths(
    wayland_protocols.get_variable(pkgconfig: 'pkgdatadir'),
    'stable',
    'xdg-shell',
    'xdg-shell.xml',
)

xdg_header = custom_target(
    'xdg-shell-server-header',
    input: xml,
    output: 'xdg-shell-server-protocol.h',
    command: [scanner, 'server-header', '@INPUT@', '@OUTPUT@'],
)
xdg_code = custom_target(
    'xdg-shell-code',
    input: xml,
    output: 'xdg-shell-protocol.c',
    command: [scanner, 'private-code', '@INPUT@', '@OUTPUT@'],
)

executable(
    'tty2wayland',
    ['main.c', 'drm.c', 'wayland.c', xdg_header, xdg_code],
    dependencies: [wayland_server, libdrm],
    include_directories: include_directories('.'),
)
