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

## Hot reloading: shaders, media, and Lua

The goal is to be able to change any part of the running system — shaders,
media clips, Lua patch — without restarting the process or dropping frames.

### Shader hot reload

Feasible now with minimal work. All sokol GPU calls must happen on the render
thread, so reloading has to happen inside `frame()`.

**Option 1 — OSC-triggered:** `/vj/reload i <index>` re-reads the `.glsl`
file, recompiles shader + pipeline. On success, swap in the new ones and
destroy the old. On failure, keep the old shader running and print the error.
The user saves the file then sends the OSC message.

**Option 2 — Automatic mtime polling (preferred):** Store `time_t mtime` in
the `Shader` struct at load time. Once per second on the render thread, call
`stat()` on each file. If mtime changed, reload. Edit and save in your editor,
the shader hot-swaps within one second — no OSC needed. Implementation:
- Add `time_t mtime` to `Shader` struct in `shaders.h`
- Add `shaders_reload_changed()` in `shaders.c` — stats files, recompiles if changed
- Call from `frame()` every ~60 frames
- On compile failure: print error, keep old pipeline running

Both options can coexist. Mtime polling is the day-to-day workflow; OSC
trigger is useful for forcing a reload when mtime polling isn't appropriate
(e.g. NFS mounts, cross-compiled files).

### Media hot add

Adding clips to a running instance without restarting. The challenge is that
`clips_upload_gpu()` must run on the render thread (GPU upload), while file
scanning can happen on a background thread.

Possible approach:
- Watch the media directory with `inotify` (Linux) on a background thread
- When a new file appears, load it into CPU memory on the background thread
- Set a flag / enqueue a pointer; the render thread picks it up next frame
  and calls `sg_make_image()` to upload to GPU
- New clip becomes available under the next index immediately

**Question:** Should hot-added clips append to the existing index list, or
trigger a full rescan that may renumber existing clips? Renumbering would
break any OSC automation or Lua state that references clips by index.
Appending is safer for live use.

**Question:** Hot-removing clips (file deleted) is harder — a clip may be
actively displayed. Safest behaviour is probably to keep the GPU texture alive
until explicitly cleared, and log a warning.

### Lua REPL and live code evaluation

The `script_eval(code)` function already exists internally — it evaluates a
Lua string in the running VM. Wiring it to an input channel enables live
coding without restart.

**Option 1 — stdin REPL:** Read lines from stdin on a background thread, queue
them, evaluate on the render thread next frame. Works immediately with
`netcat` or a terminal. Limitation: OSC is already the primary control
channel, mixing stdin complicates daemonisation.

**Option 2 — TCP code server:** Listen on a TCP port (e.g. 9001) for
newline-delimited or length-prefixed Lua chunks. An editor plugin (Neovim,
Emacs, VS Code) sends the current buffer or selection on save. Functions
redefined this way take effect on the next `on_frame` — no restart, no
dropped frames. This is the standard approach in live coding environments
(Sonic Pi, Tidal, Impromptu).

**Option 3 — OSC `/vj/eval s <code>`:** Send Lua as an OSC string argument.
OSC strings are null-padded to 4-byte alignment, limiting practical chunk size
to a few hundred bytes. Fine for one-liners, impractical for full patches.

**Recommended path:** TCP server on a separate port. Implement a minimal
length-prefixed protocol (4-byte length + UTF-8 Lua). The background thread
reads chunks, queues them, the render thread evals each chunk once per frame.
A simple shell helper:
```bash
# Send current file to running instance
send_patch() { lua_file=$1; wc -c < "$lua_file" | xargs printf '%08x' | xxd -r -p | cat - "$lua_file" | nc localhost 9001; }
```

All three options can coexist — they all call the same `script_eval()`.

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
