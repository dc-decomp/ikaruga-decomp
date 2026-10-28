// Sound output: the AICA's samples to the host's default playback device, through SDL3.
//
// The AICA makes one stereo sample for each 44.1 kHz tick of the guest's clock, and the device
// plays them on the host's clock. The window paces the guest by the wall clock or by the display,
// whose pace set_speed() gives, and a queue absorbs a frame's jitter. The stream's rate follows the
// queue by up to 0.2%, so the queue neither runs dry nor reaches its limit. After a stall, the rate
// would take most of a minute to bring the queue back, and each slow frame meanwhile would run it
// dry or over again. So silence fills the queue to its target when the device ran dry, and past
// its limit the queue drops blocks until it is back at its target. When the guest runs ahead, as
// with --unthrottled, the dropped samples are counted rather than played late.
#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

struct SDL_AudioStream;

namespace render {

class Audio {
public:
    Audio() = default;
    Audio(const Audio&) = delete;
    Audio& operator=(const Audio&) = delete;
    ~Audio();

    // 16-bit stereo at `rate`. False with error() set when there is no device, which is no reason
    // to stop a run.
    bool open(unsigned rate = 44100);
    bool is_open() const { return stream_ != nullptr; }
    void push(int16_t left, int16_t right);
    // The guest's seconds for each second of the host.
    void set_speed(double speed) { speed_ = speed; }
    const std::string& error() const { return error_; }

    // The queue's limit in stereo samples: about a fifth of a second, enough for a slow frame and
    // short enough that the sound stays with the picture.
    unsigned limit = 8192;
    uint64_t pushed = 0, dropped = 0;
    uint64_t refills = 0;  // the times that silence filled the queue after the device ran dry
    // The queue's least and most stereo samples after the first second, and the last rate ratio.
    int least = -1, most = -1;
    float ratio() const { return ratio_; }

private:
    static constexpr size_t kBlock = 512;  // stereo samples sent at a time
    SDL_AudioStream* stream_ = nullptr;
    std::vector<int16_t> block_;
    double speed_ = 1;
    float ratio_ = 1;  // the stream's frequency ratio
    std::atomic<bool> ran_dry_{false};  // set on the device's thread
    bool shedding_ = false;             // dropping blocks until the queue is back at its target
    std::string error_;
};

}  // namespace render
