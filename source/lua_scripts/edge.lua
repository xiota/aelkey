--[[
  aelkey.edge
    State change (edge) detector for continuously reported events.

  Usage:
    aelkey.edge.configure{ active_reference=false }
    aelkey.edge.detect(id, pressed, press_fn, release_fn)
    aelkey.edge.get_active()
    aelkey.edge.reset([id])
]]--

local M = {}

-- Internal state: id → boolean (true = pressed, false = released)
local last_state = {}

-- Active set: id → true
local active = {}

-- Configuration
local config = {
  active_reference = false,
}

-- configure{ active_reference = bool }
function M.configure(opts)
  if type(opts) ~= "table" then
    aelkey.log.warn("edge.configure: expected table, got %s", type(opts))
    return
  end

  if opts.active_reference ~= nil then
    if type(opts.active_reference) ~= "boolean" then
      aelkey.log.warn("edge.configure: active_reference must be boolean, got %s",
           type(opts.active_reference))
    else
      config.active_reference = opts.active_reference
    end
  end
end

-- detect(id, pressed, press_fn, release_fn)
-- Returns:
--   true  = rising edge (press)
--   false = falling edge (release)
--   nil   = no edge
function M.detect(id, pressed, press_fn, release_fn)
  -- Validate id
  if id == nil then
    aelkey.log.warn("edge.detect: id cannot be nil")
    return nil
  end

  -- Warn on non-boolean, but use Lua semantics
  if type(pressed) ~= "boolean" then
    aelkey.log.warn("edge.detect: pressed must be boolean, got %s", type(pressed))
  end

  -- Normalize using Lua truthiness (no coercion)
  local current = not not pressed

  local prev = last_state[id]

  -- First time seeing this id
  if prev == nil then
    last_state[id] = current
    if current then
      active[id] = true
      if press_fn then press_fn(id) end
      return true
    else
      -- First event is "released" → no edge
      return nil
    end
  end

  -- No change → no edge
  if prev == current then
    return nil
  end

  -- State changed → edge detected
  last_state[id] = current

  if current then
    -- Rising edge
    active[id] = true
    if press_fn then press_fn(id) end
    return true
  else
    -- Falling edge
    active[id] = nil
    if release_fn then release_fn(id) end
    return false
  end
end

-- get_active()
-- Returns:
--   - reference to internal table if active_reference=true
--   - shallow copy otherwise
function M.get_active()
  if config.active_reference then
    return active
  end

  local copy = {}
  for id in pairs(active) do
    copy[id] = true
  end
  return copy
end

-- reset([id])
-- Clears internal state for id or all if nil.
-- Returns true if something was reset, false otherwise.
function M.reset(id)
  if id == nil then
    -- Reset all
    local had_any = next(last_state) ~= nil
    last_state = {}
    active = {}
    return had_any
  end

  -- Reset specific id
  if last_state[id] ~= nil then
    last_state[id] = nil
    active[id] = nil
    return true
  end

  return false
end

return M
