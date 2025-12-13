local M = ...

function M.dump_events(events)
  print(string.format("events: %d", #events))
  for i, ev in ipairs(events) do
    print(string.format(
      "[%d] device=%s type=%s(%s) code=%s(%s) value=%s",
      i, ev.device, ev.type_name, ev.type, ev.code_name, ev.code, ev.value
    ))
  end
end

function M.dump_raw(ev)
  local data = ev.data
  local len = #data
  io.write(string.format("raw data (%d bytes):", len))
  for i = 1, len do
    io.write(string.format(" %02X", string.byte(data, i)))
  end
  io.write("\n")
end

function M.dump_table(t, indent)
  indent = indent or "  "
  print "{"
  for k, v in pairs(t) do
    if type(v) == "table" then
      print(indent .. tostring(k) .. " = ")
      print_table(v, indent .. "  ")
    else
      print(indent .. tostring(k) .. " = " .. tostring(v))
    end
  end
  print "}"
end
