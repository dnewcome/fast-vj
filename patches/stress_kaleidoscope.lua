--[[
  stress_kaleidoscope.lua — heavy GPU workload for benchmarking.

  Locks the kaleidoscope shader on top of the first available video
  (typically media/test_mandelbrot.avi from media/make-test.sh) and
  continuously sweeps its parameters across worst-case ranges:
  max segments, fast spin, deep zoom, fast image rotation.

  Combined with vsync off (-V), this reveals the GPU's sustained
  ceiling. Real-workload counterpart to patches/stress.lua, which
  exercises the engine itself by cycling shaders and clips.

  Run via:
    ./scripts/stress.sh --no-vsync -p patches/stress_kaleidoscope.lua
--]]

-- Shaders load alphabetically; in the default set kaleidoscope is index 1
-- (default=0, kaleidoscope=1, oscilloscope=2, plasma=3, spectrum=4).
-- Adjust if your shaders/ directory differs.
local KALEIDO_IDX = 1
local VIDEO_IDX   = 0

if vj.num_shaders() > KALEIDO_IDX then
    vj.shader(KALEIDO_IDX)
    vj.print("stress_kaleidoscope: locked shader " .. KALEIDO_IDX)
else
    vj.print("stress_kaleidoscope: WARNING shader " .. KALEIDO_IDX .. " missing")
end

if vj.num_video() > 0 then
    vj.video(VIDEO_IDX)
    vj.print("stress_kaleidoscope: playing video " .. VIDEO_IDX)
else
    vj.print("stress_kaleidoscope: WARNING no video — kaleidoscope will tint")
end

-- Disable audio reactivity so output is independent of mic input.
vj.uniform(3, 0.0)

local t = 0.0

function on_frame(dt)
    t = t + dt

    -- segments: 2..16 (full range of the shader's clamped input)
    vj.uniform(0, 9.0 + 7.0 * math.sin(t * 0.4))

    -- spin speed: -3..3 rad/s
    vj.uniform(1, 3.0 * math.sin(t * 0.5))

    -- zoom: 1.0..4.0 — deep zoom forces small UV deltas, exercising
    -- mip chain selection and texture sampling cost.
    vj.uniform(2, 2.5 + 1.5 * math.sin(t * 0.7))

    -- image rotate: -2..2 rad/s (independent of segment spin)
    vj.uniform(4, 2.0 * math.sin(t * 0.3))
end
