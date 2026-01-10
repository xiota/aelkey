--[[
  aelkey.tracker
  Minimal active-set tracker with press/release callbacks.

  Usage:
    local st = aelkey.state.new{
      on_press   = function(code) ... end,
      on_release = function(code) ... end,
    }

    st.press("KEY_A")
    st.release("KEY_A")
    st.release_all()
    st.each_active(function(code) ... end)
]]--

-- Internal logic (free functions)
local function press(self, code)
  if not self.active[code] then
    self.active[code] = true
    if self.on_press then
      self.on_press(code)
    end
  end
end

local function release(self, code)
  if self.active[code] then
    self.active[code] = nil
    if self.on_release then
      self.on_release(code)
    end
  end
end

local function release_all(self)
  for code, _ in pairs(self.active) do
    self.active[code] = nil
    if self.on_release then
      self.on_release(code)
    end
  end
end

local function each_active(self, fn)
  for code, _ in pairs(self.active) do
    fn(code)
  end
end

-- Instance constructor
local function new(opts)
  local self = {
    active     = {},
    on_press   = opts and opts.on_press   or nil,
    on_release = opts and opts.on_release or nil,
  }

  -- Wrapped methods (no ":" syntax)
  self.press       = function(code) return press(self, code) end
  self.release     = function(code) return release(self, code) end
  self.release_all = function()     return release_all(self) end
  self.each_active = function(fn)   return each_active(self, fn) end

  return self
end

-- Global instance
local M = new()
M.new = new

return M
