# Ælkey <span class="stitch-trim-line"/>

## JACK Audio and MIDI Routing

MIDI messages are routed through JACK.  For system-wide connectivity, use PipeWire.

**`aelkey.jack`**

- `set_client_name(name)` – set client name before initialization
- `get_client_name()` – return current client name or nil
- `list_ports([type], [flags])` – list ports
- `match_ports(pattern)` – list ports with names matching a pattern
- `connect(src, dst)` – connect two ports
- `disconnect(src, dst)` – disconnect two ports
- `get_port_info(port)` – return metadata about a port

**`aelkey.midi`**

- `send(dev_id, data)` – send a message through the specified output device.
  `data` may be a table of bytes or a raw binary string.

- `decode(data)` - return a table of message tables
- `encode(message_table)` - return a binary string suitable for use with `send()`.

- `NOTES[num]` - note number to name, `NOTES[60] = "C4"`.  Flat names are not mapped.
- `NOTE_NAMES[str]` - note name to number, `NOTE_NAMES["C4"] = 60.  Flat names are not mapped.

### Tables

```lua
inputs = {
  {
    ----- Common -----
    id         = "<string>", -- Unique identifier used in events and callbacks
    type       = "audio" | "midi",

    -- for matching --
    name       = "<string>", -- Device name

    -- callbacks --
    on_event   = "<string>", -- Function name to receive event frames

    ----- midi -----
    port       = "<string>", -- JACK port name override,   default: "<id>"
  },
}
```

```lua
outputs = {
  {
    ----- Common -----
    id         = "<string>", -- Unique identifier used in events and callbacks
    type       = "audio" | "midi",

    -- for matching --
    name       = "<string>", -- Device name

    ----- midi -----
    port       = "<string>", -- JACK port name override,   default: "<id>"
  },
}
```

The `audio` event callback receives a table of audio-block tables.

```lua
{
  [1] = {
    device    = "<id string>",     -- InputDecl.id
    data      = "<binary string>", -- raw float32 PCM samples
    size      = <int>,             -- size in bytes
    status    = "ok",

    frames    = <int>,             -- number of float32 frames
    timestamp = <uint64>,          -- microseconds (host monotonic clock)
  },
  [2] = { ... },
  ...
}
```

The `midi` event callback receives a table of message tables.

```lua
{
  [1] = {
    device    = "<id string>",     -- decl.id
    data      = "<binary string>", -- MIDI message data payload
    size      = <int>,             -- size of data payload in bytes
    status    = "<string>",        -- completion status

    timestamp = <int>,             -- when event was received (microseconds)
  },
  [2] = { ... },
  ...
}
```

Port information from `jack.get_port_info(port)` is returned in a table.

```lua
{
  name = "Client:Port",
  type = "8 bit raw midi",
  flags = {
    input = <bool>,
    output = <bool>,
    physical = <bool>,
    terminal = <bool>,
    can_monitor = <bool>,
  },
  aliases = { "alias1", "alias2", ... },
  connections = { "OtherClient:Port", ... },
}
```

Message types and tables created by `midi.decode()` and used by `midi.encode()`:

```lua
{
  [1] = { type="note_off", channel=chan, note=d1, velocity=d2 },
  [2] = { type="note_on", channel=chan, note=d1, velocity=d2 },
  [3] = { type="poly_aftertouch", channel=chan, note=d1, pressure=d2 },
  [4] = { type="cc", channel=chan, controller=d1, value=d2 },
  [5] = { type="pitchbend", channel=chan, value=value },
  [6] = { type="program_change", channel=chan, program=d1 },
  [7] = { type="channel_aftertouch", channel=chan, pressure=d1 },
  [8] = { type = "sysex", data = payload },
}
```
