--[[
  aelkey.profiles
  Pre-configured virtual device templates for uinput outputs.

  Each factory function accepts an optional configuration table to override
  default properties, add custom capabilities, or specify identifiers.

  Supported Profiles:
    - keyboard, consumer, mouse, touchpad
    - gamepad
    - gamepad_ds4, gamepad_ds4_imu
    - gamepad_nsp, gamepad_nsp_imu
    - gamepad_xpad, gamepad_xpad_bt

  Usage:
    outputs = {
      aelkey.profiles.mouse{},
      aelkey.profiles.gamepad{
        id = "virtual_gamepad",
        name = "Custom Gamepad",
        capabilities = {
          { code = "ABS_X", min = -32768, max = 32767, fuzz = 16 },
          "BTN_TRIGGER_HAPPY1",
        },
      },
    }
]]--

local M = {}

local function merge_capabilities(base_caps, user_caps)
  if not user_caps then
    return base_caps
  end

  local merged = {}
  local seen = {}

  -- Helper to index capabilities for deduplication
  local function add_cap(cap)
    local key = type(cap) == "table" and cap.code or cap
    if not seen[key] then
      seen[key] = true
      table.insert(merged, cap)
    else
      -- If it's a table override, find and replace the existing one
      if type(cap) == "table" then
        for i, existing in ipairs(merged) do
          if type(existing) == "table" and existing.code == cap.code then
            merged[i] = cap
            break
          end
        end
      end
    end
  end

  for _, cap in ipairs(base_caps) do
    add_cap(cap)
  end

  for _, cap in ipairs(user_caps) do
    add_cap(cap)
  end

  return merged
end

local function make_factory(base_defaults)
  return function(config)
    config = config or {}
    local device = {}

    -- Copy base defaults
    for k, v in pairs(base_defaults) do
      device[k] = v
    end

    -- Override with user config
    for k, v in pairs(config) do
      if k == "capabilities" then
        device.capabilities = merge_capabilities(base_defaults.capabilities, v)
      else
        device[k] = v
      end
    end

    -- Ensure type is always uinput for profiles
    device.type = "uinput"

    return device
  end
end

-- Generic union controller
M.gamepad = make_factory{
  id = "virt_gamepad",
  name = "Aelkey Gamepad",
  capabilities = {
    "BTN_SOUTH", "BTN_EAST", "BTN_NORTH", "BTN_WEST",
    "KEY_BACK", "KEY_HOMEPAGE", "BTN_C", "BTN_Z",
    "BTN_TL", "BTN_TR", "BTN_TL2", "BTN_TR2",
    "BTN_SELECT", "BTN_START", "BTN_MODE",
    "BTN_THUMBL", "BTN_THUMBR",
    { code="ABS_X", min=-32768, max=32767, fuzz=16, flat=128 },
    { code="ABS_Y", min=-32768, max=32767, fuzz=16, flat=128 },
    { code="ABS_Z", min=0, max=255 },
    { code="ABS_RX", min=-32768, max=32767, fuzz=16, flat=128 },
    { code="ABS_RY", min=-32768, max=32767, fuzz=16, flat=128 },
    { code="ABS_RZ", min=0, max=255 },
    { code="ABS_GAS", min=0, max=1023, fuzz=3, flat=63 },
    { code="ABS_BRAKE", min=0, max=1023, fuzz=3, flat=63 },
    { code="ABS_HAT0X", min=-1, max=1 },
    { code="ABS_HAT0Y", min=-1, max=1 },
    "FF_RUMBLE",
  },
}

-- Xbox style controller (usb)
M.gamepad_xpad = make_factory{
  id = "virt_gamepad",
  name = "Aelkey X-Box 360 pad",
  bus = "usb",
  capabilities = {
    "BTN_SOUTH", "BTN_EAST", "BTN_NORTH", "BTN_WEST",
    "BTN_TL", "BTN_TR",
    "BTN_SELECT", "BTN_START", "BTN_MODE",
    "BTN_THUMBL", "BTN_THUMBR",
    { code="ABS_X", min=-32768, max=32767, fuzz=16, flat=128 },
    { code="ABS_Y", min=-32768, max=32767, fuzz=16, flat=128 },
    { code="ABS_Z", min=0, max=255 },
    { code="ABS_RX", min=-32768, max=32767, fuzz=16, flat=128 },
    { code="ABS_RY", min=-32768, max=32767, fuzz=16, flat=128 },
    { code="ABS_RZ", min=0, max=255 },
    { code="ABS_HAT0X", min=-1, max=1 },
    { code="ABS_HAT0Y", min=-1, max=1 },
    "FF_RUMBLE",
  },
}

-- Xbox style controller (bluetooth)
M.gamepad_xpad_bt = make_factory{
  id = "virt_gamepad",
  name = "Aelkey Xbox Wireless Controller",
  bus = "bluetooth",
  capabilities = {
    "BTN_SOUTH", "BTN_EAST", "BTN_NORTH", "BTN_WEST",
    "KEY_BACK", "KEY_HOMEPAGE", "BTN_C", "BTN_Z",
    "BTN_TL", "BTN_TR", "BTN_TL2", "BTN_TR2",
    "BTN_SELECT", "BTN_START", "BTN_MODE",
    "BTN_THUMBL", "BTN_THUMBR",
    { code="ABS_X", min=0, max=65535, fuzz=16, flat=128 },
    { code="ABS_Y", min=0, max=65535, fuzz=16, flat=128 },
    { code="ABS_Z", min=0, max=65535, fuzz=16, flat=128 },
    { code="ABS_RZ", min=0, max=65535, fuzz=16, flat=128 },
    { code="ABS_GAS", min=0, max=1023, fuzz=4, flat=32 },
    { code="ABS_BRAKE", min=0, max=1023, fuzz=4, flat=32 },
    { code="ABS_HAT0X", min=-1, max=1 },
    { code="ABS_HAT0Y", min=-1, max=1 },
    "FF_RUMBLE",
  },
}

-- NSP style controller
M.gamepad_nsp = make_factory{
  id = "virt_gamepad",
  name = "Aelkey Pro Controller",
  bus = "bluetooth",
  capabilities = {
    "BTN_SOUTH", "BTN_EAST", "BTN_NORTH", "BTN_WEST",
    "BTN_Z",
    "BTN_TL", "BTN_TR", "BTN_TL2", "BTN_TR2",
    "BTN_SELECT", "BTN_START", "BTN_MODE",
    "BTN_THUMBL", "BTN_THUMBR",
    { code="ABS_X", min=-32767, max=32767, fuzz=16, flat=128 },
    { code="ABS_Y", min=-32767, max=32767, fuzz=16, flat=128 },
    { code="ABS_RX", min=-32767, max=32767, fuzz=16, flat=128 },
    { code="ABS_RY", min=-32767, max=32767, fuzz=16, flat=128 },
    { code="ABS_HAT0X", min=-1, max=1 },
    { code="ABS_HAT0Y", min=-1, max=1 },
    "FF_RUMBLE",
  },
}

-- NSP style controller IMU
M.gamepad_nsp_imu = make_factory{
  id = "virt_imu",
  name = "Aelkey Pro Controller (IMU)",
  bus = "bluetooth",
  capabilities = {
    "INPUT_PROP_ACCELEROMETER",
    { code="ABS_X", min=-32767, max=32767, fuzz=10, resolution=4096 },
    { code="ABS_Y", min=-32767, max=32767, fuzz=10, resolution=4096 },
    { code="ABS_Z", min=-32767, max=32767, fuzz=10, resolution=4096 },
    { code="ABS_RX", min=-32767000, max=32767000, fuzz=10, resolution=14247 },
    { code="ABS_RY", min=-32767000, max=32767000, fuzz=10, resolution=14247 },
    { code="ABS_RZ", min=-32767000, max=32767000, fuzz=10, resolution=14247 },
    "MSC_TIMESTAMP",
  },
}

-- DS4 style controller
M.gamepad_ds4 = make_factory{
  id = "virt_gamepad",
  name = "Aelkey Wireless Controller",
  bus = "bluetooth",
  capabilities = {
    "BTN_SOUTH", "BTN_EAST", "BTN_NORTH", "BTN_WEST",
    "BTN_TL", "BTN_TR", "BTN_TL2", "BTN_TR2",
    "BTN_SELECT", "BTN_START", "BTN_MODE",
    "BTN_THUMBL", "BTN_THUMBR",
    { code="ABS_X", min=0, max=255, fuzz=4, flat=16 },
    { code="ABS_Y", min=0, max=255, fuzz=4, flat=16 },
    { code="ABS_Z", min=0, max=255, fuzz=4, flat=16 },
    { code="ABS_RX", min=0, max=255, fuzz=4, flat=16 },
    { code="ABS_RY", min=0, max=255, fuzz=4, flat=16 },
    { code="ABS_RZ", min=0, max=255, fuzz=4, flat=16 },
    { code="ABS_HAT0X", min=-1, max=1 },
    { code="ABS_HAT0Y", min=-1, max=1 },
    "FF_RUMBLE",
  },
}

-- DS4 style controller IMU
M.gamepad_ds5_imu = make_factory{
  id = "virt_imu",
  name = "Aelkey Wireless Controller Motion Sensors",
  bus = "bluetooth",
  capabilities = {
    "INPUT_PROP_ACCELEROMETER",
    { code="ABS_X", min=-32768, max=32768, fuzz=16, resolution=8192 },
    { code="ABS_Y", min=-32768, max=32768, fuzz=16, resolution=8192 },
    { code="ABS_Z", min=-32768, max=32768, fuzz=16, resolution=8192 },
    { code="ABS_RX", min=-2097152, max=2097152, fuzz=16, resolution=1024 },
    { code="ABS_RY", min=-2097152, max=2097152, fuzz=16, resolution=1024 },
    { code="ABS_RZ", min=-2097152, max=2097152, fuzz=16, resolution=1024 },
    "MSC_TIMESTAMP",
  },
}

M.keyboard = make_factory{
  id = "virt_keyboard",
  name = "Aelkey Keyboard",
  capabilities = {
    "KEY_ESC", "KEY_1", "KEY_2", "KEY_3", "KEY_4", "KEY_5", "KEY_6",
    "KEY_7", "KEY_8", "KEY_9", "KEY_0", "KEY_MINUS", "KEY_EQUAL",
    "KEY_BACKSPACE", "KEY_TAB", "KEY_Q", "KEY_W", "KEY_E", "KEY_R",
    "KEY_T", "KEY_Y", "KEY_U", "KEY_I", "KEY_O", "KEY_P",
    "KEY_LEFTBRACE", "KEY_RIGHTBRACE", "KEY_ENTER", "KEY_LEFTCTRL",
    "KEY_A", "KEY_S", "KEY_D", "KEY_F", "KEY_G", "KEY_H", "KEY_J",
    "KEY_K", "KEY_L", "KEY_SEMICOLON", "KEY_APOSTROPHE", "KEY_GRAVE",
    "KEY_LEFTSHIFT", "KEY_BACKSLASH", "KEY_Z", "KEY_X", "KEY_C",
    "KEY_V", "KEY_B", "KEY_N", "KEY_M", "KEY_COMMA", "KEY_DOT",
    "KEY_SLASH", "KEY_RIGHTSHIFT", "KEY_KPASTERISK", "KEY_LEFTALT",
    "KEY_SPACE", "KEY_CAPSLOCK", "KEY_F1", "KEY_F2", "KEY_F3",
    "KEY_F4", "KEY_F5", "KEY_F6", "KEY_F7", "KEY_F8", "KEY_F9",
    "KEY_F10", "KEY_NUMLOCK", "KEY_SCROLLLOCK", "KEY_KP7", "KEY_KP8",
    "KEY_KP9", "KEY_KPMINUS", "KEY_KP4", "KEY_KP5", "KEY_KP6",
    "KEY_KPPLUS", "KEY_KP1", "KEY_KP2", "KEY_KP3", "KEY_KP0",
    "KEY_KPDOT", "KEY_ZENKAKUHANKAKU", "KEY_102ND", "KEY_F11",
    "KEY_F12", "KEY_RO", "KEY_KATAKANA", "KEY_HIRAGANA", "KEY_HENKAN",
    "KEY_KATAKANAHIRAGANA", "KEY_MUHENKAN", "KEY_KPJPCOMMA",
    "KEY_KPENTER", "KEY_RIGHTCTRL", "KEY_KPSLASH", "KEY_SYSRQ",
    "KEY_RIGHTALT", "KEY_HOME", "KEY_UP", "KEY_PAGEUP", "KEY_LEFT",
    "KEY_RIGHT", "KEY_END", "KEY_DOWN", "KEY_PAGEDOWN", "KEY_INSERT",
    "KEY_DELETE", "KEY_MACRO", "KEY_MUTE", "KEY_VOLUMEDOWN",
    "KEY_VOLUMEUP", "KEY_POWER", "KEY_KPEQUAL", "KEY_KPPLUSMINUS",
    "KEY_PAUSE", "KEY_KPCOMMA", "KEY_HANJA", "KEY_YEN", "KEY_LEFTMETA",
    "KEY_RIGHTMETA", "KEY_COMPOSE", "KEY_STOP", "KEY_AGAIN",
    "KEY_PROPS", "KEY_UNDO", "KEY_FRONT", "KEY_COPY", "KEY_OPEN",
    "KEY_PASTE", "KEY_FIND", "KEY_CUT", "KEY_HELP", "KEY_MENU",
    "KEY_CALC", "KEY_SLEEP", "KEY_WAKEUP", "KEY_WWW", "KEY_MSDOS",
    "KEY_MAIL", "KEY_BOOKMARKS", "KEY_COMPUTER", "KEY_BACK",
    "KEY_FORWARD", "KEY_EJECTCD", "KEY_NEXTSONG", "KEY_PLAYPAUSE",
    "KEY_PREVIOUSSONG", "KEY_STOPCD", "KEY_CONFIG", "KEY_HOMEPAGE",
    "KEY_REFRESH", "KEY_EDIT", "KEY_SCROLLUP", "KEY_SCROLLDOWN",
    "KEY_KPLEFTPAREN", "KEY_KPRIGHTPAREN", "KEY_F13", "KEY_F14",
    "KEY_F15", "KEY_F16", "KEY_F17", "KEY_F18", "KEY_F19", "KEY_F20",
    "KEY_F21", "KEY_F22", "KEY_F23", "KEY_F24", "KEY_SEARCH",
    "KEY_MEDIA", "KEY_UNKNOWN", "KEY_KEYBOARD", "KEY_SCREENSAVER",
  },
}

M.mouse = make_factory{
  id = "virt_mouse",
  name = "Aelkey Mouse",
  capabilities = {
    "BTN_LEFT", "BTN_RIGHT", "BTN_MIDDLE", "BTN_SIDE", "BTN_EXTRA",
    "REL_X", "REL_Y",
    "REL_WHEEL", "REL_HWHEEL",
    "REL_WHEEL_HI_RES", "REL_HWHEEL_HI_RES",
  },
}

M.consumer = make_factory{
  id = "virt_consumer",
  name = "Aelkey Consumer Control",
  capabilities = {
    "KEY_ESC", "KEY_ENTER", "KEY_KPMINUS", "KEY_KPPLUS", "KEY_UP",
    "KEY_LEFT", "KEY_RIGHT", "KEY_DOWN", "KEY_INSERT", "KEY_DELETE",
    "KEY_MUTE", "KEY_VOLUMEDOWN", "KEY_VOLUMEUP", "KEY_POWER",
    "KEY_PAUSE", "KEY_SCALE", "KEY_STOP", "KEY_PROPS", "KEY_UNDO",
    "KEY_COPY", "KEY_OPEN", "KEY_PASTE", "KEY_FIND", "KEY_CUT",
    "KEY_HELP", "KEY_MENU", "KEY_CALC", "KEY_SLEEP", "KEY_FILE",
    "KEY_WWW", "KEY_MAIL", "KEY_BOOKMARKS", "KEY_BACK", "KEY_FORWARD",
    "KEY_EJECTCD", "KEY_NEXTSONG", "KEY_PLAYPAUSE", "KEY_PREVIOUSSONG",
    "KEY_STOPCD", "KEY_RECORD", "KEY_REWIND", "KEY_PHONE",
    "KEY_CONFIG", "KEY_HOMEPAGE", "KEY_REFRESH", "KEY_EXIT",
    "KEY_EDIT", "KEY_SCROLLUP", "KEY_SCROLLDOWN", "KEY_NEW",
    "KEY_REDO", "KEY_CLOSE", "KEY_PLAY", "KEY_FASTFORWARD",
    "KEY_BASSBOOST", "KEY_PRINT", "KEY_CAMERA", "KEY_CHAT",
    "KEY_SEARCH", "KEY_FINANCE", "KEY_CANCEL", "KEY_BRIGHTNESSDOWN",
    "KEY_BRIGHTNESSUP", "KEY_KBDILLUMTOGGLE", "KEY_KBDILLUMDOWN",
    "KEY_KBDILLUMUP", "KEY_SEND", "KEY_REPLY", "KEY_FORWARDMAIL",
    "KEY_SAVE", "KEY_DOCUMENTS", "KEY_UNKNOWN", "KEY_VIDEO_NEXT",
    "BTN_0", "KEY_SELECT", "KEY_GOTO", "KEY_INFO", "KEY_PROGRAM",
    "KEY_PVR", "KEY_SUBTITLE", "KEY_KEYBOARD", "KEY_PC", "KEY_TV",
    "KEY_TV2", "KEY_VCR", "KEY_VCR2", "KEY_SAT", "KEY_CD", "KEY_TAPE",
    "KEY_TUNER", "KEY_PLAYER", "KEY_DVD", "KEY_AUDIO", "KEY_VIDEO",
    "KEY_MEMO", "KEY_CALENDAR", "KEY_RED", "KEY_GREEN", "KEY_YELLOW",
    "KEY_BLUE", "KEY_CHANNELUP", "KEY_CHANNELDOWN", "KEY_LAST",
    "KEY_NEXT", "KEY_RESTART", "KEY_SLOW", "KEY_SHUFFLE",
    "KEY_PREVIOUS", "KEY_VIDEOPHONE", "KEY_GAMES", "KEY_ZOOMIN",
    "KEY_ZOOMOUT", "KEY_ZOOMRESET", "KEY_WORDPROCESSOR", "KEY_EDITOR",
    "KEY_SPREADSHEET", "KEY_GRAPHICSEDITOR", "KEY_PRESENTATION",
    "KEY_DATABASE", "KEY_NEWS", "KEY_VOICEMAIL", "KEY_ADDRESSBOOK",
    "KEY_MESSENGER", "KEY_DISPLAYTOGGLE", "KEY_SPELLCHECK",
    "KEY_LOGOFF", "KEY_MEDIA_REPEAT", "KEY_IMAGES", "KEY_BUTTONCONFIG",
    "KEY_TASKMANAGER", "KEY_JOURNAL", "KEY_CONTROLPANEL",
    "KEY_APPSELECT", "KEY_SCREENSAVER", "KEY_VOICECOMMAND",
    "KEY_ASSISTANT", "KEY_KBD_LAYOUT_NEXT", "KEY_EMOJI_PICKER",
    "KEY_DICTATE", "KEY_CAMERA_ACCESS_ENABLE",
    "KEY_CAMERA_ACCESS_DISABLE", "KEY_CAMERA_ACCESS_TOGGLE",
    "KEY_BRIGHTNESS_MIN", "KEY_BRIGHTNESS_MAX",
    "KEY_KBDINPUTASSIST_PREV", "KEY_KBDINPUTASSIST_NEXT",
    "KEY_KBDINPUTASSIST_PREVGROUP", "KEY_KBDINPUTASSIST_NEXTGROUP",
    "KEY_KBDINPUTASSIST_ACCEPT", "KEY_KBDINPUTASSIST_CANCEL",

    "REL_WHEEL", "REL_HWHEEL",
    "REL_WHEEL_HI_RES", "REL_HWHEEL_HI_RES",

    { code="ABS_VOLUME", min=0, max=767 },
  },
}

M.touchpad = make_factory{
  id = "virt_touchpad",
  name = "Aelkey Touchpad",
  capabilities = {
    "INPUT_PROP_POINTER",
    "BTN_LEFT", "BTN_RIGHT", "BTN_MIDDLE",
    "BTN_SIDE", "BTN_EXTRA", "BTN_FORWARD", "BTN_BACK", "BTN_TASK",
    "BTN_TOUCH",
    "BTN_TOOL_FINGER",
    "BTN_TOOL_DOUBLETAP",
    "BTN_TOOL_TRIPLETAP",
    "BTN_TOOL_QUADTAP",
    "BTN_TOOL_QUINTTAP",

    "REL_WHEEL", "REL_HWHEEL",
    "REL_WHEEL_HI_RES", "REL_HWHEEL_HI_RES",

    { code = "ABS_X", min = 0, max = 4095, resolution = 25 },
    { code = "ABS_Y", min = 0, max = 4095, resolution = 25 },
  },
}

return M
