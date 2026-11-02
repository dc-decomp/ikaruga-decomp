// The game's lines of text: smooth glyphs where the port draws them, and ENGLISH TEXT (text.cpp).
#pragma once

#include <cstdint>

namespace text {

// The glyph of `code` for fontGlyphGet where the port draws it: a cell of the English line in
// progress, or Hiragino's without a boot ROM image (`rom` false). The copy of the glyph from guest
// address `at` that follows then blends its edges. False for a code that the ROM's font draws.
bool glyph(uint16_t code, bool rom, uint32_t at, uint8_t (&bits)[72]);

}  // namespace text
