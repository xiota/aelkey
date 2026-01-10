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
