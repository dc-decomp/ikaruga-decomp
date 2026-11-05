// The bloom's first pass: the frame averaged down to the glow's size, and of that only what is
// bright, fading in from a luminance of 0.85 to full at 1.
#version 450

layout(push_constant) uniform Pass {
    layout(offset = 0) vec2 step;
    layout(offset = 8) int taps;
    layout(offset = 12) float strength;
} p;

layout(set = 0, binding = 0) uniform sampler2D tex;
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 out_colour;

vec4 mean(vec2 uv) {
    const vec2 first = uv - p.step * (float(p.taps - 1) * 0.5);
    vec4 sum = vec4(0.0);
    for (int y = 0; y < p.taps; ++y)
        for (int x = 0; x < p.taps; ++x)
            sum += texture(tex, first + p.step * vec2(x, y));
    return sum / float(p.taps * p.taps);
}

void main() {
    const vec3 c = mean(v_uv).rgb;
    const float luminance = dot(c, vec3(0.2126, 0.7152, 0.0722));
    out_colour = vec4(c * smoothstep(0.85, 1.0, luminance), 1.0);
}
