fs = import('fs')

lua_click_content = fs.read('click.lua')
lua_edge_content = fs.read('edge.lua')
lua_filter_content = fs.read('filter.lua')
lua_log_content = fs.read('log.lua')
lua_midi_content = fs.read('midi.lua')
lua_monitor_content = fs.read('monitor.lua')
lua_mouse_content = fs.read('mouse.lua')
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
lua_scripts.set('AELKEY_MOUSE_SCRIPT',  lua_mouse_content.strip())
lua_scripts.set('AELKEY_SEQUENCE_SCRIPT',  lua_sequence_content.strip())
lua_scripts.set('AELKEY_TICKER_SCRIPT',  lua_ticker_content.strip())
lua_scripts.set('AELKEY_TRACKER_SCRIPT',  lua_tracker_content.strip())
lua_scripts.set('AELKEY_UTIL_SCRIPT',  lua_util_content.strip())

lua_scripts_h = configure_file(
  input: 'lua_scripts.h.in',
  output: 'lua_scripts.h',
  configuration: lua_scripts
)
