--[[
  aelkey.touchpad
  Unified touchpad state machine with optional multitouch slot tracking.

  This module provides:
    • A device‑agnostic contact tracker (single or multi‑finger)
    • Automatic slot assignment and tracking‑ID management (multitouch mode)
    • Primary‑finger tracking for ABS_X / ABS_Y emission
    • Adaptive clickpad support (button bits or pressure if provided)
    • Caller‑controlled event emission (no automatic emits)
    • Robust handling of missing fields (x/y required; others optional)

  Modes:
    mode = "basic"
      • Single‑finger absolute touchpad
      • Emits ABS_X, ABS_Y, BTN_TOUCH, BTN_TOOL_FINGER
      • No MT slots, no tracking IDs

    mode = "multitouch"
      • Full Linux MT protocol
      • Emits ABS_MT_SLOT, ABS_MT_TRACKING_ID, ABS_MT_POSITION_X/Y
      • Also emits ABS_X / ABS_Y for primary finger (libinput compatibility)
      • Emits BTN_TOUCH and BTN_TOOL_* based on finger count

  Usage:
    local tp = aelkey.touchpad.new{
      mode = "multitouch",   -- or "basic"
      max_slots = 5,
    }

    tp.begin_frame()

    tp.feed_contact{
      slot = 0,          -- required
      x = 12000,         -- required
      y = 8000,          -- required
      touching = true,   -- optional (inferred if missing)
      pressure = 42,     -- optional
      palm = false,      -- optional
      button_left = false,
      button_right = false,
      button_middle = false,
    }

    tp.end_frame()

    tp.emit_events("virt_touchpad")
    aelkey.syn_report("virt_touchpad")

  Notes:
    • No events are emitted automatically; caller controls emission.
    • Missing fields are handled gracefully.
    • Contacts omitted in a frame are treated as released.
    • Internal state is exposed for caller‑side gesture logic:
        tp.contacts
        tp.slots
        tp.primary
        tp.changed
        tp.ended
--]]

local M = {}

-- Internal helpers
local function new_tracking_id(self)
  local id = self.next_tracking_id
  self.next_tracking_id = (id % 65535) + 1
  return id
end

local function queue_event(self, ev)
  self.events[#self.events + 1] = ev
end

local function clear_missing_contacts(self)
  for tracking_id, c in pairs(self.contacts) do
    if not c._seen_this_frame then
      -- Mark as ended, but DO NOT delete yet
      c._ended = true
      self.ended[#self.ended + 1] = tracking_id
    end
  end
end

local function update_primary(self)
  -- Primary = lowest slot with active contact (not ended)
  local best_slot = nil
  for slot = 0, self.max_slots - 1 do
    local tid = self.slots[slot]
    if tid then
      local c = self.contacts[tid]
      if c and not c._ended and c.touching then
        best_slot = slot
        break
      end
    end
  end
  self.primary = best_slot
end

-- Frame lifecycle
local function begin_frame(self)
  self.changed = {}
  self.ended = {}
  self.events = {}

  -- Mark all contacts as unseen until feed_contact() updates them
  for _, c in pairs(self.contacts) do
    c._seen_this_frame = false
  end
end

local function feed_contact(self, contact)
  -- x/y required
  if not contact.x or not contact.y then
    return
  end

  -- Determine touching state
  local touching = contact.touching
  if touching == nil then
    touching = not (contact.x == 0 and contact.y == 0)
  end

  -- Determine tracking ID based on mode and slot
  local tid
  local slot

  if self.mode == "multitouch" then
    -- Determine slot (default to 0)
    slot = contact.slot or 0
    slot = math.floor(slot)

    if slot < 0 or slot >= self.max_slots then
      return -- invalid slot
    end

    -- Reuse existing tracking ID for this slot if present
    tid = self.slots[slot]
    if not tid then
      -- New contact in this slot → allocate new tracking ID
      tid = new_tracking_id(self)
      self.slots[slot] = tid
    end
  else
    -- Basic mode: single synthetic tracking ID
    if not self._basic_tid then
      self._basic_tid = new_tracking_id(self)
    end
    tid = self._basic_tid
  end

  -- Create or update contact entry
  local c = self.contacts[tid]
  local is_new = false

  if not c then
    c = {
      id = tid,
      x = contact.x,
      y = contact.y,
      touching = touching,
      pressure = contact.pressure,
      palm = contact.palm,
      button_left = contact.button_left,
      button_right = contact.button_right,
      button_middle = contact.button_middle,
      _seen_this_frame = true,
      _ended = false,
    }
    self.contacts[tid] = c
    self.changed[tid] = true
    is_new = true
  else
    if c.x ~= contact.x or c.y ~= contact.y or c.touching ~= touching then
      self.changed[tid] = true
    end

    c.x = contact.x
    c.y = contact.y
    c.touching = touching
    c.pressure = contact.pressure
    c.palm = contact.palm
    c.button_left = contact.button_left
    c.button_right = contact.button_right
    c.button_middle = contact.button_middle
    c._seen_this_frame = true
    c._ended = false
  end

  -- Build MT events as contacts are received (multitouch mode only)
  if self.mode == "multitouch" and touching then
    -- Select slot
    queue_event(self, {
      type = "EV_ABS",
      code = "ABS_MT_SLOT",
      value = slot,
    })

    -- Tracking ID: for new contact or whenever we want to (keep it simple: always)
    queue_event(self, {
      type = "EV_ABS",
      code = "ABS_MT_TRACKING_ID",
      value = tid,
    })

    -- Position
    queue_event(self, {
      type = "EV_ABS",
      code = "ABS_MT_POSITION_X",
      value = c.x,
    })
    queue_event(self, {
      type = "EV_ABS",
      code = "ABS_MT_POSITION_Y",
      value = c.y,
    })

    -- Tool type (finger=0, palm=2)
    if c.palm ~= nil then
      queue_event(self, {
        type = "EV_ABS",
        code = "ABS_MT_TOOL_TYPE",
        value = c.palm and 2 or 0,
      })
    end
  end
end

local function end_frame(self)
  -- Mark contacts not seen this frame as ended
  clear_missing_contacts(self)

  -- For multitouch: emit -1 tracking IDs for ended contacts, as if actually emitted
  if self.mode == "multitouch" then
    for _, tracking_id in ipairs(self.ended) do
      -- Find the slot(s) for this tracking ID
      for slot, tid in pairs(self.slots) do
        if tid == tracking_id then
          -- Select slot
          queue_event(self, {
            type = "EV_ABS",
            code = "ABS_MT_SLOT",
            value = slot,
          })
          -- End contact in this slot
          queue_event(self, {
            type = "EV_ABS",
            code = "ABS_MT_TRACKING_ID",
            value = -1,
          })
        end
      end
    end
  end

  -- Update primary finger (multitouch only)
  if self.mode == "multitouch" then
    update_primary(self)
  else
    self.primary = nil
  end

  -- Compute finger count and primary contact
  local finger_count = 0
  local primary_contact = nil

  if self.mode == "multitouch" then
    -- Count active contacts
    for _, c in pairs(self.contacts) do
      if not c._ended and c.touching then
        finger_count = finger_count + 1
      end
    end

    -- Primary contact from primary slot
    if self.primary ~= nil then
      local ptid = self.slots[self.primary]
      if ptid then
        local pc = self.contacts[ptid]
        if pc and not pc._ended and pc.touching then
          primary_contact = pc
        end
      end
    end
  else
    -- Basic mode: single synthetic contact
    local tid = self._basic_tid
    if tid then
      local c = self.contacts[tid]
      if c and not c._ended and c.touching then
        finger_count = 1
        primary_contact = c
      end
    end
  end

  self.finger_count = finger_count

  -- BTN_TOUCH
  queue_event(self, {
    type = "EV_KEY",
    code = "BTN_TOUCH",
    value = (finger_count > 0) and 1 or 0,
  })

  -- BTN_TOOL_* based on finger count, with state tracking
  local desired_down = {}

  if finger_count == 1 then
    desired_down["BTN_TOOL_FINGER"] = true
  elseif finger_count == 2 then
    desired_down["BTN_TOOL_DOUBLETAP"] = true
  elseif finger_count == 3 then
    desired_down["BTN_TOOL_TRIPLETAP"] = true
  elseif finger_count >= 4 then
    desired_down["BTN_TOOL_QUADTAP"] = true
  end

  -- Release codes that are currently down but no longer desired
  for code, _ in pairs(self.tool_keys_down) do
    if not desired_down[code] then
      queue_event(self, {
        type  = "EV_KEY",
        code  = code,
        value = 0,
      })
      self.tool_keys_down[code] = nil
    end
  end

  -- Press codes that should be down but aren’t yet
  for code, _ in pairs(desired_down) do
    if not self.tool_keys_down[code] then
      queue_event(self, {
        type  = "EV_KEY",
        code  = code,
        value = 1,
      })
      self.tool_keys_down[code] = true
    end
  end

  -- ABS_X / ABS_Y for primary contact (if any)
  if primary_contact then
    queue_event(self, {
      type = "EV_ABS",
      code = "ABS_X",
      value = primary_contact.x,
    })
    queue_event(self, {
      type = "EV_ABS",
      code = "ABS_Y",
      value = primary_contact.y,
    })
  end

  -- Clickpad buttons (if present)
  local button_source = primary_contact

  -- fallback: first active contact
  if not button_source then
    for _, c in pairs(self.contacts) do
      if not c._ended then
        button_source = c
        break
      end
    end
  end

  if button_source then
    if button_source.button_left ~= nil then
      local pressed = aelkey.edge.detect("_aelkey_touchpad_BTN_LEFT", button_source.button_left)
      if pressed ~= nil then
        queue_event(self, {
          type = "EV_KEY",
          code = "BTN_LEFT",
          value = pressed and 1 or 0,
        })
      end
    end
    if button_source.button_right ~= nil then
      local pressed = aelkey.edge.detect("_aelkey_touchpad_BTN_RIGHT", button_source.button_right)
      if pressed ~= nil then
        queue_event(self, {
        type = "EV_KEY",
        code = "BTN_RIGHT",
          value = pressed and 1 or 0,
        })
      end
    end
    if button_source.button_middle ~= nil then
      local pressed = aelkey.edge.detect("_aelkey_touchpad_BTN_MIDDLE", button_source.button_middle)
      if pressed ~= nil then
        queue_event(self, {
        type = "EV_KEY",
        code = "BTN_MIDDLE",
          value = pressed and 1 or 0,
        })
      end
    end
  end

  -- Cleanup ended contacts and slots AFTER queuing -1 events
  for tracking_id, c in pairs(self.contacts) do
    if c._ended then
      -- remove slot(s)
      for slot, tid in pairs(self.slots) do
        if tid == tracking_id then
          self.slots[slot] = nil
        end
      end
      -- remove contact
      self.contacts[tracking_id] = nil
    end
  end
end

-- Event retrieval and emission
local function get_pending_events(self)
  local copy = {}
  for i, ev in ipairs(self.events) do
    local evcopy = {}
    for k, v in pairs(ev) do
      evcopy[k] = v
    end
    copy[i] = evcopy
  end
  return copy
end

local function emit_events(self, dev)
  for _, ev in ipairs(self.events) do
    ev.device = dev
    aelkey.emit(ev)
  end
  -- Clear after emitting
  self.events = {}
end

-- Constructor
local function new(opts)
  opts = opts or {}

  local self = {
    mode = opts.mode or "multitouch",
    max_slots = opts.max_slots or 5,

    contacts = {},       -- tracking_id → contact
    slots = {},          -- slot → tracking_id
    primary = nil,       -- slot index
    changed = {},        -- tracking_id → true
    ended = {},          -- list of tracking_ids
    next_tracking_id = 1,

    tool_keys_down = {}, -- code -> true if currently pressed
    events = {},         -- accumulated events for current frame
  }

  -- Bind methods
  self.begin_frame = function() return begin_frame(self) end
  self.feed_contact = function(c) return feed_contact(self, c) end
  self.end_frame = function() return end_frame(self) end
  self.get_pending_events = function() return get_pending_events(self) end
  self.emit_events = function(dev) return emit_events(self, dev) end

  return self
end

M.new = new

return M
