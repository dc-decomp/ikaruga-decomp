#include "render/font.h"

#include <CoreGraphics/CoreGraphics.h>
#include <CoreText/CoreText.h>

#include <array>
#include <cstring>
#include <unordered_map>

namespace render {

namespace {

// Hiragino W3 at 22 points, smoothed and cut at kInk, keeps most strokes one or two dots wide, as
// the ROM's thinner ones are, where a stroke between two rows would take both.
constexpr CGFloat kSize = 22;
constexpr uint8_t kInk = 190;

CTFontRef base_font() {
    static CTFontRef font = CTFontCreateWithName(CFSTR("HiraginoSans-W3"), kSize, nullptr);
    return font;
}

bool draw(uint16_t code, uint8_t (&out)[72]) {
    std::memset(out, 0, sizeof out);
    const UInt8 bytes[2] = {static_cast<UInt8>(code >> 8), static_cast<UInt8>(code)};
    CFStringRef text = CFStringCreateWithBytes(nullptr, bytes, 2, kCFStringEncodingDOSJapanese, false);
    if (!text)
        return false;
    const CFIndex length = CFStringGetLength(text);
    UniChar chars[2] = {};
    bool ok = length >= 1 && length <= 2;
    if (ok)
        CFStringGetCharacters(text, CFRangeMake(0, length), chars);
    // A character that Hiragino lacks comes from the font that macOS falls back to.
    CTFontRef font = ok ? CTFontCreateForString(base_font(), text, CFRangeMake(0, length)) : nullptr;
    CFRelease(text);
    CGGlyph glyphs[2] = {};
    if (!font || !CTFontGetGlyphsForCharacters(font, chars, glyphs, length)) {
        if (font)
            CFRelease(font);
        return false;
    }
    uint8_t grey[24 * 24] = {};
    CGColorSpaceRef space = CGColorSpaceCreateDeviceGray();
    CGContextRef context = CGBitmapContextCreate(grey, 24, 24, 8, 24, space, kCGImageAlphaNone);
    CGColorSpaceRelease(space);
    CGContextSetGrayFillColor(context, 1, 1);
    // The glyph in the middle of its cell, the font's ascent from the top as in the ROM.
    CGSize advance;
    CTFontGetAdvancesForGlyphs(font, kCTFontOrientationHorizontal, glyphs, &advance, 1);
    const CGPoint at = CGPointMake((24 - advance.width) / 2, 24 - CTFontGetAscent(font));
    CTFontDrawGlyphs(font, glyphs, &at, 1, context);
    CGContextRelease(context);
    CFRelease(font);
    // The bitmap's first row is the top one, as in the ROM.
    for (int y = 0; y < 24; ++y)
        for (int x = 0; x < 24; ++x)
            if (grey[24 * y + x] >= kInk)
                out[3 * y + x / 8] |= static_cast<uint8_t>(0x80 >> (x % 8));
    return true;
}

}  // namespace

bool glyph24(uint16_t code, uint8_t (&out)[72]) {
    static std::unordered_map<uint16_t, std::pair<bool, std::array<uint8_t, 72>>> drawn;
    auto it = drawn.find(code);
    if (it == drawn.end()) {
        std::pair<bool, std::array<uint8_t, 72>> g;
        uint8_t bits[72];
        g.first = draw(code, bits);
        std::memcpy(g.second.data(), bits, sizeof bits);
        it = drawn.emplace(code, g).first;
    }
    std::memcpy(out, it->second.second.data(), sizeof out);
    return it->second.first;
}

}  // namespace render
