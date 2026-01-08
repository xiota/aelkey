--[[
  aelkey.filter
  Low-pass (EMA/EMA2), configurable high-pass filters, and easing utilities.

  This module provides:
    • Simple low-pass filters (EMA and EMA2), each maintaining independent state
    • Configurable high-pass filters built on top of user-selected low-pass functions
    • A general easing interface (0–1 directional model) for smooth, reversible transitions

  Lowpass
  -------
  Usage:
    aelkey.filter.lowpass_ema(id, new, alpha)
    aelkey.filter.lowpass_ema2(id, new, alpha)
    aelkey.filter.reset(id)
    aelkey.filter.reset_all()

  Notes:
    • Each low-pass filter maintains its own state keyed by id

  Highpass
  --------
  Usage:
    aelkey.filter.highpass_configure{
      id       = "accel_hp",
      lp_fn    = aelkey.filter.lowpass_ema,
      lp_param = 0.1
    }

    aelkey.filter.highpass("accel_hp", new_value)
    aelkey.filter.reset(id)
    aelkey.filter.reset_all()

  Notes:
    • High-pass auto-generates an internal low-pass id
    • lp_param is forwarded as-is (scalar or table)
    • Easing channels maintain independent state keyed by id

  Easing
  ------
  Usage:
    aelkey.filter.easing(id, target, init, duration, ease_fn, inv_eps) -- initialize (init ~= nil)
    aelkey.filter.easing(id, target, nil, duration, ease_fn, inv_eps) -- update (init == nil)
    aelkey.filter.easing(id) -- query current state; nil, nil when uninitialized

  Notes:
    • Easing channels maintain independent state keyed by id
--]]

local M = {}

------------------------------------------------------------------------
-- Low Pass
------------------------------------------------------------------------

-- Low-pass state tables
local state_ema  = {}
local state_ema2 = {}

-- High-pass config
local hp_config = {}

-- Reset a single state id (low-pass or high-pass LP state)
function M.reset(id)
  state_ema[id]  = nil
  state_ema2[id] = nil
end

-- Reset all states
function M.reset_all()
  for k in pairs(state_ema)  do state_ema[k]  = nil end
  for k in pairs(state_ema2) do state_ema2[k] = nil end
end

-- Get state (for debugging)
function M.get(id)
  return state_ema[id] or state_ema2[id]
end

-- Low-pass: EMA (1-pole)
function M.lowpass_ema(id, new, alpha)
  assert(type(alpha) == "number", "lowpass_ema: alpha must be a number")

  local prev = state_ema[id]
  if not prev then
    state_ema[id] = new
    return new
  end

  local out = prev + alpha * (new - prev)
  state_ema[id] = out
  return out
end

-- Low-pass: EMA2 (2-pole smoother)
function M.lowpass_ema2(id, new, alpha)
  assert(type(alpha) == "number", "lowpass_ema2: alpha must be a number")

  local s = state_ema2[id]
  if not s then
    state_ema2[id] = { new, new }
    return new
  end

  local prev1, prev2 = s[1], s[2]
  local out = prev1 + alpha * (new - prev1)

  state_ema2[id] = { out, prev1 }
  return out
end

------------------------------------------------------------------------
-- High Pass
------------------------------------------------------------------------

-- Configure a high-pass filter
function M.highpass_configure(cfg)
  assert(type(cfg.id) == "string", "highpass_configure: id must be a string")
  assert(type(cfg.lp_fn) == "function", "highpass_configure: lp_fn must be a function")

  local id    = cfg.id
  local lp_id = "_aelkey_hp_" .. id

  -- Reset internal low-pass state for this high-pass
  M.reset(lp_id)

  hp_config[id] = {
    lp_fn    = cfg.lp_fn,
    lp_param = cfg.lp_param,
    lp_id    = lp_id
  }
end

-- Run a high-pass filter
function M.highpass(id, new)
  local cfg = hp_config[id]
  if not cfg then return new end

  local low = cfg.lp_fn(cfg.lp_id, new, cfg.lp_param)
  return new - low
end

------------------------------------------------------------------------
-- Easing
------------------------------------------------------------------------

-- Internal easing state
local easing_state = {}

-- Time source (microseconds)
local function now_us()
  return aelkey.util.now("us")
end

-- Binary search inversion threshold
local INV_EPS = 1e-5

-- Shaping functions (monotonic)
function M.ease_linear(t)
  return t
end

function M.ease_exp(t)
  return 1 - math.exp(-5 * t)
end

function M.ease_poly(t, n)
  return t ^ n
end

function M.ease_smoothstep(t)
  return t * t * (3 - 2 * t)
end

-- Invert a monotonic easing function via binary search
local function invert_ease(fn, y, inv_eps)
  local lo, hi = 0, 1
  local mid = 0
  local iter = 0

  while hi - lo > inv_eps do
    iter = iter + 1
    mid = 0.5 * (lo + hi)
    local v = fn(mid)

    if v < y then
      lo = mid
    else
      hi = mid
    end
  end

  if aelkey.log.is_enabled("trace") then
    aelkey.log.trace("easing invert: iterations=%d, y=%.6f, t=%.6f", iter, y, mid)
  end

  return mid
end

-- Compute normalized value from current absolute value
local function compute_normalized(init, target, value)
  local span = target - init
  if span == 0 then
    return 1
  end
  return (value - init) / span
end

-- Main easing function
function M.easing(id, target, init, duration, ease_fn, inv_eps, ...)
  local st = easing_state[id]
  local tnow = now_us()

  -- Query only
  if target == nil and init == nil and duration == nil and ease_fn == nil then
    if not st then
      aelkey.log.warn("easing: query on uninitialized id '%s'", id)
      return nil, nil
    end

    if not st.duration or not st.ease_fn or not st.start_time or not st.init or not st.target then
      aelkey.log.warn("easing: query on broken state for id '%s'", id)
      return nil, nil
    end

    local dt = tnow - st.last_time
    st.last_time = tnow

    local tnorm = (tnow - st.start_time) / st.duration
    if tnorm < 0 then tnorm = 0 end
    if tnorm > 1 then tnorm = 1 end

    local y = st.ease_fn(tnorm, ...)
    local value = st.init + (st.target - st.init) * y
    st.last_value = value

    return value, dt
  end

  -- Initialization or reset (init provided)
  if init ~= nil then
    if not ease_fn and st then ease_fn = st.ease_fn end
    if not duration and st then duration = st.duration end
    if not target and st then target = st.target end

    if target ~= 0 and target ~= 1 then
      aelkey.log.warn("easing: non-binary target %s; inferring direction", tostring(target))
      if target > init then
        target = 1
      elseif target < init then
        target = 0
      else
        target = 1
      end
    end

    easing_state[id] = {
      init       = init,
      target     = target,
      duration   = duration,
      ease_fn    = ease_fn,
      inv_eps = inv_eps or (st and st.inv_eps) or INV_EPS,
      start_time = tnow,
      last_time  = tnow,
      last_value = init
    }

    return init, 0
  end

  -- Continuation update (no init)
  if not st then
    aelkey.log.warn("easing: update on uninitialized id '%s'", id)
    return nil, nil
  end

  local cur_value = st.last_value
  local cur_init  = st.init
  local cur_target = st.target
  local cur_duration = st.duration
  local cur_fn = st.ease_fn

  local new_target = target or cur_target
  local new_duration = duration or cur_duration
  local new_fn = ease_fn or cur_fn
  local new_inv_eps = inv_eps or st.inv_eps

  if new_target ~= 0 and new_target ~= 1 then
    aelkey.log.warn("easing: non-binary target %s; inferring direction", tostring(new_target))
    if new_target > cur_value then
      new_target = 1
    elseif new_target < cur_value then
      new_target = 0
    else
      new_target = cur_target
    end
  end

  local norm = compute_normalized(cur_init, cur_target, cur_value)

  if new_target ~= cur_target then
    norm = 1 - norm
  end

  local t0 = invert_ease(new_fn, norm, new_inv_eps)
  local new_start = tnow - t0 * new_duration

  st.init       = cur_value
  st.target     = new_target
  st.duration   = new_duration
  st.ease_fn    = new_fn
  st.inv_eps    = new_inv_eps
  st.start_time = new_start
  st.last_time  = tnow
  st.last_value = cur_value

  return cur_value, 0
end

return M
