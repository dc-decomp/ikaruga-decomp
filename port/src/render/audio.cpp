#include "render/audio.h"

#include <SDL3/SDL.h>

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

void Audio::push(int16_t left, int16_t right) {
    ++pushed;
    if (!stream_)
        return;
    block_.push_back(left);
    block_.push_back(right);
    if (block_.size() < kBlock * 2)
        return;
    const int queued = SDL_GetAudioStreamQueued(stream_) / 4;
    if (queued >= 0 && static_cast<unsigned>(queued) + kBlock > limit)
        dropped += kBlock;
    else
        SDL_PutAudioStreamData(stream_, block_.data(), static_cast<int>(block_.size() * sizeof(int16_t)));
    block_.clear();
}

}  // namespace render
