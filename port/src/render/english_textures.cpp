#include "render/english_textures.h"

#include <CoreGraphics/CoreGraphics.h>
#include <CoreText/CoreText.h>

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <string>

namespace render {

namespace {

struct Rect {
    uint32_t x, y, w, h;
};

// A phrase: the English, which goes in one line across the pieces that the game draws side by side,
// cut where the game cuts them. `size` is the font's size, which shrinks until the line fits.
struct Phrase {
    std::initializer_list<Rect> pieces;
    const char* text;
    float size;
    bool glow = false;
};

// Words of a texture that move by `dx` texels, as the stylized ones whose Japanese gloss goes.
struct Move {
    Rect from;
    int dx;
};

// Lines that the game draws as one piece: from the block's top, `pitch` texels apart, flush left.
struct Block {
    Rect rect;
    float size, pitch;
    std::initializer_list<const char*> lines;
};

struct Patch {
    uint64_t hash;
    std::initializer_list<Rect> clear;  // Japanese that goes without English in its place
    std::initializer_list<Move> moves;
    std::initializer_list<Phrase> phrases;
    std::initializer_list<Block> blocks = {};
};

// The texels of each phrase come from the display lists (IKARUGA_DUMP_TA, RENDER_VIEW_VERTICES=1
// render-view --list), the textures from IKARUGA_DUMP_TEXTURES.
const Patch kPatches[] = {
    // The selection's subtitle and its instructions: each line in three pieces of the rows.
    {0x29cd3aabcd8027c7ull,
     {},
     {},
     {{{{0, 0, 128, 24}, {0, 24, 120, 24}, {0, 48, 64, 24}}, "Please choose a game mode", 19},
      {{{0, 72, 128, 16}, {0, 88, 112, 16}, {0, 104, 112, 16}}, "Choose with the stick, confirm with a button", 13}}},
    // The boxes of the difficulties and of the two games. The difficulties' names keep their
    // stylized English, without the Japanese gloss after it, and move to the middle again.
    {0xe625d39750df1652ull,
     {{168, 0, 56, 17}, {200, 24, 40, 17}, {168, 48, 56, 17}},
     {{{8, 0, 146, 17}, 35}, {{8, 24, 178, 17}, 27}, {{8, 48, 146, 17}, 35}},
     {{{{8, 168, 104, 16}}, "No return fire", 13},
      {{{8, 88, 240, 16}}, "Return fire after kills of your color", 13},
      {{{8, 72, 240, 16}}, "Return fire after every kill", 13},
      {{{8, 128, 152, 24}}, "Normal game", 19},
      {{{8, 184, 64, 24}}, "5 chapters", 19},
      {{{8, 104, 232, 24}}, "Trial game   2 chapters", 19},
      {{{8, 152, 176, 16}}, "Chapter 1 with unlimited ships", 13}}},
    // The chapters' cards: the text below the title, the heading and the names staying.
    {0xbe95e3f6b336b8f2ull,
     {},
     {},
     {},
     {{{8, 48, 168, 64},
       10.5f,
       13,
       {"Ah, the Ikaruga goes forth......", "What moves those unwanted,", "cast out from the floating world,",
        "is nothing but the pride of", "those who hold the will to live."}}}},
    {0x4f246407637a6c72ull,
     {},
     {},
     {},
     {{{8, 48, 168, 80},
       10.5f,
       13,
       {"The firmer one's will, the more", "the trials that torment it.", "One can, of course, avoid a trial",
        "in one's way, or flee from it.", "But a trial's true meaning lies", "in overcoming that very heart."}}}},
    {0x904852e7aad657d9ull,
     {},
     {},
     {},
     {{{8, 48, 168, 80},
       10.5f,
       13,
       {"Nothing in this floating world is", "absolute, and with unjust thoughts", "at heart, one can be at a loss.",
        "To overcome this takes firm", "faith and insight, and some", "measure of the will to act."}}}},
    {0xa0cb1f85ff4386dbull,
     {},
     {},
     {},
     {{{8, 48, 168, 80},
       10.5f,
       13,
       {"And then reality shows itself.", "What did you seek..........", "What did you see..........",
        "What did you hear..........", "What did you think..........", "What did you do........"}}}},
    {0xeac4276dd50ed91bull,
     {},
     {},
     {},
     {{{8, 48, 168, 80},
       10.5f,
       13,
       {"In time, one chain of fate will", "bring that will back to where it", "began, and recall the origin",
        "carved deep into memory.", "", "Thus, the Ikaruga goes forth......"}}}},
    // The motto at the start: the first line in two pieces.
    {0xf82e49d851edd88aull,
     {},
     {},
     {{{{8, 8, 184, 24}, {8, 32, 208, 24}}, "I will not die unlived; my ideals unmet, I will not yield.", 18, true},
      {{{8, 56, 200, 24}}, "Thus I will not die with regret.", 18, true}}},
    // The final chapter's speech, a line or two at a time, at one size for all its lines. The first
    // three lines did not show in an autoplay game: their pieces are as wide as their letters,
    // rounded up to 8 texels, as the others are.
    {0x8d7e956e625605e3ull,
     {},
     {},
     {{{{0, 8, 232, 24}}, "It is I who keeps you alive,", 13.5f, true},
      {{{0, 32, 200, 24}}, "to walk the righteous path...", 13.5f, true},
      {{{0, 56, 224, 24}, {0, 80, 72, 24}}, "And yet you cannot understand this?", 13.5f, true},
      {{{0, 104, 200, 24}}, "Blind to what you can see...", 13.5f, true},
      {{{0, 128, 224, 24}, {72, 80, 56, 24}}, "do you think you can feel what is unseen?", 13.5f, true},
      {{{0, 152, 216, 24}}, "Surely you can see it too?", 13.5f, true},
      {{{0, 176, 224, 24}, {128, 80, 112, 24}}, "Never, for all eternity, can this cycle be broken...", 13.5f, true},
      {{{0, 200, 136, 24}, {0, 224, 184, 24}}, "Limiter released... The guns can no longer fire.", 13.5f, true}}},
    // The words before the last boss, and the ending's lines on white, one after the other.
    {0x5894849f15c9d2afull,
     {},
     {},
     {{{{0, 8, 208, 24}, {0, 32, 104, 24}}, "And so... see it through, with no regrets.", 14.5f, true},
      {{{0, 56, 184, 24}, {0, 80, 208, 24}}, "I will not die unlived; my ideals unmet, I will not yield.", 14.5f, true},
      {{{0, 104, 240, 24}}, "Thus I will not die with regret...", 14.5f, true},
      {{{0, 128, 232, 24}, {104, 32, 152, 24}}, "We should have known... Will we ever see freedom?", 14.5f, true},
      {{{0, 152, 176, 24}}, "Was this... the right thing?", 12.5f, true},
      {{{0, 176, 224, 24}, {0, 200, 88, 24}}, "It's all right... One day we'll understand each other.", 12.5f, true},
      {{{0, 224, 224, 24}, {88, 200, 80, 24}}, "Into the distant future... for life will be passed on.", 12.5f, true}}},
};

const Patch* find(uint64_t hash) {
    for (const Patch& p : kPatches)
        if (p.hash == hash)
            return &p;
    return nullptr;
}

CTFontRef font(float size) {
    return CTFontCreateWithName(CFSTR("HiraMinProN-W3"), size, nullptr);
}

// The line's coverage, `width` x `height` texels, the text centered or flush left, at the largest
// size up to `size` that fits.
std::vector<uint8_t> line(const char* utf8, float size, uint32_t width, uint32_t height, bool left = false) {
    std::vector<uint8_t> grey(static_cast<size_t>(width) * height);
    CFStringRef text = CFStringCreateWithCString(nullptr, utf8, kCFStringEncodingUTF8);
    if (!text)
        return grey;
    CGColorSpaceRef space = CGColorSpaceCreateDeviceGray();
    const CGFloat white[] = {1, 1};
    CGColorRef color = CGColorCreate(space, white);
    CTLineRef run = nullptr;
    CTFontRef f = nullptr;
    double w = 0;
    for (float s = size; s >= 6.0f; s -= 0.5f) {
        if (run)
            CFRelease(run);
        if (f)
            CFRelease(f);
        f = font(s);
        const void* keys[] = {kCTFontAttributeName, kCTForegroundColorAttributeName};
        const void* values[] = {f, color};
        CFDictionaryRef attributes = CFDictionaryCreate(nullptr, keys, values, 2, &kCFTypeDictionaryKeyCallBacks,
                                                        &kCFTypeDictionaryValueCallBacks);
        CFAttributedStringRef string = CFAttributedStringCreate(nullptr, text, attributes);
        run = CTLineCreateWithAttributedString(string);
        CFRelease(string);
        CFRelease(attributes);
        w = CTLineGetTypographicBounds(run, nullptr, nullptr, nullptr);
        if (w + 4 <= width)
            break;
    }
    CGContextRef context = CGBitmapContextCreate(grey.data(), width, height, 8, width, space, kCGImageAlphaNone);
    // The cap height in the middle of the line.
    const double baseline = (height - CTFontGetCapHeight(f)) / 2;
    CGContextSetTextPosition(context, left ? 1.0 : (width - w) / 2, baseline);
    CTLineDraw(run, context);
    CGContextRelease(context);
    CFRelease(run);
    CFRelease(f);
    CGColorRelease(color);
    CGColorSpaceRelease(space);
    CFRelease(text);
    return grey;
}

// A soft halo under the letters, as the motto's picture has.
void glow(std::vector<uint8_t>& grey, uint32_t width, uint32_t height) {
    std::vector<uint8_t> out(grey.size());
    constexpr int r = 2;
    for (uint32_t y = 0; y < height; ++y)
        for (uint32_t x = 0; x < width; ++x) {
            int sum = 0, n = 0;
            for (int dy = -r; dy <= r; ++dy)
                for (int dx = -r; dx <= r; ++dx) {
                    const int sx = static_cast<int>(x) + dx, sy = static_cast<int>(y) + dy;
                    if (sx >= 0 && sy >= 0 && sx < static_cast<int>(width) && sy < static_cast<int>(height)) {
                        sum += grey[sy * width + sx];
                        ++n;
                    }
                }
            const int halo = sum * 2 / (n * 3);
            out[y * width + x] = static_cast<uint8_t>(std::max<int>(grey[y * width + x], halo));
        }
    grey.swap(out);
}

void clear(const Rect& r, uint32_t width, uint32_t height, std::vector<uint32_t>& rgba) {
    for (uint32_t y = r.y; y < std::min(r.y + r.h, height); ++y)
        for (uint32_t x = r.x; x < std::min(r.x + r.w, width); ++x) rgba[y * width + x] = 0;
}

}  // namespace

bool has_english(uint64_t hash) { return find(hash) != nullptr; }

void write_english(uint64_t hash, uint32_t width, uint32_t height, std::vector<uint32_t>& rgba) {
    const Patch* p = find(hash);
    if (!p || rgba.size() < static_cast<size_t>(width) * height)
        return;
    for (const Rect& r : p->clear) clear(r, width, height, rgba);
    for (const Move& m : p->moves) {
        std::vector<uint32_t> words;
        for (uint32_t y = m.from.y; y < m.from.y + m.from.h; ++y)
            for (uint32_t x = m.from.x; x < m.from.x + m.from.w; ++x) words.push_back(rgba[y * width + x]);
        clear(m.from, width, height, rgba);
        size_t i = 0;
        for (uint32_t y = m.from.y; y < m.from.y + m.from.h; ++y)
            for (uint32_t x = m.from.x; x < m.from.x + m.from.w; ++x, ++i) {
                const uint32_t to = static_cast<uint32_t>(static_cast<int>(x) + m.dx);
                if (to < width)
                    rgba[y * width + to] = words[i];
            }
    }
    for (const Block& block : p->blocks) {
        clear(block.rect, width, height, rgba);
        uint32_t i = 0;
        for (const char* text : block.lines) {
            const auto top = static_cast<uint32_t>(block.rect.y + block.pitch * static_cast<float>(i++));
            const auto rows = static_cast<uint32_t>(block.pitch);
            const std::vector<uint8_t> grey = line(text, block.size, block.rect.w, rows, true);
            for (uint32_t y = 0; y < rows && top + y < std::min(block.rect.y + block.rect.h, height); ++y)
                for (uint32_t x = 0; x < block.rect.w; ++x)
                    if (const uint8_t a = grey[y * block.rect.w + x])
                        rgba[(top + y) * width + block.rect.x + x] = static_cast<uint32_t>(a) << 24 | 0xFFFFFFu;
        }
    }
    for (const Phrase& phrase : p->phrases) {
        uint32_t total = 0, height_px = 0;
        for (const Rect& r : phrase.pieces) {
            total += r.w;
            height_px = std::max(height_px, r.h);
        }
        std::vector<uint8_t> grey = line(phrase.text, phrase.size, total, height_px);
        if (phrase.glow)
            glow(grey, total, height_px);
        uint32_t offset = 0;
        for (const Rect& r : phrase.pieces) {
            clear(r, width, height, rgba);
            for (uint32_t y = 0; y < r.h && r.y + y < height; ++y)
                for (uint32_t x = 0; x < r.w && r.x + x < width; ++x) {
                    const uint8_t a = grey[y * total + offset + x];
                    if (a)
                        rgba[(r.y + y) * width + r.x + x] = static_cast<uint32_t>(a) << 24 | 0xFFFFFFu;
                }
            offset += r.w;
        }
    }
}

}  // namespace render
