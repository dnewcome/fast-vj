--[[
  stress_video.lua — minimum-overhead video playback baseline.

  Locks the default passthrough shader on top of the first available
  video. Measures the engine's video-playback floor cost (JPEG decode,
  texture upload, single textured quad) — the apples-to-apples
  comparison for tools like freejay that don't run shaders.

  Triplet:
    stress.lua              — engine stress (cycles everything)
    stress_kaleidoscope.lua — GPU stress (heavy shader)
    stress_video.lua        — video baseline (this file)
--]]

if vj.num_shaders() > 0 then
    vj.shader(0)   -- default passthrough (alphabetical first)
    vj.print("stress_video: locked shader 0 (default)")
end

if vj.num_video() > 0 then
    vj.video(0)
    vj.print("stress_video: playing video 0")
else
    vj.print("stress_video: WARNING no video — output will be blank")
end
