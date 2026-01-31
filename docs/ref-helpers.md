# Ælkey <span class="ak-trim-line"/>

## Input and Other Helpers

### `aelkey.click`

Detects single, double, and triple clicks.
- `configure{window=300, interval=20}`
- `detect(id, single_fn, double_fn, triple_fn)`
- `reset()`

### `aelkey.edge`

Detects state changes (edges) in continuously reported events.
- `configure{active_reference=false}`
- `detect(id, pressed, press_fn, release_fn)` - return `true`/`false` on edge, otherwise `nil`.
- `get_active()` - return table of actively pressed buttons
- `reset([id])` - clears internal state for `id` or all if `nil`

### `aelkey.filter` (lowpass)

- `lowpass_ema(id, new, alpha)`
- `lowpass_ema2(id, new, alpha)`
- `reset(id)`
- `reset_all()`

### `aelkey.filter` (highpass)

- `highpass_configure{id="accel_hp", lp_fn=lowpass_ema, lp_param=0.1}`
- `highpass("accel_hp", new_value)`
- `reset(id)`
- `reset_all()`

### `aelkey.filter` (easing)

- `easing(id)` -- Query current state. Return `nil, nil` when uninitialized.
- `easing(id, target, init, duration, ease_fn, inv_eps, ...)` -- Initialize when `init ~= nil`.  Otherwise, update.

Available `ease_fn` functions:

- `ease_exp(t)`
- `ease_linear(t)`
- `ease_poly(t, n)`
- `ease_smootherstep(t)`
- `ease_smoothstep(t)`

### `aelkey.keyboard`

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

### `aelkey.mouse`

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

### `aelkey.ticker`

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

### `aelkey.sequence`

Detects button sequences, such as numeric codes.

- `new{...}` - create new instance, same options as configure
- `configure{window=500, interval=20, stream=false}`
- `add_pattern(pattern)`
- `clear_patterns()`
- `detect(button, match_fn, timeout_fn, start_fn)`
- `reset()` - clear state, patterns preserved

### `aelkey.tracker`

Minimal active-set tracker with press/release callbacks.

- `new{ on_press = press_fn, on_release = release_fn }` - create new instance
- `press(code)`
- `release(code)`
- `release_all()`
- `each_active(active_fn)`

### `aelkey.touchpad`

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
