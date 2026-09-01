project(
  'aelkey', 'cpp',
  license: 'GPL-3.0-or-later',
  version: files('version.txt'),
  meson_version: '>=0.61',
  default_options: [
    'buildtype=debugoptimized',
    'prefix=/usr',
    'cpp_std=c++20',
    'cpp_args=-pedantic',
  ],
)

# depends
dbus_dep = dependency('dbus-1')
jack_dep = dependency('jack')
libevdev_dep = dependency('libevdev')
libudev_dep = dependency('libudev')
libusb_dep = dependency('libusb-1.0')

# lua and sol
lua_version = get_option('lua_version')
if lua_version == 'auto'
  foreach lv : ['lua5.5', 'lua5.4', 'luajit', 'lua5.3', 'lua5.2', 'lua5.1']
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

subdir('source')
subdir('docs')
