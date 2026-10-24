#include "video.h"

#include "dream/runtime/sched/scheduler.h"

namespace hw {
namespace {

constexpr uint64_t kPixelClock = 27'000'000;

}  // namespace

Video::Video(dream::sched::Scheduler& sched) : sched_(sched) {
    event_ = sched_.add("video", [this](uint64_t, uint64_t) { wake(); });
    restart();
}

uint32_t Video::line_now(bool* vsync) const {
    uint32_t line = line_;
    bool sync = vsync_;
    for (uint64_t t = line_start_ + line_cycles_; t <= sched_.now(); t += line_cycles_) {
        line = (line + 1) % lines();
        if (line == (vblank_ & 0x3FFu))
            sync = true;
        if (line == ((vblank_ >> 16) & 0x3FFu))
            sync = false;
    }
    if (vsync)
        *vsync = sync;
    return line;
}

void Video::restart() {
    while (line_start_ + line_cycles_ <= sched_.now() && line_cycles_) {
        line_start_ += line_cycles_;
        run_line();
    }
    const uint64_t clock = full_clock_ ? kPixelClock : kPixelClock / 2;
    line_cycles_ = dream::sched::kSh4Clock * ((load_ & 0x3FFu) + 1) / clock;
    if (control_ & 0x10u)  // interlaced
        line_cycles_ /= 2;
    if (!line_cycles_)
        line_cycles_ = 1;
    // The line in progress starts again, as the generator's counter does.
    line_start_ = sched_.now();
    arm();
}

void Video::wake() {
    while (line_start_ + line_cycles_ <= sched_.now()) {
        line_start_ += line_cycles_;
        run_line();
    }
    arm();
}

void Video::run_line() {
    line_ = (line_ + 1) % lines();
    const auto event = [this](uint32_t vector) {
        if (on_line)
            on_line(vector);
    };
    if (line_ == (vblank_int_ & 0x3FFu))
        event(kVBlankIn);
    if (line_ == ((vblank_int_ >> 16) & 0x3FFu)) {
        if (on_vblank_out)
            on_vblank_out();
        event(kVBlankOut);
    }
    if (line_ == (vblank_ & 0x3FFu))
        vsync_ = true;
    if (line_ == ((vblank_ >> 16) & 0x3FFu))
        vsync_ = false;
    const uint32_t hblank_mode = (hblank_int_ >> 12) & 3u;
    if ((hblank_mode == 0 && line_ == ((hblank_int_ >> 16) & 0x3FFu)) || hblank_mode == 2)
        event(kHBlank);
    if (line_ == 0) {
        field_ = (control_ & 0x10u) ? !field_ : false;
        ++frames_;
    }
}

// Wakes on the next line where an interrupt comes, the frame count changes or the vertical sync
// changes.
void Video::arm() {
    const uint32_t hblank_mode = (hblank_int_ >> 12) & 3u;
    uint32_t k = 1;
    for (; k < lines() && hblank_mode != 2; ++k) {
        const uint32_t line = (line_ + k) % lines();
        if (line == 0 || line == (vblank_int_ & 0x3FFu) || line == ((vblank_int_ >> 16) & 0x3FFu) ||
            line == (vblank_ & 0x3FFu) || line == ((vblank_ >> 16) & 0x3FFu) ||
            (hblank_mode == 0 && line == ((hblank_int_ >> 16) & 0x3FFu)))
            break;
    }
    sched_.request(event_, line_start_ + k * line_cycles_ - sched_.now());
}

uint32_t Video::read(uint32_t addr) const {
    if (addr == kStatus) {
        // The line in bits 0 to 9, the field in 10, the blank in 11 and the vertical sync in 13.
        bool vsync = false;
        const uint32_t line = line_now(&vsync);
        const bool blank = vsync || line >= (vblank_ & 0x3FFu);
        return line | (field_ ? 1u << 10 : 0) | (blank ? 1u << 11 : 0) | (vsync ? 1u << 13 : 0);
    }
    switch (addr - kRegLo) {
        case 0x00: return hblank_int_;
        case 0x04: return vblank_int_;
        case 0x08: return control_;
        case 0x0C: return hblank_;
        case 0x10: return load_;
        case 0x14: return vblank_;
        case 0x18: return width_;
        default: return 0;
    }
}

void Video::write(uint32_t addr, uint32_t value) {
    // The lines before the write ran with the old settings.
    while (line_start_ + line_cycles_ <= sched_.now()) {
        line_start_ += line_cycles_;
        run_line();
    }
    switch (addr - kRegLo) {
        case 0x00: hblank_int_ = value; break;
        case 0x04: vblank_int_ = value; break;
        case 0x08: control_ = value; restart(); return;
        case 0x0C: hblank_ = value; break;
        case 0x10: load_ = value; restart(); return;
        case 0x14: vblank_ = value; break;
        case 0x18: width_ = value; break;
        default: return;
    }
    arm();
}

void Video::set_full_clock(bool full) {
    full_clock_ = full;
    restart();
}

}  // namespace hw
