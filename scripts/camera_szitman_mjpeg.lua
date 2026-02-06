--[[
  UPP USB Frame Format

  Each bulk‑IN packet from EP1_IN contains one UPP “USB frame”.
  A single JPEG image spans multiple UPP frames that share the
  same frame‑ID (fid). A new fid marks the start of a new JPEG.

  Frame layout:
    +-----------------+--------------------+--------------+
    | USB Header (5B) | Camera Header (7B) | JPEG Payload |
    +-----------------+--------------------+--------------+

  USB Header (little‑endian):
    uint16 magic   = 0xBBAA   -- appears on wire as AA BB
    uint8  cid     = camera ID (observed: 7 or 11)
    uint16 length  = size of [camera header + payload]
                     (does NOT include the 5‑byte USB header)

  Camera Header:
    uint8  fid         -- frame ID; increments per JPEG frame
    uint8  cam_num     -- 0 or 1 (stereo index)
    uint8  flags       -- bitfield:
                           bit0 = has_g        (1 = g_sensor valid)
                           bit1 = button_press (1 = physical button pressed)
                           bit2–7 = other      (always 0 in observed data)
    uint32 g_sensor    -- little‑endian accelerometer (?) value

  JPEG Payload:
    Raw JPEG bytes. A full JPEG is reconstructed by concatenating
    payloads from all UPP frames with the same fid.

  References:
    https://github.com/hbens/geek-szitman-supercamera
--]]

aelkey = require("aelkey")
aelkey.click.configure{ window = 500, interval = 20 }

inputs = {
  {
    id         = "supercamera",
    type       = "libusb",
    vid_pid    = {
      {0x2ce3, 0x3828},
      {0x0329, 0x2022},
    },
    interfaces = {0, 1},
    on_state   = "camera_state",
    on_event   = "camera_event",
  },
}

-- Endpoints
local EP2_OUT = 0x02
local EP1_OUT = 0x01
local EP1_IN  = 0x81

-- Commands
local WAKE_CMD  = string.char(0xFF, 0x55, 0xFF, 0x55, 0xEE, 0x10)
local START_CMD = string.char(0xBB, 0xAA, 0x05, 0x00, 0x00)

-- UPP/JPEG state
local UPP_USB_MAGIC = 0xBBAA
local camera_buffer = ""
local last_fid = nil

-- MJPEG recording state
local recorder = {
  mjpeg = nil,
  mjpeg_name = nil,
  first_jpeg_name = nil,
  single_click_requested = false,
  active = false,
}

local function dump_result(label, resp)
  aelkey.log.info("%s: %s", label, aelkey.util.dump_table(resp))
end

local function u16_le(s, i)
  local b1, b2 = s:byte(i, i + 1)
  return b1 + b2 * 256
end

local function start_new_recording()
  -- close previous
  if recorder.mjpeg then
    recorder.mjpeg:close()
    recorder.mjpeg = nil
  end

  local ts = os.date("%Y-%m-%d_%H-%M-%S")
  recorder.mjpeg_name = string.format("capture_%s.mjpeg", ts)
  recorder.first_jpeg_name = string.format("capture_%s_first.jpg", ts)

  local f, err = io.open(recorder.mjpeg_name, "wb")
  if not f then
    aelkey.log.error("Failed to open MJPEG file: %s", tostring(err))
    recorder.active = false
    return
  end

  recorder.mjpeg = f
  recorder.single_click_requested = true
  recorder.active = true

  print("Started recording:", recorder.mjpeg_name)
end

local function handle_single_click()
  recorder.single_click_requested = true
end

local function handle_double_click()
  if recorder.active then
    recorder.mjpeg:close()
    recorder.mjpeg = nil
    recorder.active = false
  end
  start_new_recording()
end

local function handle_triple_click()
  if recorder.active then
    recorder.mjpeg:close()
    recorder.mjpeg = nil
    recorder.active = false
    print("Stopped recording:", recorder.mjpeg_name)
  else
    start_new_recording()
  end
end

local function parse_upp_frame(data)
  local n = #data
  if n < 12 then
    aelkey.log.debug("CAM: frame too small")
    return
  end

  -- USB header
  local magic = u16_le(data, 1)
  if magic ~= UPP_USB_MAGIC then
    aelkey.log.debug("CAM: frame bad magic: 0x%04x", magic)
    return
  end

  local cid    = data:byte(3)
  local length = u16_le(data, 4)

  -- Camera header
  local fid     = data:byte(6)
  local cam_num = data:byte(7)
  local flags   = data:byte(8)

  -- Button press
  local button_press = (flags & 0x02) ~= 0
  if button_press then
    aelkey.click.detect("camera_button",
      function() handle_single_click() end,
      function() handle_double_click() end,
      function() handle_triple_click() end
    )
  end

  -- Frame boundary: fid changed → previous frame complete
  if last_fid ~= nil and fid ~= last_fid then
    if #camera_buffer > 0 then
      -- Save JPEG of current frame
      if recorder.single_click_requested then
        local ts = os.date("%Y-%m-%d_%H-%M-%S")
        local name = string.format("capture_%s_single.jpg", ts)

        local f = assert(io.open(name, "wb"))
        f:write(camera_buffer)
        f:close()
        recorder.single_click_requested = false

        print("Saved JPEG:", name)
      end

      -- Append to MJPEG file
      if recorder.active and recorder.mjpeg then
        recorder.mjpeg:write(camera_buffer)
      end
    end

    camera_buffer = ""
  end
  last_fid = fid

  if cam_num >= 2 then
    aelkey.log.debug("CAM: unknown camera number: %d", cam_num)
    return
  end

  if cid ~= 0x07 and cid ~= 0x0b then
    aelkey.log.debug("CAM: unknown CID: 0x%02x", cid)
    return
  end

  -- Payload: everything after 5-byte USB + 8-byte cam header
  local cam_header_len = 8
  local payload_start = 4 + cam_header_len + 1
  local payload_end   = n

  if payload_start <= payload_end then
    local chunk = data:sub(payload_start, payload_end)
    camera_buffer = camera_buffer .. chunk
  end
end

function camera_state(ev)
  aelkey.log.info("CAM: state event: %s", aelkey.util.dump_table(ev))
  if ev.state ~= "add" then
    aelkey.log.info("CAM: ignoring state '%s'", tostring(ev.state))
    return
  end

  aelkey.log.info("CAM: device added, beginning initialization sequence…")

  local success = true

  local resp = aelkey.usb.set_interface_alt_setting{
    device    = "supercamera",
    interface = 1,
    alt       = 1,
  }
  dump_result("SET_INTERFACE_ALT(1,1)", resp)
  if resp.status ~= "ok" then success = false end

  resp = aelkey.usb.clear_halt{
    device   = "supercamera",
    endpoint = EP1_OUT,
  }
  dump_result("CLEAR_HALT EP1_OUT", resp)
  if resp.status ~= "ok" then success = false end

  resp = aelkey.usb.bulk_transfer{
    device   = "supercamera",
    endpoint = EP2_OUT,
    size     = #WAKE_CMD,
    timeout  = 1000,
    data     = WAKE_CMD,
  }
  dump_result("WAKE_CMD EP2_OUT", resp)
  if resp.status ~= "ok" then success = false end

  resp = aelkey.usb.bulk_transfer{
    device   = "supercamera",
    endpoint = EP1_OUT,
    size     = #START_CMD,
    timeout  = 1000,
    data     = START_CMD,
  }
  dump_result("START_CMD EP1_OUT", resp)
  if resp.status ~= "ok" then success = false end

  resp = aelkey.usb.submit_transfer{
    device   = "supercamera",
    endpoint = EP1_IN,
    type     = "bulk",
    size     = 1024,
    timeout  = 0,
  }
  dump_result("SUBMIT EP1_IN", resp)
  if resp.status ~= "ok" then success = false end

  if success then
    aelkey.log.info("CAM: initialization successful.")
    print("Press the capture button:")
    print("  - Single click: save a JPEG frame.")
    print("  - Double click: start a new video recording.")
    print("  - Triple click: stop the recording.")
  else
    aelkey.log.error("CAM: initialization failed.")
    print("Camera failed to initialize.")
  end
end

function camera_event(ev)
  if ev.status ~= "ok" and ev.status ~= "overflow" and ev.status ~= "timeout" then
    aelkey.log.error("EV endpoint=%02X status=%s", ev.endpoint or 0, tostring(ev.status))
    return
  end

  if ev.data and #ev.data > 0 then
    parse_upp_frame(ev.data)
  end
end

aelkey.start()
