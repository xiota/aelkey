fs = import('fs')

luac_prog = ''
if lua_version == 'luajit'
  luac_prog = 'luajit'
elif lua_version.startswith('lua5.1')
  luac_prog = 'luac5.1'
elif lua_version == 'lua5.2'
  luac_prog = 'luac5.2'
elif lua_version == 'lua5.3'
  luac_prog = 'luac5.3'
elif lua_version == 'lua5.4'
  luac_prog = 'luac5.4'
elif lua_version == 'lua5.5'
  luac_prog = 'luac5.5'
else
  luac_prog = 'luac'
endif

luac = find_program(luac_prog)

if luac.found()
  lua_files = [
    'click.lua',
    'edge.lua',
    'filter.lua',
    'log.lua',
    'midi.lua',
    'monitor.lua',
    'profiles.lua',
    'sequence.lua',
    'ticker.lua',
    'tracker.lua',
    'util.lua',
  ]

foreach lua_file: lua_files
    if lua_version == 'luajit'
      cmd = [luac, '-b', '@INPUT@', '@OUTPUT@']
    else
      cmd = [luac, '-p', '@INPUT@']
    endif

    custom_target(
      'check_' + fs.name(lua_file),
      input: lua_file,
      output: fs.name(lua_file) + '.stamp',
      command: cmd,
      build_always_stale: true,
      build_by_default: true,
    )
  endforeach
endif

lua_click_content = fs.read('click.lua')
lua_edge_content = fs.read('edge.lua')
lua_filter_content = fs.read('filter.lua')
lua_log_content = fs.read('log.lua')
lua_midi_content = fs.read('midi.lua')
lua_monitor_content = fs.read('monitor.lua')
lua_profiles_content = fs.read('profiles.lua')
lua_sequence_content = fs.read('sequence.lua')
lua_ticker_content = fs.read('ticker.lua')
lua_tracker_content = fs.read('tracker.lua')
lua_util_content = fs.read('util.lua')

lua_scripts = configuration_data()
lua_scripts.set('AELKEY_CLICK_SCRIPT',  lua_click_content.strip())
lua_scripts.set('AELKEY_EDGE_SCRIPT',  lua_edge_content.strip())
lua_scripts.set('AELKEY_FILTER_SCRIPT',  lua_filter_content.strip())
lua_scripts.set('AELKEY_LOG_SCRIPT',  lua_log_content.strip())
lua_scripts.set('AELKEY_MIDI_SCRIPT',  lua_midi_content.strip())
lua_scripts.set('AELKEY_MONITOR_SCRIPT',  lua_monitor_content.strip())
lua_scripts.set('AELKEY_PROFILES_SCRIPT',  lua_profiles_content.strip())
lua_scripts.set('AELKEY_SEQUENCE_SCRIPT',  lua_sequence_content.strip())
lua_scripts.set('AELKEY_TICKER_SCRIPT',  lua_ticker_content.strip())
lua_scripts.set('AELKEY_TRACKER_SCRIPT',  lua_tracker_content.strip())
lua_scripts.set('AELKEY_UTIL_SCRIPT',  lua_util_content.strip())

lua_scripts_h = configure_file(
  input: 'lua_scripts.h.in',
  output: 'lua_scripts.h',
  configuration: lua_scripts
)
