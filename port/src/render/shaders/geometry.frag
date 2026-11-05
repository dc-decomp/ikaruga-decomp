// Opaque and punch-through polygons. The blend and the depth compare are pipeline state.
#version 450

layout(push_constant) uniform Draw {
    layout(offset = 16) int flags;   // 1 texture, 2 offset, 4 use alpha, 8 ignore texture alpha, 16 alpha test
    layout(offset = 20) int shading; // the TSP word's shading instruction
    layout(offset = 24) int fog;     // its fog control
    layout(offset = 28) uint blend;  // a translucent strip: source factor in bits 0-2, destination in 3-5
    layout(offset = 32) float alpha_ref;
    layout(offset = 36) uint order;
    layout(offset = 40) float lod_bias;
    layout(offset = 44) float pad;
    layout(offset = 48) uint width;
    layout(offset = 52) uint capacity;
} draw;

layout(set = 0, binding = 0) uniform Fog {
    vec4 table_colour;   // FOG_COL_RAM
    vec4 vertex_colour;  // FOG_COL_VERT
    vec4 params;         // x: FOG_DENSITY, y: the frame's largest 1/w
    vec4 table[64];      // entry i in table[i / 2], its factor and the next one's in .xy or .zw
} fog;

layout(set = 0, binding = 1) uniform sampler2D tex;

layout(location = 0) in vec4 v_base;
layout(location = 1) in vec4 v_offset;
layout(location = 2) in vec2 v_uv;
layout(location = 0) out vec4 out_colour;

// The table's index comes from the exponent and mantissa of 1/w times the density: 16 entries for
// each doubling. The fraction interpolates within the entry.
float fog_factor(float inv_w) {
    const float z = clamp(inv_w * fog.params.x, 1.0, 255.9999);
    const float e = floor(log2(z));
    const float m = z * 16.0 / exp2(e) - 16.0;
    const int index = int(floor(m) + e * 16.0);
    const vec4 pair = fog.table[index >> 1];
    const vec2 entry = (index & 1) == 0 ? pair.xy : pair.zw;
    return mix(entry.x, entry.y, fract(m));
}

// The texture and shading unit: the texture, the shading instruction, the offset colour and fog.
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
            colour = texel;                                                   // decal
        else if (draw.shading == 1)
            colour = vec4(texel.rgb * base.rgb, texel.a);                     // modulate
        else if (draw.shading == 2)
            colour = vec4(mix(base.rgb, texel.rgb, texel.a), base.a);         // decal alpha
        else
            colour = texel * base;                                            // modulate alpha
    }
    if ((draw.flags & 2) != 0)
        colour.rgb += v_offset.rgb;
    // The depth is 1/w over the frame's largest, so the fragment's depth gives 1/w back.
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
    const vec4 colour = shade();
    if ((draw.flags & 16) != 0 && colour.a < draw.alpha_ref)
        discard;
    out_colour = colour;
}
