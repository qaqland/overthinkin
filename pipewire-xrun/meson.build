project('pipewire-xrun', 'c',
  version : '0.1.0',
  default_options : ['c_std=c11', 'warning_level=2'])

cc = meson.get_compiler('c')

add_project_arguments('-D_GNU_SOURCE', language : 'c')

pipewire_dep = dependency('libpipewire-0.3')
math_dep = cc.find_library('m', required : true)

executable('pipewire-xrun',
  'main.c',
  dependencies : [pipewire_dep, math_dep],
  install : true)
