// A full-screen triangle. The same vertex shader draws the presentation, the bloom and the overlay.
#version 450

layout(location = 0) out vec2 v_uv;

void main() {
    const vec2 p = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
    v_uv = vec2(p.x, 1.0 - p.y);
}
