// plasma.glsl — audio-reactive sine plasma
// u_p[0]: speed multiplier  (default 1.0)
// u_p[1]: image blend       (0.0 = pure plasma, 1.0 = image tinted by plasma)

void main() {
    float speed = u_p[0] > 0.0 ? u_p[0] : 1.0;

    // Pull energy from bass and mid FFT bins
    float bass = texture(fft_tex, vec2(0.01, 0.5)).r;
    float mid  = texture(fft_tex, vec2(0.08, 0.5)).r;

    float t = u_time * speed * (0.5 + bass * 2.0);
    float cx = uv.x - 0.5;
    float cy = uv.y - 0.5;

    float v = sin(uv.x * 6.0 + t)
            + sin(uv.y * 4.0 + t * 0.7)
            + sin((uv.x + uv.y) * 5.0 + t * 1.3)
            + sin(sqrt(cx*cx + cy*cy) * 10.0 - t * 2.0) * (mid + 0.3);
    v = v * 0.2 + 0.5;

    // Map to colour through a 3-phase sine (gives full-spectrum rainbow)
    const float TAU = 6.28318;
    vec3 col = vec3(
        sin(v * TAU),
        sin(v * TAU + TAU / 3.0),
        sin(v * TAU + TAU * 2.0 / 3.0)
    ) * 0.5 + 0.5;

    // Optional image blend
    vec4 img = texture(image_tex, uv);
    col = mix(col, col * img.rgb, u_p[1] * img.a);

    frag_color = vec4(col, 1.0);
}
