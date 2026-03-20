cmake_minimum_required(VERSION 3.16)
project(fast-vj C)

set(CMAKE_C_STANDARD 11)

# ---- KissFFT ----
add_library(kissfft STATIC
    lib/kissfft/kiss_fft.c
    lib/kissfft/kiss_fftr.c
)
target_include_directories(kissfft PUBLIC lib/kissfft lib/kissfft/tools)

# ---- tinyosc ----
add_library(tinyosc STATIC lib/tinyosc/tinyosc.c)
target_include_directories(tinyosc PUBLIC lib)

# ---- Platform-specific deps ----

if(EMSCRIPTEN)
    # ---- WebAssembly / WebGL2 (Emscripten) ----
    message(STATUS "Target: WebAssembly / WebGL2 (Emscripten)")
    add_compile_definitions(SOKOL_GLES3)
    set(GL_LIBS "")

    # Lua 5.4 compiled from source (no system pkg available for WASM)
    # Run fetch_deps.sh first to populate lib/lua54/
    file(GLOB LUA54_SRCS lib/lua54/src/*.c)
    list(FILTER LUA54_SRCS EXCLUDE REGEX "/(lua|luac|onelua)\\.c$")
    if(NOT LUA54_SRCS)
        message(FATAL_ERROR
            "Lua 5.4 source not found in lib/lua54/. Run ./fetch_deps.sh first.")
    endif()
    add_library(lua54_wasm STATIC ${LUA54_SRCS})
    target_include_directories(lua54_wasm PUBLIC lib/lua54/src)
    target_compile_definitions(lua54_wasm PRIVATE LUA_USE_POSIX)
    set(LUA_INCLUDE_DIRS lib/lua54/src)
    set(LUA_LINK_LIBS lua54_wasm)

else()
    # ---- Native build (desktop Linux or Raspberry Pi) ----

    # Detect Raspberry Pi (ARM) vs desktop
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "arm|aarch64")
        set(TARGET_PI TRUE)
        message(STATUS "Target: Raspberry Pi (GLES3)")
    else()
        set(TARGET_PI FALSE)
        message(STATUS "Target: Desktop (OpenGL 3.3)")
    endif()

    find_package(Threads REQUIRED)
    find_package(PkgConfig REQUIRED)

    # LuaJIT (fallback to Lua 5.4)
    pkg_check_modules(LUAJIT luajit)
    if(LUAJIT_FOUND)
        message(STATUS "LuaJIT: ${LUAJIT_VERSION}")
        set(LUA_INCLUDE_DIRS ${LUAJIT_INCLUDE_DIRS})
        set(LUA_LINK_LIBS    ${LUAJIT_LIBRARIES})
    else()
        find_package(Lua 5.4 REQUIRED)
        message(STATUS "LuaJIT not found — using Lua ${LUA_VERSION_STRING}")
        set(LUA_INCLUDE_DIRS ${LUA_INCLUDE_DIR})
        set(LUA_LINK_LIBS    ${LUA_LIBRARIES})
    endif()

    if(TARGET_PI)
        # Pi: OpenGL ES 3 via Mesa, EGL for context creation
        find_library(GLES3_LIB GLESv2 REQUIRED)
        find_library(EGL_LIB   EGL   REQUIRED)
        find_library(X11_LIB   X11   REQUIRED)
        find_library(Xi_LIB    Xi)
        find_library(Xcursor_LIB Xcursor)
        set(GL_LIBS ${GLES3_LIB} ${EGL_LIB} ${X11_LIB} ${Xi_LIB} ${Xcursor_LIB})
        add_compile_definitions(SOKOL_GLES3)
    else()
        # Desktop Linux: OpenGL 3.3 core + X11
        find_package(OpenGL REQUIRED)
        find_library(X11_LIB X11 REQUIRED)
        find_library(Xi_LIB  Xi)
        find_library(Xcursor_LIB Xcursor)
        set(GL_LIBS OpenGL::GL ${X11_LIB} ${Xi_LIB} ${Xcursor_LIB})
        add_compile_definitions(SOKOL_GLCORE)
    endif()

    # libjpeg-turbo (for video frame decode)
    find_library(TURBOJPEG_LIB turbojpeg REQUIRED)
    message(STATUS "turbojpeg: ${TURBOJPEG_LIB}")

endif() # EMSCRIPTEN

# ---- fast-vj executable ----
add_executable(fast-vj
    src/sokol_impl.c
    src/main.c
    src/clips.c
    src/osc.c
    src/video.c
    src/script.c
    src/shaders.c
    src/mic.c
)

target_include_directories(fast-vj PRIVATE
    lib/sokol
    lib
    ${LUA_INCLUDE_DIRS}
)

target_link_libraries(fast-vj PRIVATE
    kissfft
    tinyosc
    ${LUA_LINK_LIBS}
    ${GL_LIBS}
)

if(NOT EMSCRIPTEN)
    target_link_libraries(fast-vj PRIVATE
        ${TURBOJPEG_LIB}
        Threads::Threads
        m       # math
        dl      # dlopen (sokol needs it on Linux)
        asound  # ALSA for sokol_audio
    )
else()
    # Emscripten: output as a self-contained HTML page for easy browser testing
    set_target_properties(fast-vj PROPERTIES SUFFIX ".html")
    target_link_options(fast-vj PRIVATE
        "SHELL:-sUSE_WEBGL2=1"
        "SHELL:-sFULL_ES3=1"
        "SHELL:-sALLOW_MEMORY_GROWTH=1"
        "SHELL:-sINITIAL_MEMORY=67108864"
        "SHELL:-sEXIT_RUNTIME=0"
        "SHELL:-sMINIFY_HTML=0"
        "SHELL:-lwebsocket.js"
        "SHELL:--preload-file ${CMAKE_SOURCE_DIR}/shaders@shaders"
        "SHELL:--preload-file ${CMAKE_SOURCE_DIR}/media@media"
    )
endif()

# ---- Optional: strip on Pi for smaller binary ----
if(TARGET_PI AND CMAKE_BUILD_TYPE STREQUAL "Release")
    add_custom_command(TARGET fast-vj POST_BUILD
        COMMAND ${CMAKE_STRIP} $<TARGET_FILE:fast-vj>
        COMMENT "Stripping binary"
    )
endif()
