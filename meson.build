project(
  'aelkey', 'cpp',
  license: 'GPL-3.0-or-later',
  version: files('version.txt'),
  meson_version: '>=0.61',
  default_options: ['buildtype=release', 'prefix=/usr', 'cpp_std=c++20'],
)

# depends
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
  'source/aelkey.cc',
  'source/aelkey_bit.cc',
  'source/aelkey_core.cc',
  'source/aelkey_device.cc',
  'source/aelkey_loop.cc',
  'source/aelkey_state.cc',
  'source/aelkey_util.cc',
  'source/device_input.cc',
  'source/device_output.cc',
)

shared_library(
  meson.project_name(),
  src_files,
  dependencies: [
    libevdev_dep,
    libudev_dep,
    luajit_dep
  ],
  name_prefix: '',
  install: true,
  install_dir: get_option('libdir') / 'lua/5.1'
)
