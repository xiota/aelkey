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
      id = 3,            -- optional (auto‑assigned if missing)
      x = 12000,         -- required
      y = 8000,          -- required
      touching = true,   -- optional (inferred if missing)
      pressure = 42,     -- optional
      palm = false,      -- optional
      button_left = false,
      button_right = false,
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

local function assign_slot(self, tracking_id)
  -- Reuse existing slot if present
  for slot, tid in pairs(self.slots) do
    if tid == tracking_id then
      return slot
    end
  end

  -- Find free slot
  for slot = 0, self.max_slots - 1 do
    if self.slots[slot] == nil then
      self.slots[slot] = tracking_id
      return slot
    end
  end

  -- No free slot; overwrite slot 0 (fallback)
  self.slots[0] = tracking_id
  return 0
end

local function clear_missing_contacts(self)
  for tracking_id, c in pairs(self.contacts) do
    if not c._seen_this_frame then
      -- Mark as ended
      self.ended[#self.ended + 1] = tracking_id

      -- Remove from slot map
      for slot, tid in pairs(self.slots) do
        if tid == tracking_id then
          self.slots[slot] = nil
        end
      end

      -- Remove from contacts
      self.contacts[tracking_id] = nil
    end
  end
end

local function update_primary(self)
  -- Primary = lowest slot with active contact
  local best_slot = nil
  for slot = 0, self.max_slots - 1 do
    local tid = self.slots[slot]
    if tid and self.contacts[tid] then
      best_slot = slot
      break
    end
  end
  self.primary = best_slot
end

-- Frame lifecycle
local function begin_frame(self)
  self.changed = {}
  self.ended = {}

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

  -- Determine tracking ID
  local tid = contact.id
  if not tid then
    -- Auto‑assign synthetic ID
    tid = new_tracking_id(self)
  end

  -- Create or update contact entry
  local c = self.contacts[tid]
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
      _seen_this_frame = true,
    }
    self.contacts[tid] = c
    self.changed[tid] = true
  else
    -- Update existing
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
    c._seen_this_frame = true
  end

  -- Assign slot (multitouch mode only)
  if self.mode == "multitouch" then
    assign_slot(self, tid)
  end
end

local function end_frame(self)
  -- Remove contacts not seen this frame
  clear_missing_contacts(self)

  -- Update primary finger
  update_primary(self)
end

-- Event emission
local function emit_basic(self, dev)
  -- Basic mode: single contact only
  local tid = next(self.contacts)
  if not tid then
    -- No contacts → release BTN_TOUCH
    aelkey.emit{ device=dev, type="EV_KEY", code="BTN_TOUCH", value=0 }
    aelkey.emit{ device=dev, type="EV_KEY", code="BTN_TOOL_FINGER", value=0 }
    return
  end

  local c = self.contacts[tid]

  -- BTN_TOUCH
  aelkey.emit{
    device = dev,
    type = "EV_KEY",
    code = "BTN_TOUCH",
    value = c.touching and 1 or 0,
  }

  -- BTN_TOOL_FINGER
  aelkey.emit{
    device = dev,
    type = "EV_KEY",
    code = "BTN_TOOL_FINGER",
    value = c.touching and 1 or 0,
  }

  -- ABS_X / ABS_Y
  aelkey.emit{
    device = dev,
    type = "EV_ABS",
    code = "ABS_X",
    value = c.x,
  }
  aelkey.emit{
    device = dev,
    type = "EV_ABS",
    code = "ABS_Y",
    value = c.y,
  }

  -- Clickpad buttons (if present)
  if c.button_left ~= nil then
    aelkey.emit{
      device = dev,
      type = "EV_KEY",
      code = "BTN_LEFT",
      value = c.button_left and 1 or 0,
    }
  end
  if c.button_right ~= nil then
    aelkey.emit{
      device = dev,
      type = "EV_KEY",
      code = "BTN_RIGHT",
      value = c.button_right and 1 or 0,
    }
  end
end

local function emit_multitouch(self, dev)
  local finger_count = 0

  -- Emit MT slots
  for slot = 0, self.max_slots - 1 do
    local tid = self.slots[slot]
    if tid then
      local c = self.contacts[tid]

      -- Select slot
      aelkey.emit{
        device = dev,
        type = "EV_ABS",
        code = "ABS_MT_SLOT",
        value = slot,
      }

      if c then
        finger_count = finger_count + 1

        -- Tracking ID
        aelkey.emit{
          device = dev,
          type = "EV_ABS",
          code = "ABS_MT_TRACKING_ID",
          value = tid,
        }

        -- Position
        aelkey.emit{
          device = dev,
          type = "EV_ABS",
          code = "ABS_MT_POSITION_X",
          value = c.x,
        }
        aelkey.emit{
          device = dev,
          type = "EV_ABS",
          code = "ABS_MT_POSITION_Y",
          value = c.y,
        }

        -- Tool type (finger=0, palm=2)
        if c.palm ~= nil then
          aelkey.emit{
            device = dev,
            type = "EV_ABS",
            code = "ABS_MT_TOOL_TYPE",
            value = c.palm and 2 or 0,
          }
        end
      else
        -- Contact ended → tracking ID = -1
        aelkey.emit{
          device = dev,
          type = "EV_ABS",
          code = "ABS_MT_TRACKING_ID",
          value = -1,
        }
      end
    end
  end

  -- BTN_TOUCH
  aelkey.emit{
    device = dev,
    type = "EV_KEY",
    code = "BTN_TOUCH",
    value = (finger_count > 0) and 1 or 0,
  }

  -- BTN_TOOL_* based on finger count
  local tool_code = "BTN_TOOL_FINGER"
  if finger_count == 2 then tool_code = "BTN_TOOL_DOUBLETAP" end
  if finger_count == 3 then tool_code = "BTN_TOOL_TRIPLETAP" end
  if finger_count >= 4 then tool_code = "BTN_TOOL_QUADTAP" end

  aelkey.emit{
    device = dev,
    type = "EV_KEY",
    code = tool_code,
    value = (finger_count > 0) and 1 or 0,
  }

  -- Primary ABS_X / ABS_Y
  if self.primary then
    local tid = self.slots[self.primary]
    local c = self.contacts[tid]
    if c then
      aelkey.emit{
        device = dev,
        type = "EV_ABS",
        code = "ABS_X",
        value = c.x,
      }
      aelkey.emit{
        device = dev,
        type = "EV_ABS",
        code = "ABS_Y",
        value = c.y,
      }
    end
  end

  -- Clickpad buttons (if present)
  for _, c in pairs(self.contacts) do
    if c.button_left ~= nil then
      aelkey.emit{
        device = dev,
        type = "EV_KEY",
        code = "BTN_LEFT",
        value = c.button_left and 1 or 0,
      }
    end
    if c.button_right ~= nil then
      aelkey.emit{
        device = dev,
        type = "EV_KEY",
        code = "BTN_RIGHT",
        value = c.button_right and 1 or 0,
      }
    end
    break -- only primary contact's buttons matter
  end
end

local function emit_events(self, dev)
  if self.mode == "basic" then
    emit_basic(self, dev)
  else
    emit_multitouch(self, dev)
  end
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
  }

  -- Bind methods
  self.begin_frame = function() return begin_frame(self) end
  self.feed_contact = function(c) return feed_contact(self, c) end
  self.end_frame = function() return end_frame(self) end
  self.emit_events = function(dev) return emit_events(self, dev) end

  return self
end

M.new = new

return M
