/*
 * fast-vj: main.c
 *
 * Usage:  fast-vj <media-directory> [osc-port]
 *
 * Scans the directory for clips. Triggers via OSC over UDP (default 9000):
 *
 *   /vj/audio  <int>    — play audio clip N (loops)
 *   /vj/image  <int>    — show image clip N  (-1 = blank)
 *   /vj/video  <int>    — play video clip N  (-1 = stop)
 *   /vj/gain   <float>  — master audio gain
 *   /vj/stop            — stop audio
 *
 * Video clips are directories of sequential JPEG frames.
 * Create with: ffmpeg -i input.mp4 -q:v 2 media/clip_name/%06d.jpg
 *
 * Clip indices match the printed list at startup (alphabetical order).
 *
 * Build: see CMakeLists.txt
 */

/* sokol implementation is compiled in sokol_impl.c */
#include "sokol/sokol_app.h"
#include "sokol/sokol_gfx.h"
#include "sokol/sokol_audio.h"
#include "sokol/sokol_glue.h"
#include "sokol/sokol_log.h"

#include "clips.h"
#include "osc.h"
#include "video.h"
#include "script.h"

#include "turbojpeg.h"
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdio.h>

/* KissFFT compiled separately */
#include "kiss_fft.h"
#include "kiss_fftr.h"

/* ------------------------------------------------------------------ */
/* Constants                                                           */
/* ------------------------------------------------------------------ */

#define SAMPLE_RATE         44100
#define SAMPLES_PER_FRAME   1024   /* power-of-2, ~23ms, clean ALSA period */
#define FFT_SIZE            2048
#define AUDIO_TEX_WIDTH     FFT_SIZE
#define FFT_BINS            (FFT_SIZE / 2 + 1)

/* ------------------------------------------------------------------ */
/* Ring buffer — audio thread writes, render thread reads             */
/* ------------------------------------------------------------------ */

#define RING_SIZE (FFT_SIZE * 4)

typedef struct { float buf[RING_SIZE]; int write_pos, read_pos; } RingBuf;

static void ring_write(RingBuf *r, const float *src, int n) {
    for (int i = 0; i < n; i++) r->buf[r->write_pos++ % RING_SIZE] = src[i];
}
static int  ring_avail(const RingBuf *r) { return r->write_pos - r->read_pos; }
static void ring_read (RingBuf *r, float *dst, int n) {
    for (int i = 0; i < n; i++) dst[i] = r->buf[r->read_pos++ % RING_SIZE];
}

/* ------------------------------------------------------------------ */
/* Application state                                                   */
/* ------------------------------------------------------------------ */

static struct {
    /* render resources */
    sg_pipeline     pip;
    sg_bindings     bind;
    sg_pass_action  pass_action;
    sg_image        audio_img;
    sg_image        fft_img;
    sg_view         audio_view;
    sg_view         fft_view;
    sg_image        blank_img;     /* 1×1 transparent — default image slot */
    sg_view         blank_view;
    sg_sampler      smp;

    /* CPU upload buffers */
    float           audio_frame[AUDIO_TEX_WIDTH];
    float           fft_mag[AUDIO_TEX_WIDTH];

    /* audio pipeline */
    RingBuf         ring;
    kiss_fftr_cfg   fft_cfg;
    kiss_fft_cpx    fft_out[FFT_BINS];

    /* current playback state (written by audio_cb, read by render) */
    _Atomic int     current_audio; /* -1 = silent */
    _Atomic int     audio_cursor;
    int             current_image; /* render thread only */

    /* video playback — render thread only */
    int             current_video;  /* -1 = no video */
    int             video_frame;    /* current frame index */
    double          video_accum;    /* accumulated time for frame advance (seconds) */
    sg_image        video_img;      /* stream texture, created on first video trigger */
    sg_view         video_view;
    int             video_tex_w;    /* current texture dimensions */
    int             video_tex_h;
    tjhandle        tj;             /* reusable libjpeg-turbo decompressor */

    float           time;
} app;

ClipList g_clips;

/* ------------------------------------------------------------------ */
/* GLSL shaders                                                        */
/* ------------------------------------------------------------------ */

#if defined(SOKOL_GLES3)
  #define GLSL_VER "#version 300 es\nprecision mediump float;\n"
#else
  #define GLSL_VER "#version 330\n"
#endif

static const char *vs_src =
    GLSL_VER
    "in vec2 pos;\n"
    "out vec2 uv;\n"
    "void main() {\n"
    "    gl_Position = vec4(pos, 0.0, 1.0);\n"
    "    uv = pos * 0.5 + 0.5;\n"
    "}\n";

/*
 * Fragment shader:
 *  - image_tex: current image clip (RGBA). Default is 1×1 transparent.
 *  - audio_tex: waveform (1D R32F, range ~[-1,1])
 *  - fft_tex:   spectrum (1D R32F, log magnitude [0,1])
 *
 * The image is drawn first; waveform and FFT bars are added on top.
 * When no image is loaded the dark background shows through.
 */
static const char *fs_src =
    GLSL_VER
    "uniform sampler2D image_tex;\n"
    "uniform sampler2D audio_tex;\n"
    "uniform sampler2D fft_tex;\n"
    "in vec2 uv;\n"
    "out vec4 frag_color;\n"
    "const float LINE_W = 0.004;\n"
    "void main() {\n"
    "    vec4 img = texture(image_tex, uv);\n"
    "    vec3 bg  = vec3(0.05, 0.05, 0.1);\n"
    "    vec3 col = mix(bg, img.rgb, img.a);\n"
    /* waveform */
    "    float s  = texture(audio_tex, vec2(uv.x, 0.5)).r;\n"
    "    float wl = smoothstep(LINE_W, 0.0, abs(uv.y - (s*0.4+0.5)));\n"
    "    col += vec3(0.2, 0.9, 0.4) * wl;\n"
    /* FFT bars */
    "    float mag = texture(fft_tex, vec2(uv.x, 0.5)).r;\n"
    "    col += vec3(0.7, 0.2, 0.1) * step(uv.y, mag * 0.3) * 0.35;\n"
    "    frag_color = vec4(col, 1.0);\n"
    "}\n";

/* ------------------------------------------------------------------ */
/* Audio callback — runs on audio thread                               */
/* ------------------------------------------------------------------ */

static void audio_cb(float *buf, int num_frames, int num_channels) {
    (void)num_channels;

    /* Check for a clip switch requested by the main thread */
    int next = atomic_load_explicit(&app.current_audio, memory_order_acquire);
    if (next < 0 || next >= g_clips.num_audio) {
        memset(buf, 0, num_frames * sizeof(float));
        return;
    }

    AudioClip *c    = &g_clips.audio[next];
    float      gain = osc_gain();
    int        cur  = atomic_load_explicit(&app.audio_cursor, memory_order_relaxed);

    for (int i = 0; i < num_frames; i++) {
        buf[i] = c->samples[cur % c->num_frames] * gain;
        cur++;
    }
    atomic_store_explicit(&app.audio_cursor, (int)(cur % c->num_frames),
                          memory_order_relaxed);

    ring_write(&app.ring, buf, num_frames);
}

/* ------------------------------------------------------------------ */
/* Init                                                                */
/* ------------------------------------------------------------------ */

static const char *g_media_dir   = ".";
static int         g_osc_port    = 9000;
static const char *g_script_path = NULL;

static void init(void) {
    sg_setup(&(sg_desc){
        .environment = sglue_environment(),
        .logger.func = slog_func,
    });

    /* ---- Shared sampler ---- */
    app.smp = sg_make_sampler(&(sg_sampler_desc){
        .min_filter = SG_FILTER_LINEAR,
        .mag_filter = SG_FILTER_LINEAR,
        .wrap_u     = SG_WRAP_CLAMP_TO_EDGE,
        .wrap_v     = SG_WRAP_CLAMP_TO_EDGE,
    });

    /* ---- Fullscreen quad ---- */
    float    verts[] = { -1,-1, 1,-1, 1,1, -1,1 };
    uint16_t idx[]   = { 0,1,2, 0,2,3 };
    sg_buffer vbuf = sg_make_buffer(&(sg_buffer_desc){ .data = SG_RANGE(verts) });
    sg_buffer ibuf = sg_make_buffer(&(sg_buffer_desc){
        .usage.index_buffer = true, .data = SG_RANGE(idx),
    });

    /* ---- 1D stream textures for audio data ---- */
    sg_image_desc tex1d = {
        .width               = AUDIO_TEX_WIDTH,
        .height              = 1,
        .pixel_format        = SG_PIXELFORMAT_R32F,
        .usage.stream_update = true,
    };
    app.audio_img  = sg_make_image(&tex1d);
    app.fft_img    = sg_make_image(&tex1d);
    app.audio_view = sg_make_view(&(sg_view_desc){ .texture.image = app.audio_img });
    app.fft_view   = sg_make_view(&(sg_view_desc){ .texture.image = app.fft_img });

    /* ---- 1×1 transparent default image ---- */
    uint8_t blank[4] = { 0, 0, 0, 0 };
    app.blank_img = sg_make_image(&(sg_image_desc){
        .width  = 1, .height = 1,
        .pixel_format = SG_PIXELFORMAT_RGBA8,
        .data.mip_levels[0] = { .ptr = blank, .size = 4 },
    });
    app.blank_view = sg_make_view(&(sg_view_desc){ .texture.image = app.blank_img });

    /* ---- Upload image clips to GPU ---- */
    clips_upload_gpu(&g_clips, app.smp);

    /* ---- Shader ----
     * views[0] = image_tex  (2D RGBA8)
     * views[1] = audio_tex  (1D R32F)
     * views[2] = fft_tex    (1D R32F)
     * samplers[0] shared by all three
     */
    sg_shader shd = sg_make_shader(&(sg_shader_desc){
        .vertex_func.source   = vs_src,
        .fragment_func.source = fs_src,
        .views[0].texture = { .stage = SG_SHADERSTAGE_FRAGMENT,
                               .image_type  = SG_IMAGETYPE_2D,
                               .sample_type = SG_IMAGESAMPLETYPE_FLOAT },
        .views[1].texture = { .stage = SG_SHADERSTAGE_FRAGMENT,
                               .image_type  = SG_IMAGETYPE_2D,
                               .sample_type = SG_IMAGESAMPLETYPE_FLOAT },
        .views[2].texture = { .stage = SG_SHADERSTAGE_FRAGMENT,
                               .image_type  = SG_IMAGETYPE_2D,
                               .sample_type = SG_IMAGESAMPLETYPE_FLOAT },
        .samplers[0] = { .stage = SG_SHADERSTAGE_FRAGMENT,
                         .sampler_type = SG_SAMPLERTYPE_FILTERING },
        .texture_sampler_pairs[0] = { .stage = SG_SHADERSTAGE_FRAGMENT,
            .view_slot = 0, .sampler_slot = 0, .glsl_name = "image_tex" },
        .texture_sampler_pairs[1] = { .stage = SG_SHADERSTAGE_FRAGMENT,
            .view_slot = 1, .sampler_slot = 0, .glsl_name = "audio_tex" },
        .texture_sampler_pairs[2] = { .stage = SG_SHADERSTAGE_FRAGMENT,
            .view_slot = 2, .sampler_slot = 0, .glsl_name = "fft_tex" },
    });

    app.pip = sg_make_pipeline(&(sg_pipeline_desc){
        .shader     = shd,
        .index_type = SG_INDEXTYPE_UINT16,
        .layout.attrs[0].format = SG_VERTEXFORMAT_FLOAT2,
    });

    app.bind = (sg_bindings){
        .vertex_buffers[0] = vbuf,
        .index_buffer      = ibuf,
        .views[0]          = app.blank_view,  /* image slot — blank until triggered */
        .views[1]          = app.audio_view,
        .views[2]          = app.fft_view,
        .samplers[0]       = app.smp,
    };

    app.pass_action = (sg_pass_action){
        .colors[0] = { .load_action = SG_LOADACTION_CLEAR,
                       .clear_value = { 0.05f, 0.05f, 0.1f, 1.0f } },
    };

    /* ---- FFT ---- */
    app.fft_cfg = kiss_fftr_alloc(FFT_SIZE, 0, NULL, NULL);

    /* ---- libjpeg-turbo (video decode) ---- */
    app.tj = tjInitDecompress();
    if (!app.tj) fprintf(stderr, "warning: tjInitDecompress failed\n");

    /* ---- Audio + video state ---- */
    atomic_store(&app.current_audio, -1);
    atomic_store(&app.audio_cursor,   0);
    app.current_image = -1;
    app.current_video = -1;

    saudio_setup(&(saudio_desc){
        .sample_rate   = SAMPLE_RATE,
        .num_channels  = 1,
        .buffer_frames = SAMPLES_PER_FRAME,
        .stream_cb     = audio_cb,
        .logger.func   = slog_func,
    });

    /* ---- OSC ---- */
    osc_init(g_osc_port);

    /* ---- Lua scripting ---- */
    script_init(app.audio_frame, app.fft_mag, AUDIO_TEX_WIDTH);
    if (g_script_path)
        script_load(g_script_path);

    printf("\nReady. Send OSC to UDP port %d:\n", g_osc_port);
    printf("  /vj/audio <int>   play audio clip\n");
    printf("  /vj/image <int>   show image clip  (-1 = blank)\n");
    printf("  /vj/video <int>   play video clip  (-1 = stop)\n");
    printf("  /vj/gain  <float> master audio gain\n");
    printf("  /vj/stop          stop audio\n\n");
}

/* ------------------------------------------------------------------ */
/* Per-frame: drain OSC commands and update textures                  */
/* ------------------------------------------------------------------ */

static void poll_osc(void) {
    /* Audio clip trigger */
    int req_aud = atomic_exchange_explicit(&g_osc.pending_audio, -1,
                                           memory_order_acq_rel);
    if (req_aud >= 0 && req_aud < g_clips.num_audio) {
        atomic_store_explicit(&app.audio_cursor,   0,       memory_order_release);
        atomic_store_explicit(&app.current_audio,  req_aud, memory_order_release);
        printf("playing audio[%d] %s\n", req_aud, g_clips.audio[req_aud].name);
        script_call_osc("/vj/audio", 'i', req_aud, 0);
    }

    /* Stop */
    if (atomic_exchange_explicit(&g_osc.stop_audio, 0, memory_order_acq_rel)) {
        atomic_store_explicit(&app.current_audio, -1, memory_order_release);
        printf("audio stopped\n");
        script_call_osc("/vj/stop", 0, 0, 0);
    }

    /* Image clip trigger */
    int req_img = atomic_exchange_explicit(&g_osc.pending_image, -1,
                                           memory_order_acq_rel);
    if (req_img != -1) {
        app.current_video = -1;  /* image overrides video */
        app.current_image = req_img;
        if (req_img >= 0 && req_img < g_clips.num_image) {
            app.bind.views[0] = g_clips.image[req_img].gpu_view;
            printf("showing image[%d] %s\n", req_img, g_clips.image[req_img].name);
        } else {
            app.bind.views[0] = app.blank_view;
            printf("image cleared\n");
        }
        script_call_osc("/vj/image", 'i', req_img, 0);
    }

    /* Video clip trigger */
    int req_vid = atomic_exchange_explicit(&g_osc.pending_video, -1,
                                           memory_order_acq_rel);
    if (req_vid != -1) {
        if (req_vid >= 0 && req_vid < g_clips.num_video) {
            app.current_video  = req_vid;
            app.video_frame    = 0;
            app.video_accum    = 0.0;
            app.current_image  = -1;  /* video overrides static image */
            printf("playing video[%d] %s\n", req_vid, g_clips.video[req_vid].name);
        } else {
            app.current_video = -1;
            app.bind.views[0] = app.blank_view;
            printf("video stopped\n");
        }
        script_call_osc("/vj/video", 'i', req_vid, 0);
    }
}

/* ------------------------------------------------------------------ */
/* Per-frame video decode and texture upload                           */
/* ------------------------------------------------------------------ */

static void update_video_texture(void) {
    if (app.current_video < 0 || !app.tj) return;
    VideoClip *vc = &g_clips.video[app.current_video];

    /* (Re)create the stream texture if size changed or first use */
    if (app.video_img.id == SG_INVALID_ID
            || app.video_tex_w != vc->width
            || app.video_tex_h != vc->height) {
        if (app.video_img.id != SG_INVALID_ID) {
            sg_destroy_view(app.video_view);
            sg_destroy_image(app.video_img);
        }
        app.video_img = sg_make_image(&(sg_image_desc){
            .width               = vc->width,
            .height              = vc->height,
            .pixel_format        = SG_PIXELFORMAT_RGBA8,
            .usage.stream_update = true,
            .label               = "video_img",
        });
        app.video_view = sg_make_view(&(sg_view_desc){
            .texture.image = app.video_img,
        });
        app.video_tex_w = vc->width;
        app.video_tex_h = vc->height;
    }

    /* Decode current frame — libjpeg-turbo with NEON SIMD on ARM.
     * At 2K (2048×1080), this takes ~10-15ms on Pi 4. */
    if (video_decode_frame(vc, app.video_frame, app.tj) == 0) {
        sg_update_image(app.video_img, &(sg_image_data){
            .mip_levels[0] = {
                .ptr  = vc->pixels,
                .size = (size_t)(vc->width * vc->height * 4),
            },
        });
        app.bind.views[0] = app.video_view;
    }

    /* Advance frame at clip's FPS regardless of render rate */
    app.video_accum += sapp_frame_duration();
    double frame_duration = 1.0 / (double)vc->fps;
    while (app.video_accum >= frame_duration) {
        app.video_accum -= frame_duration;
        app.video_frame = (app.video_frame + 1) % vc->num_frames;
    }
}

static void update_audio_textures(void) {
    if (ring_avail(&app.ring) < SAMPLES_PER_FRAME) return;

    memset(app.audio_frame, 0, sizeof(app.audio_frame));
    ring_read(&app.ring, app.audio_frame, SAMPLES_PER_FRAME);

    sg_update_image(app.audio_img, &(sg_image_data){
        .mip_levels[0] = SG_RANGE(app.audio_frame),
    });

    kiss_fftr(app.fft_cfg, app.audio_frame, app.fft_out);
    for (int i = 0; i < AUDIO_TEX_WIDTH; i++) {
        if (i < FFT_BINS) {
            float re = app.fft_out[i].r, im = app.fft_out[i].i;
            app.fft_mag[i] = logf(1.0f + sqrtf(re*re + im*im)) / 10.0f;
        } else {
            app.fft_mag[i] = 0.0f;
        }
    }
    sg_update_image(app.fft_img, &(sg_image_data){
        .mip_levels[0] = SG_RANGE(app.fft_mag),
    });
}

/* ------------------------------------------------------------------ */
/* Sokol callbacks                                                     */
/* ------------------------------------------------------------------ */

static void frame(void) {
    app.time += (float)sapp_frame_duration();
    poll_osc();
    update_video_texture();
    update_audio_textures();
    script_call_frame(sapp_frame_duration());

    sg_begin_pass(&(sg_pass){
        .action    = app.pass_action,
        .swapchain = sglue_swapchain(),
    });
    sg_apply_pipeline(app.pip);
    sg_apply_bindings(&app.bind);
    sg_draw(0, 6, 1);
    sg_end_pass();
    sg_commit();
}

static void cleanup(void) {
    script_shutdown();
    osc_shutdown();
    saudio_shutdown();
    sg_shutdown();
    kiss_fftr_free(app.fft_cfg);
    if (app.tj) tjDestroy(app.tj);
    clips_free(&g_clips);
}

static void event(const sapp_event *e) {
    if (e->type == SAPP_EVENTTYPE_KEY_DOWN &&
        e->key_code == SAPP_KEYCODE_ESCAPE)
        sapp_quit();
}

/* ------------------------------------------------------------------ */
/* Entry point                                                         */
/* ------------------------------------------------------------------ */

sapp_desc sokol_main(int argc, char *argv[]) {
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-s") == 0 && i + 1 < argc)
            g_script_path = argv[++i];
        else if (!g_media_dir)
            g_media_dir = argv[i];
        else
            g_osc_port = atoi(argv[i]);
    }
    if (!g_media_dir) {
        fprintf(stderr, "usage: fast-vj <media-dir> [osc-port] [-s script.lua]\n");
        exit(1);
    }

    /* Scan media directory before the window opens */
    printf("Scanning %s ...\n", g_media_dir);
    clips_scan(g_media_dir, &g_clips);
    clips_print(&g_clips);

    return (sapp_desc){
        .init_cb       = init,
        .frame_cb      = frame,
        .cleanup_cb    = cleanup,
        .event_cb      = event,
        .width         = 1280,
        .height        = 720,
        .window_title  = "fast-vj",
        .swap_interval = 1,
        .logger.func   = slog_func,
    };
}
