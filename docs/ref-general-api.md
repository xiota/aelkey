# Ælkey <span class="ak-trim-line"/>

## General API

### Event Loop / Remapping

- `start()` - enter blocking event loop for remapping.
- `stop()` - terminate the running event loop gracefully, typically in response to a specific input event or condition.
- `emit(event)` - send an event to a virtual output device.
- `syn_report([dev_id])` - flush a frame (`SYN_REPORT`) to complete a batch of emitted events.
- `tick(ms, callback)` - schedule periodic ticks (e.g. timers inside the loop).

### Device Lifecycle and Info

- `open_device([dev_id])` - initialize specified device, all if none specified.
- `close_device([dev_id])` - release specified device, all if none specified.
- `get_device_info(dev_id)` - query metadata (VID, PID, bus type, name, serial/MAC).

### Service Lifecycle and Info (`aelkey.daemon`)

- `watch(ref, decls)` - add a table of input devices for state monitoring; returns the number of valid entries added.
- `unwatch(ref)` - stop monitoring a previously watched set of devices.
- `watchlist()` - list currently watched refs.
- `set_callback(cb)` - set or clear the watchlist callback; returns true on success.
- `inspect_file(path)` - safely load a script from a file for inspection.
- `inspect_string(contents)` - safely load script from a string for inspection.

Note: Only udev compatible types can be watched (evdev, hidraw, libusb).

### HID Feature Control (`aelkey.hid`)

- `get_feature_report(dev_id, report_id)` - synchronous feature report read.
- `get_report_descriptor(dev_id)` - synchronous report descriptor read.
- `read_input_report(dev_id)` - single raw input read (hidraw only).
- `send_feature_report(dev_id, data)` - synchronous feature report write.
- `send_output_report(dev_id, data)` - send one HID output report.

### USB Transfer Requests (`aelkey.usb`)

All synchronous functions return `{device, data, size, status}`.
Asynchronous functions additionally have `{..., endpoint, transfer}`.

- `bulk_transfer{device, endpoint, size, [timeout]}` - perform a synchronous bulk transfer.
- `control_transfer{device, request_type, request, value, index, length, [timeout]}` - submit a synchronous control transfer.
- `interrupt_transfer{device, endpoint, size, [timeout]}` - synchronous interrupt transfer.
- `submit_transfer{device, endpoint, type, size, timeout}` - asynchronous transfer.

### Bluetooth Low Energy Generic Attribute Profile (`aelkey.gatt`)

- `read{device[, service, characteristic]}` - synchronous read from a characteristic.
- `write{device, data [, response] [, service, characteristic]}` - write to a characteristic (default `response = false`).

### Haptics, Force Feedback, and Rumble (`aelkey.haptics`)

- `play(dev_id, effect_table)` - Trigger an effect. The table must contain source and id.
- `stop(dev_id, effect_table)` - Stop an effect. The table must contain source and id.
- `create(effect_table)` - Register a custom effect. Returns the table with source and id injected.
- `erase(effect_table)` - Unregister a custom effect.

Note: Haptics events flow backwards, compared with normal input events.  `dev_id` corresponds to devices in the `inputs` table.

> Game → virtual device (`outputs` table, haptic event) → Lua callback → real device (`inputs` table, actual vibration)

#### haptics event callback table

Sent when a virtual FF source plays or stops an effect.

```lua
{
  -- common fields
  source = "<string>",      -- virtual haptics source ID
  type   = "play" | "stop",
  id     = <int>,           -- virtual effect ID

  -- for play only
  value  = <int>,           -- magnitude from EV_FF play
  effect = { ... },         -- same format as the effect table used by haptics.create(),
                            -- including the injected source/id fields
}
```

### Effect table format (used by `haptics.create()`)

This table defines a force‑feedback effect.

```lua
{
  type   = "rumble" | "periodic" | "constant",
  length = <int>,   -- replay.length (ms)
  delay  = <int>,   -- replay.delay (ms)

  -- rumble
  strong = <int>,
  weak   = <int>,

  -- periodic
  waveform  = <int>,
  magnitude = <int>,
  offset    = <int>,
  phase     = <int>,
  period    = <int>,

  -- constant
  level = <int>,

  -- in returned tables; used by `haptics.play()`
  source = "<internal source id>",
  id     = <int>,
}
```

### Logging (`aelkey.log`)

Logging functions accept format strings or functions that return strings, along with passthrough arguments.

- `set_level(level)` - set log level: none, *error, warn, info, debug, all
- `is_enabled(level)` - check if level is enabled
- `error(...)` - log an error
- `warn(...)` - log a warning
- `info(...)` - log an informational message
- `debug(...)` - log debug output
- `trace(...)` - log debug output
- `spam(...)` - log debug output

### Miscellaneous Utilities (`aelkey.util`)

- `crc32(data, seed)` - compute CRC32 (IEEE) checksum.
- `now([resolution])` - current monotonic time in milliseconds, or in `us`/`ns` if specified.
- `dump_events(events)` - return a formatted string describing a list of input events.
- `dump_hex(bytes)` - return a hex‑dump a binary blob or array of bytes.
- `dump_raw(data)` - return a hex‑dump string of an hidraw report.
- `dump_table(table)` - return a recursively formatted string representation of a Lua table.
