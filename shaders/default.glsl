// default.glsl — waveform oscilloscope + FFT bars + image layer
// Standard uniforms available: u_time, u_p[0..14]
// Standard inputs:  image_tex, audio_tex, fft_tex, uv

const float LINE_W = 0.004;

void main() {
    vec4 img = texture(image_tex, uv);
    vec3 bg  = vec3(0.05, 0.05, 0.1);
    vec3 col = mix(bg, img.rgb, img.a);

    // Oscilloscope waveform
    float s  = texture(audio_tex, vec2(uv.x, 0.5)).r;
    float wl = smoothstep(LINE_W, 0.0, abs(uv.y - (s * 0.4 + 0.5)));
    col += vec3(0.2, 0.9, 0.4) * wl;

    // FFT spectrum bars
    float mag = texture(fft_tex, vec2(uv.x, 0.5)).r;
    col += vec3(0.7, 0.2, 0.1) * step(uv.y, mag * 0.3) * 0.35;

    frag_color = vec4(col, 1.0);
}
