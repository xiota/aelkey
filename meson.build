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

# lua scripts
fs = import('fs')

lua_click_content  = fs.read(meson.project_source_root() / 'data/aelkey_click.lua')
lua_daemon_content  = fs.read(meson.project_source_root() / 'data/aelkey_daemon.lua')
lua_util_content  = fs.read(meson.project_source_root() / 'data/aelkey_util.lua')

lua_scripts = configuration_data()
lua_scripts.set('AELKEY_CLICK_SCRIPT',  lua_click_content.strip())
lua_scripts.set('AELKEY_DAEMON_SCRIPT',  lua_daemon_content.strip())
lua_scripts.set('AELKEY_UTIL_SCRIPT',  lua_util_content.strip())

default_css_h = configure_file(
  input: 'source/lua_scripts.h.in',
  output: 'lua_scripts.h',
  configuration: lua_scripts
)

# source
src_files = files(
  'source/aelkey.cc',
  'source/aelkey_bit.cc',
  'source/aelkey_core.cc',
  'source/aelkey_daemon.cc',
  'source/aelkey_device.cc',
  'source/aelkey_hid.cc',
  'source/aelkey_loop.cc',
  'source/aelkey_state.cc',
  'source/aelkey_util.cc',
  'source/device_input.cc',
  'source/device_output.cc',
  'source/device_udev.cc',
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
