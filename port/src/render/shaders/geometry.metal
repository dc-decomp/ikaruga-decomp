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

// The blends that add a term that does not depend on the pixel, with a destination factor of 1. The
// layers of such blends between two others give the same pixel in any order: each step adds a term
// that is not negative, rounds the sum, which holds whole 8-bit steps already, and clamps at 1.
static bool adds(uint blend) {
    const uint source = blend & 7;
    return (blend >> 3) == 1 && (source == 0 || source == 1 || source == 4 || source == 5);
}

// The term of an adds() layer in 8-bit steps: what its blend step adds, rounded as the step rounds.
static uint4 term(uint colour, uint blend) {
    const float4 c = float4(colour & 255, (colour >> 8) & 255, (colour >> 16) & 255, colour >> 24);
    switch (blend & 7) {
        case 0: return uint4(0);
        case 1: return uint4(c);
        case 4: return uint4(round(c * c.a / 255.0));
        default: return uint4(round(c * (255.0 - c.a) / 255.0));
    }
}

// A fragment joins its pixel's list, unless the band's share of the pool is full. The renderer
// plans the next render's bands from what each slice of rows needed, this fragment included.
static void insert(uint colour, float depth, uint key, uint2 position, constant Lists& lists,
                   device atomic_uint* heads, device Node* nodes, device atomic_uint* used,
                   device atomic_uint* slices) {
    const uint node = atomic_fetch_add_explicit(used, 1u, memory_order_relaxed);
    if (node >= lists.capacity) {
        atomic_fetch_add_explicit(&slices[position.y / kSliceRows], 1u, memory_order_relaxed);
        return;
    }
    nodes[node].colour = colour;
    nodes[node].depth = depth;
    nodes[node].key = key;
    nodes[node].next = atomic_exchange_explicit(&heads[position.y * lists.width + position.x], node,
                                                memory_order_relaxed);
}

// A translucent fragment that passes the depth test against the opaque geometry joins its pixel's
// list. The pipeline writes no colour; the sort pass blends it.
[[early_fragment_tests]] fragment void translucent_fragment(
    Varyings in [[stage_in]], constant Draw& draw [[buffer(0)]], constant Fog& fog [[buffer(1)]],
    constant Lists& lists [[buffer(2)]], device atomic_uint* heads [[buffer(3)]], device Node* nodes [[buffer(4)]],
    device atomic_uint* used [[buffer(5)]], device atomic_uint* slices [[buffer(6)]],
    device atomic_uint* farthest [[buffer(7)]], device atomic_uint* nearest [[buffer(8)]],
    texture2d<float> tex [[texture(0)]], sampler smp [[sampler(0)]]) {
    const uint4 c = uint4(round(shade(in, draw, fog, tex, smp) * 255.0));
    // A fragment that leaves its pixel as it is takes no node: one whose destination factor is 1 and
    // whose own term is 0, as the transparent corners of a sprite that blends by its alpha.
    const uint source = draw.blend & 7, destination = draw.blend >> 3;
    const bool black = all(c == 0);
    if ((destination == 1 || (destination == 5 && c.a == 0) || (destination == 3 && black)) &&
        (source == 0 || black || (source == 4 && c.a == 0)))
        return;
    const uint2 position = uint2(in.position.xy);
    insert(c.r | c.g << 8 | c.b << 16 | c.a << 24, in.position.z, draw.order << 6 | draw.blend, position, lists, heads,
           nodes, used, slices);
    // The depths of the pixel's farthest and nearest layer, as bits that order as the depth.
    const uint pixel = position.y * lists.width + position.x;
    const uint depth = as_type<uint>(in.position.z > 0.0f ? in.position.z : 0.0f);
    atomic_fetch_min_explicit(&farthest[pixel], depth, memory_order_relaxed);
    atomic_fetch_max_explicit(&nearest[pixel], depth, memory_order_relaxed);
}

// A translucent fragment of an adds() strip, drawn after the band's other strips. Where its pixel
// has no list, or the fragment lies behind every layer of the list, its term goes to the framebuffer
// before the sort, through the pipeline's blend of one and one: the terms of such layers sum to the
// same in any order. A fragment in front of every layer waits for added_on_top(). Only one between
// the layers, or at the depth of one, joins the list, where the sort places its term.
[[early_fragment_tests]] fragment float4 added_fragment(
    Varyings in [[stage_in]], constant Draw& draw [[buffer(0)]], constant Fog& fog [[buffer(1)]],
    constant Lists& lists [[buffer(2)]], device atomic_uint* heads [[buffer(3)]], device Node* nodes [[buffer(4)]],
    device atomic_uint* used [[buffer(5)]], device atomic_uint* slices [[buffer(6)]],
    device const uint* farthest [[buffer(7)]], device const uint* nearest [[buffer(8)]],
    texture2d<float> tex [[texture(0)]], sampler smp [[sampler(0)]]) {
    const uint4 c = uint4(round(shade(in, draw, fog, tex, smp) * 255.0));
    const uint colour = c.r | c.g << 8 | c.b << 16 | c.a << 24;
    const uint2 position = uint2(in.position.xy);
    const uint pixel = position.y * lists.width + position.x;
    const uint depth = as_type<uint>(in.position.z > 0.0f ? in.position.z : 0.0f);
    if (atomic_load_explicit(&heads[pixel], memory_order_relaxed) == 0xFFFFFFFFu || depth < farthest[pixel])
        return float4(term(colour, draw.blend)) / 255.0;
    if (depth <= nearest[pixel])
        insert(colour, in.position.z, draw.order << 6 | draw.blend, position, lists, heads, nodes, used, slices);
    return float4(0.0);
}

// The fragments of adds() strips in front of every layer of their pixel's list, after the sort.
[[early_fragment_tests]] fragment float4 added_on_top(
    Varyings in [[stage_in]], constant Draw& draw [[buffer(0)]], constant Fog& fog [[buffer(1)]],
    constant Lists& lists [[buffer(2)]], device const uint* heads [[buffer(3)]],
    device const uint* nearest [[buffer(8)]], texture2d<float> tex [[texture(0)]], sampler smp [[sampler(0)]]) {
    const uint pixel = uint(in.position.y) * lists.width + uint(in.position.x);
    const uint depth = as_type<uint>(in.position.z > 0.0f ? in.position.z : 0.0f);
    if (heads[pixel] == 0xFFFFFFFFu || depth <= nearest[pixel])
        return float4(0.0);
    const uint4 c = uint4(round(shade(in, draw, fog, tex, smp) * 255.0));
    return float4(term(c.r | c.g << 8 | c.b << 16 | c.a << 24, draw.blend)) / 255.0;
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

// A pixel plus terms, clamped as each step clamps.
static float4 add_terms(float4 colour, uint4 terms) {
    return float4(min(uint4(round(colour * 255.0)) + terms, uint4(255))) / 255.0;
}

// Each pixel's translucent fragments, from the farthest (the smallest 1/w) to the nearest, the
// earlier strip first on a tie, each blended over what is there with its own factors. The
// framebuffer holds 8 bits a channel after each blend. Only the layers whose blend depends on the
// pixel are sorted: the adds() layers between two of them are summed, so an explosion of added
// sprites needs no sort. A pixel with more sorted layers than fit keeps the nearest ones.
fragment float4 sort_fragment(FullScreen in [[stage_in]], float4 dst [[color(0)]], constant Lists& lists [[buffer(2)]],
                              device const uint* heads [[buffer(3)]], device const Node* nodes [[buffer(4)]],
                              device atomic_uint* slices [[buffer(5)]]) {
    const uint pixel = uint(in.position.y) * lists.width + uint(in.position.x);
    // Each fragment is read once. Its order is one word, 1/w above the key, so that one compare sorts
    // by depth and then by the strip's place. A float that is not negative orders as its bits.
    ulong order[kMaxLayers];
    uint colours[kMaxLayers];
    uint count = 0, length = 0;
    uint4 sum = uint4(0);
    bool added = false;
    for (uint n = heads[pixel]; n != 0xFFFFFFFFu && n < lists.capacity;) {
        ++length;
        const Node node = nodes[n];
        n = node.next;
        const uint blend = node.key & 63;
        if (adds(blend)) {
            sum += term(node.colour, blend);
            added = true;
            continue;
        }
        const ulong o = ulong(as_type<uint>(node.depth > 0.0f ? node.depth : 0.0f)) << 32 | node.key;
        uint i = count;
        if (count == kMaxLayers) {
            // Full: drop the farthest unless this one is farther still.
            if (o < order[0])
                continue;
            for (uint j = 1; j < kMaxLayers; ++j) {
                order[j - 1] = order[j];
                colours[j - 1] = colours[j];
            }
            i = kMaxLayers - 1;
        }
        else
            ++count;
        for (; i > 0 && order[i - 1] >= o; --i) {
            order[i] = order[i - 1];
            colours[i] = colours[i - 1];
        }
        order[i] = o;
        colours[i] = node.colour;
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
    if (count == 0)
        return added ? add_terms(dst, sum) : dst;
    // Sorted layers and added ones: each added layer's term goes between the sorted layers that it
    // lies between, in a second walk of the list.
    uint4 gaps[kMaxLayers + 1];
    for (uint i = 0; i <= count; ++i) gaps[i] = uint4(0);
    if (added)
        for (uint n = heads[pixel]; n != 0xFFFFFFFFu && n < lists.capacity;) {
            const Node node = nodes[n];
            n = node.next;
            const uint blend = node.key & 63;
            if (!adds(blend))
                continue;
            const ulong o = ulong(as_type<uint>(node.depth > 0.0f ? node.depth : 0.0f)) << 32 | node.key;
            uint gap = 0;
            while (gap < count && order[gap] < o) ++gap;
            gaps[gap] += term(node.colour, blend);
        }
    float4 colour = add_terms(dst, gaps[0]);
    for (uint i = 0; i < count; ++i) {
        const uint c = colours[i];
        const float4 src = float4(c & 255, (c >> 8) & 255, (c >> 16) & 255, c >> 24) / 255.0;
        const uint blend = uint(order[i]) & 63;
        colour = clamp(src * factor(blend, src, colour, true) + colour * factor(blend >> 3, src, colour, false), 0.0, 1.0);
        colour = round(colour * 255.0) / 255.0;
        colour = add_terms(colour, gaps[i + 1]);
    }
    return colour;
}

// The window: a framebuffer, scaled into the drawable's 4:3 area. The same full-screen triangle
// draws the passes of the bloom.
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

// A pass over a picture: taps along each axis, `step` apart in the source's coordinates.
struct Pass {
    float2 step;
    int taps;
    float strength;  // the bloom's
};

// The mean of taps x taps bilinear samples around `uv`.
static float4 mean(texture2d<float> t, sampler smp, float2 uv, constant Pass& p) {
    const float2 first = uv - p.step * (float(p.taps - 1) * 0.5);
    float4 sum = float4(0.0);
    for (int y = 0; y < p.taps; ++y)
        for (int x = 0; x < p.taps; ++x) sum += t.sample(smp, first + p.step * float2(x, y));
    return sum / float(p.taps * p.taps);
}

fragment float4 present_fragment(Present in [[stage_in]], texture2d<float> frame [[texture(0)]],
                                 sampler smp [[sampler(0)]], constant Pass& p [[buffer(0)]]) {
    return mean(frame, smp, in.uv, p);
}

// The bloom's first pass: the frame averaged down to the glow's size, and of that only what is
// bright, fading in from a luminance of 0.85 to full at 1.
fragment float4 bloom_extract(Present in [[stage_in]], texture2d<float> frame [[texture(0)]],
                              sampler smp [[sampler(0)]], constant Pass& p [[buffer(0)]]) {
    const float3 c = mean(frame, smp, in.uv, p).rgb;
    const float luminance = dot(c, float3(0.2126, 0.7152, 0.0722));
    return float4(c * smoothstep(0.85, 1.0, luminance), 1.0);
}

// A Gaussian blur of 9 texels along `step`, in 5 bilinear samples.
fragment float4 bloom_blur(Present in [[stage_in]], texture2d<float> t [[texture(0)]], sampler smp [[sampler(0)]],
                           constant Pass& p [[buffer(0)]]) {
    float4 c = t.sample(smp, in.uv) * 0.2270270270;
    c += (t.sample(smp, in.uv + p.step * 1.3846153846) + t.sample(smp, in.uv - p.step * 1.3846153846)) * 0.3162162162;
    c += (t.sample(smp, in.uv + p.step * 3.2307692308) + t.sample(smp, in.uv - p.step * 3.2307692308)) * 0.0702702703;
    return c;
}

// The frame with both sizes of the blurred glow added.
fragment float4 bloom_composite(Present in [[stage_in]], texture2d<float> frame [[texture(0)]],
                                texture2d<float> near [[texture(1)]], texture2d<float> far [[texture(2)]],
                                sampler smp [[sampler(0)]], constant Pass& p [[buffer(0)]]) {
    const float3 c = frame.read(uint2(in.position.xy)).rgb;
    const float3 glow = near.sample(smp, in.uv).rgb + far.sample(smp, in.uv).rgb;
    return float4(min(c + glow * p.strength, 1.0), 1.0);
}
