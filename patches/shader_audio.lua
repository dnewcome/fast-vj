-- shader_audio.lua — drive shader parameters from audio
-- Switches to the plasma shader and animates it with FFT data.

-- Find the plasma shader by name
local plasma_idx = 0
for i = 0, vj.num_shaders() - 1 do
    -- shaders are loaded alphabetically; print them all so we can see
    vj.print("shader[" .. i .. "] available")
end

-- plasma is index 2 (alphabetical: default=0, oscilloscope=1, plasma=2, spectrum=3)
vj.shader(2)
vj.print("switched to plasma shader")

-- Start the test audio
vj.audio(0)
vj.print("audio clips: " .. vj.num_audio())

function on_frame(dt)
    -- Bass energy (bins 1-8, ~21-170 Hz) drives plasma speed via u_p[0]
    local bass = 0
    for i = 1, 8 do bass = bass + vj.fft(i) end
    bass = bass / 8
    vj.uniform(0, 0.5 + bass * 4.0)

    -- High-mid energy (bins 100-300) drives image blend via u_p[1]
    local mid = 0
    for i = 100, 300 do mid = mid + vj.fft(i) end
    mid = mid / 200
    vj.uniform(1, mid)
end
