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
  if type(data) == "table" and data.data ~= nil then
    data = data.data
  end

  if type(data) ~= "string" and type(data) ~= "table" then
    return string.format(
      "dump_hex: unsupported format, %s",
      type(data)
    )
  end

  local out = {}

  for i = 1, #data do
    local byte

    if type(data) == "string" then
      byte = data:byte(i)
    else
      byte = data[i]
    end

    out[#out + 1] = string.format("%02X", byte)
  end

  return table.concat(out, " ")
end

local function is_binary_string(s)
  for i = 1, #s do
    local c = s:byte(i)

    if c == 0 or c < 32 or c > 126 then
      return true
    end
  end

  return false
end

local function format_string(s, hex)
  if hex and is_binary_string(s) then
    local bytes = {}

    for i = 1, #s do
      bytes[#bytes + 1] = string.format("0x%02X", s:byte(i))
    end

    return "string.char(" .. table.concat(bytes, ", ") .. ")"
  end

  return string.format("%q", s)
end

local function dump_table_inner(t, indent, out, hex)
  for k, v in pairs(t) do
    if type(v) == "table" then
      out[#out+1] = string.format("%s%s = {", indent, tostring(k))
      dump_table_inner(v, indent .. "  ", out, hex)
      out[#out+1] = indent .. "},"
    elseif type(v) == "string" then
      out[#out+1] = string.format(
        "%s%s = %s,",
        indent, tostring(k), format_string(v, hex)
      )
    else
      out[#out+1] = string.format(
        "%s%s = %s,",
        indent, tostring(k), tostring(v)
      )
    end
  end
end

function M.dump_table(t, hex)
  if hex == nil then
    hex = true
  end

  local out = { "{" }
  dump_table_inner(t, "  ", out, hex)
  out[#out+1] = "}"

  return table.concat(out, "\n")
end

function M.pack_bytes(...)
  local chunks = {}

  local function process(item)
    local t = type(item)
    if t == "table" then
      for _, sub in ipairs(item) do
        process(sub)
      end
    elseif t == "number" then
      chunks[#chunks + 1] = string.char(item)
    elseif t == "string" then
      chunks[#chunks + 1] = item
    else
      error("pack_bytes: unexpected type " .. t)
    end
  end

  for i = 1, select("#", ...) do
    process(select(i, ...))
  end

  return table.concat(chunks)
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
  if not aelkey.log.is_enabled("bench") then
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

    aelkey.log.bench(
      "%s: between %.3f ms, exec %.3f ms",
      resolved_label, between_ms, exec_ms
    )
  end

  -- RAII-style __close for Lua 5.4+
  return setmetatable(obj, { __close = obj.finish })
end
