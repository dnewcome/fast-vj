// spectrum.glsl — full-screen colorful FFT spectrum
// u_p[0]: brightness multiplier (default 1.0)
// u_p[1]: color shift (0.0 = red→yellow, 1.0 = full rainbow)

void main() {
    float mag    = texture(fft_tex, vec2(uv.x, 0.5)).r;
    float bright = max(u_p[0], 1.0);

    // Vertical bar
    float bar = step(uv.y, mag * bright);

    // Color shifts from deep red at low freqs to white at high energy
    vec3 lo = vec3(0.6, 0.0, 0.8);
    vec3 hi = vec3(1.0, 0.8, 0.1);
    vec3 col = mix(lo, hi, uv.x) * bar;

    // Bright peak dot at the top of each bar
    float peak_half = 0.004;
    float peak = step(abs(uv.y - mag * bright), peak_half);
    col += vec3(1.0, 1.0, 0.8) * peak;

    // Dim image underneath
    vec4 img = texture(image_tex, uv);
    col = mix(col, img.rgb * 0.25, img.a * 0.4);

    frag_color = vec4(col, 1.0);
}
