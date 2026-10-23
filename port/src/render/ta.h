// The tile accelerator's parameter stream, decoded into triangle strips: what the game hands the
// PowerVR for one render. Kamui and Ninja write the stream through the store queues and DMA in
// 32-byte parameters. A parameter is a polygon or sprite header, which sets the render state, a
// vertex, a modifier volume, a user clip rectangle or the end of a list. The decoder keeps the
// render state as the hardware's own words, and the renderer interprets them.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace render {

// The five lists, numbered as the hardware numbers them.
enum class List : unsigned { Opaque, OpaqueModifier, Translucent, TranslucentModifier, PunchThrough };
constexpr unsigned kLists = 5;

// A vertex in screen space: x and y in pixels, z is 1/w (larger is nearer). Colours are ARGB8888,
// whatever their encoding in the stream. Only the first volume of a two-volume polygon is kept.
struct Vertex {
    float x = 0, y = 0, z = 0;
    float u = 0, v = 0;
    uint32_t base = 0;
    uint32_t offset = 0;  // 0 when the polygon has no offset colour
};

// A triangle strip and the words that say how to draw it.
struct Strip {
    uint32_t first = 0, count = 0;  // the strip's vertices in DisplayList::vertices
    uint32_t pcw = 0;               // the header's parameter control word
    uint32_t isp = 0;               // ISP/TSP instruction word: depth compare, culling, depth write
    uint32_t tsp = 0;               // TSP instruction word: blending, fog, texture size and filter
    uint32_t tcw = 0;               // texture control word: format and address
    uint32_t tsp1 = 0xFFFFFFFFu, tcw1 = 0xFFFFFFFFu;  // the second volume's, if any
    // The user clip rectangle in force at the header, in pixels, right and bottom exclusive.
    // Mode 2 draws inside it, mode 3 outside, 0 ignores it.
    uint32_t clip_mode = 0;
    uint16_t clip_x0 = 0, clip_y0 = 0, clip_x1 = 0, clip_y1 = 0;
    bool sprite = false;
};

// A modifier volume triangle. Ikaruga sends some, but no polygon it draws is affected by them.
struct ModifierTriangle {
    float x[3] = {}, y[3] = {}, z[3] = {};
    uint32_t isp = 0;
};

struct DisplayList {
    std::vector<Vertex> vertices;
    std::array<std::vector<Strip>, kLists> lists;
    std::vector<ModifierTriangle> modifiers;
    float min_z = 0, max_z = 0;  // the range of 1/w over every vertex
    uint32_t unknown_parameters = 0, short_strips = 0;
};

// Decodes the stream of one render. Feed it the parameters in the order the FIFO got them.
class TaDecoder {
public:
    void reset();
    void feed(const uint32_t* words, size_t count);
    const DisplayList& list() const { return out_; }

private:
    void parameter(const uint32_t* w);
    void header(const uint32_t* w, const uint32_t* second);
    void vertex(const uint32_t* w, const uint32_t* second);
    void sprite_vertex(const uint32_t* w, const uint32_t* second);
    void modifier_vertex(const uint32_t* w, const uint32_t* second);
    bool needs_second_half(const uint32_t* w) const;
    void end_strip();
    void add(const Vertex& v);

    DisplayList out_;
    unsigned list_ = kLists;  // the open list, kLists when none is open
    enum class Kind { None, Polygon, Sprite, Modifier } kind_ = Kind::None;
    Strip strip_{};
    uint32_t strip_start_ = 0;
    // The face colours of an intensity polygon. Colour type 3 keeps the last header's.
    float face_[4] = {1, 1, 1, 1}, face_offset_[4] = {0, 0, 0, 0};
    uint32_t sprite_base_ = 0, sprite_offset_ = 0;
    uint32_t clip_[4] = {};  // the user clip rectangle in pixels, empty until a clip parameter
    uint32_t held_[8] = {};  // the first half of a 64-byte parameter
    bool holding_ = false;
    bool any_z_ = false;
};

}  // namespace render
