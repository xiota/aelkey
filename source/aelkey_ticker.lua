--[[
  aelkey.ticker
  Periodic action driver with start/stop lifecycle.
  Supports standalone instances and a global id‑based registry.

  Usage (Instance):
    local t = aelkey.ticker.new{
      id        = "scroll_up",   -- optional
      interval  = 20,            -- override default
      immediate = false,          -- fire once immediately
      fn        = function() ... end,
    }
    t.start()
    t.stop()

  Usage (Global Registry):
    aelkey.ticker.set{
      id        = "scroll_up",
      interval  = 20,
      immediate = false,
      fn        = function() ... end,
    }
    aelkey.ticker.start("scroll_up")
    aelkey.ticker.stop("scroll_up")

  Notes:
    • Frontend for aelkey.tick()
    • Each ticker instance has its own interval, immediate flag, and callback
    • set{} creates or updates the instance for that id
    • Anonymous callbacks are safe (stable function references)
    • Registry is private; access only through set/start/stop
]]--

-- Internal logic (free functions)
local function reset(self)
  aelkey.tick(0, self.handler)
  self.active = false
end

local function start(self)
  if self.active then return end
  self.active = true

  -- Optional immediate fire
  if self.immediate and self.fn then
    self.fn()
  end

  aelkey.tick(self.interval_ms, self.handler)
end

local function stop(self)
  if not self.active then return end
  self.active = false
  aelkey.tick(0, self.handler)
end

local function set(registry, opts)
  if not opts or not opts.id then
    print("aelkey.ticker warning: set() requires id")
    return
  end

  local id = opts.id
  local inst = registry[id]

  if not inst then
    inst = M.new(opts)
    registry[id] = inst
  else
    if opts.fn        then inst.fn          = opts.fn        end
    if opts.interval  then inst.interval_ms = opts.interval  end
    if opts.immediate ~= nil then inst.immediate = opts.immediate end
  end

  return inst
end

local function start_global(registry, id)
  local inst = registry[id]
  if inst then inst.start() end
end

local function stop_global(registry, id)
  local inst = registry[id]
  if inst then inst.stop() end
end

-- Instance constructor
local function new(opts)
  local self = {
    id          = opts and opts.id or nil,
    interval_ms = opts and opts.interval or 20,
    immediate   = (opts and opts.immediate ~= nil) and opts.immediate or false,
    fn          = opts and opts.fn or nil,
    active      = false,
  }

  -- Stable per-instance handler
  self.handler = function()
    if self.active and self.fn then
      self.fn()
    end
  end

  -- Wrapped methods
  self.start = function() return start(self) end
  self.stop  = function() return stop(self) end
  self.reset = function() return reset(self) end

  return self
end

-- Private registry
local instances = {}

-- Public module
M = {}

M.new   = new
M.set   = function(opts) return set(instances, opts) end
M.start = function(id) return start_global(instances, id) end
M.stop  = function(id) return stop_global(instances, id) end

return M
