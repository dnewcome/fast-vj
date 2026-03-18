#pragma once
/*
 * clips.h — media directory scanner and clip store.
 *
 * Layout expected in the media directory:
 *
 *   media/
 *     00_kick.wav           → audio clip [0]
 *     01_bass.wav           → audio clip [1]
 *     00_bg.png             → image clip [0]
 *     00_loop/              → video clip [0]  (dir of sequential JPEGs)
 *       000001.jpg
 *       000002.jpg
 *       ...
 *
 * Files/directories are indexed in alphabetical order within each type.
 *
 * Scans a directory for .wav (audio) and .png/.jpg (image) files.
 * Audio is decoded to mono float32 in CPU RAM.
 * Images are decoded to RGBA8 pixels in CPU RAM, then uploaded to
 * GPU textures via clips_upload_gpu() (must be called after sg_setup()).
 *
 * Clips are sorted alphabetically so indices are stable across runs.
 * Index 0 = first file alphabetically, etc.
 */

#include <stdint.h>
#include "sokol/sokol_gfx.h"
#include "video.h"

#define CLIPS_MAX 256

typedef struct {
    char        path[512];
    char        name[64];    /* basename without extension */
    float      *samples;     /* mono float32 PCM */
    uint64_t    num_frames;
} AudioClip;

typedef struct {
    char        path[512];
    char        name[64];
    int         width, height;
    sg_image    gpu_img;
    sg_view     gpu_view;
    uint8_t    *pixels;  /* RGBA8 CPU copy kept after GPU upload (w*h*4 bytes) */
} ImageClip;

typedef struct {
    AudioClip   audio[CLIPS_MAX];
    int         num_audio;
    ImageClip   image[CLIPS_MAX];
    int         num_image;
    VideoClip   video[CLIPS_MAX];
    int         num_video;
} ClipList;

/* Scan dir for supported files. Loads audio to CPU RAM. Decodes image
 * pixels but does NOT upload to GPU — call clips_upload_gpu() after
 * sg_setup() for that. */
void clips_scan(const char *dir, ClipList *cl);

/* Upload decoded image pixels to GPU. Must be called from the GL thread
 * after sg_setup(). Creates sg_image + sg_view for each image clip. */
void clips_upload_gpu(ClipList *cl, sg_sampler smp);

/* Print loaded clips to stdout. */
void clips_print(const ClipList *cl);

/* Free CPU-side memory (does NOT destroy GPU resources). */
void clips_free(ClipList *cl);

/* Returns 1 if the named directory contains at least one JPEG. */
int dir_has_jpegs(const char *dir);

/* Global clip list — defined in main.c, accessible to script.c. */
extern ClipList g_clips;
