--[[
  aelkey.filter
  Low-pass (EMA/EMA2) and configurable high-pass filters.

  This module provides:
    • Simple low-pass filters (EMA and EMA2), each maintaining independent state
    • Configurable high-pass filters built on top of user-selected low-pass functions

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
--]]

local M = {}

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

return M
