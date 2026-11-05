// The PowerVR's drawing in Vulkan. A vertex is in screen space: x and y in pixels, z is 1/w. Clip
// space gets w = 1 / (1/w), so the GPU interpolates every attribute perspective-correct, as the
// PowerVR does, and the depth is 1/w scaled into 0..1, which keeps the order and the ties of the
// hardware's depth compare.
#version 450

layout(location = 0) in vec3 a_position;
layout(location = 1) in vec2 a_uv;
layout(location = 2) in uvec4 a_base;
layout(location = 3) in uvec4 a_offset;

// The screen's scale: x: 2 / width, y: 2 / height, z: 1 / the frame's largest 1/w.
layout(push_constant) uniform Screen {
    layout(offset = 0) vec4 scale;
} screen;

layout(location = 0) out vec4 v_base;
layout(location = 1) out vec4 v_offset;
layout(location = 2) out vec2 v_uv;

invariant gl_Position;

void main() {
    const float inv_w = a_position.z;
    const float w = 1.0 / inv_w;
    const vec2 ndc = vec2(a_position.x * screen.scale.x - 1.0, 1.0 - a_position.y * screen.scale.y);
    gl_Position = vec4(ndc * w, inv_w * screen.scale.z * w, w);
    // The ARGB word's bytes are B, G, R, A in memory; the colours are R, G, B, A.
    v_base = vec4(a_base.zyxw) / 255.0;
    v_offset = vec4(a_offset.zyxw) / 255.0;
    v_uv = a_uv;
}
