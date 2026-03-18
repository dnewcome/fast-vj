# fast-vj design questions and TODOs

## Architecture: simplify threading model

**Question:** Can we eliminate the OSC queue and all locking primitives if we
move all input handling into the Lua interpreter?

Currently the OSC listener runs on a background thread and uses atomics +
a mutex-protected queue to pass messages to the render thread. This works but
adds complexity. An alternative:

- Run a non-blocking UDP `recvfrom` directly inside `poll_osc()` on the render
  thread, parse the message, and dispatch immediately to Lua — no queue, no
  mutex, no atomics for the generic path.
- The only cost is that OSC polling happens once per frame (~16ms at 60fps),
  which is fine for a VJ tool where human reaction time is the bottleneck.
- Built-in atomic state (audio/image/video/gain) could also collapse into Lua
  calls, leaving a single unified dispatch path.

**Question:** Is there any reason to keep the listener on a separate thread at
all? The main risk would be a slow Lua callback blocking the render loop, but
that's already a risk with the current design since Lua runs on the render
thread anyway.

---

## Interactive Lua shell / live coding

**TODO:** Expose an interactive Lua REPL so the environment can be modified
at runtime without restarting.

Options:
- Read from stdin on a background thread, eval each line with `script_eval()`
  on the next frame tick.
- Listen on a TCP port for code strings (e.g. from an editor plugin or
  netcat). Functions redefined this way take effect on the next `on_frame`
  call — no restart required.
- A dedicated OSC address `/vj/eval s <code>` to send Lua snippets over UDP.

Live coding would allow redefining `on_frame` and `on_osc` while the
visuals are running — the core use case for a performance tool.

---

## Default handlers and the role of Lua

**Question:** Should built-in OSC commands (`/vj/audio`, `/vj/image`, etc.)
remain hardcoded in C, or should everything be driven by Lua?

Two directions:

1. **Keep C defaults, augment with Lua.** Current approach. Clip triggering
   works out of the box without a script. Lua adds reactivity on top.

2. **Lua-only interactivity.** Ship no default handlers. Without `-s`, the
   app renders the default shader and that's it — no OSC response. All
   behaviour is defined in a patch. This would:
   - Simplify `osc.c` to a pure message pump with no dispatch logic.
   - Make patches fully self-describing — the patch *is* the show.
   - Require users to write (or copy) a patch to get basic clip triggering,
     which raises the floor but also raises the ceiling.

**Question:** Could the built-in defaults themselves be a Lua file loaded
automatically from e.g. `patches/default.lua` if present, with a hardcoded
fallback inline string if not?

---

## Crossfading and compositing

**TODO:** Enable crossfading between clips or shader outputs, ideally without
adding C-level blend state.

Options:
- **Shader-level blend:** Pass a `u_blend` uniform and a second texture slot.
  The shader lerps between them. The Lua patch drives the blend value over
  time. No new C infrastructure needed beyond an extra texture binding.
- **Offscreen render pass:** Render the outgoing scene to an FBO, then
  composite with the incoming scene in a fullscreen pass. More flexible but
  adds a render pass and framebuffer management.
- **Lua-driven envelope:** `on_frame` increments a blend value each frame
  over N seconds. Works today with `vj.uniform()` if the shader supports it.

### Video crossfade: medium difficulty

The current pipeline has one `image_tex` slot. To crossfade two clips:

1. Add a second texture slot (`image_tex2`) to the shader header and pipeline
   descriptor in `shaders.c`.
2. Add a second `views[]` binding in the app bindings struct.
3. Expose `vj.image2(i)` / `vj.video2(i)` in Lua to set the outgoing clip.
4. Use an existing `u_p` slot as the blend amount.

The Lua patch drives the fade:

```lua
local blend = 0
local fade_time = 1.0

function on_frame(dt)
    blend = math.min(blend + dt / fade_time, 1.0)
    vj.uniform(14, blend)  -- shader lerps between image_tex and image_tex2
end
```

The shader side is a one-liner: `mix(texture(image_tex, uv), texture(image_tex2, uv), u_p[14])`.
The C work is roughly an afternoon.

### Shader crossfade: significantly harder

You can't blend two different pipelines in one pass. You'd need:

1. Two offscreen framebuffers (sokol `sg_pass` with `sg_attachments`).
2. Render shader A → FBO A, shader B → FBO B each frame during the transition.
3. A third composite pass that blends the two FBO textures on screen.

Sokol supports this but it triples the render passes during any active
transition and adds meaningful complexity to the frame loop. On Pi, rendering
the full quad three times per frame has a real performance cost.

**Practical shortcut:** don't blend pipelines at all. On a shader switch,
animate `u_p` values in `on_frame` to smooth the visual transition within the
incoming shader rather than truly compositing two pipelines. For most VJ
purposes this is indistinguishable from a real crossfade.

---

## Advanced effects

**TODO:** Chroma keying (green screen / colour replacement).

- Straightforward as a GLSL shader: sample `image_tex`, test distance from
  key colour in YCbCr or HSV space, output alpha 0 for matched pixels.
- Could be a built-in shader `chroma.glsl` with `u_p[0..2]` as key RGB and
  `u_p[3]` as threshold.
- Background clip would need a second texture slot or a compositing pass.

**TODO:** Other effect ideas to explore:
- Feedback / echo: render to texture, blend previous frame back in.
- Kaleidoscope / mirror UV transforms.
- Pixel sorting (compute shader or multi-pass).
- Beat-synced strobe / flash via `u_time` modulo.
- LUT colour grading via a 1D or 3D texture.
- Slit-scan time displacement using a ring of past frames.

---

## Shader uniform warnings: sokol GL_UNIFORMBLOCK_NAME_NOT_FOUND

Sokol currently emits warnings like `GL_UNIFORMBLOCK_NAME_NOT_FOUND_IN_SHADER`
and `GL_IMAGE_SAMPLER_NAME_NOT_FOUND_IN_SHADER` at startup for every shader.
These are suppressed by a log filter in `main.c` but not actually fixed.

### Root cause

The standard header injected before every user shader declares `u_time` and
`u_p[15]` as individual `uniform` variables. The GL driver's GLSL compiler
removes any uniform that the shader body never references. When sokol then
tries to bind `u_time` by name at runtime, it no longer exists in the compiled
program — hence the warning. The shader still renders correctly; the bind is
simply a no-op.

### Why we can't just "put them in"

By the time sokol tries to find the uniform, the GL driver has already compiled
and linked the shader. The uniform doesn't exist in the program object. There
is no way to add it after the fact.

### Options

1. **Dummy reference in each shader** — add `float _t = u_time * 0.0;` in
   shaders that don't use time. Keeps the uniform alive through compilation.
   Works but is boilerplate noise in every shader file.

2. **Named std140 UBO (proper fix)** — restructure the injected header to use
   a named uniform block:
   ```glsl
   layout(std140) uniform fs_params {
       float u_time;
       float u_p[15];
   };
   ```
   Sokol looks up the block by name rather than individual members. As long as
   any member is used, the block survives the optimizer. Change sokol descriptor
   to `SG_UNIFORMLAYOUT_STD140`.

   **Catch:** std140 pads every `float` array element to 16 bytes (`vec4`),
   making `u_p[15]` consume 240 bytes instead of 60. The struct passed from C
   must match exactly. This requires refactoring `ShaderParams` in `shaders.h`,
   the sokol uniform block descriptor in `shaders.c`, and the GLSL header — plus
   updating every shader that accesses `u_p`. Roughly an hour of work.

3. **Suppress the warning (current approach)** — filter out log IDs 10 and 11
   in the `vj_log` callback in `main.c`. Harmless since the warnings are
   genuinely benign — uniforms that aren't used don't need to be bound.

**TODO:** Do the std140 refactor when time allows. It's the correct fix and
also makes the uniform layout explicit and portable across GL/GLES.
