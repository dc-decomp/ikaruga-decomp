// The PowerVR as Kamui's decompiled code and Ninja's machine code reach it. Its registers are memory
// that the renderer reads, and what the game sends to the tile accelerator is the display list that
// the renderer draws when Kamui starts a render. Nothing takes time: a render is drawn when it
// starts and reports its end at once, and a transfer is complete when it returns. The video timing
// is still dream-recomp's (dream::pvr::Spg), which the timing registers go to.
#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <vector>

#include "dream/runtime/mem/dc_memory.h"

namespace dream::pvr {
class Spg;
}
namespace dream::holly {
class Intc;
}

namespace hw {

class Pvr final : public dream::mem::MmioHandler {
public:
    // The registers, with the fog table at 0x200 and the palette at 0x1000.
    static constexpr uint32_t kRegBase = 0x005F8000u, kRegEnd = 0x005FA000u;
    // The tile accelerator's input, the YUV converter's, and the path into texture memory.
    static constexpr uint32_t kTaBase = 0x10000000u, kYuvBase = 0x10800000u, kTexBase = 0x11000000u,
                              kTaEnd = 0x12000000u;

    Pvr(dream::pvr::Spg& spg, dream::holly::Intc& holly, uint8_t* vram, uint32_t vram_size);

    uint32_t read(uint32_t addr, unsigned size) override;
    void write(uint32_t addr, uint32_t value, unsigned size) override;
    void write_burst(uint32_t addr, const uint32_t* words) override;

    const uint32_t* registers() const { return regs_.data(); }
    uint32_t reg(uint32_t offset) const { return regs_[offset >> 2]; }

    // Called when a render starts, with the display list it draws.
    std::function<void(const std::vector<uint32_t>& list)> on_render;
    uint64_t renders = 0;

private:
    void feed(const uint32_t* words);
    void start_render();

    std::array<uint32_t, (kRegEnd - kRegBase) / 4> regs_{};
    std::vector<uint32_t> list_;  // since the last TA_LIST_INIT
    uint32_t chunk_[8] = {};      // single words into the TA, until 32 bytes are in
    dream::pvr::Spg& spg_;
    dream::holly::Intc& holly_;
    uint8_t* vram_;
    uint32_t vram_mask_;
    bool reported_yuv_ = false;
};

}  // namespace hw
