#pragma once
/*
 * osc.h — OSC UDP listener.
 *
 * Runs a background POSIX thread that receives UDP packets on a given
 * port, parses them with tinyosc, and writes commands into shared
 * atomic state that the main/render thread reads each frame.
 *
 * OSC address space:
 *
 *   /vj/audio  <int>    — trigger audio clip by index
 *   /vj/image  <int>    — switch to image clip by index (-1 = blank)
 *   /vj/gain   <float>  — master audio gain (default 1.0)
 *   /vj/stop           — stop audio playback
 */

#include <stdatomic.h>

typedef struct {
    _Atomic int   pending_audio;  /* -1 = no pending, >=0 = clip index */
    _Atomic int   pending_image;  /* -1 = no pending, >=0 = clip index */
    _Atomic int   pending_video;  /* -1 = no pending, >=0 = clip index */
    _Atomic int   stop_audio;     /* non-zero = stop requested */
    /* gain stored as IEEE 754 bits so we can use _Atomic int */
    _Atomic int   gain_bits;      /* reinterpret_cast<int>(float gain) */
} OscState;

extern OscState g_osc;

/* Start OSC listener thread on given UDP port. */
void osc_init(int port);

/* Signal listener thread to stop and join it. */
void osc_shutdown(void);

/* Convenience: read gain as float from atomic bits. */
static inline float osc_gain(void) {
    int bits = atomic_load_explicit(&g_osc.gain_bits, memory_order_relaxed);
    float f;
    __builtin_memcpy(&f, &bits, sizeof(f));
    return f;
}
