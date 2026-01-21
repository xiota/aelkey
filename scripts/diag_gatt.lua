aelkey = require("aelkey")

inputs = {
  {
    id = "ble_gatt",
    type = "gatt",
    name = ".+",
    on_event = "remap"
  },
}

outputs = {
  { id = "virt_mouse", type = "mouse", name = "Virtual Mouse (GATT)" },
  { id = "virt_keyboard", type = "keyboard", name = "Virtual Keyboard (GATT)" },
}

function remap(ev)
  print(ev.path)
  print(aelkey.util.dump_raw(ev))
  print()
end

aelkey.start()
