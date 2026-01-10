--[[
  aelkey.keyboard
  Unified keyboard remapper with optional Fn mode handling.

  Usage:
    local kb = aelkey.keyboard.new{
      normal_map = key_map,
      modifier_map = modifier_map,
      fn_map = fn_map,
    }

    function remap_evdev(ev)
      kb.begin_frame()
      kb.feed_events(ev)

      kb.end_frame()
      kb.emit_events("virt_keyboard")
      aelkey.syn_report("virt_keyboard")
    end

  Functions:
    new(opts)
    begin_frame()
    feed_events(evlist)
    feed_key(event)
    end_frame()
    emit_events(dev_id)
    get_pending_events()
    set_fn_down(boolean)
    get_fn_down()

  Notes:
    • Separate maps for:
        - modifier_map (Ctrl/Alt/Meta/Fn/Caps, etc.)
        - normal_map   (general key remaps)
        - fn_map       (Fn-layer remaps when Fn mode is active)
    • Fn as a mode flag:
        - Fn can be toggled via KEY_FN placeholder mapping
        - Fn can be controlled externally via set_fn_down()
    • Press-time mapping consistency:
        - Mapping is chosen on key press and reused on release
        - Multi-key mappings emit press forward, release reverse
    • EVDEV-friendly:
        - Processes EV_KEY events (value 0/1)
        - Ignores EV_MSC, EV_SYN, and value==2 (auto-repeat)
--]]

local M = {}

-- Internal constants
local KEY_EVENT_TYPE = "EV_KEY"
local FN_CODE = "KEY_FN"

local IDENTITY_MAP = {
  -- Standard modifiers
  KEY_LEFTSHIFT = true,
  KEY_RIGHTSHIFT = true,
  KEY_LEFTCTRL = true,
  KEY_RIGHTCTRL = true,
  KEY_LEFTALT = true,
  KEY_RIGHTALT = true,
  KEY_LEFTMETA = true,
  KEY_RIGHTMETA = true,

  -- Lock keys
  KEY_CAPSLOCK = true,
  KEY_NUMLOCK = true,
  KEY_SCROLLLOCK = true,

  -- International / ISO modifiers
  KEY_COMPOSE = true,
  KEY_ISO_LEVEL3_SHIFT = true,  -- AltGr
  KEY_ISO_LEVEL5_SHIFT = true,  -- Mode switch

  -- Fn
  KEY_FN = true,
}

-- Internal helpers
local function append_event(self, type_name, code_name, value)
  local buf = self.buffer
  local n = #buf + 1
  buf[n] = buf[n] or {}
  local e = buf[n]
  e.type = type_name
  e.code = code_name
  e.value = value
end

-- Determine mapping for a press event, given current Fn state.
-- Returns:
--   mapped_codes: table of code strings, or nil to suppress
--   is_fn_placeholder: true if this mapping should toggle Fn mode only
local function resolve_mapping(self, code_name)
  local mapped

  if self.fn_down then
    mapped = self.modifier_map[code_name]
             or ( IDENTITY_MAP[code_name] and { code_name })
             or self.fn_map[code_name]
  else
    mapped = self.modifier_map[code_name]
             or self.normal_map[code_name]
             or { code_name }
  end

  -- Empty table explicitly means "suppress"
  if type(mapped) == "table" and #mapped == 0 then
    return nil, false
  end

  -- Normalize single-string mapping to table
  if type(mapped) == "string" then
    mapped = { mapped }
  end

  -- Fn placeholder: first mapped code is KEY_FN and used to toggle Fn mode
  local is_fn_placeholder = (mapped[1] == FN_CODE and #mapped == 1)

  return mapped, is_fn_placeholder
end

-- Frame lifecycle
local function begin_frame(self)
  -- Clear output buffer for this frame.
  self.buffer = {}
end

local function feed_events(self, events)
  if not events then
    return
  end

  for _, e in ipairs(events) do
    local type_name = e.type_name or e.type
    local code_name = e.code_name or e.code
    local value = e.value

    -- Only handle EV_KEY presses/releases (value 0/1)
    if type_name == KEY_EVENT_TYPE and (value == 0 or value == 1) then
      if value == 1 then
        -- Key press
        local mapped, is_fn_placeholder = resolve_mapping(self, code_name)

        if is_fn_placeholder then
          -- Toggle Fn mode on press; remember for release
          self.fn_down = true
          self.active_keys[code_name] = { FN_CODE } -- marker
        elseif mapped then
          -- Remember mapping used at press time
          self.active_keys[code_name] = mapped

          -- Emit press events in order
          for _, out_code in ipairs(mapped) do
            append_event(self, KEY_EVENT_TYPE, out_code, 1)
          end
        else
          -- mapped == nil → suppressed key
          -- Do nothing, do not track active_keys
        end

      else
        -- Key release
        local mapped = self.active_keys[code_name]
        self.active_keys[code_name] = nil

        if mapped and mapped[1] == FN_CODE and #mapped == 1 then
          -- Fn placeholder release: turn off Fn mode
          self.fn_down = false

        elseif mapped then
          -- Emit release events in reverse order
          for i = #mapped, 1, -1 do
            append_event(self, KEY_EVENT_TYPE, mapped[i], 0)
          end
        else
          -- Untracked release:
          -- Ignored
        end
      end

    elseif type_name == "EV_MSC" or type_name == "EV_SYN" or value == 2 then
      -- Ignore misc, sync, and auto-repeat by default.
      -- If needed later, this can be made configurable.
    else
      -- Unknown or unsupported event type → ignore.
    end
  end
end

local function end_frame(self)
  -- Currently no additional per-frame logic required.
  -- active_keys must persist across frames.
  -- buffer stays as-is until emit_events() is called.
end

-- Pending events
local function get_pending_events(self)
  -- Return a shallow copy so callers can't mutate internal buffer
  local out = {}
  for i, e in ipairs(self.buffer) do
    out[i] = { type = e.type, code = e.code, value = e.value }
  end
  return out
end

-- Event emission
local function emit_events(self, dev)
  local buf = self.buffer
  for i = 1, #buf do
    local e = buf[i]
    aelkey.emit{
      device = dev,
      type = e.type,
      code = e.code,
      value = e.value,
    }
  end

  -- Clear buffer after emission to support streaming use.
  self.buffer = {}
end

-- Fn state control
local function set_fn_down(self, down)
  self.fn_down = not not down
end

local function get_fn_down(self)
  return self.fn_down
end

-- Optional single-key feed helper
local function feed_key(self, event)
  if not event then
    return
  end
  -- Wrap single event into a list for reuse of feed_events logic.
  feed_events(self, { event })
end

-- Constructor
local function new(opts)
  opts = opts or {}

  local self = {
    -- Mapping tables:
    --   normal_map   : general key remaps
    --   modifier_map : modifier normalization / reassignment
    --   fn_map       : Fn-layer remaps (when fn_down == true)
    normal_map = opts.normal_map   or {},
    modifier_map = opts.modifier_map or {},
    fn_map = opts.fn_map       or {},

    -- State
    fn_down = false,        -- Fn mode flag
    active_keys = {},       -- physical_code_name → mapped_codes table
    buffer = {},            -- list of { type, code, value }
  }

  -- Bind methods as closures
  self.begin_frame = function() return begin_frame(self) end
  self.feed_events = function(ev) return feed_events(self, ev) end
  self.feed_key = function(ev) return feed_key(self, ev) end
  self.end_frame = function() return end_frame(self) end
  self.get_pending_events = function() return get_pending_events(self) end
  self.emit_events = function(dev) return emit_events(self, dev) end
  self.set_fn_down = function(down) return set_fn_down(self, down) end
  self.get_fn_down = function() return get_fn_down(self) end

  return self
end

M.new = new

return M
