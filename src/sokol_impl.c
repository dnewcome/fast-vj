/*
 * sokol_impl.c — single compilation unit for all Sokol implementations.
 *
 * Sokol headers contain both declarations and implementation, gated by
 * SOKOL_IMPL. Defining it here (and ONLY here) means all other .c files
 * can safely include the sokol headers as declarations only, with no
 * risk of duplicate symbol definitions.
 */
#define SOKOL_IMPL
/* Backend is set by CMake (-DSOKOL_GLCORE or -DSOKOL_GLES3) */
#if !defined(SOKOL_GLES3) && !defined(SOKOL_GLCORE)
  #if defined(__arm__) || defined(__aarch64__)
    #define SOKOL_GLES3
  #else
    #define SOKOL_GLCORE
  #endif
#endif
#include "sokol/sokol_app.h"
#include "sokol/sokol_gfx.h"
#include "sokol/sokol_audio.h"
#include "sokol/sokol_glue.h"
#include "sokol/sokol_log.h"
