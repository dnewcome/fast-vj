/*
 * shaders.c — fragment shader library loader.
 */

#include "shaders.h"

#include <dirent.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

ShaderList   g_shaders;
ShaderParams g_shader_params;
int          g_current_shader = 0;

/* ------------------------------------------------------------------ */
/* GLSL version string (matches sokol backend)                        */
/* ------------------------------------------------------------------ */

#if defined(SOKOL_GLES3)
  #define GLSL_VER "#version 300 es\nprecision mediump float;\n"
#else
  #define GLSL_VER "#version 330\n"
#endif

/* ------------------------------------------------------------------ */
/* Standard header injected before every user shader                  */
/* ------------------------------------------------------------------ */

static const char *s_frag_header =
    "uniform sampler2D image_tex;\n"
    "uniform sampler2D audio_tex;\n"
    "uniform sampler2D fft_tex;\n"
    "in vec2 uv;\n"
    "out vec4 frag_color;\n"
    "uniform float u_time;\n"
    "uniform float u_p[15];\n";

static const char *s_vs_src =
    GLSL_VER
    "in vec2 pos;\n"
    "out vec2 uv;\n"
    "void main() {\n"
    "    gl_Position = vec4(pos, 0.0, 1.0);\n"
    "    uv = pos * 0.5 + 0.5;\n"
    "}\n";

/* ------------------------------------------------------------------ */
/* Built-in default shader (waveform + FFT bars + image)              */
/* ------------------------------------------------------------------ */

static const char *s_default_body =
    "const float LINE_W = 0.004;\n"
    "void main() {\n"
    "    vec4 img = texture(image_tex, uv);\n"
    "    vec3 bg  = vec3(0.05, 0.05, 0.1);\n"
    "    vec3 col = mix(bg, img.rgb, img.a);\n"
    "    float s  = texture(audio_tex, vec2(uv.x, 0.5)).r;\n"
    "    float wl = smoothstep(LINE_W, 0.0, abs(uv.y - (s*0.4+0.5)));\n"
    "    col += vec3(0.2, 0.9, 0.4) * wl;\n"
    "    float mag = texture(fft_tex, vec2(uv.x, 0.5)).r;\n"
    "    col += vec3(0.7, 0.2, 0.1) * step(uv.y, mag * 0.3) * 0.35;\n"
    "    frag_color = vec4(col, 1.0);\n"
    "}\n";

/* ------------------------------------------------------------------ */
/* Pipeline factory                                                   */
/* ------------------------------------------------------------------ */

static int make_shader_and_pipeline(const char *fs_body, sg_shader *out_shd, sg_pipeline *out_pip) {
    size_t len = strlen(GLSL_VER) + strlen(s_frag_header) + strlen(fs_body) + 1;
    char *fs_src = malloc(len);
    if (!fs_src) return 0;
    strcpy(fs_src, GLSL_VER);
    strcat(fs_src, s_frag_header);
    strcat(fs_src, fs_body);

    sg_shader shd = sg_make_shader(&(sg_shader_desc){
        .vertex_func.source   = s_vs_src,
        .fragment_func.source = fs_src,
        .views[0].texture = { .stage       = SG_SHADERSTAGE_FRAGMENT,
                               .image_type  = SG_IMAGETYPE_2D,
                               .sample_type = SG_IMAGESAMPLETYPE_FLOAT },
        .views[1].texture = { .stage       = SG_SHADERSTAGE_FRAGMENT,
                               .image_type  = SG_IMAGETYPE_2D,
                               .sample_type = SG_IMAGESAMPLETYPE_FLOAT },
        .views[2].texture = { .stage       = SG_SHADERSTAGE_FRAGMENT,
                               .image_type  = SG_IMAGETYPE_2D,
                               .sample_type = SG_IMAGESAMPLETYPE_FLOAT },
        .samplers[0] = { .stage        = SG_SHADERSTAGE_FRAGMENT,
                         .sampler_type = SG_SAMPLERTYPE_FILTERING },
        .samplers[1] = { .stage        = SG_SHADERSTAGE_FRAGMENT,
                         .sampler_type = SG_SAMPLERTYPE_NONFILTERING },
        .texture_sampler_pairs[0] = { .stage = SG_SHADERSTAGE_FRAGMENT,
            .view_slot = 0, .sampler_slot = 0, .glsl_name = "image_tex" },
        .texture_sampler_pairs[1] = { .stage = SG_SHADERSTAGE_FRAGMENT,
            .view_slot = 1, .sampler_slot = 1, .glsl_name = "audio_tex" },
        .texture_sampler_pairs[2] = { .stage = SG_SHADERSTAGE_FRAGMENT,
            .view_slot = 2, .sampler_slot = 1, .glsl_name = "fft_tex" },
        .uniform_blocks[0] = {
            .stage  = SG_SHADERSTAGE_FRAGMENT,
            .size   = sizeof(ShaderParams),
            .layout = SG_UNIFORMLAYOUT_NATIVE,
            .glsl_uniforms = {
                [0] = { .glsl_name = "u_time", .type = SG_UNIFORMTYPE_FLOAT,
                        .array_count = 1  },
                [1] = { .glsl_name = "u_p",    .type = SG_UNIFORMTYPE_FLOAT,
                        .array_count = 15 },
            },
        },
    });
    free(fs_src);

    if (sg_query_shader_state(shd) != SG_RESOURCESTATE_VALID) {
        fprintf(stderr, "shaders: shader compile failed\n");
        sg_destroy_shader(shd);
        return 0;
    }

    sg_pipeline pip = sg_make_pipeline(&(sg_pipeline_desc){
        .shader     = shd,
        .index_type = SG_INDEXTYPE_UINT16,
        .layout.attrs[0].format = SG_VERTEXFORMAT_FLOAT2,
    });
    if (sg_query_pipeline_state(pip) != SG_RESOURCESTATE_VALID) {
        fprintf(stderr, "shaders: pipeline creation failed\n");
        sg_destroy_shader(shd);
        sg_destroy_pipeline(pip);
        return 0;
    }
    /* Do NOT destroy shd — the pipeline references it and needs it alive. */
    *out_shd = shd;
    *out_pip = pip;
    return 1;
}

/* ------------------------------------------------------------------ */
/* File helpers                                                       */
/* ------------------------------------------------------------------ */

static int is_glsl(const char *name) {
    const char *ext = strrchr(name, '.');
    return ext && strcasecmp(ext, ".glsl") == 0;
}

static char *read_text_file(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) { perror(path); return NULL; }
    fseek(f, 0, SEEK_END);
    size_t sz = (size_t)ftell(f);
    rewind(f);
    char *buf = malloc(sz + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t n = fread(buf, 1, sz, f);
    buf[n] = '\0';
    fclose(f);
    return buf;
}

static void add_shader(const char *path, const char *name, const char *body) {
    Shader *s = &g_shaders.shaders[g_shaders.num_shaders];
    if (!make_shader_and_pipeline(body, &s->shd, &s->pip)) {
        fprintf(stderr, "shaders: failed to compile '%s'\n", name);
        return;
    }
    strncpy(s->path, path, sizeof(s->path) - 1);
    strncpy(s->name, name, sizeof(s->name) - 1);
    printf("  shader[%d] %s\n", g_shaders.num_shaders, name);
    g_shaders.num_shaders++;
}

/* ------------------------------------------------------------------ */
/* Public API                                                         */
/* ------------------------------------------------------------------ */

void shaders_scan(const char *dir) {
    memset(&g_shaders,       0, sizeof(g_shaders));
    memset(&g_shader_params, 0, sizeof(g_shader_params));

    if (dir) {
        struct dirent **entries;
        int n = scandir(dir, &entries, NULL, alphasort);
        if (n >= 0) {
            for (int i = 0; i < n && g_shaders.num_shaders < SHADERS_MAX; i++) {
                const char *name = entries[i]->d_name;
                if (is_glsl(name)) {
                    char path[512];
                    snprintf(path, sizeof(path), "%s/%s", dir, name);
                    char *body = read_text_file(path);
                    if (body) {
                        /* Strip .glsl extension for display name */
                        char display[64];
                        const char *dot = strrchr(name, '.');
                        size_t nlen = dot ? (size_t)(dot - name) : strlen(name);
                        if (nlen >= sizeof(display)) nlen = sizeof(display) - 1;
                        memcpy(display, name, nlen);
                        display[nlen] = '\0';
                        add_shader(path, display, body);
                        free(body);
                    }
                }
                free(entries[i]);
            }
            free(entries);
        }
    }

    if (g_shaders.num_shaders == 0) {
        printf("  shader[0] default (built-in)\n");
        add_shader("<built-in>", "default", s_default_body);
    }
}

void shaders_free(void) {
    for (int i = 0; i < g_shaders.num_shaders; i++) {
        sg_destroy_pipeline(g_shaders.shaders[i].pip);
        sg_destroy_shader(g_shaders.shaders[i].shd);
    }
    memset(&g_shaders, 0, sizeof(g_shaders));
}
