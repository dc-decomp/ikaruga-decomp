// The PowerVR's registers and the path into texture memory. The registers are memory that the
// renderer reads. The display list does not come through here: Ninja and Kamui build it on the host
// (frame.h), and Kamui's scenes hand it to draw(). The timing registers are the frame clock's
// (video.h), and FB_R_CTRL's pixel clock goes to it.
#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <vector>

#include "dream/runtime/mem/dc_memory.h"
#include "video.h"


namespace hw {

class Pvr final : public dream::mem::MmioHandler {
public:
    // The registers, with the fog table at 0x200 and the palette at 0x1000.
    static constexpr uint32_t kRegBase = 0x005F8000u, kRegEnd = 0x005FA000u;
    // The tile accelerator's input, the YUV converter's, and the path into texture memory.
    static constexpr uint32_t kTaBase = 0x10000000u, kYuvBase = 0x10800000u, kTexBase = 0x11000000u,
                              kTaEnd = 0x12000000u;

    Pvr(Video& video, uint8_t* vram, uint32_t vram_size);

    uint32_t read(uint32_t addr, unsigned size) override;
    void write(uint32_t addr, uint32_t value, unsigned size) override;
    void write_burst(uint32_t addr, const uint32_t* words) override;

    const uint32_t* registers() const { return regs_.data(); }
    uint32_t reg(uint32_t offset) const { return regs_[offset >> 2]; }

    // Called with the display list of each render.
    std::function<void(const std::vector<uint32_t>& list)> on_render;
    void draw(const std::vector<uint32_t>& list);
    uint64_t renders = 0;

private:
    void report(uint32_t bit, const char* what);

    std::array<uint32_t, (kRegEnd - kRegBase) / 4> regs_{};
    Video& video_;
    uint8_t* vram_;
    uint32_t vram_mask_;
    uint32_t reported_ = 0;
};

}  // namespace hw
