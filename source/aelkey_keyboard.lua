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
local function append_event(self, type, code, value)
  local buf = self.buffer
  local n = #buf + 1
  buf[n] = buf[n] or {}
  local e = buf[n]
  e.type = type
  e.code = code
  e.value = value
end

-- Determine mapping for a press event, given current Fn state.
-- Returns:
--   mapped_codes: table of code strings, or nil to suppress
--   is_fn_placeholder: true if this mapping should toggle Fn mode only
local function resolve_mapping(self, code)
  local mapped

  if self.fn_down then
    mapped = self.modifier_map[code]
             or ( IDENTITY_MAP[code] and { code })
             or self.fn_map[code]
  else
    mapped = self.modifier_map[code]
             or self.normal_map[code]
             or code
  end

  -- Empty table explicitly means "suppress"
  if mapped and type(mapped) == "table" and #mapped == 0 then
    aelkey.log.trace("aelkey.keyboard: unmapped, %s", code)
    return nil, false
  end

  -- Normalize single-string mapping to table
  if type(mapped) == "string" then
    aelkey.log.trace("aelkey.keyboard: fallback, %s", code)
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
    local type = e.type
    local code = e.code
    local value = e.value

    -- Only handle EV_KEY presses/releases (value 0/1)
    if type == KEY_EVENT_TYPE and (value == 0 or value == 1) then
      if value == 1 then
        -- Key press
        local mapped, is_fn_placeholder = resolve_mapping(self, code)

        if is_fn_placeholder then
          -- Toggle Fn mode on press; remember for release
          self.fn_down = true
          self.active_keys[code] = { FN_CODE } -- marker
        elseif mapped then
          -- Remember mapping used at press time
          self.active_keys[code] = mapped

          -- Emit press events in order
          for _, out_code in ipairs(mapped) do
            append_event(self, KEY_EVENT_TYPE, out_code, 1)
          end
        else
          -- mapped == nil → suppressed key
          -- Do nothing, do not track active_keys
          aelkey.log.trace("aelkey.keyboard: ignored key %s", out_code)
        end

      else
        -- Key release
        local mapped = self.active_keys[code]
        self.active_keys[code] = nil

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
          aelkey.log.trace("aelkey.keyboard: ignored key %s", out_code)
        end
      end

    elseif type == "EV_MSC" or type == "EV_SYN" or value == 2 then
      -- Ignore misc, sync, and auto-repeat by default.
      aelkey.log.spam("aelkey.keyboard: unwanted event %s, %d", type, value)
    else
      -- Unknown or unsupported event type → ignore.
      aelkey.log.spam("aelkey.keyboard: unknown event %s, %d", type, value)
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

----------------------------------------------------------------
-- Universal HID keyboard parser (8‑byte boot report, no RID)
----------------------------------------------------------------

-- Modifier bit → Linux keycode
local MOD_BITS = {
  [0] = "KEY_LEFTCTRL",
  [1] = "KEY_LEFTSHIFT",
  [2] = "KEY_LEFTALT",
  [3] = "KEY_LEFTMETA",
  [4] = "KEY_RIGHTCTRL",
  [5] = "KEY_RIGHTSHIFT",
  [6] = "KEY_RIGHTALT",
  [7] = "KEY_RIGHTMETA",
}

-- HID usage → Linux keycode
local HID_TO_KEY = {
  [0x00] = nil,                -- No event
  [0x01] = nil,                -- ErrorRollOver
  [0x02] = nil,                -- POSTFail
  [0x03] = nil,                -- ErrorUndefined

  -- Letters
  [0x04] = "KEY_A",
  [0x05] = "KEY_B",
  [0x06] = "KEY_C",
  [0x07] = "KEY_D",
  [0x08] = "KEY_E",
  [0x09] = "KEY_F",
  [0x0A] = "KEY_G",
  [0x0B] = "KEY_H",
  [0x0C] = "KEY_I",
  [0x0D] = "KEY_J",
  [0x0E] = "KEY_K",
  [0x0F] = "KEY_L",
  [0x10] = "KEY_M",
  [0x11] = "KEY_N",
  [0x12] = "KEY_O",
  [0x13] = "KEY_P",
  [0x14] = "KEY_Q",
  [0x15] = "KEY_R",
  [0x16] = "KEY_S",
  [0x17] = "KEY_T",
  [0x18] = "KEY_U",
  [0x19] = "KEY_V",
  [0x1A] = "KEY_W",
  [0x1B] = "KEY_X",
  [0x1C] = "KEY_Y",
  [0x1D] = "KEY_Z",

  -- Numbers row
  [0x1E] = "KEY_1",
  [0x1F] = "KEY_2",
  [0x20] = "KEY_3",
  [0x21] = "KEY_4",
  [0x22] = "KEY_5",
  [0x23] = "KEY_6",
  [0x24] = "KEY_7",
  [0x25] = "KEY_8",
  [0x26] = "KEY_9",
  [0x27] = "KEY_0",

  -- Symbols / control
  [0x28] = "KEY_ENTER",
  [0x29] = "KEY_ESC",
  [0x2A] = "KEY_BACKSPACE",
  [0x2B] = "KEY_TAB",
  [0x2C] = "KEY_SPACE",
  [0x2D] = "KEY_MINUS",
  [0x2E] = "KEY_EQUAL",
  [0x2F] = "KEY_LEFTBRACE",
  [0x30] = "KEY_RIGHTBRACE",
  [0x31] = "KEY_BACKSLASH",
  [0x32] = "KEY_BACKSLASH",     -- Non‑US \|
  [0x33] = "KEY_SEMICOLON",
  [0x34] = "KEY_APOSTROPHE",
  [0x35] = "KEY_GRAVE",
  [0x36] = "KEY_COMMA",
  [0x37] = "KEY_DOT",
  [0x38] = "KEY_SLASH",

  -- Lock keys
  [0x39] = "KEY_CAPSLOCK",

  -- Function keys
  [0x3A] = "KEY_F1",
  [0x3B] = "KEY_F2",
  [0x3C] = "KEY_F3",
  [0x3D] = "KEY_F4",
  [0x3E] = "KEY_F5",
  [0x3F] = "KEY_F6",
  [0x40] = "KEY_F7",
  [0x41] = "KEY_F8",
  [0x42] = "KEY_F9",
  [0x43] = "KEY_F10",
  [0x44] = "KEY_F11",
  [0x45] = "KEY_F12",

  -- Print / system
  [0x46] = "KEY_SYSRQ",
  [0x47] = "KEY_SCROLLLOCK",
  [0x48] = "KEY_PAUSE",
  [0x49] = "KEY_INSERT",
  [0x4A] = "KEY_HOME",
  [0x4B] = "KEY_PAGEUP",
  [0x4C] = "KEY_DELETE",
  [0x4D] = "KEY_END",
  [0x4E] = "KEY_PAGEDOWN",

  -- Arrows
  [0x4F] = "KEY_RIGHT",
  [0x50] = "KEY_LEFT",
  [0x51] = "KEY_DOWN",
  [0x52] = "KEY_UP",

  -- Keypad
  [0x53] = "KEY_NUMLOCK",
  [0x54] = "KEY_KPSLASH",
  [0x55] = "KEY_KPASTERISK",
  [0x56] = "KEY_KPMINUS",
  [0x57] = "KEY_KPPLUS",
  [0x58] = "KEY_KPENTER",
  [0x59] = "KEY_KP1",
  [0x5A] = "KEY_KP2",
  [0x5B] = "KEY_KP3",
  [0x5C] = "KEY_KP4",
  [0x5D] = "KEY_KP5",
  [0x5E] = "KEY_KP6",
  [0x5F] = "KEY_KP7",
  [0x60] = "KEY_KP8",
  [0x61] = "KEY_KP9",
  [0x62] = "KEY_KP0",
  [0x63] = "KEY_KPDOT",

  -- Non‑US keys
  [0x64] = "KEY_102ND",         -- Non‑US \<> key

  -- More function keys
  [0x65] = "KEY_COMPOSE",       -- Application/Menu
  [0x66] = "KEY_POWER",
  [0x67] = "KEY_KPEQUAL",

  -- F13–F24
  [0x68] = "KEY_F13",
  [0x69] = "KEY_F14",
  [0x6A] = "KEY_F15",
  [0x6B] = "KEY_F16",
  [0x6C] = "KEY_F17",
  [0x6D] = "KEY_F18",
  [0x6E] = "KEY_F19",
  [0x6F] = "KEY_F20",
  [0x70] = "KEY_F21",
  [0x71] = "KEY_F22",
  [0x72] = "KEY_F23",
  [0x73] = "KEY_F24",

  -- International / language keys
  [0x87] = "KEY_RO",
  [0x88] = "KEY_KATAKANAHIRAGANA",
  [0x89] = "KEY_YEN",
  [0x8A] = "KEY_HENKAN",
  [0x8B] = "KEY_MUHENKAN",
  [0x8C] = "KEY_KPJPCOMMA",

  -- Keypad extensions
  [0x8D] = "KEY_KPENTER",
  [0x8E] = "KEY_RIGHTCTRL",
  [0x8F] = "KEY_KPSLASH",
  [0x90] = "KEY_SYSRQ",
  [0x91] = "KEY_RIGHTALT",
  [0x92] = "KEY_LINEFEED",
  [0x93] = "KEY_HOME",
  [0x94] = "KEY_UP",
  [0x95] = "KEY_PAGEUP",
  [0x96] = "KEY_LEFT",
  [0x97] = "KEY_RIGHT",
  [0x98] = "KEY_END",
  [0x99] = "KEY_DOWN",
  [0x9A] = "KEY_PAGEDOWN",
  [0x9B] = "KEY_INSERT",
  [0x9C] = "KEY_DELETE",

  -- Modifiers (redundant with MOD_BITS but included for completeness)
  [0xE0] = "KEY_LEFTCTRL",
  [0xE1] = "KEY_LEFTSHIFT",
  [0xE2] = "KEY_LEFTALT",
  [0xE3] = "KEY_LEFTMETA",
  [0xE4] = "KEY_RIGHTCTRL",
  [0xE5] = "KEY_RIGHTSHIFT",
  [0xE6] = "KEY_RIGHTALT",
  [0xE7] = "KEY_RIGHTMETA",
}

-- Previous state (sets)
local prev_mods = {}   -- code → true
local prev_keys = {}   -- code → true

-- Parse 8‑byte HID keyboard report (no RID)
-- Returns list of EV_KEY events
function parse_report(bytes)
  if #bytes < 8 then
    aelkey.log.error(
      "aelkey.keyboard.parse_report: unexpected input (%d bytes): %s",
      #bytes,
      aelkey.util.dump_hex(bytes)
    )
    return
  end

  local events = {}

  -- Decode modifiers
  local mods = {}
  local mod_byte = bytes[1]

  for bit = 0, 7 do
    if mod_byte & (1 << bit) ~= 0 then
      local code = MOD_BITS[bit]
      if code then mods[code] = true end
    end
  end

  -- Decode up to 6 HID keycodes
  local keys = {}
  for i = 3, 8 do
    local hid = bytes[i]
    if hid ~= 0 then
      local code = HID_TO_KEY[hid]
      if code then keys[code] = true end
    end
  end

  -- Emit modifier releases
  for code in pairs(prev_mods) do
    if not mods[code] then
      events[#events+1] = { type="EV_KEY", code=code, value=0 }
    end
  end

  -- Emit modifier presses
  for code in pairs(mods) do
    if not prev_mods[code] then
      events[#events+1] = { type="EV_KEY", code=code, value=1 }
    end
  end

  -- Emit key releases
  for code in pairs(prev_keys) do
    if not keys[code] then
      events[#events+1] = { type="EV_KEY", code=code, value=0 }
    end
  end

  -- Emit key presses
  for code in pairs(keys) do
    if not prev_keys[code] then
      events[#events+1] = { type="EV_KEY", code=code, value=1 }
    end
  end

  -- Save state
  prev_mods = mods
  prev_keys = keys

  return events
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
    active_keys = {},       -- physical_code → mapped_codes table
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
  self.parse_report = parse_report

  return self
end

M.new = new

return M
