project(
    'jump-or-exec',
    'c',
    version: '0.1.0',
    default_options: [
        'c_std=c11',
        'warning_level=2',
    ],
)

wayland_client = dependency('wayland-client')
wayland_scanner = find_program('wayland-scanner', native: true)

protocol_xml = files('wlr-foreign-toplevel-management-unstable-v1.xml')

protocol_header = custom_target(
    'wlr-foreign-toplevel-management-client-header',
    input: protocol_xml,
    output: 'wlr-foreign-toplevel-management-unstable-v1-client-protocol.h',
    command: [wayland_scanner, 'client-header', '@INPUT@', '@OUTPUT@'],
)

protocol_code = custom_target(
    'wlr-foreign-toplevel-management-private-code',
    input: protocol_xml,
    output: 'wlr-foreign-toplevel-management-unstable-v1-protocol.c',
    command: [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'],
)

prog_version_define = '-DPROG_VERSION="@0@"'.format(meson.project_version())

executable(
    'jump-or-exec',
    [
        'jump-or-exec.c',
        protocol_header,
        protocol_code,
    ],
    dependencies: [wayland_client],
    c_args: [prog_version_define],
    install: true,
)
