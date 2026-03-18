/* kaleidoscope.glsl
 *
 * Mirrors the image into N radial segments to produce a kaleidoscope effect.
 *
 * u_p[0]  segments      number of mirror slices (2..16, default 6)
 * u_p[1]  spin speed    rotation speed in radians/sec (0 = static)
 * u_p[2]  zoom          zoom into source image (1.0 = normal, 2.0 = 2x in)
 * u_p[3]  audio react   bass pulse on zoom (0 = off, 1 = full)
 */

#define PI  3.14159265358979
#define TAU 6.28318530717959

void main() {
    float segments  = max(2.0, u_p[0] > 0.0 ? u_p[0] : 6.0);
    float spin      = u_p[1];
    float zoom      = u_p[2] > 0.0 ? u_p[2] : 1.0;
    float audio_amt = u_p[3];

    /* Bass energy for audio reactivity */
    float bass = 0.0;
    for (int i = 1; i <= 4; i++)
        bass += texture(fft_tex, vec2(float(i) / 512.0, 0.5)).r;
    bass /= 4.0;

    zoom += bass * audio_amt * 0.5;

    /* Center and rotate UV */
    vec2 c = uv - 0.5;
    float angle  = atan(c.y, c.x) + u_time * spin;
    float radius = length(c);

    /* Fold into one mirror segment */
    float slice = PI / segments;
    angle = mod(angle, 2.0 * slice);
    if (angle > slice) angle = 2.0 * slice - angle;

    /* Back to cartesian, apply zoom, re-center, wrap so image tiles */
    vec2 kale_uv = fract(vec2(cos(angle), sin(angle)) * (radius / zoom) + 0.5);

    vec4 col = texture(image_tex, kale_uv);

    /* If no image is loaded (all black), show a colour-cycling tint instead */
    float luma = dot(col.rgb, vec3(0.299, 0.587, 0.114));
    vec3 tint = 0.5 + 0.5 * vec3(
        sin(u_time * 0.3),
        sin(u_time * 0.3 + TAU / 3.0),
        sin(u_time * 0.3 + 2.0 * TAU / 3.0)
    );
    col.rgb = mix(tint * 0.4, col.rgb, clamp(luma * 4.0, 0.0, 1.0));

    frag_color = vec4(col.rgb, 1.0);
}
