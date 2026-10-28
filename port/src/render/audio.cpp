#include "render/audio.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace render {

namespace {

// A request that the queue cannot fill to half: the device ran dry and plays silence. The
// resampler's rounding asks for a frame more at any time.
void SDLCALL on_request(void* ran_dry, SDL_AudioStream*, int additional, int total) {
    if (total > 0 && additional > total / 2)
        static_cast<std::atomic<bool>*>(ran_dry)->store(true, std::memory_order_relaxed);
}

}  // namespace

Audio::~Audio() {
    if (!stream_)
        return;
    SDL_DestroyAudioStream(stream_);
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
}

bool Audio::open(unsigned rate) {
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        error_ = std::string("SDL audio: ") + SDL_GetError();
        return false;
    }
    const SDL_AudioSpec spec{SDL_AUDIO_S16LE, 2, static_cast<int>(rate)};
    stream_ = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, on_request, &ran_dry_);
    if (!stream_) {
        error_ = std::string("no playback device: ") + SDL_GetError();
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        return false;
    }
    SDL_ResumeAudioStreamDevice(stream_);
    block_.reserve(kBlock * 2);
    return true;
}

void Audio::push(int16_t left, int16_t right) {
    ++pushed;
    if (!stream_)
        return;
    block_.push_back(left);
    block_.push_back(right);
    if (block_.size() < kBlock * 2)
        return;
    const int queued = SDL_GetAudioStreamQueued(stream_) / 4;
    if (pushed > 44100) {
        least = least < 0 ? queued : std::min(least, queued);
        most = std::max(most, queued);
    }
    // Faster while the queue holds more than a quarter of the limit, slower while it holds less.
    const double target = limit / 4.0;
    const auto ratio = static_cast<float>(speed_ * (1 + 0.002 * std::clamp((queued - target) / target, -1.0, 1.0)));
    if (std::abs(ratio - ratio_) > 1e-5f && SDL_SetAudioStreamFrequencyRatio(stream_, ratio))
        ratio_ = ratio;
    if (ran_dry_.exchange(false, std::memory_order_relaxed) && queued >= 0 && queued + kBlock < target) {
        const std::vector<int16_t> silence(2 * (static_cast<size_t>(target) - static_cast<size_t>(queued) - kBlock), 0);
        SDL_PutAudioStreamData(stream_, silence.data(), static_cast<int>(silence.size() * sizeof(int16_t)));
        ++refills;
    }
    if (queued >= 0 && static_cast<unsigned>(queued) + kBlock > limit)
        shedding_ = true;
    else if (queued <= target)
        shedding_ = false;
    if (shedding_)
        dropped += kBlock;
    else
        SDL_PutAudioStreamData(stream_, block_.data(), static_cast<int>(block_.size() * sizeof(int16_t)));
    block_.clear();
}

}  // namespace render
