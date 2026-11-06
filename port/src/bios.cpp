// The boot ROM's services, which the game reaches through the SDK's small wrappers around the
// vectors at 0x8C0000B0 to 0x8C0000E0: the system flash, the ROM font and the console ID. The
// wrappers are replaced here (port/replaced.txt), so no boot ROM code is needed; the font is read
// from a boot ROM image when the user supplies one. The GD-ROM drive's wrappers are not reached:
// the game's file system is native (files.cpp).
#ifndef _WIN32
#include <execinfo.h>
#endif

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "decomp.h"
#include "disc.h"
#include "hw.h"
#include "text.h"

namespace {

// ---- the system flash: a synthetic image with the five partitions --------------------------------

std::vector<uint8_t> flash(128 * 1024, 0xFF);
const uint32_t kPartition[5][2] = {
    {0x1A000, 0x2000}, {0x18000, 0x2000}, {0x1C000, 0x4000}, {0x10000, 0x8000}, {0x00000, 0x10000}};

struct FlashInit {
    FlashInit() {
        // Factory data: region, broadcast and language, all Japan (the disc's region).
        const char* id = "00000";
        std::memcpy(&flash[0x1A000], id, 5);
        // The console's unique ID.
        const uint8_t unique[8] = {0x12, 0x34, 0x56, 0x78, 0x9A, 0xBC, 0xDE, 0xF0};
        std::memcpy(&flash[0x1A056], unique, 8);
        const char* magic = "KATANA_FLASH____";
        std::memcpy(&flash[0x1C000], magic, 16);
        std::memcpy(&flash[0x10000], magic, 16);
    }
} flash_init;

constexpr uint32_t kIdAddress = 0x8C000068u;

// SYSINFO_INIT: the ID (8 bytes at flash 0x1A056) and the factory settings (5 bytes at 0x1A000:
// country, language and broadcast at 2 to 4) into the 24-byte block that SYSINFO_ID returns.
// syVideoInit reads the broadcast standard there, and without it the game chose interlaced TV
// output instead of VGA.
void sysinfo_init() {
    for (uint32_t i = 0; i < 24; ++i) {
        const uint8_t b = i < 8 ? flash[0x1A056 + i] : i < 13 ? flash[0x1A000 + i - 8] : 0;
        guest::store<uint8_t>(kIdAddress + i, b);
    }
}

}  // namespace

namespace hw {

bool open_disc(const std::string& path, std::string& error) {
    return disc::open(path, error);
}

namespace {
bool g_rom_loaded = false;
}

bool load_rom(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f)
        return false;
    f.read(reinterpret_cast<char*>(guest::rom()), kRomSize);
    return g_rom_loaded = true;
}

bool rom_loaded() { return g_rom_loaded; }

}  // namespace hw

// Calls through the vectors themselves, in case code reaches one outside the wrappers. The vectors
// hold these addresses, in the boot ROM's range, where the game has no code.
namespace bios {
bool call(uint32_t address, const CallArgs& args, CallResult& result) {
    if (address < 0xA0000B00u || address >= 0xA0000B20u)
        return false;
    std::fprintf(stderr, "boot ROM vector 0x%08x called directly (r4 %08x r5 %08x r6 %08x r7 %08x)\n",
                 address, args.r[0], args.r[1], args.r[2], args.r[3]);
    result.r0 = 0;
    return true;
}

void install_vectors() {
    for (uint32_t i = 0; i < 6; ++i) guest::store<uint32_t>(0x8C0000B0u + 4 * i, 0xA0000B00u + 4 * i);
    guest::store<uint32_t>(0x8C0000E0u, 0xA0000B18u);
    // The boot ROM fills the block before the game starts, as dream-recomp's does.
    sysinfo_init();
}
}  // namespace bios

// ---- the replaced wrappers -----------------------------------------------------------------------

namespace game {

gptr<void> syBtFntGet(Uint32 num) {
    (void)num;
    return gptr<void>(0xA0100020u);  // the font in the boot ROM
}
int syBtFntChkSmph() { return 0; }
void syBtFntClrSmph() {}

// The glyph of the Shift-JIS character `code` in the boot ROM's 24-dot font, 72 bytes, for the
// game's text textures. Without a boot ROM image macOS's Hiragino Sans draws it, and ENGLISH TEXT
// draws its own cells (text.h).
undefined4 fontGlyphGet(gptr<undefined4> out, Sint16 code) {
    diff::Check check("fontGlyphGet", 0x8C06AA40u, out, code);
    uint8_t glyph[72];
    if (text::glyph(static_cast<uint16_t>(code), hw::rom_loaded(), out.a, glyph)) {
        diff::own_state();
        for (uint32_t i = 0; i < 0x48; ++i)
            guest::store<uint8_t>(out.a + i, glyph[i]);
        return check.returns(undefined4(0));
    }
    const Uint32 at = syBtFntGetAddr(SYE_BT_FNT_FONTSET_JP_LVL2_24, code);
    for (uint32_t i = 0; i < 0x48; ++i)
        guest::store<uint8_t>(out.a + i, guest::load<uint8_t>(0xA0100020u + at + i));
    return check.returns(undefined4(0));
}

undefined4 kbCfgIni() {
    sysinfo_init();
    return 0;
}
undefined4 kbCfgGetId() { return kIdAddress; }

int syBtFlGetInfo(int partition, gptr<uint> info) {
    if (partition < 0 || partition > 4)
        return -1;
    info[0] = kPartition[partition][0];
    info[1] = kPartition[partition][1];
    return 0;
}
int syBtFlRead(uint offset, gptr<void> buffer, uint size) {
    for (uint i = 0; i < size && offset + i < flash.size(); ++i)
        guest::store<uint8_t>(buffer.a + i, flash[offset + i]);
    return 0;
}
int syBtFlWrite(uint offset, gptr<void> buffer, uint size) {
    diff::guard("writes the flash");
    for (uint i = 0; i < size && offset + i < flash.size(); ++i)
        flash[offset + i] &= guest::load<uint8_t>(buffer.a + i);  // flash only clears bits
    return static_cast<int>(size);
}
int syBtFlDeleteSct(uint offset) {
    diff::guard("writes the flash");
    for (const auto& p : kPartition)
        if (offset >= p[0] && offset < p[0] + p[1])
            std::memset(&flash[p[0]], 0xFF, p[1]);
    return 0;
}

void syBtExit() {
    diff::guard("exits");
#ifndef _WIN32
    void* frames[32];
    const int n = backtrace(frames, 32);
    std::fprintf(stderr, "syBtExit called from:\n");
    backtrace_symbols_fd(frames, n, 2);
#else
    std::fprintf(stderr, "syBtExit called\n");
#endif
    throw ExitRequested{};
}
Sint32 syBtCheckDisc() { return 0; }

}  // namespace game
