// Ninja's initialization and its end, natively: its globals, contexts, matrix stack, 3D state and
// the background plane. Each function does what its machine code does, and IKARUGA_DIFF tests
// each against its machine code on the game's own calls.
#include "decomp.h"
#include "diff.h"

namespace game {
namespace {

uint32_t word(uint32_t a) { return guest::load<uint32_t>(a); }
void put(uint32_t a, uint32_t v) { guest::store<uint32_t>(a, v); }
float real(uint32_t a) { return guest::load<float>(a); }
void put_real(uint32_t a, float v) { guest::store<float>(a, v); }

// The projection: the screen scales at 0 and 4, their inverses at 16 and 20, the distance to the
// screen at 24 and the aspect at 28 to 36 (njSetPerspective, njSetAspect).
constexpr uint32_t kProjection = 0x8C10AF34u;
// The far and near clip depths (njClipZ).
constexpr uint32_t kClip = 0x8C10AF64u;
// Ninja's flags, which njControl3D sets.
constexpr uint32_t kControl = 0x8C10AF98u;

}  // namespace

// A strip context of 44 bytes: the parameter control, ISP, TSP and texture control words of an
// opaque strip without a texture, and no texture selected.
void njInitContext(gptr<dword> param_1) {
    diff::Check check("njInitContext", 0x8C01014Cu, param_1);
    const uint32_t c = param_1.a;
    put(c, 0x808C0000u);
    put(c + 4, 0xC8000000u);
    put(c + 8, 0x949004C0u);
    put(c + 12, 0);
    put(c + 16, 0x949004C0u);
    put(c + 20, 0);
    put(c + 24, 0x00080012u);
    put(c + 28, 0xFFFFFFFFu);
    put(c + 32, 0xFFFFFFFFu);
    put(c + 36, 0xC8000000u << 2);
    put(c + 40, 0);
}

// Ninja's globals: the projection for a screen 500 away with an aspect of 1, the clip depths, the
// six strip contexts, the culling value and the two default vertex contexts at 0x8C1226C0 and
// 0x8C1227E0, each copied through a buffer on the stack, with 3 in its word at 64.
void njInitGlobal() {
    diff::Check check("njInitGlobal", 0x8C010D60u);
    guest::Frame frame(212);
    const uint32_t buffer = frame.top - 212;
    const uint32_t p = kProjection;
    put_real(p + 28, 1.0f);
    put_real(p + 32, 1.0f);
    put_real(p + 36, 1.0f);
    put_real(p + 24, 500.0f);
    put_real(p, real(p + 28) * real(p + 24));
    put_real(p + 4, -(real(p + 32) * real(p + 24)));
    put_real(p + 16, 1.0f / real(p));
    put_real(p + 20, 1.0f / real(p + 4));
    put_real(kClip, 60000.0f);
    put_real(kClip + 4, 1.0f);
    put(kControl, 0);
    put(0x8C0CD078u, 0);
    put(0x8C10B370u, 0x37800080u);
    for (uint32_t c = 0x8C10AFD4u; c < 0x8C10AFD4u + 0x120; c += 48)
        njInitContext(gptr<dword>(c));
    kmSetCullingRegister(0.01f);
    const uint32_t defaults[2][2] = {{0x8C0CA480u, 0x8C1226C0u}, {0x8C0CA540u, 0x8C1227E0u}};
    for (const auto& d : defaults) {
        _quick_mvn(0xC0, gptr<void>(buffer), gptr<void>(d[0]));
        for (uint32_t i = 0; i < 0xC0; i += 4) put(d[1] + i, word(buffer + i));
        put(d[1] + 64, 3);
    }
}

// Kamui's device, then Ninja's globals. Before Holly revision 10 the border turns orange and the
// function at 0x8C0CCDC4 runs first.
void FUN_8c01098a() {
    diff::Check check("FUN_8c01098a", 0x8C01098Au);
    if ((word(0xA05F689Cu) & 0xFF) < 10) {
        njSetBorderColor(0xFFFF6600u);
        guest::call(word(0x8C0CCDC4u), CallArgs{});
    }
    kmInitDevice(KMDEVICE(0));
    njInitGlobal();
    put(0x8C0CCDC8u, 0);
    put(0x8C0CCDCCu, 0);
}

// The matrix stack in `nSize` matrices at `pBuf`, from its first 32-byte boundary: its base, top and
// limit go to GBR + 384, 388 and 392. The first matrix is the unit matrix, and it becomes the
// current one in the FPU's back bank.
void njInitMatrix(gptr<NJS_MATRIX> pBuf, Uint32 nSize) {
    diff::Check check("njInitMatrix", 0x8C014764u, pBuf, nSize);
    const uint32_t end = pBuf.a + (nSize << 6), base = (pBuf.a + 31) & ~31u;
    put(cpu::gbr + 384, base);
    put(cpu::gbr + 388, base);
    put(cpu::gbr + 392, base + ((end - base) & ~31u) - 64);
    syMemCopy4(gptr<void>(base), gptr<void>(0x8C0CF560u), 16);
    for (uint32_t i = 0; i < 16; ++i) cpu::xf[i ^ 1] = real(base + 4 * i);
}

// The 3D state: the vertex buffer `vbuf` (`vn` is not used), no control flags, no current texture
// and the default material colour.
void njInit3D(gptr<NJS_VERTEX_BUF> vbuf, Int vn) {
    diff::Check check("njInit3D", 0x8C02F5B4u, vbuf, vn);
    put(0x8C10AFA4u, vbuf.a);
    put(kControl, 0);
    put(0x8C10AFB4u, 0xFFFFFFFFu);
    put(0x8C10AFB8u, 0);
    put_real(0x8C10AFBCu, 255.0f);
    put_real(0x8C10AFC0u, 0.0f);
    put_real(0x8C10AFC4u, 0.0f);
    put_real(0x8C10AFC8u, 0.0f);
}

// The near and far clip depths: each at most -1 and at least -65535, stored negated.
void njClipZ(Float n, Float f) {
    diff::Check check("njClipZ", 0x8C02F6B8u, n, f);
    const float low = guest::load<float>(0x8C02F6E8u);
    if (n > -1.0f)
        n = -1.0f;
    if (low > n)
        n = low;
    if (f > -1.0f)
        f = -1.0f;
    if (low > f)
        f = low;
    put_real(kClip + 4, -n);
    put_real(kClip, -f);
}

// The variable at 0x8C0CCD8C.
void FUN_8c0108f8(undefined4 param_1) {
    diff::Check check("FUN_8c0108f8", 0x8C0108F8u, param_1);
    put(0x8C0CCD8Cu, param_1);
}

// Ninja's count of vertical blanks for each frame, at 0x8C10AF94, and Kamui's.
void FUN_8c01126a(KMINT32 param_1) {
    diff::Check check("FUN_8c01126a", 0x8C01126Au, param_1);
    put(0x8C10AF94u, static_cast<uint32_t>(param_1));
    kmSetWaitVsyncCount(param_1);
}

// The background plane: a triangle over the screen, with the colours `col1` at the top left, `col2`
// at the top right and `col3` at the bottom left, behind everything else. Kamui reads fields of the
// vertex context that the machine code leaves as the stack held them, so the frame is where the
// machine code's is when C calls it: kPushRoom below the caller's stack, as sh4::run starts it.
void njSetBackColor(Uint32 col1, Uint32 col2, Uint32 col3) {
    diff::Check check("njSetBackColor", 0x8C010614u, col1, col2, col3);
    guest::Frame frame(308 + guest::Frame::kPushRoom + 8);
    const uint32_t sp = ((frame.top - guest::Frame::kPushRoom) & ~7u) - 308;
    const uint32_t v0 = sp + 12, v1 = sp + 44, v2 = sp + 76, context = sp + 108;
    put(sp + 252, 0x808C0002u);
    put(sp + 256, 0xC0000000u);
    put(sp + 260, 0x208004C0u);
    const uint32_t depth = 0x3727C5ACu;
    const uint32_t vertices[3][4] = {{v0, 0xE0000000u, 0, col1},
                                     {v1, 0xE0000000u, 0, col2},
                                     {v2, 0xF0000000u, 0, col3}};
    for (const auto& v : vertices) {
        put(v[0], v[1]);
        put(v[0] + 4, 0);
        put(v[0] + 8, 0);
        put(v[0] + 12, depth);
        put(v[0] + 24, v[3]);
    }
    put(v1 + 4, word(0x8C10AF5Cu));
    put(v2 + 8, word(0x8C10AF60u));
    put(sp, v0);
    put(sp + 4, v1);
    put(sp + 8, v2);
    kmSetBackGroundRenderState(PKMVERTEXCONTEXT(context));
    kmSetBackGroundPlane(gptr<PVOID>(sp), KMVERTEXTYPE(0), 32);
}

// The bytes that Ninja's vertex buffers and Kamui's work area take, rounded up to 32: the five
// buffers of each of the info's lists, a word for each vertex, and Kamui's work area for the info's
// vertex buffer and the passes.
Uint32 njCalcVertexBufferSize(gptr<NJS_VERTEXBUFFER_INFO> pVBInfo) {
    diff::Check check("njCalcVertexBufferSize", 0x8C011132u, pVBInfo);
    guest::Frame frame(68);
    const uint32_t config = frame.top - 68, info = pVBInfo.a;
    uint32_t total = 0;
    for (int32_t i = 0; i < static_cast<int32_t>(word(info + 16)); ++i)
        for (uint32_t j = 0; j < 5; ++j) {
            const int32_t n = static_cast<int32_t>(word(info + 32 + 24 * static_cast<uint32_t>(i) + 4 * j));
            total += static_cast<uint32_t>(n < 0 ? 0 : n) << 2;
        }
    put(config, 2);
    put(config + 4, 2);
    put(config + 8, word(info + 20));
    for (uint32_t i = 12; i <= 36; i += 4) put(config + i, 0);
    put(config + 40, word(info + 24));
    kmuCalculateKamuiWorkareaSize(gptr<int>(config));
    total += word(config + 52);
    return check.returns((total + 31) & ~31u);
}

// The display for the mode `param_1`, `param_2` (Ninja's frame buffer format) and `param_3` vertical
// blanks a frame. Bit 31 of the mode takes the cable from syVideoGetVideoMode and fits the mode to
// it; without it a mode that the cable cannot show fails with -1, as does bit 30 without a VGA
// cable. Bits 4 and 5 double the width and the height, and with bit 6, bits 17 to 19 pick the
// height of a PAL screen. The screen size goes to 0x8C10AF9C and 0x8C10AFA0, and as floats to
// 0x8C10AF5C and 0x8C10AF60; bit 16, Kamui's anti-aliasing, doubles the floats but a 480-line
// height.
undefined4 FUN_8c0109de(KMDISPLAYMODE param_1, int param_2, undefined4 param_3) {
    diff::Check check("FUN_8c0109de", 0x8C0109DEu, param_1, param_2, param_3);
    uint32_t mode = param_1;
    if (word(0x8C0CCDC8u)) {
        FUN_8c01124c();
        kmResetRenderer();
    }
    put(0x8C10AF94u, param_3);
    if (mode & 0x80000000u) {
        const int32_t video = syVideoGetVideoMode();
        mode &= 0x7FFFFFFFu;
        if (video == 1)
            mode &= ~0x40u & 0xFFF1FFFFu;
        else if (video == 0)
            mode = (mode & ~0xCu & ~0x40u & 0xFFF1FFFFu) | 1;
        else if (video == 2)
            mode |= 2;
    }
    else {
        const int32_t video = syVideoGetVideoMode();
        bool fits = true;
        if (mode & 0x40000000u)
            fits = video == 2;
        else if (video == 1)
            fits = (mode & 3) == 0;
        else if (video == 0)
            fits = (mode & 3) == 1;
        else if (video == 2)
            fits = (mode & 3) == 2;
        if (!fits)
            return check.returns(0xFFFFFFFFu);
    }
    put(0x8C10AF90u, static_cast<uint32_t>(param_2));
    put(0x8C10AF8Cu, mode);
    const uint32_t screen = 0x8C10AF34u, width = 0x8C10AF9Cu, height = 0x8C10AFA0u;
    const bool wide = mode & 16, tall = mode & 32;
    put_real(screen + 40, wide ? 640.0f : 320.0f);
    put(width, wide ? 640 : 320);
    if (!(mode & 64)) {
        put(height, tall ? 480 : 240);
        put_real(screen + 44, tall ? 480.0f : 240.0f);
    }
    else {
        const uint32_t pal = mode & 0xE0000u;
        const uint32_t heights[2][5] = {{0xF8, 0x100, 0x108, 0x110, 0x118}, {0x1F0, 0x200, 0x210, 0x220, 0x230}};
        if (pal <= 0x80000u)
            put(height, heights[tall][pal >> 17]);
        put_real(screen + 44, tall ? 480.0f : 240.0f);
    }
    uint32_t scaled = 0;
    if (mode & 0x10000u) {
        scaled = 1;
        put_real(screen + 40, real(screen + 40) * 2.0f);
        if (!tall)
            put_real(screen + 44, real(screen + 44) * 2.0f);
    }
    uint32_t bpp = height;
    switch (param_2) {
    case 0: bpp = 0; put(0x8C10B3D0u, 16); break;
    case 1: bpp = 1; put(0x8C10B3D0u, 16); break;
    case 3: bpp = 3; put(0x8C10B3D0u, 16); break;
    case 4: bpp = 4; put(0x8C10B3D0u, 24); break;
    case 5: bpp = 5; put(0x8C10B3D0u, 32); break;
    default: break;
    }
    put(0x8C10B3C8u, 0);
    kmSetDisplayMode(mode & 0xFFF, bpp, 1, scaled);
    if (mode & 64) {
        put(0x8C10AFD0u, static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>((mode & 0xE0000u) >> 17))));
        kmSetPALEXTCallback(PKMCALLBACKFUNC(0x8C010982u), PVOID(0));
    }
    for (uint32_t a = 0x8C0CCD8Cu; a <= 0x8C0CCD9Cu; a += 4) put(a, 0);
    kmSetEORCallback(PKMCALLBACKFUNC(0x8C010910u), PVOID(0));
    kmSetEndOfVertexCallback(PKMCALLBACKFUNC(0x8C010926u), PKMVOID(0));
    kmSetHSyncCallback(PKMCALLBACKFUNC(0x8C01093Cu), PVOID(0));
    kmSetHSyncLine(0);
    njInitGlobal2();
    put(0x8C0CCDC8u, 1);
    return check.returns(1u);
}

// Ninja's vertex buffers and Kamui's system configuration (KMSYSTEMCONFIGSTRUCT at 0x8C10B118)
// from the info `pVBInfo`: for each of its lists an entry of 56 bytes, the sizes of its five
// buffers (from 0x8C122B4C, 20 bytes a list) and each buffer's share of the vertex buffer in
// percent, then Kamui's work area after the buffers. A negative size is 0, and the entry keeps the
// last buffer that had one. Kamui takes the configuration with the interrupts masked.
void njInitVertexBufferEx(gptr<NJS_VERTEXBUFFER_INFO> pVBInfo) {
    diff::Check check("njInitVertexBufferEx", 0x8C010F28u, pVBInfo);
    guest::Frame frame(92);
    const uint32_t sp = frame.top - 92, info = pVBInfo.a;
    constexpr uint32_t config = 0x8C10B118u, sizes = 0x8C122B40u;
    const auto lists = [info] { return static_cast<int32_t>(word(info + 16)); };
    put(config, 0x1F8);
    put(config + 4, 0x00100000u);
    if (word(info + 12) == 1)
        put(config + 4, word(config + 4) | 32);
    put(0x8C10B310u, 0x8C10B318u);
    put(0x8C10B314u, 0x8C10B338u);
    put(config + 8, 0x8C10B310u);
    put(config + 36, 0x8C10B0F8u);
    for (int32_t i = 0; i < lists(); ++i) {
        const uint32_t list = static_cast<uint32_t>(i), entry = config + 56 + 56 * list;
        put(entry + 12, 0xFFFFFFFFu);
        put(sp, 56 * list);
        for (uint32_t j = 0; j < 5; ++j) {
            int32_t n = static_cast<int32_t>(word(info + 32 + 24 * list + 4 * j));
            if (n < 0) {
                put(entry + 12, j);
                put(config + 4, word(config + 4) | 16);
                n = 0;
            }
            put(sizes + 12 + 20 * list + 4 * j, static_cast<uint32_t>(n) << 2);
        }
        put(entry, word(info + 28 + 24 * list) ? 1 : 0);
    }
    put(sp + 4, 2);
    put(sp + 8, 2);
    put(sp + 12, word(info + 20));
    for (uint32_t i = 16; i <= 40; i += 4) put(sp + i, 0);
    put(sp + 44, word(info + 24));
    kmuCalculateKamuiWorkareaSize(gptr<int>(sp + 4));
    put(config + 52, word(info + 16));
    put(config + 12, 2);
    put(config + 40, 2);
    put(config + 24, word(sp + 48));
    put(config + 28, word(sp + 52));
    put(config + 32, word(info));
    put(config + 20, word(info + 8) << 2);
    const uint32_t rest = word(info + 4) - ((word(sp + 56) + 31) & ~31u);
    put(config + 48, rest);
    for (int32_t i = 0; i < lists(); ++i) {
        const uint32_t list = static_cast<uint32_t>(i);
        for (uint32_t j = 0; j < 5; ++j) {
            const float size = static_cast<float>(static_cast<int32_t>(word(sizes + 12 + 20 * list + 4 * j)));
            float total = static_cast<float>(static_cast<int32_t>(rest));
            if (static_cast<int32_t>(rest) < 0)
                total += 4294967296.0f;
            put_real(config + 56 + 56 * list + 16 + 4 * j, size / total * 100.0f);
        }
    }
    put(sp, (cpu::sr >> 4) & 15);
    put(config + 44, ((word(sp + 56) + 31) & ~31u) + word(info));
    put(sizes, word(info + 16));
    cpu::set_sr((cpu::sr & 0xFFFFFF0Fu) | 0xF0);
    kmyWaitVBlank();
    kmSetSystemConfiguration(PKMSYSTEMCONFIGSTRUCT(config));
    cpu::set_sr((cpu::sr & 0xFFFFFF0Fu) | ((word(sp) & 15) << 4));
    FUN_8c01126a(static_cast<KMINT32>(word(0x8C10AF94u)));
    put(0x8C10B3ACu, njCalcTexture(0));
    njSetCurrentContext(0);
}

// Ninja's end: Kamui's callbacks off, a scene that is open drawn, Kamui's device unloaded and the
// display unset.
void njExitSystem() {
    diff::Check check("njExitSystem", 0x8C011216u);
    kmSetEORCallback(PKMCALLBACKFUNC(0), PVOID(0));
    kmSetEndOfVertexCallback(PKMCALLBACKFUNC(0), PKMVOID(0));
    kmSetHSyncCallback(PKMCALLBACKFUNC(0), PVOID(0));
    if (word(0x8C0CCDCCu) == 1)
        njRender();
    kmUnloadDevice();
    put(0x8C0CCDC8u, 0);
}

}  // namespace game
