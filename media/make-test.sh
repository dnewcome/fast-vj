ffmpeg -f lavfi -i mandelbrot=size=1920x1080:rate=30 -t 4 \
    -vcodec mjpeg -q:v 3 -pix_fmt yuvj420p \
    test_mandelbrot.avi
