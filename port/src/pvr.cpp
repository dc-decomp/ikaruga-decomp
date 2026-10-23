#include "pvr.h"

#include <cstdio>
#include <cstring>

#include "dream/runtime/holly/intc.h"
#include "dream/runtime/pvr/spg.h"

namespace hw {
namespace {

// Offsets from kRegBase.
constexpr uint32_t kId = 0x000, kRevision = 0x004, kSoftReset = 0x008, kStartRender = 0x014, kFbRCtrl = 0x044,
                   kFbRSof1 = 0x050, kFbRSof2 = 0x054, kFbWSof1 = 0x060, kFbWSof2 = 0x064, kTaIspBase = 0x128,
                   kTaNextOpb = 0x134, kTaItpCurrent = 0x138, kTaListInit = 0x144, kTaYuvTexCnt = 0x150,
                   kTaListCont = 0x160, kTaNextOpbInit = 0x164;

bool timing(uint32_t addr) {
    return (addr >= dream::pvr::Spg::kRegLo && addr < dream::pvr::Spg::kRegHi) || addr == dream::pvr::Spg::kStatus;
}

}  // namespace

Pvr::Pvr(dream::pvr::Spg& spg, dream::holly::Intc& holly, uint8_t* vram, uint32_t vram_size)
    : spg_(spg), holly_(holly), vram_(vram), vram_mask_(vram_size - 1) {
    // What the chip reports, which Kamui checks when it starts.
    regs_[kId >> 2] = 0x17FD11DBu;
    regs_[kRevision >> 2] = 0x00000011u;
    regs_[kSoftReset >> 2] = 0x00000007u;
}

uint32_t Pvr::read(uint32_t addr, unsigned) {
    if (addr < kRegBase || addr >= kRegEnd)
        return 0;  // the TA's input cannot be read
    if (timing(addr))
        return spg_.read(addr, 4);
    return regs_[(addr - kRegBase) >> 2];
}

void Pvr::write(uint32_t addr, uint32_t value, unsigned size) {
    if (addr >= kTaBase && addr < kYuvBase) {
        chunk_[(addr >> 2) & 7] = value;
        if (((addr >> 2) & 7) == 7)
            feed(chunk_);
        return;
    }
    if (addr >= kYuvBase && addr < kTexBase) {
        if (!reported_yuv_)
            std::fprintf(stderr, "pvr: the game sends data to the YUV converter, which the port does not have\n");
        reported_yuv_ = true;
        return;
    }
    if (addr >= kTexBase && addr < kTaEnd) {  // the 64-bit view of texture memory
        std::memcpy(vram_ + (addr & vram_mask_ & ~3u), &value, 4);
        return;
    }
    if (addr < kRegBase || addr >= kRegEnd)
        return;
    if (timing(addr)) {
        spg_.write(addr, value, size);
        return;
    }
    const uint32_t off = addr - kRegBase;
    uint32_t& r = regs_[off >> 2];
    switch (off) {
        case kId:
        case kRevision:
        case kTaYuvTexCnt:
            return;
        case kStartRender:
            start_render();
            return;
        case kTaListInit:
            if (value >> 31) {
                list_.clear();
                regs_[kTaNextOpb >> 2] = regs_[kTaNextOpbInit >> 2];
                regs_[kTaItpCurrent >> 2] = regs_[kTaIspBase >> 2];
            }
            return;
        case kTaListCont:
            list_.clear();
            return;
        case kSoftReset:
            r = value;
            if (value & 1)
                list_.clear();
            return;
        case kFbRCtrl:
            if (((r ^ value) >> 23) & 1)
                spg_.set_vclk_div((value >> 23) & 1);
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
    if (addr >= kTaBase && addr < kYuvBase) {
        feed(words);
        return;
    }
    for (uint32_t i = 0; i < 8; ++i) write(addr + 4 * i, words[i], 4);
}

void Pvr::feed(const uint32_t* words) {
    list_.insert(list_.end(), words, words + 8);
    regs_[kTaItpCurrent >> 2] += 32;
}

// The render is drawn now, and its end goes to the game as the interrupts of the ISP, the TSP and
// the video output, which the game takes when SR lets it.
void Pvr::start_render() {
    ++renders;
    if (on_render)
        on_render(list_);
    holly_.raise(dream::holly::Irq::RenderDone);
    holly_.raise(dream::holly::Irq::RenderDoneIsp);
    holly_.raise(dream::holly::Irq::RenderDoneVideo);
}

}  // namespace hw
