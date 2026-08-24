project(
    'check-my-alsa',
    'c',
    version: '0.1.0',
    default_options: ['c_std=gnu11', 'warning_level=2'],
)

add_project_arguments('-D_GNU_SOURCE', language: 'c')

alsa_dep = dependency('alsa')
math_dep = meson.get_compiler('c').find_library('m', required: true)

sources = files(
    'main.c',
    'app-common.c',
    'app-caps.c',
    'app-jack.c',
    'app-xrun.c',
    'app-latency.c',
    'app-recover.c',
    'app-play.c',
    'app-suspend.c',
    'alsa-pcm.c',
    'alsa-pcm-sink.c',
)

executable(
    'check-my-alsa',
    sources,
    dependencies: [alsa_dep, math_dep],
    install: true,
)
