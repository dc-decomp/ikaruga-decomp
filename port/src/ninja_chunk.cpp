// Ninja's chunk models, natively, as far as they go. Each function computes what its machine code
// computes, in the same order and with the same roundings, and writes the same memory through the
// same store-queue blocks. IKARUGA_DIFF tests each against its machine code on the game's own calls.
#include <cmath>
#include <cstdint>
#include <cstring>
#include <initializer_list>

#include "decomp.h"
#include "diff.h"
#include "hw.h"

namespace game {
namespace {

// Element n of the current matrix, which Ninja keeps in the FPU's back bank.
float X(int n) { return cpu::xf[n ^ 1]; }

uint32_t word(uint32_t a) { return guest::load<uint32_t>(a); }
float real(uint32_t a) { return guest::load<float>(a); }

struct Vec4 {
    float x, y, z, w;
};

// ftrv: the current matrix times (x, y, z, 1), each row's products summed in double and rounded
// once, as the interpreter computes it.
Vec4 transform(float x, float y, float z) {
    float out[4];
    for (int i = 0; i < 4; ++i)
        out[i] = static_cast<float>(X(i) * static_cast<double>(x) + X(4 + i) * static_cast<double>(y) +
                                    X(8 + i) * static_cast<double>(z) + X(12 + i) * 1.0);
    return {out[0], out[1], out[2], out[3]};
}

// ftrv for a direction: the current matrix times (x, y, z, 0).
Vec4 rotate(float x, float y, float z) {
    float out[4];
    for (int i = 0; i < 4; ++i)
        out[i] = static_cast<float>(X(i) * static_cast<double>(x) + X(4 + i) * static_cast<double>(y) +
                                    X(8 + i) * static_cast<double>(z) + X(12 + i) * 0.0);
    return {out[0], out[1], out[2], out[3]};
}

// fipr: the products summed in double from 0, rounded once.
float fipr(float a0, float a1, float a2, float a3, float b0, float b1, float b2, float b3) {
    double sum = 0;
    sum += static_cast<double>(a0) * b0;
    sum += static_cast<double>(a1) * b1;
    sum += static_cast<double>(a2) * b2;
    sum += static_cast<double>(a3) * b3;
    return static_cast<float>(sum);
}

// The vertex context that the vertex list fills for each chunk: the light's direction (0, 4, 8), its
// intensity (12) and the ambient light (16), then the chunk's vertex count (20), its data (24), its
// place in the vertex buffer (28) and that place's store-queue address (32).
constexpr uint32_t kCount = 20, kData = 24, kOut = 32;
// The projection to the screen: the x and y scale, then the x and y offset.
constexpr uint32_t kProjection = 0x8C10AF34u;

// The vertex buffer: 16 bytes for each vertex, the screen's x and y, 1/w and the colour or the
// light. They go through the store queues, which are sent each time two vertices fill one, and
// once more after an odd count.
struct VertexOut {
    uint32_t at, written = 0;
    void put(float x, float y, float q, uint32_t fourth) {
        guest::store<float>(at, x);
        guest::store<float>(at + 4, y);
        guest::store<float>(at + 8, q);
        guest::store<uint32_t>(at + 12, fourth);
        if (++written % 2 == 0)
            hw::prefetch(at);
        at += 16;
    }
    void end() {
        if (written % 2)
            hw::prefetch(at);
    }
};

uint32_t bits_of(float f) {
    uint32_t b;
    std::memcpy(&b, &f, 4);
    return b;
}

// The vertices with a colour each: x, y, z, then the normal if `normals`, then the colour. Returns
// the vertices that are not more than 1 in front of the camera, which the strips clip.
uint32_t vertices_with_colour(uint32_t context, bool normals) {
    uint32_t count = word(context + kCount), data = word(context + kData);
    VertexOut out{word(context + kOut)};
    const float sx = real(kProjection), sy = real(kProjection + 4), ox = real(kProjection + 8),
                oy = real(kProjection + 12);
    uint32_t near = 0;
    do {
        const Vec4 t = transform(real(data), real(data + 4), real(data + 8));
        const uint32_t colour = word(data + (normals ? 24 : 12));
        data += normals ? 28 : 16;
        const float depth = -t.z, q = 1.0f / depth;
        if (!(depth > 1.0f))
            ++near;
        out.put(std::fma(q, t.x * sx, ox), std::fma(q, t.y * sy, oy), q, colour);
    } while (--count != 0);
    out.end();
    return near;
}

float from_bits(uint32_t b) {
    float f;
    std::memcpy(&f, &b, 4);
    return f;
}

// ftrc: towards zero, and the SH-4's saturated values out of range.
uint32_t ftrc(float f) {
    if (std::isnan(f))
        return 0x80000000u;
    if (f >= 2147483648.0f)
        return 0x7FFFFFFFu;
    if (f < -2147483648.0f)
        return 0x80000000u;
    return static_cast<uint32_t>(static_cast<int32_t>(f));
}

// Where a vertex lies off the 640 x 480 screen: left (1), right (2), above (4), below (8). A vertex
// behind the camera (1/w below 0) counts as beyond the edges opposite its position.
uint32_t outcode(float x, float y, float q) {
    if (0.0f > q)
        return (x > 0.0f ? 1u : 2u) | (y > 0.0f ? 4u : 8u);
    uint32_t code = 0;
    if (0.0f > x)
        code |= 1;
    if (x > 640.0f)
        code |= 2;
    if (0.0f > y)
        code |= 4;
    if (y > 480.0f)
        code |= 8;
    return code;
}

// The colour `t` of the way from `from` to `to`, each byte interpolated on its own.
uint32_t mix_colour(uint32_t from, uint32_t to, float t) {
    uint32_t result = 0;
    for (int shift = 24; shift >= 0; shift -= 8) {
        const float a = static_cast<float>((from >> shift) & 0xFF), b = static_cast<float>((to >> shift) & 0xFF);
        float c = std::fma(t, b - a, a);
        if (!(2147483648.0f > c))
            c -= 4294967296.0f;
        result |= ftrc(c) << shift;
    }
    return result;
}

// Calls a strip function with its registers: the strip data, the vertex buffer, the strip count and
// where the vertices go in r4 to r7, and the floats from fr4 on.
uint32_t strip_function(uint32_t function, uint32_t data, uint32_t buffer, uint32_t strips, uint32_t out,
                        std::initializer_list<float> fr) {
    CallArgs args;
    args.r[0] = data;
    args.r[1] = buffer;
    args.r[2] = strips;
    args.r[3] = out;
    args.nr = 4;
    for (const float f : fr) args.fr[args.nfr++] = f;
    return guest::call(function, args).r0;
}

// Calls a function that a table gives with `a` in r4.
uint32_t call_with(uint32_t function, uint32_t a) {
    CallArgs args;
    args.r[0] = a;
    args.nr = 1;
    return guest::call(function, args).r0;
}

// The parameter control words of a strip's vertices: the last one ends the strip.
constexpr uint32_t kVertex = 0xE0000000u, kLastVertex = 0xF0000000u;

// One vertex to the tile accelerator through the store queues: 32 bytes, sent at once.
void send(uint32_t at, uint32_t control, uint32_t x, uint32_t y, uint32_t q, float u, float v, uint32_t base,
          uint32_t offset) {
    const uint32_t words[8] = {control, x, y, q, bits_of(u), bits_of(v), base, offset};
    for (uint32_t i = 0; i < 8; ++i) guest::store<uint32_t>(at + 4 * i, words[i]);
    hw::prefetch(at);
}

}  // namespace

// Vertices with a colour: CV_D8 chunks.
int njCnkEasyVertexVD8(int param_1) {
    diff::Check check("njCnkEasyVertexVD8", 0x8C019890u, param_1);
    return check.returns(static_cast<int>(vertices_with_colour(static_cast<uint32_t>(param_1), false)));
}

// Vertices with a normal and a colour: CV_VN_D8 chunks. The normal is not used.
int njCnkEasyVertexVND8(int param_1) {
    diff::Check check("njCnkEasyVertexVND8", 0x8C0199D4u, param_1);
    return check.returns(static_cast<int>(vertices_with_colour(static_cast<uint32_t>(param_1), true)));
}

// Vertices with a normal: CV_VN chunks, lit by the easy light. The light's direction goes into
// the model's frame through the matrix's first three columns and is scaled by its intensity. A
// vertex's light is its normal times that, plus the ambient light, and never less than the
// ambient light. The decompiler took the FPU registers it saves for a second parameter.
int njCnkEasyVertexVN(gptr<float> param_1, ulonglong) {
    diff::Check check("njCnkEasyVertexVN", 0x8C019B60u, param_1);
    const uint32_t context = param_1.a;
    const float lx = real(context), ly = real(context + 4), lz = real(context + 8);
    const float intensity = real(context + 12), ambient = real(context + 16);
    const float mx = fipr(X(0), X(1), X(2), X(3), lx, ly, lz, 0.0f) * intensity;
    const float my = fipr(X(4), X(5), X(6), X(7), lx, ly, lz, 0.0f) * intensity;
    const float mz = fipr(X(8), X(9), X(10), X(11), lx, ly, lz, 0.0f) * intensity;
    uint32_t count = word(context + kCount), data = word(context + kData);
    VertexOut out{word(context + kOut)};
    const float sx = real(kProjection), sy = real(kProjection + 4), ox = real(kProjection + 8),
                oy = real(kProjection + 12);
    uint32_t near = 0;
    do {
        const Vec4 t = transform(real(data), real(data + 4), real(data + 8));
        float light = fipr(real(data + 12), real(data + 16), real(data + 20), 1.0f, mx, my, mz, ambient);
        data += 24;
        const float depth = -t.z, q = 1.0f / depth;
        if (!(light > ambient))
            light = ambient;
        if (!(depth > 1.0f))
            ++near;
        out.put(std::fma(q, t.x * sx, ox), std::fma(q, t.y * sy, oy), q, bits_of(light));
    } while (--count != 0);
    out.end();
    return check.returns(static_cast<int>(near));
}

// Textured strips of the vertex buffer, for a model that the near plane does not cut: the strip
// chunks with u and v, scaled by `param_5` (1/256 or 1/1024). The strips' count is in the low 14 bits
// of `param_3`, and above them the number of user words after each vertex from the third on. Each
// vertex goes to the tile accelerator at `param_4` as 32 bytes: its parameter word, the screen
// position and 1/w from the vertex buffer, u and v, then the buffer's fourth word as the base
// intensity and that word as a float less 1 as the offset intensity. Returns where the next
// vertex goes.
gptr<float> njCnkEasyTexture(gptr<short> param_1, int param_2, uint param_3, gptr<float> param_4, float param_5,
                             float, float, float, float, float) {
    diff::Check check("njCnkEasyTexture", 0x8C0196FCu, param_1, param_2, param_3, param_4, param_5);
    uint32_t in = param_1.a, out = param_4.a, strips = param_3 & 0x3FFF;
    const uint32_t buffer = static_cast<uint32_t>(param_2), user = ((param_3 >> 14) & 3) * 2;
    const auto next = [&in] {
        const int32_t v = guest::load<int16_t>(in);
        in += 2;
        return v;
    };
    uint32_t total = 0;
    do {
        // The sign of a strip's length is the side it faces, which the hardware's culling decides.
        int32_t length = next();
        if (length < 0)
            length = -length;
        total += static_cast<uint32_t>(length);
        for (int32_t k = 0; k < length; ++k) {
            const uint32_t vertex = buffer + (static_cast<uint32_t>(next()) << 4);
            const float u = static_cast<float>(next()) * param_5;
            const float v = static_cast<float>(next()) * param_5;
            if (k >= 2)
                in += user;
            const uint32_t light = word(vertex + 12);
            send(out, k == length - 1 ? kLastVertex : kVertex, word(vertex), word(vertex + 4), word(vertex + 8), u, v,
                 light, bits_of(from_bits(light) - 1.0f));
            out += 32;
        }
    } while (--strips != 0);
    guest::store<uint32_t>(0x8C10AFB0u, word(0x8C10AFB0u) + total);
    guest::store<uint32_t>(0x8C10AFACu, word(0x8C10AFACu) + total);
    return check.returns(gptr<float>(out));
}

// Textured strips for a model that the near plane may cut, with the arguments of
// njCnkEasyTexture. A triangle whose three vertices have 1/w in (0, 1] goes to the tile
// accelerator. A triangle with a vertex in front of the near plane, 1/w in (0, 1), but not all
// three in (0, 1], goes to the near clip, which sends what remains of it. The other triangles are
// left out, and the strips break around the triangles that are clipped or left out. A vertex's
// 1/w is taken when its triangle starts, its position and light when it is sent: the near clip
// writes the vertex buffer.
gptr<dword> njCnkEasyTextureClip(gptr<short> param_1, int param_2, uint param_3, gptr<dword> param_4, float arg_fr4,
                                 float, float, float, float, float) {
    diff::Check check("njCnkEasyTextureClip", 0x8C018F1Cu, param_1, param_2, param_3, param_4, arg_fr4);
    uint32_t in = param_1.a, out = param_4.a, strips = param_3 & 0x3FFF;
    const uint32_t buffer = static_cast<uint32_t>(param_2), user = ((param_3 >> 14) & 3) * 2;
    struct Corner {
        uint32_t at;
        float u, v, q;
    };
    const auto next = [&in] {
        const int32_t v = guest::load<int16_t>(in);
        in += 2;
        return v;
    };
    const auto read = [&] {
        Corner c;
        c.at = buffer + (static_cast<uint32_t>(next()) << 4);
        c.u = static_cast<float>(next()) * arg_fr4;
        c.v = static_cast<float>(next()) * arg_fr4;
        c.q = real(c.at + 8);
        return c;
    };
    const auto emit = [&out](const Corner& c, uint32_t control) {
        const uint32_t light = word(c.at + 12);
        send(out, control, word(c.at), word(c.at + 4), bits_of(c.q), c.u, c.v, light,
             bits_of(from_bits(light) - 1.0f));
        out += 32;
    };
    const auto ahead = [](float q) { return q > 0.0f && 1.0f > q; };
    uint32_t sent = 0, total = 0;
    do {
        int32_t length = next();
        if (length < 0)
            length = -length;
        total += static_cast<uint32_t>(length);
        Corner a = read(), b = read();
        uint32_t open = 0;  // the vertices of the strip being sent, but its last
        uint32_t triangles = static_cast<uint32_t>(length - 2);
        do {
            const Corner c = read();
            in += user;
            if (a.q > 0.0f && b.q > 0.0f && c.q > 0.0f && !(a.q > 1.0f) && !(b.q > 1.0f) && !(c.q > 1.0f)) {
                if (open == 0) {
                    emit(a, kVertex);
                    ++open;
                }
                emit(b, kVertex);
                ++open;
            }
            else {
                if (open) {
                    emit(b, kLastVertex);
                    sent += open + 1;
                    open = 0;
                }
                if (ahead(a.q) || ahead(b.q) || ahead(c.q))
                    out = njCnkEasyTextureNearClip(gptr<float>(a.at), gptr<float>(b.at), gptr<float>(c.at),
                                                   gptr<dword>(out), a.u, a.v, b.u, b.v, c.u, c.v)
                              .a;
            }
            a = b;
            b = c;
        } while (--triangles != 0);
        if (open) {
            emit(b, kLastVertex);
            sent += open + 1;
        }
    } while (--strips != 0);
    guest::store<uint32_t>(0x8C10AFB0u, word(0x8C10AFB0u) + sent);
    guest::store<uint32_t>(0x8C10AFACu, word(0x8C10AFACu) + total);
    return check.returns(gptr<dword>(out));
}

namespace {

// The light of the multi-light models' lights for a vertex at `p` with the normal `n`. The lights'
// bits are at `lights`, three for each light: lit, a point light, the last record; the records
// follow from 16 on, 32 bytes each. A directional light (its direction, then its colour at 20) adds
// its colour times the normal's product with its direction, when that is negative. A point light
// (its position, its reach squared at 12, a distance squared at 16, its colour at 20) adds its
// colour when the vertex is within reach and faces it, times the normal's product with the
// direction to it, over the distance, and beyond the distance at 16 also times that distance over
// the distance squared. `fr7` is the FPU register whose product with zero ends a directional
// light's product: what the vertex function left there, or the last point light's distance
// squared.
struct Light {
    float r = 0.0f, g = 0.0f, b = 0.0f;
};
Light multi_light(const Vec4& p, const Vec4& n, uint32_t lights, float& fr7) {
    Light l;
    uint32_t mask = word(lights), light = lights + 16;
    const auto bit = [&mask] {
        const bool t = mask & 1;
        mask = (mask >> 1) | (mask << 31);
        return t;
    };
    for (;;) {
        const bool lit = bit(), point = bit();
        bool last;
        if (!lit) {
            last = bit();
        }
        else if (!point) {
            const float dot = fipr(real(light), real(light + 4), real(light + 8), fr7, n.x, n.y, n.z, 0.0f);
            fr7 = 0.0f;
            last = bit();
            if (0.0f > dot) {
                const float k = -dot;
                l.r = std::fma(k, real(light + 20), l.r);
                l.g = std::fma(k, real(light + 24), l.g);
                l.b = std::fma(k, real(light + 28), l.b);
            }
        }
        else {
            const float dx = real(light) - p.x, dy = real(light + 4) - p.y, dz = real(light + 8) - p.z;
            const float dot = fipr(dx, dy, dz, 0.0f, n.x, n.y, n.z, 0.0f);
            const float reach = real(light + 12);
            fr7 = fipr(dx, dy, dz, 0.0f, dx, dy, dz, 0.0f);
            const float inverse = 1.0f / std::sqrt(fr7), distance = real(light + 16);
            last = bit();
            if (dot > 0.0f && reach > fr7) {
                float k = dot * inverse;
                const float inverse_squared = inverse * inverse;
                if (fr7 > distance) {
                    k = k * distance;
                    k = k * inverse_squared;
                }
                l.r = std::fma(k, real(light + 20), l.r);
                l.g = std::fma(k, real(light + 24), l.g);
                l.b = std::fma(k, real(light + 28), l.b);
            }
        }
        light += 32;
        if (last)
            return l;
    }
}

}  // namespace

// Vertices with a normal, lit by the lights at 0x8C122710 (see multi_light): CV_VN chunks of the
// multi-light models. Each vertex goes to the buffer as 32 bytes: -z, the screen position, 1/w, 1
// and the light's red, green and blue. Returns the vertices less than 1 in front of the camera.
int njCnkEasyMultiVertexVN(gptr<int> param_1, gptr<float> param_2, float) {
    diff::Check check("njCnkEasyMultiVertexVN", 0x8C01BE98u, param_1, param_2);
    const uint32_t context = param_1.a, projection = param_2.a;
    uint32_t count = word(context), data = word(context + 4), out = word(context + 12);
    float fr7 = 0.0f;
    int near = 0;
    do {
        const Vec4 p = transform(real(data), real(data + 4), real(data + 8));
        const Vec4 n = rotate(real(data + 12), real(data + 16), real(data + 20));
        data += 24;
        const Light l = multi_light(p, n, 0x8C122710u, fr7);
        const float depth = -p.z;
        if (1.0f > depth)
            ++near;
        const float q = 1.0f / depth;
        const float x = std::fma(q, real(projection) * p.x, real(projection + 8));
        const float y = std::fma(q, real(projection + 4) * p.y, real(projection + 12));
        const float words[8] = {depth, x, y, q, 1.0f, l.r, l.g, l.b};
        for (uint32_t i = 0; i < 8; ++i) guest::store<float>(out + 4 * i, words[i]);
        hw::prefetch(out);
        out += 32;
    } while (--count != 0);
    return check.returns(near);
}

// Textured strips of the multi-light vertex buffer, with the arguments of njCnkEasyTexture but for
// the material: alpha in arg_fr4, red, green and blue in arg_fr5 to arg_fr7, and the u and v
// scale in param_9. Each vertex goes to the tile accelerator as 64 bytes. The first 32 are its
// parameter word, the screen position and 1/w, u and v; their last two words are not written and
// keep what the store queue held. The second 32 are the base colour and the offset colour, each
// alpha first: alpha is the buffer's fifth word times the material's alpha, the offset's red,
// green and blue are the light's plus the material's less 1, and the base's are those plus 1.
gptr<ulonglong> njCnkEasyMultiTexture(gptr<short> param_1, int param_2, uint param_3, gptr<ulonglong> param_4,
                                      float arg_fr4, float arg_fr5, float arg_fr6, float arg_fr7, float param_9) {
    diff::Check check("njCnkEasyMultiTexture", 0x8C01B500u, param_1, param_2, param_3, param_4, arg_fr4, arg_fr5,
                      arg_fr6, arg_fr7, param_9);
    uint32_t in = param_1.a, out = param_4.a, strips = param_3 & 0x3FFF;
    const uint32_t buffer = static_cast<uint32_t>(param_2), user = ((param_3 >> 14) & 3) * 2;
    const float red = arg_fr5 - 1.0f, green = arg_fr6 - 1.0f, blue = arg_fr7 - 1.0f;
    const auto next = [&in] {
        const int32_t v = guest::load<int16_t>(in);
        in += 2;
        return v;
    };
    uint32_t total = 0;
    do {
        int32_t length = next();
        if (length < 0)
            length = -length;
        total += static_cast<uint32_t>(length);
        for (int32_t k = 0; k < length; ++k) {
            const uint32_t vertex = buffer + (static_cast<uint32_t>(next()) << 5);
            const float u = static_cast<float>(next()) * param_9;
            const float v = static_cast<float>(next()) * param_9;
            if (k >= 2)
                in += user;
            guest::store<uint32_t>(out, k == length - 1 ? kLastVertex : kVertex);
            guest::store<uint32_t>(out + 4, word(vertex + 4));
            guest::store<uint32_t>(out + 8, word(vertex + 8));
            guest::store<uint32_t>(out + 12, word(vertex + 12));
            guest::store<float>(out + 16, u);
            guest::store<float>(out + 20, v);
            hw::prefetch(out);
            out += 32;
            const float r = real(vertex + 20) + red, g = real(vertex + 24) + green, b = real(vertex + 28) + blue;
            const float alpha = real(vertex + 16) * arg_fr4;
            const float words[8] = {alpha, r + 1.0f, g + 1.0f, b + 1.0f, alpha, r, g, b};
            for (uint32_t i = 0; i < 8; ++i) guest::store<float>(out + 4 * i, words[i]);
            hw::prefetch(out);
            out += 32;
        }
    } while (--strips != 0);
    guest::store<uint32_t>(0x8C10AFB0u, word(0x8C10AFB0u) + total);
    guest::store<uint32_t>(0x8C10AFACu, word(0x8C10AFACu) + total);
    return check.returns(gptr<ulonglong>(out));
}

// Untextured strips of a multi-light model: each vertex's screen position and 1/w from the vertex
// buffer (32 bytes for each vertex), its alpha times `param_1`, and its light's red, green and blue
// times `param_2` to `param_4` plus `param_5` to `param_7`.
gptr<ulonglong> njCnkEasyMultiPolygon(float param_1, float param_2, float param_3, float param_4, float param_5,
                                      float param_6, float param_7, gptr<short> param_8, int param_9, uint param_10,
                                      gptr<ulonglong> param_11) {
    diff::Check check("njCnkEasyMultiPolygon", 0x8C01AB8Cu, param_1, param_2, param_3, param_4, param_5, param_6,
                      param_7, param_8, param_9, param_10, param_11);
    uint32_t in = param_8.a, out = param_11.a, strips = param_10 & 0x3FFF;
    const uint32_t buffer = static_cast<uint32_t>(param_9), user = ((param_10 >> 14) & 3) * 2;
    const auto next = [&in] {
        const int32_t v = guest::load<int16_t>(in);
        in += 2;
        return v;
    };
    uint32_t total = 0;
    do {
        int32_t length = next();
        if (length < 0)
            length = -length;
        total += static_cast<uint32_t>(length);
        for (int32_t k = 0; k < length; ++k) {
            const uint32_t vertex = buffer + (static_cast<uint32_t>(next()) << 5);
            if (k >= 2)
                in += user;
            const uint32_t head[4] = {k == length - 1 ? kLastVertex : kVertex, word(vertex + 4), word(vertex + 8),
                                      word(vertex + 12)};
            for (uint32_t i = 0; i < 4; ++i) guest::store<uint32_t>(out + 4 * i, head[i]);
            const float colour[4] = {real(vertex + 16) * param_1, real(vertex + 20) * param_2 + param_5,
                                     real(vertex + 24) * param_3 + param_6, real(vertex + 28) * param_4 + param_7};
            for (uint32_t i = 0; i < 4; ++i) guest::store<float>(out + 16 + 4 * i, colour[i]);
            hw::prefetch(out);
            out += 32;
        }
    } while (--strips != 0);
    guest::store<uint32_t>(0x8C10AFB0u, word(0x8C10AFB0u) + total);
    guest::store<uint32_t>(0x8C10AFACu, word(0x8C10AFACu) + total);
    return check.returns(gptr<ulonglong>(out));
}

// The texture of a chunk model's texture chunk. `param_2` holds its number in the current texture
// list (bits 0-12), super sampling (13) and the filter (14-15); `param_1` the chunk's head bits,
// the mipmap D adjust (8-11) and the flip and clamp of u and v (12-15). They go into the TSP word
// (at 8) and the texture control word (at 12) of the strip context that 0x8C10B0F4 points to,
// with the texture's size from its memory entry. Returns the top half of a word of the texture's
// information.
uint njSetkmCnkTextureNum(uint param_1, uint param_2) {
    diff::Check check("njSetkmCnkTextureNum", 0x8C012044u, param_1, param_2);
    const uint32_t context = word(0x8C10B0F4u);
    const uint32_t memory = word(word(word(context + 40)) + (param_2 & 0x1FFF) * 12 + 8);
    guest::store<uint32_t>(context + 8, (word(context + 8) & 0xFFFFFFC0u) | (word(memory) & 63));
    guest::store<uint32_t>(context + 12, word(memory + 4));
    guest::store<uint32_t>(context + 8, (word(context + 8) & 0xFFF800FFu) | ((param_1 & 0xF000) << 3) |
                                            (param_1 & 0x0F00) | ((param_2 & 0x2000) >> 1) |
                                            ((param_2 & 0xC000) >> 1));
    return check.returns(word(word(memory + 12) + 4) >> 16);
}

// The colour mode of the strips that follow. Bits 0-7 and 25 of `param_1` go into the first word
// of the strip context that 0x8C10B0F4 points to (the parameter control word), bit 20 into its TSP
// word, and the whole mode to 24. The control word's bit 7, the offset colour, is set when Ninja's
// flags at 0x8C10AF98 have bit 10, or bit 12 while the mode has neither bit 20 nor bit 25, and is
// cleared otherwise.
void njSetkmColorMode(uint param_1) {
    diff::Check check("njSetkmColorMode", 0x8C011F24u, param_1);
    const uint32_t context = word(0x8C10B0F4u), flags = word(0x8C10AF98u);
    uint32_t control = (word(context) & 0xFDFFFF00u) | (param_1 & 0x020000FFu);
    const uint32_t tsp = (word(context + 8) & 0xFFEFFFFFu) | (param_1 & 0x00100000u);
    if ((flags & 0x400) || ((flags & 0x1000) && !(param_1 & 0x02100000u)))
        control |= 0x80;
    else
        control &= 0xFFFFFF7Fu;
    guest::store<uint32_t>(context, control);
    guest::store<uint32_t>(context + 8, tsp);
    guest::store<uint32_t>(context + 24, param_1);
}

// The polygon list of an Easy model, its chunks up to the end chunk. `param_2` is the type of the
// vertex list's last chunk (35, a colour for each vertex, means no light), `param_3` the vertices
// that the vertex list found not more than 1 in front of the camera, and `param_4` the vertex
// buffer. When all vertices are that near, no strip is drawn; when some are, the strips go
// through the clipping functions.
void njCnkEasyPlist(gptr<ushort> param_1, int param_2, int param_3, int param_4, float, float, float, float, float,
                  float, float, float) {
    diff::Check check("njCnkEasyPlist", 0x8C019CD0u, param_1, param_2, param_3, param_4);
    // The cached polygon lists and their count, the bump vectors and whether a bump chunk set
    // them, the material's colour and the light's.
    constexpr uint32_t kCache = 0x8C12290Cu, kCached = 0x8C122908u, kBump = 0x8C10B434u, kBumped = 0x8C10B558u;
    constexpr uint32_t kMaterial = 0x8C0CD0B0u, kLight = 0x8C0CF520u, kSpecular = 0x8C0CD0C0u;
    guest::Frame frame(64);
    const uint32_t colour = frame.top - 16;  // an untextured strip's colour
    guest::store<uint32_t>(kBumped, 0);
    uint32_t at = param_1.a;
    for (;;) {
        const int32_t head = guest::load<int16_t>(at);
        at += 2;
        if (head == 0xFF)
            return;
        const int32_t type = head & 0xFF;
        if (type < 8) {
            // Bits chunks: the blend mode, and polygon lists cached for later and drawn from there.
            const uint32_t slot = kCache + 4 * static_cast<uint32_t>((head >> 8) & 0xFF);
            if (type == 1) {
                njSetkmCnkBlendMode(static_cast<uint>(head));
            }
            else if (type == 4) {
                guest::store<uint32_t>(slot, at);
                guest::store<uint32_t>(kCached, word(kCached) + 1);
                return;
            }
            else if (type == 5) {
                njCnkEasyPlist(gptr<ushort>(word(slot)), param_2, param_3, param_4, 0, 0, 0, 0, 0, 0, 0, 0);
                guest::store<uint32_t>(kCached, word(kCached) - 1);
            }
            continue;
        }
        if (type < 16) {
            // A texture, and after a bump-mapped one the bump chunk's two vectors.
            const int32_t texture = guest::load<int16_t>(at);
            at += 2;
            const uint result = njSetkmCnkTextureNum(static_cast<uint>(head), static_cast<uint>(texture));
            guest::store<uint32_t>(kBumped, 0);
            if ((result & 0xFF) == 4 && (guest::load<int16_t>(at) & 24)) {
                at += 4;
                for (uint32_t i = 0; i < 6; ++i, at += 2)
                    guest::store<float>(kBump + 4 * i, static_cast<float>(guest::load<int16_t>(at)) / 32767.0f);
                guest::store<uint32_t>(kBumped, 1);
            }
            continue;
        }
        if (type < 64) {
            const int32_t size = guest::load<int16_t>(at);
            at += 2;
            njCnkSetMaterial(static_cast<uint>(head), gptr<short>(at), gptr<float>(kMaterial), gptr<float>(0),
                             gptr<float>(kSpecular));
            guest::store<float>(kLight, real(kMaterial));
            at += static_cast<uint32_t>(size) * 2;
            continue;
        }
        // Strips: the chunk's size, then the count of strips and of user words, then the strips.
        const uint32_t size = guest::load<uint16_t>(at);
        const int32_t strips = guest::load<int16_t>(at + 2);
        const uint32_t data = at + 4;
        if (!(param_3 >= static_cast<int32_t>(word(0x8C122904u)))) {
            uint32_t h = static_cast<uint32_t>(head);
            if (word(0x8C10AF98u) & 0x800)
                h = (h & word(0x8C10AFB4u)) | word(0x8C10AFB8u);
            const int32_t flags = (static_cast<int16_t>(h) >> 8) & 42;
            guest::store<uint32_t>(0x8C0CCD88u, static_cast<uint32_t>(flags & 32));
            const uint32_t buffer = static_cast<uint32_t>(param_4), clip = static_cast<uint32_t>(param_3);
            uint32_t out = 0;
            if (type == 65 || type == 66) {
                const float scale = type == 65 ? 1.0f / 256.0f : 1.0f / 1024.0f;
                if (word(kBumped)) {
                    // Ikaruga draws no bump-mapped model.
                    njSetkmColorMode(flags & 8 ? 0x0210000Cu : 0x0008000Cu);
                    out = (flags & 8 ? njStartVertexTexTrans() : njStartVertexTexOpaque()).a;
                    out = clip ? njCnkEasyTextureBumpClip(gptr<short>(data), param_4, strips, gptr<dword>(out), scale, 0).a
                               : njCnkEasyTextureBump(gptr<short>(data), param_4, strips, gptr<dword>(out), scale, 0).a;
                }
                else {
                    out = param_2 == 35 ? njStartVertex(flags | 3).a
                                        : njStartVertexIntensity(flags | 17, kLight, kSpecular).a;
                    out = clip ? njCnkEasyTextureClip(gptr<short>(data), static_cast<int>(buffer), strips,
                                                      gptr<dword>(out), scale, 0, 0, 0, 0, 0)
                                     .a
                               : njCnkEasyTexture(gptr<short>(data), static_cast<int>(buffer), strips,
                                                  gptr<float>(out), scale, 0, 0, 0, 0, 0)
                                     .a;
                }
            }
            else if (type == 64) {
                if (param_2 == 35) {
                    out = njStartVertex(flags).a;
                }
                else {
                    guest::store<float>(colour, real(kMaterial));
                    guest::store<float>(colour + 4, real(kMaterial + 4) * real(kLight + 4));
                    guest::store<float>(colour + 8, real(kMaterial + 8) * real(kLight + 8));
                    guest::store<float>(colour + 12, real(kMaterial + 12) * real(kLight + 12));
                    out = njStartVertexIntensity(flags | 16, colour, kSpecular).a;
                }
                out = clip ? njCnkEasyPolygonClip(gptr<short>(data), param_4, strips, gptr<float>(out), 0, 0, 0, 0, 0,
                                                  0, 0, 0)
                                 .a
                           : njCnkEasyPolygon(gptr<short>(data), param_4, strips, gptr<ulonglong>(out)).a;
            }
            if (out)
                njEndVertex(out);
        }
        at = data + (size - 1) * 2;
    }
}

// The polygon list of a multi-light model, its chunks up to the end chunk. `param_2` is the
// vertices that the vertex list found less than 1 in front of the camera, `param_3` the vertex
// buffer. When all vertices are that near, no strip is drawn; when some are, the strips go through
// the clipping functions. The strips take the material's colour, or with their flag 4 clear, the
// material's colour times the light's.
void njCnkEasyMultiPlist(gptr<ushort> param_1, int param_2, int param_3, float, float, float, float, float, float,
                         float, float) {
    diff::Check check("njCnkEasyMultiPlist", 0x8C01BFC4u, param_1, param_2, param_3);
    constexpr uint32_t kCache = 0x8C12290Cu, kCached = 0x8C122908u;
    // The material: alpha and red, green and blue (0-12), the specular part (16), and the colour
    // that the light scales (20-28). The light's colour is at 84-92 of 0x8C1226C0.
    constexpr uint32_t kMaterial = 0x8C10B55Cu, kLight = 0x8C1226C0u + 84;
    uint32_t at = param_1.a;
    for (;;) {
        const int32_t head = guest::load<int16_t>(at);
        at += 2;
        if (head == 0xFF)
            return;
        const int32_t type = head & 0xFF;
        if (type < 8) {
            const uint32_t slot = kCache + 4 * static_cast<uint32_t>((head >> 8) & 0xFF);
            if (type == 1) {
                njSetkmCnkBlendMode(static_cast<uint>(head));
            }
            else if (type == 4) {
                guest::store<uint32_t>(slot, at);
                guest::store<uint32_t>(kCached, word(kCached) + 1);
                return;
            }
            else if (type == 5) {
                njCnkEasyMultiPlist(gptr<ushort>(word(slot)), param_2, param_3, 0, 0, 0, 0, 0, 0, 0, 0);
                guest::store<uint32_t>(kCached, word(kCached) - 1);
            }
            continue;
        }
        if (type < 16) {
            const int32_t texture = guest::load<int16_t>(at);
            at += 2;
            njSetkmCnkTextureNum(static_cast<uint>(head), static_cast<uint>(texture));
            continue;
        }
        if (type < 64) {
            const int32_t size = guest::load<int16_t>(at);
            at += 2;
            njCnkSetMaterial(static_cast<uint>(head), gptr<short>(at), gptr<float>(kMaterial),
                             gptr<float>(kMaterial + 16), gptr<float>(0));
            at += static_cast<uint32_t>(size) * 2;
            continue;
        }
        const uint32_t size = guest::load<uint16_t>(at);
        const int32_t strips = guest::load<int16_t>(at + 2);
        const uint32_t data = at + 4;
        if (!(param_2 >= static_cast<int32_t>(word(0x8C122904u)))) {
            uint32_t h = static_cast<uint32_t>(head);
            if (word(0x8C10AF98u) & 0x800)
                h = (h & word(0x8C10AFB4u)) | word(0x8C10AFB8u);
            int32_t flags = static_cast<int16_t>(h) >> 8;
            if (word(0x8C10B3E0u))
                flags |= 8;
            guest::store<uint32_t>(0x8C0CCD88u, static_cast<uint32_t>(flags & 32));
            const bool translucent = flags & 8, near = param_2 != 0;
            // The mode's low bits: 0x10, plus 2 without flag 32, plus 4 without flag 2 when textured.
            const uint32_t base = translucent ? 0x02100000u : 0x00080000u;
            const uint32_t lit = (flags & 32) ? 0 : 2;
            const float m0 = real(kMaterial), zero = 0.0f;
            const auto lit_colour = [&](int i) { return real(kMaterial + 20 + 4 * i) * real(kLight + 4 * i); };
            uint32_t out = 0;
            if (type == 65 || type == 66) {
                njSetkmColorMode(base | 0x18 | ((flags & 2) ? 0 : 4) | lit);
                out = (translucent ? njStartVertexTexTrans() : njStartVertexTexOpaque()).a;
                const float scale = type == 65 ? 1.0f / 256.0f : 1.0f / 1024.0f;
                float r = zero, g = zero, b = zero;
                if (!(flags & 4)) {
                    b = lit_colour(2);
                    g = lit_colour(1);
                    r = lit_colour(0);
                }
                out = near ? njCnkEasyMultiTextureClip(gptr<short>(data), param_3, strips, gptr<float>(out), m0, r, g,
                                                       b, scale, 0)
                                 .a
                           : njCnkEasyMultiTexture(gptr<short>(data), param_3, strips, gptr<ulonglong>(out), m0, r, g,
                                                   b, scale)
                                 .a;
            }
            else if (type == 64) {
                njSetkmColorMode(base | 0x10 | lit);
                out = (translucent ? njStartVertexNonTexTrans() : njStartVertexNonTexOpaque()).a;
                float r = zero, g = zero, b = zero;
                if (!(flags & 4)) {
                    b = lit_colour(2);
                    g = lit_colour(1);
                    r = lit_colour(0);
                }
                const float m1 = real(kMaterial + 4), m2 = real(kMaterial + 8), m3 = real(kMaterial + 12);
                out = near ? njCnkEasyMultiPolygonClip(gptr<short>(data), param_3, strips, gptr<float>(out), m0, m1,
                                                       m2, m3, r, g, b, 0)
                                 .a
                           : njCnkEasyMultiPolygon(m0, m1, m2, m3, r, g, b, gptr<short>(data), param_3, strips,
                                                   gptr<ulonglong>(out))
                                 .a;
            }
            if (out)
                njEndVertex(out);
        }
        at = data + (size - 1) * 2;
    }
}

// A material chunk: its diffuse, ambient and specular colours, each as ARGB bytes, which the
// chunk type's bits 0-2 say are there. Each colour present goes to its output as four floats
// times 1/255, alpha first, unless the output is null. With bit 4 of Ninja's flags at
// 0x8C10AF98, the diffuse colour is the constant at 0x8C10AFBC instead, and with bit 5 it is that
// constant plus the chunk's. Then the chunk's blend mode is set.
void njCnkSetMaterial(uint param_1, gptr<short> param_2, gptr<float> param_3, gptr<float> param_4,
                      gptr<float> param_5) {
    diff::Check check("njCnkSetMaterial", 0x8C02F2E0u, param_1, param_2, param_3, param_4, param_5);
    constexpr uint32_t kFlags = 0x8C10AF98u, kDiffuse = 0x8C10AFBCu;
    const float scale = real(0x8C02F460u);  // 1/255
    const uint32_t kinds = (param_1 & 0xFF) - 16;
    uint32_t at = param_2.a;
    // The two halfwords of an ARGB colour hold, from the second's high byte to the first's low
    // byte, alpha, red, green and blue.
    const auto channel = [](int32_t first, int32_t second, int i) -> float {
        const int32_t w = i < 2 ? second : first;
        return static_cast<float>(i % 2 ? w & 0xFF : (static_cast<uint32_t>(w) >> 8) & 0xFF);
    };
    if (kinds & 1) {
        const int32_t first = guest::load<int16_t>(at), second = guest::load<int16_t>(at + 2);
        at += 4;
        const uint32_t flags = word(kFlags), out = param_3.a;
        if (flags & 16) {
            for (uint32_t i = 0; i < 4; ++i) guest::store<uint32_t>(out + 4 * i, word(kDiffuse + 4 * i));
        }
        else if (out) {
            for (int i = 0; i < 4; ++i)
                guest::store<float>(out + 4 * static_cast<uint32_t>(i),
                                    flags & 32 ? std::fma(scale, channel(first, second, i), real(kDiffuse + 4 * i))
                                               : channel(first, second, i) * scale);
        }
    }
    if (kinds & 2) {
        const int32_t first = guest::load<int16_t>(at), second = guest::load<int16_t>(at + 2);
        at += 4;
        if (param_4.a)
            for (int i = 0; i < 4; ++i)
                guest::store<float>(param_4.a + 4 * static_cast<uint32_t>(i), channel(first, second, i) * scale);
    }
    if (kinds & 4) {
        const int32_t first = guest::load<int16_t>(at), second = guest::load<int16_t>(at + 2);
        if (param_5.a)
            for (int i = 0; i < 4; ++i)
                guest::store<float>(param_5.a + 4 * static_cast<uint32_t>(i), channel(first, second, i) * scale);
    }
    njSetkmCnkBlendMode(param_1);
}

// Whether a model's bounding sphere, its centre at 8 and its radius at 20, lies outside the view:
// beyond the depths at 0x8C10AF64 and 0x8C10AF68, or past the screen's edges, each scaled by the
// sphere's depth. Returns 1 when the model needs no drawing.
undefined4 njCnkModelClip(int param_1) {
    diff::Check check("njCnkModelClip", 0x8C02F270u, param_1);
    constexpr uint32_t kView = 0x8C10AF64u;
    const uint32_t model = static_cast<uint32_t>(param_1);
    const Vec4 c = transform(real(model + 8), real(model + 12), real(model + 16));
    const float radius = real(model + 20);
    float depth = -c.z;
    if (depth > real(kView) + radius)
        return check.returns(1);
    depth = depth + radius;
    if (!(depth > real(kView + 4)))
        return check.returns(1);
    const float x = c.x * real(kProjection), y = c.y * real(kProjection + 4);
    const float rx = real(kProjection) * radius, ry = real(kProjection + 4) * radius;
    if (x - rx > real(kView + 8) * depth)
        return check.returns(1);
    if (real(kView + 12) * depth > x + rx)
        return check.returns(1);
    if (y + ry > real(kView + 16) * depth)
        return check.returns(1);
    if (real(kView + 20) * depth > y - ry)
        return check.returns(1);
    return check.returns(0);
}

// The vertex list of an Easy model: each vertex chunk goes into the vertex buffer at `param_2`,
// at its index offset, through the vertex function of its type. The vertices that the functions
// find not more than 1 in front of the camera add up at 0x8C122900, the vertices at 0x8C122904
// and 0x8C10AFA8; a chunk without bit 15 starts the first two again. Returns the last chunk's
// head, or for an empty list what the caller held in r11.
uint njCnkEasyVlist(gptr<short> param_1, int param_2, undefined4 ctx_r8, undefined4 ctx_r9, uint ctx_r11) {
    diff::Check check("njCnkEasyVlist", 0x8C01A0E4u, param_1, param_2);
    constexpr uint32_t kContext = 0x8C10B3E8u, kNear = 0x8C122900u, kCounted = 0x8C122904u;
    uint32_t at = param_1.a, result = ctx_r11;
    for (;;) {
        const int32_t head = guest::load<int16_t>(at);
        if (head == 0xFF)
            return check.returns(result);
        const uint32_t size = guest::load<uint16_t>(at + 2);
        const int32_t offset = guest::load<int16_t>(at + 4), count = guest::load<int16_t>(at + 6);
        at += 8;
        uint32_t h = static_cast<uint32_t>(head);
        if (!(h & 0x8000)) {
            guest::store<uint32_t>(kNear, 0);
            guest::store<uint32_t>(kCounted, 0);
        }
        const uint32_t place = (static_cast<uint32_t>(offset) << 4) + static_cast<uint32_t>(param_2);
        guest::store<int32_t>(kContext + 20, count);
        guest::store<uint32_t>(kContext + 24, at);
        h &= 0x7FFFFFFFu;
        guest::store<uint32_t>(kContext + 28, place);
        guest::store<uint32_t>(kContext + 32, sySqStart(place));
        guest::store<uint32_t>(0x8C10AFA8u, word(0x8C10AFA8u) + static_cast<uint32_t>(count));
        int32_t near;
        switch (h & 0xFF) {
            case 41: near = njCnkEasyVertexVN(gptr<float>(kContext), 0); break;
            case 42:
                near = njCnkEasyVertexVND8(static_cast<int>(kContext));
                h = 35;
                break;
            case 44: near = njCnkEasyVertexVNNF(gptr<float>(kContext), h); break;
            case 43: near = njCnkEasyVertexVN1(gptr<float>(kContext)); break;
            case 34: near = njCnkEasyVertexV(static_cast<int>(kContext)); break;
            case 36: near = njCnkEasyVertexV1(static_cast<int>(kContext)); break;
            case 35: near = njCnkEasyVertexVD8(static_cast<int>(kContext)); break;
            default: near = count;
        }
        guest::store<uint32_t>(kNear, word(kNear) + static_cast<uint32_t>(near));
        // A weighted chunk counts its vertices with its last part only.
        if ((h & 0xFF00) == 0x200 || (h & 0xFF) != 44)
            guest::store<uint32_t>(kCounted, word(kCounted) + static_cast<uint32_t>(count));
        at += size * 4 - 4;
        result = h;
    }
}

// An Easy model: when culling is on (bit 8 of Ninja's flags) and the model has a radius, a model
// outside the view returns -1. Otherwise its vertex list fills the vertex buffer at 0x8C10AFA4 and
// its polygon list draws from it.
undefined4 njCnkEasyDrawModel(gptr<undefined4> param_1, uint param_2, float, float, float, float, float, float, float,
                        float, undefined4 ctx_r8, undefined4 ctx_r9, undefined4 ctx_r11) {
    diff::Check check("njCnkEasyDrawModel", 0x8C01A23Au, param_1, param_2);
    const uint32_t model = param_1.a;
    if ((word(0x8C10AF98u) & 0x100) && real(model + 20) > 0.0f && njCnkModelClip(static_cast<int>(model)) != 0u)
        return check.returns(0xFFFFFFFFu);
    const uint32_t buffer = word(0x8C10AFA4u);
    uint32_t type = param_2;
    if (word(model))
        type = njCnkEasyVlist(gptr<short>(word(model)), static_cast<int>(buffer), ctx_r8, ctx_r9, ctx_r11);
    const uint32_t polygons = word(model + 4), near = word(0x8C122900u);
    if (polygons) {
        guest::store<uint32_t>(0x8C10B430u, type);
        njCnkEasyPlist(gptr<ushort>(polygons), static_cast<int>(type), static_cast<int>(near), static_cast<int>(buffer),
                     0, 0, 0, 0, 0, 0, 0, 0);
    }
    return check.returns(0);
}

namespace {

// The vertex list of a multi-light model: like an Easy model's, with the chunk's context on the
// stack. While depth cueing is on (0x8C10B3E0), the depth-cueing vertex functions run. `simple`
// takes the Simple models' vertex functions, with 64 bytes in the vertex buffer for each vertex
// instead of 32.
void multi_vlist(uint32_t at, uint32_t buffer, bool simple) {
    constexpr uint32_t kNear = 0x8C122900u, kCounted = 0x8C122904u;
    guest::Frame frame(20);
    // The vertex functions' context: the count, the data, the place in the buffer and its
    // store-queue address.
    const uint32_t context = frame.top - 16;
    for (;;) {
        const int32_t head = guest::load<int16_t>(at);
        if (head == 0xFF)
            return;
        const uint32_t size = guest::load<uint16_t>(at + 2);
        const int32_t offset = guest::load<int16_t>(at + 4), count = guest::load<int16_t>(at + 6);
        at += 8;
        uint32_t h = static_cast<uint32_t>(head);
        if (!(h & 0x8000)) {
            guest::store<uint32_t>(kNear, 0);
            guest::store<uint32_t>(kCounted, 0);
        }
        const uint32_t place = (static_cast<uint32_t>(offset) << (simple ? 6 : 5)) + buffer;
        guest::store<int32_t>(context, count);
        guest::store<uint32_t>(context + 4, at);
        h &= 0x7FFFFFFFu;
        guest::store<uint32_t>(context + 8, place);
        guest::store<uint32_t>(context + 12, sySqStart(place));
        guest::store<uint32_t>(0x8C10AFA8u, word(0x8C10AFA8u) + static_cast<uint32_t>(count));
        const bool cueing = word(0x8C10B3E0u) != 0;
        const gptr<int> c(context);
        const uint32_t type = h & 0xFF;
        int32_t near = count;
        if (simple) {
            const gptr<ulonglong> projection(kProjection);
            if (type == 41)
                near = cueing ? njCnkSimpleMultiVertexVNDepthQueue(c, projection, 0)
                              : njCnkSimpleMultiVertexVN(c, projection, 0);
            else if (type == 44)
                near = cueing ? njCnkSimpleMultiVertexVNNFDepthQueue(0.0f, c, h, projection, 0)
                              : njCnkSimpleMultiVertexVNNF(0.0f, c, h, projection, 0);
            else if (type == 42)
                near = cueing ? njCnkSimpleMultiVertexVND8DepthQueue(c, projection, 0, 0, 0, 0)
                              : njCnkSimpleMultiVertexVND8(c, projection, 0);
        }
        else {
            if (type == 41)
                near = cueing ? njCnkEasyMultiVertexVNDepthQueue(c, gptr<float>(kProjection), 0.0f)
                              : njCnkEasyMultiVertexVN(c, gptr<float>(kProjection), 0.0f);
            else if (type == 44)
                near = cueing ? njCnkEasyMultiVertexVNNFDepthQueue(c, h, gptr<ulonglong>(kProjection), 0)
                              : njCnkEasyMultiVertexVNNF(c, h, gptr<ulonglong>(kProjection), 0);
            else if (type == 42)
                near = cueing ? njCnkEasyMultiVertexVND8DepthQueue(c, gptr<float>(kProjection), 0, 0, 0, 0)
                              : njCnkEasyMultiVertexVND8(c, gptr<float>(kProjection), 0, 0, 0, 0);
        }
        guest::store<uint32_t>(kNear, word(kNear) + static_cast<uint32_t>(near));
        if ((h & 0xFF00) == 0x200 || type != 44)
            guest::store<uint32_t>(kCounted, word(kCounted) + static_cast<uint32_t>(count));
        at += size * 4 - 4;
    }
}

}  // namespace

// The vertex list of a multi-light Easy model (see multi_vlist). Returns 0.
undefined4 njCnkEasyMultiVlist(gptr<short> param_1, int param_2, float, float, float, float) {
    diff::Check check("njCnkEasyMultiVlist", 0x8C01C43Au, param_1, param_2);
    multi_vlist(param_1.a, static_cast<uint32_t>(param_2), false);
    return check.returns(undefined4(0));
}

namespace {

// A multi-light model: culled as an Easy model is. With bit 6 of Ninja's flags, depth cueing is
// on when the far side of its bounding sphere lies beyond the depth at 0x8C10B3D4. `simple` takes
// the Simple models' vertex and polygon lists.
Sint32 multi_draw_model(uint32_t m, bool simple) {
    if ((word(0x8C10AF98u) & 0x100) && real(m + 20) > 0.0f && njCnkModelClip(static_cast<int>(m)) != 0u)
        return -1;
    guest::store<uint32_t>(0x8C10B3E0u, 0);
    if (word(0x8C10AF98u) & 64) {
        guest::Frame frame(12);
        const uint32_t centre = frame.top - 12;
        njCalcPoint(gptr<NJS_MATRIX>(0), gptr<NJS_POINT3>(m + 8), gptr<NJS_POINT3>(centre));
        if (real(centre + 8) + real(m + 20) > real(0x8C10B3D4u))
            guest::store<uint32_t>(0x8C10B3E0u, 1);
    }
    const uint32_t buffer = word(0x8C10AFA4u);
    if (word(m)) {
        if (simple)
            njCnkSimpleMultiVlist(gptr<short>(word(m)), static_cast<int>(buffer), 0, 0, 0, 0);
        else
            njCnkEasyMultiVlist(gptr<short>(word(m)), static_cast<int>(buffer), 0, 0, 0, 0);
    }
    const uint32_t polygons = word(m + 4), near = word(0x8C122900u);
    if (polygons) {
        if (simple)
            njCnkSimpleMultiPlist(gptr<ushort>(polygons), static_cast<int>(near), static_cast<int>(buffer), 0, 0, 0,
                                  0, 0, 0, 0, 0);
        else
            njCnkEasyMultiPlist(gptr<ushort>(polygons), static_cast<int>(near), static_cast<int>(buffer), 0, 0, 0, 0,
                                0, 0, 0, 0);
    }
    return 0;
}

}  // namespace

// A multi-light Easy model (see multi_draw_model).
Sint32 njCnkEasyMultiDrawModel(gptr<NJS_CNK_MODEL> model) {
    diff::Check check("njCnkEasyMultiDrawModel", 0x8C01C596u, model);
    return check.returns(multi_draw_model(model.a, false));
}

// Vertices with a normal for a Simple model, lit in the view's frame: the normal goes through the
// matrix, and its product with the light's direction (0-8 of the context, scaled by the intensity
// at 12) is the light, and that to the 17th power the highlight. Each vertex goes to the buffer as
// 32 bytes: -z, the screen position, 1/w, the light, the highlight, and the turned normal's x and
// negated y. Returns the vertices less than 1 in front of the camera.
int njCnkSimpleVertexVN(gptr<float> param_1) {
    diff::Check check("njCnkSimpleVertexVN", 0x8C028ACCu, param_1);
    const uint32_t context = param_1.a;
    const float intensity = real(context + 12);
    const float lx = real(context) * intensity, ly = real(context + 4) * intensity, lz = real(context + 8) * intensity;
    uint32_t count = word(context + kCount), data = word(context + kData), out = word(context + kOut);
    const float sx = real(kProjection), sy = real(kProjection + 4), ox = real(kProjection + 8),
                oy = real(kProjection + 12);
    int near = 0;
    do {
        const Vec4 p = transform(real(data), real(data + 4), real(data + 8));
        const Vec4 n = rotate(real(data + 12), real(data + 16), real(data + 20));
        data += 24;
        const float depth = -p.z;
        const float light = fipr(n.x, n.y, n.z, n.w, lx, ly, lz, 0.0f);
        const float q = 1.0f / depth;
        if (1.0f > depth)
            ++near;
        float highlight = light * light;
        highlight = highlight * highlight;
        highlight = highlight * highlight;
        highlight = highlight * highlight;
        highlight = highlight * light;
        const float words[8] = {depth, std::fma(q, p.x * sx, ox), std::fma(q, p.y * sy, oy), q, light, highlight,
                                n.x, -n.y};
        for (uint32_t i = 0; i < 8; ++i) guest::store<float>(out + 4 * i, words[i]);
        hw::prefetch(out + 20);
        out += 32;
    } while (--count != 0);
    return check.returns(near);
}

namespace {

// Textured strips of a Simple model. `ambient` is the ambient light, `scale` the u and v scale, then
// come the strip data, the vertex buffer (32 bytes for each vertex), the count of strips and user
// words, and where the vertices go. A triangle faces the camera when the cross product of its edges
// on the screen, times a sign that alternates along the strip, is negative; a strip given
// backwards, or bit 31 of Ninja's flags, turns the sign round. The strips break where the facing
// changes. A vertex's light is the buffer's plus the ambient light, and never less than it. With
// `both_sides`, the triangles that face away go out too, lit from behind: the ambient light minus
// the buffer's, never less than the ambient light, and the highlight negated. Without, they are
// left out. With `clipped`, a triangle with a vertex less than 1 deep breaks the strip, and when
// one of its vertices is more than 1 deep, it goes to the near clip with the ambient light and its
// sign; with both faces, to their near clip, also when only its third vertex is exactly 1 deep.
// With `coloured`, a vertex takes the buffer's colour and the word after it instead of a
// light; clipped, a vertex not more than 1 deep breaks the strip, and the near clip gets only the
// sign. Without `textured`, the strips have no u and v, their words keep what the queue held, and
// the near clip is the untextured one. Returns where the next vertex goes.
uint32_t simple_textured_strips(float ambient, float scale, uint32_t in, uint32_t buffer, uint32_t count, uint32_t out,
                                bool both_sides, bool clipped = false, bool coloured = false, bool textured = true) {
    uint32_t strips = count & 0x3FFF;
    const uint32_t user = ((count >> 14) & 3) * 2;
    const bool turned = word(0x8C10AF98u) & 0x80000000u;
    struct Corner {
        uint32_t at;
        float u, v;
    };
    const auto next = [&in] {
        const int32_t v = guest::load<int16_t>(in);
        in += 2;
        return v;
    };
    const auto read = [&] {
        Corner c{buffer + (static_cast<uint32_t>(next()) << 5), 0.0f, 0.0f};
        if (textured) {
            c.u = static_cast<float>(next()) * scale;
            c.v = static_cast<float>(next()) * scale;
        }
        return c;
    };
    const auto emit = [&](const Corner& c, uint32_t control, bool behind) {
        uint32_t light = word(c.at + 16), highlight = word(c.at + 20);
        if (!coloured) {
            float lit = from_bits(light);
            if (behind) {
                lit = -lit + ambient;
                highlight ^= 0x80000000u;
            }
            else {
                lit = lit + ambient;
            }
            if (ambient > lit)
                lit = ambient;
            light = bits_of(lit);
        }
        const uint32_t words[8] = {control,       word(c.at + 4), word(c.at + 8), word(c.at + 12),
                                   bits_of(c.u), bits_of(c.v),   light,          highlight};
        for (uint32_t i = 0; i < 8; ++i)
            if (textured || (i != 4 && i != 5))
                guest::store<uint32_t>(out + 4 * i, words[i]);
        // The machine code prefetches the lit strips' blocks at their light, the coloured ones at
        // their start, and the flush keeps the address.
        hw::prefetch(coloured ? out : out + 24);
        out += 32;
    };
    uint32_t sent = 0, total = 0;
    do {
        int32_t length = next();
        float sign = turned ? -1.0f : 1.0f;
        if (length < 0) {
            length = -length;
            sign = -sign;
        }
        total += static_cast<uint32_t>(length);
        Corner a = read(), b = read();
        // The vertices of the strip being sent, but its last, and whether it faces away.
        uint32_t open = 0;
        bool behind = false;
        uint32_t triangles = static_cast<uint32_t>(length - 2);
        do {
            const Corner c = read();
            in += user;
            const bool near =
                clipped && (coloured ? !(real(a.at) > 1.0f && real(b.at) > 1.0f && real(c.at) > 1.0f)
                                     : 1.0f > real(a.at) || 1.0f > real(b.at) || 1.0f > real(c.at));
            if (near) {
                if (open) {
                    emit(b, kLastVertex, behind);
                    sent += open + 1;
                    open = 0;
                }
            }
            else {
                const float bx = real(b.at + 4), by = real(b.at + 8);
                const float ax = real(a.at + 4) - bx, ay = real(a.at + 8) - by;
                const float cx = real(c.at + 4) - bx, cy = real(c.at + 8) - by;
                const bool away = !(0.0f > (ay * cx - cy * ax) * sign);
                if (open && behind != away) {
                    emit(b, kLastVertex, behind);
                    sent += open + 1;
                    open = 0;
                }
                if (!away || both_sides) {
                    behind = away;
                    if (open == 0) {
                        emit(a, kVertex, behind);
                        ++open;
                    }
                    emit(b, kVertex, behind);
                    ++open;
                }
            }
            const bool deep = real(a.at) > 1.0f || real(b.at) > 1.0f ||
                              (both_sides ? !(1.0f > real(c.at)) : real(c.at) > 1.0f);
            if (near && deep) {
                const gptr<float> pa(a.at), pb(b.at), pc(c.at);
                if (!textured)
                    out = njCnkSimplePolygonNearClip(pa, pb, pc, gptr<dword>(out), ambient, sign).a;
                else if (coloured)
                    out = njCnkSimpleTextureD8NearClip(pa, pb, pc, gptr<dword>(out), a.u, a.v, b.u, b.v, c.u, c.v,
                                                       sign, 0).a;
                else if (both_sides)
                    out = njCnkSimpleTextureDbNearClip(pa, pb, pc, gptr<dword>(out), a.u, a.v, b.u, b.v, c.u, c.v,
                                                       ambient, sign).a;
                else
                    out = njCnkSimpleTextureNearClip(pa, pb, pc, gptr<dword>(out), a.u, a.v, b.u, b.v, c.u, c.v,
                                                     ambient, sign).a;
            }
            sign = -sign;
            a = b;
            b = c;
        } while (--triangles != 0);
        if (open) {
            emit(b, kLastVertex, behind);
            sent += open + 1;
        }
    } while (--strips != 0);
    guest::store<uint32_t>(0x8C10AFB0u, word(0x8C10AFB0u) + sent);
    guest::store<uint32_t>(0x8C10AFACu, word(0x8C10AFACu) + total);
    return out;
}

}  // namespace

// The polygon list of a Simple model, its chunks up to the end chunk. `param_2` is the vertices that
// the vertex list found less than 1 in front of the camera, `param_3` the vertex buffer. When all
// vertices are that near, no strip is drawn; when some are, the strips go through the clipping
// functions. The tables at 0x8C0CD198 and, for clipped strips, 0x8C0CE198 hold 32 bytes for each
// value of a strip chunk's flags: the colour modes of lit and unlit untextured strips, their
// functions, and the same for textured strips. With the lighting flag at 0x8C10B57C set, a lit
// strip starts with the material's colour, or with the strip's flag 1 (4 when untextured) clear,
// the material's colour times the light's.
void njCnkSimplePlist(gptr<ushort> param_1, int param_2, int param_3) {
    diff::Check check("njCnkSimplePlist", 0x8C01C688u, param_1, param_2, param_3);
    constexpr uint32_t kCache = 0x8C12290Cu, kCached = 0x8C122908u, kBump = 0x8C10B44Cu, kBumped = 0x8C10B580u;
    // The material's alpha, red, green and blue, its specular colour, and the light's colour.
    constexpr uint32_t kMaterial = 0x8C0CD0D0u, kSpecular = 0x8C0CD0F0u, kLight = 0x8C0CF540u;
    constexpr uint32_t kStrips = 0x8C0CD198u, kClippedStrips = 0x8C0CE198u;
    guest::Frame frame(16);
    const uint32_t colour = frame.top - 16;
    const auto set_colour = [&](bool plain) {
        guest::store<uint32_t>(colour, word(kMaterial));
        for (uint32_t i = 4; i < 16; i += 4)
            if (plain)
                guest::store<uint32_t>(colour + i, word(kMaterial + i));
            else
                guest::store<float>(colour + i, real(kMaterial + i) * real(kLight + i));
    };
    uint32_t at = param_1.a;
    for (;;) {
        const int32_t head = guest::load<int16_t>(at);
        at += 2;
        if (head == 0xFF)
            return;
        const int32_t type = head & 0xFF;
        if (type < 8) {
            const uint32_t slot = kCache + 4 * static_cast<uint32_t>((head >> 8) & 0xFF);
            if (type == 1) {
                njSetkmCnkBlendMode(static_cast<uint>(head));
            }
            else if (type == 4) {
                guest::store<uint32_t>(slot, at);
                guest::store<uint32_t>(kCached, word(kCached) + 1);
                return;
            }
            else if (type == 5) {
                njCnkSimplePlist(gptr<ushort>(word(slot)), param_2, param_3);
                guest::store<uint32_t>(kCached, word(kCached) - 1);
            }
            continue;
        }
        if (type < 16) {
            const int32_t texture = guest::load<int16_t>(at);
            at += 2;
            const uint result = njSetkmCnkTextureNum(static_cast<uint>(head), static_cast<uint>(texture));
            guest::store<uint32_t>(kBumped, 0);
            if ((result & 0xFF) == 4 && (guest::load<int16_t>(at) & 24)) {
                at += 4;
                for (uint32_t i = 0; i < 6; ++i, at += 2)
                    guest::store<float>(kBump + 4 * i, static_cast<float>(guest::load<int16_t>(at)) / 32767.0f);
                guest::store<uint32_t>(kBumped, 1);
            }
            continue;
        }
        if (type < 64) {
            const int32_t size = guest::load<int16_t>(at);
            at += 2;
            njCnkSetMaterial(static_cast<uint>(head), gptr<short>(at), gptr<float>(kMaterial), gptr<float>(0),
                             gptr<float>(kSpecular));
            at += static_cast<uint32_t>(size) * 2;
            continue;
        }
        const uint32_t size = guest::load<uint16_t>(at);
        const uint32_t strips = static_cast<uint32_t>(guest::load<int16_t>(at + 2));
        const uint32_t data = at + 4, buffer = static_cast<uint32_t>(param_3);
        if (!(param_2 >= static_cast<int32_t>(word(0x8C122904u)))) {
            uint32_t h = static_cast<uint32_t>(head);
            if (word(0x8C10AF98u) & 0x800)
                h = (h & word(0x8C10AFB4u)) | word(0x8C10AFB8u);
            const int32_t flags = static_cast<int16_t>(h) >> 8;
            float ambient = flags & 4 ? 0.0f : real(0x8C10B41Cu);
            if (flags & 1)
                ambient = 1.0f;
            guest::store<uint32_t>(0x8C0CCD88u, flags & 32 ? 1 : 0);
            const uint32_t entry = 32 * static_cast<uint32_t>(flags);
            const uint32_t functions = (param_2 ? kClippedStrips : kStrips) + entry, modes = kStrips + entry;
            const bool lit = word(0x8C10B57Cu) != 0, translucent = flags & 8;
            const auto start_textured_lit = [&] {
                if (translucent)
                    return flags & 2 ? njStartVertexTexTransIntens(gptr<undefined4>(colour)).a
                                     : njStartVertexTexTransIntensH(gptr<undefined4>(colour), gptr<undefined4>(kSpecular)).a;
                return flags & 2 ? njStartVertexTexOpaqueIntens(gptr<undefined4>(colour)).a
                                 : njStartVertexTexOpaqueIntensH(gptr<undefined4>(colour), gptr<undefined4>(kSpecular)).a;
            };
            const auto start_textured = [&] {
                return translucent ? njStartVertexTexTrans().a : njStartVertexTexOpaque().a;
            };
            uint32_t out = 0;
            if (type == 65 || type == 66) {
                const float scale = type == 65 ? 1.0f / 256.0f : 1.0f / 1024.0f;
                if (word(kBumped)) {
                    // Ikaruga draws no bump-mapped model.
                    if (translucent) {
                        njSetkmColorMode(0x0210000Cu);
                        guest::store<uint32_t>(kLight, word(kMaterial));
                    }
                    else {
                        njSetkmColorMode(0x0008000Cu);
                    }
                    out = start_textured();
                    out = strip_function(word(param_2 ? 0x8C0CF19Cu : 0x8C0CF198u), data, buffer, strips, out,
                                         {ambient, scale});
                }
                else if (lit) {
                    if (word(0x8C10AF98u) & 0x200) {
                        guest::store<uint32_t>(colour, word(kMaterial));
                        for (uint32_t i = 4; i < 16; i += 4)
                            if (flags & 1)
                                guest::store<float>(colour + i, 1.0f);
                            else
                                guest::store<uint32_t>(colour + i, word(kLight + i));
                    }
                    else {
                        set_colour(flags & 1);
                    }
                    njSetkmColorMode(word(modes + 16));
                    out = start_textured_lit();
                    out = strip_function(word(functions + 24), data, buffer, strips, out, {ambient, scale});
                }
                else {
                    njSetkmColorMode(word(modes + 20));
                    out = start_textured();
                    out = strip_function(word(functions + 28), data, buffer, strips, out, {scale, 0.0f});
                }
            }
            else if (type == 64) {
                // Untextured strips, but with flag 64 they start as textured ones.
                if (lit) {
                    set_colour(flags & 64 ? flags & 1 : flags & 4);
                    njSetkmColorMode(word(modes));
                    if (flags & 64)
                        out = start_textured_lit();
                    else
                        out = translucent ? njStartVertexNonTexTransIntens(gptr<undefined4>(colour)).a
                                          : njStartVertexNonTexOpaqueIntens(gptr<undefined4>(colour)).a;
                    out = strip_function(word(functions + 8), data, buffer, strips, out, {ambient, 0.0f});
                }
                else {
                    njSetkmColorMode(word(modes + 4));
                    if (flags & 64)
                        out = start_textured();
                    else
                        out = translucent ? njStartVertexNonTexTrans().a : njStartVertexNonTexOpaque().a;
                    out = strip_function(word(functions + 12), data, buffer, strips, out, {1.0f, 0.0f});
                }
            }
            if (out)
                njEndVertex(out);
        }
        at = data + (size - 1) * 2;
    }
}

// The vertex list of a Simple model, as the Easy model's but with 32 bytes for each vertex. The
// table at 0x8C0CD100 gives each vertex type's function, and whether the polygon list lights the
// strips (0x8C10B57C).
void njCnkSimpleVlist(gptr<ushort> param_1, int param_2, undefined4, undefined4) {
    diff::Check check("njCnkSimpleVlist", 0x8C01CE1Au, param_1, param_2);
    constexpr uint32_t kContext = 0x8C10B40Cu, kNear = 0x8C122900u, kCounted = 0x8C122904u;
    uint32_t at = param_1.a;
    for (;;) {
        uint32_t head = guest::load<uint16_t>(at);
        if (head == 0xFF)
            return;
        const uint32_t size = guest::load<uint16_t>(at + 2);
        const int32_t offset = guest::load<int16_t>(at + 4), count = guest::load<int16_t>(at + 6);
        at += 8;
        if (!(head & 0x8000)) {
            guest::store<uint32_t>(kNear, 0);
            guest::store<uint32_t>(kCounted, 0);
        }
        head &= 0x7FFF;
        const uint32_t place = (static_cast<uint32_t>(offset) << 5) + static_cast<uint32_t>(param_2);
        guest::store<int32_t>(kContext + 20, count);
        guest::store<uint32_t>(kContext + 24, at);
        guest::store<uint32_t>(kContext + 28, place);
        guest::store<uint32_t>(kContext + 32, sySqStart(place));
        guest::store<uint32_t>(0x8C10AFA8u, word(0x8C10AFA8u) + static_cast<uint32_t>(count));
        const uint32_t type = head & 0xFF, entry = 0x8C0CD100u + 8 * (type - 32);
        int32_t near = count;
        if (type == 44)
            near = njCnkSimpleVertexVNNF(gptr<float>(kContext), head, 0);
        else if (word(entry))
            near = static_cast<int32_t>(call_with(word(entry), kContext));
        guest::store<uint32_t>(0x8C10B57Cu, word(entry + 4));
        guest::store<uint32_t>(kNear, word(kNear) + static_cast<uint32_t>(near));
        // A weighted chunk counts its vertices with its last part only.
        if ((head & 0xFF00) == 0x200 || type != 44)
            guest::store<uint32_t>(kCounted, word(kCounted) + static_cast<uint32_t>(count));
        at += size * 4 - 4;
    }
}

// A Simple model: when culling is on (bit 8 of Ninja's flags) and the model has a radius, a model
// outside the view returns -1. Otherwise its vertex list fills the vertex buffer at 0x8C10AFA4 and
// its polygon list draws from it.
undefined4 njCnkSimpleDrawModel(gptr<undefined4> param_1, undefined4 ctx_r8, undefined4 ctx_r9) {
    diff::Check check("njCnkSimpleDrawModel", 0x8C01CF3Eu, param_1);
    const uint32_t model = param_1.a, buffer = word(0x8C10AFA4u), vertices = word(model);
    if ((word(0x8C10AF98u) & 0x100) && real(model + 20) > 0.0f && njCnkModelClip(static_cast<int>(model)) != 0u)
        return check.returns(0xFFFFFFFFu);
    if (vertices)
        njCnkSimpleVlist(gptr<ushort>(vertices), static_cast<int>(buffer), ctx_r8, ctx_r9);
    if (const uint32_t polygons = word(model + 4))
        njCnkSimplePlist(gptr<ushort>(polygons), static_cast<int>(word(0x8C122900u)), static_cast<int>(buffer));
    return check.returns(0);
}

// A textured triangle of an Easy model that the near plane cuts, from njCnkEasyTextureClip: its
// vertices in the vertex buffer, where its vertices go, and each vertex's u and v. A triangle with
// all three vertices beyond one edge of the 640 x 480 screen is left out; a vertex behind the
// camera counts as beyond the edges opposite its position. Otherwise each edge (c to a, a to b,
// b to c) that crosses w = 1 gives a vertex there, interpolated in the view's x and y, and each
// vertex with w > 1 stays. The three or four vertices go out as one strip. A vertex's light, or for
// vertex colours (type 35 at 0x8C10B430) each byte of its colour, is interpolated too; flat strips
// (0x8C0CCD88) take the third vertex's for the last two. The vertex count at 0x8C10AFB0 grows by the
// vertices sent.
gptr<dword> njCnkEasyTextureNearClip(gptr<float> param_1, gptr<float> param_2, gptr<float> param_3,
                                     gptr<dword> param_4, float arg_fr4, float arg_fr5, float arg_fr6, float arg_fr7,
                                     float arg_fr8, float arg_fr9) {
    diff::Check check("njCnkEasyTextureNearClip", 0x8C019140u, param_1, param_2, param_3, param_4, arg_fr4, arg_fr5,
                      arg_fr6, arg_fr7, arg_fr8, arg_fr9);
    if (word(0x8C10AF98u) & 0x10000)
        return check.returns(param_4);
    const auto code = [](uint32_t v) { return outcode(real(v), real(v + 4), real(v + 8)); };
    if (code(param_1.a) & code(param_2.a) & code(param_3.a))
        return check.returns(param_4);
    const float cx = real(0x8C10AF3Cu), cy = real(0x8C10AF40u);
    // A vertex: its words as they go out (colour or light, v, u, 1/w, y, x), and for the clipping
    // its x and y in the view, and w.
    struct Vertex {
        uint32_t colour, v, u, q, y, x;
        float vx, vy, w;
    };
    const auto vertex = [&](uint32_t at, float u, float v) {
        Vertex r{word(at + 12), bits_of(v), bits_of(u), word(at + 8), word(at + 4), word(at), 0, 0, 0};
        r.w = 1.0f / real(at + 8);
        r.vx = (real(at) - cx) * r.w;
        r.vy = (real(at + 4) - cy) * r.w;
        return r;
    };
    const Vertex in[3] = {vertex(param_1.a, arg_fr4, arg_fr5), vertex(param_2.a, arg_fr6, arg_fr7),
                          vertex(param_3.a, arg_fr8, arg_fr9)};
    const bool colours = word(0x8C10B430u) == 35;
    Vertex out[6] = {};
    uint32_t n = 0;
    const Vertex* previous = &in[2];
    for (const Vertex& current : in) {
        const bool beyond = current.w > 1.0f;
        if (beyond != (previous->w > 1.0f)) {
            const float t = (-previous->w + 1.0f) / (current.w - previous->w);
            const auto lerp = [t](float a, float b) { return std::fma(t, b - a, a); };
            Vertex& r = out[n++];
            r.x = bits_of(lerp(previous->vx, current.vx) + cx);
            r.y = bits_of(lerp(previous->vy, current.vy) + cy);
            r.q = bits_of(1.0f);
            r.u = bits_of(lerp(from_bits(previous->u), from_bits(current.u)));
            r.v = bits_of(lerp(from_bits(previous->v), from_bits(current.v)));
            r.colour = colours ? mix_colour(previous->colour, current.colour, t)
                               : bits_of(lerp(from_bits(previous->colour), from_bits(current.colour)));
        }
        if (beyond)
            out[n++] = current;
        previous = &current;
    }
    if (word(0x8C0CCD88u))
        out[2].colour = out[3].colour = in[2].colour;
    uint32_t at = param_4.a;
    const auto send_vertex = [&at](const Vertex& r, uint32_t control) {
        const uint32_t words[8] = {control, r.x, r.y, r.q, r.u, r.v, r.colour, bits_of(from_bits(r.colour) + -1.0f)};
        for (uint32_t i = 0; i < 8; ++i) guest::store<uint32_t>(at + 4 * i, words[i]);
        hw::prefetch(at);
        at += 32;
    };
    send_vertex(out[0], kVertex);
    send_vertex(out[1], kVertex);
    if (n != 3)
        send_vertex(out[3], kVertex);
    send_vertex(out[2], kLastVertex);
    guest::store<uint32_t>(0x8C10AFB0u, word(0x8C10AFB0u) + n);
    return check.returns(gptr<dword>(at));
}

// Textured strips of a Simple model, front faces only.
gptr<float> njCnkSimpleTexture(float param_1, float param_2, gptr<short> param_3, int param_4, uint param_5,
                               gptr<float> param_6, float, float, uint, undefined4) {
    diff::Check check("njCnkSimpleTexture", 0x8C028558u, param_1, param_2, param_3, param_4, param_5, param_6);
    return check.returns(gptr<float>(
        simple_textured_strips(param_1, param_2, param_3.a, static_cast<uint32_t>(param_4), param_5, param_6.a,
                               false)));
}

// Textured strips of a Simple model, both faces.
gptr<float> njCnkSimpleTextureDb(float param_1, float param_2, gptr<short> param_3, int param_4, uint param_5,
                                 gptr<float> param_6, float, float, uint, undefined4) {
    diff::Check check("njCnkSimpleTextureDb", 0x8C025888u, param_1, param_2, param_3, param_4, param_5, param_6);
    return check.returns(gptr<float>(
        simple_textured_strips(param_1, param_2, param_3.a, static_cast<uint32_t>(param_4), param_5, param_6.a, true)));
}

// Textured strips of a Simple model that the near plane may cut, front faces only.
gptr<ulonglong> njCnkSimpleTextureClip(gptr<short> param_1, int param_2, uint param_3, gptr<ulonglong> param_4,
                                       float arg_fr4, float arg_fr5, float, float) {
    diff::Check check("njCnkSimpleTextureClip", 0x8C02209Cu, param_1, param_2, param_3, param_4, arg_fr4, arg_fr5);
    return check.returns(gptr<ulonglong>(
        simple_textured_strips(arg_fr4, arg_fr5, param_1.a, static_cast<uint32_t>(param_2), param_3, param_4.a, false,
                               true)));
}

// Textured strips of a Simple model that the near plane may cut, both faces.
gptr<ulonglong> njCnkSimpleTextureDbClip(gptr<short> param_1, int param_2, uint param_3, gptr<ulonglong> param_4,
                                         float arg_fr4, float arg_fr5, float, float) {
    diff::Check check("njCnkSimpleTextureDbClip", 0x8C02503Cu, param_1, param_2, param_3, param_4, arg_fr4, arg_fr5);
    return check.returns(gptr<ulonglong>(
        simple_textured_strips(arg_fr4, arg_fr5, param_1.a, static_cast<uint32_t>(param_2), param_3, param_4.a, true,
                               true)));
}

// Untextured strips of a Simple model that the near plane may cut, lit as the textured ones, with
// the ambient light in `arg_fr4`.
gptr<ulonglong> njCnkSimplePolygonClip(gptr<short> param_1, int param_2, uint param_3, gptr<ulonglong> param_4,
                                       float arg_fr4, float arg_fr5, float arg_fr6, float arg_fr7, float arg_fr8,
                                       float arg_fr9, float arg_fr10, float arg_fr11) {
    diff::Check check("njCnkSimplePolygonClip", 0x8C01D14Cu, param_1, param_2, param_3, param_4, arg_fr4, arg_fr5,
                      arg_fr6, arg_fr7, arg_fr8, arg_fr9, arg_fr10, arg_fr11);
    return check.returns(gptr<ulonglong>(simple_textured_strips(
        arg_fr4, 0.0f, param_1.a, static_cast<uint32_t>(param_2), param_3, param_4.a, false, true, false, false)));
}


// A textured triangle of a Simple model that the near plane cuts, from njCnkSimpleTextureClip: its
// vertices in the vertex buffer, where its vertices go, each vertex's u and v, the ambient light
// and the triangle's sign. A triangle with all three vertices beyond one edge of the screen is left
// out, and so is one that faces away: the determinant of the vertices' view positions (x, y and
// depth), times the sign, is positive. Otherwise each edge (c to a, a to b, b to c) that crosses the
// depth 1 gives a vertex there, and each vertex deeper than 1 stays; the three or four vertices go
// out as one strip. A vertex's light is the buffer's plus the ambient light, never less than it, and
// the light and the highlight are interpolated as u and v are. The vertex count at 0x8C10AFB0
// grows by the vertices sent.
gptr<dword> njCnkSimpleTextureNearClip(gptr<float> param_1, gptr<float> param_2, gptr<float> param_3,
                                       gptr<dword> param_4, float arg_fr4, float arg_fr5, float arg_fr6,
                                       float arg_fr7, float arg_fr8, float arg_fr9, float param_11, float param_12) {
    diff::Check check("njCnkSimpleTextureNearClip", 0x8C028044u, param_1, param_2, param_3, param_4, arg_fr4,
                      arg_fr5, arg_fr6, arg_fr7, arg_fr8, arg_fr9, param_11, param_12);
    if (word(0x8C10AF98u) & 0x10000)
        return check.returns(param_4);
    const auto code = [](uint32_t v) { return outcode(real(v + 4), real(v + 8), real(v + 12)); };
    if (code(param_1.a) & code(param_2.a) & code(param_3.a))
        return check.returns(param_4);
    const float cx = real(0x8C10AF3Cu), cy = real(0x8C10AF40u), ambient = param_11;
    // A vertex: its words as they go out (x, y, 1/w, u, v, light, highlight), and for the clipping
    // its x and y in the view, and its depth.
    struct Vertex {
        uint32_t x, y, q, u, v, light, highlight;
        float vx, vy, depth;
    };
    const auto vertex = [&](uint32_t at, float u, float v) {
        float light = real(at + 16) + ambient;
        if (ambient > light)
            light = ambient;
        const float depth = real(at);
        return Vertex{word(at + 4),      word(at + 8),   word(at + 12),
                      bits_of(u),        bits_of(v),     bits_of(light),
                      word(at + 20),     (real(at + 4) - cx) * depth,
                      (real(at + 8) - cy) * depth, depth};
    };
    const Vertex in[3] = {vertex(param_1.a, arg_fr4, arg_fr5), vertex(param_2.a, arg_fr6, arg_fr7),
                          vertex(param_3.a, arg_fr8, arg_fr9)};
    const Vertex &a = in[0], &b = in[1], &c = in[2];
    const float e0 = b.depth * a.vy - b.vy * a.depth;
    const float e1 = b.vx * a.depth - b.depth * a.vx;
    const float e2 = b.vy * a.vx - b.vx * a.vy;
    float facing = std::fma(e0, c.vx, c.vy * e1);
    facing = std::fma(e2, c.depth, facing) * param_12;
    if (facing > 0.0f)
        return check.returns(param_4);
    Vertex out[6] = {};
    uint32_t n = 0;
    const Vertex* previous = &in[2];
    for (const Vertex& current : in) {
        const bool deep = current.depth > 1.0f;
        if (deep != (previous->depth > 1.0f)) {
            const float t = (-previous->depth + 1.0f) / (current.depth - previous->depth);
            const auto lerp = [t](float p, float q) { return std::fma(t, q - p, p); };
            const auto lerp_bits = [&](uint32_t p, uint32_t q) { return bits_of(lerp(from_bits(p), from_bits(q))); };
            Vertex& r = out[n++];
            r.x = bits_of(lerp(previous->vx, current.vx) + cx);
            r.y = bits_of(lerp(previous->vy, current.vy) + cy);
            r.q = bits_of(1.0f);
            r.u = lerp_bits(previous->u, current.u);
            r.v = lerp_bits(previous->v, current.v);
            r.light = lerp_bits(previous->light, current.light);
            r.highlight = lerp_bits(previous->highlight, current.highlight);
        }
        if (deep)
            out[n++] = current;
        previous = &current;
    }
    uint32_t at = param_4.a;
    const auto send_vertex = [&at](const Vertex& r, uint32_t control) {
        const uint32_t words[8] = {control, r.x, r.y, r.q, r.u, r.v, r.light, r.highlight};
        for (uint32_t i = 0; i < 8; ++i) guest::store<uint32_t>(at + 4 * i, words[i]);
        hw::prefetch(at);
        at += 32;
    };
    send_vertex(out[0], kVertex);
    send_vertex(out[1], kVertex);
    if (n != 3)
        send_vertex(out[3], kVertex);
    send_vertex(out[2], kLastVertex);
    guest::store<uint32_t>(0x8C10AFB0u, word(0x8C10AFB0u) + n);
    return check.returns(gptr<dword>(at));
}

// An untextured triangle of a Simple model that the near plane cuts, from njCnkSimplePolygonClip, as
// njCnkSimpleTextureNearClip without u, v and the highlight: a vertex's words 4, 5 and 7 keep what
// the queue held. `arg_fr4` is the ambient light, `arg_fr5` the triangle's sign.
gptr<dword> njCnkSimplePolygonNearClip(gptr<float> param_1, gptr<float> param_2, gptr<float> param_3,
                                       gptr<dword> param_4, float arg_fr4, float arg_fr5) {
    diff::Check check("njCnkSimplePolygonNearClip", 0x8C020E80u, param_1, param_2, param_3, param_4, arg_fr4,
                      arg_fr5);
    if (word(0x8C10AF98u) & 0x10000)
        return check.returns(param_4);
    const auto code = [](uint32_t v) { return outcode(real(v + 4), real(v + 8), real(v + 12)); };
    if (code(param_1.a) & code(param_2.a) & code(param_3.a))
        return check.returns(param_4);
    const float cx = real(0x8C10AF3Cu), cy = real(0x8C10AF40u), ambient = arg_fr4;
    struct Vertex {
        uint32_t x, y, q, light;
        float vx, vy, depth;
    };
    const auto vertex = [&](uint32_t at) {
        float light = real(at + 16) + ambient;
        if (ambient > light)
            light = ambient;
        const float depth = real(at);
        return Vertex{word(at + 4),           word(at + 8),           word(at + 12), bits_of(light),
                      (real(at + 4) - cx) * depth, (real(at + 8) - cy) * depth, depth};
    };
    const Vertex in[3] = {vertex(param_1.a), vertex(param_2.a), vertex(param_3.a)};
    const Vertex &a = in[0], &b = in[1], &c = in[2];
    const float e0 = b.depth * a.vy - b.vy * a.depth;
    const float e1 = b.vx * a.depth - b.depth * a.vx;
    const float e2 = b.vy * a.vx - b.vx * a.vy;
    float facing = std::fma(e0, c.vx, c.vy * e1);
    facing = std::fma(e2, c.depth, facing) * arg_fr5;
    if (facing > 0.0f)
        return check.returns(param_4);
    Vertex out[4] = {};
    uint32_t n = 0;
    const Vertex* previous = &in[2];
    for (const Vertex& current : in) {
        const bool deep = current.depth > 1.0f;
        if (deep != (previous->depth > 1.0f)) {
            const float t = (-previous->depth + 1.0f) / (current.depth - previous->depth);
            const auto lerp = [t](float p, float q) { return std::fma(t, q - p, p); };
            Vertex& r = out[n++];
            r.x = bits_of(cx + lerp(previous->vx, current.vx));
            r.y = bits_of(cy + lerp(previous->vy, current.vy));
            r.q = bits_of(1.0f);
            r.light = bits_of(lerp(from_bits(previous->light), from_bits(current.light)));
        }
        if (deep)
            out[n++] = current;
        previous = &current;
    }
    uint32_t at = param_4.a;
    const auto send_vertex = [&at](const Vertex& r, uint32_t control) {
        guest::store<uint32_t>(at, control);
        guest::store<uint32_t>(at + 4, r.x);
        guest::store<uint32_t>(at + 8, r.y);
        guest::store<uint32_t>(at + 12, r.q);
        guest::store<uint32_t>(at + 24, r.light);
        hw::prefetch(at);
        at += 32;
    };
    send_vertex(out[0], kVertex);
    send_vertex(out[1], kVertex);
    if (n != 3)
        send_vertex(out[3], kVertex);
    send_vertex(out[2], kLastVertex);
    guest::store<uint32_t>(0x8C10AFB0u, word(0x8C10AFB0u) + n);
    return check.returns(gptr<dword>(at));
}

// A textured triangle of a Simple model with both faces that the near plane cuts, from
// njCnkSimpleTextureDbClip, with the arguments of njCnkSimpleTextureNearClip. No triangle is left
// out. One whose determinant, times the sign, is negative is lit from behind: the ambient light
// minus the buffer's, and the highlight negated. Here the view's x and y take the projection's
// scales at 0x8C10AF44 and 0x8C10AF48, and a vertex at the depth 1 goes back to the screen through
// the scales at 0x8C10AF34 and 0x8C10AF38.
gptr<dword> njCnkSimpleTextureDbNearClip(gptr<float> param_1, gptr<float> param_2, gptr<float> param_3,
                                         gptr<dword> param_4, float arg_fr4, float arg_fr5, float arg_fr6,
                                         float arg_fr7, float arg_fr8, float arg_fr9, float param_11, float param_12) {
    diff::Check check("njCnkSimpleTextureDbNearClip", 0x8C0253F8u, param_1, param_2, param_3, param_4, arg_fr4,
                      arg_fr5, arg_fr6, arg_fr7, arg_fr8, arg_fr9, param_11, param_12);
    if (word(0x8C10AF98u) & 0x10000)
        return check.returns(param_4);
    const float sx = real(0x8C10AF34u), sy = real(0x8C10AF38u), cx = real(0x8C10AF3Cu), cy = real(0x8C10AF40u);
    const float vsx = real(0x8C10AF44u), vsy = real(0x8C10AF48u), ambient = param_11;
    struct Vertex {
        uint32_t x, y, q;
        float vx, vy, depth;
        uint32_t u, v, light, highlight;
    };
    const auto vertex = [&](uint32_t at, float u, float v) {
        const float depth = real(at);
        return Vertex{word(at + 4),
                      word(at + 8),
                      word(at + 12),
                      (real(at + 4) - cx) * depth * vsx,
                      (real(at + 8) - cy) * depth * vsy,
                      depth,
                      bits_of(u),
                      bits_of(v),
                      0,
                      0};
    };
    Vertex in[3] = {vertex(param_1.a, arg_fr4, arg_fr5), vertex(param_2.a, arg_fr6, arg_fr7),
                    vertex(param_3.a, arg_fr8, arg_fr9)};
    const Vertex &a = in[0], &b = in[1], &c = in[2];
    const float e0 = b.depth * a.vy - b.vy * a.depth;
    const float e1 = b.vx * a.depth - b.depth * a.vx;
    const float e2 = b.vy * a.vx - b.vx * a.vy;
    float facing = std::fma(e0, c.vx, c.vy * e1);
    facing = std::fma(e2, c.depth, facing) * param_12;
    const bool behind = 0.0f > facing;
    const uint32_t from[3] = {param_1.a, param_2.a, param_3.a};
    for (uint32_t i = 0; i < 3; ++i) {
        const float light = real(from[i] + 16);
        in[i].light = bits_of(behind ? ambient - light : ambient + light);
        in[i].highlight = word(from[i] + 20) ^ (behind ? 0x80000000u : 0);
    }
    for (Vertex& v : in)
        if (ambient > from_bits(v.light))
            v.light = bits_of(ambient);
    Vertex out[4] = {};
    uint32_t n = 0;
    const Vertex* previous = &in[2];
    for (const Vertex& current : in) {
        const bool deep = current.depth > 1.0f;
        if (deep != (previous->depth > 1.0f)) {
            const float t = (-previous->depth + 1.0f) / (current.depth - previous->depth);
            const auto lerp = [t](float p, float q) { return std::fma(t, q - p, p); };
            const auto lerp_bits = [&](uint32_t p, uint32_t q) { return bits_of(lerp(from_bits(p), from_bits(q))); };
            Vertex& r = out[n++];
            r.x = bits_of(std::fma(sx, lerp(previous->vx, current.vx), cx));
            r.y = bits_of(std::fma(sy, lerp(previous->vy, current.vy), cy));
            r.q = bits_of(1.0f);
            r.u = lerp_bits(previous->u, current.u);
            r.v = lerp_bits(previous->v, current.v);
            r.light = lerp_bits(previous->light, current.light);
            r.highlight = lerp_bits(previous->highlight, current.highlight);
        }
        if (deep)
            out[n++] = current;
        previous = &current;
    }
    uint32_t at = param_4.a;
    const auto send_vertex = [&at](const Vertex& r, uint32_t control) {
        const uint32_t words[8] = {control, r.x, r.y, r.q, r.u, r.v, r.light, r.highlight};
        for (uint32_t i = 0; i < 8; ++i) guest::store<uint32_t>(at + 4 * i, words[i]);
        hw::prefetch(at);
        at += 32;
    };
    send_vertex(out[0], kVertex);
    send_vertex(out[1], kVertex);
    if (n != 3)
        send_vertex(out[3], kVertex);
    send_vertex(out[2], kLastVertex);
    guest::store<uint32_t>(0x8C10AFB0u, word(0x8C10AFB0u) + n);
    return check.returns(gptr<dword>(at));
}

// A chunk object tree without a motion: each object in its own matrix, moved, turned and scaled as
// its flags allow, then its model through the draw function `param_2`, then its children. The flags
// leave out the move (1), the turn (2), the scale (4), the model (8) and the children (16); with
// all of the first three, the object keeps its parent's matrix. Flag 32 turns the angles in the
// other order, 0x400 turns by the quaternion, 0x800 keeps the current matrix at 0x8C10B5AC first, and
// 0x1000 turns by that matrix's rotation. With 0x100, the children are left out when the draw
// function returns other than 0, as it does for a model outside the view.
void njCnkTransformObject(gptr<uint> param_1, gptr<undefined> param_2) {
    diff::Check check("njCnkTransformObject", 0x8C02F490u, param_1, param_2);
    constexpr uint32_t kKept = 0x8C10B5ACu;
    uint32_t object = param_1.a;
    do {
        const uint32_t flags = word(object);
        uint32_t drawn = 0;
        if (flags & 0x800)
            njGetMatrix(gptr<NJS_MATRIX>(kKept));
        const bool own = (flags & 7) != 7;
        if (own) {
            njPushMatrixEx();
            if (!(flags & 1))
                njTranslateEx(gptr<NJS_VECTOR>(object + 8));
            if (!(flags & 2)) {
                if (flags & 0x1000)
                    _njSetRotate(gptr<undefined4>(kKept));
                else if (flags & 0x400)
                    njQuaternionEx2(real(object + 52), gptr<Float>(object + 20));
                else
                    njRotateEx(gptr<Angle>(object + 20), static_cast<Sint32>(flags & 32));
            }
            if (!(flags & 4))
                njScaleEx(gptr<NJS_VECTOR>(object + 32));
        }
        if (!(flags & 8))
            drawn = call_with(param_2.a, word(object + 4));
        if (!(flags & 16) && (!(flags & 0x100) || drawn == 0))
            njCnkTransformObject(gptr<uint>(word(object + 44)), param_2);
        if (own)
            njPopMatrixEx();
        object = word(object + 48);
    } while (object);
}

// Vertices with a colour: CV_D8 chunks of a Simple model. Each vertex goes to the vertex buffer as 32
// bytes: -z, the screen position, 1/w, the colour, 0, and the turned vertex's x and y. Returns the
// vertices not more than 1 in front of the camera.
int njCnkSimpleVertexVD8(int param_1) {
    diff::Check check("njCnkSimpleVertexVD8", 0x8C0287C4u, param_1);
    const uint32_t context = static_cast<uint32_t>(param_1);
    uint32_t count = word(context + kCount), data = word(context + kData), out = word(context + kOut);
    const float sx = real(kProjection), sy = real(kProjection + 4), ox = real(kProjection + 8),
                oy = real(kProjection + 12);
    int near = 0;
    do {
        const Vec4 p = transform(real(data), real(data + 4), real(data + 8));
        const uint32_t colour = word(data + 12);
        data += 16;
        const float depth = -p.z, q = 1.0f / depth;
        if (!(depth > 1.0f))
            ++near;
        const uint32_t words[8] = {bits_of(depth), bits_of(p.x * sx * q + ox), bits_of(p.y * sy * q + oy),
                                   bits_of(q),     colour,                     bits_of(0.0f),
                                   bits_of(p.x),   bits_of(p.y)};
        for (uint32_t i = 0; i < 8; ++i) guest::store<uint32_t>(out + 4 * i, words[i]);
        hw::prefetch(out);
        out += 32;
    } while (--count != 0);
    return check.returns(near);
}

// Textured strips of a Simple model with vertex colours, front faces only: `param_1` is the u and
// v scale.
gptr<float> njCnkSimpleTextureD8(float param_1, gptr<short> param_2, int param_3, uint param_4, gptr<float> param_5,
                                 float, float, float, uint, undefined4) {
    diff::Check check("njCnkSimpleTextureD8", 0x8C024E7Cu, param_1, param_2, param_3, param_4, param_5);
    return check.returns(gptr<float>(
        simple_textured_strips(0.0f, param_1, param_2.a, static_cast<uint32_t>(param_3), param_4, param_5.a, false,
                               false, true)));
}

// Textured strips of a Simple model with vertex colours that the near plane may cut, front faces
// only: `arg_fr4` is the u and v scale.
gptr<dword> njCnkSimpleTextureD8Clip(gptr<short> param_1, int param_2, uint param_3, gptr<dword> param_4,
                                     float arg_fr4, float, float, float, float, float, float, float) {
    diff::Check check("njCnkSimpleTextureD8Clip", 0x8C022320u, param_1, param_2, param_3, param_4, arg_fr4);
    return check.returns(gptr<dword>(
        simple_textured_strips(0.0f, arg_fr4, param_1.a, static_cast<uint32_t>(param_2), param_3, param_4.a, false,
                               true, true)));
}

// A textured triangle of a Simple model with vertex colours that the near plane cuts, from
// njCnkSimpleTextureD8Clip: as njCnkSimpleTextureNearClip, but without the test against the screen's
// edges, and with the sign in `arg_fr10`. The view positions are scaled by the words at 16 and 20 of
// the projection, which turns the determinant round: a triangle whose determinant times the sign
// is negative faces away. A vertex at the near plane goes through the projection again, and the
// two colours of each vertex are interpolated byte by byte.
gptr<dword> njCnkSimpleTextureD8NearClip(gptr<float> param_1, gptr<float> param_2, gptr<float> param_3,
                                         gptr<dword> param_4, float arg_fr4, float arg_fr5, float arg_fr6,
                                         float arg_fr7, float arg_fr8, float arg_fr9, float arg_fr10,
                                         float arg_fr11) {
    diff::Check check("njCnkSimpleTextureD8NearClip", 0x8C02480Cu, param_1, param_2, param_3, param_4, arg_fr4,
                      arg_fr5, arg_fr6, arg_fr7, arg_fr8, arg_fr9, arg_fr10, arg_fr11);
    if (word(0x8C10AF98u) & 0x10000)
        return check.returns(param_4);
    const float sx = real(kProjection), sy = real(kProjection + 4), ox = real(kProjection + 8),
                oy = real(kProjection + 12), kx = real(kProjection + 16), ky = real(kProjection + 20);
    struct Vertex {
        uint32_t x, y, q, u, v, colour, offset;
        float vx, vy, depth;
    };
    const auto vertex = [&](uint32_t at, float u, float v) {
        const float depth = real(at);
        return Vertex{word(at + 4),  word(at + 8),  word(at + 12),
                      bits_of(u),    bits_of(v),    word(at + 16),
                      word(at + 20), (real(at + 4) - ox) * depth * kx,
                      (real(at + 8) - oy) * depth * ky, depth};
    };
    const Vertex in[3] = {vertex(param_1.a, arg_fr4, arg_fr5), vertex(param_2.a, arg_fr6, arg_fr7),
                          vertex(param_3.a, arg_fr8, arg_fr9)};
    const Vertex &a = in[0], &b = in[1], &c = in[2];
    const float e0 = b.depth * a.vy - b.vy * a.depth;
    const float e1 = b.vx * a.depth - b.depth * a.vx;
    const float e2 = b.vy * a.vx - b.vx * a.vy;
    float facing = std::fma(e0, c.vx, c.vy * e1);
    facing = std::fma(e2, c.depth, facing) * arg_fr10;
    if (0.0f > facing)
        return check.returns(param_4);
    Vertex out[6] = {};
    uint32_t n = 0;
    const Vertex* previous = &in[2];
    for (const Vertex& current : in) {
        const bool deep = current.depth > 1.0f;
        if (deep != (previous->depth > 1.0f)) {
            const float t = (-previous->depth + 1.0f) / (current.depth - previous->depth);
            const auto lerp = [t](float p, float q) { return std::fma(t, q - p, p); };
            const auto lerp_bits = [&](uint32_t p, uint32_t q) { return bits_of(lerp(from_bits(p), from_bits(q))); };
            Vertex& r = out[n++];
            r.x = bits_of(std::fma(sx, lerp(previous->vx, current.vx), ox));
            r.y = bits_of(std::fma(sy, lerp(previous->vy, current.vy), oy));
            r.q = bits_of(1.0f);
            r.u = lerp_bits(previous->u, current.u);
            r.v = lerp_bits(previous->v, current.v);
            r.colour = mix_colour(previous->colour, current.colour, t);
            r.offset = mix_colour(previous->offset, current.offset, t);
        }
        if (deep)
            out[n++] = current;
        previous = &current;
    }
    uint32_t at = param_4.a;
    const auto send_vertex = [&at](const Vertex& r, uint32_t control) {
        const uint32_t words[8] = {control, r.x, r.y, r.q, r.u, r.v, r.colour, r.offset};
        for (uint32_t i = 0; i < 8; ++i) guest::store<uint32_t>(at + 4 * i, words[i]);
        hw::prefetch(at);
        at += 32;
    };
    send_vertex(out[0], kVertex);
    send_vertex(out[1], kVertex);
    if (n != 3)
        send_vertex(out[3], kVertex);
    send_vertex(out[2], kLastVertex);
    guest::store<uint32_t>(0x8C10AFB0u, word(0x8C10AFB0u) + n);
    return check.returns(gptr<dword>(at));
}

// A textured triangle of a multi-light model that the near plane cuts, from njCnkEasyMultiTextureClip:
// its vertices in the vertex buffer, where its vertices go, each vertex's u and v, the factor of the
// alpha and the colour's red; its green and blue are on the stack. A triangle with all three
// vertices beyond one edge of the screen is left out (see outcode), and so is every triangle with
// bit 16 of Ninja's flags. Otherwise each edge (c to a, a to b, b to c) that crosses the depth 1
// gives a vertex there, interpolated in the view's x and y, and each vertex more than 1 deep stays.
// A vertex's colour is its light, interpolated, plus the colour, and its alpha its light's alpha
// times the factor; flat strips (0x8C0CCD88) take the third vertex's for the last two. The machine
// code builds the vertices in its frame, 9 words each, and so does this: what it does not write
// there, as the colours of a flat strip's first two vertices, is what the frame held. The three or
// four vertices go out as one strip, two blocks each, and the vertex count at 0x8C10AFB0 grows by
// them.
gptr<float> njCnkEasyMultiTextureNearClip(gptr<float> param_1, gptr<float> param_2, gptr<float> param_3,
                                          gptr<float> param_4, float param_5, float param_6, float arg_fr6,
                                          float arg_fr7, float arg_fr8, float arg_fr9, float arg_fr10, float arg_fr11) {
    diff::Check check("njCnkEasyMultiTextureNearClip", 0x8C01AF28u, param_1, param_2, param_3, param_4, param_5,
                      param_6, arg_fr6, arg_fr7, arg_fr8, arg_fr9, arg_fr10, arg_fr11);
    if (word(0x8C10AF98u) & 0x10000)
        return check.returns(param_4);
    const auto code = [](uint32_t v) { return outcode(real(v + 4), real(v + 8), real(v + 12)); };
    if (code(param_1.a) & code(param_2.a) & code(param_3.a))
        return check.returns(param_4);
    const float green = real(guest::sp), blue = real(guest::sp + 4);
    const float cx = real(0x8C10AF3Cu), cy = real(0x8C10AF40u);
    // A vertex in the buffer, its u and v, and its x and y in the view.
    struct Vertex {
        uint32_t at;
        float u, v, vx, vy;
    };
    const auto vertex = [&](uint32_t at, float u, float v) {
        return Vertex{at, u, v, (real(at + 4) - cx) * real(at), (real(at + 8) - cy) * real(at)};
    };
    const Vertex in[3] = {vertex(param_1.a, param_5, param_6), vertex(param_2.a, arg_fr6, arg_fr7),
                          vertex(param_3.a, arg_fr8, arg_fr9)};
    // The vertices: x, y, 1/w, u, v, red, green, blue and alpha.
    const uint32_t frame = guest::sp - 244;
    const auto slot = [frame](uint32_t i, uint32_t k) { return frame + 36 * i + 4 * k; };
    const auto colour = [&](uint32_t i, float r, float g, float b, float a) {
        const float words[4] = {r + arg_fr11, g + green, b + blue, a * arg_fr10};
        for (uint32_t k = 0; k < 4; ++k) guest::store<float>(slot(i, 5 + k), words[k]);
    };
    const bool flat = word(0x8C0CCD88u) != 0;
    uint32_t n = 0;
    const Vertex* previous = &in[2];
    for (const Vertex& current : in) {
        const bool beyond = real(current.at) > 1.0f;
        if (beyond != (real(previous->at) > 1.0f)) {
            const float t = (-real(previous->at) + 1.0f) / (real(current.at) - real(previous->at));
            const auto lerp = [t](float a, float b) { return std::fma(t, b - a, a); };
            const auto light = [&](uint32_t k) { return lerp(real(previous->at + k), real(current.at + k)); };
            const float words[5] = {cx + lerp(previous->vx, current.vx), cy + lerp(previous->vy, current.vy), 1.0f,
                                    lerp(previous->u, current.u), lerp(previous->v, current.v)};
            for (uint32_t k = 0; k < 5; ++k) guest::store<float>(slot(n, k), words[k]);
            if (!flat)
                colour(n, light(20), light(24), light(28), light(16));
            ++n;
        }
        if (beyond) {
            const uint32_t words[5] = {word(current.at + 4), word(current.at + 8), word(current.at + 12),
                                       bits_of(current.u), bits_of(current.v)};
            for (uint32_t k = 0; k < 5; ++k) guest::store<uint32_t>(slot(n, k), words[k]);
            if (!flat)
                colour(n, real(current.at + 20), real(current.at + 24), real(current.at + 28), real(current.at + 16));
            ++n;
        }
        previous = &current;
    }
    if (flat) {
        const uint32_t c = param_3.a;
        colour(2, real(c + 20), real(c + 24), real(c + 28), real(c + 16));
        if (n != 3)
            colour(3, real(c + 20), real(c + 24), real(c + 28), real(c + 16));
    }
    uint32_t at = param_4.a;
    const auto send_vertex = [&](uint32_t i, uint32_t control) {
        const uint32_t first[6] = {control,       word(slot(i, 0)), word(slot(i, 1)),
                                   word(slot(i, 2)), word(slot(i, 3)), word(slot(i, 4))};
        for (uint32_t k = 0; k < 6; ++k) guest::store<uint32_t>(at + 4 * k, first[k]);
        hw::prefetch(at);
        at += 32;
        const float r = real(slot(i, 5)), g = real(slot(i, 6)), b = real(slot(i, 7));
        const float second[4] = {real(slot(i, 8)), r, g, b};
        for (uint32_t k = 0; k < 4; ++k) guest::store<float>(at + 4 * k, second[k]);
        const float offset[3] = {r + -1.0f, g + -1.0f, b + -1.0f};
        for (uint32_t k = 0; k < 3; ++k) guest::store<float>(at + 20 + 4 * k, offset[k]);
        hw::prefetch(at);
        at += 32;
    };
    send_vertex(0, kVertex);
    send_vertex(1, kVertex);
    if (n != 3)
        send_vertex(3, kVertex);
    send_vertex(2, kLastVertex);
    guest::store<uint32_t>(0x8C10AFB0u, word(0x8C10AFB0u) + n);
    return check.returns(gptr<float>(at));
}

// Textured strips of a multi-light model that the near plane may cut, with the arguments of
// njCnkEasyMultiTexture and the u and v scale in `arg_fr8`. A triangle with a vertex less than 1 deep
// breaks the strip, and when one of its vertices is not, it goes to the near clip with the
// colours, green and blue on the stack.
gptr<float> njCnkEasyMultiTextureClip(gptr<short> param_1, int param_2, uint param_3, gptr<float> param_4,
                                      float arg_fr4, float arg_fr5, float arg_fr6, float arg_fr7, float arg_fr8,
                                      float arg_fr9) {
    diff::Check check("njCnkEasyMultiTextureClip", 0x8C01ACA8u, param_1, param_2, param_3, param_4, arg_fr4,
                      arg_fr5, arg_fr6, arg_fr7, arg_fr8, arg_fr9);
    uint32_t in = param_1.a, out = param_4.a, strips = param_3 & 0x3FFF;
    const uint32_t buffer = static_cast<uint32_t>(param_2), user = ((param_3 >> 14) & 3) * 2;
    const float red = arg_fr5 - 1.0f, green = arg_fr6 - 1.0f, blue = arg_fr7 - 1.0f;
    struct Corner {
        uint32_t at;
        float u, v;
    };
    const auto next = [&in] {
        const int32_t v = guest::load<int16_t>(in);
        in += 2;
        return v;
    };
    const auto read = [&] {
        Corner c;
        c.at = buffer + (static_cast<uint32_t>(next()) << 5);
        c.u = static_cast<float>(next()) * arg_fr8;
        c.v = static_cast<float>(next()) * arg_fr8;
        return c;
    };
    const auto emit = [&](const Corner& c, uint32_t control) {
        const uint32_t first[6] = {control, word(c.at + 4), word(c.at + 8), word(c.at + 12), bits_of(c.u),
                                   bits_of(c.v)};
        for (uint32_t i = 0; i < 6; ++i) guest::store<uint32_t>(out + 4 * i, first[i]);
        hw::prefetch(out);
        out += 32;
        const float r = real(c.at + 20) + red, g = real(c.at + 24) + green, b = real(c.at + 28) + blue;
        const float alpha = real(c.at + 16) * arg_fr4;
        const float second[8] = {alpha, r + 1.0f, g + 1.0f, b + 1.0f, alpha, r, g, b};
        for (uint32_t i = 0; i < 8; ++i) guest::store<float>(out + 4 * i, second[i]);
        hw::prefetch(out);
        out += 32;
    };
    const auto near = [](uint32_t at) { return 1.0f > real(at); };
    uint32_t sent = 0, total = 0;
    do {
        int32_t length = next();
        if (length < 0)
            length = -length;
        total += static_cast<uint32_t>(length);
        Corner a = read(), b = read();
        uint32_t open = 0;
        uint32_t triangles = static_cast<uint32_t>(length - 2);
        do {
            const Corner c = read();
            if (!near(a.at) && !near(b.at) && !near(c.at)) {
                if (open == 0) {
                    emit(a, kVertex);
                    ++open;
                }
                emit(b, kVertex);
                ++open;
            }
            else {
                if (open) {
                    emit(b, kLastVertex);
                    sent += open + 1;
                    open = 0;
                }
                if (!near(a.at) || !near(b.at) || !near(c.at)) {
                    guest::Frame frame(8);
                    guest::store<float>(guest::sp, green + 1.0f);
                    guest::store<float>(guest::sp + 4, blue + 1.0f);
                    out = njCnkEasyMultiTextureNearClip(gptr<float>(a.at), gptr<float>(b.at), gptr<float>(c.at),
                                                        gptr<float>(out), a.u, a.v, b.u, b.v, c.u, c.v, arg_fr4,
                                                        red + 1.0f)
                              .a;
                }
            }
            in += user;
            a = b;
            b = c;
        } while (--triangles != 0);
        if (open) {
            emit(b, kLastVertex);
            sent += open + 1;
        }
    } while (--strips != 0);
    guest::store<uint32_t>(0x8C10AFB0u, word(0x8C10AFB0u) + sent);
    guest::store<uint32_t>(0x8C10AFACu, word(0x8C10AFACu) + total);
    return check.returns(gptr<float>(out));
}

// Untextured strips of an Easy model: each vertex's screen position, 1/w and colour or light from
// the vertex buffer (16 bytes for each vertex). The offset colour's word gets the control word of a
// strip's last vertex, which Ninja keeps in fr9; the u and v words keep what the queue held.
gptr<ulonglong> njCnkEasyPolygon(gptr<short> param_1, int param_2, uint param_3, gptr<ulonglong> param_4) {
    diff::Check check("njCnkEasyPolygon", 0x8C01835Cu, param_1, param_2, param_3, param_4);
    uint32_t in = param_1.a, out = param_4.a, strips = param_3 & 0x3FFF;
    const uint32_t buffer = static_cast<uint32_t>(param_2), user = ((param_3 >> 14) & 3) * 2;
    const auto next = [&] {
        const int32_t i = guest::load<int16_t>(in);
        in += 2;
        return buffer + (static_cast<uint32_t>(i) << 4);
    };
    const auto send_vertex = [&out](uint32_t v, uint32_t control) {
        guest::store<uint32_t>(out, control);
        for (uint32_t i = 0; i < 12; i += 4) guest::store<uint32_t>(out + 4 + i, word(v + i));
        guest::store<uint32_t>(out + 24, word(v + 12));
        guest::store<uint32_t>(out + 28, kLastVertex);
        hw::prefetch(out);
        out += 32;
    };
    uint32_t total = 0;
    do {
        int32_t length = guest::load<int16_t>(in);
        in += 2;
        if (length < 0)
            length = -length;
        total += static_cast<uint32_t>(length);
        uint32_t current = next();
        uint32_t triangles = static_cast<uint32_t>(length - 2);
        const uint32_t second = next();
        send_vertex(current, kVertex);
        current = second;
        do {
            const uint32_t following = next();
            in += user;
            send_vertex(current, kVertex);
            current = following;
        } while (--triangles != 0);
        send_vertex(current, kLastVertex);
    } while (--strips != 0);
    guest::store<uint32_t>(0x8C10AFB0u, word(0x8C10AFB0u) + total);
    guest::store<uint32_t>(0x8C10AFACu, word(0x8C10AFACu) + total);
    return check.returns(gptr<ulonglong>(out));
}

// Untextured strips of an Easy model that the near plane may cut, as njCnkEasyPolygon draws them,
// but only a triangle whose three vertices have 1/w in (0, 1] goes out. Otherwise the strip breaks,
// and a triangle with a vertex in front of the near plane, 1/w in (0, 1), goes to the near clip. A
// vertex's u, v and offset colour words keep what the queue held.
gptr<float> njCnkEasyPolygonClip(gptr<short> param_1, int param_2, uint param_3, gptr<float> param_4, float arg_fr4,
                                 float arg_fr5, float arg_fr6, float arg_fr7, float arg_fr8, float arg_fr9,
                                 float arg_fr10, float param_12) {
    diff::Check check("njCnkEasyPolygonClip", 0x8C017C28u, param_1, param_2, param_3, param_4, arg_fr4, arg_fr5,
                      arg_fr6, arg_fr7, arg_fr8, arg_fr9, arg_fr10, param_12);
    uint32_t in = param_1.a, out = param_4.a, strips = param_3 & 0x3FFF;
    const uint32_t buffer = static_cast<uint32_t>(param_2), user = ((param_3 >> 14) & 3) * 2;
    const auto next = [&] {
        const int32_t i = guest::load<int16_t>(in);
        in += 2;
        return buffer + (static_cast<uint32_t>(i) << 4);
    };
    const auto send_vertex = [&out](uint32_t v, uint32_t control) {
        guest::store<uint32_t>(out, control);
        for (uint32_t i = 0; i < 12; i += 4) guest::store<uint32_t>(out + 4 + i, word(v + i));
        guest::store<uint32_t>(out + 24, word(v + 12));
        hw::prefetch(out);
        out += 32;
    };
    const auto q = [](uint32_t v) { return real(v + 8); };
    const auto ahead = [&](uint32_t v) { return q(v) > 0.0f && 1.0f > q(v); };
    uint32_t sent = 0, total = 0;
    do {
        int32_t length = guest::load<int16_t>(in);
        in += 2;
        if (length < 0)
            length = -length;
        total += static_cast<uint32_t>(length);
        uint32_t a = next(), b = next(), open = 0;
        uint32_t triangles = static_cast<uint32_t>(length - 2);
        do {
            const uint32_t c = next();
            if (q(a) > 0.0f && q(b) > 0.0f && q(c) > 0.0f && !(q(a) > 1.0f) && !(q(b) > 1.0f) && !(q(c) > 1.0f)) {
                if (open == 0) {
                    send_vertex(a, kVertex);
                    ++open;
                }
                send_vertex(b, kVertex);
                ++open;
            }
            else {
                if (open) {
                    send_vertex(b, kLastVertex);
                    sent += open + 1;
                    open = 0;
                }
                if (ahead(a) || ahead(b) || ahead(c))
                    out = njCnkEasyPolygonNearClip(gptr<float>(a), gptr<float>(b), gptr<float>(c), gptr<dword>(out)).a;
            }
            in += user;
            a = b;
            b = c;
        } while (--triangles != 0);
        if (open) {
            send_vertex(b, kLastVertex);
            sent += open + 1;
        }
    } while (--strips != 0);
    guest::store<uint32_t>(0x8C10AFB0u, word(0x8C10AFB0u) + sent);
    guest::store<uint32_t>(0x8C10AFACu, word(0x8C10AFACu) + total);
    return check.returns(gptr<float>(out));
}

// The lights of the multi-light models: their bits at 0x8C122710 (see njCnkEasyMultiVertexVN). The
// light set `param_1` takes its bits from the table at 0x8C0CA6A0.
void njCnkSetEasyMultiLight(int param_1) {
    diff::Check check("njCnkSetEasyMultiLight", 0x8C01A2C8u, param_1);
    guest::store<uint32_t>(0x8C122710u, word(0x8C0CA6A0u + 4 * static_cast<uint32_t>(param_1)));
}

// Switches light `param_1` on, or off when `param_2` is 0, with its bits from the table at 0x8C0CA6BC.
void njCnkSetEasyMultiLightSwitch(int param_1, int param_2) {
    diff::Check check("njCnkSetEasyMultiLightSwitch", 0x8C01A2EAu, param_1, param_2);
    const uint32_t bits = word(0x8C0CA6BCu + 4 * static_cast<uint32_t>(param_1)), lights = word(0x8C122710u);
    guest::store<uint32_t>(0x8C122710u, param_2 ? lights | bits : lights & ~bits);
}

// The same for the lights of the Simple multi-light models, whose bits are at 0x8C122830, from the
// tables at 0x8C0CA6F8 and 0x8C0CA714.
void njCnkSetSimpleMultiLight(int param_1) {
    diff::Check check("njCnkSetSimpleMultiLight", 0x8C029838u, param_1);
    guest::store<uint32_t>(0x8C122830u, word(0x8C0CA6F8u + 4 * static_cast<uint32_t>(param_1)));
}

void njCnkSetSimpleMultiLightSwitch(int param_1, int param_2) {
    diff::Check check("njCnkSetSimpleMultiLightSwitch", 0x8C02985Au, param_1, param_2);
    const uint32_t bits = word(0x8C0CA714u + 4 * static_cast<uint32_t>(param_1)), lights = word(0x8C122830u);
    guest::store<uint32_t>(0x8C122830u, param_2 ? lights | bits : lights & ~bits);
}

// An untextured triangle of an Easy model that the near plane cuts, from njCnkEasyPolygonClip: as
// njCnkEasyTextureNearClip, without u and v. With the flat flag (0x8C0CCD88), only the last two
// vertices get a colour, the third vertex's; the first two keep the colour words of the machine
// code's stack, which a flat strip does not use, and here keep 0.
gptr<dword> njCnkEasyPolygonNearClip(gptr<float> param_1, gptr<float> param_2, gptr<float> param_3,
                                     gptr<dword> param_4) {
    diff::Check check("njCnkEasyPolygonNearClip", 0x8C017DECu, param_1, param_2, param_3, param_4);
    if (word(0x8C10AF98u) & 0x10000)
        return check.returns(param_4);
    const auto code = [](uint32_t v) { return outcode(real(v), real(v + 4), real(v + 8)); };
    if (code(param_1.a) & code(param_2.a) & code(param_3.a))
        return check.returns(param_4);
    const float cx = real(0x8C10AF3Cu), cy = real(0x8C10AF40u);
    struct Vertex {
        uint32_t x, y, q, colour;
        float vx, vy, w;
    };
    const auto vertex = [&](uint32_t at) {
        Vertex r{word(at), word(at + 4), word(at + 8), word(at + 12), 0, 0, 0};
        r.w = 1.0f / real(at + 8);
        r.vx = (real(at) - cx) * r.w;
        r.vy = (real(at + 4) - cy) * r.w;
        return r;
    };
    const Vertex in[3] = {vertex(param_1.a), vertex(param_2.a), vertex(param_3.a)};
    const bool flat = word(0x8C0CCD88u) != 0, colours = word(0x8C10B430u) == 35;
    Vertex out[6] = {};
    uint32_t n = 0;
    const Vertex* previous = &in[2];
    for (const Vertex& current : in) {
        const bool beyond = current.w > 1.0f;
        if (beyond != (previous->w > 1.0f)) {
            const float t = (-previous->w + 1.0f) / (current.w - previous->w);
            const auto lerp = [t](float a, float b) { return std::fma(t, b - a, a); };
            Vertex& r = out[n++];
            r.x = bits_of(cx + lerp(previous->vx, current.vx));
            r.y = bits_of(cy + lerp(previous->vy, current.vy));
            r.q = bits_of(1.0f);
            if (!flat)
                r.colour = colours ? mix_colour(previous->colour, current.colour, t)
                                   : bits_of(lerp(from_bits(previous->colour), from_bits(current.colour)));
        }
        if (beyond) {
            Vertex& r = out[n++];
            r.x = current.x;
            r.y = current.y;
            r.q = current.q;
            if (!flat)
                r.colour = current.colour;
        }
        previous = &current;
    }
    if (flat)
        out[2].colour = out[3].colour = in[2].colour;
    uint32_t at = param_4.a;
    const auto send_vertex = [&at](const Vertex& r, uint32_t control) {
        const uint32_t words[4] = {control, r.x, r.y, r.q};
        for (uint32_t i = 0; i < 4; ++i) guest::store<uint32_t>(at + 4 * i, words[i]);
        guest::store<uint32_t>(at + 24, r.colour);
        hw::prefetch(at);
        at += 32;
    };
    send_vertex(out[0], kVertex);
    send_vertex(out[1], kVertex);
    if (n != 3)
        send_vertex(out[3], kVertex);
    send_vertex(out[2], kLastVertex);
    guest::store<uint32_t>(0x8C10AFB0u, word(0x8C10AFB0u) + n);
    return check.returns(gptr<dword>(at));
}

// Vertices with a normal for the Simple multi-light models, lit by the lights at 0x8C122830 (see
// multi_light). Each vertex goes to the buffer as 64 bytes: -z, the screen position, 1/w, 1 and
// the light's red, green and blue, then the turned vertex's x and y, the turned normal's x, negated
// y and z, and the light's red, green and blue minus 1. The screen y stays in fr7 for the next
// vertex's lights. Returns the vertices less than 1 in front of the camera.
int njCnkSimpleMultiVertexVN(gptr<int> param_1, gptr<ulonglong> param_2, ulonglong param_3) {
    diff::Check check("njCnkSimpleMultiVertexVN", 0x8C02F128u, param_1, param_2, param_3);
    const uint32_t context = param_1.a, projection = param_2.a;
    uint32_t count = word(context), data = word(context + 4), out = word(context + 12);
    float fr7 = 0.0f;
    int near = 0;
    do {
        const Vec4 p = transform(real(data), real(data + 4), real(data + 8));
        const Vec4 n = rotate(real(data + 12), real(data + 16), real(data + 20));
        data += 24;
        const Light l = multi_light(p, n, 0x8C122830u, fr7);
        const float depth = -p.z;
        if (1.0f > depth)
            ++near;
        const float q = 1.0f / depth;
        const float x = std::fma(q, real(projection) * p.x, real(projection + 8));
        fr7 = std::fma(q, real(projection + 4) * p.y, real(projection + 12));
        const float words[16] = {depth, x,   fr7, q,           1.0f,           l.r,         l.g,         l.b,
                                 p.x,   p.y, n.x, -n.y, n.z, -1.0f + l.r, l.g - 1.0f, l.b - 1.0f};
        for (uint32_t i = 0; i < 16; ++i) guest::store<float>(out + 4 * i, words[i]);
        hw::prefetch(out);
        hw::prefetch(out + 32);
        out += 64;
    } while (--count != 0);
    return check.returns(near);
}

namespace {

// Strips of a Simple multi-light model from the 64-byte vertices of njCnkSimpleMultiVertexVN. Each
// vertex goes out as two blocks: the control word, the screen position, 1/w, u and v, then the
// alpha, the colour and, as the offset colour, the light minus 1. `material` holds the factor of the
// alpha, the factors of the light's red, green and blue, and the colour's base red, green and blue.
// The words that the blocks leave out keep what the queue held. A triangle faces the camera when
// the cross product of its edges on the screen, times a sign that alternates along the strip, is
// negative; a strip given backwards, or bit 31 of Ninja's flags, turns the sign round. The
// triangles that face away are left out and break the strip. `textured` strips give u and v after
// each index, times `scale`. Otherwise u and v are half the turned normal's x and negated y plus a
// half, and with bit 17 of Ninja's flags plus the environment map's offsets at 0x8C0CCDA8.
uint32_t simple_multi_strips(uint32_t in, uint32_t buffer, uint32_t count, uint32_t out, const float (&material)[7],
                             bool textured, float scale) {
    uint32_t strips = count & 0x3FFF;
    const uint32_t user = ((count >> 14) & 3) * 2, flags = word(0x8C10AF98u);
    float base_u = 0.5f, base_v = 0.5f;
    if (!textured && (flags & 0x20000u) != 0u) {
        base_u = real(0x8C0CCDA8u) + 0.5f;
        base_v = real(0x8C0CCDACu) + 0.5f;
    }
    struct Corner {
        uint32_t at;
        float u, v;
    };
    const auto next = [&in] {
        const int32_t v = guest::load<int16_t>(in);
        in += 2;
        return v;
    };
    const auto read = [&] {
        Corner c;
        c.at = buffer + (static_cast<uint32_t>(next()) << 6);
        if (textured) {
            c.u = static_cast<float>(next()) * scale;
            c.v = static_cast<float>(next()) * scale;
        }
        else {
            c.u = std::fma(0.5f, real(c.at + 40), base_u);
            c.v = std::fma(0.5f, real(c.at + 44), base_v);
        }
        return c;
    };
    const auto emit = [&](const Corner& c, uint32_t control) {
        const uint32_t first[6] = {control, word(c.at + 4), word(c.at + 8), word(c.at + 12), bits_of(c.u),
                                   bits_of(c.v)};
        for (uint32_t i = 0; i < 6; ++i) guest::store<uint32_t>(out + 4 * i, first[i]);
        hw::prefetch(out);
        out += 32;
        const float second[4] = {material[0] * real(c.at + 16), std::fma(real(c.at + 20), material[1], material[4]),
                                 std::fma(real(c.at + 24), material[2], material[5]),
                                 std::fma(real(c.at + 28), material[3], material[6])};
        for (uint32_t i = 0; i < 4; ++i) guest::store<float>(out + 4 * i, second[i]);
        for (uint32_t i = 5; i < 8; ++i) guest::store<uint32_t>(out + 4 * i, word(c.at + 32 + 4 * i));
        hw::prefetch(out);
        out += 32;
    };
    uint32_t sent = 0, total = 0;
    do {
        int32_t length = next();
        float sign = (flags & 0x80000000u) != 0u ? -1.0f : 1.0f;
        if (length < 0) {
            length = -length;
            sign = -sign;
        }
        total += static_cast<uint32_t>(length);
        Corner a = read(), b = read();
        uint32_t open = 0;
        uint32_t triangles = static_cast<uint32_t>(length - 2);
        do {
            const Corner c = read();
            in += user;
            const float bx = real(b.at + 4), by = real(b.at + 8);
            const float cross = (bx - real(a.at + 4)) * (real(c.at + 8) - by) - (by - real(a.at + 8)) * (real(c.at + 4) - bx);
            if (0.0f > cross * sign) {
                if (open == 0) {
                    emit(a, kVertex);
                    ++open;
                }
                emit(b, kVertex);
                ++open;
            }
            else if (open) {
                emit(b, kLastVertex);
                sent += open + 1;
                open = 0;
            }
            sign = -sign;
            a = b;
            b = c;
        } while (--triangles != 0);
        if (open) {
            emit(b, kLastVertex);
            sent += open + 1;
        }
    } while (--strips != 0);
    guest::store<uint32_t>(0x8C10AFB0u, word(0x8C10AFB0u) + sent);
    guest::store<uint32_t>(0x8C10AFACu, word(0x8C10AFACu) + total);
    return out;
}

}  // namespace

// Textured strips of a Simple multi-light model (see simple_multi_strips): the u and v scale in
// `param_5`, then the material.
gptr<dword> njCnkSimpleMultiTextureDiff(gptr<short> param_1, int param_2, uint param_3, gptr<dword> param_4,
                                        float param_5, float param_6, float param_7, float param_8, float param_9,
                                        float param_10, float param_11, float param_12) {
    diff::Check check("njCnkSimpleMultiTextureDiff", 0x8C02CF20u, param_1, param_2, param_3, param_4, param_5, param_6,
                      param_7, param_8, param_9, param_10, param_11, param_12);
    const float material[7] = {param_6, param_7, param_8, param_9, param_10, param_11, param_12};
    return check.returns(gptr<dword>(
        simple_multi_strips(param_1.a, static_cast<uint32_t>(param_2), param_3, param_4.a, material, true, param_5)));
}

// Environment-mapped strips of a Simple multi-light model (see simple_multi_strips), with the
// material in `param_5` to `arg_fr10`.
gptr<dword> njCnkSimpleMultiPolygonDiffEnv(gptr<short> param_1, int param_2, uint param_3, gptr<dword> param_4,
                                           float param_5, float param_6, float param_7, float param_8, float param_9,
                                           float param_10, float arg_fr10, float arg_fr11) {
    diff::Check check("njCnkSimpleMultiPolygonDiffEnv", 0x8C02A174u, param_1, param_2, param_3, param_4, param_5,
                      param_6, param_7, param_8, param_9, param_10, arg_fr10, arg_fr11);
    const float material[7] = {param_5, param_6, param_7, param_8, param_9, param_10, arg_fr10};
    return check.returns(gptr<dword>(
        simple_multi_strips(param_1.a, static_cast<uint32_t>(param_2), param_3, param_4.a, material, false, 0.0f)));
}

// The vertex list of a multi-light Simple model (see multi_vlist). Returns 0.
undefined4 njCnkSimpleMultiVlist(gptr<short> param_1, int param_2, float, float, float, float) {
    diff::Check check("njCnkSimpleMultiVlist", 0x8C029612u, param_1, param_2);
    multi_vlist(param_1.a, static_cast<uint32_t>(param_2), true);
    return check.returns(undefined4(0));
}

// The polygon list of a multi-light Simple model: as njCnkEasyMultiPlist, with the Simple models'
// strip functions and Ninja's flat flag (0x8C0CCD88) at 1 for strips with flag 32. Textured strips
// (type 65 or 66) scale u and v by 1/256 or 1/1024; with flag 64 they, and untextured strips with
// it, map the environment instead. The strips take the material's alpha and the colour that the
// light scales times the light's, or with their flag 4 set, black. Without bit 9 of Ninja's flags,
// and always for untextured strips without flag 64, they take the material's colour and the
// vertices' light too.
void njCnkSimpleMultiPlist(gptr<ushort> param_1, int param_2, int param_3, float, float, float, float, float, float,
                           float, float) {
    diff::Check check("njCnkSimpleMultiPlist", 0x8C028BECu, param_1, param_2, param_3);
    constexpr uint32_t kCache = 0x8C12290Cu, kCached = 0x8C122908u, kFlat = 0x8C0CCD88u;
    // The material: alpha and red, green and blue (0-12), the specular part (16), and the colour
    // that the light scales (20-28). The light's colour is at 84-92 of 0x8C1227E0.
    constexpr uint32_t kMaterial = 0x8C10B58Cu, kLight = 0x8C1227E0u + 84;
    const uint32_t buffer = static_cast<uint32_t>(param_3);
    uint32_t at = param_1.a;
    for (;;) {
        const int32_t head = guest::load<int16_t>(at);
        at += 2;
        if (head == 0xFF)
            return;
        const int32_t type = head & 0xFF;
        if (type < 8) {
            const uint32_t slot = kCache + 4 * static_cast<uint32_t>((head >> 8) & 0xFF);
            if (type == 1) {
                njSetkmCnkBlendMode(static_cast<uint>(head));
            }
            else if (type == 4) {
                guest::store<uint32_t>(slot, at);
                guest::store<uint32_t>(kCached, word(kCached) + 1);
                return;
            }
            else if (type == 5) {
                njCnkSimpleMultiPlist(gptr<ushort>(word(slot)), param_2, param_3, 0, 0, 0, 0, 0, 0, 0, 0);
                guest::store<uint32_t>(kCached, word(kCached) - 1);
            }
            continue;
        }
        if (type < 16) {
            const int32_t texture = guest::load<int16_t>(at);
            at += 2;
            njSetkmCnkTextureNum(static_cast<uint>(head), static_cast<uint>(texture));
            continue;
        }
        if (type < 64) {
            const int32_t size = guest::load<int16_t>(at);
            at += 2;
            njCnkSetMaterial(static_cast<uint>(head), gptr<short>(at), gptr<float>(kMaterial),
                             gptr<float>(kMaterial + 16), gptr<float>(0));
            at += static_cast<uint32_t>(size) * 2;
            continue;
        }
        const uint32_t size = guest::load<uint16_t>(at);
        const uint32_t strips = static_cast<uint32_t>(guest::load<int16_t>(at + 2));
        const uint32_t data = at + 4;
        if (!(param_2 >= static_cast<int32_t>(word(0x8C122904u)))) {
            uint32_t h = static_cast<uint32_t>(head);
            if (word(0x8C10AF98u) & 0x800)
                h = (h & word(0x8C10AFB4u)) | word(0x8C10AFB8u);
            int32_t flags = static_cast<int16_t>(h) >> 8;
            if (word(0x8C10B3E0u))
                flags |= 8;
            guest::store<uint32_t>(kFlat, 0);
            const bool translucent = flags & 8, near = param_2 != 0;
            const auto set_mode = [&](uint32_t mode) {
                njSetkmColorMode((translucent ? 0x02100000u : 0x00080000u) | mode | ((flags & 32) ? 0 : 2));
                if (flags & 32)
                    guest::store<uint32_t>(kFlat, 1);
            };
            const auto m = [](uint32_t i) { return real(kMaterial + 4 * i); };
            const auto lit = [&](uint32_t i) {
                return (flags & 4) ? 0.0f : real(kMaterial + 20 + 4 * i) * real(kLight + 4 * i);
            };
            uint32_t out = 0;
            if (type == 64 && !(flags & 64)) {
                set_mode(0x10);
                out = (translucent ? njStartVertexNonTexTrans() : njStartVertexNonTexOpaque()).a;
                out = strip_function(near ? 0x8C0299D0u : 0x8C02B1FCu, data, buffer, strips, out,
                                     {m(0), m(1), m(2), m(3), lit(0), lit(1), lit(2)});
            }
            else if (type == 64 || type == 65 || type == 66) {
                set_mode(0x18 | ((flags & 2) ? 0 : 4));
                out = (translucent ? njStartVertexTexTrans() : njStartVertexTexOpaque()).a;
                const bool mapped = type == 64 || (flags & 64);
                const float scale = type == 65 ? 1.0f / 256.0f : 1.0f / 1024.0f;
                if (word(0x8C10AF98u) & 0x200) {
                    if (mapped)
                        out = strip_function(type == 64 ? (near ? 0x8C02A4F4u : 0x8C02A8F8u)
                                                        : (near ? 0x8C02D2A0u : 0x8C02DBB0u),
                                             data, buffer, strips, out, {m(0), lit(0), lit(1), lit(2)});
                    else
                        out = strip_function(near ? 0x8C02B3D8u : 0x8C02E614u, data, buffer, strips, out,
                                             {scale, m(0), lit(0), lit(1), lit(2)});
                }
                else if (mapped) {
                    out = strip_function(type == 64 ? (near ? 0x8C029D24u : 0x8C02A174u)
                                                    : (near ? 0x8C02BC7Cu : 0x8C02C660u),
                                         data, buffer, strips, out, {m(0), m(1), m(2), m(3), lit(0), lit(1), lit(2)});
                }
                else {
                    out = strip_function(near ? 0x8C02B804u : 0x8C02CF20u, data, buffer, strips, out,
                                         {scale, m(0), m(1), m(2), m(3), lit(0), lit(1), lit(2)});
                }
            }
            if (out)
                njEndVertex(out);
        }
        at = data + (size - 1) * 2;
    }
}

// A multi-light Simple model (see multi_draw_model).
Sint32 njCnkSimpleMultiDrawModel(gptr<NJS_CNK_MODEL> model) {
    diff::Check check("njCnkSimpleMultiDrawModel", 0x8C029772u, model);
    return check.returns(multi_draw_model(model.a, true));
}

namespace {

void put3(uint32_t at, float a, float b, float c) {
    guest::store<float>(at, a);
    guest::store<float>(at + 4, b);
    guest::store<float>(at + 8, c);
}

// shld: a left shift, or for a negative count a right shift.
uint32_t shld(uint32_t v, int32_t count) {
    if (count >= 0)
        return v << (count & 31);
    const uint32_t right = (~static_cast<uint32_t>(count) & 31) + 1;
    return right == 32 ? 0 : v >> right;
}

// The multi-light models' lights: the Easy models' at 0x8C1226C0, the Simple models' at
// 0x8C1227E0. From there come the lights' directions or positions, 12 bytes each, the bits of the
// lights at 80, the colour that scales the material's at 84, and from 96 on the lights' records
// (see multi_light), with the direction or position in the view.
constexpr uint32_t kEasyLights = 0x8C1226C0u, kSimpleLights = 0x8C1227E0u;

// Light n, from 1: its direction, or for a point light its position.
void set_light(uint32_t lights, int n, float x, float y, float z, bool point) {
    const uint32_t i = static_cast<uint32_t>(n - 1);
    put3(lights + 12 * i, x, y, z);
    const uint32_t bit = shld(2, static_cast<int32_t>(3 * i)), mask = word(lights + 80);
    guest::store<uint32_t>(lights + 80, point ? mask | bit : mask & ~bit);
}

// Light n's reach and the distance from which it fades, squared.
void set_light_range(uint32_t lights, int n, float distance, float reach) {
    const uint32_t record = lights + 96 + 32 * static_cast<uint32_t>(n - 1);
    guest::store<float>(record + 12, reach * reach);
    guest::store<float>(record + 16, distance * distance);
}

// The lit lights' directions and positions through the current matrix into their records.
void view_lights(uint32_t lights) {
    uint32_t mask = word(lights + 80), from = lights, to = lights + 96;
    for (;;) {
        if (mask & 1) {
            mask >>= 1;
            if (mask & 1)
                njCalcPoint(gptr<NJS_MATRIX>(0), gptr<NJS_POINT3>(from), gptr<NJS_POINT3>(to));
            else
                njCalcVector(gptr<NJS_MATRIX>(0), gptr<NJS_VECTOR>(from), gptr<NJS_VECTOR>(to));
        }
        else {
            mask >>= 1;
        }
        mask >>= 1;
        if (mask & 1)
            return;
        from += 12;
        mask >>= 1;
        to += 32;
    }
}

}  // namespace

// The Easy models' light: its direction, its intensity and the ambient light (the intensity goes
// negated), and its colour.
void njCnkSetEasyLight(float param_1, float param_2, float param_3) {
    diff::Check check("njCnkSetEasyLight", 0x8C019CA4u, param_1, param_2, param_3);
    put3(0x8C10B3E8u, param_1, param_2, param_3);
}
void njCnkSetEasyLightIntensity(float arg_fr4, float arg_fr5) {
    diff::Check check("njCnkSetEasyLightIntensity", 0x8C019CB2u, arg_fr4, arg_fr5);
    guest::store<float>(0x8C10B3F4u, -arg_fr4);
    guest::store<float>(0x8C10B3F8u, arg_fr5);
}
void njCnkSetEasyLightColor(float param_1, float param_2, float param_3) {
    diff::Check check("njCnkSetEasyLightColor", 0x8C019CC0u, param_1, param_2, param_3);
    put3(0x8C0CF524u, param_1, param_2, param_3);
}

// The Simple models' light, as the Easy models'.
void njCnkSetSimpleLight(float param_1, float param_2, float param_3) {
    diff::Check check("njCnkSetSimpleLight", 0x8C01C65Cu, param_1, param_2, param_3);
    put3(0x8C10B40Cu, param_1, param_2, param_3);
}
void njCnkSetSimpleLightIntensity(float arg_fr4, float arg_fr5) {
    diff::Check check("njCnkSetSimpleLightIntensity", 0x8C01C66Au, arg_fr4, arg_fr5);
    guest::store<float>(0x8C10B418u, -arg_fr4);
    guest::store<float>(0x8C10B41Cu, arg_fr5);
}
void njCnkSetSimpleLightColor(Float r, Float g, Float b) {
    diff::Check check("njCnkSetSimpleLightColor", 0x8C01C678u, r, g, b);
    put3(0x8C0CF544u, r, g, b);
}

// The multi-light Easy models' lights: the colour that scales the material's, a light's colour,
// a directional light, a point light, a point light's range, and the lights in the view.
void njCnkSetEasyMultiAmbient(float param_1, float param_2, float param_3) {
    diff::Check check("njCnkSetEasyMultiAmbient", 0x8C01A322u, param_1, param_2, param_3);
    put3(kEasyLights + 84, param_1, param_2, param_3);
}
void njCnkSetEasyMultiLightColor(int param_1, float param_2, float param_3, float param_4) {
    diff::Check check("njCnkSetEasyMultiLightColor", 0x8C01A330u, param_1, param_2, param_3, param_4);
    put3(kEasyLights + 116 + 32 * static_cast<uint32_t>(param_1 - 1), param_2, param_3, param_4);
}
void njCnkSetEasyMultiLightVectorEx(int param_1, float param_2, float param_3, float param_4) {
    diff::Check check("njCnkSetEasyMultiLightVectorEx", 0x8C01A348u, param_1, param_2, param_3, param_4);
    set_light(kEasyLights, param_1, param_2, param_3, param_4, false);
}
void njCnkSetEasyMultiLightPoint(int param_1, float param_2, float param_3, float param_4) {
    diff::Check check("njCnkSetEasyMultiLightPoint", 0x8C01A380u, param_1, param_2, param_3, param_4);
    set_light(kEasyLights, param_1, param_2, param_3, param_4, true);
}
void njCnkSetEasyMultiLightRange(int param_1, float arg_fr4, float arg_fr5) {
    diff::Check check("njCnkSetEasyMultiLightRange", 0x8C01A3B2u, param_1, arg_fr4, arg_fr5);
    set_light_range(kEasyLights, param_1, arg_fr4, arg_fr5);
}
void njCnkSetEasyMultiLightMatrices() {
    diff::Check check("njCnkSetEasyMultiLightMatrices", 0x8C01A3CAu);
    view_lights(kEasyLights);
}

// The multi-light Simple models' lights, as the Easy models'.
void njCnkSetSimpleMultiAmbient(float param_1, float param_2, float param_3) {
    diff::Check check("njCnkSetSimpleMultiAmbient", 0x8C029892u, param_1, param_2, param_3);
    put3(kSimpleLights + 84, param_1, param_2, param_3);
}
void njCnkSetSimpleMultiLightColor(int param_1, float param_2, float param_3, float param_4) {
    diff::Check check("njCnkSetSimpleMultiLightColor", 0x8C0298A0u, param_1, param_2, param_3, param_4);
    put3(kSimpleLights + 116 + 32 * static_cast<uint32_t>(param_1 - 1), param_2, param_3, param_4);
}
void njCnkSetSimpleMultiLightVectorEx(int param_1, float param_2, float param_3, float param_4) {
    diff::Check check("njCnkSetSimpleMultiLightVectorEx", 0x8C0298B8u, param_1, param_2, param_3, param_4);
    set_light(kSimpleLights, param_1, param_2, param_3, param_4, false);
}
void njCnkSetSimpleMultiLightPoint(int param_1, float param_2, float param_3, float param_4) {
    diff::Check check("njCnkSetSimpleMultiLightPoint", 0x8C0298F0u, param_1, param_2, param_3, param_4);
    set_light(kSimpleLights, param_1, param_2, param_3, param_4, true);
}
void njCnkSetSimpleMultiLightRange(int param_1, float arg_fr4, float arg_fr5) {
    diff::Check check("njCnkSetSimpleMultiLightRange", 0x8C029922u, param_1, arg_fr4, arg_fr5);
    set_light_range(kSimpleLights, param_1, arg_fr4, arg_fr5);
}
void njCnkSetSimpleMultiLightMatrices() {
    diff::Check check("njCnkSetSimpleMultiLightMatrices", 0x8C02993Au);
    view_lights(kSimpleLights);
}

// njCnkTransformObject with the draw functions of the chunk models: njCnkEasyDrawModel's models, Simple
// models (njCnkSimpleDrawModel), multi-light Easy models and multi-light Simple models.
void njCnkEasyDrawObject(gptr<NJS_CNK_OBJECT> object) {
    diff::Check check("njCnkEasyDrawObject", 0x8C01A296u, object);
    njCnkTransformObject(gptr<uint>(object.a), gptr<undefined>(0x8C01A23Au));
}
void njCnkSimpleDrawObject(gptr<uint> param_1) {
    diff::Check check("njCnkSimpleDrawObject", 0x8C01CF94u, param_1);
    njCnkTransformObject(param_1, gptr<undefined>(0x8C01CF3Eu));
}
void njCnkEasyMultiDrawObject(gptr<NJS_CNK_OBJECT> model) {
    diff::Check check("njCnkEasyMultiDrawObject", 0x8C01C64Cu, model);
    njCnkTransformObject(gptr<uint>(model.a), gptr<undefined>(0x8C01C596u));
}
void njCnkSimpleMultiDrawObject(gptr<NJS_CNK_OBJECT> model) {
    diff::Check check("njCnkSimpleMultiDrawObject", 0x8C029828u, model);
    njCnkTransformObject(gptr<uint>(model.a), gptr<undefined>(0x8C029772u));
}

}  // namespace game
