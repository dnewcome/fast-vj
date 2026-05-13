--[[
  stress.lua — deterministic worst-case workload for benchmarking fast-vj.

  Cycles every available shader, video, and image, and sweeps all 15
  shader uniforms each frame. No audio or OSC dependency — output FPS
  depends only on hardware and the render pipeline.

  Run via scripts/stress.sh, or directly:
    ./build/fast-vj media/ 9000 -f -s patches/stress.lua
--]]

local SHADER_PERIOD = 0.5   -- s between shader switches
local VIDEO_PERIOD  = 1.0   -- s between video cuts
local IMAGE_PERIOD  = 0.75  -- s between image cuts

local t = 0.0
local last_shader_t = -1.0
local last_video_t  = -1.0
local last_image_t  = -1.0
local shader_idx, video_idx, image_idx = 0, 0, 0

local num_shaders = vj.num_shaders()
local num_video   = vj.num_video()
local num_image   = vj.num_image()

vj.print(string.format("stress: %d shaders, %d videos, %d images",
    num_shaders, num_video, num_image))

function on_frame(dt)
    t = t + dt

    -- Sweep all 15 uniforms each frame. sin gives [-1,1]; +1 yields
    -- [0,2], a range most shaders treat as a useful parameter scale.
    for i = 0, 14 do
        local phase = t * (0.5 + i * 0.13)
        vj.uniform(i, 1.0 + math.sin(phase))
    end

    if num_shaders > 1 and t - last_shader_t >= SHADER_PERIOD then
        shader_idx = (shader_idx + 1) % num_shaders
        vj.shader(shader_idx)
        last_shader_t = t
    end

    if num_video > 1 and t - last_video_t >= VIDEO_PERIOD then
        video_idx = (video_idx + 1) % num_video
        vj.video(video_idx)
        last_video_t = t
    end

    if num_image > 1 and t - last_image_t >= IMAGE_PERIOD then
        image_idx = (image_idx + 1) % num_image
        vj.image(image_idx)
        last_image_t = t
    end
end
