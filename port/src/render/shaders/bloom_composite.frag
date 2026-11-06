// The frame with both sizes of the blurred glow added.
#version 450

layout(push_constant) uniform Pass {
    layout(offset = 0) vec2 step;
    layout(offset = 8) int taps;
    layout(offset = 12) float strength;
} p;

layout(set = 0, binding = 0) uniform sampler2D frame;
layout(set = 0, binding = 1) uniform sampler2D near_glow;
layout(set = 0, binding = 2) uniform sampler2D far_glow;

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 out_colour;

void main() {
    const vec3 c = texelFetch(frame, ivec2(gl_FragCoord.xy), 0).rgb;
    const vec3 glow = texture(near_glow, v_uv).rgb + texture(far_glow, v_uv).rgb;
    out_colour = vec4(min(c + glow * p.strength, 1.0), 1.0);
}
