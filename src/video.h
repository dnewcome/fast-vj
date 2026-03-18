#pragma once
/*
 * video.h — video clip playback.
 *
 * Supports two source formats:
 *   1. JPEG directory  — a directory of sequential JPEG files (000001.jpg …)
 *   2. MJPEG AVI       — an AVI file with a MJPEG video stream
 *
 * Both formats keep all compressed JPEG data accessible in one memory
 * region; AVI files are mmap'd (zero-copy), JPEG directories are
 * malloc'd into a single contiguous buffer.
 *
 * libjpeg-turbo (NEON-accelerated on ARM) decodes one frame per render
 * loop into a shared RGBA buffer, which is then uploaded via
 * sg_update_image().
 *
 * Trigger latency: < 1 frame.
 * Memory: ~200-400 KB/frame compressed. 10s@30fps 2K ≈ 75-120 MB.
 *
 * Requires: libjpeg-turbo (apt install libturbojpeg0-dev)
 * Create JPEG-dir clips:
 *   ffmpeg -i input.mp4 -q:v 2 path/to/clip_name/%06d.jpg
 * Create MJPEG AVI clips:
 *   ffmpeg -i input.mp4 -c:v mjpeg -q:v 2 -an clip.avi
 */

#include <stddef.h>
#include <stdint.h>
#include "turbojpeg.h"

#define VIDEO_MAX_FRAMES 18000  /* 10 min @ 30fps */

typedef struct {
    char      path[512];        /* source path (dir or .avi file) */
    char      name[64];         /* basename */
    int       width, height;    /* from first frame */
    int       num_frames;
    float     fps;

    /* Backing store: mmap'd (AVI) or malloc'd (JPEG dir) */
    uint8_t  *source;           /* base pointer */
    size_t    source_size;      /* bytes — used by munmap or free */
    int       fd;               /* >= 0 → mmap'd file; -1 → malloc'd */

    /* Per-frame index into source */
    size_t   *offsets;          /* [num_frames] byte offset into source */
    size_t   *sizes;            /* [num_frames] JPEG byte length */

    /* Decode buffer: current frame as RGBA, width*height*4 bytes */
    uint8_t  *pixels;
} VideoClip;

/*
 * Load from a directory of sequential JPEG files.
 * Returns 1 on success, 0 on failure.
 */
int video_load(const char *dir, VideoClip *vc);

/*
 * Load from an MJPEG AVI file (mmap'd, zero-copy).
 * Returns 1 on success, 0 on failure.
 */
int video_load_avi(const char *file, VideoClip *vc);

/*
 * Decode frame index `idx` into vc->pixels.
 * `tj` is a reusable tjhandle (call tjInitDecompress() once, share it).
 * Returns 0 on success.
 */
int video_decode_frame(VideoClip *vc, int idx, tjhandle tj);

/* Free all memory. Does not destroy any GPU resources. */
void video_unload(VideoClip *vc);
