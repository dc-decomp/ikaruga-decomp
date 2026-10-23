// The PowerVR's drawing in Metal. A vertex is in screen space: x and y in pixels, z is 1/w. Clip
// space gets w = 1 / (1/w), so the GPU interpolates every attribute perspective-correct, as the
// PowerVR does, and the depth is 1/w scaled into 0..1, which keeps the order and the ties of the
// hardware's depth compare.
//
// Ikaruga runs the PowerVR in autosort mode: for each pixel, the hardware blends the translucent
// polygons from the farthest to the nearest, whatever their order in the list. Here each
// translucent fragment is shaded and added to its pixel's list in device memory, and a full-screen
// pass sorts each list and blends it, with the blend factors of each fragment's polygon.
#include <metal_stdlib>
using namespace metal;

struct VertexIn {
    float3 position [[attribute(0)]];
    float2 uv [[attribute(1)]];
    uchar4 base [[attribute(2)]];    // the ARGB word's bytes: B, G, R, A
    uchar4 offset [[attribute(3)]];
};

struct Screen {
    float4 scale;  // x: 2 / width, y: 2 / height, z: 1 / the frame's largest 1/w
};

struct Varyings {
    float4 position [[position, invariant]];
    float4 base;
    float4 offset;
    float2 uv;
};

vertex Varyings geometry_vertex(VertexIn in [[stage_in]], constant Screen& screen [[buffer(1)]]) {
    Varyings out;
    const float inv_w = in.position.z;
    const float w = 1.0 / inv_w;
    const float2 ndc = float2(in.position.x * screen.scale.x - 1.0, 1.0 - in.position.y * screen.scale.y);
    out.position = float4(ndc * w, inv_w * screen.scale.z * w, w);
    out.base = float4(in.base.zyxw) / 255.0;
    out.offset = float4(in.offset.zyxw) / 255.0;
    out.uv = in.uv;
    return out;
}

// For each strip.
struct Draw {
    int flags;     // 1 texture, 2 offset, 4 use alpha, 8 ignore texture alpha, 16 alpha test
    int shading;   // the TSP word's shading instruction
    int fog;       // its fog control
    uint blend;    // a translucent strip: source factor in bits 0-2, destination in 3-5
    float alpha_ref;
    uint order;    // a translucent strip: its place in the list, which decides a tie in depth
    float lod_bias;  // the mipmap D adjust, as a level bias
    float pad;
};

// For each frame: the fog registers and the 128-entry table.
struct Fog {
    float4 table_colour;   // FOG_COL_RAM
    float4 vertex_colour;  // FOG_COL_VERT
    float4 params;         // x: FOG_DENSITY, y: the frame's largest 1/w
    float4 table[64];      // entry i in table[i / 2]: its factor and the next one's, .xy or .zw
};

// The table's index comes from the exponent and mantissa of 1/w times the density: 16 entries for
// each doubling. The fraction interpolates within the entry.
static float fog_factor(constant Fog& fog, float inv_w) {
    const float z = clamp(inv_w * fog.params.x, 1.0, 255.9999);
    const float e = floor(log2(z));
    const float m = z * 16.0 / exp2(e) - 16.0;
    const int index = int(floor(m) + e * 16.0);
    const float4 pair = fog.table[index >> 1];
    const float2 entry = (index & 1) == 0 ? pair.xy : pair.zw;
    return mix(entry.x, entry.y, fract(m));
}

// The texture and shading unit: the texture, the shading instruction, the offset colour and fog.
// False when the alpha test discards the pixel.
static float4 shade(Varyings in, constant Draw& draw, constant Fog& fog, texture2d<float> tex, sampler smp) {
    float4 base = in.base;
    if ((draw.flags & 4) == 0)
        base.a = 1.0;
    float4 colour = base;
    if ((draw.flags & 1) != 0) {
        float4 texel = tex.sample(smp, in.uv, bias(draw.lod_bias));
        if ((draw.flags & 8) != 0)
            texel.a = 1.0;
        switch (draw.shading) {
            case 0: colour = texel; break;                                               // decal
            case 1: colour = float4(texel.rgb * base.rgb, texel.a); break;               // modulate
            case 2: colour = float4(mix(base.rgb, texel.rgb, texel.a), base.a); break;   // decal alpha
            default: colour = texel * base; break;                                       // modulate alpha
        }
    }
    if ((draw.flags & 2) != 0)
        colour.rgb += in.offset.rgb;
    // The depth is 1/w over the frame's largest, so the fragment's depth gives 1/w back.
    const float inv_w = in.position.z * fog.params.y;
    if (draw.fog == 0)
        colour.rgb = mix(colour.rgb, fog.table_colour.rgb, fog_factor(fog, inv_w));
    else if (draw.fog == 1 && (draw.flags & 2) != 0)
        colour.rgb = mix(colour.rgb, fog.vertex_colour.rgb, in.offset.a);
    else if (draw.fog == 3)
        colour = float4(fog.table_colour.rgb, fog_factor(fog, inv_w));
    return clamp(colour, 0.0, 1.0);
}

// Opaque and punch-through polygons: blending and the depth compare are pipeline state.
fragment float4 geometry_fragment(Varyings in [[stage_in]], constant Draw& draw [[buffer(0)]],
                                  constant Fog& fog [[buffer(1)]], texture2d<float> tex [[texture(0)]],
                                  sampler smp [[sampler(0)]]) {
    const float4 colour = shade(in, draw, fog, tex, smp);
    if ((draw.flags & 16) != 0 && colour.a < draw.alpha_ref)
        discard_fragment();
    return colour;
}

// One translucent fragment, waiting for its pixel's sort.
struct Node {
    uint colour;  // RGBA8, red in the lowest byte
    float depth;  // 1/w, as the depth buffer holds it
    uint key;     // the strip's place in the list in bits 6-31, the blend factors in bits 0-5
    uint next;    // the pixel's next node, or 0xFFFFFFFF
};

// The renderer counts translucent fragments in slices of this many rows, and draws the screen in
// bands of whole slices.
constant uint kSliceRows = 16;

struct Lists {
    uint width;
    uint capacity;  // nodes
};

// A translucent fragment that passes the depth test against the opaque geometry joins its pixel's
// list. The pipeline writes no colour; the sort pass blends it.
[[early_fragment_tests]] fragment void translucent_fragment(
    Varyings in [[stage_in]], constant Draw& draw [[buffer(0)]], constant Fog& fog [[buffer(1)]],
    constant Lists& lists [[buffer(2)]], device atomic_uint* heads [[buffer(3)]], device Node* nodes [[buffer(4)]],
    device atomic_uint* used [[buffer(5)]], device atomic_uint* slices [[buffer(6)]], texture2d<float> tex [[texture(0)]],
    sampler smp [[sampler(0)]]) {
    const float4 colour = shade(in, draw, fog, tex, smp);
    const uint node = atomic_fetch_add_explicit(used, 1u, memory_order_relaxed);
    if (node >= lists.capacity) {
        // The band's share of the pool is full. The renderer plans the next render's bands from what
        // each slice of rows needed, this fragment included.
        atomic_fetch_add_explicit(&slices[uint(in.position.y) / kSliceRows], 1u, memory_order_relaxed);
        return;
    }
    const uint pixel = uint(in.position.y) * lists.width + uint(in.position.x);
    const uint4 c = uint4(round(colour * 255.0));
    nodes[node].colour = c.r | c.g << 8 | c.b << 16 | c.a << 24;
    nodes[node].depth = in.position.z;
    nodes[node].key = draw.order << 6 | draw.blend;
    nodes[node].next = atomic_exchange_explicit(&heads[pixel], node, memory_order_relaxed);
}

struct FullScreen {
    float4 position [[position]];
};

vertex FullScreen full_screen_vertex(uint id [[vertex_id]]) {
    const float2 p = float2((id << 1) & 2, id & 2);
    FullScreen out;
    out.position = float4(p * 2.0 - 1.0, 0.0, 1.0);
    return out;
}

// A blend factor of the TSP word. The source's "other" colour is the destination's and the other
// way round.
static float4 factor(uint instruction, float4 src, float4 dst, bool source) {
    const float4 other = source ? dst : src;
    switch (instruction & 7) {
        case 0: return float4(0.0);
        case 1: return float4(1.0);
        case 2: return other;
        case 3: return 1.0 - other;
        case 4: return float4(src.a);
        case 5: return float4(1.0 - src.a);
        case 6: return float4(dst.a);
        default: return float4(1.0 - dst.a);
    }
}

constant uint kMaxLayers = 64;

// Each pixel's translucent fragments, from the farthest (the smallest 1/w) to the nearest, the
// earlier strip first on a tie, each blended over what is there with its own factors. The
// framebuffer holds 8 bits a channel after each blend. A pixel with more fragments than fit keeps
// the nearest ones.
fragment float4 sort_fragment(FullScreen in [[stage_in]], float4 dst [[color(0)]], constant Lists& lists [[buffer(2)]],
                              device const uint* heads [[buffer(3)]], device const Node* nodes [[buffer(4)]],
                              device atomic_uint* slices [[buffer(5)]]) {
    const uint pixel = uint(in.position.y) * lists.width + uint(in.position.x);
    uint index[kMaxLayers];
    uint count = 0, length = 0;
    for (uint n = heads[pixel]; n != 0xFFFFFFFFu && n < lists.capacity; n = nodes[n].next) {
        ++length;
        // Insertion by (depth, key): the farthest first.
        const float d = nodes[n].depth;
        const uint k = nodes[n].key;
        uint i = count < kMaxLayers ? count++ : kMaxLayers;
        if (i == kMaxLayers) {
            // Full: drop the farthest unless this one is farther still.
            const Node first = nodes[index[0]];
            if (d < first.depth || (d == first.depth && k < first.key))
                continue;
            for (uint j = 1; j < kMaxLayers; ++j) index[j - 1] = index[j];
            i = kMaxLayers - 1;
        }
        while (i > 0) {
            const Node prev = nodes[index[i - 1]];
            if (prev.depth < d || (prev.depth == d && prev.key < k))
                break;
            index[i] = index[i - 1];
            --i;
        }
        index[i] = n;
    }
    // One atomic for each SIMD group that lies in one slice: thousands of pixels count into each.
    const uint slice = uint(in.position.y) / kSliceRows;
    if (simd_all(slice == simd_broadcast_first(slice))) {
        const uint total = simd_sum(length);
        if (simd_is_first() && total)
            atomic_fetch_add_explicit(&slices[slice], total, memory_order_relaxed);
    }
    else if (length)
        atomic_fetch_add_explicit(&slices[slice], length, memory_order_relaxed);
    float4 colour = dst;
    for (uint i = 0; i < count; ++i) {
        const Node node = nodes[index[i]];
        const float4 src = float4(node.colour & 255, (node.colour >> 8) & 255, (node.colour >> 16) & 255,
                                  node.colour >> 24) / 255.0;
        const uint blend = node.key & 63;
        colour = clamp(src * factor(blend, src, colour, true) + colour * factor(blend >> 3, src, colour, false), 0.0, 1.0);
        colour = round(colour * 255.0) / 255.0;
    }
    return colour;
}

// The window: a framebuffer, scaled into the drawable's 4:3 area.
struct Present {
    float4 position [[position]];
    float2 uv;
};

vertex Present present_vertex(uint id [[vertex_id]]) {
    const float2 p = float2((id << 1) & 2, id & 2);
    Present out;
    out.position = float4(p * 2.0 - 1.0, 0.0, 1.0);
    out.uv = float2(p.x, 1.0 - p.y);
    return out;
}

fragment float4 present_fragment(Present in [[stage_in]], texture2d<float> frame [[texture(0)]],
                                 sampler smp [[sampler(0)]]) {
    return frame.sample(smp, in.uv);
}
