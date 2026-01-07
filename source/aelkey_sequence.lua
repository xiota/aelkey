--[[
  aelkey.sequence
  Blind multi-button sequence matcher with optional stream mode.

  Usage:
    local seq = aelkey.sequence.new{window=500, interval=20, stream=true}
    seq.add_pattern({1,2,3,4})
    seq.detect(button, match_fn, timeout_fn, start_fn)
    seq.reset()

  Usage (Global):
    aelkey.sequence.configure{window=500, interval=20, stream=false}
    aelkey.sequence.add_pattern({1,2,3,4})
    aelkey.sequence.clear_patterns()
    aelkey.sequence.detect(button, match_fn, timeout_fn, start_fn)
    aelkey.sequence.reset()

  Notes:
    • Blind matching: no mismatch feedback
    • First pattern to complete wins
    • Timeout always fires (unless match ends session)
    • stream=true keeps session alive after match (Alt‑Tab style)
    • Uses aelkey.tick() heartbeat like aelkey.click
]]--

-- Internal logic (free functions)
local function configure(self, opts)
  if not opts then return end

  if opts.window   then self.window_ms   = opts.window   end
  if opts.interval then self.interval_ms = opts.interval end
  if opts.stream ~= nil then self.stream = opts.stream end

  self.threshold = math.floor(self.window_ms / self.interval_ms)
end

local function reset(self)
  -- Stop heartbeat
  aelkey.tick(0, self.handler)

  -- If stream=true and session active, force timeout_fn
  if self.stream and self.active and self.timeout_fn then
    self.timeout_fn()
  end

  self.tick_count = 0
  self.active     = false

  for i = 1, #self.progress do
    self.progress[i] = 0
  end

  self.match_fn   = nil
  self.timeout_fn = nil
  self.start_fn   = nil
end

local function add_pattern(self, pat)
  if type(pat) ~= "table" or #pat == 0 then
    print("aelkey.sequence warning: ignoring empty pattern")
    return
  end

  table.insert(self.patterns, pat)
  table.insert(self.progress, 0)
end

local function clear_patterns(self)
  self.patterns = {}
  self.progress = {}
end

local function handler(self)
  self.tick_count = self.tick_count + 1

  if self.active and self.tick_count > self.threshold then
    if self.timeout_fn then
      self.timeout_fn()
      self.timeout_fn = nil
    end
    reset(self)
  end
end

local function detect(self, button, match_cb, timeout_cb, start_cb)
  if #self.patterns == 0 then return end

  -- If no session active, see if this input can start one
  if not self.active then
    local starts_any = false
    local completed  = false

    for i, pat in ipairs(self.patterns) do
      if pat[1] == button then
        self.progress[i] = 1
        starts_any = true

        -- Single-button pattern completes immediately
        if #pat == 1 then
          completed = true
          break
        end
      else
        self.progress[i] = 0
      end
    end

    if not starts_any then
      return
    end

    -- Start new session
    self.active     = true
    self.match_fn   = match_cb
    self.timeout_fn = timeout_cb
    self.start_fn   = start_cb
    self.tick_count = 0

    if self.start_fn then
      self.start_fn()
    end

    -- Start heartbeat
    aelkey.tick(self.interval_ms, self.handler)

    -- If a single-button pattern completed immediately
    if completed and self.match_fn then
      self.match_fn()

    -- Reset progress on matches
      for i = 1, #self.progress do
        self.progress[i] = 0
      end

      if not self.stream then
        self.reset()
      end
      return
    end
  else
    -- Session already active: every input extends timeout
    self.tick_count = 0
  end

  -- Advance patterns for this input
  local completed = false

  for i, pat in ipairs(self.patterns) do
    local idx = self.progress[i]

    if idx > 0 then
      -- Continue an in-progress pattern
      if pat[idx + 1] == button then
        self.progress[i] = idx + 1
        if self.progress[i] == #pat then
          completed = true
          break
        end
      end
    else
      -- Pattern was idle; see if this input can start it
      if pat[1] == button then
        self.progress[i] = 1

        -- Single-button pattern completes immediately
        if #pat == 1 then
          completed = true
          break
        end
      end
    end
  end

  -- Handle completed pattern
  if completed and self.match_fn then
    self.match_fn()

    -- Reset progress on matches
    for i = 1, #self.progress do
      self.progress[i] = 0
    end

    if not self.stream then
      -- non‑stream: match ends session immediately
      self.reset()
    end
  end
end

-- Instance constructor
local function new(opts)
  local self = {
    -- Config
    window_ms   = 500,
    interval_ms = 20,
    stream      = false,
    threshold   = math.floor(500 / 20),

    -- Runtime state
    tick_count  = 0,
    active      = false,

    patterns    = {},
    progress    = {},

    -- Callbacks
    match_fn    = nil,
    timeout_fn  = nil,
    start_fn    = nil,
  }

  -- Stable per-instance handler
  self.handler = function(n)
    handler(self)
  end

  -- Wrapped methods (no self needed at call site)
  self.configure      = function(opts) return configure(self, opts) end
  self.reset          = function() return reset(self) end
  self.add_pattern    = function(pat) return add_pattern(self, pat) end
  self.clear_patterns = function() return clear_patterns(self) end
  self.detect         = function(...) return detect(self, ...) end

  -- Apply initial config
  if opts then self.configure(opts) end

  return self
end

-- Global instance
local M = new()
M.new = new

return M
