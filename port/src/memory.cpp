// Guest memory: main RAM, video memory, sound memory and the boot ROM are the buffers of
// dream-recomp's DcMemory (port/src/machine.cpp), reached directly; every other access goes to the
// hardware layer.
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <vector>

#include "hw.h"

namespace guest {

uint8_t* ram = nullptr;  // set when the machine is built
uint32_t sp = 0;

namespace {
std::vector<uint8_t> sq_buf(64);  // the store queues; a flush copies them to DcMemory's
}  // namespace

uint8_t* store_queues() { return sq_buf.data(); }

// Areas with plain memory behind them. The 64-bit video memory path (0x04000000) is the layout
// textures are stored in; the 32-bit path (0x05000000) interleaves the two banks and goes through
// hw::vram32_read/write.
uint8_t* direct(uint32_t a) {
    static uint8_t* const vram_buf = vram();
    static uint8_t* const aram_buf = aram();
    static uint8_t* const rom_buf = rom();
    if (a >= 0xE0000000u) {
        if (a < 0xE4000000u)
            return sq_buf.data() + (a & 0x3Fu);  // the store queues; a flush is hw::prefetch
        return nullptr;
    }
    const uint32_t p = a & 0x1FFFFFFFu;
    if (p >= 0x0C000000u && p < 0x10000000u)
        return ram + (p & (kRamSize - 1));
    if ((p >= 0x04000000u && p < 0x05000000u) || (p >= 0x06000000u && p < 0x07000000u))
        return vram_buf + (p & (hw::kVramSize - 1));
    if (p >= 0x00800000u && p < 0x00A00000u)
        return aram_buf + (p & (hw::kAramSize - 1));
    if (p < 0x00200000u)
        return rom_buf + p;
    return nullptr;
}

void g2_wait() { hw::tick(40); }

uint32_t io_read(uint32_t a, unsigned size) { return hw::read(a, size); }
void io_write(uint32_t a, uint32_t value, unsigned size) { hw::write(a, value, size); }

void io_read_block(uint32_t a, void* out, unsigned size) {
    auto* o = static_cast<uint8_t*>(out);
    for (unsigned i = 0; i < size; i += 4) {
        const uint32_t v = hw::read(a + i, 4);
        std::memcpy(o + i, &v, size - i < 4 ? size - i : 4);
    }
}

void io_write_block(uint32_t a, const void* in, unsigned size) {
    const auto* p = static_cast<const uint8_t*>(in);
    for (unsigned i = 0; i < size; i += 4) {
        uint32_t v = 0;
        std::memcpy(&v, p + i, size - i < 4 ? size - i : 4);
        hw::write(a + i, v, 4);
    }
}

uint32_t address_of(const void* host) {
    const auto* p = static_cast<const uint8_t*>(host);
    if (p >= ram && p < ram + kRamSize)
        return 0x8C000000u + static_cast<uint32_t>(p - ram);
    if (p >= vram() && p < vram() + hw::kVramSize)
        return 0xA4000000u + static_cast<uint32_t>(p - vram());
    if (p >= aram() && p < aram() + hw::kAramSize)
        return 0xA0800000u + static_cast<uint32_t>(p - aram());
    if (p >= sq_buf.data() && p < sq_buf.data() + sq_buf.size())
        return 0xE0000000u + static_cast<uint32_t>(p - sq_buf.data());
    // The boot ROM area, which is also where arithmetic on a null pointer lands.
    if (p >= rom() && p < rom() + hw::kRomSize)
        return static_cast<uint32_t>(p - rom());
    std::fprintf(stderr, "address_of: host pointer %p is not in guest memory\n", host);
    std::abort();
}

}  // namespace guest
