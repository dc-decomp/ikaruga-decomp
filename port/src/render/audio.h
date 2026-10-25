// Sound output: the AICA's samples to the host's default playback device, through SDL3.
//
// The AICA makes one stereo sample for each 44.1 kHz tick of the guest's clock, and the device
// plays them on the host's clock. The window paces the guest by the wall clock or by the display,
// whose pace set_speed() gives, and a queue absorbs a frame's jitter. The stream's rate follows the
// queue by up to 0.2%, so the queue neither runs dry nor reaches its limit. When the guest runs
// ahead, as with --unthrottled, samples past the queue's limit are dropped and counted rather than
// played late.
#pragma once

#include <array>
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
    // The queue's least and most stereo samples after the first second, and the last rate ratio.
    int least = -1, most = -1;
    float ratio() const { return ratio_; }

private:
    static constexpr size_t kBlock = 512;  // stereo samples sent at a time
    SDL_AudioStream* stream_ = nullptr;
    std::vector<int16_t> block_;
    double speed_ = 1;
    float ratio_ = 1;  // the stream's frequency ratio
    std::string error_;
};

// The peaks of the AICA's mix above the 16-bit range, brought under it instead of clipped. The gain
// falls in the 64 samples before a peak, so that the peak stays under the range, and comes back
// over about 150 ms. A mix below the range passes as it is, 64 samples (1.5 ms) later.
class Limiter {
public:
    void process(int32_t& left, int32_t& right);

private:
    static constexpr int kAhead = 64;
    std::array<int32_t, kAhead * 2> delayed_{};
    std::array<float, kAhead> needs_{};  // the gain that each delayed sample needs
    int next_ = 0, lowered_ = 0;        // lowered_: the needs below 1
    float gain_ = 1.0f;
};

}  // namespace render
