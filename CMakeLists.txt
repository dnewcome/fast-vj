cmake_minimum_required(VERSION 3.16)
project(fast-vj C)

set(CMAKE_C_STANDARD 11)

# ---- Detect Raspberry Pi (ARM) vs desktop ----
if(CMAKE_SYSTEM_PROCESSOR MATCHES "arm|aarch64")
    set(TARGET_PI TRUE)
    message(STATUS "Target: Raspberry Pi (GLES3)")
else()
    set(TARGET_PI FALSE)
    message(STATUS "Target: Desktop (OpenGL 3.3)")
endif()

# ---- Find system libraries ----
find_package(Threads REQUIRED)
find_package(PkgConfig REQUIRED)

# ---- LuaJIT (fallback to Lua 5.4) ----
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
    set(GL_LIBS ${GLES3_LIB} ${EGL_LIB})
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

# ---- KissFFT ----
add_library(kissfft STATIC
    lib/kissfft/kiss_fft.c
    lib/kissfft/kiss_fftr.c
)
target_include_directories(kissfft PUBLIC lib/kissfft lib/kissfft/tools)

# ---- tinyosc ----
add_library(tinyosc STATIC lib/tinyosc/tinyosc.c)
target_include_directories(tinyosc PUBLIC lib)

# ---- libjpeg-turbo (for video frame decode) ----
# Header is in lib/turbojpeg.h (fetched by fetch_deps.sh).
# Runtime .so: apt install libturbojpeg0-dev  (Ubuntu/Debian/Raspbian)
find_library(TURBOJPEG_LIB turbojpeg REQUIRED)
message(STATUS "turbojpeg: ${TURBOJPEG_LIB}")

# ---- fast-vj executable ----
add_executable(fast-vj
    src/sokol_impl.c
    src/main.c
    src/clips.c
    src/osc.c
    src/video.c
    src/script.c
)

target_include_directories(fast-vj PRIVATE
    lib/sokol
    lib
    ${LUA_INCLUDE_DIRS}
)

target_link_libraries(fast-vj PRIVATE
    kissfft
    tinyosc
    ${GL_LIBS}
    ${TURBOJPEG_LIB}
    ${LUA_LINK_LIBS}
    Threads::Threads
    m       # math
    dl      # dlopen (sokol needs it on Linux)
    asound  # ALSA for sokol_audio
)

# ---- Optional: strip on Pi for smaller binary ----
if(TARGET_PI AND CMAKE_BUILD_TYPE STREQUAL "Release")
    add_custom_command(TARGET fast-vj POST_BUILD
        COMMAND ${CMAKE_STRIP} $<TARGET_FILE:fast-vj>
        COMMENT "Stripping binary"
    )
endif()
