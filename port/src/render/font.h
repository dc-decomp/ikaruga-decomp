// The boot ROM's 24-dot font from macOS's own Japanese font, for a run without a boot ROM image,
// and lines of English in the same cells (text.cpp). The port ships no font.
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace render {

// A glyph as the ROM holds one: 24 rows of 24 dots, the first dot in each byte's top bit, 72 bytes.
// `cover` says how much of each dot the character covers, 0 to 255, for edges that blend.
struct Glyph {
    std::array<uint8_t, 72> bits{};
    std::array<uint8_t, 24 * 24> cover{};
};

// The Shift-JIS character `code` (its lead byte high) in Hiragino Sans. False for a code that names
// no character, which leaves `out` empty.
bool glyph24(uint16_t code, Glyph& out);

// A line of text in Hiragino Sans, 24 dots high and centered in as many 24-dot cells as it needs.
std::vector<Glyph> line24(const std::string& utf8);

}  // namespace render
