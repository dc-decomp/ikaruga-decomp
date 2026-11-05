// A Gaussian blur of 9 texels along `step`, in 5 bilinear samples.
#version 450

layout(push_constant) uniform Pass {
    layout(offset = 0) vec2 step;
    layout(offset = 8) int taps;
    layout(offset = 12) float strength;
} p;

layout(set = 0, binding = 0) uniform sampler2D tex;
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 out_colour;

void main() {
    vec4 c = texture(tex, v_uv) * 0.2270270270;
    c += (texture(tex, v_uv + p.step * 1.3846153846) + texture(tex, v_uv - p.step * 1.3846153846)) * 0.3162162162;
    c += (texture(tex, v_uv + p.step * 3.2307692308) + texture(tex, v_uv - p.step * 3.2307692308)) * 0.0702702703;
    out_colour = c;
}
