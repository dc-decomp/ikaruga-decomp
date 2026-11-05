// Each pixel's translucent fragments, from the farthest (the smallest 1/w) to the nearest, the
// earlier strip first on a tie, each blended over what is there with its own factors. The
// framebuffer holds 8 bits a channel after each blend. Only the layers whose blend depends on the
// pixel are sorted: the adds() layers between two of them are summed, so an explosion of added
// sprites needs no sort. A pixel with more sorted layers than fit keeps the nearest ones.
#version 450

layout(push_constant) uniform Sort {
    layout(offset = 0) uint width;
    layout(offset = 4) uint capacity;
} lists;

struct Node {
    uint colour;
    float depth;
    uint key;
    uint next;
};
layout(set = 0, binding = 0) buffer Heads { uint heads[]; };
layout(set = 0, binding = 1) buffer Nodes { Node nodes[]; };
layout(set = 0, binding = 2) buffer Slices { uint slices[]; };
layout(set = 0, binding = 3) uniform sampler2D dst_tex;

layout(location = 0) out vec4 out_colour;

const uint kMaxLayers = 64;

bool adds(uint blend) {
    const uint source = blend & 7u;
    return (blend >> 3) == 1 && (source == 0 || source == 1 || source == 4 || source == 5);
}

// The same order as Metal's one word, 1/w above the key: by depth, then by the strip's place.
bool before(uint depth_a, uint key_a, uint depth_b, uint key_b) {
    return depth_a < depth_b || (depth_a == depth_b && key_a < key_b);
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

vec4 factor(uint instruction, vec4 src, vec4 dst, bool source) {
    const vec4 other = source ? dst : src;
    const uint f = instruction & 7u;
    if (f == 0)
        return vec4(0.0);
    if (f == 1)
        return vec4(1.0);
    if (f == 2)
        return other;
    if (f == 3)
        return 1.0 - other;
    if (f == 4)
        return vec4(src.a);
    if (f == 5)
        return vec4(1.0 - src.a);
    if (f == 6)
        return vec4(dst.a);
    return vec4(1.0 - dst.a);
}

uvec4 add_terms(vec4 colour, uvec4 terms) {
    return uvec4(min(uvec4(round(colour * 255.0)) + terms, uvec4(255)));
}

void main() {
    const uvec2 pixel_xy = uvec2(gl_FragCoord.xy);
    const uint pixel = pixel_xy.y * lists.width + pixel_xy.x;
    vec4 dst = texelFetch(dst_tex, ivec2(gl_FragCoord.xy), 0);
    // Each fragment is read once. A layer is ordered by its depth and then by the strip's place.
    uint depths[kMaxLayers];
    uint keys[kMaxLayers];
    uint colours[kMaxLayers];
    uint count = 0, length = 0;
    uvec4 sum = uvec4(0);
    bool added = false;
    for (uint n = heads[pixel]; n != 0xFFFFFFFFu && n < lists.capacity;) {
        ++length;
        const Node node = nodes[n];
        n = node.next;
        const uint blend = node.key & 63u;
        if (adds(blend)) {
            sum += term(node.colour, blend);
            added = true;
            continue;
        }
        const uint d = floatBitsToUint(node.depth > 0.0 ? node.depth : 0.0);
        const uint k = node.key;
        uint i = count;
        if (count == kMaxLayers) {
            if (before(d, k, depths[0], keys[0]))
                continue;
            for (uint j = 1; j < kMaxLayers; ++j) {
                depths[j - 1] = depths[j];
                keys[j - 1] = keys[j];
                colours[j - 1] = colours[j];
            }
            i = kMaxLayers - 1;
        }
        else
            ++count;
        for (; i > 0 && !before(depths[i - 1], keys[i - 1], d, k); --i) {
            depths[i] = depths[i - 1];
            keys[i] = keys[i - 1];
            colours[i] = colours[i - 1];
        }
        depths[i] = d;
        keys[i] = k;
        colours[i] = node.colour;
    }
    if (length != 0)
        atomicAdd(slices[pixel_xy.y / 16u], length);
    if (count == 0) {
        out_colour = added ? vec4(add_terms(dst, sum)) / 255.0 : dst;
        return;
    }
    // Sorted layers and added ones: each added layer's term goes between the sorted layers that it
    // lies between, in a second walk of the list.
    uvec4 gaps[kMaxLayers + 1];
    for (uint i = 0; i <= count; ++i)
        gaps[i] = uvec4(0);
    if (added)
        for (uint n = heads[pixel]; n != 0xFFFFFFFFu && n < lists.capacity;) {
            const Node node = nodes[n];
            n = node.next;
            const uint blend = node.key & 63u;
            if (!adds(blend))
                continue;
            const uint d = floatBitsToUint(node.depth > 0.0 ? node.depth : 0.0);
            const uint k = node.key;
            uint gap = 0;
            while (gap < count && before(depths[gap], keys[gap], d, k))
                ++gap;
            gaps[gap] += term(node.colour, blend);
        }
    vec4 colour = vec4(add_terms(dst, gaps[0])) / 255.0;
    for (uint i = 0; i < count; ++i) {
        const uint c = colours[i];
        const vec4 src = vec4(c & 255u, (c >> 8) & 255u, (c >> 16) & 255u, c >> 24) / 255.0;
        const uint blend = keys[i] & 63u;
        colour = clamp(src * factor(blend, src, colour, true) + colour * factor(blend >> 3, src, colour, false),
                       0.0, 1.0);
        colour = round(colour * 255.0) / 255.0;
        colour = vec4(add_terms(colour, gaps[i + 1])) / 255.0;
    }
    out_colour = colour;
}
