project('pulse-monitor', 'c',
  default_options: ['warning_level=2']
)

pulse_dep = dependency('libpulse')

executable('pulse-monitor',
  'main.c',
  dependencies: [pulse_dep],
  install: true
)
