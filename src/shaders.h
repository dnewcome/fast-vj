#pragma once
/*
 * shaders.h — fragment shader library.
 *
 * Scans a directory for .glsl files at startup (after sg_setup).
 * Each file provides just the void main() body; the engine injects
 * a standard header with all sampler and uniform declarations:
 *
 *   uniform sampler2D image_tex;   // current image or video frame
 *   uniform sampler2D audio_tex;   // waveform  — 1D R32F, 2048 wide
 *   uniform sampler2D fft_tex;     // FFT mag   — 1D R32F, 2048 wide
 *   in  vec2 uv;                   // 0..1 bottom-left origin
 *   out vec4 frag_color;
 *   uniform float u_time;          // elapsed seconds
 *   uniform float u_p[15];         // user params set via vj.uniform(i, v)
 *
 * Switch active shader:  vj.shader(i)
 * Set a user parameter:  vj.uniform(i, v)   i = 0..14
 */

#include "sokol/sokol_gfx.h"

#define SHADERS_MAX 64
#define SHADER_PARAMS_COUNT 15

/* Standard uniform block — passed to every shader every frame. */
typedef struct {
    float time;
    float p[SHADER_PARAMS_COUNT];
} ShaderParams;

typedef struct {
    char        path[512];
    char        name[64];
    sg_pipeline pip;
} Shader;

typedef struct {
    Shader shaders[SHADERS_MAX];
    int    num_shaders;
} ShaderList;

extern ShaderList   g_shaders;
extern ShaderParams g_shader_params;
extern int          g_current_shader;

/* Scan dir for .glsl fragment shader files. Creates one sg_pipeline each.
 * Must be called after sg_setup(). If no files are found (or dir is NULL),
 * a built-in default shader is added as shaders[0]. */
void shaders_scan(const char *dir);

/* Destroy all pipelines. */
void shaders_free(void);
