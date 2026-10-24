#include "pvr.h"

#include <cstdio>
#include <cstring>


namespace hw {
namespace {

// Offsets from kRegBase.
constexpr uint32_t kId = 0x000, kRevision = 0x004, kSoftReset = 0x008, kStartRender = 0x014, kFbRCtrl = 0x044,
                   kFbRSof1 = 0x050, kFbRSof2 = 0x054, kFbWSof1 = 0x060, kFbWSof2 = 0x064, kTaYuvTexCnt = 0x150;

}  // namespace

Pvr::Pvr(Video& video, uint8_t* vram, uint32_t vram_size) : video_(video), vram_(vram), vram_mask_(vram_size - 1) {
    // What the chip reports, which Kamui checks when it starts.
    regs_[kId >> 2] = 0x17FD11DBu;
    regs_[kRevision >> 2] = 0x00000011u;
    regs_[kSoftReset >> 2] = 0x00000007u;
}

uint32_t Pvr::read(uint32_t addr, unsigned) {
    if (addr < kRegBase || addr >= kRegEnd)
        return 0;  // the TA's input cannot be read
    return regs_[(addr - kRegBase) >> 2];
}

void Pvr::write(uint32_t addr, uint32_t value, unsigned size) {
    if (addr >= kTaBase && addr < kYuvBase) {
        report(1, "the tile accelerator");
        return;
    }
    if (addr >= kYuvBase && addr < kTexBase) {
        report(2, "the YUV converter");
        return;
    }
    if (addr >= kTexBase && addr < kTaEnd) {  // the 64-bit view of texture memory
        std::memcpy(vram_ + (addr & vram_mask_ & ~3u), &value, 4);
        return;
    }
    if (addr < kRegBase || addr >= kRegEnd)
        return;
    const uint32_t off = addr - kRegBase;
    uint32_t& r = regs_[off >> 2];
    switch (off) {
        case kId:
        case kRevision:
        case kTaYuvTexCnt:
            return;
        case kStartRender:
            report(4, "the renderer's start");
            return;
        case kFbRCtrl:
            if (((r ^ value) >> 23) & 1)
                video_.set_full_clock((value >> 23) & 1);
            r = value;
            return;
        case kFbRSof1:
        case kFbRSof2:
            r = value & 0x00FFFFFCu;
            return;
        case kFbWSof1:
        case kFbWSof2:
            r = value & 0x01FFFFFCu;
            return;
        default:
            r = value;
    }
}

void Pvr::write_burst(uint32_t addr, const uint32_t* words) {
    for (uint32_t i = 0; i < 8; ++i) write(addr + 4 * i, words[i], 4);
}

void Pvr::draw(const std::vector<uint32_t>& list) {
    ++renders;
    if (on_render)
        on_render(list);
}

// What the port does not model, once each: the game's code no longer reaches it.
void Pvr::report(uint32_t bit, const char* what) {
    if (reported_ & bit)
        return;
    reported_ |= bit;
    std::fprintf(stderr, "pvr: the game reaches %s, which the port does not have\n", what);
}

}  // namespace hw
