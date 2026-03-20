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
#include "shaders.h"
#include "mic.h"

#ifndef __EMSCRIPTEN__
#  include "turbojpeg.h"
#endif
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdio.h>
#include <pthread.h>

/* Sokol logger — suppress benign GL uniform/sampler-not-found warnings that
   fire when the GLSL compiler optimizes away unused uniforms or samplers. */
static void vj_log(const char *tag, uint32_t lvl, uint32_t id,
                   const char *msg, uint32_t line, const char *file, void *ud) {
    (void)ud;
    /* id 10 = GL_UNIFORMBLOCK_NAME_NOT_FOUND_IN_SHADER
       id 11 = GL_IMAGE_SAMPLER_NAME_NOT_FOUND_IN_SHADER */
    if (id == 10 || id == 11) return;
    slog_func(tag, lvl, id, msg, line, file, ud);
}

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
    sg_bindings     bind;
    sg_pass_action  pass_action;
    sg_image        audio_img;
    sg_image        fft_img;
    sg_view         audio_view;
    sg_view         fft_view;
    sg_image        blank_img;     /* 1×1 transparent — default image slot */
    sg_view         blank_view;
    sg_sampler      smp;           /* linear — for RGBA image textures */
    sg_sampler      float_smp;    /* nearest — for R32F audio/FFT textures (WebGL2 compat) */

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
    int             video_frame_decoded; /* last frame index actually decoded */
    double          video_accum;    /* accumulated time for frame advance (seconds) */
    sg_image        video_img;      /* stream texture, created on first video trigger */
    sg_view         video_view;
    int             video_tex_w;    /* current texture dimensions */
    int             video_tex_h;
    void           *tj;             /* libjpeg-turbo handle (tjhandle); NULL on WASM */

    float           time;

    /* FPS counter */
    int             fps_frames;
    double          fps_accum;
} app;

ClipList g_clips;

/* Shaders are loaded from g_shaders_dir by shaders_scan() in init().
 * See shaders.h for the standard uniforms available in every shader. */

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

static const char *g_media_dir    = NULL;
static int         g_osc_port     = 9000;
static const char *g_script_path  = NULL;
static const char *g_shaders_dir  = "shaders";
static int         g_mic_mode     = 0;
static const char *g_mic_device   = NULL;   /* NULL = "default" */
#ifdef __EMSCRIPTEN__
static int         g_show_fps     = 1;
#else
static int         g_show_fps     = 0;
#endif
static int         g_fullscreen   = 0;

static void init(void) {
    sg_setup(&(sg_desc){
        .environment = sglue_environment(),
        .logger.func = vj_log,
    });

    /* ---- Samplers ---- */
    app.smp = sg_make_sampler(&(sg_sampler_desc){
        .min_filter    = SG_FILTER_LINEAR,
        .mag_filter    = SG_FILTER_LINEAR,
        .mipmap_filter = SG_FILTER_LINEAR,   /* trilinear */
        .wrap_u        = SG_WRAP_REPEAT,
        .wrap_v        = SG_WRAP_REPEAT,
    });
    /* R32F textures require NEAREST filtering in WebGL2 to be texture-complete
       (OES_texture_float_linear not guaranteed). Audio/FFT visualisation
       doesn't benefit from linear interpolation anyway. */
    app.float_smp = sg_make_sampler(&(sg_sampler_desc){
        .min_filter = SG_FILTER_NEAREST,
        .mag_filter = SG_FILTER_NEAREST,
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

    /* ---- Load shaders from disk ---- */
    printf("Loading shaders from %s ...\n", g_shaders_dir);
    shaders_scan(g_shaders_dir);

    app.bind = (sg_bindings){
        .vertex_buffers[0] = vbuf,
        .index_buffer      = ibuf,
        .views[0]          = app.blank_view,  /* image slot — blank until triggered */
        .views[1]          = app.audio_view,
        .views[2]          = app.fft_view,
        .samplers[0]       = app.smp,         /* linear — image_tex */
        .samplers[1]       = app.float_smp,   /* nearest — audio_tex, fft_tex */
    };

    app.pass_action = (sg_pass_action){
        .colors[0] = { .load_action = SG_LOADACTION_CLEAR,
                       .clear_value = { 0.05f, 0.05f, 0.1f, 1.0f } },
    };

    /* ---- FFT ---- */
    app.fft_cfg = kiss_fftr_alloc(FFT_SIZE, 0, NULL, NULL);

    /* ---- libjpeg-turbo (video decode, native only) ---- */
#ifndef __EMSCRIPTEN__
    app.tj = tjInitDecompress();
    if (!app.tj) fprintf(stderr, "warning: tjInitDecompress failed\n");
#endif

    /* ---- Audio + video state ---- */
    atomic_store(&app.current_audio, -1);
    atomic_store(&app.audio_cursor,   0);
    app.current_image = -1;
    app.current_video = -1;

    /* sokol_audio uses ScriptProcessorNode on WASM, which runs on the main
       thread and disrupts frame pacing. Skip it until AudioWorklet support
       is added. */
#ifndef __EMSCRIPTEN__
    saudio_setup(&(saudio_desc){
        .sample_rate   = SAMPLE_RATE,
        .num_channels  = 1,
        .buffer_frames = SAMPLES_PER_FRAME,
        .stream_cb     = audio_cb,
        .logger.func   = slog_func,
    });
#endif

    /* ---- Microphone input (optional) ---- */
    if (g_mic_mode)
        mic_init(g_mic_device, SAMPLE_RATE);

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
            app.current_video        = req_vid;
            app.video_frame          = 0;
            app.video_frame_decoded  = -1;
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

    /* Shader trigger */
    int req_shd = atomic_exchange_explicit(&g_osc.pending_shader, -1,
                                           memory_order_acq_rel);
    if (req_shd >= 0 && req_shd < g_shaders.num_shaders) {
        g_current_shader = req_shd;
        printf("shader[%d] %s\n", req_shd, g_shaders.shaders[req_shd].name);
        script_call_osc("/vj/shader", 'i', req_shd, 0);
    }

    /* Animate queue */
    pthread_mutex_lock(&g_osc.anim_mutex);
    while (g_osc.anim_tail != g_osc.anim_head) {
        OscAnimate a = g_osc.anim_queue[g_osc.anim_tail];
        g_osc.anim_tail = (g_osc.anim_tail + 1) % OSC_QUEUE_SIZE;
        pthread_mutex_unlock(&g_osc.anim_mutex);
        script_call_animate(a.param, a.from, a.to, a.duration);
        pthread_mutex_lock(&g_osc.anim_mutex);
    }
    pthread_mutex_unlock(&g_osc.anim_mutex);

    /* Generic queue — /vj/pN sets shader param N, rest forwarded to Lua */
    pthread_mutex_lock(&g_osc.q_mutex);
    while (g_osc.q_tail != g_osc.q_head) {
        OscMsg m = g_osc.queue[g_osc.q_tail];
        g_osc.q_tail = (g_osc.q_tail + 1) % OSC_QUEUE_SIZE;
        pthread_mutex_unlock(&g_osc.q_mutex);

        int pidx = -1;
        if (strncmp(m.addr, "/vj/p", 5) == 0 && m.addr[5] != '\0') {
            char *end;
            long n = strtol(m.addr + 5, &end, 10);
            if (*end == '\0' && n >= 0 && n < SHADER_PARAMS_COUNT)
                pidx = (int)n;
        }
        if (pidx >= 0) {
            float val = (m.type == 'f') ? m.fval : (float)m.ival;
            g_shader_params.p[pidx] = val;
            printf("p[%d] = %.3f\n", pidx, val);
        } else {
            script_call_osc(m.addr, m.type, m.ival, m.fval);
        }

        pthread_mutex_lock(&g_osc.q_mutex);
    }
    pthread_mutex_unlock(&g_osc.q_mutex);
}

/* ------------------------------------------------------------------ */
/* Per-frame video decode and texture upload                           */
/* ------------------------------------------------------------------ */

static void update_video_texture(void) {
#ifndef __EMSCRIPTEN__
    if (app.current_video < 0 || !app.tj) return;
#else
    if (app.current_video < 0) return;
#endif
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

    /* Decode only when the frame index has changed */
    if (app.video_frame != app.video_frame_decoded) {
        if (video_decode_frame(vc, app.video_frame, app.tj) == 0) {
            sg_update_image(app.video_img, &(sg_image_data){
                .mip_levels[0] = {
                    .ptr  = vc->pixels,
                    .size = (size_t)(vc->width * vc->height * 4),
                },
            });
        }
        app.video_frame_decoded = app.video_frame;
    }
    app.bind.views[0] = app.video_view;

    /* Advance frame at clip's FPS regardless of render rate */
    app.video_accum += sapp_frame_duration();
    double frame_duration = 1.0 / (double)vc->fps;
    while (app.video_accum >= frame_duration) {
        app.video_accum -= frame_duration;
        app.video_frame = (app.video_frame + 1) % vc->num_frames;
    }
}

static void update_audio_textures(void) {
    if (g_mic_mode) {
        if (mic_available() < SAMPLES_PER_FRAME) return;
        mic_read(app.audio_frame, SAMPLES_PER_FRAME);
    } else {
        if (ring_avail(&app.ring) < SAMPLES_PER_FRAME) return;
        memset(app.audio_frame, 0, sizeof(app.audio_frame));
        ring_read(&app.ring, app.audio_frame, SAMPLES_PER_FRAME);
    }

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
    double dt = sapp_frame_duration();
    app.time += (float)dt;
    g_shader_params.time = app.time;

    if (g_show_fps) {
        app.fps_frames++;
        app.fps_accum += dt;
        if (app.fps_accum >= 1.0) {
            double fps = app.fps_frames / app.fps_accum;
            char title[64];
            snprintf(title, sizeof(title), "fast-vj  %.1f fps", fps);
            sapp_set_window_title(title);
#ifndef __EMSCRIPTEN__
            printf("fps: %.1f\n", fps);
            fflush(stdout);
#endif
            app.fps_frames = 0;
            app.fps_accum  = 0.0;
        }
    }

    poll_osc();
    update_video_texture();
    update_audio_textures();
    script_call_frame(dt);

    sg_begin_pass(&(sg_pass){
        .action    = app.pass_action,
        .swapchain = sglue_swapchain(),
    });
    sg_apply_pipeline(g_shaders.shaders[g_current_shader].pip);
    sg_apply_bindings(&app.bind);
    sg_apply_uniforms(0, &SG_RANGE(g_shader_params));
    sg_draw(0, 6, 1);
    sg_end_pass();
    sg_commit();
}

static void cleanup(void) {
    script_shutdown();
    if (g_mic_mode) mic_shutdown();
    osc_shutdown();
    saudio_shutdown();
    shaders_free();
    sg_shutdown();
    kiss_fftr_free(app.fft_cfg);
#ifndef __EMSCRIPTEN__
    if (app.tj) tjDestroy(app.tj);
#endif
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
        else if (strcmp(argv[i], "-S") == 0 && i + 1 < argc)
            g_shaders_dir = argv[++i];
        else if (strcmp(argv[i], "-f") == 0)
            g_show_fps = 1;
        else if (strcmp(argv[i], "-F") == 0)
            g_fullscreen = 1;
        else if (strcmp(argv[i], "-m") == 0) {
            g_mic_mode = 1;
            /* optional device name follows: -m hw:1,0 */
            if (i + 1 < argc && argv[i+1][0] != '-')
                g_mic_device = argv[++i];
        } else if (!g_media_dir)
            g_media_dir = argv[i];
        else
            g_osc_port = atoi(argv[i]);
    }
#ifdef __EMSCRIPTEN__
    if (!g_media_dir)
        g_media_dir = "media";
#else
    if (!g_media_dir)
        g_media_dir = ".";
#endif

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
        .fullscreen    = g_fullscreen,
        .window_title  = "fast-vj",
        .swap_interval = 1,
        .logger.func   = vj_log,
        .html5.canvas_resize = true,
    };
}
