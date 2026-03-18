#pragma once
/*
 * script.h — LuaJIT scripting layer.
 *
 * The VM runs entirely on the render thread. Lua callbacks are called
 * once per frame (on_frame) and once per OSC event (on_osc).
 *
 * Lua API exposed as the global `vj` table:
 *
 *   vj.audio(i)       trigger audio clip i
 *   vj.image(i)       show image clip i  (-1 = blank)
 *   vj.video(i)       play video clip i  (-1 = stop)
 *   vj.gain(f)        set master audio gain
 *   vj.stop()         stop audio
 *   vj.sample(i)      get audio ring-buffer sample [0 .. tex_width-1]
 *   vj.fft(i)         get FFT magnitude [0 .. tex_width-1]
 *   vj.num_audio()    number of loaded audio clips
 *   vj.num_image()    number of loaded image clips
 *   vj.num_video()    number of loaded video clips
 *   vj.print(s)       print to stdout with [lua] prefix
 *
 * Callbacks the script may define:
 *
 *   function on_frame(dt)          called every render frame; dt = seconds
 *   function on_osc(addr, arg)     called for each OSC event; arg is int
 *                                  or float (nil if no arg)
 */

/* Initialize the LuaJIT VM and register the vj.* table.
 * audio_frame and fft_mag must point to arrays of tex_width floats that
 * remain valid for the lifetime of the script. Call after clips_scan(). */
void script_init(float *audio_frame, float *fft_mag, int tex_width);

/* Load and execute a Lua script file. Returns 1 on success. */
int  script_load(const char *path);

/* Evaluate a Lua string in the running VM (for live patching).
 * Returns 1 on success. */
int  script_eval(const char *code);

/* Call on_osc(addr, arg) if defined in the script.
 * arg_type: 'i' integer, 'f' float, 0 for no arg. */
void script_call_osc(const char *addr, int arg_type, int ival, float fval);

/* Call on_frame(dt) if defined in the script. */
void script_call_frame(double dt);

/* Shut down and free the VM. */
void script_shutdown(void);
