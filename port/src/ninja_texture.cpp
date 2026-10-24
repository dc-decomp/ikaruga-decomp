// Ninja's texture management, natively, as far as it goes. Each function does what its machine
// code does, and IKARUGA_DIFF tests each against its machine code on the game's own calls.
#include "decomp.h"
#include "diff.h"

namespace game {
namespace {

uint32_t word(uint32_t a) { return guest::load<uint32_t>(a); }
void put(uint32_t a, uint32_t v) { guest::store<uint32_t>(a, v); }

// The texture manage table, 24 bytes for each entry (NJS_TEXMANAGE), and its size. An entry's word
// at 8 is its bank, and the word at 12 points to its texture system entry, whose first word is the
// texture's global index. The texture system table holds 40 bytes for each entry (NJS_TEXSYSTEM); a
// free one has the index -1. The error code goes to 0x8C10B394.
constexpr uint32_t kManage = 0x8C10B374u, kManageSize = 0x8C10B378u;
constexpr uint32_t kSystem = 0x8C10B37Cu, kSystemSize = 0x8C10B380u, kError = 0x8C10B394u;
// The number of texture system entries in use, the error status (NJS_TEXERRSTAT) and the current
// context, whose words at 8 and 12 are the current texture's TSP and texture control words and
// whose word at 28 is the current texture's number.
constexpr uint32_t kSystemsUsed = 0x8C10B384u, kErrStat = 0x8C10B38Cu, kContext = 0x8C10B0F4u;
// The bytes of a texture with mipmaps, by its level (njGetTextureLevel), less 4.
constexpr uint32_t kMipmapSize = 0x8C0CD048u;

int32_t size(uint32_t variable) { return static_cast<int32_t>(word(variable)); }
uint32_t manage(uint32_t n) { return word(kManage) + 24 * n; }

// A texture surface (NJS_TEXSURFACE): its type, then the bit depth, the pixel format, the width,
// the height, the size in bytes, the surface flags and the address in texture memory.
enum : uint32_t { kType = 0, kBitDepth = 4, kPixelFormat = 8, kWidth = 12, kHeight = 16, kSize = 20,
                  kFlags = 24, kSurface = 28 };
constexpr uint32_t kMipmapped = 0x80000000u, kVq = 0x40000000u, kPalettized = 0x8000u;

// Ninja's format numbers (bits 8 to 15 of a texture type) of the palettized formats.
bool palettized(uint32_t format) {
    switch (format) {
    case 0x500: case 0x600: case 0x700: case 0x800: case 0x1300: case 0x1400: case 0x1500: case 0x1600:
        return true;
    default:
        return false;
    }
}

// The code that the PowerVR's TSP word takes for a side of 8 to 1024 texels, 0 to 7, or -1.
int32_t side_code(uint32_t n) {
    for (int32_t code = 0; code < 8; ++code)
        if (n == 8u << code)
            return code;
    return -1;
}

}  // namespace

// The entry of the texture with the global index `param_1`, or -1.
int njSearchTexManage(int param_1) {
    diff::Check check("njSearchTexManage", 0x8C0133D0u, param_1);
    for (int32_t i = 0; i < size(kManageSize); ++i) {
        const uint32_t system = word(word(kManage) + 24 * static_cast<uint32_t>(i) + 12);
        if (system && word(system) == static_cast<uint32_t>(param_1))
            return check.returns(i);
    }
    return check.returns(-1);
}

// A free texture manage entry into `pmng`, and with `psys` a free texture system entry into it.
// Returns the manage entry's number, or -1 with error 7 when one is missing.
Sint32 njGetTexManageNum(gptr<gptr<NJS_TEXMANAGE>> pmng, gptr<gptr<NJS_TEXSYSTEM>> psys) {
    diff::Check check("njGetTexManageNum", 0x8C013408u, pmng, psys);
    guest::store<uint32_t>(pmng.a, 0);
    int32_t found = 0;
    uint32_t entry = word(kManage);
    for (int32_t i = 0; i < size(kManageSize); ++i, entry += 24) {
        if (!word(entry + 12)) {
            guest::store<uint32_t>(pmng.a, entry);
            found = i;
            break;
        }
    }
    const auto error = [] {
        guest::store<uint32_t>(kError, 7);
        return -1;
    };
    if (!word(pmng.a))
        return check.returns(error());
    if (psys.a) {
        guest::store<uint32_t>(psys.a, 0);
        uint32_t system = word(kSystem);
        for (int32_t i = 0; i < size(kSystemSize); ++i, system += 40) {
            if (word(system) == 0xFFFFFFFFu) {
                guest::store<uint32_t>(psys.a, system);
                break;
            }
        }
        if (!word(psys.a))
            return check.returns(error());
    }
    return check.returns(found);
}

// The entry of the texture with the global index `param_1` in the bank `param_2`, or -1.
int njSearchTexManageBank(int param_1, int param_2) {
    diff::Check check("njSearchTexManageBank", 0x8C013470u, param_1, param_2);
    uint32_t entry = word(kManage);
    for (int32_t i = 0; i < size(kManageSize); ++i, entry += 24) {
        if (word(entry + 8) == static_cast<uint32_t>(param_2) && word(entry + 12) &&
            word(word(entry + 12)) == static_cast<uint32_t>(param_1))
            return check.returns(i);
    }
    return check.returns(-1);
}

// ---- Texture sizes and surfaces ----------------------------------------------------------------

// The level of a texture with sides `param_1` and `param_2`: the base-2 logarithm of the longer
// side, from 1 to 1024 texels, or -1.
undefined4 njGetTextureLevel(uint param_1, uint param_2) {
    diff::Check check("njGetTextureLevel", 0x8C012532u, param_1, param_2);
    const uint32_t side = param_1 >= param_2 ? param_1 : param_2;
    for (uint32_t level = 0; level <= 10; ++level)
        if (side == 1u << level)
            return check.returns(level);
    return check.returns(0xFFFFFFFFu);
}

namespace {

uint32_t mipmap_size(uint32_t surface) {
    const int32_t level = static_cast<int32_t>(njGetTextureLevel(word(surface + kWidth), word(surface + kHeight)));
    return word(kMipmapSize + 4 * static_cast<uint32_t>(level));
}

}  // namespace

// The bytes of a surface of 16-bit texels.
int njTextureSize_Normal(int param_1) {
    diff::Check check("njTextureSize_Normal", 0x8C012A10u, param_1);
    const uint32_t s = static_cast<uint32_t>(param_1);
    if (word(s + kFlags) & kMipmapped)
        return check.returns(static_cast<int>(mipmap_size(s) + 4));
    return check.returns(static_cast<int>(word(s + kWidth) * word(s + kHeight) * 2));
}

// The bytes of a VQ surface: its 2 KB codebook, then a byte for each 2 by 2 texels.
int njTextureSize_VQ(int param_1) {
    diff::Check check("njTextureSize_VQ", 0x8C012A42u, param_1);
    const uint32_t s = static_cast<uint32_t>(param_1);
    const uint32_t w = word(s + kWidth), h = word(s + kHeight);
    uint32_t n;
    if (word(s + kFlags) & kMipmapped) {
        n = ((mipmap_size(s) + 4) >> 3) + 10;
    }
    else {
        n = (w * h) >> 2;
        if (w == 8 && h == 8)
            n += 16;
    }
    return check.returns(static_cast<int>(n + 0x800));
}

// The bytes of a palettized surface, of 4 bits a texel in pixel format 0x28000000 and 8 otherwise.
uint njTextureSize_Palettized(int param_1) {
    diff::Check check("njTextureSize_Palettized", 0x8C012A90u, param_1);
    const uint32_t s = static_cast<uint32_t>(param_1);
    const bool four = word(s + kPixelFormat) == 0x28000000u;
    uint32_t n;
    if (word(s + kFlags) & kMipmapped)
        n = ((mipmap_size(s) + 4) >> 1) + (four ? 40 : 8);
    else
        n = word(s + kWidth) * word(s + kHeight);
    if (four)
        n >>= 1;
    return check.returns(n);
}

// The bytes of a small VQ surface, from Ninja's tables by level.
int njTextureSize_SVQ(int param_1) {
    diff::Check check("njTextureSize_SVQ", 0x8C012ADAu, param_1);
    const uint32_t s = static_cast<uint32_t>(param_1);
    const uint32_t level = 4 * static_cast<uint32_t>(njGetTextureLevel(word(s + kWidth), word(s + kHeight)));
    if (word(s + kFlags) & kMipmapped)
        return check.returns(static_cast<int>(word(0x8C0CCFF4u + level) + word(0x8C0CD02Cu + level) + 10));
    return check.returns(static_cast<int>(word(0x8C0CCFD8u + level) + word(0x8C0CD010u + level)));
}

// The bytes of a palettized VQ surface: its 2 KB codebook, then the indices.
int njTextureSize_VQPalette(int param_1) {
    diff::Check check("njTextureSize_VQPalette", 0x8C012B38u, param_1);
    const uint32_t s = static_cast<uint32_t>(param_1);
    const uint32_t w = word(s + kWidth), h = word(s + kHeight);
    const bool four = word(s + kPixelFormat) == 0x28000000u;
    uint32_t n;
    if (word(s + kFlags) & kMipmapped) {
        const uint32_t m = mipmap_size(s);
        n = four ? (m + 84) >> 5 : (m + 20) >> 4;
    }
    else if (four) {
        n = (w * h) >> 4;
        if (w == 8 && h == 8)
            n += 28;
        if (w == 16 && h == 16)
            n += 16;
    }
    else {
        n = (w * h) >> 3;
        if (w == 8 && h == 8)
            n += 24;
    }
    return check.returns(static_cast<int>(n + 0x800));
}

// The bytes of a surface, by its kind.
uint njTextureSizeAddDummy(gptr<uint> param_1) {
    diff::Check check("njTextureSizeAddDummy", 0x8C0125C0u, param_1);
    const uint32_t s = param_1.a;
    const int surface = static_cast<int>(s);
    if ((word(s + kType) & 0xFFFF) == 3)
        return check.returns(static_cast<uint>(njTextureSize_SVQ(surface)));
    const uint32_t flags = word(s + kFlags);
    if (flags & kVq)
        return check.returns(static_cast<uint>(flags & kPalettized ? njTextureSize_VQPalette(surface)
                                                                    : njTextureSize_VQ(surface)));
    return check.returns(flags & kPalettized ? njTextureSize_Palettized(surface)
                                             : static_cast<uint>(njTextureSize_Normal(surface)));
}

// Fills the surface `param_1` for a texture of `param_2` by `param_3` texels of the type
// `param_4`: Ninja's category in bits 0 to 7, its format in bits 8 to 15. Returns 0, or 4 for a
// side or a category the PowerVR has no code for, or a format that must be square and is not.
undefined4 njCheckTextureFormatLow(gptr<uint> param_1, uint param_2, uint param_3, uint param_4) {
    diff::Check check("njCheckTextureFormatLow", 0x8C012264u, param_1, param_2, param_3, param_4);
    const uint32_t s = param_1.a, w = param_2, h = param_3, category = param_4 & 0xFF;
    const uint32_t format = param_4 & 0xFF00;
    uint32_t result = 0;
    put(s + kType, (param_4 << 16) | (format == 0x1000 || format == 0x1100 ? 3 : 2));
    put(s + kBitDepth, 1);
    if (format == 0x500 || format == 0x600 || format == 0x1300 || format == 0x1400) {
        put(s + kPixelFormat, 0x28000000u);
    }
    else if (format == 0x700 || format == 0x800 || format == 0x1500 || format == 0x1600) {
        put(s + kPixelFormat, 0x30000000u);
    }
    else if (category <= 4) {
        put(s + kPixelFormat, category << 27);
    }
    else {
        result = 4;
    }
    put(s + kWidth, w);
    put(s + kHeight, h);
    const int32_t wc = side_code(w), hc = side_code(h);
    uint32_t sides = 0;
    if (wc < 0)
        result = 4;
    else
        sides = static_cast<uint32_t>(wc) << 3;
    if (hc < 0)
        result = 4;
    else
        sides |= static_cast<uint32_t>(hc);
    const bool rectangle_ok = format == 0x900 || format == 0xB00 || format == 0xD00 || format == 0x500 ||
                              format == 0x700;
    if (!rectangle_ok && w != h)
        result = 4;
    uint32_t flags = 0;
    switch (format) {
    case 0x200: case 0x1200: case 0x400: case 0x1100: case 0x600: case 0x800: case 0x1400: case 0x1600:
        flags |= kMipmapped;
        break;
    default:
        break;
    }
    if (format == 0xB00 || format == 0x900)
        flags |= 0x04000000u;
    switch (format) {
    case 0x300: case 0x400: case 0x1000: case 0x1100: case 0x1300: case 0x1500: case 0x1400: case 0x1600:
        flags |= kVq;
        break;
    default:
        break;
    }
    if (format == 0xB00)
        flags |= 0x02000000u;
    if (palettized(format))
        flags |= kPalettized;
    put(s + kFlags, flags | sides);
    put(s + kSize, njTextureSizeAddDummy(param_1));
    return check.returns(result);
}

// Checks the format of the surface `param_1` and allocates its texture memory through Kamui:
// with bit 8 of Ninja's flags at 0x8C10B11C from Kamui's second allocator. Returns 0, 4 for a
// format the PowerVR cannot take, or Kamui's error.
int njCreateTextureSurface(gptr<uint> param_1, uint param_2, uint param_3, uint param_4) {
    diff::Check check("njCreateTextureSurface", 0x8C012BD4u, param_1, param_2, param_3, param_4);
    const uint32_t s = param_1.a;
    uint32_t result = njCheckTextureFormatLow(param_1, param_2, param_3, param_4);
    if (result != 0)
        return check.returns(static_cast<int>(result));
    const uint32_t second = (word(0x8C10B11Cu) & 0x100) ? 1 : 0;
    if ((word(s + kType) & 0xFFFF) == 3) {
        const uint32_t bytes = kmtex_ConfirmSmallVQ(static_cast<int>(s));
        result = kmyAllocateSmallVQArea(second, static_cast<int>(bytes), gptr<int>(s + kSurface),
                                        cpu::ctx[11]);
    }
    else {
        result = kmyAllocateArea(second, word(s + kSize), gptr<uint>(s + kSurface));
    }
    if (result == 3)
        kmyRequestCallback(3);
    return check.returns(static_cast<int>(result));
}

// ---- The texture manage and system tables ------------------------------------------------------

// The PowerVR's code for a texture side of 8 to 1024 texels: 0 to 7. Another size stays as it is.
int ConvertUVnum(int param_1) {
    diff::Check check("ConvertUVnum", 0x8C013168u, param_1);
    const int32_t code = side_code(static_cast<uint32_t>(param_1));
    return check.returns(code < 0 ? param_1 : code);
}

// The manage entry's TSP and texture control words, from its surface: the sides, the pixel format,
// the flags and the texture's address, with the bank of a palettized texture.
void njSetTextureParamEx(gptr<NJS_TEXMANAGE> pmng) {
    diff::Check check("njSetTextureParamEx", 0x8C0131BEu, pmng);
    const uint32_t m = pmng.a, system = word(m + 12), s = system + 4;
    const uint32_t u = static_cast<uint32_t>(ConvertUVnum(static_cast<int>(word(s + kWidth))));
    const uint32_t v = static_cast<uint32_t>(ConvertUVnum(static_cast<int>(word(s + kHeight))));
    put(m, ((u << 3) & ~7u) | v);
    uint32_t texture = ((word(s + kFlags) | word(s + kPixelFormat)) & 0xFF800000u) | (word(s + kSurface) >> 3);
    const uint32_t format = (word(s + kType) >> 16) & 0xFF00;
    uint32_t bank = 0;
    bool banked = true;
    if (format == 0x500 || format == 0x600 || format == 0x1300 || format == 0x1400)
        bank = word(m + 8) & 63;
    else if (format == 0x700 || format == 0x800 || format == 0x1500 || format == 0x1600)
        bank = word(m + 8) & 48;
    else
        banked = false;
    if (banked)
        texture = (bank << 21) | (texture & 0xF81FFFFFu);
    put(m + 4, texture);
}

// Whether the texture type `nTextureType` is palettized.
Sint32 njCheckPaletteType(Uint32 nTextureType) {
    diff::Check check("njCheckPaletteType", 0x8C01330Au, nTextureType);
    return check.returns(palettized(nTextureType & 0xFF00) ? 1 : 0);
}

// Frees a manage entry and its system entry where their counts have come to 0.
void njFreeTexManage(gptr<undefined4> param_1) {
    diff::Check check("njFreeTexManage", 0x8C0134A2u, param_1);
    const uint32_t m = param_1.a, system = word(m + 12);
    if (static_cast<int32_t>(word(system + 36)) <= 0) {
        put(system, 0xFFFFFFFFu);
        for (uint32_t i = 4; i <= 32; i += 4)
            put(system + i, 0);
        put(kSystemsUsed, word(kSystemsUsed) - 1);
    }
    if (static_cast<int32_t>(word(m + 16)) <= 0) {
        put(m + 8, 0xFFFFFFFFu);
        put(m, 0);
        put(m + 4, 0);
        put(m + 12, 0);
        put(m + 20, 0);
    }
}

// Releases one use of a manage entry, and frees its texture memory with the last use of its system
// entry.
undefined4 njReleaseTextureLN(gptr<undefined4> param_1) {
    diff::Check check("njReleaseTextureLN", 0x8C012F9Cu, param_1);
    const uint32_t m = param_1.a;
    if (!word(m + 12))
        return check.returns(0xFFFFFFFFu);
    if (static_cast<int32_t>(word(m + 16)) > 0)
        put(m + 16, word(m + 16) - 1);
    const uint32_t system = word(m + 12);
    if (static_cast<int32_t>(word(system + 36)) > 0) {
        put(system + 36, word(system + 36) - 1);
        if (word(system + 36) == 0) {
            kmFreeTexture(PKMSURFACEDESC(system + 4));
            put(word(kContext) + 28, 0xFFFFFFFFu);
        }
    }
    njFreeTexManage(param_1);
    return check.returns(1u);
}

// Releases the textures of a list that are in use. Returns 1, or -1 with the first texture that
// failed in the error status.
Sint32 njReleaseTexture(gptr<NJS_TEXLIST> texlist) {
    diff::Check check("njReleaseTexture", 0x8C013074u, texlist);
    Sint32 result = 1;
    for (uint32_t i = 0; i < word(texlist.a + 4); ++i) {
        const uint32_t name = word(texlist.a) + 12 * i;
        if (!(word(name + 4) & 0x00200000u))
            continue;
        if (static_cast<int32_t>(njReleaseTextureLN(gptr<undefined4>(word(name + 8)))) < 0 && result != -1) {
            result = -1;
            put(kErrStat, i);
        }
        put(name + 4, word(name + 4) & 0xFFDFFFFFu);
        put(name + 8, 0);
    }
    return check.returns(result);
}

// Points the list's texture `tex_no` at the manage entry `texmng_no` and marks it in use.
void njLinkTexlist(gptr<NJS_TEXLIST> texlist, Sint32 tex_no, Sint32 texmng_no) {
    diff::Check check("njLinkTexlist", 0x8C013F86u, texlist, tex_no, texmng_no);
    const uint32_t name = word(texlist.a) + 12 * static_cast<uint32_t>(tex_no);
    put(name + 8, manage(static_cast<uint32_t>(texmng_no)));
    put(name + 4, word(name + 4) | 0x00200000u);
}

// The manage entry for a texture already in memory: the one with its global index, or for a
// palettized one the one in its bank, which a new entry with the texture's surface makes. Returns
// 1 with the entry's number in `texmng_no`, 0 when the texture is not in memory, or -1.
Sint32 njSearchTextureManageNum(Uint32 globalIndex, Uint32 bank, Uint32 nTextureType, gptr<Sint32> texmng_no) {
    diff::Check check("njSearchTextureManageNum", 0x8C013DDAu, globalIndex, bank, nTextureType, texmng_no);
    guest::Frame frame(36);
    const uint32_t local = frame.top - 36;
    const int32_t found = njSearchTexManage(static_cast<int>(globalIndex));
    const uint32_t entry = manage(static_cast<uint32_t>(found));
    const auto use = [](uint32_t m) {
        put(m + 16, word(m + 16) + 1);
        const uint32_t system = word(m + 12);
        put(system + 36, word(system + 36) + 1);
    };
    if (!njCheckPaletteType(nTextureType)) {
        guest::store<int32_t>(texmng_no.a, found);
        if (found < 0)
            return check.returns(0);
        use(entry);
        return check.returns(1);
    }
    if (found < 0) {
        guest::store<int32_t>(texmng_no.a, -1);
        return check.returns(0);
    }
    const int32_t in_bank = njSearchTexManageBank(static_cast<int>(globalIndex), static_cast<int>(bank));
    if (in_bank >= 0) {
        guest::store<int32_t>(texmng_no.a, in_bank);
        use(manage(static_cast<uint32_t>(in_bank)));
        return check.returns(1);
    }
    const Sint32 n = njGetTexManageNum(gptr<gptr<NJS_TEXMANAGE>>(local), gptr<gptr<NJS_TEXSYSTEM>>(0));
    put(local + 4, static_cast<uint32_t>(n));
    if (n < 0)
        return check.returns(-1);
    const uint32_t m = word(local);
    put(m + 8, bank);
    put(m + 12, word(entry + 12));
    const uint32_t system = word(m + 12);
    put(system + 36, word(system + 36) + 1);
    put(m + 16, 1);
    put(m + 20, word(entry + 20));
    njSetTextureParamEx(gptr<NJS_TEXMANAGE>(m));
    guest::store<int32_t>(texmng_no.a, n);
    return check.returns(1);
}

// The surface of a manage entry for the texture in a PVR header: its format and its texture
// memory. Returns 1, or -1 with error 6.
Sint32 njGetTexSurfaceEx(gptr<NJS_TEXMANAGE> texmanage, gptr<NJS_PVRHEADERDATA> pvrh) {
    diff::Check check("njGetTexSurfaceEx", 0x8C0137E8u, texmanage, pvrh);
    const uint32_t h = pvrh.a;
    if (njCreateTextureSurface(gptr<uint>(word(texmanage.a + 12) + 4), guest::load<uint16_t>(h + 4),
                               guest::load<uint16_t>(h + 6), word(h)) != 0) {
        put(kError, 6);
        return check.returns(-1);
    }
    return check.returns(1);
}

// A new manage entry and system entry for the texture in a PVR header. Returns the entry's number,
// or -1.
Sint32 njSetTextureManageNum(Uint32 globalIndex, Uint32 bank, gptr<NJS_PVRHEADERDATA> pvrh) {
    diff::Check check("njSetTextureManageNum", 0x8C013EF8u, globalIndex, bank, pvrh);
    guest::Frame frame(28);
    const uint32_t local = frame.top - 28;
    const Sint32 n = njGetTexManageNum(gptr<gptr<NJS_TEXMANAGE>>(local), gptr<gptr<NJS_TEXSYSTEM>>(local + 4));
    if (n < 0)
        return check.returns(-1);
    const uint32_t m = word(local);
    put(m + 12, word(local + 4));
    if (njGetTexSurfaceEx(gptr<NJS_TEXMANAGE>(m), pvrh) != 1) {
        put(m + 12, 0);
        return check.returns(-1);
    }
    if (globalIndex != 0xFFFFFFFFu)
        put(word(m + 12), globalIndex);
    put(m + 8, njCheckPaletteType(word(pvrh.a)) ? bank : 0xFFFFFFFFu);
    njSetTextureParamEx(gptr<NJS_TEXMANAGE>(m));
    put(m + 16, 1);
    put(word(m + 12) + 36, 1);
    put(kSystemsUsed, word(kSystemsUsed) + 1);
    return check.returns(n);
}

// Enters the list's texture `n` with the texture in a PVR header: an entry already in memory or a
// new one. Returns 1 for an entry in memory, 0 for a new one, or -1.
Sint32 njEntryTextureManageNum(gptr<NJS_TEXLIST> texlist, Sint32 n, Uint32 globalIndex, Uint32 bank,
                               gptr<NJS_PVRHEADERDATA> pvrh, gptr<Sint32> texmng_no) {
    diff::Check check("njEntryTextureManageNum", 0x8C01406Au, texlist, n, globalIndex, bank, pvrh, texmng_no);
    const Sint32 found = njSearchTextureManageNum(globalIndex, bank, word(pvrh.a), texmng_no);
    if (found == 0) {
        const Sint32 made = njSetTextureManageNum(globalIndex, bank, pvrh);
        guest::store<int32_t>(texmng_no.a, made);
        if (made < 0)
            return check.returns(-1);
        njLinkTexlist(texlist, n, guest::load<int32_t>(texmng_no.a));
        return check.returns(0);
    }
    if (found == 1) {
        njLinkTexlist(texlist, n, guest::load<int32_t>(texmng_no.a));
        return check.returns(1);
    }
    return check.returns(-1);
}

// For the list's texture `n`: the global index and bank that its name gives, and for a texture that
// shares the surface of the texture before it a manage entry in that bank. Returns 1 for a shared
// surface, 0 otherwise, or -1.
Sint32 njCheckAttrGlobalIndex(gptr<NJS_TEXLIST> texlist, Uint32 n, gptr<Uint32> globalIndex, gptr<Uint32> bank) {
    diff::Check check("njCheckAttrGlobalIndex", 0x8C012134u, texlist, n, globalIndex, bank);
    guest::Frame frame(24);
    const uint32_t local = frame.top - 24;
    const uint32_t name = word(texlist.a) + 12 * n, attr = word(name + 4);
    if ((attr & 0x10000u) && n > 0) {
        put(bank.a, (word(name + 8) & 0xFC000000u) >> 26);
        const uint32_t before = word(word(texlist.a) + 12 * (n - 1) + 8);
        put(globalIndex.a, word(word(before + 12)));
        const int32_t found = njSearchTexManageBank(static_cast<int>(word(globalIndex.a)),
                                                    static_cast<int>(word(bank.a)));
        if (found >= 0) {
            const uint32_t m = manage(static_cast<uint32_t>(found));
            put(local, m);
            put(name + 8, m);
            const uint32_t system = word(m + 12);
            put(system + 36, word(system + 36) + 1);
            put(m + 16, word(m + 16) + 1);
            return check.returns(1);
        }
        if (njGetTexManageNum(gptr<gptr<NJS_TEXMANAGE>>(local), gptr<gptr<NJS_TEXSYSTEM>>(0)) < 0)
            return check.returns(-1);
        const uint32_t m = word(local);
        put(name + 8, m);
        put(m + 8, word(bank.a));
        put(m + 12, word(before + 12));
        put(m + 16, 1);
        const uint32_t system = word(m + 12);
        put(system + 36, word(system + 36) + 1);
        put(m + 20, word(before + 20));
        njSetTextureParamEx(gptr<NJS_TEXMANAGE>(m));
        return check.returns(1);
    }
    const uint32_t address = word(name + 8);
    if (attr & 0x00800000u) {
        put(globalIndex.a, address);
        put(bank.a, 0xFFFFFFFFu);
        return check.returns(0);
    }
    put(globalIndex.a, attr & 0x01000000u ? address & 0x03FFFFFFu : 0xFFFFFFFFu);
    put(bank.a, (address & 0xFC000000u) >> 26);
    return check.returns(0);
}

// ---- Loading textures --------------------------------------------------------------------------

// The PVR header of a texture file into `pvrh` and its global index into `globalIndex`: the GBIX
// chunks before the PVRT chunk. Returns the texture's data after the header, or 0 with error 4 in
// the error status for another chunk.
gptr<Sint8> njLoadPvrHeader(gptr<NJS_PVRHEADERDATA> pvrh, gptr<Uint32> globalIndex, gptr<Sint8> pTexture) {
    diff::Check check("njLoadPvrHeader", 0x8C01376Cu, pvrh, globalIndex, pTexture);
    if (globalIndex.a)
        put(globalIndex.a, 0xFFFFFFFFu);
    uint32_t at = pTexture.a, chunk;
    do {
        chunk = word(at);
        at += 4;
        if (chunk == 0x54525650u) {  // PVRT
            at += 4;
            syMemCopy(gptr<void>(pvrh.a), gptr<void>(at), 8);
            at += 8;
        }
        else if (chunk == 0x58494247u) {  // GBIX
            const uint32_t length = word(at);
            at += 4;
            if (globalIndex.a)
                put(globalIndex.a, word(at));
            at += length & ~3u;
        }
        else {
            put(kErrStat + 8, 4);
            return check.returns(gptr<Sint8>(0));
        }
    } while (chunk != 0x54525650u);
    return check.returns(gptr<Sint8>(at));
}

// Copies the part `param_3` to `param_3 + param_4` of a small VQ texture to its surface `param_1`,
// in the two places Kamui keeps it: the codebook, then the indices from 0x800 on.
undefined4 njConvertSmallVQToMem(int param_1, gptr<undefined8> param_2, uint param_3, uint param_4, float arg_fr4,
                                 float arg_fr5, float arg_fr6, float arg_fr7) {
    diff::Check check("njConvertSmallVQToMem", 0x8C013530u, param_1, param_2, param_3, param_4);
    const uint32_t s = static_cast<uint32_t>(param_1), offset = param_3, bytes = param_4;
    const uint32_t level = 4 * static_cast<uint32_t>(njGetTextureLevel(word(s + kWidth), word(s + kHeight)));
    uint32_t codebook, indices;
    if (word(s + kFlags) & kMipmapped) {
        codebook = word(0x8C0CCFF4u + level);
        indices = word(0x8C0CD02Cu + level) + 10;
    }
    else {
        codebook = word(0x8C0CCFD8u + level);
        indices = word(0x8C0CD010u + level);
    }
    uint32_t first = 0, second = 0;
    if (codebook >= offset) {
        first = codebook - offset;
        if (bytes > first) {
            second = bytes - first;
            if (second > indices)
                second = indices;
        }
        else {
            first = bytes;
        }
    }
    else {
        second = bytes > indices ? indices : bytes;
    }
    uint32_t source = param_2.a;
    if (first) {
        kmyCopyToTexture(word(s + kSurface) + offset, gptr<undefined8>(source), first, gptr<undefined>(0), arg_fr4,
                         arg_fr5, arg_fr6, arg_fr7, static_cast<int>(first));
        if (!second)
            return check.returns(0u);
        source += first;
        kmyCopyToTexture(word(s + kSurface) + offset + 0x800, gptr<undefined8>(source), second, gptr<undefined>(0),
                         arg_fr4, arg_fr5, arg_fr6, arg_fr7, static_cast<int>(first));
    }
    else if (second) {
        kmyCopyToTexture(word(s + kSurface) + offset - codebook + 0x800, gptr<undefined8>(source), second,
                         gptr<undefined>(0), arg_fr4, arg_fr5, arg_fr6, arg_fr7, 0);
    }
    return check.returns(0u);
}

// Copies the part `param_3` to `param_3 + param_4` of a texture to its surface `param_1`, both
// rounded down to words; a VQ texture's part goes after its codebook word. Returns 0, or 7 for a
// part past the surface.
undefined4 njLoadTexturePart(gptr<uint> param_1, gptr<undefined8> param_2, uint param_3, uint param_4, float arg_fr4,
                             float arg_fr5, float arg_fr6, float arg_fr7) {
    diff::Check check("njLoadTexturePart", 0x8C013676u, param_1, param_2, param_3, param_4);
    const uint32_t s = param_1.a, bytes = param_4 & ~3u, offset = param_3 & ~3u;
    if (!bytes)
        return check.returns(0u);
    if ((word(s + kType) & 0xFFFF) == 3) {
        njConvertSmallVQToMem(static_cast<int>(s), param_2, param_3, param_4, arg_fr4, arg_fr5, arg_fr6, arg_fr7);
        return check.returns(0u);
    }
    const uint32_t start = offset + (((word(s + kType) >> 16) & 0xFF00) == 0x200 ? 4 : 0);
    const uint32_t total = word(s + kSize);
    if (start >= total)
        return check.returns(7u);
    uint32_t n = total - start;
    if (n > bytes)
        n = bytes;
    kmyCopyToTexture(word(s + kSurface) + start, param_2, n, gptr<undefined>(0), arg_fr4, arg_fr5, arg_fr6, arg_fr7,
                     static_cast<int>(bytes));
    return check.returns(0u);
}

// Copies the part `offset` to `offset + size` of a texture to the surface of the manage entry
// `texmng_no`. Returns 1, or -1 with error 1 for a free entry or 8 for a part past the surface.
Sint32 njLoadTexturePartManageNum(Sint32 texmng_no, gptr<Sint8> pTexture, Uint32 offset, Uint32 size) {
    diff::Check check("njLoadTexturePartManageNum", 0x8C013FB4u, texmng_no, pTexture, offset, size);
    const uint32_t m = manage(static_cast<uint32_t>(texmng_no));
    if (!word(m + 12)) {
        put(kError, 1);
        return check.returns(-1);
    }
    if (njLoadTexturePart(gptr<uint>(word(m + 12) + 4), gptr<undefined8>(pTexture.a), offset, size, 0, 0, 0, 0)) {
        put(kError, 8);
        return check.returns(-1);
    }
    return check.returns(1);
}

// Copies a whole texture to the surface of the manage entry `texmng_no`. Returns 1, or -1 with
// error 1 for a free entry or 8 when the copy fails.
Sint32 njLoadTextureManageNum(Sint32 texmng_no, gptr<Sint8> pTexture) {
    diff::Check check("njLoadTextureManageNum", 0x8C013FFAu, texmng_no, pTexture);
    guest::Frame frame(16);
    const uint32_t local = frame.top - 16;
    const uint32_t m = manage(static_cast<uint32_t>(texmng_no));
    if (!word(m + 12)) {
        put(kError, 1);
        return check.returns(-1);
    }
    put(local, m);
    put(local + 4, word(m + 12));
    const uint32_t system = word(m + 12);
    if (njLoadTexturePart(gptr<uint>(system + 4), gptr<undefined8>(pTexture.a), 0, word(system + 24), 0, 0, 0, 0)) {
        put(kError, 8);
        return check.returns(-1);
    }
    return check.returns(1);
}

// 1 when Kamui's last texture DMA has finished, else -1.
Sint32 njLoadTextureFinishDMA() {
    diff::Check check("njLoadTextureFinishDMA", 0x8C013958u);
    return check.returns(kmQueryFinishLastTextureDMA() == 0 ? 1 : -1);
}

// Upper-case letters for the lower-case ones of the string `param_1`, by the C library's character
// table. Returns the address of the string's end.
gptr<byte> FUN_8c01327c(gptr<byte> param_1) {
    diff::Check check("FUN_8c01327c", 0x8C01327Cu, param_1);
    uint32_t at = param_1.a;
    for (uint8_t c; (c = guest::load<uint8_t>(at)) != 0; ++at)
        if (guest::load<uint8_t>(0x8C0CC8A8u + c) & 2)
            guest::store<uint8_t>(at, static_cast<uint8_t>(c - 32));
    return check.returns(gptr<byte>(at));
}

// `namesrc` into `namedst`, with the extension `addext` when its last four characters, in upper
// case, are not that extension.
void njCheckFileName(gptr<char> namedst, gptr<char> namesrc, gptr<char> addext) {
    diff::Check check("njCheckFileName", 0x8C0132A4u, namedst, namesrc, addext);
    guest::Frame frame(24);
    const uint32_t local = frame.top - 24;
    put(local, addext.a);
    const int length = strlen(namesrc);
    _slow_strcpy(gptr<void>(local + 4), gptr<void>(namesrc.a + static_cast<uint32_t>(length) - 4));
    FUN_8c01327c(gptr<byte>(local + 4));
    if (guest::load<uint8_t>(local + 4) == '.' && _slow_strcmp1(gptr<uint>(local + 4), gptr<uint>(addext.a)) == 0) {
        _slow_strcpy(gptr<void>(namedst.a), gptr<void>(namesrc.a));
        return;
    }
    _slow_strcpy(gptr<void>(namedst.a), gptr<void>(namesrc.a));
    strcat(namedst, addext);
}

// The current context's texture: the manage entry with the global index `globalIndex`. Returns 1,
// or -1 when it is not in memory.
Sint32 njSetTextureNumG(Uint32 globalIndex) {
    diff::Check check("njSetTextureNumG", 0x8C013114u, globalIndex);
    const int found = njSearchTexManage(static_cast<int>(globalIndex));
    if (found < 0)
        return check.returns(-1);
    const uint32_t m = manage(static_cast<uint32_t>(found));
    put(word(kContext) + 8, (word(word(kContext) + 8) & ~63u) | (word(m) & 63));
    put(word(kContext) + 12, word(m + 4));
    return check.returns(1);
}

// A name entry of a texture list.
void njSetTextureName(gptr<NJS_TEXNAME> texname, gptr<void> addr, Uint32 globalIndex, Uint32 attr) {
    diff::Check check("njSetTextureName", 0x8C01310Cu, texname, addr, globalIndex, attr);
    put(texname.a, addr.a);
    put(texname.a + 4, attr);
    put(texname.a + 8, globalIndex);
}

// The free texture memory: for `type` 0 its size, for 2 Ninja's own count at 0x8C10B3AC, and for
// another type the largest free block.
Uint32 njCalcTexture(Uint32 type) {
    diff::Check check("njCalcTexture", 0x8C014174u, type);
    if (type == 2)
        return check.returns(word(0x8C10B3ACu));
    guest::Frame frame(16);
    const uint32_t local = frame.top - 16;
    put(local + 8, type);
    kmGetFreeTextureMem(PKMUINT32(local), PKMUINT32(local + 4));
    return check.returns(word(type == 0 ? local : local + 4));
}

// ---- Palettes ----------------------------------------------------------------------------------

// The palette bank of the current context's texture.
void njSetPaletteBank(uint param_1) {
    diff::Check check("njSetPaletteBank", 0x8C012C80u, param_1);
    const uint32_t context = word(kContext);
    put(context + 12, (word(context + 12) & 0xF81FFFFFu) | ((param_1 & 63) << 21));
}

// The palette's colour mode: 0 to 2 are Kamui's 16-bit modes, 6 its 32-bit mode. The entry size,
// 16 or 32 bits, goes to 0x8C10B3B4.
void njSetPaletteMode(Uint32 mode) {
    diff::Check check("njSetPaletteMode", 0x8C012D50u, mode);
    put(0x8C10B3B0u, mode);
    uint32_t kamui = 0;
    if (mode == 6) {
        put(0x8C10B3B4u, 32);
        kamui = 3;
    }
    else {
        if (mode == 1 || mode == 2)
            kamui = mode;
        put(0x8C10B3B4u, 16);
    }
    kmySetPaletteMode(kamui);
}

// `count` palette entries from `data` into the PowerVR's palette memory from the entry `offset`: as
// halfwords in a 16-bit mode, as words in the 32-bit one.
void njSetPaletteData(Sint32 offset, Sint32 count, gptr<void> data) {
    diff::Check check("njSetPaletteData", 0x8C012DC2u, offset, count, data);
    const uint32_t palette = (0x005F9000u + 4 * static_cast<uint32_t>(offset)) | 0xA0000000u;
    if (word(0x8C10B3B4u) == 16) {
        uint32_t from = data.a, to = palette;
        for (int32_t i = offset; i < offset + count; ++i, from += 2, to += 4)
            put(to, guest::load<uint16_t>(from));
        return;
    }
    syMemCopy4(gptr<void>(palette), data, count);
}

// A palette file (PVPL) into the palette `pl`, in the bank that the file names. Returns 1, or -1
// for another file.
Sint32 njLoadPaletteData(gptr<void> data, gptr<NJS_TEXPALETTE> pl) {
    diff::Check check("njLoadPaletteData", 0x8C012E82u, data, pl);
    njInitTextureErrStat();
    const uint32_t d = data.a, p = pl.a;
    if (word(d) != 0x4C505650u)  // PVPL
        return check.returns(-1);
    const uint32_t head = d + 4;
    njSetPaletteBank(static_cast<uint32_t>(static_cast<int32_t>(guest::load<int16_t>(head + 6))));
    syMemCopy(gptr<void>(word(p)), gptr<void>(d + 16), static_cast<Sint32>(word(head) - 8));
    guest::store<uint16_t>(p + 6, guest::load<uint16_t>(head + 6));
    guest::store<uint16_t>(p + 8, guest::load<uint16_t>(head + 8));
    guest::store<uint16_t>(p + 10, guest::load<uint16_t>(head + 10));
    guest::store<uint16_t>(p + 4, guest::load<uint8_t>(head + 4));
    return check.returns(1);
}

// ---- Initialization ----------------------------------------------------------------------------

// The texture manage table: `param_2` free entries at `param_1`.
void njInitTexManage(gptr<undefined4> param_1, int param_2) {
    diff::Check check("njInitTexManage", 0x8C013374u, param_1, param_2);
    put(kManage, param_1.a);
    put(kManageSize, static_cast<uint32_t>(param_2));
    for (uint32_t m = param_1.a, end = m + 24 * static_cast<uint32_t>(param_2 > 0 ? param_2 : 0); m < end; m += 24) {
        put(m, 0);
        put(m + 4, 0);
        put(m + 8, 0xFFFFFFFFu);
        for (uint32_t i = 12; i <= 20; i += 4)
            put(m + i, 0);
    }
}

// The texture system table: `param_2` free entries at `param_1`.
void njInitTexSystem(gptr<undefined4> param_1, int param_2) {
    diff::Check check("njInitTexSystem", 0x8C01339Eu, param_1, param_2);
    put(kSystem, param_1.a);
    put(kSystemSize, static_cast<uint32_t>(param_2));
    for (uint32_t s = param_1.a, end = s + 40 * static_cast<uint32_t>(param_2 > 0 ? param_2 : 0); s < end; s += 40) {
        put(s, 0xFFFFFFFFu);
        for (uint32_t i = 4; i <= 36; i += 4)
            put(s + i, 0);
    }
}

// Both texture tables.
void njInitTextureEx(gptr<NJS_TEXMANAGE> pmng, Int nmng, gptr<NJS_TEXSYSTEM> psys, Int nsys) {
    diff::Check check("njInitTextureEx", 0x8C014114u, pmng, nmng, psys, nsys);
    njInitTexManage(gptr<undefined4>(pmng.a), nmng);
    njInitTexSystem(gptr<undefined4>(psys.a), nsys);
}

// The error status: no texture, no error.
void njInitTextureErrStat() {
    diff::Check check("njInitTextureErrStat", 0x8C01420Cu);
    put(kErrStat, 0xFFFFFFFFu);
    put(kErrStat + 4, 0xFFFFFFFFu);
    put(kErrStat + 8, 0);
    put(kErrStat + 12, 0);
    put(kErrStat + 16, 0);
}

// Every texture released: the system entries' texture memory freed and the entries cleared but for
// their use counts, the manage entries cleared, and no current texture.
void njReleaseTextureAll() {
    diff::Check check("njReleaseTextureAll", 0x8C012F00u);
    uint32_t s = word(kSystem);
    for (int32_t i = 0; i < size(kSystemSize); ++i, s += 40) {
        if (word(s) != 0xFFFFFFFFu)
            kmFreeTexture(PKMSURFACEDESC(s + 4));
        put(s, 0xFFFFFFFFu);
        for (uint32_t k = 4; k <= 32; k += 4)
            put(s + k, 0);
    }
    uint32_t m = word(kManage);
    for (int32_t i = 0; i < size(kManageSize); ++i, m += 24) {
        put(m + 8, 0xFFFFFFFFu);
        put(m, 0);
        put(m + 4, 0);
        for (uint32_t k = 12; k <= 20; k += 4)
            put(m + k, 0);
    }
    put(kSystemsUsed, 0);
    put(word(kContext) + 28, 0xFFFFFFFFu);
}

// Ninja's texture end.
void njExitTexture() {
    diff::Check check("njExitTexture", 0x8C01412Cu);
    njReleaseTextureAll();
}

}  // namespace game
