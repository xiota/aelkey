aelkey = require("aelkey")
aelkey.click.configure{window = 500, interval = 20}

inputs = {
  {
    id = "audio_in",
    type = "audio",
    name = ".*Stereo:capture_FL",
    on_event = "on_audio",
  },
}

local function compute_peak_and_rms(ev)
  local pos = 1
  local peak = 0.0
  local sum = 0.0

  for i = 1, ev.frames do
    local s
    s, pos = string.unpack("=f", ev.data, pos)
    local a = math.abs(s)
    if a > peak then peak = a end
    sum = sum + s * s
  end

  local rms = math.sqrt(sum / ev.frames)
  return peak, rms
end

local noise = 0
local last_peak = 0

local PEAK_T = 0.3    -- absolute minimum peak
local ATTACK_T = 0.2  -- minimum rise rate
local RMS_T = 0.05

function on_audio(events)
  local scope <close> = aelkey.util.bench_scope("remap")
  local now = aelkey.util.now("ms")

  for _, ev in ipairs(events) do
    local peak, rms = compute_peak_and_rms(ev)
    local attack = peak - last_peak

    -- update adaptive noise floor
    noise = aelkey.filter.lowpass_ema("noise_floor", peak, 0.05)
    local dyn_signal = noise * 4.0
    local dyn_threshold = math.max(PEAK_T, dyn_signal)

    -- clap detection
    local is_clap = peak > dyn_threshold
      and attack > ATTACK_T
      and rms > RMS_T

    if peak > dyn_signal then
      aelkey.log.trace(aelkey.util.dump_table{
        peak = peak,
        attack = attack,
        rms = rms,
        noise = noise,
        dyn_thr = dyn_threshold,
        clap = is_clap,
      })
    end

    if is_clap then
      aelkey.click.detect(
        "clap",
        function() print("Clap On!") end,
        function() print("Clap Off!!") end,
        function() print("The Clapper!!!") end
      )
    end

    last_peak = peak
  end
end

aelkey.start()
