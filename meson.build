project(
  'aelkey', 'cpp',
  license: 'GPL-3.0-or-later',
  version: files('version.txt'),
  meson_version: '>=0.61',
  default_options: ['buildtype=release', 'prefix=/usr', 'cpp_std=c++20'],
)

# depends
dbus_dep = dependency('dbus-1')
libevdev_dep = dependency('libevdev')
libudev_dep = dependency('libudev')
libusb_dep = dependency('libusb-1.0')

# lua and sol
lua_version = get_option('lua_version')
if lua_version == 'auto'
  foreach lv : ['lua5.4', 'luajit', 'lua5.3', 'lua5.2', 'lua5.1']
    lua_dep = dependency(lv, required: false)
    if lua_dep.found()
      lua_version = lv
      break
    endif
  endforeach

  if not lua_dep.found()
    error('No suitable Lua implementation found')
  endif
else
  lua_dep = dependency(lua_version)
endif

if lua_version == 'luajit'
  sol2_dep = dependency('sol2',
    fallback: ['sol2', 'sol2_dep'],
    default_options: ['use_luajit=true']
  )
else
  sol2_dep = dependency('sol2',
    fallback: ['sol2', 'sol2_dep'],
  )
endif

# config.h
conf_data = configuration_data()
conf_data.set('VERSION', '"' + meson.project_version() + '"')

# lua scripts
fs = import('fs')

lua_click_content  = fs.read(meson.project_source_root() / 'source/aelkey_click.lua')
lua_daemon_content  = fs.read(meson.project_source_root() / 'source/aelkey_daemon.lua')
lua_log_content  = fs.read(meson.project_source_root() / 'source/aelkey_log.lua')
lua_util_content  = fs.read(meson.project_source_root() / 'source/aelkey_util.lua')

lua_scripts = configuration_data()
lua_scripts.set('AELKEY_CLICK_SCRIPT',  lua_click_content.strip())
lua_scripts.set('AELKEY_DAEMON_SCRIPT',  lua_daemon_content.strip())
lua_scripts.set('AELKEY_LOG_SCRIPT',  lua_log_content.strip())
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
  'source/aelkey_gatt.cc',
  'source/aelkey_hid.cc',
  'source/aelkey_loop.cc',
  'source/aelkey_state.cc',
  'source/aelkey_usb.cc',
  'source/aelkey_util.cc',
  'source/device_gatt.cc',
  'source/device_input.cc',
  'source/device_libusb.cc',
  'source/device_output.cc',
  'source/device_udev.cc',
)

shared_library(
  meson.project_name(),
  src_files,
  dependencies: [
    dbus_dep,
    libevdev_dep,
    libudev_dep,
    libusb_dep,
    lua_dep,
    sol2_dep,
  ],
  name_prefix: '',
  install: true,
  install_dir: lua_dep.get_variable('INSTALL_CMOD')
)

install_data(
  [
    'data/72-aelkey-general-group.rules',
    'data/72-aelkey-general-uaccess.rules',
  ],
  install_dir: get_option('datadir') / meson.project_name(),
)

sysusersdir = get_option('sysusersdir')
if sysusersdir == ''
  sysusersdir = get_option('libdir') / 'sysusers.d'
endif

install_data(
  'data/sysusers-aelkey.conf',
  rename: meson.project_name() + '.conf',
  install_dir: sysusersdir,
)
