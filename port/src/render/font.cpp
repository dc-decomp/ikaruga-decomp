#include "render/font.h"

#include <CoreGraphics/CoreGraphics.h>
#include <CoreText/CoreText.h>

#include <algorithm>
#include <cmath>
#include <unordered_map>

namespace render {

namespace {

// Hiragino W3 at 22 points, cut at kInk for the dots, keeps most strokes one or two dots wide, as
// the ROM's thinner ones are, where a stroke between two rows would take both.
constexpr CGFloat kSize = 22;
constexpr uint8_t kInk = 190;

CTFontRef base_font() {
    static CTFontRef font = CTFontCreateWithName(CFSTR("HiraginoSans-W3"), kSize, nullptr);
    return font;
}

// The dots of a glyph from its coverage. The bitmap's first row is the top one, as in the ROM.
void cut(Glyph& g) {
    g.bits.fill(0);
    for (int y = 0; y < 24; ++y)
        for (int x = 0; x < 24; ++x)
            if (g.cover[24 * y + x] >= kInk)
                g.bits[3 * y + x / 8] |= static_cast<uint8_t>(0x80 >> (x % 8));
}

bool draw(uint16_t code, Glyph& out) {
    out = Glyph{};
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
    CGColorSpaceRef space = CGColorSpaceCreateDeviceGray();
    CGContextRef context = CGBitmapContextCreate(out.cover.data(), 24, 24, 8, 24, space, kCGImageAlphaNone);
    CGColorSpaceRelease(space);
    CGContextSetGrayFillColor(context, 1, 1);
    // The glyph in the middle of its cell, the font's ascent from the top as in the ROM.
    CGSize advance;
    CTFontGetAdvancesForGlyphs(font, kCTFontOrientationHorizontal, glyphs, &advance, 1);
    const CGPoint at = CGPointMake((24 - advance.width) / 2, 24 - CTFontGetAscent(font));
    CTFontDrawGlyphs(font, glyphs, &at, 1, context);
    CGContextRelease(context);
    CFRelease(font);
    cut(out);
    return true;
}

CTFontRef latin_font() {
    static CTFontRef font = CTFontCreateWithName(CFSTR("HiraginoSans-W3"), 20, nullptr);
    return font;
}

}  // namespace

std::vector<Glyph> line24(const std::string& utf8) {
    std::vector<Glyph> cells;
    CFStringRef text = CFStringCreateWithCString(nullptr, utf8.c_str(), kCFStringEncodingUTF8);
    if (!text)
        return cells;
    CGColorSpaceRef space = CGColorSpaceCreateDeviceGray();
    const CGFloat white[] = {1, 1};
    CGColorRef color = CGColorCreate(space, white);
    const void* keys[] = {kCTFontAttributeName, kCTForegroundColorAttributeName};
    const void* values[] = {latin_font(), color};
    CFDictionaryRef attributes = CFDictionaryCreate(nullptr, keys, values, 2, &kCFTypeDictionaryKeyCallBacks,
                                                    &kCFTypeDictionaryValueCallBacks);
    CFAttributedStringRef string = CFAttributedStringCreate(nullptr, text, attributes);
    CTLineRef line = CTLineCreateWithAttributedString(string);
    CFRelease(string);
    CFRelease(attributes);
    CGColorRelease(color);
    CFRelease(text);
    const double width = CTLineGetTypographicBounds(line, nullptr, nullptr, nullptr);
    const size_t count = std::max<size_t>(1, static_cast<size_t>(std::ceil((width + 2) / 24)));
    const size_t w = 24 * count;
    std::vector<uint8_t> grey(w * 24);
    CGContextRef context = CGBitmapContextCreate(grey.data(), w, 24, 8, w, space, kCGImageAlphaNone);
    CGColorSpaceRelease(space);
    // The font's ascent from the top, as the glyphs of the ROM's font have it.
    CGContextSetTextPosition(context, (static_cast<double>(w) - width) / 2, 24 - CTFontGetAscent(latin_font()));
    CTLineDraw(line, context);
    CGContextRelease(context);
    CFRelease(line);
    cells.resize(count);
    for (size_t c = 0; c < count; ++c) {
        for (size_t y = 0; y < 24; ++y)
            for (size_t x = 0; x < 24; ++x) cells[c].cover[24 * y + x] = grey[w * y + 24 * c + x];
        cut(cells[c]);
    }
    return cells;
}

bool glyph24(uint16_t code, Glyph& out) {
    static std::unordered_map<uint16_t, std::pair<bool, Glyph>> drawn;
    auto it = drawn.find(code);
    if (it == drawn.end()) {
        Glyph g;
        const bool ok = draw(code, g);
        it = drawn.emplace(code, std::make_pair(ok, g)).first;
    }
    out = it->second.second;
    return it->second.first;
}

}  // namespace render
