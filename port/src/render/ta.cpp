#include "render/ta.h"

#include <algorithm>
#include <cstring>

namespace render {
namespace {

// The parameter types, in bits 31 to 29 of the parameter control word.
constexpr uint32_t kEndOfList = 0, kUserTileClip = 1, kObjectListSet = 2, kPolygon = 4, kSprite = 5, kVertex = 7;

// The object control bits of a header's parameter control word.
constexpr uint32_t kUv16 = 1u << 0, kOffset = 1u << 2, kTexture = 1u << 3, kTwoVolumes = 1u << 6;
constexpr uint32_t kEndOfStrip = 1u << 28;

float as_float(uint32_t w) {
    float f;
    std::memcpy(&f, &w, 4);
    return f;
}

// A colour component from a float, saturated to 0..1 and rounded to the nearest of 256 steps. No
// hardware test pins the rounding down; dream-recomp's decoder, the reference, rounds the same way.
uint32_t channel(float f) {
    if (!(f > 0.0f))
        return 0;
    if (f >= 1.0f)
        return 255;
    return static_cast<uint32_t>(f * 255.0f + 0.5f);
}

uint32_t argb(float a, float r, float g, float b) {
    return channel(a) << 24 | channel(r) << 16 | channel(g) << 8 | channel(b);
}

// A face colour (A, R, G, B) scaled by a vertex's intensity. The face colour is 8 bits a channel
// before the scaling, and the alpha is the face's.
uint32_t intensity(const float (&face)[4], float i) {
    auto scaled = [i](float c) { return channel(static_cast<float>(channel(c)) / 255.0f * i); };
    return channel(face[0]) << 24 | scaled(face[1]) << 16 | scaled(face[2]) << 8 | scaled(face[3]);
}

// 16-bit texture coordinates are the upper halves of two floats.
void uv16(uint32_t w, float& u, float& v) {
    u = as_float(w & 0xFFFF0000u);
    v = as_float(w << 16);
}

// The value at (x, y) of the plane through three points with values q: how the hardware fills the
// fourth corner of a sprite, which only bounds the quad.
float plane(const float (&x)[3], const float (&y)[3], const float (&q)[3], float px, float py) {
    const float dx1 = x[1] - x[0], dy1 = y[1] - y[0], dx2 = x[2] - x[0], dy2 = y[2] - y[0];
    const float det = dx1 * dy2 - dx2 * dy1;
    if (det == 0.0f)
        return q[0] + q[2] - q[1];
    const float dq1 = q[1] - q[0], dq2 = q[2] - q[0];
    const float a = (dq1 * dy2 - dq2 * dy1) / det, b = (dx1 * dq2 - dx2 * dq1) / det;
    return q[0] + a * (px - x[0]) + b * (py - y[0]);
}

}  // namespace

void TaDecoder::reset() {
    out_ = DisplayList{};
    list_ = kLists;
    kind_ = Kind::None;
    strip_ = Strip{};
    strip_start_ = 0;
    holding_ = false;
    any_z_ = false;
}

void TaDecoder::feed(const uint32_t* words, size_t count) {
    for (size_t i = 0; i + 8 <= count; i += 8) {
        const uint32_t* w = words + i;
        if (holding_) {
            holding_ = false;
            const uint32_t type = held_[0] >> 29;
            if (type == kVertex)
                vertex(held_, w);
            else
                header(held_, w);
            continue;
        }
        if (needs_second_half(w)) {
            std::memcpy(held_, w, sizeof held_);
            holding_ = true;
            continue;
        }
        parameter(w);
    }
}

// Which parameters are 64 bytes: an intensity header with an offset colour or two volumes, a
// textured vertex with float colours or two volumes, and every sprite or modifier volume vertex.
bool TaDecoder::needs_second_half(const uint32_t* w) const {
    const uint32_t type = w[0] >> 29;
    if (type == kPolygon) {
        const unsigned list = list_ < kLists ? list_ : (w[0] >> 24) & 7;
        if (list == static_cast<unsigned>(List::OpaqueModifier) || list == static_cast<unsigned>(List::TranslucentModifier))
            return false;
        const uint32_t obj = w[0], col = (obj >> 4) & 3;
        return col == 2 && (obj & (kOffset | kTwoVolumes));
    }
    if (type != kVertex)
        return false;
    if (kind_ == Kind::Sprite || kind_ == Kind::Modifier)
        return true;
    if (kind_ != Kind::Polygon)
        return false;
    const uint32_t obj = strip_.pcw, col = (obj >> 4) & 3;
    return (obj & kTexture) && (col == 1 || (obj & kTwoVolumes));
}

void TaDecoder::parameter(const uint32_t* w) {
    switch (w[0] >> 29) {
        case kEndOfList:
            end_strip();
            list_ = kLists;
            kind_ = Kind::None;
            return;
        case kUserTileClip:
            // In tiles of 32 pixels, the last one included.
            clip_[0] = (w[4] & 0x3F) * 32;
            clip_[1] = (w[5] & 0x1F) * 32;
            clip_[2] = ((w[6] & 0x3F) + 1) * 32;
            clip_[3] = ((w[7] & 0x1F) + 1) * 32;
            return;
        case kObjectListSet:
            if (list_ == kLists)
                list_ = (w[0] >> 24) & 7;
            return;
        case kPolygon:
        case kSprite:
            header(w, nullptr);
            return;
        case kVertex:
            vertex(w, nullptr);
            return;
        default:
            ++out_.unknown_parameters;
    }
}

void TaDecoder::header(const uint32_t* w, const uint32_t* second) {
    end_strip();
    if (list_ == kLists)
        list_ = (w[0] >> 24) & 7;
    if (list_ >= kLists) {
        ++out_.unknown_parameters;
        kind_ = Kind::None;
        return;
    }
    strip_ = Strip{};
    strip_.pcw = w[0];
    strip_.isp = w[1];
    if (list_ == static_cast<unsigned>(List::OpaqueModifier) || list_ == static_cast<unsigned>(List::TranslucentModifier)) {
        kind_ = Kind::Modifier;
        return;
    }
    strip_.tsp = w[2];
    strip_.tcw = w[3];
    strip_.clip_mode = (w[0] >> 16) & 3;
    strip_.clip_x0 = static_cast<uint16_t>(clip_[0]);
    strip_.clip_y0 = static_cast<uint16_t>(clip_[1]);
    strip_.clip_x1 = static_cast<uint16_t>(clip_[2]);
    strip_.clip_y1 = static_cast<uint16_t>(clip_[3]);
    if ((w[0] >> 29) == kSprite) {
        kind_ = Kind::Sprite;
        strip_.sprite = true;
        sprite_base_ = w[4];
        sprite_offset_ = w[5];
        return;
    }
    kind_ = Kind::Polygon;
    const uint32_t obj = w[0], col = (obj >> 4) & 3;
    if (obj & kTwoVolumes) {
        strip_.tsp1 = w[4];
        strip_.tcw1 = w[5];
    }
    if (col != 2)
        return;
    if (second) {
        // With an offset colour: the face colour and the face offset colour. With two volumes:
        // the face colours of the two volumes, of which the first is drawn.
        for (int i = 0; i < 4; ++i) face_[i] = as_float(second[i]);
        if (!(obj & kTwoVolumes))
            for (int i = 0; i < 4; ++i) face_offset_[i] = as_float(second[4 + i]);
    }
    else {
        for (int i = 0; i < 4; ++i) face_[i] = as_float(w[4 + i]);
    }
}

void TaDecoder::vertex(const uint32_t* w, const uint32_t* second) {
    if (kind_ == Kind::Sprite) {
        sprite_vertex(w, second);
        return;
    }
    if (kind_ == Kind::Modifier) {
        modifier_vertex(w, second);
        return;
    }
    if (kind_ != Kind::Polygon) {
        ++out_.unknown_parameters;
        return;
    }
    const uint32_t obj = strip_.pcw, col = (obj >> 4) & 3;
    const bool textured = obj & kTexture, two = obj & kTwoVolumes, short_uv = obj & kUv16;
    Vertex v;
    v.x = as_float(w[1]);
    v.y = as_float(w[2]);
    v.z = as_float(w[3]);
    auto coordinates = [&] {
        if (short_uv) {
            uv16(w[4], v.u, v.v);
        }
        else {
            v.u = as_float(w[4]);
            v.v = as_float(w[5]);
        }
    };
    if (!textured) {
        if (two)
            v.base = col == 0 ? w[4] : intensity(face_, as_float(w[4]));
        else if (col == 0)
            v.base = w[6];
        else if (col == 1)
            v.base = argb(as_float(w[4]), as_float(w[5]), as_float(w[6]), as_float(w[7]));
        else
            v.base = intensity(face_, as_float(w[6]));
    }
    else {
        coordinates();
        if (col == 1 && !two && second) {
            v.base = argb(as_float(second[0]), as_float(second[1]), as_float(second[2]), as_float(second[3]));
            v.offset = argb(as_float(second[4]), as_float(second[5]), as_float(second[6]), as_float(second[7]));
        }
        else if (col == 2 || col == 3) {
            v.base = intensity(face_, as_float(w[6]));
            v.offset = intensity(face_offset_, as_float(w[7]));
        }
        else {
            v.base = w[6];
            v.offset = w[7];
        }
    }
    if (!(obj & kOffset))
        v.offset = 0;
    add(v);
    if (w[0] & kEndOfStrip)
        end_strip();
}

// A sprite is a quad: corners A, B and C carry their coordinates, D only its position. It becomes
// a strip of four in the order the hardware draws it, D C A B, which decides its facing for culling.
// The offset colour applies to a textured sprite only.
void TaDecoder::sprite_vertex(const uint32_t* w, const uint32_t* second) {
    if (!second) {
        ++out_.unknown_parameters;
        return;
    }
    const float x[3] = {as_float(w[1]), as_float(w[4]), as_float(w[7])};
    const float y[3] = {as_float(w[2]), as_float(w[5]), as_float(second[0])};
    const float z[3] = {as_float(w[3]), as_float(w[6]), as_float(second[1])};
    const float dx = as_float(second[2]), dy = as_float(second[3]);
    float u[3] = {}, v[3] = {};
    const bool textured = strip_.pcw & kTexture;
    if (textured)
        for (int i = 0; i < 3; ++i) uv16(second[5 + i], u[i], v[i]);
    const uint32_t offset = textured ? sprite_offset_ : 0;
    auto corner = [&](float cx, float cy, float cz, float cu, float cv) {
        Vertex c;
        c.x = cx;
        c.y = cy;
        c.z = cz;
        c.u = cu;
        c.v = cv;
        c.base = sprite_base_;
        c.offset = offset;
        add(c);
    };
    corner(dx, dy, plane(x, y, z, dx, dy), textured ? plane(x, y, u, dx, dy) : 0.0f, textured ? plane(x, y, v, dx, dy) : 0.0f);
    corner(x[2], y[2], z[2], u[2], v[2]);
    corner(x[0], y[0], z[0], u[0], v[0]);
    corner(x[1], y[1], z[1], u[1], v[1]);
    end_strip();
}

void TaDecoder::modifier_vertex(const uint32_t* w, const uint32_t* second) {
    if (!second) {
        ++out_.unknown_parameters;
        return;
    }
    ModifierTriangle t;
    t.x[0] = as_float(w[1]);
    t.y[0] = as_float(w[2]);
    t.z[0] = as_float(w[3]);
    t.x[1] = as_float(w[4]);
    t.y[1] = as_float(w[5]);
    t.z[1] = as_float(w[6]);
    t.x[2] = as_float(w[7]);
    t.y[2] = as_float(second[0]);
    t.z[2] = as_float(second[1]);
    t.isp = strip_.isp;
    out_.modifiers.push_back(t);
}

void TaDecoder::add(const Vertex& v) {
    out_.vertices.push_back(v);
    if (!any_z_) {
        out_.min_z = out_.max_z = v.z;
        any_z_ = true;
    }
    out_.min_z = std::min(out_.min_z, v.z);
    out_.max_z = std::max(out_.max_z, v.z);
}

// A strip of fewer than three vertices draws nothing.
void TaDecoder::end_strip() {
    const auto size = static_cast<uint32_t>(out_.vertices.size());
    const uint32_t count = size - strip_start_;
    if ((kind_ == Kind::Polygon || kind_ == Kind::Sprite) && list_ < kLists && count >= 3) {
        Strip s = strip_;
        s.first = strip_start_;
        s.count = count;
        out_.lists[list_].push_back(s);
    }
    else if (count > 0) {
        ++out_.short_strips;
        out_.vertices.resize(strip_start_);
    }
    strip_start_ = static_cast<uint32_t>(out_.vertices.size());
}

}  // namespace render
