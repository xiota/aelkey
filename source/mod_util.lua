local M = ...

function M.dump_events(events)
  local out = {}
  out[#out+1] = string.format("events: %d", #events)

  for i, ev in ipairs(events) do
    out[#out+1] = string.format(
      "[%d] device=%s type=%s code=%s value=%s",
      i, ev.device, ev.type, ev.code, ev.value
    )
  end

  return table.concat(out, "\n")
end

function M.dump_hex(data)
  local bytes = {}

  if type(data) == "string" then
    -- Convert string → byte table
    for i = 1, #data do
      bytes[#bytes+1] = string.byte(data, i)
    end
  elseif type(data) == "table" then
    -- Assume table of numbers
    for i = 1, #data do
      bytes[#bytes+1] = data[i]
    end
  else
    return string.format("dump_hex: unsupported format, %s", type(data))
  end

  -- Format as hex
  local out = {}
  for i = 1, #bytes do
    out[#out+1] = string.format("%02X", bytes[i])
  end

  return table.concat(out, " ")
end

function M.dump_raw(ev)
  local data = ev.data
  local len = #data
  local out = {}

  out[#out+1] = string.format("raw data (%d bytes):", len)

  for i = 1, len do
    out[#out+1] = string.format(" %02X", string.byte(data, i))
  end

  return table.concat(out)
end

local function dump_table_inner(t, indent, out)
  for k, v in pairs(t) do
    if type(v) == "table" then
      out[#out+1] = string.format("%s%s = {", indent, tostring(k))
      dump_table_inner(v, indent .. "  ", out)
      out[#out+1] = indent .. "}"
    else
      out[#out+1] = string.format("%s%s = %s", indent, tostring(k), tostring(v))
    end
  end
end

function M.dump_table(t)
  local out = { "{" }
  dump_table_inner(t, "  ", out)
  out[#out+1] = "}"
  return table.concat(out, "\n")
end

--[[
  bench_scope
  A tiny scope timer that measures execution time and the interval between calls.

  Usage (Lua 5.1+):
    function remap(events)
      local scope = aelkey.util.bench_scope("remap")
      -- remap code
      scope.finish()
    end

  Usage (5.4+):
    function remap(events)
      local scope <close> = aelkey.util.bench_scope("remap")
      -- remap code
    end
--]]
local now_fn = M.now -- M is aelkey.util, loading in progress
local function now_us()
  return now_fn("us")
end

local bench_noop = { finish = function() end }
setmetatable(bench_noop, { __close = function() end })

local bench_state = { last = now_us() }

function M.bench_scope(label)
  if not aelkey.log.is_enabled("trace") then
    return bench_noop
  end

  local resolved_label = label or "remap"
  local start_us = now_us()

  -- compute between-calls immediately
  local between_ms = (start_us - bench_state.last) / 1000
  bench_state.last = start_us

  local obj = {}

  -- manual finish() for Lua 5.1–5.3
  function obj.finish()
    if obj._done then return end
    obj._done = true

    local end_us = now_us()
    local exec_ms = (end_us - start_us) / 1000

    aelkey.log.trace(
      "%s: between %.3f ms, exec %.3f ms",
      resolved_label, between_ms, exec_ms
    )
  end

  -- RAII-style __close for Lua 5.4+
  return setmetatable(obj, { __close = obj.finish })
end
