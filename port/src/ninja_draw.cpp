// Ninja's drawing, natively, as far as it goes: each function sends the tile accelerator the same
// blocks through the store queues as its machine code, so the display lists stay the same.
// IKARUGA_SH4_TEST captures both and compares them.
#include <cmath>
#include <cstring>

#include "decomp.h"
#include "diff.h"
#include "hw.h"

namespace game {
namespace {

// The block of one vertex: the control word, then the vertex's x, y, z, u, v and colour, then the
// offset colour. Ninja writes fr7 there without loading it, so a vertex gets what its caller left
// in fr7; calls from C++ leave 0.
void vertex(uint32_t at, uint32_t control, uint32_t v) {
    guest::store<uint32_t>(at, control);
    for (uint32_t i = 0; i < 6; ++i) guest::store<uint32_t>(at + 4 + 4 * i, guest::load<uint32_t>(v + 4 * i));
    guest::store<uint32_t>(at + 28, 0);
    hw::prefetch(at);
}

uint32_t word(uint32_t a) { return guest::load<uint32_t>(a); }
void put(uint32_t a, uint32_t v) { guest::store<uint32_t>(a, v); }

float real(uint32_t a) { return guest::load<float>(a); }

// ftrv: the current matrix, which Ninja keeps in the FPU's back bank, times (x, y, z, 1). Each row's
// products are summed in double and rounded once, as the interpreter computes them.
void transform(float x, float y, float z, float (&out)[4]) {
    for (int i = 0; i < 4; ++i)
        out[i] = static_cast<float>(cpu::xf[i ^ 1] * static_cast<double>(x) +
                                    cpu::xf[(4 + i) ^ 1] * static_cast<double>(y) +
                                    cpu::xf[(8 + i) ^ 1] * static_cast<double>(z) + cpu::xf[(12 + i) ^ 1] * 1.0);
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

uint32_t bits_of(float f) {
    uint32_t b;
    std::memcpy(&b, &f, 4);
    return b;
}

// A strip starts with its polygon's global parameters. Ninja keeps the control, ISP, TSP and texture
// words in the context that 0x8C10B0F4 points to. The table that 0x8C10B0F8 points to holds the
// store-queue address of the opaque list's next block, and 8 bytes after it the translucent list's.
// A start sends the parameters to one list and puts that list's place at 0x8C10B364, where
// njEndVertex puts the address after the strip.
constexpr uint32_t kContext = 0x8C10B0F4u, kLists = 0x8C10B0F8u, kCurrentList = 0x8C10B364u;
constexpr uint32_t kOpaque = 0, kTranslucent = 8;
// In the opaque list, the TSP word blends with one and zero and does not use alpha. The translucent
// list mostly keeps the context's TSP word.
constexpr uint32_t kOpaqueKeep = 0x03EFFFFFu, kOpaqueAdd = 0x20000000u, kUseAlpha = 0x00100000u;

// Sets bit 7 of the control word, which lets modifier volumes change the polygon, when one of `flags`
// is set in Ninja's 3D control flags.
void modifiable(uint32_t flags) {
    if (word(0x8C10AF98u) & flags) {
        const uint32_t p = word(kContext);
        put(p, word(p) | 0x80);
    }
}

// The global parameters in the next block of `list`, with the TSP word through `keep` and `add`,
// and the texture word if `texture`. Returns the block's address.
uint32_t parameters(uint32_t list, uint32_t keep, uint32_t add, bool texture) {
    const uint32_t p = word(kContext), at = word(word(kLists) + list);
    put(at, word(p));
    put(at + 4, word(p + 4));
    put(at + 8, (word(p + 8) & keep) | add);
    if (texture)
        put(at + 12, word(p + 12));
    return at;
}

// The four words of the colour at `c`, at `at`.
void colour(uint32_t at, uint32_t c) {
    for (uint32_t i = 0; i < 16; i += 4) put(at + i, word(c + i));
}

// Sends the block at `at` and makes `list` the current list. Returns the address of the next block.
uint32_t sent(uint32_t list, uint32_t at) {
    hw::prefetch(at);
    put(kCurrentList, word(kLists) + list);
    return at + 32;
}

// With two colours, the parameters fill a second block: the base colour, then the offset colour.
uint32_t with_two_colours(uint32_t list, uint32_t at, uint32_t base, uint32_t offset) {
    hw::prefetch(at);
    colour(at + 32, base);
    colour(at + 48, offset);
    return sent(list, at + 32);
}

// The strip kinds' table at 0x8C0CCDD8: for each kind, the control word's type bits and the start
// function, which gets `a` and `b`.
uint32_t start_kind(uint32_t kind, uint32_t a, uint32_t b) {
    const uint32_t entry = 0x8C0CCDD8u + 8 * kind, p = word(kContext);
    put(p, (word(p) & 0xF0FFFF00u) | word(entry));
    CallArgs args;
    args.r[0] = a;
    args.r[1] = b;
    args.nr = 2;
    return guest::call(word(entry + 4), args).r0;
}

// Replaces the bits of the context's TSP word outside `keep` with `add`.
void set_tsp(uint32_t keep, uint32_t add) {
    const uint32_t p = word(kContext);
    put(p + 8, (word(p + 8) & keep) | add);
}

}  // namespace

// Starts the strips of njDrawTexture2DExSetData in the opaque list, or with `trans` in the translucent
// one, and keeps their store-queue address at 0x8C10B360.
void njDrawTexture2DExStart(Int trans) {
    diff::Check check("njDrawTexture2DExStart", 0x8C030558u, trans);
    njSetkmColorMode(trans ? 0x0210000Au : 0x0008000Au);
    put(0x8C10B360u, (trans ? njStartVertexTexTrans() : njStartVertexTexOpaque()).a);
}

void njDrawTexture2DExEnd() {
    diff::Check check("njDrawTexture2DExEnd", 0x8C030586u);
    njEndVertex(word(0x8C10B360u));
}

// Starts the untextured strips of njDrawPolygon2DExSetData, in the translucent list with `trans`.
void njDrawPolygon2DExStart(Int trans) {
    diff::Check check("njDrawPolygon2DExStart", 0x8C02FCC0u, trans);
    njSetkmColorMode(trans ? 0x02100002u : 0x00080002u);
    put(0x8C10B360u, (trans ? njStartVertexNonTexTrans() : njStartVertexNonTexOpaque()).a);
}

void njDrawPolygon2DExEnd() {
    diff::Check check("njDrawPolygon2DExEnd", 0x8C02FCEEu);
    njEndVertex(word(0x8C10B360u));
}

// Starts the textured 3D strips of njDrawTexture3DExSetData, in the translucent list only when
// `trans` is 1.
void njDrawTexture3DExStart(Int trans) {
    diff::Check check("njDrawTexture3DExStart", 0x8C030DDCu, trans);
    njSetkmColorMode(trans == 1 ? 0x0210000Au : 0x0008000Au);
    put(0x8C10B360u, (trans == 1 ? njStartVertexTexTrans() : njStartVertexTexOpaque()).a);
}

void njDrawTexture3DExEnd() {
    diff::Check check("njDrawTexture3DExEnd", 0x8C030EC0u);
    njEndVertex(word(0x8C10B360u));
}

void njDrawPolygon3DExEnd() {
    diff::Check check("njDrawPolygon3DExEnd", 0x8C030548u);
    njEndVertex(word(0x8C10B360u));
}

// A strip of textured vertices for njDrawTexture2DExStart. The variable at 0x8C10B360 holds the
// store-queue address of the next block; the last vertex's control word ends the strip.
void njDrawTexture2DExSetData(gptr<NJS_TEXTURE_VTX> p, Int count) {
    diff::Check check("njDrawTexture2DExSetData", 0x8C0305ACu, p, count);
    constexpr uint32_t kTable = 0x8C030604u;  // the control words and the variable's address
    const uint32_t control = guest::load<uint32_t>(kTable), last = guest::load<uint32_t>(kTable + 4);
    const uint32_t next = guest::load<uint32_t>(kTable + 8);
    uint32_t at = guest::load<uint32_t>(next);
    for (Int i = 0; i < count; ++i, at += 32)
        vertex(at, i == count - 1 ? last : control, p.a + 24u * static_cast<uint32_t>(i));
    guest::store<uint32_t>(next, at);
}

gptr<uint> njStartVertexOpaque() {
    diff::Check check("njStartVertexOpaque", 0x8C01167Cu);
    modifiable(0x1400);
    return check.returns(gptr<uint>(sent(kOpaque, parameters(kOpaque, kOpaqueKeep, kOpaqueAdd, true))));
}

gptr<uint> njStartVertexTrans() {
    diff::Check check("njStartVertexTrans", 0x8C0116C4u);
    modifiable(0x400);
    return check.returns(gptr<uint>(sent(kTranslucent, parameters(kTranslucent, ~0u, kUseAlpha, true))));
}

gptr<uint> njStartVertex(int param_1) {
    diff::Check check("njStartVertex", 0x8C01170Au, param_1);
    return check.returns(gptr<uint>(start_kind(static_cast<uint32_t>(param_1), 0, 0)));
}

gptr<uint> njStartVertexIntensityOpaque(gptr<uint> param_1) {
    diff::Check check("njStartVertexIntensityOpaque", 0x8C011734u, param_1);
    modifiable(0x1400);
    const uint32_t at = parameters(kOpaque, kOpaqueKeep, kOpaqueAdd, true);
    colour(at + 16, param_1.a);
    return check.returns(gptr<uint>(sent(kOpaque, at)));
}

gptr<uint> njStartVertexIntensityOpaqueSpec(gptr<uint> param_1, gptr<uint> param_2) {
    diff::Check check("njStartVertexIntensityOpaqueSpec", 0x8C0117BCu, param_1, param_2);
    modifiable(0x1400);
    const uint32_t at = parameters(kOpaque, kOpaqueKeep, kOpaqueAdd, true);
    return check.returns(gptr<uint>(with_two_colours(kOpaque, at, param_1.a, param_2.a)));
}

gptr<uint> njStartVertexIntensityTrans(gptr<uint> param_1) {
    diff::Check check("njStartVertexIntensityTrans", 0x8C01183Cu, param_1);
    modifiable(0x400);
    const uint32_t at = parameters(kTranslucent, ~0u, kUseAlpha, true);
    colour(at + 16, param_1.a);
    return check.returns(gptr<uint>(sent(kTranslucent, at)));
}

gptr<uint> njStartVertexIntensityTransSpec(gptr<uint> param_1, gptr<uint> param_2) {
    diff::Check check("njStartVertexIntensityTransSpec", 0x8C01189Au, param_1, param_2);
    modifiable(0x400);
    const uint32_t at = parameters(kTranslucent, ~0u, kUseAlpha, true);
    return check.returns(gptr<uint>(with_two_colours(kTranslucent, at, param_1.a, param_2.a)));
}

gptr<uint> njStartVertexIntensity(int param_1, undefined4 param_2, undefined4 param_3) {
    diff::Check check("njStartVertexIntensity", 0x8C011938u, param_1, param_2, param_3);
    return check.returns(gptr<uint>(start_kind(static_cast<uint32_t>(param_1), param_2, param_3)));
}

gptr<undefined4> njStartVertexNonTexOpaque() {
    diff::Check check("njStartVertexNonTexOpaque", 0x8C011974u);
    return check.returns(gptr<undefined4>(sent(kOpaque, parameters(kOpaque, kOpaqueKeep, kOpaqueAdd, false))));
}

gptr<undefined4> njStartVertexNonTexTrans() {
    diff::Check check("njStartVertexNonTexTrans", 0x8C0119A6u);
    return check.returns(gptr<undefined4>(sent(kTranslucent, parameters(kTranslucent, ~0u, 0, false))));
}

gptr<undefined4> njStartVertexTexOpaque() {
    diff::Check check("njStartVertexTexOpaque", 0x8C0119D2u);
    return check.returns(gptr<undefined4>(sent(kOpaque, parameters(kOpaque, kOpaqueKeep, kOpaqueAdd, true))));
}

gptr<undefined4> njStartVertexTexTrans() {
    diff::Check check("njStartVertexTexTrans", 0x8C011A0Au);
    return check.returns(gptr<undefined4>(sent(kTranslucent, parameters(kTranslucent, ~0u, 0, true))));
}

gptr<undefined4> njStartVertexNonTexOpaqueIntens(gptr<undefined4> param_1) {
    diff::Check check("njStartVertexNonTexOpaqueIntens", 0x8C011B50u, param_1);
    const uint32_t at = parameters(kOpaque, kOpaqueKeep, kOpaqueAdd, false);
    colour(at + 16, param_1.a);
    return check.returns(gptr<undefined4>(sent(kOpaque, at)));
}

gptr<undefined4> njStartVertexTexOpaqueIntens(gptr<undefined4> param_1) {
    diff::Check check("njStartVertexTexOpaqueIntens", 0x8C011BE2u, param_1);
    const uint32_t at = parameters(kOpaque, kOpaqueKeep, kOpaqueAdd, true);
    colour(at + 16, param_1.a);
    return check.returns(gptr<undefined4>(sent(kOpaque, at)));
}

gptr<undefined4> njStartVertexTexTransIntens(gptr<undefined4> param_1) {
    diff::Check check("njStartVertexTexTransIntens", 0x8C011C54u, param_1);
    const uint32_t at = parameters(kTranslucent, ~0u, 0, true);
    colour(at + 16, param_1.a);
    return check.returns(gptr<undefined4>(sent(kTranslucent, at)));
}

gptr<undefined4> njStartVertexTexTransIntensH(gptr<undefined4> param_1, gptr<undefined4> param_2) {
    diff::Check check("njStartVertexTexTransIntensH", 0x8C011D0Cu, param_1, param_2);
    const uint32_t at = parameters(kTranslucent, ~0u, 0, true);
    return check.returns(gptr<undefined4>(with_two_colours(kTranslucent, at, param_1.a, param_2.a)));
}

// An untextured translucent sprite: the control word gets the sprite type, and the base colour
// `param_1` follows the TSP word.
gptr<uint> njStartVertexNonTexSpriteTrans(uint param_1) {
    diff::Check check("njStartVertexNonTexSpriteTrans", 0x8C011DE4u, param_1);
    const uint32_t p = word(kContext), at = word(word(kLists) + kTranslucent);
    put(at + 16, param_1);
    put(at + 8, (word(p + 8) & 0xFFF7FFFFu) | kUseAlpha);
    put(at + 4, word(p + 4));
    put(at, (word(p) & 0x00030000u) | (word(0x8C10AF98u) & 0x400 ? 0xA2800080u : 0xA2800000u));
    return check.returns(gptr<uint>(sent(kTranslucent, at)));
}

void njEndVertex(undefined4 param_1) {
    diff::Check check("njEndVertex", 0x8C011EDAu, param_1);
    put(word(kCurrentList), param_1);
}

void njTextureFilterMode(Int mode) {
    diff::Check check("njTextureFilterMode", 0x8C0101B2u, mode);
    set_tsp(0xFFFF9FFFu, static_cast<uint32_t>(mode));
}

void njTextureClampMode(Int mode) {
    diff::Check check("njTextureClampMode", 0x8C0101ECu, mode);
    set_tsp(0xFFFE7FFFu, static_cast<uint32_t>(mode));
}

void njFogMode(uint param_1) {
    diff::Check check("njFogMode", 0x8C0103A0u, param_1);
    set_tsp(0xFF3FFFFFu, param_1);
}

void njFogDisable() {
    diff::Check check("njFogDisable", 0x8C01582Eu);
    njFogMode(0x00800000u);
}

void njFogEnable() {
    diff::Check check("njFogEnable", 0x8C015828u);
    njFogMode(0);
}

// The fade, as Ninja sets it: the same as the fog.
void njFadeEnable() {
    diff::Check check("njFadeEnable", 0x8C015836u);
    njFogMode(0);
}

void njFadeDisable() {
    diff::Check check("njFadeDisable", 0x8C01583Cu);
    njFogMode(0x00800000u);
}

// The texture list that njSetTextureNum chooses from, at 40 in the strip context.
Sint32 njSetTexture(gptr<NJS_TEXLIST> texlist) {
    diff::Check check("njSetTexture", 0x8C014132u, texlist);
    put(word(kContext) + 40, texlist.a);
    return check.returns(1);
}

// The source's (`target` 0) or the destination's blend factor. The table at 0x8C0CA450 gives each
// mode's factor. Mode 0 sets both: the source alpha and its inverse.
void njColorBlendingMode(Int target, Int mode) {
    diff::Check check("njColorBlendingMode", 0x8C010222u, target, mode);
    if (mode == 0) {
        set_tsp(0x03FFFFFFu, 0xB0000000u);
        return;
    }
    const uint32_t factor = word(0x8C0CA450u + 4u * static_cast<uint32_t>(mode));
    if (target == 0)
        set_tsp(0x1FFFFFFFu, factor << 29);
    else
        set_tsp(0xE3FFFFFFu, factor << 26);
}

// A chunk's blend factors in bits 8 to 13 and its source and destination selection in bits 14 and
// 15. With bit 15 of the 3D control flags set, the mode at 0x8C10AFCC replaces the chunk's.
void njSetkmCnkBlendMode(uint param_1) {
    diff::Check check("njSetkmCnkBlendMode", 0x8C0120ACu, param_1);
    const uint32_t mode = word(0x8C10AF98u) & 0x8000 ? word(0x8C10AFCCu) : param_1;
    set_tsp(0x00FFFFFFu, ((mode << 18) & 0xFC000000u) | ((mode << 10) & 0x03000000u));
}

// Texture `n` of the current texture list, which the context holds at 40. Each texture's entry is
// 12 bytes, and its third word points to the texture's TSP bits and its texture control word.
Sint32 njSetTextureNum(Uint32 n) {
    diff::Check check("njSetTextureNum", 0x8C014144u, n);
    const uint32_t texture = word(word(word(word(kContext) + 40)) + 12 * n + 8);
    set_tsp(~63u, word(texture) & 63);
    put(word(kContext) + 12, word(texture + 4));
    return check.returns(1);
}

// Starts the lines of njDrawLine2DExSetStrip as sprites with the base colour `BaseColor`, in the
// translucent list with `Trans`, and keeps their half-width `r` at 0x8C10B3E4.
void njDrawLineExStart(Float r, Uint32 BaseColor, Sint32 Trans) {
    diff::Check check("njDrawLineExStart", 0x8C02F870u, r, BaseColor, Trans);
    njSetkmColorMode(Trans ? 0x02100000u : 0x00080000u);
    put(0x8C10B360u,
        (Trans ? njStartVertexNonTexSpriteTrans(BaseColor) : njStartVertexNonTexSpriteOpaque(BaseColor)).a);
    guest::store<float>(0x8C10B3E4u, r);
}

void njDrawLineExEnd() {
    diff::Check check("njDrawLineExEnd", 0x8C02F8B2u);
    njEndVertex(word(0x8C10B360u));
}

// Lines as sprites, for Ninja's 2D lines: `param_5` lines, each from (x0, y0) to (x1, y1) at
// `param_4` and `param_6` bytes apart, at the depth `param_1`. A line becomes a quad around it,
// twice the half-width at 0x8C10B3E4 wide. The variable at 0x8C10B360 holds the store-queue
// address of the next sprite: 64 bytes, the control word, then the corners' x, y and z, but for
// the last corner's z.
void njCalcLine2D(float param_1, float param_2, float param_3, gptr<ulonglong> param_4, int param_5, int param_6,
                  ulonglong param_7, ulonglong param_8) {
    diff::Check check("njCalcLine2D", 0x8C02F79Cu, param_1, param_2, param_3, param_4, param_5, param_6, param_7,
                      param_8);
    constexpr uint32_t kNext = 0x8C10B360u;
    const float width = guest::load<float>(0x8C10B3E4u);
    const uint32_t control = word(0x8C02F84Cu), z = bits_of(param_1);
    uint32_t in = param_4.a, at = word(kNext), count = static_cast<uint32_t>(param_5);
    do {
        const float x0 = guest::load<float>(in), y0 = guest::load<float>(in + 4);
        const float x1 = guest::load<float>(in + 8), y1 = guest::load<float>(in + 12);
        in += static_cast<uint32_t>(param_6);
        const float dx = x1 - x0, dy = y1 - y0;
        const float scale = 1.0f / std::sqrt(dy * dy + dx * dx);
        const float nx = dx * width * scale, ny = dy * width * scale;
        const uint32_t first[8] = {control,          bits_of(x0 + ny), bits_of(y0 - nx), z,
                                   bits_of(x1 + ny), bits_of(y1 - nx), z,                bits_of(x1 - ny)};
        for (uint32_t i = 0; i < 8; ++i) put(at + 4 * i, first[i]);
        hw::prefetch(at);
        const uint32_t second[4] = {bits_of(y1 + nx), z, bits_of(x0 - ny), bits_of(y0 + nx)};
        for (uint32_t i = 0; i < 4; ++i) put(at + 32 + 4 * i, second[i]);
        hw::prefetch(at + 32);
        at += 64;
    } while (--count != 0);
    put(kNext, at);
}

namespace {

// Textured vertices in 3D at `p` (x, y, z, u, v and colour each) go through the current matrix and
// the projection at 0x8C10AF34 into the vertex buffer at 0x8C10AFA4: each vertex's colour, v, u, 1/w
// and screen y and x. Returns -1 when a vertex is beyond the far plane (0x8C10AF64), which leaves
// the strip out and stops there, 1 when a vertex is not beyond the near plane (0x8C10AF68), else 0.
int points_3d(uint32_t in, uint32_t count) {
    constexpr uint32_t kProjection = 0x8C10AF34u;
    const float sx = real(kProjection), sy = real(kProjection + 4), ox = real(kProjection + 8),
                oy = real(kProjection + 12);
    const float far_plane = real(0x8C10AF64u), near_plane = real(0x8C10AF68u);
    int state = 0;
    uint32_t at = word(0x8C10AFA4u), n = count;
    do {
        float t[4];
        transform(real(in), real(in + 4), real(in + 8), t);
        const float depth = -t[2], q = t[3] / depth;
        if (depth > far_plane)
            return -1;
        const uint32_t words[6] = {word(in + 20), word(in + 16), word(in + 12), bits_of(q),
                                   bits_of(t[1] * sy * q + oy), bits_of(t[0] * sx * q + ox)};
        for (uint32_t i = 0; i < 6; ++i) put(at + 4 * i, words[i]);
        if (!(depth > near_plane))
            state = 1;
        in += 24;
        at += 24;
    } while (--n != 0);
    return state;
}

// Sends the `count` vertices in the vertex buffer as one strip from `out`; njTextureClip3DP sends
// them when one is near. Each vertex's last word gets the projection's y offset, which Ninja leaves
// in fr7. Returns where the next vertex goes.
uint32_t send_3d(uint32_t count, uint32_t out, bool near) {
    const uint32_t buffer = word(0x8C10AFA4u);
    if (near)
        return njTextureClip3DP(gptr<uint>(buffer), static_cast<int>(count), gptr<dword>(out), gptr<dword>(0)).a;
    const uint32_t offset = word(0x8C10AF40u);
    for (uint32_t i = 0, v = buffer; i < count; ++i, v += 24, out += 32) {
        const uint32_t control = i == count - 1 ? 0xF0000000u : 0xE0000000u;
        const uint32_t words[8] = {control,     word(v + 20), word(v + 16), word(v + 12),
                                   word(v + 8), word(v + 4),  word(v),      offset};
        for (uint32_t k = 0; k < 8; ++k) put(out + 4 * k, words[k]);
        hw::prefetch(out);
    }
    return out;
}

}  // namespace

// A strip of `count` textured vertices in 3D at `p`, in the translucent list with `trans`.
void njDrawTexture3DEx(gptr<NJS_TEXTURE_VTX> p, Int count, Int trans) {
    diff::Check check("njDrawTexture3DEx", 0x8C030614u, p, count, trans);
    const int state = points_3d(p.a, static_cast<uint32_t>(count));
    if (state < 0)
        return;
    njSetkmColorMode(trans ? 0x0210000Au : 0x0008000Au);
    const uint32_t out = (trans ? njStartVertexTexTrans() : njStartVertexTexOpaque()).a;
    njEndVertex(send_3d(static_cast<uint32_t>(count), out, state != 0));
}

// More vertices of the strip that njDrawTexture3DExStart began, which keeps where they go at
// 0x8C10B360.
void njDrawTexture3DExSetData(gptr<NJS_TEXTURE_VTX> p, Int count) {
    diff::Check check("njDrawTexture3DExSetData", 0x8C030E24u, p, count);
    const int state = points_3d(p.a, static_cast<uint32_t>(count));
    if (state < 0)
        return;
    put(0x8C10B360u, send_3d(static_cast<uint32_t>(count), word(0x8C10B360u), state != 0));
}

// The user clip rectangle `p` (two corners) in `mode`. The mode goes into the control word's clip
// bits, and Kamui gets the rectangle for each list that 0x8C122B48 marks and for the current list.
// With mode 2 the bounds that Ninja's clipping uses at 0x8C10AF7C are the rectangle's tiles (32
// pixels each), otherwise the screen; 0x8C10AF6C holds them from the screen's centre.
void njUserClipping(Int mode, gptr<NJS_POINT2> p) {
    diff::Check check("njUserClipping", 0x8C0103F4u, mode, p);
    constexpr uint32_t kState = 0x8C122B40u, kPrevious = 0x8C0CCD80u, kBounds = 0x8C10AF64u;
    constexpr uint32_t kProjection = 0x8C10AF34u;
    const uint32_t current = word(0x8C10B15Cu + 56 * word(kState + 4));
    put(word(kContext), word(word(kContext)) & 0xFF7CFFFFu);
    const uint32_t bits = mode == 0 ? 0x800000u : mode == 2 ? 0x820000u : mode == 3 ? 0x830000u : 0;
    if (bits)
        put(word(kContext), word(word(kContext)) | bits);
    guest::Frame frame(16);
    const uint32_t rect = frame.top - 16;
    for (uint32_t i = 0; i < 16; i += 4) put(rect + i, ftrc(real(p.a + i)));
    for (uint32_t list = 0; list < 5; ++list)
        if (word(word(kState + 8) + 4 * list) || current == list)
            kmSetUserClipping(PKMVERTEXBUFFDESC(0x8C10B0F8u), static_cast<KMUSERCLIPMODE>(word(kPrevious)),
                              static_cast<KMLISTTYPE>(list), PKMRECT(rect));
    if (mode == 2) {
        put(kBounds + 24, bits_of(real(p.a) * 32.0f));
        put(kBounds + 28, bits_of(real(p.a + 4) * 32.0f));
        put(kBounds + 32, bits_of(std::fma(32.0f, real(p.a + 8), 31.0f)));
        put(kBounds + 36, bits_of(std::fma(32.0f, real(p.a + 12), 31.0f)));
    }
    else {
        put(kBounds + 24, 0);
        put(kBounds + 28, 0);
        put(kBounds + 32, bits_of(real(kProjection + 40) + -1.0f));
        put(kBounds + 36, bits_of(real(kProjection + 44) + -1.0f));
    }
    put(kBounds + 8, bits_of(real(kBounds + 32) - real(kProjection + 8)));
    put(kBounds + 12, bits_of(real(kBounds + 24) - real(kProjection + 8)));
    put(kBounds + 16, bits_of(real(kBounds + 36) - real(kProjection + 12)));
    put(kBounds + 20, bits_of(real(kBounds + 28) - real(kProjection + 12)));
    put(kPrevious, static_cast<uint32_t>(mode));
}

// Ninja's 3D control flags at 0x8C10AF98.
void njControl3D(Uint32 flag) {
    diff::Check check("njControl3D", 0x8C02F5E0u, flag);
    put(0x8C10AF98u, flag);
}

namespace {

// The projection at 0x8C10AF34: the x and y scale (0, 4), the screen's centre (8, 12), the scales'
// inverses (16, 20), the distance to the screen (24), the aspect's x and y (28, 32) and their
// ratio (36), and the screen's width (40). The scales are the distance times the aspect, y
// negated.
void scales() {
    constexpr uint32_t kProjection = 0x8C10AF34u;
    put(kProjection, bits_of(real(kProjection + 28) * real(kProjection + 24)));
    put(kProjection + 4, bits_of(-(real(kProjection + 32) * real(kProjection + 24))));
    put(kProjection + 16, bits_of(1.0f / real(kProjection)));
    put(kProjection + 20, bits_of(1.0f / real(kProjection + 4)));
}

}  // namespace

void njSetAspect(Float ax, Float ay) {
    diff::Check check("njSetAspect", 0x8C02F6F0u, ax, ay);
    put(0x8C10AF34u + 28, bits_of(ax));
    put(0x8C10AF34u + 32, bits_of(ay));
    put(0x8C10AF34u + 36, bits_of(ay / ax));
    scales();
}

// The field of view `ang`: the distance to the screen is half its width over the tangent of half
// the angle.
void njSetPerspective(Angle ang) {
    diff::Check check("njSetPerspective", 0x8C02F738u, ang);
    const int32_t half = (static_cast<int32_t>(ang) + (static_cast<int32_t>(ang) < 0 ? 1 : 0)) >> 1;
    const float tangent = njTan(static_cast<Angle>(half));
    put(0x8C10AF34u + 24, bits_of(real(0x8C10AF34u + 40) / (tangent * 2.0f)));
    scales();
}

// A strip of 2D lines through the `Count` points at `vtx`, at the depth `ooz`.
void njDrawLine2DExSetStrip(gptr<NJS_POINT2> vtx, Float ooz, Sint32 Count) {
    diff::Check check("njDrawLine2DExSetStrip", 0x8C02F854u, vtx, ooz, Count);
    njCalcLine2D(ooz, 0, 0, gptr<ulonglong>(vtx.a), Count - 1, 8, 0, 0);
}

namespace {

// The `count` points at `in` (x, y and z each) through the current matrix and the projection at
// 0x8C10AF34, for 3D lines: each point's screen x and y, its depth and 1/w go to `out`, 16 bytes a
// point. Returns how many points are not deeper than w. The machine code clears each other point's
// slot with movca before the point goes there, and with an odd count the word after the last point
// keeps that store: the count of the points before the last one.
int32_t line_points_3d(uint32_t in, uint32_t out, uint32_t count) {
    constexpr uint32_t kProjection = 0x8C10AF34u;
    const float sx = real(kProjection), sy = real(kProjection + 4), cx = real(kProjection + 8),
                cy = real(kProjection + 12);
    int32_t near = 0;
    for (uint32_t i = 0; i < count; ++i, in += 12) {
        const uint32_t at = out + 16 * i;
        if (i % 2 == 0)
            put(at + 16, static_cast<uint32_t>(near));
        float t[4];
        transform(real(in), real(in + 4), real(in + 8), t);
        const float depth = -t[2], q = t[3] / depth;
        if (!(depth > t[3]))
            ++near;
        put(at + 8, bits_of(depth));
        put(at + 12, bits_of(q));
        put(at, bits_of(t[0] * sx * q + cx));
        put(at + 4, bits_of(t[1] * sy * q + cy));
    }
    return near;
}

// 3D lines as sprites: `count` lines between the points at `in`, `stride` bytes apart, as from
// line_points_3d. A line becomes a quad around it on the screen, as njCalcLine2D makes, at its ends'
// 1/w; the sprite's last corner has none.
void line_sprites_3d(uint32_t in, uint32_t count, uint32_t stride) {
    constexpr uint32_t kNext = 0x8C10B360u;
    const float width = real(0x8C10B3E4u);
    const uint32_t control = word(0x8C02FA08u);
    uint32_t at = word(kNext);
    do {
        const float x0 = real(in), y0 = real(in + 4), x1 = real(in + 16), y1 = real(in + 20);
        const uint32_t q0 = word(in + 12), q1 = word(in + 28);
        in += stride;
        const float dx = x1 - x0, dy = y1 - y0;
        const float scale = 1.0f / std::sqrt(dy * dy + dx * dx);
        const float nx = dx * scale * width, ny = dy * scale * width;
        const uint32_t first[8] = {control, bits_of(x0 + ny), bits_of(y0 - nx), q0,
                                   bits_of(x1 + ny), bits_of(y1 - nx), q1, bits_of(x1 - ny)};
        for (uint32_t i = 0; i < 8; ++i) put(at + 4 * i, first[i]);
        hw::prefetch(at);
        const uint32_t second[4] = {bits_of(y1 + nx), q1, bits_of(x0 - ny), bits_of(y0 + nx)};
        for (uint32_t i = 0; i < 4; ++i) put(at + 32 + 4 * i, second[i]);
        hw::prefetch(at + 32);
        at += 64;
    } while (--count != 0);
    put(kNext, at);
}

}  // namespace

// A strip of 3D lines through the `Count` points at `vtx`: as sprites when no point is near the
// camera, through njCalcLine3DClip line by line when some are, and left out when all are.
void njDrawLine3DExSetStrip(gptr<NJS_POINT3> vtx, Sint32 Count) {
    diff::Check check("njDrawLine3DExSetStrip", 0x8C02FC14u, vtx, Count);
    uint32_t buffer = word(0x8C10AFA4u);
    const int32_t near = line_points_3d(vtx.a, buffer, static_cast<uint32_t>(Count));
    if (near == 0) {
        line_sprites_3d(buffer, static_cast<uint32_t>(Count - 1), 16);
        return;
    }
    if (near >= Count)
        return;
    for (uint32_t n = static_cast<uint32_t>(Count - 1); n != 0; --n, buffer += 16)
        njCalcLine3DClip(gptr<float>(buffer));
}

// A strip of `count` untextured 2D vertices at `p` (x, y, z and colour each) after the strip start
// that 0x8C10B360 keeps. The offset colour's word gets the last vertex's control word, which Ninja
// keeps in fr5.
void njDrawPolygon2DExSetData(gptr<NJS_POLYGON_VTX> p, Int count) {
    diff::Check check("njDrawPolygon2DExSetData", 0x8C02FD14u, p, count);
    uint32_t in = p.a, out = word(0x8C10B360u), n = static_cast<uint32_t>(count) - 1;
    const auto send_vertex = [&](uint32_t control) {
        const uint32_t words[4] = {control, word(in), word(in + 4), word(in + 8)};
        for (uint32_t i = 0; i < 4; ++i) put(out + 4 * i, words[i]);
        put(out + 24, word(in + 12));
        put(out + 28, 0xF0000000u);
        hw::prefetch(out);
        in += 16;
        out += 32;
    };
    do
        send_vertex(0xE0000000u);
    while (--n != 0);
    send_vertex(0xF0000000u);
    put(0x8C10B360u, out);
}

// An untextured strip of `count` vertices at `polygon` (x, y, z and colour each), in the
// translucent list with `trans`.
void njDrawPolygon(gptr<NJS_POLYGON_VTX> polygon, Int count, Int trans) {
    diff::Check check("njDrawPolygon", 0x8C01578Cu, polygon, count, trans);
    njSetkmColorMode(trans ? 0x02100002u : 0x00080002u);
    uint32_t out = (trans ? njStartVertexNonTexTrans() : njStartVertexNonTexOpaque()).a;
    uint32_t in = polygon.a, n = static_cast<uint32_t>(count);
    for (;;) {
        const bool last = --n == 0;
        const uint32_t words[4] = {last ? 0xF0000000u : 0xE0000000u, word(in), word(in + 4), word(in + 8)};
        for (uint32_t i = 0; i < 4; ++i) put(out + 4 * i, words[i]);
        put(out + 24, word(in + 12));
        hw::prefetch(out);
        in += 16;
        out += 32;
        if (last)
            break;
    }
    njEndVertex(out);
}

// The fog table `fog`, 128 entries, for the near and far distances `n` and `f` (negative, in front
// of the camera), and Kamui's fog density from `f`. Entry i is the far distance, or 1 when the near
// distance is within f / 256, over (16 + i % 16) * 2^(i / 16) / 16. With the far distance, the
// entries up to the first beyond the near distance then run from 1 down to 0, and the rest are 0.
void njGenerateFogTable3(gptr<Float> fog, Float n, Float f) {
    diff::Check check("njGenerateFogTable3", 0x8C0158BCu, fog, n, f);
    uint32_t density = 0x8010;
    if (!(-65535.0f > n)) {
        guest::Frame frame(8);
        const uint32_t exponent = frame.top - 8;
        const float mantissa = frexp(-f, gptr<int>(exponent));
        const int8_t shift = static_cast<int8_t>(guest::load<int32_t>(exponent) - 1);
        density = (ftrc(mantissa * 65536.0f) & 0xFF00u) | static_cast<uint32_t>(static_cast<int32_t>(shift));
    }
    kmSetFogDensity(density);
    const float scale = f / 256.0f > n ? -f : 1.0f;
    uint32_t inside = 0;
    for (int32_t i = 0; i < 128; ++i) {
        const float step = static_cast<float>(1 << (i >> 4)) * (static_cast<float>(i & 15) + 16.0f) / 16.0f;
        const float value = scale / step;
        put(fog.a + 4 * static_cast<uint32_t>(i), bits_of(value));
        if (value > -n)
            ++inside;
    }
    if (scale == -f) {
        const float end = real(fog.a + 4 * inside) + f;
        for (uint32_t i = 0; i < inside; ++i)
            put(fog.a + 4 * i, bits_of(1.0f - (real(fog.a + 4 * i) + f) / end));
        for (uint32_t i = inside; i < 128; ++i) put(fog.a + 4 * i, 0);
    }
}

namespace {

// Ninja's frame: whether a scene is open, the hooks at its start and end, and the framebuffer copy
// by DMA, when 0x8C10B3C8 gives the copy's memory. Kamui's system configuration is at 0x8C10B118,
// the passes' count and the current pass at 0x8C122B40.
constexpr uint32_t kSceneOpen = 0x8C0CCDCCu, kStartHook = 0x8C0CCD94u, kEndHook = 0x8C0CCD98u;
constexpr uint32_t kCopyMemory = 0x8C10B3C8u, kCopyDma = 0x8C10B36Cu, kCopyDone = 0x8C0CCDD0u;
constexpr uint32_t kSystem = 0x8C10B118u, kPasses = 0x8C122B40u;

// Calls the function that `at` points to, if any.
void hook(uint32_t at) {
    if (const uint32_t function = word(at)) {
        CallArgs args;
        guest::call(function, args);
    }
}

// Waits for the DMA whose number `at` holds.
void wait_dma(uint32_t at) {
    while (kmyQueryFinishDMA(static_cast<int>(word(at))) == 0x100u) {
    }
}

}  // namespace

// The strip context `param_1` of Ninja's contexts at 0x8C10AFD4, 48 bytes each.
void njSetCurrentContext(int param_1) {
    diff::Check check("njSetCurrentContext", 0x8C010138u, param_1);
    put(kContext, static_cast<uint32_t>(param_1) * 48 + 0x8C10AFD4u);
}

// The game's hooks at 0x8C0CCD8C and 0x8C0CCD90.
void njEORCallback() {
    diff::Check check("njEORCallback", 0x8C010910u);
    hook(0x8C0CCD8Cu);
}
void njEOVCallback() {
    diff::Check check("njEOVCallback", 0x8C010926u);
    hook(0x8C0CCD90u);
}

// Punch-through on or off: on a change, Ninja swaps the current pointers of the translucent list (8)
// and the punch-through list (16) in Kamui's vertex buffer, so its translucent strips go to the other.
void njSetPunchThroughMode(int param_1) {
    diff::Check check("njSetPunchThroughMode", 0x8C01163Au, param_1);
    constexpr uint32_t kPunchThrough = 0x8C10B368u;
    if (static_cast<uint32_t>(param_1) == word(kPunchThrough))
        return;
    put(kPunchThrough, static_cast<uint32_t>(param_1));
    const uint32_t current = word(kLists);
    const uint32_t punch_through = word(current + 16), translucent = word(current + 8);
    put(current + 8, punch_through);
    put(word(kLists) + 16, translucent);
}

void njBeginPass() {
    diff::Check check("njBeginPass", 0x8C011566u);
    put(kPasses + 4, 0);
    put(kPasses + 8, kPasses + 12);
    kmBeginPass(PKMVERTEXBUFFDESC(kLists));
}

// Ends the pass, with punch-through off, and continues Kamui's passes up to the last.
void njEndPass() {
    diff::Check check("njEndPass", 0x8C0115D0u);
    njSetPunchThroughMode(0);
    for (int32_t i = guest::load<int32_t>(kPasses + 4); i < guest::load<int32_t>(kPasses) - 1; ++i)
        kmContinuePass(PKMVERTEXBUFFDESC(kLists));
    kmEndPass(PKMVERTEXBUFFDESC(kLists));
}

// Opens the frame's scene. The first call of a frame calls the start hook and starts the framebuffer
// copy, if there is one; after the scene and its first pass begin, it chooses the first strip
// context, notes the timer and clears the frame's vertex and polygon counts.
void njStartDraw() {
    diff::Check check("njStartDraw", 0x8C011344u);
    if (!word(kSceneOpen)) {
        hook(kStartHook);
        if (word(kCopyMemory)) {
            guest::Frame frame(488);
            const uint32_t metrics = frame.top - 488;
            put(metrics, 8);
            kmGetSystemMetrics(PKMSYSTEMMETRICS(metrics));
            wait_dma(kCopyDone);
            const uint32_t surface = word(word(0x8C10B310u + (word(metrics + 0x1A8) ? 4 : 0)) + 28);
            njSetCH0DMAMode(static_cast<int>(word(0x8C10B3BCu)));
            const int handle = static_cast<int>(kmyGetDMAHANDLE(2).a);
            kmySetLocal32DMA(handle, word(kCopyMemory), word(0x8C10B3C0u) + surface, word(0x8C10B3C4u), 1);
            kmySetDMACallback(handle, 0x8C0112C8u);
            put(kCopyDma, kmyRequestDMA(gptr<undefined4>(static_cast<uint32_t>(handle))));
        }
    }
    kmBeginScene(PKMSYSTEMCONFIGSTRUCT(kSystem));
    njBeginPass();
    if (!word(kSceneOpen)) {
        njSetCurrentContext(0);
        put(0x8C10B35Cu, syTmrGetCount());
        put(0x8C10AFA8u, 0);
        put(0x8C10AFACu, 0);
        put(0x8C10AFB0u, 0);
        put(kSceneOpen, 1);
    }
}

// Renders the frame's scene: ends the pass, renders, ends the scene with bit 5 of its flags set
// for that call, copies the framebuffer back, if there is a copy, and calls the end hook. Returns
// what kmRender returns.
Sint32 njRender() {
    diff::Check check("njRender", 0x8C011484u);
    put(0x8C10B358u, syTmrGetCount());
    njEndPass();
    const KMINT32 result = kmRender(0);
    const uint32_t flags = kSystem + 4;
    if (word(flags) & 32) {
        kmEndScene(PKMSYSTEMCONFIGSTRUCT(kSystem));
    }
    else {
        put(flags, word(flags) | 32);
        kmEndScene(PKMSYSTEMCONFIGSTRUCT(kSystem));
        put(flags, word(flags) & ~32u);
    }
    if (word(kCopyMemory)) {
        wait_dma(kCopyDma);
        const int handle = static_cast<int>(kmyGetDMAHANDLE(2).a);
        kmySetTextureDMA(handle, word(0x8C10B3CCu), word(kCopyMemory), word(0x8C10B3C4u));
        put(kCopyDone, kmyRequestDMA(gptr<undefined4>(static_cast<uint32_t>(handle))));
    }
    put(kSceneOpen, 0);
    hook(kEndHook);
    return check.returns(result);
}

namespace {

// The colour `t` of the way from `from` to `to`, each byte interpolated on its own, as the chunk
// models' near clips mix it.
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

// One vertex of the 3D sprites' vertex buffer to the store queues: the control word, then its six
// words in reverse, x first. The last word keeps what the queue held.
uint32_t send_buffered(uint32_t out, uint32_t v, uint32_t control) {
    const uint32_t words[7] = {control, word(v + 20), word(v + 16), word(v + 12), word(v + 8), word(v + 4), word(v)};
    for (uint32_t i = 0; i < 7; ++i) put(out + 4 * i, words[i]);
    hw::prefetch(out);
    return out + 32;
}

}  // namespace

// The 3D sprites' strip when the near plane may cut it: `param_2` vertices of 24 bytes at `param_1`
// in the vertex buffer (colour, v, u, 1/w, y and x). A vertex lies in front of the near plane when
// its 1/w is above 0 and below 1 over the near distance at 0x8C10AF68. The triangles with all three
// vertices in front go out as the strip; a triangle with one behind breaks the strip, and when one
// of its vertices is in front, it goes to njTextureNearClip3DP. Returns where the next vertex goes.
gptr<dword> njTextureClip3DP(gptr<uint> param_1, int param_2, gptr<dword> param_3, gptr<dword> ctx_r12) {
    diff::Check check("njTextureClip3DP", 0x8C030764u, param_1, param_2, param_3);
    (void)ctx_r12;
    const float limit = 1.0f / real(0x8C10AF68u);
    const auto in_front = [limit](uint32_t v) {
        const float q = real(v + 12);
        return q > 0.0f && limit > q;
    };
    uint32_t out = param_3.a, a = param_1.a, b = a + 24, c = a + 48, open = 0;
    for (int32_t i = 2; i < param_2; ++i) {
        if (in_front(a) && in_front(b) && in_front(c)) {
            if (open == 0)
                out = send_buffered(out, a, 0xE0000000u);
            out = send_buffered(out, b, 0xE0000000u);
            ++open;
        }
        else {
            if (open) {
                out = send_buffered(out, b, 0xF0000000u);
                open = 0;
            }
            if (in_front(a) || in_front(b) || in_front(c))
                out = njTextureNearClip3DP(gptr<uint>(a), gptr<undefined4>(b), gptr<undefined4>(c), gptr<dword>(out)).a;
        }
        a = b;
        b = c;
        c += 24;
    }
    if (open)
        out = send_buffered(out, b, 0xF0000000u);
    return check.returns(gptr<dword>(out));
}

// A triangle of the 3D sprites that the near plane cuts, from njTextureClip3DP: its vertices in the
// vertex buffer and where its vertices go. Each edge (a to b, b to c, c to a) whose w crosses the
// near distance at 0x8C10AF68 gives a vertex there, interpolated in the view's x and y, u and v, and
// each byte of the colour, with 1/w the near distance's inverse. Each vertex beyond the near
// distance stays. The machine code builds the vertices in its frame, and so does this: what it does
// not write there is what the frame held. The three or four vertices go out as one strip.
gptr<dword> njTextureNearClip3DP(gptr<uint> param_1, gptr<undefined4> param_2, gptr<undefined4> param_3,
                                 gptr<dword> param_4) {
    diff::Check check("njTextureNearClip3DP", 0x8C030904u, param_1, param_2, param_3, param_4);
    const float ox = real(0x8C10AF3Cu), oy = real(0x8C10AF40u), kx = real(0x8C10AF44u), ky = real(0x8C10AF48u);
    // A vertex: its words, w, and its x and y in the view.
    struct Vertex {
        uint32_t at;
        float w, vx, vy;
    };
    const auto vertex = [&](uint32_t at) {
        Vertex r{at, 1.0f / real(at + 12), 0, 0};
        r.vy = (real(at + 16) - oy) * r.w * ky;
        r.vx = (real(at + 20) - ox) * r.w * kx;
        return r;
    };
    const Vertex in[3] = {vertex(param_1.a), vertex(param_2.a), vertex(param_3.a)};
    const float near_distance = real(0x8C10AF68u), inverse = 1.0f / near_distance;
    const float x_scale = real(0x8C10AF34u) * inverse, y_scale = real(0x8C10AF38u) * inverse;
    const uint32_t frame = guest::sp - 280;
    uint32_t n = 0;
    const auto keep = [&](const uint32_t (&words)[6]) {
        for (uint32_t i = 0; i < 6; ++i) put(frame + 24 * n + 4 * i, words[i]);
        ++n;
    };
    for (int i = 0; i < 3; ++i) {
        const Vertex &previous = in[i], &current = in[(i + 1) % 3];
        const bool beyond = current.w > near_distance;
        if (beyond != (previous.w > near_distance)) {
            const float t = (near_distance - previous.w) / (current.w - previous.w);
            const auto lerp = [t](float a, float b) { return std::fma(t, b - a, a); };
            const auto between = [&](uint32_t k) { return lerp(real(previous.at + k), real(current.at + k)); };
            const uint32_t words[6] = {mix_colour(word(previous.at), word(current.at), t),
                                       bits_of(between(4)),
                                       bits_of(between(8)),
                                       bits_of(inverse),
                                       bits_of(std::fma(lerp(previous.vy, current.vy), y_scale, oy)),
                                       bits_of(std::fma(lerp(previous.vx, current.vx), x_scale, ox))};
            keep(words);
        }
        if (beyond) {
            const uint32_t words[6] = {word(current.at),      word(current.at + 4),  word(current.at + 8),
                                       word(current.at + 12), word(current.at + 16), word(current.at + 20)};
            keep(words);
        }
    }
    uint32_t out = param_4.a;
    out = send_buffered(out, frame, 0xE0000000u);
    out = send_buffered(out, frame + 24, 0xE0000000u);
    if (n != 3)
        out = send_buffered(out, frame + 72, 0xE0000000u);
    out = send_buffered(out, frame + 48, 0xF0000000u);
    return check.returns(gptr<dword>(out));
}

// Ninja's wrappers of Kamui's wait for the vertical blank and its fog table.
KMSTATUS kmWaitVBlank() {
    diff::Check check("kmWaitVBlank", 0x8C011272u);
    return check.returns(kmWaitVBlank_8c037080());
}

KMSTATUS njSetFogTable(PKMFLOAT pfFogTable) {
    diff::Check check("njSetFogTable", 0x8C01589Eu, pfFogTable);
    return check.returns(kmSetFogTable(pfFogTable));
}

// The fog table's colour, which Kamui takes on the stack.
void njSetFogColor(Uint32 c) {
    diff::Check check("njSetFogColor", 0x8C015844u, c);
    kmSetFogTableColor(c, 0, 0, 0, pun<KMPACKEDARGB>(c));
}

// The fade: a fog table with each of its 128 entries `f`.
void njExecuteFade(Float f) {
    diff::Check check("njExecuteFade", 0x8C015876u, f);
    guest::Frame frame(512);
    const uint32_t table = frame.top - 512;
    for (uint32_t i = 0; i < 512; i += 4) guest::store<float>(table + i, f);
    kmSetFogTable(PKMFLOAT(table));
}

// The words that Ninja's 3D control flag 0x800 puts into a strip's head: through and_attr, then
// or_attr.
void njSetConstantAttr(Uint32 and_attr, Uint32 or_attr) {
    diff::Check check("njSetConstantAttr", 0x8C02F5E6u, and_attr, or_attr);
    put(0x8C10AFB4u, and_attr);
    put(0x8C10AFB8u, or_attr);
}

// Copies the colour at `mat` (alpha, red, green and blue) to the constant material at 0x8C10AFBC.
void njSetConstantMaterial(gptr<NJS_ARGB> mat) {
    diff::Check check("njSetConstantMaterial", 0x8C02F5F0u, mat);
    for (uint32_t i = 0; i < 16; i += 4) put(0x8C10AFBCu + i, word(mat.a + i));
}

}  // namespace game
