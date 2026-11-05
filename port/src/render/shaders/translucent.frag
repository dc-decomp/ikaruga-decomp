// A translucent fragment that passes the depth test against the opaque geometry joins its pixel's
// list. The pipeline writes no colour; the sort pass blends it.
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

struct Node {
    uint colour;  // RGBA8, red in the lowest byte
    float depth;  // 1/w, as the depth buffer holds it
    uint key;     // the strip's place in the list in bits 6-31, the blend factors in bits 0-5
    uint next;    // the pixel's next node, or 0xFFFFFFFF
};

layout(set = 0, binding = 2) buffer Heads { uint heads[]; };
layout(set = 0, binding = 3) buffer Nodes { Node nodes[]; };
layout(set = 0, binding = 4) buffer Counters { uint counters[]; };
layout(set = 0, binding = 5) buffer Slices { uint slices[]; };
layout(set = 0, binding = 6) buffer Farthest { uint farthest[]; };
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

void main() {
    const uvec4 c = uvec4(round(shade() * 255.0));
    // A fragment that leaves its pixel as it is takes no node: one whose destination factor is 1 and
    // whose own term is 0, as the transparent corners of a sprite that blends by its alpha.
    const uint source = draw.blend & 7;
    const uint destination = draw.blend >> 3;
    const bool black = all(equal(c, uvec4(0)));
    if ((destination == 1 || (destination == 5 && c.a == 0) || (destination == 3 && black)) &&
        (source == 0 || black || (source == 4 && c.a == 0)))
        return;
    const uvec2 position = uvec2(gl_FragCoord.xy);
    const uint pixel = position.y * draw.width + position.x;
    const uint node = atomicAdd(counters[draw.band], 1u);
    if (node >= draw.capacity) {
        atomicAdd(slices[position.y / 16u], 1u);
        return;
    }
    nodes[node].colour = c.r | c.g << 8 | c.b << 16 | c.a << 24;
    nodes[node].depth = gl_FragCoord.z;
    nodes[node].key = draw.order << 6 | draw.blend;
    nodes[node].next = atomicExchange(heads[pixel], node);
    // The depths of the pixel's farthest and nearest layer, as bits that order as the depth.
    const uint depth = floatBitsToUint(gl_FragCoord.z > 0.0 ? gl_FragCoord.z : 0.0);
    atomicMin(farthest[pixel], depth);
    atomicMax(nearest[pixel], depth);
    out_colour = vec4(0.0);
}
