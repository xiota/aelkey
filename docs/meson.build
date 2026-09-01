go_md2man = find_program('go-md2man')

lua_prog = ''
if lua_version == 'luajit'
  lua_prog = 'luajit'
elif lua_version.startswith('lua5.1')
  lua_prog = 'lua5.1'
elif lua_version == 'lua5.2'
  lua_prog = 'lua5.2'
elif lua_version == 'lua5.3'
  lua_prog = 'lua5.3'
elif lua_version == 'lua5.4'
  lua_prog = 'lua5.4'
elif lua_version == 'lua5.5'
  lua_prog = 'lua5.5'
else
  lua_prog = 'lua'
endif

lua_exec = find_program(lua_prog)

# To detect changes and regenerate
md_files = [
  'guide-udev.md',
  'readme.md',
  'ref-device-tables.md',
  'ref-general-api.md',
  'ref-haptics.md',
  'ref-helpers.md',
  'ref-jack-audio.md',
]

# Run stitch.lua on readme.md
stitched_docs = custom_target('aelkey-stitched-md',
  input: ['stitch.lua', 'readme.md'],
  output: 'aelkey.7.md',
  depend_files: md_files,
  command: [lua_exec, '@INPUT0@', '@INPUT1@'],
  capture: true,
)

# Generate the final manpage with go-md2man
custom_target('aelkey.7',
  input: stitched_docs,
  output: 'aelkey.7',
  command: [
    go_md2man,
    '-in', '@INPUT@',
    '-out', '@OUTPUT@',
  ],
  build_by_default: true,
  install: true,
  install_dir: get_option('mandir') / 'man7',
)
