// A picture scaled or averaged into the drawable, and the overlay blended over it. A pass over a
// picture: taps along each axis, `step` apart in the source's coordinates. `strength` is here the
// picture's turn: 0 none, 1 a quarter turn counter-clockwise, 2 a quarter turn clockwise (tate).
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
    // A turned picture: the drawable's (u, v), from its top left, reads the picture at (s, t).
    //   counter-clockwise: the picture's right edge goes to the top, so (s, t) = (1 - v, u)
    //   clockwise:         its left edge goes to the top, so (s, t) = (v, 1 - u)
    vec2 uv = v_uv;
    if (p.strength > 1.5)
        uv = vec2(v_uv.y, 1.0 - v_uv.x);
    else if (p.strength > 0.5)
        uv = vec2(1.0 - v_uv.y, v_uv.x);
    out_colour = mean(uv);
}
