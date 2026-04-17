#!/usr/bin/env lua
--[[
  USB HID‑POS Scale Reader + Control

  This script listens to HID‑POS compatible postal scales,
  and prints decoded weight, units, and status in real time.

  Two vendor‑specific Output Report commands are implemented by these scales:

    -- Zero the scale (force baseline to 0)
    aelkey.hid.send_output_report("scale", string.char(0x02, 0x02))

    -- Tare toggle (tare / un‑tare depending on current state)
    aelkey.hid.send_output_report("scale", string.char(0x02, 0x01))
]]

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

local STATUS_CODES = {
  [0x01] = "fault",
  [0x02] = "zero",
  [0x03] = "weighing",
  [0x04] = "stable",
  [0x05] = "under zero",
  [0x06] = "overweight",
  [0x07] = "calibration needed",
  [0x08] = "rezero needed",
}

local UNITS = {
  [0x01] = "mg",
  [0x02] = "g",
  [0x03] = "kg",
  [0x04] = "ct",
  [0x05] = "tael",
  [0x06] = "gr",
  [0x07] = "dwt",
  [0x08] = "tonne",
  [0x09] = "ton",
  [0x0a] = "ozt",
  [0x0b] = "oz",
  [0x0c] = "lb",
}

local UNIT_CODES = {}
for code, name in pairs(UNITS) do
  UNIT_CODES[name] = code
end

local function set_unit(target)
  for _ = 1, 4 do
    local f = aelkey.hid.get_feature_report("scale", 0x01)
    local unit = f:byte(3)
    if unit == target then return true end
    aelkey.hid.send_feature_report("scale", string.char(0x01, 0x00))
  end
  return false
end

local function show_help()
  print([[
Usage:
  scale_pos.lua [options]

Options:
  --once         print one reading and exit
  --wait         wait for device connection

  --zero         zero the scale
  --tare         apply or remove tare
  --toggle       toggle unit (cycle through available units)
  --unit=UNIT    set unit (g, oz)

  --help
]])
end

-- simple one‑shot command line options
local do_once = false
local do_wait = false
local do_zero = false
local do_tare = false
local do_toggle = false
local want_unit = nil

for _, arg in ipairs(arg) do
  if arg == "--help" then
    show_help()
    os.exit(0)
  end
  if arg == "--once" then
    do_once = true
  end
  if arg == "--wait" then
    do_wait = true
  end
  if arg == "--zero" then
    do_zero = true
  end
  if arg == "--tare" then
    do_tare = true
  end
  if arg == "--toggle" then
    do_toggle = true
  end
  if arg:match("^%-%-unit=") then
    want_unit = arg:match("^%-%-unit=(.+)$")
  end
end

-- connect to scale
local is_open = aelkey.open_device("scale")

if not is_open then
  if not do_wait then
    print("Failed to open device")
    os.exit(1)
  else
    print("Waiting for connection...")
  end
end

function run_one_shot_commands()
  if do_zero then
    aelkey.hid.send_output_report("scale", string.char(0x02, 0x02))
  end

  if do_tare then
    aelkey.hid.send_output_report("scale", string.char(0x02, 0x01))
  end

  if do_toggle then
    aelkey.hid.send_feature_report("scale", string.char(0x01, 0x00))
  end

  if want_unit then
    local target = UNIT_CODES[want_unit]
    if not target then
      print("Unknown unit: " .. want_unit)
      os.exit(1)
    end
    if set_unit(target) then
      print("Unit set to " .. want_unit)
    else
      print("Failed to set unit")
    end
  end
end

local previous = {}

function process_weight_report(events)
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

  -- state and unit
  local state = STATUS_CODES[status] or ("unknown(" .. status .. ")")
  local unit_name = UNITS[unit] or "unknown"

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
end

function remap(events)
  -- If any one-shot commands were requested
  if do_zero or do_tare or do_toggle or want_unit then
    run_one_shot_commands()
    os.exit(0)
  end

  -- Normal weight processing
  process_weight_report(events)

  -- Exit after first reading in --once mode
  if do_once then
    os.exit(0)
  end
end

aelkey.start()
