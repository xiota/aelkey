# Ælkey API Reference

A programmable input remapping and device‑control framework for Lua.

## Device Tables

Devices to are defined in globally scoped tables.

### Inputs

Each entry in the `inputs` table describes one physical device to attach.
This table must exist in the global scope so the event loop can access it during initialization.

```lua
inputs = {
  {
    ----- Common -----
    id         = "<string>", -- Unique identifier used in events and callbacks
    type       = "<string>", -- Device type: evdev, gatt, hidraw, libusb
    grab       = <bool>,     -- Attempt exclusive access

    -- for matching --
    name       = "<string>", -- Device name for matching
    bus        = "<string>", -- Bus type ("usb", "bluetooth")
    vendor     = <int>,
    product    = <int>,
    interface  = <int>,      -- HID interface index (libusb)

    -- callbacks --
    on_event   = "<string>", -- Function name to receive event frames
    on_state   = "<string>", -- Function name to receive connect/disconnect notifications

    ----- gatt -----
    service        = <int>, -- GATT service handle
    characteristic = <int>, -- GATT characteristic handle
  },
}
```

Notes:

* GATT devices do not notify for state changes and cannot be added to the watchlist.

### Outputs

Each entry in the global `outputs` table defines a virtual device.
This table must exist in the global scope so the event loop can access it during initialization.

```lua
outputs = {
  {
    ----- Common -----
    id         = "<string>", -- Unique identifier used in events and callbacks
    type       = "<string>", -- predefined uinput type

    -- callbacks --
    on_haptics = "<string>", -- function name to receive haptic events

    ----- uinput -----
    -- type = "digitizer" | "imu" | "keyboard" | "mouse" |
              "touchpad" | "touchpad_mt" | "touchscreen",
    name       = "<string>", -- device name for matching
    bus        = "<string>", -- bus type ("usb", "bluetooth")
    vendor     = <int>,
    product    = <int>,
    version    = <int>,      -- version identifier
    capabilities = { <string>, ... },  -- optional
  },
}
```

### Event callback tables

#### `evdev` events

The evdev event callback receives an table of event tables.

```lua
{
  [1] = {
    device    = "<id string>",
    type      = "<string>",  -- e.g. "EV_KEY"
    code      = "<string>",  -- e.g. "KEY_A"
    value     = <int>,       -- event value
    sec       = <int>,       -- timestamp seconds
    usec      = <int>,       -- timestamp microseconds
  },
  [2] = { ... },
  ...
}
```

#### `hidraw` events

The hidraw event callback receives a single table.

```lua
{
  device = "<id string>",   -- ctx.decl.id
  data = "<binary string>", -- raw HID report payload
  size   = <int>,           -- number of bytes read
  status = "<string>",      -- completion status ("ok", "error")
}
```

#### `libusb` events

The libusb event callback receives a single table, similar to hidraw, but with additional metadata fields.

```lua
{
  device   = "<id string>",     -- ctx.decl.id
  data     = "<binary string>", -- raw USB data payload
  size     = <int>,             -- size of data payload in bytes
  status   = "<string>",        -- completion status ("ok", "stall", "timeout", etc.)

  endpoint = <int>,             -- numeric endpoint address (e.g. 0x81)
  transfer = "<string>",        -- transfer type ("control", "interrupt", "bulk", "iso")
}
```

#### `gatt` events

The gatt event callback receives a single table, similar to hidraw, but with additional metadata fields.

```lua
{
  device   = "<id string>",     -- ctx.decl.id
  data     = "<binary string>", -- raw USB data payload
  size     = <int>,             -- size of data payload in bytes
  status   = "<string>",        -- "ok", "error"

  path     = "<characteristic path>",
}
```

### state notifications

The state callback receives a single table.

```lua
{
  device = "<id string>",
  state  = "<string>",    -- "connect", "disconnect"
}
```

## API

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

### Input and Other Helpers

#### `aelkey.click`

Detects single, double, and triple clicks.
- `configure{window=300, interval=20}`
- `detect(id, single_fn, double_fn, triple_fn)`
- `reset()`

#### `aelkey.edge`

Detects state changes (edges) in continuously reported events.
- `configure{active_reference=false}`
- `detect(id, pressed, press_fn, release_fn)` - return `true`/`false` on edge, otherwise `nil`.
- `get_active()` - return table of actively pressed buttons
- `reset([id])` - clears internal state for `id` or all if `nil`

#### `aelkey.filter` (lowpass)

- `lowpass_ema(id, new, alpha)`
- `lowpass_ema2(id, new, alpha)`
- `reset(id)`
- `reset_all()`

#### `aelkey.filter` (highpass)

- `highpass_configure{id="accel_hp", lp_fn=lowpass_ema, lp_param=0.1}`
- `highpass("accel_hp", new_value)`
- `reset(id)`
- `reset_all()`

#### `aelkey.filter` (easing)

- `easing(id)` -- Query current state. Return `nil, nil` when uninitialized.
- `easing(id, target, init, duration, ease_fn, inv_eps, ...)` -- Initialize when `init ~= nil`.  Otherwise, update.

Available `ease_fn` functions:

- `ease_exp(t)`
- `ease_linear(t)`
- `ease_poly(t, n)`
- `ease_smootherstep(t)`
- `ease_smoothstep(t)`

#### `aelkey.keyboard`

Keyboard report parsing and event remapping with Fn‑layer support.

- `new(opts)` – Create a keyboard remapper with `normal_map`, `modifier_map`, and `fn_map`.
- `begin_frame()` – Start a new frame and clear the pending output buffer.
- `feed_events(evlist)` – Process a batch of EVDEV key events and generate mapped output.
- `feed_key(event)` – Process a single EVDEV key event.
- `end_frame()` – Finish the frame (reserved for future logic).
- `emit_events(dev_id)` – Emit all buffered mapped events to the given virtual device.
- `get_pending_events()` – Return a copy of buffered events without clearing them.
- `set_fn_down(boolean)` – Force Fn‑mode on or off externally.
- `get_fn_down()` – Return current Fn‑mode state.

Event tables contain evdev-compatible fields: `type`, `code`, and `value`.

Remap tables (`normal_map`, `modifier_map`, `fn_map`) map physical key codes to output codes.  Values may be a string `"KEY_X"`, a list `{ "KEY_X", "KEY_Y" }`, an empty list to suppress `{}`, or omitted / `nil` for identity fallback (Fn inactive only). `modifier_map` always takes priority; `fn_map` applies only when Fn (`KEY_FN`) is active.

#### `aelkey.mouse`

Mouse report parsing and emulation.
- `parse_report(data)` - Returns report table.
- `emit_events(dev_id, report)`
- `emit_motion(dev_id, report)`
- `emit_buttons(dev_id, report)`
- `emit_wheels(dev_id, report)`

Report table:
```
{
  left   = false,
  right  = false,
  middle = false,
  side   = false,
  extra  = false,
  back   = false,
  forward = false,

  dx = 0,
  dy = 0,

  wheel_vert = 0,
  wheel_horz = 0,
}
```

#### `aelkey.ticker`

Repeats events at specified interval.

**Usage (Instance):**

```lua
local t = aelkey.ticker.new{
  id        = "scroll_up",   -- optional
  interval  = 20,            -- override default
  immediate = false,          -- fire once immediately
  fn        = function() ... end,
}
t.start()
t.stop()
```

**Usage (Global Registry):**

```lua
aelkey.ticker.set{
  id        = "scroll_up",
  interval  = 20,
  immediate = false,
  fn        = function() ... end,
}
aelkey.ticker.start("scroll_up")
aelkey.ticker.stop("scroll_up")
```

#### `aelkey.sequence`

Detects button sequences, such as numeric codes.

- `new{...}` - create new instance, same options as configure
- `configure{window=500, interval=20, stream=false}`
- `add_pattern(pattern)`
- `clear_patterns()`
- `detect(button, match_fn, timeout_fn, start_fn)`
- `reset()` - clear state, patterns preserved

#### `aelkey.tracker`

Minimal active-set tracker with press/release callbacks.

- `new{ on_press = press_fn, on_release = release_fn }` - create new instance
- `press(code)`
- `release(code)`
- `release_all()`
- `each_active(active_fn)`

#### `aelkey.touchpad`

Basic and multi-touch touchpad emulation.

```lua
  local tp = aelkey.touchpad.new{
    mode = "multitouch",   -- or "basic"
    max_slots = 5,
  }

  tp.begin_frame()

  tp.feed_contact{
    slot = 0,          -- required
    x = 12000,         -- required
    y = 8000,          -- required
    touching = true,   -- optional (inferred if missing)
    pressure = 42,     -- optional
    palm = false,      -- optional
    button_left = false,
    button_right = false,
    button_middle = false,
  }

  tp.end_frame()

  tp.emit_events("virt_touchpad")
  aelkey.syn_report("virt_touchpad")
```
