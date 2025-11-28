project(
  'aelkey', 'cpp',
  license: 'GPL-3.0-or-later',
  version: files('version.txt'),
  meson_version: '>=0.61',
  default_options: ['buildtype=release', 'prefix=/usr', 'cpp_std=c++20'],
)

# depends
cli11_dep = dependency('CLI11')
libevdev_dep = dependency('libevdev')
libudev_dep  = dependency('libudev')
luajit_dep   = dependency('luajit')

# config.h
conf_data = configuration_data()
conf_data.set('VERSION', '"' + meson.project_version() + '"')

configure_file(
  output: 'config.h',
  configuration: conf_data,
)

# source
src_files = files(
  'source/main.cc',
)

executable(
  'aelkey',
  src_files,
  dependencies: [
    cli11_dep,
    libevdev_dep,
    libudev_dep,
    luajit_dep
  ],
  install: true,
  install_dir: get_option('bindir'),
)
