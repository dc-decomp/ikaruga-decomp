#include "render/draw.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace render {
namespace {

// PowerVR registers, as word offsets from 0x005F8000.
constexpr size_t kParamBase = 0x020 / 4, kFbWSof1 = 0x060 / 4, kFbXClip = 0x068 / 4, kFbYClip = 0x06C / 4;
constexpr size_t kIspBackgndD = 0x088 / 4, kIspBackgndT = 0x08C / 4;
constexpr size_t kFogColRam = 0x0B0 / 4, kFogColVert = 0x0B4 / 4, kFogDensity = 0x0B8 / 4;
constexpr size_t kTextControl = 0x0E4 / 4, kPalRamCtrl = 0x108 / 4, kPtAlphaRef = 0x11C / 4;
constexpr size_t kFogTable = 0x200 / 4, kPaletteRam = 0x1000 / 4;

float as_float(uint32_t w) {
    float f;
    std::memcpy(&f, &w, 4);
    return f;
}

// The 32-bit view of video memory interleaves its two 4 MB banks by word. The framebuffers and the
// parameter buffer are addressed in it.
uint32_t word32(const Hardware& hw, uint32_t a) {
    a &= 0x7FFFFF;
    const uint32_t o = ((a & 0x3FFFFC) << 1) | ((a >> 20) & 4) | (a & 3);
    if (o + 4 > hw.vram_size)
        return 0;
    uint32_t w;
    std::memcpy(&w, hw.vram + o, 4);
    return w;
}

Draw draw_of(const Strip& s, List list, uint32_t stride_texels) {
    Draw d;
    d.list = list;
    d.first = s.first;
    d.count = s.count;
    d.gouraud = s.pcw & 2;
    d.sprite = s.sprite;
    d.depth = static_cast<DepthTest>(s.isp >> 29);
    d.depth_write = !((s.isp >> 26) & 1);
    d.source = static_cast<Blend>(s.tsp >> 29);
    d.target = static_cast<Blend>((s.tsp >> 26) & 7);
    d.use_alpha = (s.tsp >> 20) & 1;
    d.ignore_texture_alpha = (s.tsp >> 19) & 1;
    d.offset_colour = (s.pcw >> 2) & 1;
    d.shading = static_cast<Shading>((s.tsp >> 6) & 3);
    d.fog = static_cast<Fog>((s.tsp >> 22) & 3);
    d.textured = s.pcw & 8;
    if (d.textured)
        d.texture_readable = describe_texture(s.tcw, s.tsp, stride_texels, d.texture);
    d.filter = static_cast<Filter>((s.tsp >> 13) & 3);
    d.clamp_u = (s.tsp >> 16) & 1;
    d.clamp_v = (s.tsp >> 15) & 1;
    d.flip_u = (s.tsp >> 18) & 1;
    d.flip_v = (s.tsp >> 17) & 1;
    // The mipmap D adjust counts in quarters, 4 being no change.
    const uint32_t adjust = (s.tsp >> 8) & 15;
    if (d.textured && (s.tcw >> 31) && adjust)
        d.mipmap_bias = std::log2(static_cast<float>(adjust) / 4.0f);
    d.clip = static_cast<Clip>(s.clip_mode);
    d.clip_x0 = s.clip_x0;
    d.clip_y0 = s.clip_y0;
    d.clip_x1 = s.clip_x1;
    d.clip_y1 = s.clip_y1;
    return d;
}

}  // namespace

Frame translate(const DisplayList& list, const Hardware& hw) {
    const uint32_t* regs = hw.registers;
    Frame f;
    f.vertices = list.vertices;
    f.stride_texels = (regs[kTextControl] & 31) * 32;
    for (unsigned n = 0; n < kLists; ++n)
        for (const Strip& s : list.lists[n]) f.draws.push_back(draw_of(s, static_cast<List>(n), f.stride_texels));
    f.target = regs[kFbWSof1] & 0xFFFFFF;
    const uint32_t xc = regs[kFbXClip], yc = regs[kFbYClip];
    f.clip_x0 = static_cast<int>(xc & 0x7FF);
    f.clip_x1 = static_cast<int>((xc >> 16) & 0x7FF) + 1;
    f.clip_y0 = static_cast<int>(yc & 0x3FF);
    f.clip_y1 = static_cast<int>((yc >> 16) & 0x3FF) + 1;
    // The background plane: an untextured one fills the screen with its colour at its depth.
    f.background_depth = std::max(as_float(regs[kIspBackgndD]), 0.0f);
    const uint32_t tag = regs[kIspBackgndT], skip = (tag >> 24) & 7;
    const uint32_t base = ((regs[kParamBase] & 0xF00000) + ((tag >> 3) & 0x1FFFFF) * 4) & 0x7FFFFF;
    if (!((word32(hw, base) >> 25) & 1) && skip >= 1)
        f.background = word32(hw, base + 12 + (tag & 7) * (3 + skip) * 4 + 12);
    f.max_z = list.max_z;
    f.fog_table_colour = regs[kFogColRam];
    f.fog_vertex_colour = regs[kFogColVert];
    // FOG_DENSITY: an 8-bit mantissa over 128, times 2 to a signed 8-bit exponent.
    const uint32_t density = regs[kFogDensity];
    f.fog_density = static_cast<float>((density >> 8) & 255) / 128.0f * std::ldexp(1.0f, static_cast<int8_t>(density & 255));
    for (int i = 0; i < 128; ++i) {
        f.fog_table[i][0] = static_cast<uint8_t>(regs[kFogTable + i] >> 8);
        f.fog_table[i][1] = static_cast<uint8_t>(regs[kFogTable + i]);
    }
    f.alpha_ref = static_cast<float>(regs[kPtAlphaRef] & 255) / 255.0f;
    f.palette_format = regs[kPalRamCtrl] & 3;
    unpack_palette(regs + kPaletteRam, regs[kPalRamCtrl], f.palette);
    return f;
}

}  // namespace render
