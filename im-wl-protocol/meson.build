project('im-wl-protocol', 'c',
  version: '0.1.0',
  default_options: ['c_std=c11', 'warning_level=2'],
)

add_project_arguments(
  '-D_POSIX_C_SOURCE=200809L',
  '-DSOCKET_NAME="im-wl-chat"',
  language: 'c',
)

wayland_server = dependency('wayland-server')
wayland_client = dependency('wayland-client')
wayland_scanner = find_program('wayland-scanner')

protocol_xml = files('im-chat-v1.xml')

im_chat_server_h = custom_target('im-chat-v1-server-header',
  input: protocol_xml,
  output: 'im-chat-v1-server-protocol.h',
  command: [wayland_scanner, 'server-header', '@INPUT@', '@OUTPUT@'],
)

im_chat_client_h = custom_target('im-chat-v1-client-header',
  input: protocol_xml,
  output: 'im-chat-v1-client-protocol.h',
  command: [wayland_scanner, 'client-header', '@INPUT@', '@OUTPUT@'],
)

im_chat_code = custom_target('im-chat-v1-code',
  input: protocol_xml,
  output: 'im-chat-v1-protocol.c',
  command: [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'],
)

executable('im-server',
  ['server.c', im_chat_server_h, im_chat_code],
  dependencies: [wayland_server],
  include_directories: include_directories('.'),
)

executable('im-client',
  ['client.c', im_chat_client_h, im_chat_code],
  dependencies: [wayland_client],
  include_directories: include_directories('.'),
)
