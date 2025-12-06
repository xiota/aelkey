--[[
  aelkey.click
  Single/double/triple‑click helper module with flush support.

  Usage:
    aelkey.click.configure{window=300, interval=20}
    aelkey.click.detect(button, single_fn, double_fn, triple_fn)
    aelkey.click.reset()

  Notes:
    • Uses aelkey.tick() heartbeat for timing
    • Flushes a pending single immediately if a different button is pressed
    • Emits clicks immediately when no higher level is defined
]]--

local M = {}

-- Timing
local tick_count = 0   -- increments each heartbeat tick
local click_window_ms = 250
local click_interval = 15
local click_threshold = math.floor(click_window_ms / click_interval)

-- Clicks
local click_count = 0   -- number of clicks seen in current window
local click_pending = false
local click_last_code = nil

-- Actions
local single_action = nil
local double_action = nil
local triple_action = nil

-- Internal: recompute threshold
local function update_threshold()
  click_threshold = math.floor(click_window_ms / click_interval)
end

-- Heartbeat handler
local function click_handler(n)
  tick_count = tick_count + 1
  if click_pending and tick_count > click_threshold then
    if click_count == 1 and single_action then
      single_action(click_last_code)
    elseif click_count == 2 and double_action then
      double_action(click_last_code)
    elseif click_count == 3 and triple_action then
      triple_action(click_last_code)
    end
    M.reset()
  end
end

-- Reset state
function M.reset()
  tick_count = 0
  click_count = 0
  click_pending = false
  click_last_code = nil
  single_action = nil
  double_action = nil
  triple_action = nil
  aelkey.tick(0, click_handler) -- stop heartbeat
end

-- Configure both window and interval
function M.configure(opts)
  if opts.window then click_window_ms = opts.window end
  if opts.interval then click_interval = opts.interval end
  update_threshold()
end

-- Entry point
function M.detect(button, single, double, triple)
  -- Different button flush
  if click_pending and button ~= click_last_code then
    if click_count == 1 and single_action then single_action(click_last_code)
    elseif click_count == 2 and double_action then double_action(click_last_code) end
    M.reset()
  end

  -- First click
  if not click_pending then
    single_action   = single
    double_action   = double
    triple_action   = triple
    click_last_code = button
    click_count     = 1
    tick_count      = 0
    click_pending   = true

    if not double_action and not triple_action then
      if single_action then single_action(button) end
      M.reset()
      return
    end

    aelkey.tick(click_interval, click_handler)
    return
  end

  -- Subsequent clicks (same button, within window)
  if tick_count <= click_threshold then
    click_count = click_count + 1
    if click_count == 2 and not triple_action then
      if double_action then double_action(button) end
      M.reset()
    elseif click_count == 3 then
      if triple_action then
        triple_action(button)
        M.reset()
      end
    end
  end
end

-- Return module
return M
