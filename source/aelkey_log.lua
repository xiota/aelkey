--[[
  aelkey.log
  Lightweight logging module with level filtering and lazy evaluation.

  Usage:
    aelkey.log.set_level("debug")
    aelkey.log.info("system started")
    aelkey.log.debug("value=%d", x)
    aelkey.log.debug(aelkey.util.dump_table, t)
    aelkey.log.debug(function() return expensive_string() end)

  Notes:
    • Supports levels: none, error, warn, info, debug
    • Accepts strings, format strings, functions, and dump utilities
    • Lazy evaluation: functions are only executed if the level is enabled
]]--

local M = {}

local default_level = "error"
local current_level = default_level

local levels = {
  none  = 0,
  error = 1,
  warn  = 2,
  info  = 3,
  debug = 4,
  trace = 5,
  spam = 6,
  all = 0xffff,
}

local function log_write(level, text)
  io.stderr:write("[", level, "] ", text, "\n")
end

local function log_dispatch(level, msg, ...)
  local lvl = levels[level]
  local cur = levels[current_level]

  if not lvl then
    log_write("error", "unknown log level: " .. tostring(level))
    return
  end

  if not cur then
    log_write("error", "invalid current log level: " .. tostring(current_level))
    return
  end

  if lvl > cur then
    return
  end

  local t = type(msg)

  -- 1. Function: lazy evaluation
  if t == "function" then
    local ok, result = pcall(msg, ...)
    if not ok then
      log_write("error", "function threw: " .. tostring(result))
      return
    end
    if type(result) == "string" then
      log_write(level, result)
    end
    return
  end

  -- 2. String with optional formatting
  if t == "string" then
    local n = select("#", ...)
    if n > 0 then
      local ok, formatted = pcall(string.format, msg, ...)
      if ok then
        log_write(level, formatted)
      else
        log_write("error", "bad format string: " .. msg)
      end
    else
      log_write(level, msg)
    end
    return
  end

  -- 3. Anything else → tostring
  log_write(level, tostring(msg))
end

function M.set_level(level)
  if levels[level] then
    current_level = level
  else
    log_write("error", "unknown log level: " .. tostring(level))
    current_level = default_level
  end
end

function M.error(...) log_dispatch("error", ...) end
function M.warn(...)  log_dispatch("warn",  ...) end
function M.info(...)  log_dispatch("info",  ...) end
function M.debug(...) log_dispatch("debug", ...) end
function M.trace(...) log_dispatch("trace", ...) end
function M.spam(...) log_dispatch("spam", ...) end

return M
