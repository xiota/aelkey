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
  local data = ev.report
  local len = #data
  io.write(string.format("hidraw report (%d bytes):", len))
  for i = 1, len do
    io.write(string.format(" %02X", string.byte(data, i)))
  end
  io.write("\n")
end
