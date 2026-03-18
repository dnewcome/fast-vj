#!/bin/bash
# Downloads all header-only dependencies into lib/
set -e

LIB=lib

# Sokol - the core framework
SOKOL_DIR=$LIB/sokol
if [ ! -f "$SOKOL_DIR/sokol_app.h" ]; then
    echo "Fetching sokol..."
    git clone --depth=1 https://github.com/floooh/sokol.git $SOKOL_DIR
fi

# stb_image - PNG/JPG loading
echo "Fetching stb_image..."
curl -fsSL https://raw.githubusercontent.com/nothings/stb/master/stb_image.h -o $LIB/stb_image.h

# dr_wav - WAV loading
echo "Fetching dr_wav..."
curl -fsSL https://raw.githubusercontent.com/mackron/dr_libs/master/dr_wav.h -o $LIB/dr_wav.h

# stb_image_resize2 - image downscaling
echo "Fetching stb_image_resize2..."
curl -fsSL https://raw.githubusercontent.com/nothings/stb/master/stb_image_resize2.h -o $LIB/stb_image_resize2.h

# KissFFT - small FFT library
KISSFFT_DIR=$LIB/kissfft
if [ ! -f "$KISSFFT_DIR/kiss_fft.c" ]; then
    echo "Fetching kissfft..."
    git clone --depth=1 https://github.com/mborgerding/kissfft.git $KISSFFT_DIR
fi

# turbojpeg.h - header only; runtime .so installed by system package
# Ubuntu/Raspbian: apt install libturbojpeg0-dev
echo "Fetching turbojpeg.h..."
curl -fsSL "https://raw.githubusercontent.com/libjpeg-turbo/libjpeg-turbo/3.0.1/turbojpeg.h" \
    > $LIB/turbojpeg.h

# tinyosc - OSC message parsing (single .h + .c)
TINYOSC_DIR=$LIB/tinyosc
if [ ! -f "$TINYOSC_DIR/tinyosc.c" ]; then
    echo "Fetching tinyosc..."
    git clone --depth=1 https://github.com/mhroth/tinyosc.git $TINYOSC_DIR
fi

echo "Done. All deps in $LIB/"
