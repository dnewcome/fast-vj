/*
 * mic.c — ALSA microphone capture.
 *
 * On WASM: ALSA is not available in the browser. Stub functions are
 * provided so the build succeeds; mic mode is a no-op on WASM.
 */

#include "mic.h"

#ifdef __EMSCRIPTEN__

#include <string.h>

int mic_init(const char *device, int sample_rate) {
    (void)device; (void)sample_rate;
    return 0;
}
int  mic_available(void) { return 0; }
void mic_read(float *dst, int n) { memset(dst, 0, (size_t)n * sizeof(float)); }
void mic_shutdown(void) {}

#else /* !__EMSCRIPTEN__ — full ALSA implementation below */

#include <alsa/asoundlib.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>

#define MIC_RING_SIZE (44100 * 4)   /* 4 seconds of float32 mono */
#define MIC_PERIOD     512          /* frames per ALSA read */

static float      s_ring[MIC_RING_SIZE];
static atomic_int s_write_pos;
static atomic_int s_read_pos;
static snd_pcm_t *s_pcm    = NULL;
static pthread_t  s_thread;
static atomic_int s_running;

/* ------------------------------------------------------------------ */
/* Internal ring buffer                                               */
/* ------------------------------------------------------------------ */

static void ring_push(const float *src, int n) {
    int wp = atomic_load_explicit(&s_write_pos, memory_order_relaxed);
    for (int i = 0; i < n; i++)
        s_ring[wp++ % MIC_RING_SIZE] = src[i];
    atomic_store_explicit(&s_write_pos, wp, memory_order_release);
}

/* ------------------------------------------------------------------ */
/* Capture thread                                                     */
/* ------------------------------------------------------------------ */

static void *capture_thread(void *arg) {
    (void)arg;
    float *buf = malloc(MIC_PERIOD * sizeof(float));
    if (!buf) return NULL;

    while (atomic_load_explicit(&s_running, memory_order_acquire)) {
        int n = (int)snd_pcm_readi(s_pcm, buf, MIC_PERIOD);
        if (n == -EPIPE) {                      /* xrun: buffer overrun */
            snd_pcm_prepare(s_pcm);
            continue;
        }
        if (n < 0) {
            int err = snd_pcm_recover(s_pcm, n, 1 /* silent */);
            if (err < 0) {
                fprintf(stderr, "mic: unrecoverable error: %s\n", snd_strerror(err));
                break;
            }
            continue;
        }
        ring_push(buf, n);
    }
    free(buf);
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Public API                                                         */
/* ------------------------------------------------------------------ */

int mic_init(const char *device, int sample_rate) {
    const char *dev = device ? device : "default";

    int err = snd_pcm_open(&s_pcm, dev, SND_PCM_STREAM_CAPTURE, 0);
    if (err < 0) {
        fprintf(stderr, "mic: open '%s': %s\n", dev, snd_strerror(err));
        return 0;
    }

    err = snd_pcm_set_params(s_pcm,
        SND_PCM_FORMAT_FLOAT_LE,
        SND_PCM_ACCESS_RW_INTERLEAVED,
        1,              /* mono */
        (unsigned)sample_rate,
        1,              /* allow software resampling */
        50000);         /* target latency: 50ms */
    if (err < 0) {
        fprintf(stderr, "mic: set_params: %s\n", snd_strerror(err));
        snd_pcm_close(s_pcm);
        s_pcm = NULL;
        return 0;
    }

    atomic_store(&s_write_pos, 0);
    atomic_store(&s_read_pos,  0);
    atomic_store(&s_running,   1);
    pthread_create(&s_thread, NULL, capture_thread, NULL);

    printf("mic: capturing from '%s' at %d Hz mono\n", dev, sample_rate);
    return 1;
}

int mic_available(void) {
    return atomic_load_explicit(&s_write_pos, memory_order_acquire)
         - atomic_load_explicit(&s_read_pos,  memory_order_relaxed);
}

void mic_read(float *dst, int n) {
    int rp    = atomic_load_explicit(&s_read_pos,  memory_order_relaxed);
    int avail = atomic_load_explicit(&s_write_pos, memory_order_acquire) - rp;

    int to_copy = avail < n ? avail : n;
    for (int i = 0; i < to_copy; i++)
        dst[i] = s_ring[(rp + i) % MIC_RING_SIZE];
    for (int i = to_copy; i < n; i++)
        dst[i] = 0.0f;

    atomic_store_explicit(&s_read_pos, rp + to_copy, memory_order_relaxed);
}

void mic_shutdown(void) {
    atomic_store_explicit(&s_running, 0, memory_order_release);
    pthread_join(s_thread, NULL);
    if (s_pcm) {
        snd_pcm_close(s_pcm);
        s_pcm = NULL;
    }
}

#endif /* __EMSCRIPTEN__ */
