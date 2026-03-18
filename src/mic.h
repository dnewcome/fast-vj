#pragma once
/*
 * mic.h — ALSA microphone capture.
 *
 * Runs a background thread that continuously captures mono float32
 * samples from an ALSA device into an internal ring buffer. The render
 * thread drains it via mic_read() each frame to drive the audio/FFT
 * textures, exactly as it would with WAV playback data.
 *
 * WAV clip playback is unaffected — mic mode only changes what drives
 * the GPU visualisation textures.
 */

/* Start capture. device = NULL uses "default". Returns 1 on success. */
int  mic_init(const char *device, int sample_rate);

/* Samples available in the capture ring buffer. */
int  mic_available(void);

/* Read n samples into dst. Pads with zeros if not enough data yet. */
void mic_read(float *dst, int n);

/* Stop capture thread and close the ALSA handle. */
void mic_shutdown(void);
