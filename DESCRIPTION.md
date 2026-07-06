# fast-vj

A fast, minimal live-visuals (VJ) engine built for real-time performance.

fast-vj plays back video clips, images, and audio-reactive shaders in a tight,
vsync'd render loop, driven live over OSC. It's written in C on top of Sokol +
OpenGL, decodes video with libjpeg-turbo (MJPEG/JPEG sequences), and reads live
audio from ALSA into GPU textures for waveform and spectrum effects.

Highlights:

- **OSC control** — every parameter (`/vj/audio`, `/vj/image`, `/vj/video`, `/vj/gain`, …) is driveable live from any OSC controller.
- **Hot-swappable GLSL shaders** — drop in a `.glsl` effect and reload it without restarting.
- **Lua scripting** — `on_frame` / `on_osc` hooks and a `vj.*` API for scripted shows.
- **Runs anywhere** — native on Raspberry Pi (NEON-accelerated decode, fullscreen KMS) and in the browser via an Emscripten/WASM port.

The build log below tracks its evolution from the first render loop to the WASM port.
