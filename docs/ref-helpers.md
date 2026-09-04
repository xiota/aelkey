# Ælkey <span class="stitch-trim-line"/>

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
