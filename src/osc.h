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
 *   <anything else>    — forwarded to Lua on_osc(addr, val)
 */

#include <stdatomic.h>
#include <pthread.h>

/* Generic OSC message queue — unrecognised addresses land here so the
   render thread can forward them to Lua without involving more atomics. */
#define OSC_QUEUE_SIZE 32

typedef struct {
    char  addr[64];
    char  type;    /* 'f' or 'i' */
    float fval;
    int   ival;
} OscMsg;

/* Animate queue — /vj/animate ifff <param> <from> <to> <duration> */
typedef struct {
    int   param;
    float from, to, duration;
} OscAnimate;

typedef struct {
    _Atomic int   pending_audio;  /* -1 = no pending, >=0 = clip index */
    _Atomic int   pending_image;  /* -1 = no pending, >=0 = clip index */
    _Atomic int   pending_video;  /* -1 = no pending, >=0 = clip index */
    _Atomic int   pending_shader; /* -1 = no pending, >=0 = shader index */
    _Atomic int   stop_audio;     /* non-zero = stop requested */
    /* gain stored as IEEE 754 bits so we can use _Atomic int */
    _Atomic int   gain_bits;      /* reinterpret_cast<int>(float gain) */

    /* Generic message queue for Lua forwarding */
    OscMsg        queue[OSC_QUEUE_SIZE];
    int           q_head;
    int           q_tail;
    pthread_mutex_t q_mutex;

    /* Animate queue */
    OscAnimate    anim_queue[OSC_QUEUE_SIZE];
    int           anim_head;
    int           anim_tail;
    pthread_mutex_t anim_mutex;
} OscState;

extern OscState g_osc;

/* Start OSC listener thread on given UDP port. */
void osc_init(int port);

/* Signal listener thread to stop and join it. */
void osc_shutdown(void);

/* Read gain as float from atomic bits. */
static inline float osc_gain(void) {
    int bits = atomic_load_explicit(&g_osc.gain_bits, memory_order_relaxed);
    float f;
    __builtin_memcpy(&f, &bits, sizeof(f));
    return f;
}

/* Write gain as float into atomic bits. */
void osc_set_gain(float g);
