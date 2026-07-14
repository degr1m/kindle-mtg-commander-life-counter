project(
  'mtg-commander-life-counter',
  'c',
  version: '2.0.0',
  default_options: ['c_std=c11', 'warning_level=2', 'werror=true'],
  meson_version: '>=1.1',
)

cc = meson.get_compiler('c')
add_project_arguments(
  '-D_POSIX_C_SOURCE=200809L',
  '-Wno-unused-local-typedefs',
  language: 'c',
)
gtk = dependency('gtk+-2.0')
math = cc.find_library('m', required: true)

executable(
  'mtg-life-counter',
  files('src/main.c', 'src/game.c', 'src/layout.c', 'src/device.c'),
  dependencies: [gtk, math],
  install: false,
)
