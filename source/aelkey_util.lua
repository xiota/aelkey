local M = ...

function M.dump_events(events)
  local out = {}
  out[#out+1] = string.format("events: %d", #events)

  for i, ev in ipairs(events) do
    out[#out+1] = string.format(
      "[%d] device=%s type=%s(%s) code=%s(%s) value=%s",
      i, ev.device, ev.type_name, ev.type, ev.code_name, ev.code, ev.value
    )
  end

  return table.concat(out, "\n")
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
