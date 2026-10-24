// The game's frame clock: the console's video timing, as the PowerVR's sync pulse generator keeps
// it. A line takes SPG_LOAD's pixels at the pixel clock, and a frame SPG_LOAD's lines. On the lines
// of the vertical blank's start and end and of the horizontal blank's interrupt, the clock calls
// on_line with the event's vector in Shinobi's interrupt chains. It counts the frames at line 0 and
// answers the timing registers, whose SPG_STATUS gives the line. It wakes only on the lines where
// something happens: Ikaruga's frame takes five of them, not one for each of its 263 lines.
#pragma once

#include <cstdint>
#include <functional>

namespace dream::sched {
class Scheduler;
}

namespace hw {

class Video {
public:
    // The timing registers, from 0x005F80C8 to 0x005F80E0, and SPG_STATUS.
    static constexpr uint32_t kRegLo = 0x005F80C8u, kRegHi = 0x005F80E4u, kStatus = 0x005F810Cu;

    // The vectors of the vertical blank's start and end and of the horizontal blank.
    static constexpr uint32_t kVBlankIn = 0x930, kVBlankOut = 0x940, kHBlank = 0x950;

    explicit Video(dream::sched::Scheduler& sched);

    uint32_t read(uint32_t addr) const;
    void write(uint32_t addr, uint32_t value);
    // FB_R_CTRL's bit 23: the pixel clock at 27 MHz instead of 13.5 MHz.
    void set_full_clock(bool full);

    uint64_t frames() const { return frames_; }
    // The scheduler's event, which the clock re-arms itself.
    int event() const { return event_; }
    // Called with the vector of each event on its line.
    std::function<void(uint32_t vector)> on_line;
    // Called on the line where the vertical blank ends, before its event.
    std::function<void()> on_vblank_out;

private:
    uint32_t lines() const { return ((load_ >> 16) & 0x3FFu) + 1; }
    // The line now and the vertical sync's state then: the lines since the last one run.
    uint32_t line_now(bool* vsync) const;
    // The line's length changed: the next line starts a full line from now.
    void restart();
    // Runs the lines up to the one due, then wakes on the next line where something happens.
    void wake();
    void run_line();
    void arm();

    dream::sched::Scheduler& sched_;
    int event_ = -1;
    bool full_clock_ = false;
    uint64_t line_cycles_ = 0;
    // The last line run, its start on the scheduler's clock, the vertical sync then, and the field of
    // an interlaced frame.
    uint32_t line_ = 0;
    uint64_t line_start_ = 0;
    bool vsync_ = false, field_ = false;
    uint64_t frames_ = 0;
    uint32_t hblank_int_ = 0x031D0000u, vblank_int_ = 0x00150104u, control_ = 0, hblank_ = 0x007E0345u,
             load_ = 0x01060359u, vblank_ = 0x00150104u, width_ = 0x07F1933Fu;
};

}  // namespace hw
