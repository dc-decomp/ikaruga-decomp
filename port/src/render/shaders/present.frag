// A picture scaled or averaged into the drawable, and the overlay blended over it. A pass over a
// picture: taps along each axis, `step` apart in the source's coordinates.
#version 450

layout(push_constant) uniform Pass {
    layout(offset = 0) vec2 step;
    layout(offset = 8) int taps;
    layout(offset = 12) float strength;
} p;

layout(set = 0, binding = 0) uniform sampler2D tex;

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 out_colour;

// The mean of taps x taps bilinear samples around `uv`.
vec4 mean(vec2 uv) {
    const vec2 first = uv - p.step * (float(p.taps - 1) * 0.5);
    vec4 sum = vec4(0.0);
    for (int y = 0; y < p.taps; ++y)
        for (int x = 0; x < p.taps; ++x)
            sum += texture(tex, first + p.step * vec2(x, y));
    return sum / float(p.taps * p.taps);
}

void main() {
    out_colour = mean(v_uv);
}
