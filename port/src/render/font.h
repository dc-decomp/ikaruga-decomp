// The boot ROM's 24-dot font from macOS's own Japanese font, for a run without a boot ROM image
// (bios.cpp). The port ships no font.
#pragma once

#include <cstdint>

namespace render {

// The Shift-JIS character `code` (its lead byte high) in Hiragino Sans, as the ROM holds a glyph:
// 24 rows of 24 dots, the first dot in each byte's top bit, 72 bytes. False for a code that names
// no character, which leaves `out` empty.
bool glyph24(uint16_t code, uint8_t (&out)[72]);

}  // namespace render
