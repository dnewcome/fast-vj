--[[
  example.lua — fast-vj starter patch
  Load with:  fast-vj media/ 9000 -s patches/example.lua

  Demonstrates:
    - Reacting to OSC triggers
    - Per-frame audio analysis
    - Cross-domain: FFT bass energy → auto video cut
--]]

-- Print loaded clip counts at startup
vj.print("clips loaded: " ..
    vj.num_audio() .. " audio, " ..
    vj.num_image() .. " image, " ..
    vj.num_video() .. " video")

-- ---- State ----
local current_video = 0
local cut_cooldown  = 0.0   -- seconds until next auto-cut is allowed
local CUT_INTERVAL  = 2.0   -- minimum seconds between auto-cuts

-- ---- on_osc: called when an OSC event fires ----
-- The C engine already handled the event; this lets you layer
-- additional logic on top or override it.
function on_osc(addr, arg)
    if addr == "/vj/video" and arg then
        current_video = arg
        vj.print("video cut to " .. arg)
    end
end

-- ---- on_frame: called every render frame ----
-- dt = seconds since last frame (~0.033 at 30fps, ~0.016 at 60fps)
function on_frame(dt)
    cut_cooldown = cut_cooldown - dt

    -- Sample bass energy from first 8 FFT bins (~0-170 Hz at 44100/2048)
    local bass = 0
    for i = 0, 7 do
        bass = bass + vj.fft(i)
    end
    bass = bass / 8

    -- Auto-cut to next video on a strong bass hit
    if bass > 0.6 and cut_cooldown <= 0 and vj.num_video() > 1 then
        current_video = (current_video + 1) % vj.num_video()
        vj.video(current_video)
        cut_cooldown = CUT_INTERVAL
    end

    -- Map peak audio amplitude to gain (simple ducking demo)
    -- local peak = 0
    -- for i = 0, 63 do
    --     local s = math.abs(vj.sample(i))
    --     if s > peak then peak = s end
    -- end
    -- vj.gain(0.3 + peak * 0.7)
end
