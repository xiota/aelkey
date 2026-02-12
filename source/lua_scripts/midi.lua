--[[
  aelkey.midi
  Structured helpers for decoding and encoding common MIDI messages.

  Features:
    * Stateless decoding of channel‑voice messages into Lua tables
    * Encoding of structured tables back into raw MIDI bytes
    * Lightweight SysEx framing (F0 ... F7)
    * Symbolic note names (aelkey.midi.NOTES["C4"] → 60)

  Usage:
    -- Decode raw MIDI bytes into structured messages
    local msgs = aelkey.midi.decode(event.data)
    for _, m in ipairs(msgs) do
      if m.type == "note_on" then
        print("note:", m.note, "vel:", m.velocity)
      end
    end

    -- Encode a structured message into raw bytes
    local data = aelkey.midi.encode{
      type = "cc",
      channel = 0,
      controller = 74,
      value = 32,
    }

    -- Send through an Ælkey MIDI output
    aelkey.midi.send("out1", data)

    -- Use symbolic note names
    local n = aelkey.midi.NOTES["C4"]   -- 60
]]--

local M = ...

-- Internal helpers
local function decode_status(byte)
  local status = byte & 0xF0
  local chan = byte & 0x0F
  return status, chan
end

local function is_status(byte)
  return byte >= 0x80
end

local function is_sysex_start(byte)
  return byte == 0xF0
end

local function is_sysex_end(byte)
  return byte == 0xF7
end

-- Public: decode(bytes) → {msg1, msg2, ...}
function M.decode(bytes)
  local out = {}
  local b = {string.byte(bytes, 1, #bytes)}
  local i = 1

  while i <= #b do
    local byte = b[i]

    -- SysEx framing (simple)
    if is_sysex_start(byte) then
      local payload = {}
      i = i + 1
      while i <= #b and not is_sysex_end(b[i]) do
        payload[#payload+1] = b[i]
        i = i + 1
      end
      -- skip F7 if present
      if i <= #b and is_sysex_end(b[i]) then
        i = i + 1
      end
      out[#out+1] = { type = "sysex", data = payload }
      goto continue
    end

    -- Channel voice messages
    if is_status(byte) then
      local status, chan = decode_status(byte)

      -- 2‑data‑byte messages
      if status == 0x80 or status == 0x90 or
         status == 0xA0 or status == 0xB0 or
         status == 0xE0 then

        local d1 = b[i+1]
        local d2 = b[i+2]
        if not d1 or not d2 then break end

        if status == 0x80 then
          out[#out+1] = { type="note_off", channel=chan, note=d1, velocity=d2 }
        elseif status == 0x90 then
          out[#out+1] = { type="note_on", channel=chan, note=d1, velocity=d2 }
        elseif status == 0xA0 then
          out[#out+1] = { type="poly_aftertouch", channel=chan, note=d1, pressure=d2 }
        elseif status == 0xB0 then
          out[#out+1] = { type="cc", channel=chan, controller=d1, value=d2 }
        elseif status == 0xE0 then
          local value = d1 | (d2 << 7)
          out[#out+1] = { type="pitchbend", channel=chan, value=value }
        end

        i = i + 3
        goto continue
      end

      -- 1‑data‑byte messages
      if status == 0xC0 then
        local d1 = b[i+1]
        if not d1 then break end
        out[#out+1] = { type="program_change", channel=chan, program=d1 }
        i = i + 2
        goto continue
      end

      if status == 0xD0 then
        local d1 = b[i+1]
        if not d1 then break end
        out[#out+1] = { type="channel_aftertouch", channel=chan, pressure=d1 }
        i = i + 2
        goto continue
      end
    end

    -- Unhandled byte → skip
    i = i + 1

    ::continue::
  end

  return out
end

-- Public: encode(msg) → raw binary string
function M.encode(msg)
  local t = msg.type
  local ch = msg.channel or 0
  local out = {}

  local function push(...) for _,v in ipairs({...}) do out[#out+1] = v end end

  if t == "note_on" then
    push(0x90 | ch, msg.note, msg.velocity)

  elseif t == "note_off" then
    push(0x80 | ch, msg.note, msg.velocity)

  elseif t == "cc" then
    push(0xB0 | ch, msg.controller, msg.value)

  elseif t == "program_change" then
    push(0xC0 | ch, msg.program)

  elseif t == "channel_aftertouch" then
    push(0xD0 | ch, msg.pressure)

  elseif t == "poly_aftertouch" then
    push(0xA0 | ch, msg.note, msg.pressure)

  elseif t == "pitchbend" then
    local v = msg.value or 8192
    local lsb = v & 0x7F
    local msb = (v >> 7) & 0x7F
    push(0xE0 | ch, lsb, msb)

  elseif t == "sysex" then
    push(0xF0)
    for _,v in ipairs(msg.data or {}) do push(v) end
    push(0xF7)
  end

  return string.char(table.unpack(out))
end

-- Base note names
local NAMES = {"C","C#","D","D#","E","F","F#","G","G#","A","A#","B"}

-- Helper to generate note-name → MIDI tables
local function make_notes(octave_offset)
  local t = {}
  for midi = 0, 127 do
    local name = NAMES[(midi % 12) + 1]
    local octave = math.floor(midi / 12) + octave_offset
    t[name .. octave] = midi
  end
  return t
end

-- Helper to generate reverse tables (MIDI → note-name)
local function make_reverse(notes)
  local rev = {}
  for name, num in pairs(notes) do
    -- keep first canonical name only
    if not rev[num] then
      rev[num] = name
    end
  end
  return rev
end

-- C3 system: 60 = C3
M.NOTES_C3 = make_notes(-2)
M.NOTE_NAMES_C3 = make_reverse(M.NOTES_C3)

-- C4 system: 60 = C4 (scientific pitch)
M.NOTES_C4 = make_notes(-1)
M.NOTE_NAMES_C4 = make_reverse(M.NOTES_C4)

-- C5 system: 60 = C5
M.NOTES_C5 = make_notes(0)
M.NOTE_NAMES_C5 = make_reverse(M.NOTES_C5)

-- default
M.NOTES = M.NOTES_C4
M.NOTE_NAMES = M.NOTE_NAMES_C4
