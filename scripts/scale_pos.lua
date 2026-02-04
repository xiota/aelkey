aelkey = require("aelkey")

inputs = {
  {
    id = "scale",
    type = "hidraw",
    vid_pid = {
      {0x0922, 0x8003}, -- DYMO 1772057 Digital Postal Scale
      {0x0922, 0x8004}, -- Dymo-CoStar Corp. M25 Digital Postal Scale
      {0x0922, 0x8009}, -- Dymo-CoStar Corp. S180 180kg Portable Digital Shipping Scale
      {0x0b67, 0x555e}, -- Fairbanks Scales SCB-R9000
      {0x0d8f, 0x0200}, -- Pitney Bowes 10lb scale 397-B (X.J. Group XJ-6K809)
      {0x0eb8, 0xf000}, -- Mettler Toledo
      {0x0eb8, 0xf001}, -- Mettler Toledo
      {0x1446, 0x6a73}, -- Stamps.com Model 510 5LB Scale
      {0x1446, 0x6a79}, -- USPS DS25 25lb postage scale, Royal / X.J.GROUP
      {0x2474, 0x0550}, -- Stamps.com Stainless Steel 5 lb. Digital Scale
      {0x2474, 0x3550}, -- Stamps.com Stainless Steel 35 lb. Digital Scale
      {0x6096, 0x0158}, -- SANFORD Dymo 10 lb USB Postal Scale
      {0x7b7c, 0x0100}, -- USPS (Elane) PS311 "XM Elane Elane UParcel 30lb"
    },
    on_event = "remap",
  },
}

local previous = {}

function remap(events)
  local data = events.data
  local size = events.size

  if size ~= 6 then
    return
  end

  local report_id = data:byte(1)
  if report_id ~= 0x03 and report_id ~= 0x04 then
    print("Unexpected report:", report_id)
    return
  end

  -- HID POS fields
  local status = data:byte(2)
  local unit   = data:byte(3)

  local raw_ex = data:byte(4)
  local expt   = (raw_ex >= 0x80) and (raw_ex - 0x100) or raw_ex

  local raw_lo = data:byte(5)
  local raw_hi = data:byte(6)
  local raw_weight = raw_lo + raw_hi * 256

  -- Apply exponent (base‑10)
  local weight = raw_weight * (10 ^ expt)

  -- HID POS unit names (index = unit code)
  local UNITS = {
    "units", "mg", "g", "kg", "cd", "taels", "gr",
    "dwt", "tonnes", "tons", "ozt", "oz", "lbs"
  }

  local unit_name = UNITS[unit + 1] or "unknown"

  -- HID POS status decoding
  local state = ({
    [0x01] = "fault",
    [0x02] = "zero",
    [0x03] = "weighing",
    [0x04] = "stable",
    [0x05] = "under zero",
    [0x06] = "overweight",
    [0x07] = "calibration needed",
    [0x08] = "rezero needed"
  })[status] or ("unknown(" .. status .. ")")

  -- formatting for oz → lb/oz
  local weight_str
  if unit == 0x0b then  -- ounce
    local lbs = math.floor(weight / 16)
    local oz  = weight - lbs * 16
    weight_str = string.format("%d lb %.1f oz", lbs, oz)
  else
    weight_str = string.format("%g %s", weight, unit_name)
  end

  if weight_str ~= previous.weight_str or state ~= previous.state then
    previous.weight_str = weight_str
    previous.state = state
    print(weight_str .. " / " .. state)
  end

  -- tare
  -- aelkey.hid.send_output_report("scale", string.char(0x02, 0x02))
end

aelkey.start()
