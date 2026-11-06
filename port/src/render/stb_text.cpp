// The overlay's text and boxes, and the game's 24-dot text font, without Core Text: stb_truetype
// rasterizes a system font (a Japanese one where there is one) into the same premultiplied RGBA
// bitmap and 24x24 cells that the macOS build makes with Core Text and Hiragino. The port ships no
// font, so a system without one gets boxes but no text, as the ROM's own font still draws the game's
// Japanese when a boot ROM image is given.
#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

#include "render/font.h"
#include "render/overlay.h"
#include "render/stb_text.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <unordered_map>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#else
#include <iconv.h>
#endif

namespace render {
namespace {

struct Face {
    std::vector<unsigned char> data;
    stbtt_fontinfo info{};
    bool ready = false;
};

const char* const kFonts[] = {
#if defined(_WIN32)
    "C:\\Windows\\Fonts\\msgothic.ttc",
    "C:\\Windows\\Fonts\\meiryo.ttc",
    "C:\\Windows\\Fonts\\YuGothM.ttc",
    "C:\\Windows\\Fonts\\arial.ttf",
    "C:\\Windows\\Fonts\\segoeui.ttf",
#else
    "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
    "/usr/share/fonts/truetype/noto/NotoSansCJK-Regular.ttc",
    "/usr/share/fonts/opentype/noto/NotoSansMonoCJKjp-Regular.otf",
    "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
    "/usr/share/fonts/TTF/DejaVuSans.ttf",
#endif
};

Face& face() {
    static Face f = [] {
        Face out;
        for (const char* path : kFonts) {
            std::ifstream in(path, std::ios::binary);
            if (!in)
                continue;
            out.data.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
            if (out.data.empty())
                continue;
            const int offset = stbtt_GetFontOffsetForIndex(out.data.data(), 0);
            if (offset >= 0 && stbtt_InitFont(&out.info, out.data.data(), offset)) {
                out.ready = true;
                break;
            }
            out.data.clear();
        }
        return out;
    }();
    return f;
}

// The Shift-JIS character's Unicode code point, or 0 when the code names none.
uint32_t unicode_of_sjis(uint16_t code) {
#if defined(_WIN32)
    char in[2] = {static_cast<char>((code >> 8) ? (code >> 8) : code), static_cast<char>(code)};
    const int length = (code >> 8) ? 2 : 1;
    wchar_t out[2] = {};
    if (MultiByteToWideChar(932, 0, in, length, out, 2) != 1)
        return 0;
    return static_cast<uint32_t>(out[0]);
#else
    static iconv_t cd = iconv_open("UTF-32LE", "SHIFT-JIS");
    if (cd == reinterpret_cast<iconv_t>(-1))
        return 0;
    char in[2] = {static_cast<char>((code >> 8) ? (code >> 8) : code), static_cast<char>(code)};
    char out[8] = {};
    char* input = in;
    char* output = out;
    size_t input_bytes = (code >> 8) ? 2 : 1;
    size_t output_bytes = sizeof(out);
    if (iconv(cd, &input, &input_bytes, &output, &output_bytes) == static_cast<size_t>(-1))
        return 0;
    uint32_t cp = 0;
    if (sizeof(out) - output_bytes >= 4)
        std::memcpy(&cp, out, 4);
    return cp;
#endif
}

// The dots of a glyph from its coverage. The bitmap's first row is the top one, as in the ROM.
void cut(Glyph& g) {
    g.bits.fill(0);
    for (int y = 0; y < 24; ++y)
        for (int x = 0; x < 24; ++x)
            if (g.cover[24 * y + x] >= 190)
                g.bits[3 * y + x / 8] |= static_cast<uint8_t>(0x80 >> (x % 8));
}

std::vector<uint32_t> codepoints(const std::string& utf8) {
    std::vector<uint32_t> out;
    for (size_t i = 0; i < utf8.size();) {
        const unsigned char c = static_cast<unsigned char>(utf8[i]);
        uint32_t cp = '?';
        size_t n = 1;
        if (c < 0x80) {
            cp = c;
        }
        else if ((c & 0xE0) == 0xC0 && i + 1 < utf8.size()) {
            cp = ((c & 0x1Fu) << 6) | (static_cast<unsigned char>(utf8[i + 1]) & 0x3Fu);
            n = 2;
        }
        else if ((c & 0xF0) == 0xE0 && i + 2 < utf8.size()) {
            cp = ((c & 0x0Fu) << 12) | ((static_cast<unsigned char>(utf8[i + 1]) & 0x3Fu) << 6) |
                 (static_cast<unsigned char>(utf8[i + 2]) & 0x3Fu);
            n = 3;
        }
        else if ((c & 0xF8) == 0xF0 && i + 3 < utf8.size()) {
            cp = ((c & 0x07u) << 18) | ((static_cast<unsigned char>(utf8[i + 1]) & 0x3Fu) << 12) |
                 ((static_cast<unsigned char>(utf8[i + 2]) & 0x3Fu) << 6) |
                 (static_cast<unsigned char>(utf8[i + 3]) & 0x3Fu);
            n = 4;
        }
        out.push_back(cp);
        i += n;
    }
    return out;
}

float advance(const stbtt_fontinfo& font, uint32_t cp, float scale) {
    int a = 0, lsb = 0;
    stbtt_GetCodepointHMetrics(&font, cp, &a, &lsb);
    return a * scale;
}

// Draws `utf8` at the baseline with the font's pixels.
void draw_text(const stbtt_fontinfo& font, float scale, float x, float baseline, const std::string& utf8,
               uint8_t* grey, int stride, int width, int height) {
    float pen = x;
    for (uint32_t cp : codepoints(utf8)) {
        int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
        stbtt_GetCodepointBitmapBox(&font, cp, scale, scale, &x0, &y0, &x1, &y1);
        const int gw = x1 - x0, gh = y1 - y0;
        if (gw > 0 && gh > 0) {
            std::vector<unsigned char> bitmap(static_cast<size_t>(gw) * gh);
            stbtt_MakeCodepointBitmap(&font, bitmap.data(), gw, gh, gw, scale, scale, cp);
            const int at_x = static_cast<int>(std::lround(pen)) + x0;
            int at_y = static_cast<int>(std::lround(baseline)) + y0;
            for (int gy = 0; gy < gh; ++gy, ++at_y) {
                if (at_y < 0 || at_y >= height)
                    continue;
                for (int gx = 0; gx < gw; ++gx) {
                    const int px = at_x + gx;
                    if (px >= 0 && px < width)
                        grey[static_cast<size_t>(at_y) * stride + px] =
                            std::max(grey[static_cast<size_t>(at_y) * stride + px], bitmap[static_cast<size_t>(gy) * gw + gx]);
                }
            }
        }
        pen += advance(font, cp, scale);
    }
}

void blend(uint32_t* pixel, uint32_t rgba) {
    const uint32_t a = rgba >> 24;
    if (a == 0)
        return;
    const uint32_t r = (rgba & 255) * a / 255;
    const uint32_t g = ((rgba >> 8) & 255) * a / 255;
    const uint32_t b = ((rgba >> 16) & 255) * a / 255;
    const uint32_t dst = *pixel;
    const uint32_t da = dst >> 24;
    const uint32_t keep = 255 - a;
    const uint32_t out_a = a + da * keep / 255;
    const uint32_t out_r = r + (dst & 255) * keep / 255;
    const uint32_t out_g = g + ((dst >> 8) & 255) * keep / 255;
    const uint32_t out_b = b + ((dst >> 16) & 255) * keep / 255;
    *pixel = (out_a << 24) | (out_b << 16) | (out_g << 8) | out_r;
}

}  // namespace

namespace stbtext {

double measure(const char* utf8, float size, bool bold) {
    (void)bold;
    Face& f = face();
    if (!f.ready || !utf8)
        return 0;
    const float scale = stbtt_ScaleForPixelHeight(&f.info, size);
    double width = 0;
    for (uint32_t cp : codepoints(utf8))
        width += advance(f.info, cp, scale);
    return width;
}

bool line(const char* utf8, float size, uint32_t width, uint32_t height, bool left, bool bold,
          std::vector<uint8_t>& out) {
    out.assign(static_cast<size_t>(width) * height, 0);
    Face& f = face();
    if (!f.ready || !utf8 || !width || !height)
        return false;
    float chosen = size;
    for (; chosen >= 6.0f; chosen -= 0.5f)
        if (measure(utf8, chosen, bold) + 4 <= width)
            break;
    if (chosen < 6.0f)
        chosen = 6.0f;
    const float scale = stbtt_ScaleForPixelHeight(&f.info, chosen);
    int ascent = 0, descent = 0, gap = 0;
    stbtt_GetFontVMetrics(&f.info, &ascent, &descent, &gap);
    const float cap = ascent * scale * 0.72f;
    const float baseline = (height + cap) / 2;
    const double text_width = measure(utf8, chosen, bold);
    const float x = left ? 1.0f : static_cast<float>((width - text_width) / 2);
    draw_text(f.info, scale, x, baseline, utf8, out.data(), static_cast<int>(width), static_cast<int>(width),
              static_cast<int>(height));
    if (bold)
        draw_text(f.info, scale, x + 0.8f, baseline, utf8, out.data(), static_cast<int>(width), static_cast<int>(width),
                  static_cast<int>(height));
    return true;
}

}  // namespace stbtext

Overlay::Overlay() = default;
Overlay::~Overlay() = default;

void Overlay::begin(uint32_t width, uint32_t height) {
    if (width != width_ || height != height_ || pixels_.empty()) {
        width_ = width;
        height_ = height;
        pixels_.assign(static_cast<size_t>(width) * height, 0);
    }
    std::fill(pixels_.begin(), pixels_.end(), 0u);
    active_ = true;
}

void Overlay::box(float x, float y, float w, float h, uint32_t rgba) {
    if (!active_ || w <= 0 || h <= 0)
        return;
    const int x0 = std::max(0, static_cast<int>(std::lround(x)));
    const int y0 = std::max(0, static_cast<int>(std::lround(y)));
    const int x1 = std::min(static_cast<int>(width_), static_cast<int>(std::lround(x + w)));
    const int y1 = std::min(static_cast<int>(height_), static_cast<int>(std::lround(y + h)));
    for (int py = y0; py < y1; ++py)
        for (int px = x0; px < x1; ++px)
            blend(&pixels_[static_cast<size_t>(py) * width_ + px], rgba);
}

float Overlay::text_width(const std::string& s, float size) {
    Face& f = face();
    if (!f.ready || s.empty())
        return 0;
    const float scale = stbtt_ScaleForPixelHeight(&f.info, size);
    float width = 0;
    for (uint32_t cp : codepoints(s))
        width += advance(f.info, cp, scale);
    return width;
}

float Overlay::text(float x, float y, const std::string& s, float size, uint32_t rgba) {
    Face& f = face();
    if (!f.ready || !active_ || s.empty())
        return 0;
    const float scale = stbtt_ScaleForPixelHeight(&f.info, size);
    int ascent = 0, descent = 0, gap = 0;
    stbtt_GetFontVMetrics(&f.info, &ascent, &descent, &gap);
    std::vector<unsigned char> grey(static_cast<size_t>(width_) * height_);
    draw_text(f.info, scale, x, y + ascent * scale, s, grey.data(), static_cast<int>(width_),
              static_cast<int>(width_), static_cast<int>(height_));
    const uint32_t base = rgba;
    for (uint32_t py = 0; py < height_; ++py)
        for (uint32_t px = 0; px < width_; ++px) {
            const uint32_t cover = grey[static_cast<size_t>(py) * width_ + px];
            if (!cover)
                continue;
            const uint32_t a = ((base >> 24) * cover) / 255;
            blend(&pixels_[static_cast<size_t>(py) * width_ + px], (a << 24) | (base & 0x00FFFFFFu));
        }
    float width = 0;
    for (uint32_t cp : codepoints(s))
        width += advance(f.info, cp, scale);
    return width;
}

std::vector<Glyph> line24(const std::string& utf8) {
    std::vector<Glyph> cells;
    Face& f = face();
    if (!f.ready)
        return cells;
    constexpr float kSize = 20;
    const float scale = stbtt_ScaleForPixelHeight(&f.info, kSize);
    int ascent = 0, descent = 0, gap = 0;
    stbtt_GetFontVMetrics(&f.info, &ascent, &descent, &gap);
    float width = 0;
    for (uint32_t cp : codepoints(utf8))
        width += advance(f.info, cp, scale);
    const size_t count = std::max<size_t>(1, static_cast<size_t>(std::ceil((width + 2) / 24)));
    const int w = static_cast<int>(24 * count);
    std::vector<unsigned char> grey(static_cast<size_t>(w) * 24);
    draw_text(f.info, scale, (w - width) / 2, 24 - ascent * scale, utf8, grey.data(), w, w, 24);
    cells.resize(count);
    for (size_t c = 0; c < count; ++c) {
        for (size_t y = 0; y < 24; ++y)
            for (size_t x = 0; x < 24; ++x)
                cells[c].cover[24 * y + x] = grey[w * y + 24 * c + x];
        cut(cells[c]);
    }
    return cells;
}

bool glyph24(uint16_t code, Glyph& out) {
    static std::unordered_map<uint16_t, std::pair<bool, Glyph>> drawn;
    auto it = drawn.find(code);
    if (it == drawn.end()) {
        Glyph g;
        bool ok = false;
        Face& f = face();
        const uint32_t cp = unicode_of_sjis(code);
        if (f.ready && cp) {
            constexpr float kSize = 22;
            const float scale = stbtt_ScaleForPixelHeight(&f.info, kSize);
            int ascent = 0, descent = 0, gap = 0;
            stbtt_GetFontVMetrics(&f.info, &ascent, &descent, &gap);
            int adv = 0, lsb = 0;
            stbtt_GetCodepointHMetrics(&f.info, cp, &adv, &lsb);
            int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
            stbtt_GetCodepointBitmapBox(&f.info, cp, scale, scale, &x0, &y0, &x1, &y1);
            const int gw = x1 - x0, gh = y1 - y0;
            if (gw > 0 && gh > 0) {
                std::vector<unsigned char> bitmap(static_cast<size_t>(gw) * gh);
                stbtt_MakeCodepointBitmap(&f.info, bitmap.data(), gw, gh, gw, scale, scale, cp);
                const int ox = static_cast<int>(std::lround((24 - adv * scale) / 2)) + x0;
                const int oy = static_cast<int>(std::lround(24 - ascent * scale)) + y0;
                for (int y = 0; y < gh; ++y)
                    for (int x = 0; x < gw; ++x) {
                        const int px = ox + x, py = oy + y;
                        if (px >= 0 && px < 24 && py >= 0 && py < 24)
                            g.cover[24 * py + px] = bitmap[static_cast<size_t>(y) * gw + x];
                    }
                cut(g);
                ok = true;
            }
        }
        it = drawn.emplace(code, std::make_pair(ok, g)).first;
    }
    out = it->second.second;
    return it->second.first;
}

}  // namespace render
