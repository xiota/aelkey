#pragma once

namespace aelkey::capabilities {

// clang-format off
constexpr int keyboard_keys[] = {
  // Letters
  KEY_A, KEY_B, KEY_C, KEY_D, KEY_E, KEY_F, KEY_G, KEY_H, KEY_I, KEY_J, KEY_K, KEY_L, KEY_M, KEY_N, KEY_O, KEY_P, KEY_Q, KEY_R, KEY_S, KEY_T, KEY_U, KEY_V, KEY_W, KEY_X, KEY_Y, KEY_Z,

  // Numbers (top row)
  KEY_1, KEY_2, KEY_3, KEY_4, KEY_5, KEY_6, KEY_7, KEY_8, KEY_9, KEY_0,

  // Function keys
  KEY_F1, KEY_F2, KEY_F3, KEY_F4, KEY_F5, KEY_F6, KEY_F7, KEY_F8, KEY_F9, KEY_F10, KEY_F11, KEY_F12, KEY_F13, KEY_F14, KEY_F15, KEY_F16, KEY_F17, KEY_F18, KEY_F19, KEY_F20, KEY_F21, KEY_F22, KEY_F23, KEY_F24,

  // Modifiers
  KEY_LEFTSHIFT, KEY_RIGHTSHIFT, KEY_LEFTCTRL, KEY_RIGHTCTRL, KEY_LEFTALT, KEY_RIGHTALT, KEY_LEFTMETA, KEY_RIGHTMETA, KEY_CAPSLOCK, KEY_COMPOSE, KEY_NUMLOCK, KEY_SCROLLLOCK,

  // Navigation / editing
  KEY_UP, KEY_DOWN, KEY_LEFT, KEY_RIGHT, KEY_HOME, KEY_END, KEY_PAGEUP, KEY_PAGEDOWN, KEY_INSERT, KEY_DELETE, KEY_BACKSPACE, KEY_ENTER, KEY_ESC, KEY_TAB, KEY_SPACE,

  // Symbols (punctuation row)
  KEY_MINUS, KEY_EQUAL, KEY_LEFTBRACE, KEY_RIGHTBRACE, KEY_SEMICOLON, KEY_APOSTROPHE, KEY_GRAVE, KEY_BACKSLASH, KEY_COMMA, KEY_DOT, KEY_SLASH,

  // Extra keypad keys
  KEY_KP0, KEY_KP1, KEY_KP2, KEY_KP3, KEY_KP4, KEY_KP5, KEY_KP6, KEY_KP7, KEY_KP8, KEY_KP9,
  KEY_KPSLASH, KEY_KPASTERISK, KEY_KPMINUS, KEY_KPPLUS, KEY_KPENTER, KEY_KPDOT,
  KEY_KPEQUAL, KEY_KPCOMMA, KEY_KPLEFTPAREN, KEY_KPRIGHTPAREN,

  // Media / system (still considered keyboard, not consumer)
  KEY_POWER, KEY_SLEEP, KEY_SCREENLOCK, KEY_SYSRQ, KEY_PAUSE, KEY_PRINT, KEY_MENU,
  KEY_MUTE, KEY_VOLUMEDOWN, KEY_VOLUMEUP, KEY_EJECTCD, KEY_NEXTSONG, KEY_PLAYPAUSE, KEY_PREVIOUSSONG, KEY_STOPCD, KEY_WWW, KEY_BACK, KEY_FORWARD, KEY_REFRESH, KEY_SCROLLUP, KEY_SCROLLDOWN,

  // International / layout-specific
  KEY_ZENKAKUHANKAKU, KEY_RO, KEY_KATAKANA, KEY_HIRAGANA, KEY_HENKAN, KEY_KATAKANAHIRAGANA, KEY_MUHENKAN, KEY_KPJPCOMMA, KEY_HANGUEL, KEY_HANJA, KEY_YEN, KEY_102ND,

  // Editing / office keys
  KEY_STOP, KEY_AGAIN, KEY_PROPS, KEY_UNDO, KEY_FRONT, KEY_COPY, KEY_OPEN, KEY_PASTE, KEY_FIND, KEY_CUT, KEY_HELP, KEY_CALC, KEY_EDIT,

  // Miscellaneous
  KEY_UNKNOWN,
};

constexpr int consumer_keys[] = {
  // Playback controls
  KEY_PLAY, KEY_PAUSE, KEY_PLAYPAUSE, KEY_STOP, KEY_STOPCD, KEY_RECORD, KEY_NEXTSONG, KEY_PREVIOUSSONG, KEY_FASTFORWARD, KEY_REWIND,

  // Volume controls
  KEY_VOLUMEUP, KEY_VOLUMEDOWN, KEY_MUTE,

  // Browser / navigation
  KEY_WWW, KEY_BACK, KEY_FORWARD, KEY_REFRESH, KEY_STOP, KEY_SEARCH, KEY_FAVORITES, KEY_HOMEPAGE, KEY_MAIL,

  // Application launch
  KEY_EMAIL, KEY_CALC, KEY_COMPUTER, KEY_MEDIA, KEY_CHAT, KEY_PHONE,

  // System power
  KEY_POWER, KEY_SLEEP, KEY_WAKEUP,

  // Programmable keys
  KEY_PROG1, KEY_PROG2, KEY_PROG3, KEY_PROG4,

  // Multimedia / device
  KEY_CAMERA, KEY_VIDEO, KEY_AUDIO, KEY_CD, KEY_TUNER, KEY_MP3, KEY_DVD, KEY_RADIO, KEY_TV, KEY_VCR,
};

constexpr int gamepad_buttons[] = {
  BTN_A,       // South
  BTN_B,       // East
  BTN_X,       // North
  BTN_Y,       // West
  BTN_C,       // Extra (rare, sometimes Capture)
  BTN_Z,       // Extra (rare)
  BTN_TL,      // L1
  BTN_TR,      // R1
  BTN_TL2,     // L2 (ZL)
  BTN_TR2,     // R2 (ZR)
  BTN_SELECT,  // Minus
  BTN_START,   // Plus
  BTN_MODE,    // Home / Guide
  BTN_THUMBL,  // Left stick click
  BTN_THUMBR   // Right stick click
};

constexpr int gamepad_abs[] = {
  ABS_X,     ABS_Y,     // Left stick
  ABS_RX,    ABS_RY,    // Right stick
  ABS_Z,     ABS_RZ,    // Triggers
  ABS_HAT0X, ABS_HAT0Y  // D-pad
};

constexpr int mouse_buttons[] = {
  BTN_LEFT,  BTN_RIGHT,   BTN_MIDDLE, BTN_SIDE, BTN_EXTRA, BTN_FORWARD, BTN_BACK,   BTN_TASK,
};

constexpr int mouse_rel[] = {
  REL_X, REL_Y, REL_WHEEL, REL_HWHEEL, REL_WHEEL_HI_RES, REL_HWHEEL_HI_RES,
};

constexpr int touchpad_buttons[] = {
  BTN_LEFT, BTN_RIGHT, BTN_MIDDLE,
  BTN_SIDE, BTN_EXTRA, BTN_FORWARD, BTN_BACK, BTN_TASK,
  BTN_TOUCH,
  BTN_TOOL_FINGER,
  BTN_TOOL_DOUBLETAP,
  BTN_TOOL_TRIPLETAP,
  BTN_TOOL_QUADTAP,
  BTN_TOOL_QUINTTAP,
};

constexpr int touchpad_rel[] = {
  REL_WHEEL, REL_HWHEEL,
  REL_WHEEL_HI_RES, REL_HWHEEL_HI_RES,
};

constexpr int touchpad_abs[] = {
  ABS_X, ABS_Y,
};

constexpr int touchpad_mt_abs[] = {
  ABS_X, ABS_Y,
  ABS_MT_SLOT,
  ABS_MT_POSITION_X, ABS_MT_POSITION_Y,
  ABS_MT_TRACKING_ID,
  ABS_MT_TOOL_TYPE,
};

constexpr int touchscreen_keys[] = {
  BTN_TOUCH,
};

constexpr int touchscreen_abs[] = {
  // Core absolute axes
  ABS_X, ABS_Y,

  // Multitouch slot management
  ABS_MT_SLOT,          // slot selector (0–4 for 5 fingers)
  ABS_MT_POSITION_X,    // X coordinate per slot
  ABS_MT_POSITION_Y,    // Y coordinate per slot
  ABS_MT_TRACKING_ID,   // unique ID per finger
  ABS_MT_PRESSURE,      // optional pressure
  ABS_MT_TOUCH_MAJOR,   // major axis of contact ellipse
  ABS_MT_TOUCH_MINOR    // minor axis of contact ellipse
};

constexpr int digitizer_keys[] = {
  BTN_TOOL_PEN,
  BTN_TOOL_RUBBER,
  BTN_TOOL_BRUSH,
  BTN_TOOL_PENCIL,
  BTN_TOOL_AIRBRUSH,
  BTN_TOOL_FINGER,
  BTN_TOOL_MOUSE,
  BTN_TOOL_LENS,
  BTN_STYLUS,
  BTN_STYLUS2,
  BTN_STYLUS3,
  BTN_TOUCH,
};

constexpr int digitizer_abs[] = {
  ABS_X, ABS_Y,
  ABS_PRESSURE,
  ABS_TILT_X, ABS_TILT_Y,
  ABS_DISTANCE,
  ABS_WHEEL,
};

// clang-format on

}  // namespace aelkey::capabilities
