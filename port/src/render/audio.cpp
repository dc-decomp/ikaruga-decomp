#include "render/audio.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace render {

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
    stream_ = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
    if (!stream_) {
        error_ = std::string("no playback device: ") + SDL_GetError();
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        return false;
    }
    SDL_ResumeAudioStreamDevice(stream_);
    block_.reserve(kBlock * 2);
    return true;
}

void Limiter::process(int32_t& left, int32_t& right) {
    constexpr float kCeiling = 32000.0f;
    // 20 dB in 150 ms, multiplied up to the gain needed, which it then holds exactly.
    static const float release = std::pow(10.0f, 20.0f / 20.0f / (0.15f * 44100.0f));
    const float peak = static_cast<float>(std::max(std::abs(left), std::abs(right)));
    // The sample that leaves now came in 64 samples ago, and its need still counts.
    const int32_t out_left = delayed_[2 * next_], out_right = delayed_[2 * next_ + 1];
    float target = needs_[next_] > 0.0f ? needs_[next_] : 1.0f;
    lowered_ -= needs_[next_] > 0.0f && needs_[next_] < 1.0f;
    delayed_[2 * next_] = left;
    delayed_[2 * next_ + 1] = right;
    needs_[next_] = peak > kCeiling ? kCeiling / peak : 1.0f;
    lowered_ += needs_[next_] < 1.0f;
    next_ = (next_ + 1) % kAhead;
    // Most of the time no sample in the window needs less than the full gain.
    if (lowered_ > 0)
        for (float need : needs_)
            if (need > 0.0f)
                target = std::min(target, need);
    gain_ = target < gain_ ? gain_ + (target - gain_) * 0.1f : std::min(target, gain_ * release);
    left = static_cast<int32_t>(std::lround(static_cast<float>(out_left) * gain_));
    right = static_cast<int32_t>(std::lround(static_cast<float>(out_right) * gain_));
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
    if (queued >= 0 && static_cast<unsigned>(queued) + kBlock > limit)
        dropped += kBlock;
    else
        SDL_PutAudioStreamData(stream_, block_.data(), static_cast<int>(block_.size() * sizeof(int16_t)));
    block_.clear();
}

}  // namespace render
