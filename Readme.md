# Ælkey Input Remapping Framework

A programmable input remapping framework for Linux to observe, transform, and synthesize input events across a wide range of devices. Designed for advanced workflows where traditional keybinding tools are too complicated or fall short.

Supports `evdev`, `hidraw`, `libusb`, and `gatt` backends with optional force‑feedback routing and custom haptic effects.

## Why use Ælkey

Make non‑standard, unsupported, or poorly behaved input devices usable on Linux.

* Create complex input behaviors with Lua instead of static keymaps
* Combine events from multiple devices with a single logic layer
* Interact with HID, USB, and BLE devices directly
* Write userspace drivers for devices that are otherwise unusable
* Build custom haptics and feedback loops

## Example

A minimal script that connects to a BLE GATT device and prints incoming events along with the associated characteristic.

```lua
aelkey = require("aelkey")

inputs = {
  { id = "ble_gatt", type = "gatt", name = ".+", on_event = "remap" },
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
```

## Requirements

Lua 5.4 or 5.5 are recommended for optimal runtime performance.  The LuaJIT garbage collector has excessive latency spikes when used with the sol2 library.

- Linux-based operating system
- C++20-compatible compiler (GCC or clang)
- Lua 5.1-5.5 or LuaJIT
- dbus‑1
- libevdev
- libudev
- libusb‑1.0

## Build

```bash
meson setup build
meson compile -C build
sudo meson install -C build

```

## Documentation

[**Reference**](docs/readme.md)

* [Device Tables](docs/ref-device-tables.md)
* [General API](docs/ref-general-api.md)
* [Input and Other Helpers](docs/ref-helpers.md)

**Other**

* [Mini Guide to Writing udev Rules](docs/guide-udev.md)

## License

GPL-3.0-or-later
