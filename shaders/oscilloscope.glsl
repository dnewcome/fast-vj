// oscilloscope.glsl — glowing waveform over image
// u_p[0]: glow width  (default 0 → uses 300.0)
// u_p[1]: waveform amplitude scale (default 0 → uses 0.4)
// u_p[2]: image blend (0.0 = black bg, 1.0 = full image)

void main() {
    float glow_k  = u_p[0] > 0.0 ? u_p[0] * 1000.0 : 300.0;
    float amp     = u_p[1] > 0.0 ? u_p[1]           : 0.4;
    float img_mix = u_p[2];

    float s    = texture(audio_tex, vec2(uv.x, 0.5)).r;
    float dist = abs(uv.y - (s * amp + 0.5));
    float line = exp(-dist * glow_k);

    vec4 img = texture(image_tex, uv);
    vec3 col = mix(vec3(0.0), img.rgb, img_mix * img.a);
    col += vec3(0.1, 1.0, 0.3) * line;

    frag_color = vec4(col, 1.0);
}
