// The stb_truetype side of the cross-platform text: the 24-dot game font, the overlay (stb_text.cpp)
// and the English texture painter (english_textures.cpp) share one system font.
#pragma once

#include <cstdint>
#include <vector>

namespace render::stbtext {

// The line's coverage, `width` x `height` texels, centered or flush left, shrunk from `size` until
// it fits. `bold` draws a second pass a fraction of a texel to the right.
bool line(const char* utf8, float size, uint32_t width, uint32_t height, bool left, bool bold,
          std::vector<uint8_t>& out);

// The line's width at `size`.
double measure(const char* utf8, float size, bool bold);

}  // namespace render::stbtext
