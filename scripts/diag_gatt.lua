aelkey = require("aelkey")

inputs = {
  {
    id = "ble_gatt",
    type = "gatt",
    name = ".+",
    on_event = "remap"
  },
}

function remap(ev)
  print(ev.path)
  print(aelkey.util.dump_hex(ev))
  print()
end

aelkey.start()
