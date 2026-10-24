// A frame as the renderer draws it: typed draws, whose fields say what the tile accelerator's words
// encode, and the frame's settings from the PowerVR's registers. translate() makes one from a
// decoded display list. Between translate() and Gpu::render(), a frame can be read and changed:
// a draw's blend, depth test, texture or vertices, or the frame's fog and palette.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "render/ta.h"
#include "render/texture.h"

namespace render {

// What a render reads besides the display list: the PowerVR's registers (0x2000 bytes from
// 0x005F8000, palette memory and the fog table included) and video memory in its 64-bit view.
struct Hardware {
    const uint32_t* registers = nullptr;
    const uint8_t* vram = nullptr;
    size_t vram_size = 0;
};

// The blend factors. The "other" colour is the target's for the source and the source's for the
// target.
enum class Blend : uint8_t {
    Zero, One, OtherColour, InverseOtherColour, SourceAlpha, InverseSourceAlpha, TargetAlpha, InverseTargetAlpha
};

// The depth test on 1/w, so that larger is nearer.
enum class DepthTest : uint8_t { Never, Less, Equal, LessOrEqual, Greater, NotEqual, GreaterOrEqual, Always };

// How the texture's colour and the vertex's colour combine.
enum class Shading : uint8_t { Decal, Modulate, DecalAlpha, ModulateAlpha };

// Fog by the fog table, by the vertex's offset alpha, none, or by the table's second mode.
enum class Fog : uint8_t { Table, Vertex, None, Table2 };

enum class Filter : uint8_t { Point, Bilinear, TrilinearA, TrilinearB };

// The user clip: none, drawn inside the rectangle, or drawn outside it.
enum class Clip : uint8_t { None, Inside = 2, Outside = 3 };

// One triangle strip.
struct Draw {
    List list = List::Opaque;
    uint32_t first = 0, count = 0;  // the strip's vertices in Frame::vertices
    bool gouraud = true;            // else each triangle takes its colours from its last vertex
    bool sprite = false;
    DepthTest depth = DepthTest::GreaterOrEqual;
    bool depth_write = true;
    Blend source = Blend::One, target = Blend::Zero;
    bool use_alpha = false, ignore_texture_alpha = false, offset_colour = false;
    Shading shading = Shading::Modulate;
    Fog fog = Fog::None;
    bool textured = false;
    // The texture, when `textured` and `texture_readable`: an encoding the decoder does not read
    // draws without it.
    TextureFormat texture;
    bool texture_readable = true;
    Filter filter = Filter::Point;
    bool clamp_u = false, clamp_v = false, flip_u = false, flip_v = false;
    float mipmap_bias = 0;  // the mipmap level's shift, in powers of two
    Clip clip = Clip::None;
    uint16_t clip_x0 = 0, clip_y0 = 0, clip_x1 = 0, clip_y1 = 0;  // right and bottom excluded
};

struct Frame {
    std::vector<Vertex> vertices;
    std::vector<Draw> draws;  // each list's strips in the order they came, lists in their numbers' order
    uint32_t target = 0;      // the frame buffer drawn into, an address in video memory's 32-bit view
    int clip_x0 = 0, clip_y0 = 0, clip_x1 = 640, clip_y1 = 480;  // right and bottom excluded
    uint32_t background = 0xFF000000u;  // ARGB, what the background plane fills the frame with
    float background_depth = 0;
    float max_z = 0;  // the largest 1/w of the vertices
    uint32_t fog_table_colour = 0, fog_vertex_colour = 0;  // RGB
    float fog_density = 0;
    uint8_t fog_table[128][2] = {};  // each entry's two fog values
    float alpha_ref = 0;             // the punch-through list's alpha threshold
    uint32_t palette[1024] = {};     // RGBA8
    uint32_t palette_format = 0;     // PAL_RAM_CTRL, with which palette textures were decoded
    uint32_t stride_texels = 0;      // TEXT_CONTROL's row length for textures in rows
};

// The typed frame of a decoded display list with the PowerVR's registers and video memory.
Frame translate(const DisplayList& list, const Hardware& hw);

}  // namespace render
