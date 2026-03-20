/*
 * clips.c — media directory scanner.
 */

#include "clips.h"

#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_RESIZE_IMPLEMENTATION
#include "stb_image.h"
#include "stb_image_resize2.h"

#define DR_WAV_IMPLEMENTATION
#include "dr_wav.h"

#include <dirent.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <sys/stat.h>

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

static const char *file_ext(const char *name) {
    const char *dot = strrchr(name, '.');
    return dot ? dot : "";
}

static void basename_noext(const char *path, char *out, int outlen) {
    const char *slash = strrchr(path, '/');
    const char *base  = slash ? slash + 1 : path;
    const char *dot   = strrchr(base, '.');
    int len = dot ? (int)(dot - base) : (int)strlen(base);
    if (len >= outlen) len = outlen - 1;
    memcpy(out, base, len);
    out[len] = '\0';
}

static int is_audio_ext(const char *ext) {
    return strcasecmp(ext, ".wav") == 0;
}

static int is_image_ext(const char *ext) {
    return strcasecmp(ext, ".png") == 0
        || strcasecmp(ext, ".jpg") == 0
        || strcasecmp(ext, ".jpeg") == 0;
}

static int is_video_ext(const char *ext) {
    return strcasecmp(ext, ".avi") == 0;
}

int dir_has_jpegs(const char *dir) {
    struct dirent **e;
    int n = scandir(dir, &e, NULL, NULL);
    if (n < 0) return 0;
    int found = 0;
    for (int i = 0; i < n; i++) {
        const char *ext = strrchr(e[i]->d_name, '.');
        if (ext && (strcasecmp(ext, ".jpg") == 0 || strcasecmp(ext, ".jpeg") == 0))
            found = 1;
        free(e[i]);
    }
    free(e);
    return found;
}

/* ------------------------------------------------------------------ */
/* Audio loading                                                       */
/* ------------------------------------------------------------------ */

static int load_wav(const char *path, AudioClip *c) {
    drwav wav;
    if (!drwav_init_file(&wav, path, NULL)) {
        fprintf(stderr, "clips: failed to open %s\n", path);
        return 0;
    }
    drwav_uint64 total = wav.totalPCMFrameCount;
    int ch = wav.channels;

    float *raw = malloc(total * ch * sizeof(float));
    if (!raw) { drwav_uninit(&wav); return 0; }

    drwav_uint64 decoded = drwav_read_pcm_frames_f32(&wav, total, raw);
    drwav_uninit(&wav);

    c->samples    = malloc(decoded * sizeof(float));
    c->num_frames = decoded;
    for (drwav_uint64 i = 0; i < decoded; i++) {
        float s = 0;
        for (int j = 0; j < ch; j++) s += raw[i * ch + j];
        c->samples[i] = s / ch;
    }
    free(raw);
    return 1;
}

/* ------------------------------------------------------------------ */
/* Image loading (CPU side only — GPU upload done separately)         */
/* ------------------------------------------------------------------ */

/* We store pixels temporarily in a side-channel. Use a static table
 * parallel to ImageClip to avoid making the public header depend on
 * implementation details. Freed after GPU upload. */
static uint8_t *s_pixels[CLIPS_MAX];

#define MAX_TEX_SIZE 4096

static int load_image_cpu(const char *path, ImageClip *c, int idx) {
    int w, h, ch;
    uint8_t *px = stbi_load(path, &w, &h, &ch, 4); /* force RGBA */
    if (!px) {
        fprintf(stderr, "clips: failed to load %s\n", path);
        return 0;
    }

    /* Downscale if either dimension exceeds GPU texture size limit */
    if (w > MAX_TEX_SIZE || h > MAX_TEX_SIZE) {
        int nw = w, nh = h;
        if (nw > MAX_TEX_SIZE) { nh = nh * MAX_TEX_SIZE / nw; nw = MAX_TEX_SIZE; }
        if (nh > MAX_TEX_SIZE) { nw = nw * MAX_TEX_SIZE / nh; nh = MAX_TEX_SIZE; }
        printf("  clips: resizing %s from %dx%d to %dx%d (GPU limit %d)\n",
               path, w, h, nw, nh, MAX_TEX_SIZE);
        uint8_t *scaled = malloc((size_t)(nw * nh * 4));
        if (!scaled) { stbi_image_free(px); return 0; }
        stbir_resize_uint8_linear(px, w, h, 0, scaled, nw, nh, 0, STBIR_RGBA);
        stbi_image_free(px);
        px = scaled;
        w  = nw;
        h  = nh;
    }

    c->width  = w;
    c->height = h;
    s_pixels[idx] = px;
    return 1;
}

/* ------------------------------------------------------------------ */
/* Directory scan                                                      */
/* ------------------------------------------------------------------ */

void clips_scan(const char *dir, ClipList *cl) {
    memset(cl, 0, sizeof(*cl));

    struct dirent **entries;
    int n = scandir(dir, &entries, NULL, alphasort);
    if (n < 0) {
        perror("clips: scandir");
        return;
    }

    for (int i = 0; i < n; i++) {
        const char *name = entries[i]->d_name;
        const char *ext  = file_ext(name);

        char path[512];
        snprintf(path, sizeof(path), "%s/%s", dir, name);

        if (is_audio_ext(ext) && cl->num_audio < CLIPS_MAX) {
            AudioClip *c = &cl->audio[cl->num_audio];
            strncpy(c->path, path, sizeof(c->path) - 1);
            basename_noext(name, c->name, sizeof(c->name));
            if (load_wav(path, c)) {
                printf("  audio[%d] %s  (%.1fs)\n",
                       cl->num_audio, c->name,
                       (float)c->num_frames / 44100.0f);
                cl->num_audio++;
            }
        } else if (is_image_ext(ext) && cl->num_image < CLIPS_MAX) {
            ImageClip *c = &cl->image[cl->num_image];
            strncpy(c->path, path, sizeof(c->path) - 1);
            basename_noext(name, c->name, sizeof(c->name));
            if (load_image_cpu(path, c, cl->num_image)) {
                printf("  image[%d] %s  (%dx%d)\n",
                       cl->num_image, c->name, c->width, c->height);
                cl->num_image++;
            }
        } else if (is_video_ext(ext) && cl->num_video < CLIPS_MAX) {
            VideoClip *c = &cl->video[cl->num_video];
            if (video_load_avi(path, c)) {
                printf("  video[%d] %s  %d frames  %dx%d  %.1ffps  %.1fMB (mmap)\n",
                       cl->num_video, c->name, c->num_frames,
                       c->width, c->height, c->fps,
                       (float)c->source_size / (1024.0f * 1024.0f));
                cl->num_video++;
            }
        } else {
            /* Check if it's a directory containing JPEGs → video clip */
            struct stat st;
            if (stat(path, &st) == 0 && S_ISDIR(st.st_mode)
                    && name[0] != '.'
                    && dir_has_jpegs(path)
                    && cl->num_video < CLIPS_MAX) {
                VideoClip *c = &cl->video[cl->num_video];
                if (video_load(path, c)) {
                    printf("  video[%d] %s  %d frames  %dx%d\n",
                           cl->num_video, c->name,
                           c->num_frames, c->width, c->height);
                    cl->num_video++;
                }
            }
        }

        free(entries[i]);
    }
    free(entries);
}

/* ------------------------------------------------------------------ */
/* GPU upload (must be called from GL thread after sg_setup)          */
/* ------------------------------------------------------------------ */

void clips_upload_gpu(ClipList *cl, sg_sampler smp) {
    for (int i = 0; i < cl->num_image; i++) {
        ImageClip *c = &cl->image[i];
        uint8_t   *px = s_pixels[i];
        if (!px) continue;

        /* Build full mip chain with stb_image_resize2 */
        sg_image_desc desc = {
            .width        = c->width,
            .height       = c->height,
            .pixel_format = SG_PIXELFORMAT_RGBA8,
        };
        desc.data.mip_levels[0].ptr  = px;
        desc.data.mip_levels[0].size = (size_t)(c->width * c->height * 4);

        uint8_t *mip_bufs[SG_MAX_MIPMAPS] = {0};
        int mw = c->width, mh = c->height, num_mips = 1;
        while ((mw > 1 || mh > 1) && num_mips < SG_MAX_MIPMAPS) {
            int sw = mw > 1 ? mw / 2 : 1;
            int sh = mh > 1 ? mh / 2 : 1;
            uint8_t *buf = malloc((size_t)(sw * sh * 4));
            if (!buf) break;
            stbir_resize_uint8_srgb(
                (num_mips == 1 ? px : mip_bufs[num_mips - 1]),
                mw, mh, 0, buf, sw, sh, 0, STBIR_RGBA);
            mip_bufs[num_mips] = buf;
            desc.data.mip_levels[num_mips].ptr  = buf;
            desc.data.mip_levels[num_mips].size = (size_t)(sw * sh * 4);
            desc.num_mipmaps = ++num_mips;
            mw = sw; mh = sh;
        }

        c->gpu_img  = sg_make_image(&desc);
        c->gpu_view = sg_make_view(&(sg_view_desc){ .texture.image = c->gpu_img });

        for (int m = 1; m < num_mips; m++) free(mip_bufs[m]);

        c->pixels   = px;
        s_pixels[i] = NULL;
    }
    (void)smp;
}

/* ------------------------------------------------------------------ */
/* Utilities                                                           */
/* ------------------------------------------------------------------ */

void clips_print(const ClipList *cl) {
    printf("Audio clips (%d):\n", cl->num_audio);
    for (int i = 0; i < cl->num_audio; i++)
        printf("  [%d] %s\n", i, cl->audio[i].name);
    printf("Image clips (%d):\n", cl->num_image);
    for (int i = 0; i < cl->num_image; i++)
        printf("  [%d] %s  (%dx%d)\n", i,
               cl->image[i].name, cl->image[i].width, cl->image[i].height);
    printf("Video clips (%d):\n", cl->num_video);
    for (int i = 0; i < cl->num_video; i++)
        printf("  [%d] %s  %d frames  %dx%d\n", i,
               cl->video[i].name, cl->video[i].num_frames,
               cl->video[i].width, cl->video[i].height);
}

void clips_free(ClipList *cl) {
    for (int i = 0; i < cl->num_audio; i++)
        free(cl->audio[i].samples);
    for (int i = 0; i < cl->num_image; i++)
        if (cl->image[i].pixels) stbi_image_free(cl->image[i].pixels);
    for (int i = 0; i < cl->num_video; i++)
        video_unload(&cl->video[i]);
    memset(cl, 0, sizeof(*cl));
}
