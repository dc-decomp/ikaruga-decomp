// Sound output: the AICA's samples to the host's default playback device, through SDL3.
//
// The AICA makes one stereo sample for each 44.1 kHz tick of the guest's clock, and the device
// plays them on the host's clock. The window paces the guest by the wall clock, so the two agree to
// within a frame's jitter, and a queue absorbs that. When the guest runs ahead, as with
// --unthrottled, samples past the queue's limit are dropped and counted rather than played late.
#pragma once

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
    const std::string& error() const { return error_; }

    // The queue's limit in stereo samples: about a fifth of a second, enough for a slow frame and
    // short enough that the sound stays with the picture.
    unsigned limit = 8192;
    uint64_t pushed = 0, dropped = 0;

private:
    static constexpr size_t kBlock = 512;  // stereo samples sent at a time
    SDL_AudioStream* stream_ = nullptr;
    std::vector<int16_t> block_;
    std::string error_;
};

}  // namespace render
