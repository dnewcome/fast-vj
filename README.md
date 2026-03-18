# fast-vj

A realtime audio/visual sampler for live performance. The guiding idea is
**data is data**: audio waveforms, images, FFT spectra, and geometry are all
just arrays of floats. They live on the GPU as textures and a GLSL shader is
the universal transform between them.

Targets resource-constrained devices — tested on Raspberry Pi 4 and desktop
Ubuntu/Debian.

---

## Concept

Traditional VJ software treats audio and video as separate domains. fast-vj
does not. A scanline of pixels can drive an audio envelope. The magnitude
spectrum of an audio clip can modulate image brightness. A waveform can be
drawn as a path. The goal is to make these cross-domain transforms cheap
enough to happen every frame.

---

## Architecture

```
Media directory
  ├── *.wav   → dr_wav decodes to float32 mono PCM
  ├── *.png   → stb_image decodes to RGBA8, uploaded as immutable GPU texture
  ├── *.avi   → mmap'd MJPEG AVI; frame index built at load time
  └── clip/   → directory of sequential JPEGs (000001.jpg …)
                 all frames malloc'd into one contiguous buffer at startup

OSC UDP listener (background thread)
  receives: /vj/audio <i>, /vj/image <i>, /vj/video <i>,
            /vj/gain <f>, /vj/stop
  writes atomic pending_* fields (memory_order_release)

Render loop (main thread, vsync)
  poll_osc()
    atomic_exchange pending fields → activate clips
  audio callback (ALSA thread)
    reads current_audio, fills buffer from samples, pushes to ring buffer
  update_video_texture()
    video_decode_frame() → libjpeg-turbo NEON decode → sg_update_image()
  sg_begin_pass / draw / sg_end_pass
    fragment shader reads:
      views[0] = current image or video frame (RGBA8)
      views[1] = audio waveform (1D R32F, 2048 samples)
      views[2] = FFT spectrum  (1D R32F, 2048 bins)
```

### Why this is fast on Raspberry Pi

The Pi 4 uses a **unified memory architecture (UMA)**: CPU and GPU share the
same physical RAM. There is no PCIe bus transfer. Calling `sg_update_image()`
is literally a `memcpy` within the same memory pool. This is fundamentally
different from a discrete GPU where texture uploads cross a bus.

For video, `sg_update_image()` with a 2K frame (~8MB RGBA) costs around
8ms — well within a 33ms frame budget. AVI files are `mmap`'d so the OS
manages paging; on Pi with enough RAM to hold the whole clip, all frames
stay resident after the first playthrough.

Sokol uses `SG_USAGE_STREAM` / `glTexSubImage2D` for stream textures, which
never reallocates. This is why previous Pd/Gem approaches using `pix_buffer`
were slow: they triggered `glTexImage2D` (reallocation) on every frame.

### Audio/video sync

The audio buffer size is 1024 samples (power-of-2 for ALSA alignment).
At 44100Hz this is ~23ms per buffer. The render loop runs at vsync
(~60fps desktop, ~30fps Pi). The ring buffer decouples the two rates:
the audio callback pushes into it, the render loop drains it once per frame.

Video frame advance is driven by wall-clock accumulation (`sapp_frame_duration`)
against the clip's FPS, so it stays in sync regardless of render rate.

### The shader pipeline

Everything on the GPU is a 2D texture, even 1D signals — a waveform is stored
as a `2048×1` `R32F` texture. Fragment shaders sample these textures by UV
coordinate and can combine them freely. Crossing domains (audio → visual,
image → audio envelope) is just a texture lookup in GLSL.

---

## Video formats

fast-vj supports two video source formats. Both use libjpeg-turbo (NEON-
accelerated on ARM) to decode one JPEG frame per render loop.

### MJPEG AVI (recommended)

A standard AVI container with a Motion JPEG video stream. The file is
`mmap`'d at startup — no separate allocation, zero copy on Pi UMA. The frame
index is built from the AVI's `idx1` chunk (or by scanning the `movi` LIST
if `idx1` is absent).

Every frame is independently decodable (no inter-frame dependencies), which
gives instant random-access seeking with sub-frame trigger latency.

```bash
# Convert any video to MJPEG AVI, no audio, quality 2 (scale: 2=best, 31=worst)
ffmpeg -i input.mp4 -c:v mjpeg -q:v 2 -an media/clip.avi

# Resize to 1920×1080 while encoding
ffmpeg -i input.mp4 -vf scale=1920:1080 -c:v mjpeg -q:v 2 -an media/clip.avi

# Trim to 10 seconds
ffmpeg -i input.mp4 -t 10 -c:v mjpeg -q:v 2 -an media/clip.avi
```

Approximate file sizes: ~3–6 MB/s at `-q:v 2` for 1080p, ~10–18 MB/s for 2K.
A 10-second 2K clip is roughly 100–180 MB.

### JPEG directory

A directory of sequentially numbered JPEG files. All frames are loaded into
a single contiguous malloc'd buffer at startup. Useful if you already have
frames as individual files, or want fine-grained control over per-frame
quality.

```bash
# Export as numbered JPEGs
ffmpeg -i input.mp4 -q:v 2 media/clip_name/%06d.jpg
```

---

## OSC control

fast-vj listens for OSC messages on a UDP port (default **9000**).

| Address | Args | Action |
|---------|------|--------|
| `/vj/audio` | `i` index | Start audio clip (loops) |
| `/vj/image` | `i` index | Show image clip |
| `/vj/video` | `i` index | Start video clip from frame 0 |
| `/vj/gain`  | `f` 0.0–1.0 | Set audio output gain |
| `/vj/stop`  | — | Stop audio playback |

Clips are indexed in the order they are scanned from the media directory
(alphabetical). Run `fast-vj media/ 9000` and check stdout at startup to
see the assigned indices.

OSC bundles are supported. Commands are processed at the start of each
render frame via `atomic_exchange`, so no command is lost even if it
arrives mid-frame.

Example using `oscsend` (from the `liblo-tools` package):

```bash
# Trigger video clip 0
oscsend localhost 9000 /vj/video i 0

# Trigger audio clip 2 at half gain
oscsend localhost 9000 /vj/gain f 0.5
oscsend localhost 9000 /vj/audio i 2

# Stop audio
oscsend localhost 9000 /vj/stop
```

---

## GLSL shaders

Fragment shaders live in `shaders/*.glsl`. Each file is just a `void main()`
body — the engine injects a standard header so you don't repeat boilerplate.
Shaders are loaded at startup (or from an alternate directory with `-S`).

### Standard header (injected automatically)

Every shader has access to:

```glsl
uniform sampler2D image_tex;  // current image or video frame (RGBA8)
uniform sampler2D audio_tex;  // waveform   — 1D R32F, 2048 wide, range ~[-1,1]
uniform sampler2D fft_tex;    // FFT mag    — 1D R32F, 2048 wide, range [0,1]
in  vec2 uv;                  // fragment UV, (0,0) bottom-left, (1,1) top-right
out vec4 frag_color;

uniform float u_time;         // elapsed seconds since startup
uniform float u_p[15];        // user parameters — set via vj.uniform(i, v)
```

### Writing a shader

Create any `.glsl` file in `shaders/`. The file provides only `void main()`:

```glsl
// shaders/myeffect.glsl
void main() {
    float bass = texture(fft_tex, vec2(0.01, 0.5)).r;
    float wave = texture(audio_tex, vec2(uv.x, 0.5)).r;

    vec3 col = vec3(
        sin(uv.x * 10.0 + u_time * (1.0 + bass * 4.0)),
        wave * 0.5 + 0.5,
        uv.y
    ) * 0.5 + 0.5;

    frag_color = vec4(col, 1.0);
}
```

### Included shaders

| File | Description | Key params |
|------|-------------|------------|
| `default.glsl` | Waveform oscilloscope + FFT bars + image layer | — |
| `spectrum.glsl` | Full-screen colorful FFT spectrum with peak dots | `u_p[0]` brightness |
| `oscilloscope.glsl` | Glowing waveform over image | `u_p[0]` glow, `u_p[1]` amp, `u_p[2]` image blend |
| `plasma.glsl` | Audio-reactive sine plasma | `u_p[0]` speed, `u_p[1]` image blend |

### Using shaders from a Lua patch

```lua
-- Switch to shader by index (alphabetical load order)
vj.shader(2)

-- Set u_p[0] on the current shader
vj.uniform(0, 1.5)

-- Animate a parameter from audio each frame
function on_frame(dt)
    local bass = vj.fft(3) + vj.fft(4) + vj.fft(5)
    vj.uniform(0, bass / 3.0)   -- drive u_p[0] with bass energy
end
```

---

## Lua scripting

fast-vj embeds [LuaJIT](https://luajit.org/) (falling back to Lua 5.4 if
LuaJIT is not installed). A patch is a plain `.lua` file loaded at startup
with `-s patch.lua`. The script runs on the render thread — all `vj.*` calls
are safe from within any callback.

### Loading a patch

```bash
./build/fast-vj media/ 9000 -s patches/example.lua
```

The script is executed once at load time (top-level code runs immediately),
then the engine calls `on_frame` and `on_osc` every frame/event if those
functions are defined.

### Callbacks

```lua
-- Called every render frame.
-- dt: seconds elapsed since the previous frame (~0.033 at 30fps)
function on_frame(dt)
end

-- Called once for each OSC event that the engine processes.
-- addr: OSC address string e.g. "/vj/audio"
-- arg:  integer or float argument, or nil if the message had no argument
function on_osc(addr, arg)
end
```

Both callbacks are optional. If neither is defined the patch can still use
top-level code to set an initial state.

### vj.* API reference

#### Playback triggers

| Function | Description |
|----------|-------------|
| `vj.audio(i)` | Start audio clip `i` (loops). `i` is the clip index printed at startup. |
| `vj.image(i)` | Show image clip `i`. Pass `-1` to clear to blank. |
| `vj.video(i)` | Play video clip `i` from frame 0. Pass `-1` to stop. |
| `vj.gain(f)` | Set master audio gain. `f` is a float, typically `0.0`–`1.0`. |
| `vj.stop()` | Stop audio playback. |

Trigger calls made from `on_frame` take effect on the next frame (they write
to the same atomic queue as incoming OSC messages). At 30fps that is ~33ms —
imperceptible for live VJ work.

#### Audio data

The audio texture is a ring buffer of the most recent samples from the
playing audio clip, updated once per render frame. The FFT magnitude texture
is computed from the same window.

| Function | Returns | Description |
|----------|---------|-------------|
| `vj.sample(i)` | `float` ~[-1, 1] | Raw audio sample at index `i`. `i`: 0–2047. |
| `vj.fft(i)` | `float` [0, 1] | Log-magnitude FFT bin at index `i`. `i`: 0–2047. Bin 0 is DC; bin 1 is ~21 Hz at 44100 Hz / 2048. |

FFT bin to frequency: `freq = i * sample_rate / fft_size = i * 44100 / 2048 ≈ i * 21.5 Hz`

Useful ranges:

| Band | Bin range | `vj.fft(i)` loop |
|------|-----------|-----------------|
| Sub-bass (20–60 Hz) | 1–3 | `for i = 1, 3` |
| Bass (60–250 Hz) | 3–12 | `for i = 3, 12` |
| Midrange (250 Hz–2 kHz) | 12–93 | `for i = 12, 93` |
| Presence (2–6 kHz) | 93–279 | `for i = 93, 279` |
| Brilliance (6–20 kHz) | 279–930 | `for i = 279, 930` |

#### Clip counts

| Function | Returns | Description |
|----------|---------|-------------|
| `vj.num_audio()` | `int` | Number of loaded audio clips. |
| `vj.num_image()` | `int` | Number of loaded image clips. |
| `vj.num_video()` | `int` | Number of loaded video clips. |

#### Shaders

| Function | Description |
|----------|-------------|
| `vj.shader(i)` | Switch to shader `i`. `i` is the index printed at startup (alphabetical). |
| `vj.num_shaders()` | Number of loaded shaders. |
| `vj.uniform(i, v)` | Set shader parameter `u_p[i]` to float `v`. `i`: 0–14. Takes effect next frame. |

#### Image pixel access

CPU copies of image pixels are kept after GPU upload, enabling per-pixel reads
from Lua (useful for driving audio or visual parameters from image content).

| Function | Returns | Description |
|----------|---------|-------------|
| `vj.image_pixel(clip, x, y)` | `r, g, b, a` floats 0–1 | RGBA pixel at `(x, y)` in image clip `clip`. Returns `nil` if out of bounds. |
| `vj.image_width(clip)` | `int` | Width in pixels of image clip `clip`. |
| `vj.image_height(clip)` | `int` | Height in pixels of image clip `clip`. |

`vj.image_pixel` returns four values. Unpack them with:

```lua
local r, g, b, a = vj.image_pixel(0, 100, 50)

-- Scan a row for luminance and use it as an audio gain envelope
local w = vj.image_width(0)
local luma = 0
for x = 0, w - 1 do
    local r, g, b = vj.image_pixel(0, x, 100)
    luma = luma + r * 0.299 + g * 0.587 + b * 0.114
end
vj.gain(luma / w)
```

#### Utilities

| Function | Description |
|----------|-------------|
| `vj.print(s)` | Print string to stdout with a `[lua]` prefix. |

Lua's standard `print()` is also available and accepts multiple arguments
separated by tabs, which is handy for quick debugging:

```lua
print("bass:", vj.fft(4), "dt:", dt)
-- outputs: bass:   0.42   dt:   0.033

vj.print("bass: " .. vj.fft(4))
-- outputs: [lua] bass: 0.42
```

### Example patches

#### React to OSC and cycle clips

```lua
function on_osc(addr, arg)
    if addr == "/vj/audio" then
        -- mirror audio trigger to a matching video
        vj.video(arg % vj.num_video())
    end
end
```

#### Bass-reactive auto-cut

```lua
local cooldown = 0

function on_frame(dt)
    cooldown = cooldown - dt

    local bass = 0
    for i = 1, 8 do bass = bass + vj.fft(i) end
    bass = bass / 8

    if bass > 0.65 and cooldown <= 0 then
        vj.video(math.random(0, vj.num_video() - 1))
        cooldown = 2.0
    end
end
```

#### Use audio peak as gain envelope

```lua
function on_frame(dt)
    local peak = 0
    for i = 0, 63 do
        local s = math.abs(vj.sample(i))
        if s > peak then peak = s end
    end
    vj.gain(0.2 + peak * 0.8)
end
```

#### Drive shader parameters from audio

```lua
function on_frame(dt)
    -- Bass drives plasma speed (u_p[0])
    local bass = 0
    for i = 1, 8 do bass = bass + vj.fft(i) end
    vj.uniform(0, 0.5 + bass / 8 * 3.0)

    -- Treble drives image blend (u_p[1])
    local treble = 0
    for i = 200, 400 do treble = treble + vj.fft(i) end
    vj.uniform(1, treble / 200)
end
```

#### Scanline luminance as gain envelope

```lua
function on_frame(dt)
    local w = vj.image_width(0)
    if w == 0 then return end
    -- Sample middle row of image[0], use luminance as gain
    local y = math.floor(vj.image_height(0) / 2)
    local luma = 0
    for x = 0, w - 1 do
        local r, g, b = vj.image_pixel(0, x, y)
        luma = luma + r * 0.299 + g * 0.587 + b * 0.114
    end
    vj.gain(luma / w)
end
```

#### Cycle images on a beat (simple threshold detector)

```lua
local prev_bass = 0
local image_idx = 0

function on_frame(dt)
    local bass = vj.fft(3) + vj.fft(4) + vj.fft(5)
    if bass > 0.5 and prev_bass <= 0.5 then   -- rising edge
        image_idx = (image_idx + 1) % vj.num_image()
        vj.image(image_idx)
    end
    prev_bass = bass
end
```

### Live patching (future)

The function `script_eval(code)` is exposed internally and can be wired to
a TCP listener or special OSC address, allowing you to send Lua code strings
to the running VM. Functions redefined this way take effect immediately on
the next frame — no restart required.

---

## Dependencies

All C dependencies are header-only or small static libraries. `fetch_deps.sh`
clones or downloads them into `lib/`:

| Library | Use |
|---------|-----|
| [Sokol](https://github.com/floooh/sokol) | Window, OpenGL context, audio output |
| [KissFFT](https://github.com/mborgerding/kissfft) | Real FFT |
| [stb_image](https://github.com/nothings/stb) | PNG/JPG loading |
| [dr_wav](https://github.com/mackron/dr_libs) | WAV decoding |
| [tinyosc](https://github.com/mhroth/tinyosc) | OSC message parsing |

System libraries required:

| Library | Package (Ubuntu/Debian) | Notes |
|---------|------------------------|-------|
| OpenGL / GLES3 | `libgl-dev` / `libgles2-mesa-dev` | Automatic per platform |
| ALSA | `libasound2-dev` | Audio output |
| X11 | `libx11-dev libxi-dev libxcursor-dev` | Desktop only |
| libjpeg-turbo | `libturbojpeg0-dev` | JPEG video decode |
| LuaJIT | `libluajit-5.1-dev` | Scripting VM (Lua 5.4 used if absent) |

---

## Building

### Ubuntu / Debian (desktop x86-64)

```bash
sudo apt install build-essential cmake git \
    libgl-dev libx11-dev libxi-dev libxcursor-dev \
    libasound2-dev libturbojpeg0-dev libluajit-5.1-dev
```

```bash
./fetch_deps.sh
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

### Raspbian / Raspberry Pi OS (Pi 4, aarch64)

Make sure the Mesa V3D GPU driver is active:

```bash
glxinfo | grep "OpenGL renderer"   # should show "V3D 4.2" or similar
```

If you see `llvmpipe`, enable the driver via `raspi-config` → Advanced Options
→ GL Driver → Full KMS, then reboot.

```bash
sudo apt install build-essential cmake git \
    libgles2-mesa-dev libegl-mesa0 libegl1-mesa-dev \
    libasound2-dev libturbojpeg0-dev libluajit-5.1-dev
```

```bash
./fetch_deps.sh
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

CMake detects `aarch64` and selects the `SOKOL_GLES3` backend automatically.
The binary is stripped in Release mode.

### Cross-compiling for Pi from Ubuntu x86-64

```bash
sudo apt install gcc-aarch64-linux-gnu

cmake -B build-pi \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_SYSTEM_NAME=Linux \
  -DCMAKE_SYSTEM_PROCESSOR=aarch64 \
  -DCMAKE_C_COMPILER=aarch64-linux-gnu-gcc
cmake --build build-pi -j$(nproc)
```

You will need the Pi's sysroot for GL/ALSA headers. For most purposes it is
simpler to build natively on the Pi; a Pi 4 compiles the project in a few
seconds.

---

## Running

```bash
./build/fast-vj <media-dir> [osc-port] [-s patch.lua] [-S shaders/]
```

```bash
# Default OSC port 9000, shaders loaded from ./shaders/
./build/fast-vj media/

# Custom port and patch
./build/fast-vj media/ 7000 -s patches/example.lua

# Custom shaders directory
./build/fast-vj media/ -S /path/to/my/shaders/
```

At startup, fast-vj scans the media directory and the shaders directory and
prints the index of everything loaded:

```
Scanning media/ ...
  audio[0] drums  (4.2s)
  audio[1] bass   (2.1s)
  image[0] logo   (1920x1080)
  video[0] intro  120 frames  1920x1080  30.0fps
  video[1] loop   300 frames  1920x1080  30.0fps  (mmap)
Loading shaders from shaders/ ...
  shader[0] default
  shader[1] spectrum
  shader[2] oscilloscope
  shader[3] plasma
osc: listening on UDP port 9000
```

Clips labeled `(mmap)` are MJPEG AVI files mapped directly into the address
space. Clips without that label are JPEG directories loaded into RAM.

| Key | Action |
|-----|--------|
| Esc | Quit |

---

## Media directory layout

```
media/
  drums.wav           → audio[0]
  bass.wav            → audio[1]
  logo.png            → image[0]
  title.jpg           → image[1]
  intro.avi           → video[0]   (MJPEG AVI, mmap'd)
  loop/               → video[1]   (JPEG directory)
    000001.jpg
    000002.jpg
    …
```

Files are scanned in alphabetical order within each type. Video clips can be
either `.avi` files or subdirectories containing JPEGs — both are recognized
automatically.

---

## Project layout

```
fast-vj/
  src/
    main.c          — app loop, audio callback, OSC polling, render
    clips.c / .h    — media directory scanner, GPU upload, CPU pixel store
    osc.c / .h      — OSC UDP listener (background thread)
    video.c / .h    — JPEG directory and MJPEG AVI loader/decoder
    shaders.c / .h  — shader file loader, pipeline factory, uniform block
    script.c / .h   — LuaJIT VM, vj.* API, on_frame/on_osc dispatch
    sokol_impl.c    — single translation unit that defines SOKOL_IMPL
  shaders/          — GLSL fragment shader library
    default.glsl    — waveform + FFT bars + image
    spectrum.glsl   — colorful full-screen spectrum
    oscilloscope.glsl — glowing waveform over image
    plasma.glsl     — audio-reactive sine plasma
  patches/          — Lua patch scripts
    example.lua
  lib/              — populated by fetch_deps.sh (git-ignored)
    sokol/
    kissfft/
    tinyosc/
    stb_image.h
    dr_wav.h
    turbojpeg.h
  CMakeLists.txt
  fetch_deps.sh
```

---

## Roadmap

- **Live patch reload** — TCP or OSC listener that calls `script_eval()` to
  redefine `on_frame`/`on_osc` in the running VM without restarting
- **Shader hot-reload** — watch `.glsl` files, recompile on change with no
  restart; makes writing new shaders much faster
- **Multi-pass rendering** — render to offscreen texture, chain transforms;
  expose render target handles to Lua (`vj.render_to(target)`)
- **SVG paths** — parse with nanosvg, render as line strip geometry
- **MIDI input** — map controller values to `vj.uniform()` or Lua globals
