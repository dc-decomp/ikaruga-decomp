// IKARUGA_PERF=1: a line on stderr for every frame that took longer than the display's refresh to
// produce, with where the time went. The frame's work runs on one thread: the game's code, the
// translation of its display list, the textures' decoding and upload, and the drawing. The
// counters here are summed per frame by the parts that do that work and printed, then cleared, by
// the frame hook in main.cpp.
#pragma once

#include <chrono>
#include <cstdint>
#include <cstdlib>

namespace perf {

inline bool on() {
    static const bool enabled = std::getenv("IKARUGA_PERF") != nullptr;
    return enabled;
}

using Clock = std::chrono::steady_clock;

inline double ms_since(Clock::time_point from) {
    return std::chrono::duration<double, std::milli>(Clock::now() - from).count();
}

struct Frame {
    double render_ms = 0;    // translate and draw, all renders of the frame
    double decode_ms = 0;    // texture decode (CPU)
    double upload_ms = 0;    // texture uploads, including their waits for the GPU
    double hash_ms = 0;      // texture change detection
    double create_ms = 0;    // images and views made for textures the cache had not seen
    double pipeline_ms = 0;  // pipelines built the first time a draw needs them
    uint64_t pipelines = 0;
    uint64_t renders = 0;
    uint64_t decoded = 0;    // textures decoded this frame
    uint64_t uploads = 0;    // image uploads (one per mip level)
    uint64_t bytes = 0;      // uploaded bytes
};

inline Frame& frame() {
    static Frame f;
    return f;
}

}  // namespace perf
