// The fragments of adds() strips in front of every layer of their pixel's list, after the sort.
#version 450
layout(early_fragment_tests) in;

layout(push_constant) uniform Draw {
    layout(offset = 16) int flags;
    layout(offset = 20) int shading;
    layout(offset = 24) int fog;
    layout(offset = 28) uint blend;
    layout(offset = 32) float alpha_ref;
    layout(offset = 36) uint order;
    layout(offset = 40) float lod_bias;
    layout(offset = 44) float pad;
    layout(offset = 48) uint width;
    layout(offset = 52) uint capacity;
    layout(offset = 56) uint band;
} draw;

layout(set = 0, binding = 0) uniform Fog {
    vec4 table_colour;
    vec4 vertex_colour;
    vec4 params;
    vec4 table[64];
} fog;
layout(set = 0, binding = 1) uniform sampler2D tex;

layout(set = 0, binding = 2) buffer Heads { uint heads[]; };
layout(set = 0, binding = 7) buffer Nearest { uint nearest[]; };

layout(location = 0) in vec4 v_base;
layout(location = 1) in vec4 v_offset;
layout(location = 2) in vec2 v_uv;
layout(location = 0) out vec4 out_colour;

float fog_factor(float inv_w) {
    const float z = clamp(inv_w * fog.params.x, 1.0, 255.9999);
    const float e = floor(log2(z));
    const float m = z * 16.0 / exp2(e) - 16.0;
    const int index = int(floor(m) + e * 16.0);
    const vec4 pair = fog.table[index >> 1];
    const vec2 entry = (index & 1) == 0 ? pair.xy : pair.zw;
    return mix(entry.x, entry.y, fract(m));
}

vec4 shade() {
    vec4 base = v_base;
    if ((draw.flags & 4) == 0)
        base.a = 1.0;
    vec4 colour = base;
    if ((draw.flags & 1) != 0) {
        vec4 texel = texture(tex, v_uv, draw.lod_bias);
        if ((draw.flags & 8) != 0)
            texel.a = 1.0;
        if (draw.shading == 0)
            colour = texel;
        else if (draw.shading == 1)
            colour = vec4(texel.rgb * base.rgb, texel.a);
        else if (draw.shading == 2)
            colour = vec4(mix(base.rgb, texel.rgb, texel.a), base.a);
        else
            colour = texel * base;
    }
    if ((draw.flags & 2) != 0)
        colour.rgb += v_offset.rgb;
    const float inv_w = gl_FragCoord.z * fog.params.y;
    if (draw.fog == 0)
        colour.rgb = mix(colour.rgb, fog.table_colour.rgb, fog_factor(inv_w));
    else if (draw.fog == 1 && (draw.flags & 2) != 0)
        colour.rgb = mix(colour.rgb, fog.vertex_colour.rgb, v_offset.a);
    else if (draw.fog == 3)
        colour = vec4(fog.table_colour.rgb, fog_factor(inv_w));
    return clamp(colour, 0.0, 1.0);
}

uvec4 term(uint colour, uint blend) {
    const vec4 c = vec4(colour & 255u, (colour >> 8) & 255u, (colour >> 16) & 255u, colour >> 24);
    if ((blend & 7) == 0)
        return uvec4(0);
    if ((blend & 7) == 1)
        return uvec4(c);
    if ((blend & 7) == 4)
        return uvec4(round(c * c.a / 255.0));
    return uvec4(round(c * (255.0 - c.a) / 255.0));
}

void main() {
    const uvec2 position = uvec2(gl_FragCoord.xy);
    const uint pixel = position.y * draw.width + position.x;
    const uint depth = floatBitsToUint(gl_FragCoord.z > 0.0 ? gl_FragCoord.z : 0.0);
    if (heads[pixel] == 0xFFFFFFFFu || depth <= nearest[pixel]) {
        out_colour = vec4(0.0);
        return;
    }
    const uvec4 c = uvec4(round(shade() * 255.0));
    out_colour = vec4(term(c.r | c.g << 8 | c.b << 16 | c.a << 24, draw.blend)) / 255.0;
}
