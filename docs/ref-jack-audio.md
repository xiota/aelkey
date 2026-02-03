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

### Tables

```lua
inputs = {
  {
    ----- Common -----
    id         = "<string>", -- Unique identifier used in events and callbacks
    type       = "midi",

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
    type       = "midi",

    -- for matching --
    name       = "<string>", -- Device name

    ----- midi -----
    port       = "<string>", -- JACK port name override,   default: "<id>"
  },
}
```

The MIDI event callback receives a table of message tables.

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

Port information from `aelkey.jack.get_port_info(port)` is returned in a table.

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
