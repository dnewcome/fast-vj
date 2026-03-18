/* waveform.glsl
 * Renders audio waveform and FFT spectrum from 1D float textures.
 * Both textures are R32F, normalized to [-1, 1] for audio, [0, 1] for FFT.
 *
 * This file is split by @vs/@fs tags and processed by sokol-shdc,
 * OR you can just inline the strings manually (see main.c).
 */

@vs vs
in vec2 pos;       // fullscreen quad: [-1,1] x [-1,1]
out vec2 uv;       // [0,1] x [0,1]

void main() {
    gl_Position = vec4(pos, 0.0, 1.0);
    uv = pos * 0.5 + 0.5;
}
@end

@fs fs
uniform sampler2D audio_tex;   // 1D strip: R32F waveform [-1, 1]
uniform sampler2D fft_tex;     // 1D strip: R32F magnitude [0, 1]
uniform float time;
in vec2 uv;
out vec4 frag_color;

// line thickness in UV space
const float LINE_W = 0.004;

float waveform_line(float sample, float y) {
    return smoothstep(LINE_W, 0.0, abs(y - (sample * 0.4 + 0.5)));
}

float fft_bar(float magnitude, float y) {
    // bars grow from bottom
    return step(y, magnitude * 0.5);
}

void main() {
    float sample  = texture(audio_tex, vec2(uv.x, 0.5)).r;
    float mag     = texture(fft_tex,   vec2(uv.x, 0.5)).r;

    vec3 bg    = vec3(0.05, 0.05, 0.1);
    vec3 wave  = vec3(0.2, 0.9, 0.4) * waveform_line(sample, uv.y);
    vec3 fft   = vec3(0.8, 0.3, 0.1) * fft_bar(mag, uv.y) * 0.3;

    frag_color = vec4(bg + wave + fft, 1.0);
}
@end
