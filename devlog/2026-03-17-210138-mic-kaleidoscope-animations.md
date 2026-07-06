# Mic input, kaleidoscope shader, and OSC parameter animation

_2026-03-17_

## What happened

Three additions in the late session:

**Mic input** (`-m [device]` flag): ALSA capture thread feeds live microphone audio into the same waveform and FFT ring buffer as clip playback. This means every audio-reactive patch works with either a playing clip or a live mic — no code change needed. The `-m` flag accepts an optional ALSA device name; defaults to `default`.

**Kaleidoscope shader**: folds the active image into N radial mirror segments. `u_p[0]` is segment count (2–16), `u_p[1]` spin speed in radians/sec, `u_p[2]` zoom, `u_p[3]` audio-reactive zoom pulse from bass. Works with any image or video clip; falls back to color-cycling if no clip is loaded. The kaleidoscope spin commit followed shortly after, adding the rotation accumulator.

**OSC parameter animations** (`/vj/animate ifff`): takes a parameter index, from-value, to-value, and duration in seconds. Linear interpolation runs in the render loop; a new animate message cancels any current one on that parameter. `/vj/anim_clear i -1` stops all of them. This makes it possible to do smooth sweeps — ramp kaleidoscope segment count from 2 to 12 over 3 seconds — entirely over OSC without touching Lua.

A full `osc_control.lua` patch was added as a reference: it wires `/vj/pN` messages to `vj.uniform(N, v)` so any shader parameter is reachable by OSC without writing patch-specific code.

---

_commits: 43bfcd1, 1f264ab, 83052f0, 689f92e, 1b4f059_
