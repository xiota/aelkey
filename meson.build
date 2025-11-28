project(
  'aelkey', 'cpp',
  license: 'GPL-3.0-or-later',
  version: files('version.txt'),
  meson_version: '>=0.61',
  default_options: ['buildtype=release', 'prefix=/usr', 'cpp_std=c++20'],
)

libevdev_dep = dependency('libevdev')
libudev_dep  = dependency('libudev')
luajit_dep   = dependency('luajit')

src_files = files(
  'source/main.cc',
)

executable(
  'aelkey',
  src_files,
  dependencies: [libevdev_dep, libudev_dep, luajit_dep],
  install: true,
  install_dir: get_option('bindir'),
)
