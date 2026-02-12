# Ælkey <span class="stitch-trim-line"/>

## General API

### Event Loop Control

- `start()` - enter blocking event loop for remapping.
- `stop()` - terminate the running event loop gracefully, typically in response to a specific input event or condition.

### Device Management

- `open_device([dev_id])` - initialize specified device, all if none specified.
- `close_device([dev_id])` - release specified device, all if none specified.
- `get_device_info(dev_id)` - query metadata (VID, PID, bus type, name, serial/MAC).

### Linux Input Events (`aelkey.evdev`)

- `send(event)` - send an event to a virtual output device.
- `sync(dev_id)` - flush a frame (`SYN_REPORT`) to complete a batch of emitted events.

### HID Feature Control (`aelkey.hid`)

- `get_feature_report(dev_id, report_id)` - synchronous feature report read.
- `get_report_descriptor(dev_id)` - synchronous report descriptor read.
- `read_input_report(dev_id)` - single raw input read (hidraw only).
- `send_feature_report(dev_id, data)` - synchronous feature report write.
- `send_output_report(dev_id, data)` - send one HID output report.

### Bluetooth Low Energy Generic Attribute Profile (`aelkey.gatt`)

- `read{device[, service, characteristic]}` - synchronous read from a characteristic.
- `write{device, data [, response] [, service, characteristic]}` - write to a characteristic (default `response = false`).

### USB Transfer Requests (`aelkey.usb`)

Synchronous transfers return `{device, data, size, status}`.

- `bulk_transfer{device, endpoint, size, [timeout]}` - Request high‑throughput data.
- `control_transfer{device, request_type, request, value, index, length, [timeout]}` - Send commands and configuration requests.
- `interrupt_transfer{device, endpoint, size, [timeout]}` - Handle periodic, latency‑sensitive messages.

Asynchronous transfers have additional fields `{..., endpoint, transfer}`.

- `submit_transfer{device, endpoint, type, size, timeout}` - Request an asynchronous transfer.

Configuration functions return `{device, status}`.

- `clear_halt{device, endpoint}` - Clear a stalled endpoint so transfers can resume.
- `reset_device{device}` - Perform a USB‑level device reset.
- `set_configuration{device, config}` - Select a USB configuration.
- `set_interface_alt_setting{device, interface, alt}` - Select an alternate interface setting to use a different endpoint layout.

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

### Watchlist and Script Info (`aelkey.monitor`)

- `watch(ref, decls)` - add a table of input devices for state monitoring; returns the number of valid entries added.
- `unwatch(ref)` - stop monitoring a previously watched set of devices.
- `watchlist()` - list currently watched refs.
- `set_callback(cb)` - set or clear the watchlist callback; returns true on success.
- `inspect_file(path)` - safely load a script from a file for inspection.
- `inspect_string(contents)` - safely load script from a string for inspection.

Note: Only udev compatible types can be watched (evdev, hidraw, libusb).

### Miscellaneous Utilities (`aelkey.util`)

- `crc32_core(data, seed)` - Compute a CRC‑32 iteration with no XORs.
- `crc32_ieee(data [, seed])` - Compute an IEEE CRC‑32 checksum (with initial and final XOR).
- `dump_events(events)` - Return a formatted string describing a list of input events.
- `dump_hex(bytes)` - Return a hex‑dump a binary blob or array of bytes.
- `dump_raw(data)` - Return a hex‑dump string of an hidraw report.
- `dump_table(table)` - Return a recursively formatted string representation of a Lua table.
- `now([resolution])` - Return current monotonic time in milliseconds, or in `us`/`ns` if specified.
- `tick(ms, callback, [oneshot])` - Schedule periodic ticks (e.g. timers inside the loop).  `callback` is a function name or reference.  `oneshot` is a bool.
- `bench_scope(label)` - Measure execution time and the interval between calls.

  Usage for Lua 5.1+:
  ```lua
  function remap(events)
    local scope = aelkey.util.bench_scope("remap")
    -- remap code
    scope.finish()
  end
  ```

  Usage for Lua 5.4+:
  ```lua
  function remap(events)
    local scope <close> = aelkey.util.bench_scope("remap")
    -- remap code
  end
  ```
