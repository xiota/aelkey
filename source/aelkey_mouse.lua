--[[
  aelkey.mouse

  Usage:
    aelkey.mouse.parse_report(data)
    aelkey.mouse.emit_events(dev_id, report)
    aelkey.mouse.emit_motion(dev_id, report)
    aelkey.mouse.emit_buttons(dev_id, report)
    aelkey.mouse.emit_wheels(dev_id, report)
--]]

local M = {}

local events = {
  dx = { type="EV_REL", code="REL_X" },
  dy = { type="EV_REL", code="REL_Y" },

  left   = { type="EV_KEY", code="BTN_LEFT" },
  right  = { type="EV_KEY", code="BTN_RIGHT" },
  middle = { type="EV_KEY", code="BTN_MIDDLE" },
  side = { type="EV_KEY", code="BTN_SIDE" },
  extra = { type="EV_KEY", code="BTN_EXTRA" },
  back = { type="EV_KEY", code="BTN_BACK" },
  forward = { type="EV_KEY", code="BTN_FORWARD" },
  task = { type="EV_KEY", code="BTN_TASK" },

  wheel_vert = { type="EV_REL", code="REL_WHEEL" },
  wheel_vert_hr = { type="EV_REL", code="REL_WHEEL_HI_RES" },

  wheel_horz = { type="EV_REL", code="REL_HWHEEL" },
  wheel_horz_hr = { type="EV_REL", code="REL_HWHEEL_HI_RES" },
}

local function to_signed(value, bits)
  local mask = 1 << bits      -- 2^bits
  local half = mask >> 1      -- 2^(bits-1)
  if value >= half then
    return value - mask
  else
    return value
  end
end

function M.parse_report(data)
  local len = #data
  if len < 3 then return end

  local buttons = string.byte(data, 1)
  local dx, dy = 0
  local wheel_vert = 0
  local wheel_horz = 0

  if len < 6 then
    --[[
      [1] = buttons
      [2] = dx
      [3] = dy
      [4] = wheel (vert)
      [5] = wheel (horz)
    --]]

    dx = to_signed(string.byte(data, 2), 8)
    dy = to_signed(string.byte(data, 3), 8)

    if len >= 4 then
      wheel_vert = to_signed(string.byte(data, 4), 8)
    end
    if len >= 5 then
      wheel_horz = to_signed(string.byte(data, 5), 8)
    end
  else
    --[[
      [1] = buttons
      [2-3] = dx
      [4-5] = dy
      [6] = wheel (vert)
      [7] = wheel (horz)
    --]]

    local dx_lo = string.byte(data, 2)
    local dx_hi = string.byte(data, 3)
    dx = to_signed(dx_lo | (dx_hi << 8), 16)

    local dy_lo = string.byte(data, 4)
    local dy_hi = string.byte(data, 5)
    dy = to_signed(dy_lo | (dy_hi << 8), 16)

    if len >= 6 then
      wheel_vert = to_signed(string.byte(data, 6), 8)
    end
    if len >= 7 then
      wheel_horz = to_signed(string.byte(data, 7), 8)
    end
  end

  local report = {
    left   = (buttons & 0x01) ~= 0,
    right  = (buttons & 0x02) ~= 0,
    middle = (buttons & 0x04) ~= 0,
    side   = (buttons & 0x08) ~= 0,
    extra  = (buttons & 0x10) ~= 0,
    back   = (buttons & 0x20) ~= 0,
    forward = (buttons & 0x40) ~= 0,
    task = (buttons & 0x80) ~= 0,

    dx = dx,
    dy = dy,

    wheel_vert = wheel_vert,
    wheel_horz = wheel_horz,
  }

  return report
end

function M.emit_motion(dev_id, report)
  local emitted = false
  if not report then return false end

  -- dx
  local dx = report.dx
  if dx and dx ~= 0 then
    emitted = true
    aelkey.emit{
      device = dev_id,
      type   = events.dx.type,
      code   = events.dx.code,
      value  = dx,
    }
  end

  -- dy
  local dy = report.dy
  if dy and dy ~= 0 then
    emitted = true
    aelkey.emit{
      device = dev_id,
      type   = events.dy.type,
      code   = events.dy.code,
      value  = dy,
    }
  end

  return emitted
end

function M.emit_buttons(dev_id, report)
  local emitted = false
  if not report then return false end

  for name, ev in pairs(events) do
    if ev.type == "EV_KEY" then
      local pressed = report[name]
      if pressed ~= nil then
        local edge = aelkey.edge.detect("_aelkey_mouse_:" .. dev_id .. ":" .. name, pressed)
        if edge ~= nil then
          emitted = true
          aelkey.emit{
            device = dev_id,
            type   = ev.type,
            code   = ev.code,
            value  = pressed and 1 or 0,
          }
        end
      end
    end
  end

  return emitted
end

function M.emit_wheels(dev_id, report)
  local emitted = false
  if not report then return false end

  -- vertical wheel
  local v = report.wheel_vert
  if v and v ~= 0 then
    emitted = true
    aelkey.emit{
      device = dev_id,
      type   = events.wheel_vert.type,
      code   = events.wheel_vert.code,
      value  = v,
    }
    aelkey.emit{
      device = dev_id,
      type   = events.wheel_vert_hr.type,
      code   = events.wheel_vert_hr.code,
      value  = v * 120,
    }
  end

  -- horizontal wheel
  local h = report.wheel_horz
  if h and h ~= 0 then
    emitted = true
    aelkey.emit{
      device = dev_id,
      type   = events.wheel_horz.type,
      code   = events.wheel_horz.code,
      value  = h,
    }
    aelkey.emit{
      device = dev_id,
      type   = events.wheel_horz_hr.type,
      code   = events.wheel_horz_hr.code,
      value  = h * 120,
    }
  end

  return emitted
end

function M.emit_events(dev_id, report)
  local emitted = false

  for field, value in pairs(report) do
    local ev = events[field]
    if ev then
      if ev.type == "EV_KEY" then
        -- edge detection for buttons
        if aelkey.edge.detect("_mouse:"..dev_id..":"..field, value) ~= nil then
          emitted = true
          aelkey.emit{
            device = dev_id,
            type   = ev.type,
            code   = ev.code,
            value  = value and 1 or 0,
          }
        end
      else
        -- direct emit for motion/wheels
        if value ~= 0 then
          emitted = true
          aelkey.emit{
            device = dev_id,
            type   = ev.type,
            code   = ev.code,
            value  = value,
          }
        end
      end
    end
  end

  return emitted
end

return M
