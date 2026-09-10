subdir('lua_scripts')

src_inc = include_directories('.', 'lua_scripts')

src_files = files(
  'lua_bindings/aelkey.cc',
  'lua_bindings/audio.cc',
  'lua_bindings/core.cc',
  'lua_bindings/evdev.cc',
  'lua_bindings/gatt.cc',
  'lua_bindings/haptics.cc',
  'lua_bindings/hid.cc',
  'lua_bindings/jack.cc',
  'lua_bindings/loop.cc',
  'lua_bindings/midi.cc',
  'lua_bindings/monitor.cc',
  'lua_bindings/uhid.cc',
  'lua_bindings/usb.cc',
  'lua_bindings/util.cc',

  'aelkey_state.cc',
  'backend_bluez.cc',
  'backend_jack.cc',
  'device_in_audio.cc',
  'device_in_evdev.cc',
  'device_in_gatt.cc',
  'device_in_hidraw.cc',
  'device_in_libusb.cc',
  'device_in_midi.cc',
  'device_out_audio.cc',
  'device_out_midi.cc',
  'device_out_uhid.cc',
  'device_out_uinput.cc',
  'device_parser.cc',
  'dispatcher.cc',
  'dispatcher_haptics.cc',
  'dispatcher_registry.cc',
  'dispatcher_udev.cc',
  'manager_device_in.cc',
  'manager_device_out.cc',
  'router_device_state.cc',
  'router_watch_list.cc',
  'signal_handler.cc',
  'utils/regex_match.cc',
)

shared_library(
  meson.project_name(),
  src_files,
  include_directories: src_inc,
  dependencies: [
    dbus_dep,
    jack_dep,
    libevdev_dep,
    libudev_dep,
    libusb_dep,
    lua_dep,
    readerwriterqueue_dep,
    sol2_dep,
  ],
  name_prefix: '',
  install: true,
  install_dir: lua_dep.get_variable('INSTALL_CMOD')
)
