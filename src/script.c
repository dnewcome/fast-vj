/*
 * script.c — LuaJIT scripting layer.
 *
 * All Lua calls happen on the render thread. The VM is single-threaded;
 * OSC events are forwarded here from poll_osc() (render thread), not
 * from the UDP listener thread.
 */

#include "script.h"
#include "osc.h"
#include "clips.h"

#include <lua.h>
#include <lualib.h>
#include <lauxlib.h>

#include <stdio.h>
#include <stdatomic.h>

static lua_State *L            = NULL;
static float     *s_audio_frame = NULL;
static float     *s_fft_mag     = NULL;
static int        s_tex_width   = 0;

/* ------------------------------------------------------------------ */
/* vj.* C functions                                                   */
/* ------------------------------------------------------------------ */

static int l_vj_audio(lua_State *l) {
    int idx = (int)luaL_checkinteger(l, 1);
    atomic_store_explicit(&g_osc.pending_audio, idx, memory_order_release);
    return 0;
}

static int l_vj_image(lua_State *l) {
    int idx = (int)luaL_checkinteger(l, 1);
    atomic_store_explicit(&g_osc.pending_image, idx, memory_order_release);
    return 0;
}

static int l_vj_video(lua_State *l) {
    int idx = (int)luaL_checkinteger(l, 1);
    atomic_store_explicit(&g_osc.pending_video, idx, memory_order_release);
    return 0;
}

static int l_vj_gain(lua_State *l) {
    float g = (float)luaL_checknumber(l, 1);
    osc_set_gain(g);
    return 0;
}

static int l_vj_stop(lua_State *l) {
    (void)l;
    atomic_store_explicit(&g_osc.stop_audio, 1, memory_order_release);
    return 0;
}

static int l_vj_sample(lua_State *l) {
    int i = (int)luaL_checkinteger(l, 1);
    float v = (i >= 0 && i < s_tex_width && s_audio_frame) ? s_audio_frame[i] : 0.0f;
    lua_pushnumber(l, v);
    return 1;
}

static int l_vj_fft(lua_State *l) {
    int i = (int)luaL_checkinteger(l, 1);
    float v = (i >= 0 && i < s_tex_width && s_fft_mag) ? s_fft_mag[i] : 0.0f;
    lua_pushnumber(l, v);
    return 1;
}

static int l_vj_num_audio(lua_State *l) {
    lua_pushinteger(l, g_clips.num_audio);
    return 1;
}

static int l_vj_num_image(lua_State *l) {
    lua_pushinteger(l, g_clips.num_image);
    return 1;
}

static int l_vj_num_video(lua_State *l) {
    lua_pushinteger(l, g_clips.num_video);
    return 1;
}

static int l_vj_print(lua_State *l) {
    const char *s = luaL_checkstring(l, 1);
    printf("[lua] %s\n", s);
    return 0;
}

static const luaL_Reg s_vj_funcs[] = {
    { "audio",     l_vj_audio     },
    { "image",     l_vj_image     },
    { "video",     l_vj_video     },
    { "gain",      l_vj_gain      },
    { "stop",      l_vj_stop      },
    { "sample",    l_vj_sample    },
    { "fft",       l_vj_fft       },
    { "num_audio", l_vj_num_audio },
    { "num_image", l_vj_num_image },
    { "num_video", l_vj_num_video },
    { "print",     l_vj_print     },
    { NULL,        NULL           }
};

/* ------------------------------------------------------------------ */
/* Public API                                                         */
/* ------------------------------------------------------------------ */

void script_init(float *audio_frame, float *fft_mag, int tex_width) {
    s_audio_frame = audio_frame;
    s_fft_mag     = fft_mag;
    s_tex_width   = tex_width;

    L = luaL_newstate();
    luaL_openlibs(L);

    /* Build the vj table and set it as a global */
    lua_newtable(L);
    for (const luaL_Reg *r = s_vj_funcs; r->name; r++) {
        lua_pushcfunction(L, r->func);
        lua_setfield(L, -2, r->name);
    }
    lua_setglobal(L, "vj");
}

int script_load(const char *path) {
    if (!L) return 0;
    if (luaL_dofile(L, path) != 0) {
        fprintf(stderr, "script: %s\n", lua_tostring(L, -1));
        lua_pop(L, 1);
        return 0;
    }
    printf("script: loaded %s\n", path);
    return 1;
}

int script_eval(const char *code) {
    if (!L) return 0;
    if (luaL_dostring(L, code) != 0) {
        fprintf(stderr, "script eval: %s\n", lua_tostring(L, -1));
        lua_pop(L, 1);
        return 0;
    }
    return 1;
}

void script_call_osc(const char *addr, int arg_type, int ival, float fval) {
    if (!L) return;
    lua_getglobal(L, "on_osc");
    if (!lua_isfunction(L, -1)) { lua_pop(L, 1); return; }

    lua_pushstring(L, addr);
    int nargs = 1;
    if (arg_type == 'i') { lua_pushinteger(L, ival); nargs++; }
    else if (arg_type == 'f') { lua_pushnumber(L, (lua_Number)fval); nargs++; }

    if (lua_pcall(L, nargs, 0, 0) != 0) {
        fprintf(stderr, "script on_osc: %s\n", lua_tostring(L, -1));
        lua_pop(L, 1);
    }
}

void script_call_frame(double dt) {
    if (!L) return;
    lua_getglobal(L, "on_frame");
    if (!lua_isfunction(L, -1)) { lua_pop(L, 1); return; }

    lua_pushnumber(L, (lua_Number)dt);
    if (lua_pcall(L, 1, 0, 0) != 0) {
        fprintf(stderr, "script on_frame: %s\n", lua_tostring(L, -1));
        lua_pop(L, 1);
    }
}

void script_shutdown(void) {
    if (L) {
        lua_close(L);
        L = NULL;
    }
}
