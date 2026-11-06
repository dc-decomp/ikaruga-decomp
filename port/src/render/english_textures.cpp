#include "render/english_textures.h"

#ifdef __APPLE__
#include <CoreGraphics/CoreGraphics.h>
#include <CoreText/CoreText.h>
#else
#include "render/stb_text.h"
#endif

#include <algorithm>
#include <cmath>
#include <cstring>
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

// A paragraph that the game draws as a block of lines and a piece to its right, which goes on with
// the block's first lines: `right` shows at (dx, dy) from the block's top left, over the block's last
// texels there. The English wraps over both at the largest size up to `size` that fits.
struct Paragraph {
    Rect block, right;
    int dx, dy;
    const char* text;
    float size;
};

// Lines that the game holds in a picture over its art, as on the gallery's pages of the characters.
// The art has no green: what has green in `rect` are the letters, which go with their edges, and the
// art around them fills their place. The English goes there in white with a dark edge, wrapped to
// `rect`, its paragraphs (split by '\n') an empty line apart, at the largest size up to `size` that
// fits. Its lines lie `pitch` texels apart, as the Japanese did, so that they cover where the art
// is filled.
struct Caption {
    Rect rect;
    const char* text;
    float size;
    uint32_t pitch;
};

struct Patch {
    uint64_t hash;
    std::initializer_list<Rect> clear;  // Japanese that goes without English in its place
    std::initializer_list<Move> moves;
    std::initializer_list<Phrase> phrases;
    std::initializer_list<Block> blocks = {};
    std::initializer_list<Paragraph> paragraphs = {};
    std::initializer_list<Caption> captions = {};
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
    // The tutorial's title, above the game's own IKARUGA:TUTORIAL, and its paragraphs, one or two at
    // a time.
    {0x65f2bc837f661916ull, {}, {}, {{{{0, 0, 128, 32}, {0, 48, 88, 32}}, "How to Play", 24, true}}},
    {0x1f6dd1474ba06bddull,
     {},
     {},
     {},
     {},
     {{{0, 0, 256, 64},
       {0, 176, 112, 40},
       248,
       2,
       "Move your ship with the 8-way stick, and fire with the shot button. (Hold it down for rapid fire.)",
       15},
      {{0, 64, 256, 64},
       {120, 176, 104, 40},
       248,
       2,
       "Press the polarity button to switch your ship between white polarity and black polarity.",
       15},
      {{0, 128, 256, 48}, {0, 216, 104, 24}, 248, 0, "Every enemy has a polarity too: white or black.", 15}}},
    {0xad1325e10fb746acull,
     {},
     {},
     {},
     {},
     {{{0, 0, 256, 48}, {0, 160, 112, 40}, 248, 1, "Shots of the polarity opposite to an enemy's do double damage.", 15},
      {{0, 48, 256, 48},
       {0, 208, 112, 40},
       248,
       2,
       "An enemy destroyed by shots of its own polarity always fires back a burst of bullets.",
       15},
      {{0, 96, 256, 64},
       {120, 160, 112, 64},
       248,
       0,
       "Your ship absorbs such return fire, and any enemy bullets of its own polarity, when it touches them.",
       15}}},
    {0xc087a822f8bc1ac0ull,
     {},
     {},
     {},
     {},
     {{{0, 0, 256, 64},
       {120, 176, 112, 64},
       248,
       0,
       "A bullet of the opposite polarity costs you a ship. (With no ships left, the game is over.)",
       15},
      {{0, 64, 256, 48}, {0, 176, 112, 24}, 248, 0, "Touching the scenery or an enemy also costs you a ship.", 15},
      {{0, 112, 256, 64}, {0, 200, 112, 40}, 248, 2, "Absorbing bullets of your own polarity fills the power gauge.", 15}}},
    {0x6a15ce16467b7a97ull,
     {},
     {},
     {},
     {},
     {{{0, 0, 256, 64},
       {0, 168, 112, 64},
       248,
       0,
       "With at least one gauge full, press both buttons at once to fire powerful homing lasers. (Power "
       "release)",
       15},
      {{0, 64, 256, 64},
       {120, 168, 112, 64},
       248,
       0,
       "A power release fires one laser for each full gauge, up to 12. You can use it as often as you like.",
       15},
      {{0, 128, 256, 40}, {0, 232, 112, 24}, 248, -1, "The lasers of a power release also pass through the scenery.", 15}}},
    {0xc628edcf2befcbe9ull,
     {},
     {},
     {},
     {},
     {{{0, 0, 256, 64},
       {120, 152, 112, 48},
       248,
       0,
       "Destroy three enemies of the same polarity in a row to earn a chain bonus.",
       15},
      {{0, 64, 256, 88},
       {0, 152, 112, 88},
       248,
       0,
       "A chain forms when each three kills share one polarity, as in white, white, white or black, black, "
       "black. Keep chaining, and the bonus doubles each time.",
       15}}},
    // GALLERY 2 of the APPENDIX: the characters, each a page whose picture holds the words. The names
    // stay, with their own romaji.
    {0xbec0d1ad60c64b4aull,
     {},
     {},
     {},
     {},
     {},
     {{{130, 166, 382, 289},
       "He holds a sincere resolve to stay true to his convictions, yet has the recklessness of youth. For "
       "all his fierce looks, he keeps his feelings to himself.\n"
       "So that no regret remains when death comes, he acts without care for himself, and to some he may "
       "seem in a hurry to die. In time death does come, but Shinra, changed into a being of spirit, "
       "lives on.",
       18,
       24}}},
    {0x8996624b180e7477ull,
     {},
     {},
     {},
     {},
     {},
     {{{130, 190, 382, 266},
       "Once a pilot in Horai's army, she was the assassin who came for Shinra in a Horai fighter. A "
       "second-generation pilot with enhanced genes, she outlasts ordinary pilots in endurance and more.\n"
       "Defeated by Shinra, she crashes and is rescued. Still doubtful, she grows curious about the way "
       "Shinra and the others live, and flies the fighter Ginkei at Shinra's side.",
       18,
       24}}},
    {0x23bad286913250c0ull,
     {},
     {},
     {},
     {},
     {},
     {{{130, 190, 382, 266},
       "Burdened by Horai's policies, Ikaruga Village became a place to leave the old and the unwanted. Old "
       "Kazamori is its elder. Wise, with the eye and the judgment to see the world as a whole, he is "
       "Shinra's support at heart.\n"
       "With Amanai and Shinkai he built the fighter Ikaruga, and he throws himself into the fight, "
       "believing that a day will come when freedom lets people live.",
       18,
       24}}},
    {0x5a84a01b236471cfull,
     {},
     {},
     {},
     {},
     {},
     {{{130, 214, 382, 242},
       "Before he was sent to Ikaruga Village, he was a brilliant designer of fighters. When he arrived he "
       "had lost the will to live, a walking corpse, but Old Kazamori persuaded him to help build the "
       "Ikaruga, and he did most of its design and assembly.\n"
       "Like many engineers he is a little contrary and seldom open with people, but at heart he is kind.",
       18,
       24}}},
    {0x904dc40b8765be28ull,
     {},
     {},
     {},
     {},
     {},
     {{{130, 190, 382, 265},
       "Bold and rough, yet caring toward others, he is the head of Ikaruga Village. He is as "
       "straightforward as he looks, but his soft heart sometimes works against him. He is a good older "
       "brother to Shinra, and an able go-between for Amanai and Shinra.\n"
       "He knows machines to a degree, but Amanai, who hates rough work, seems to ask him only for heavy "
       "lifting.",
       18,
       24}}},
    {0x0b59e4b8e1385800ull,
     {},
     {},
     {},
     {},
     {},
     {{{130, 143, 382, 313},
       "A psychic from the start, she met a stone-like object in a cavern beneath her country and sensed "
       "an intense lingering will. Horai took it for a divine revelation, but she attuned to it without "
       "grasping its true meaning, and began to run wild. Then, holding on to a shallow sense of mission, "
       "Horai went mad: to take even the stone's power into herself, she fitted the stone-like object with "
       "a control device and set out.\n"
       "She should be young, yet her face is deeply lined and her hair is white, like an old woman's.",
       18,
       24}}},
};

const Patch* find(uint64_t hash) {
    for (const Patch& p : kPatches)
        if (p.hash == hash)
            return &p;
    return nullptr;
}

#ifdef __APPLE__
CTFontRef font(float size, bool bold = false) {
    return CTFontCreateWithName(bold ? CFSTR("HiraMinProN-W6") : CFSTR("HiraMinProN-W3"), size, nullptr);
}

// The line's coverage, `width` x `height` texels, the text centered or flush left, at the largest
// size up to `size` that fits.
std::vector<uint8_t> line(const char* utf8, float size, uint32_t width, uint32_t height, bool left = false,
                          bool bold = false) {
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
        f = font(s, bold);
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
#else
// The same line with stb_truetype: the English textures use the same system font as the overlay and
// the game's 24-dot font.
std::vector<uint8_t> line(const char* utf8, float size, uint32_t width, uint32_t height, bool left = false,
                          bool bold = false) {
    std::vector<uint8_t> grey;
    stbtext::line(utf8, size, width, height, left, bold, grey);
    return grey;
}
#endif

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

#ifdef __APPLE__
double measure(const std::string& utf8, float size, bool bold = false) {
    CFStringRef text = CFStringCreateWithCString(nullptr, utf8.c_str(), kCFStringEncodingUTF8);
    if (!text)
        return 0;
    CTFontRef f = font(size, bold);
    const void* keys[] = {kCTFontAttributeName};
    const void* values[] = {f};
    CFDictionaryRef attributes =
        CFDictionaryCreate(nullptr, keys, values, 1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFAttributedStringRef string = CFAttributedStringCreate(nullptr, text, attributes);
    CTLineRef run = CTLineCreateWithAttributedString(string);
    const double w = CTLineGetTypographicBounds(run, nullptr, nullptr, nullptr);
    CFRelease(run);
    CFRelease(string);
    CFRelease(attributes);
    CFRelease(f);
    CFRelease(text);
    return w;
}
#else
double measure(const std::string& utf8, float size, bool bold = false) {
    return stbtext::measure(utf8.c_str(), size, bold);
}
#endif

// The paragraph's lines at `size`, `pitch` texels apart: beside the right piece as wide as the block
// and the piece together. None when the text does not fit.
std::vector<std::string> wrap(const Paragraph& p, float size, uint32_t pitch) {
    std::vector<std::string> words;
    for (const char* s = p.text; *s;) {
        const char* end = std::strchr(s, ' ');
        words.emplace_back(s, end ? end : s + std::strlen(s));
        s = end ? end + 1 : s + std::strlen(s);
    }
    std::vector<std::string> lines;
    size_t next = 0;
    for (int top = 0; top + static_cast<int>(pitch) <= static_cast<int>(p.block.h) + 1 && next < words.size();
         top += static_cast<int>(pitch)) {
        const bool beside = top + 3 >= p.dy && top + static_cast<int>(pitch) - 3 <= p.dy + static_cast<int>(p.right.h);
        const double room = (beside ? p.dx + static_cast<int>(p.right.w) : static_cast<int>(p.block.w)) - 4.0;
        std::string text = words[next++];
        while (next < words.size() && measure(text + " " + words[next], size) <= room) text += " " + words[next++];
        lines.push_back(text);
    }
    if (next < words.size())
        lines.clear();
    return lines;
}

void paragraph(const Paragraph& p, uint32_t width, uint32_t height, std::vector<uint32_t>& rgba) {
    float size = p.size;
    uint32_t pitch = 0;
    std::vector<std::string> lines;
    for (; size >= 6.0f && lines.empty(); size -= 0.5f) {
        pitch = static_cast<uint32_t>(std::lround(size * 4 / 3));
        lines = wrap(p, size, pitch);
    }
    size += 0.5f;
    const int cw = std::max(static_cast<int>(p.block.w), p.dx + static_cast<int>(p.right.w)),
              ch = std::max(static_cast<int>(p.block.h), p.dy + static_cast<int>(p.right.h));
    std::vector<uint8_t> canvas(static_cast<size_t>(cw) * ch);
    for (size_t i = 0; i < lines.size(); ++i) {
        const std::vector<uint8_t> grey = line(lines[i].c_str(), size, cw, pitch, true);
        for (uint32_t y = 0; y < pitch && i * pitch + y < static_cast<size_t>(ch); ++y)
            std::copy_n(&grey[y * cw], cw, &canvas[(i * pitch + y) * cw]);
    }
    glow(canvas, cw, ch);
    clear(p.block, width, height, rgba);
    clear(p.right, width, height, rgba);
    const auto put = [&](const Rect& r, int dx, int dy, int from_x) {
        for (int y = 0; y < static_cast<int>(r.h) && r.y + y < height; ++y)
            for (int x = 0; x < static_cast<int>(r.w) && r.x + x < width; ++x) {
                const int cx = dx + x, cy = dy + y;
                if (cx < from_x || cx >= cw || cy < 0 || cy >= ch)
                    continue;
                if (const uint8_t a = canvas[cy * cw + cx])
                    rgba[(r.y + y) * width + r.x + x] = static_cast<uint32_t>(a) << 24 | 0xFFFFFFu;
            }
    };
    put(p.block, 0, 0, 0);
    // Where the right piece lies over the block, the block draws the letters.
    put(p.right, p.dx, p.dy, static_cast<int>(p.block.w));
}


// The caption's lines at `size`: each paragraph's words wrapped to `room` texels, an empty line
// between paragraphs.
std::vector<std::string> wrap(const char* text, float size, double room) {
    std::vector<std::string> lines;
    std::string paragraph_text;
    for (const char* s = text;; ++s) {
        if (*s && *s != '\n') {
            paragraph_text += *s;
            continue;
        }
        if (!lines.empty())
            lines.emplace_back();
        std::string line_text;
        size_t from = 0;
        while (from < paragraph_text.size()) {
            size_t end = paragraph_text.find(' ', from);
            if (end == std::string::npos)
                end = paragraph_text.size();
            const std::string word = paragraph_text.substr(from, end - from);
            if (!line_text.empty() && measure(line_text + " " + word, size, true) > room) {
                lines.push_back(line_text);
                line_text.clear();
            }
            line_text += (line_text.empty() ? "" : " ") + word;
            from = end + 1;
        }
        if (!line_text.empty())
            lines.push_back(line_text);
        paragraph_text.clear();
        if (!*s)
            break;
    }
    return lines;
}

void caption(const Caption& c, uint32_t width, uint32_t height, std::vector<uint32_t>& rgba) {
    const Rect& r = c.rect;
    if (r.x + r.w > width || r.y + r.h > height)
        return;
    const auto at = [&](uint32_t x, uint32_t y) -> uint32_t& { return rgba[(r.y + y) * width + r.x + x]; };
    // The letters with their edges.
    const int w = static_cast<int>(r.w), h = static_cast<int>(r.h);
    std::vector<uint8_t> hole(static_cast<size_t>(w) * h);
    constexpr int edge = 2;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            if ((at(x, y) >> 8 & 0xFF) > 40)
                for (int my = std::max(0, y - edge); my <= std::min(h - 1, y + edge); ++my)
                    for (int mx = std::max(0, x - edge); mx <= std::min(w - 1, x + edge); ++mx) hole[my * w + mx] = 1;
    // The art from around them: the rect and a texel around it as floats, the holes first filled a ring
    // at a time from their known neighbors, then smoothed, each texel the mean of its four neighbors.
    const int fw = w + 2, fh = h + 2;
    std::vector<float> art(static_cast<size_t>(fw) * fh * 3);
    std::vector<uint8_t> known(static_cast<size_t>(fw) * fh, 1);
    for (int y = 0; y < fh; ++y)
        for (int x = 0; x < fw; ++x) {
            const int tx = std::clamp(static_cast<int>(r.x) + x - 1, 0, static_cast<int>(width) - 1),
                      ty = std::clamp(static_cast<int>(r.y) + y - 1, 0, static_cast<int>(height) - 1);
            const uint32_t v = rgba[ty * width + tx];
            for (int k = 0; k < 3; ++k) art[(y * fw + x) * 3 + k] = static_cast<float>(v >> (8 * k) & 0xFF);
            if (x > 0 && y > 0 && x <= w && y <= h && hole[(y - 1) * w + x - 1])
                known[y * fw + x] = 0;
        }
    for (bool filled = true; filled;) {
        filled = false;
        std::vector<uint8_t> next = known;
        for (int y = 1; y <= h; ++y)
            for (int x = 1; x <= w; ++x) {
                if (known[y * fw + x])
                    continue;
                float sum[3] = {};
                int n = 0;
                for (int dy = -1; dy <= 1; ++dy)
                    for (int dx = -1; dx <= 1; ++dx)
                        if (known[(y + dy) * fw + x + dx]) {
                            for (int k = 0; k < 3; ++k) sum[k] += art[((y + dy) * fw + x + dx) * 3 + k];
                            ++n;
                        }
                if (n) {
                    for (int k = 0; k < 3; ++k) art[(y * fw + x) * 3 + k] = sum[k] / static_cast<float>(n);
                    next[y * fw + x] = 1;
                    filled = true;
                }
            }
        known.swap(next);
    }
    for (int pass = 0; pass < 100; ++pass)
        for (int y = 1; y <= h; ++y)
            for (int x = 1; x <= w; ++x)
                if (hole[(y - 1) * w + x - 1])
                    for (int k = 0; k < 3; ++k)
                        art[(y * fw + x) * 3 + k] =
                            (art[((y - 1) * fw + x) * 3 + k] + art[((y + 1) * fw + x) * 3 + k] +
                             art[(y * fw + x - 1) * 3 + k] + art[(y * fw + x + 1) * 3 + k]) /
                            4;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            if (hole[y * w + x]) {
                uint32_t v = 0xFF000000u;
                for (int k = 0; k < 3; ++k)
                    v |= static_cast<uint32_t>(std::lround(art[((y + 1) * fw + x + 1) * 3 + k])) << (8 * k);
                at(x, y) = v;
            }
    float size = c.size;
    const uint32_t pitch = c.pitch;
    std::vector<std::string> lines;
    for (; size >= 6.0f; size -= 0.5f) {
        lines = wrap(c.text, size, r.w - 4.0);
        if (lines.size() * pitch <= r.h)
            break;
    }
    std::vector<uint8_t> letters(static_cast<size_t>(r.w) * r.h);
    for (size_t i = 0; i < lines.size(); ++i) {
        if (lines[i].empty())
            continue;
        const std::vector<uint8_t> grey = line(lines[i].c_str(), size, r.w, pitch, true, true);
        for (uint32_t y = 0; y < pitch && i * pitch + y < r.h; ++y)
            std::copy_n(&grey[y * r.w], r.w, &letters[(i * pitch + y) * r.w]);
    }
    // The dark edge: the letters a texel wider, darkening the art.
    for (uint32_t y = 0; y < r.h; ++y)
        for (uint32_t x = 0; x < r.w; ++x) {
            int shade = 0;
            for (int dy = -1; dy <= 1; ++dy)
                for (int dx = -1; dx <= 1; ++dx) {
                    const int mx = static_cast<int>(x) + dx, my = static_cast<int>(y) + dy;
                    if (mx >= 0 && my >= 0 && mx < static_cast<int>(r.w) && my < static_cast<int>(r.h))
                        shade = std::max<int>(shade, letters[my * r.w + mx]);
                }
            const uint32_t v = at(x, y);
            const int a = letters[y * r.w + x];
            uint32_t out = 0xFF000000u;
            for (int k = 0; k < 3; ++k) {
                const int dark = (v >> (8 * k) & 0xFF) * (255 - shade * 3 / 4) / 255;
                out |= static_cast<uint32_t>(dark + (255 - dark) * a / 255) << (8 * k);
            }
            at(x, y) = out;
        }
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
    for (const Paragraph& para : p->paragraphs) paragraph(para, width, height, rgba);
    for (const Caption& c : p->captions) caption(c, width, height, rgba);
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
