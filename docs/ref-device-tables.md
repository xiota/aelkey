# Ælkey <span class="stitch-trim-line"/>

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
    type       = "<string>", -- Device type: audio, evdev, gatt, hidraw, libusb, midi
    grab       = <bool>,     -- Attempt exclusive access

    -- for matching --
    name       = "<string>",     -- Device name
    bus        = "<string>",     -- Bus type ("usb", "bluetooth")
    vid_pid    = { {<int>, <int>}, ... }
    interfaces = { <int>, ... }  -- HID interface indices (libusb)

    -- callbacks --
    on_event   = "<string>", -- Function name to receive event frames
    on_state   = "<string>", -- Function name to receive connect/disconnect notifications

    ----- gatt -----
    serv_char       = { { <int>, <int> }, ... },  -- service, characteristic pairs
    services        = { <int>, ... },             -- service handles
    characteristics = { <int>, ... },             -- characteristic handles
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
    type       = "<string>", -- Virtual device type: audio, midi, uhid, uinput

    -- for matching --
    name       = "<string>", -- Device name

    -- callbacks --
    on_haptics = "<string>", -- Function name to receive haptic events (uinput)
    on_report = "<string>",  -- Function name to receive host-to-device reports (uhid)

    ----- uhid -----
    bus          = "<string>", -- Bus type ("usb", "bluetooth"; default: "usb")
    vendor       = <int>,      -- Vendor ID
    product      = <int>,      -- Product ID
    version      = <int>,      -- Version identifier
    report_desc  = "<binary string>", -- Raw HID report descriptor bytes

    ----- uinput -----
    profile    = "digitizer" | "imu" | "keyboard" | "mouse" |
                 "touchpad" | "touchpad_mt" | "touchscreen",
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

#### `gatt` events

The gatt event callback receives a single table, similar to hidraw, but with additional metadata fields.

```lua
{
  device   = "<id string>",     -- decl.id
  data     = "<binary string>", -- raw USB data payload
  size     = <int>,             -- size of data payload in bytes
  status   = "<string>",        -- "ok", "error"

  path     = "<characteristic path>",
}
```

#### `hidraw` events

The hidraw event callback receives a single table.

```lua
{
  device   = "<id string>",     -- decl.id
  data     = "<binary string>", -- raw HID report payload
  size     = <int>,             -- number of bytes read
  status   = "<string>",        -- completion status ("ok", "error")
}
```

#### `libusb` events

The libusb event callback receives a single table, similar to hidraw, but with additional metadata fields.

```lua
{
  device   = "<id string>",     -- decl.id
  data     = "<binary string>", -- raw USB data payload
  size     = <int>,             -- size of data payload in bytes
  status   = "<string>",        -- completion status ("ok", "stall", "timeout", etc.)

  endpoint = <int>,             -- numeric endpoint address (e.g. 0x81)
  transfer = "<string>",        -- transfer type ("control", "interrupt", "bulk", "iso")
}
```

#### `uhid` report events

```lua
{
  device = "<id string>",        -- decl.id
  type   = "output" | "feature", -- Report type sent by the host
  data   = "<binary string>",    -- Raw HID report payload
  size   = <int>,                -- Payload size in bytes
  status = "<string>",           -- "ok" or "error"
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
