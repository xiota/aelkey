# Ælkey <span class="stitch-trim-line"/>

## Force Feedback and Haptics (`aelkey.haptics`)

- `play(dev_id, effect_table)` - Trigger an effect. The table must contain source and id.
- `stop(dev_id, effect_table)` - Stop an effect. The table must contain source and id.
- `create(effect_table)` - Register a custom effect. Returns the table with source and id injected.
- `erase(effect_table)` - Unregister a custom effect.

Notes:

- Haptics events flow backwards, compared with normal input events.  `dev_id` corresponds to devices in the `inputs` table.

    > Game → virtual device (`outputs` table, haptic event) → Lua callback → real device (`inputs` table, actual vibration)

- The haptics pipeline has significant lag when used over Bluetooth.

### haptics event callback table

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

## Effect table format (used by `haptics.create()`)

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
