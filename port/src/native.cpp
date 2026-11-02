// Functions of the game written natively: the C library's single-precision math, the one Ninja
// function the decompiler cannot handle, Shinobi's cache functions and a corrected game function.
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

#include "decomp.h"
#include "diff.h"

namespace game {

float frexp(float value, gptr<int> exponent) {
    int e = 0;
    const float m = std::frexp(value, &e);
    if (exponent)
        *exponent = e;
    return m;
}
float log10(float x) { return std::log10(x); }
float _in_atans(float x) { return std::atan(x); }
float _in_atan2s(float y, float x) { return std::atan2(y, x); }
float _in_fmods(float x, float y) { return std::fmod(x, y); }
float _in_ldexps(float x, int exponent) { return std::ldexp(x, exponent); }
float _in_log10s(float x) { return std::log10(x); }
float _in_logs(float x) { return std::log(x); }

// Written from the disassembly at 0x8C010E90. It derives the screen's half size and clip
// rectangle from its size: a holds the size at +0x28 and +0x2C, b receives the rectangle.
void njInitGlobal2() {
    const uint32_t a = guest::load<uint32_t>(0x8C010FE8u);
    const uint32_t b = guest::load<uint32_t>(0x8C010FECu);
    auto f = [](uint32_t at) { return guest::load<float>(at); };
    auto set = [](uint32_t at, float v) { guest::store<float>(at, v); };
    set(a + 0x08, f(a + 0x28) / 2.0f);
    set(a + 0x0C, f(a + 0x2C) / 2.0f);
    set(b + 0x18, 0.0f);
    set(b + 0x1C, 0.0f);
    set(b + 0x20, f(a + 0x28) - 1.0f);
    set(b + 0x24, f(a + 0x2C) - 1.0f);
    set(b + 0x08, f(b + 0x20) - f(a + 0x08));
    set(b + 0x0C, f(b + 0x18) - f(a + 0x08));
    set(b + 0x10, f(b + 0x24) - f(a + 0x0C));
    set(b + 0x14, f(b + 0x1C) - f(a + 0x0C));
}

// Shinobi's operand cache functions. The port has no cache: DMA reads and writes guest memory
// itself. The machine code masks the interrupts, saves the old mask in a stack word and jumps to
// an uncached copy of itself, which restores the mask from that word. C cannot pass the word, so
// these leave SR alone.
void syCacheOCBI(gptr<void> address, Uint32 size) { diff::Check check("syCacheOCBI", 0x8C04241Eu, address, size); }
void syCacheOCBP(gptr<void> address, Uint32 size) { diff::Check check("syCacheOCBP", 0x8C04267Au, address, size); }
void syCacheOCWB(gptr<void> address, Uint32 size) { diff::Check check("syCacheOCWB", 0x8C0428CAu, address, size); }
void syCachePurgeAll() { diff::Check check("syCachePurgeAll", 0x8C042B58u); }

}  // namespace game

namespace game {

// The SHC runtime's division helpers: divisor in r0, dividend in r1. Division by zero returns 0
// here; the console's helper also sets errno.
int _divls(int divisor, int dividend) {
    if (divisor == 0) return 0;
    if (divisor == -1) return static_cast<int>(0u - static_cast<uint32_t>(dividend));
    return dividend / divisor;
}
uint _divlu(uint divisor, uint dividend) { return divisor ? dividend / divisor : 0; }
int _modls(int divisor, int dividend) {
    if (divisor == 0 || divisor == -1) return 0;
    return dividend % divisor;
}
uint _modlu(uint divisor, uint dividend) { return divisor ? dividend % divisor : 0; }
int _divws(int divisor, int dividend) {
    const int16_t d = static_cast<int16_t>(divisor), n = static_cast<int16_t>(dividend);
    return d ? n / d : 0;
}

// Structure copies: size in r0, destination in r1, source in r2.
namespace {
void copy(uint32_t dst, uint32_t src, uint32_t size) {
    if (uint8_t* d = guest::direct(dst)) {
        if (const uint8_t* s = guest::direct(src)) {
            std::memmove(d, s, size);
            return;
        }
    }
    for (uint32_t i = 0; i < size; ++i) guest::store<uint8_t>(dst + i, guest::load<uint8_t>(src + i));
}
void strcpy_guest(uint32_t dst, uint32_t src) {
    for (uint32_t i = 0;; ++i) {
        const uint8_t c = guest::load<uint8_t>(src + i);
        guest::store<uint8_t>(dst + i, c);
        if (!c) break;
    }
}
}  // namespace

void _quick_mvn(uint size, gptr<void> dst, gptr<void> src) { copy(dst.a, src.a, size); }
void _quick_odd_mvn(uint size, gptr<void> dst, gptr<void> src) { copy(dst.a, src.a, size); }
void _quick_evn_mvn(uint size, gptr<void> dst, gptr<void> src) { copy(dst.a, src.a, size); }
void _slow_mvn(uint size, gptr<void> dst, gptr<void> src) { copy(dst.a, src.a, size); }
gptr<void> _quick_strcpy(gptr<void> dst, gptr<void> src) {
    strcpy_guest(dst.a, src.a);
    return dst;
}
gptr<void> quick_strcpy1(gptr<void> dst, gptr<void> src) {
    strcpy_guest(dst.a, src.a);
    return dst;
}
gptr<void> _slow_strcpy(gptr<void> dst, gptr<void> src) {
    strcpy_guest(dst.a, src.a);
    return dst;
}

}  // namespace game

// ---- sprintf and textPrintf ------------------------------------------------------------------------

namespace game {

namespace {
std::string guest_string(uint32_t a) {
    std::string s;
    for (char c; a && (c = static_cast<char>(guest::load<uint8_t>(a))) != 0 && s.size() < 4096; ++a)
        s += c;
    return s;
}
}  // namespace

int format_words(gptr<char> buffer, uint32_t format, const uint32_t* words, size_t count) {
    const std::string fmt = guest_string(format);
    std::string out;
    size_t next = 0;
    auto word = [&]() { return next < count ? words[next++] : 0u; };
    for (size_t i = 0; i < fmt.size(); ++i) {
        if (fmt[i] != '%') {
            out += fmt[i];
            continue;
        }
        // One conversion: flags, width, precision and length as written, then the type.
        size_t j = i + 1;
        while (j < fmt.size() && std::strchr("-+ #0", fmt[j])) ++j;
        std::string spec = fmt.substr(i, j - i);
        auto number = [&] {
            if (j < fmt.size() && fmt[j] == '*') {
                spec += std::to_string(static_cast<int32_t>(word()));
                ++j;
            }
            while (j < fmt.size() && std::isdigit(static_cast<unsigned char>(fmt[j]))) spec += fmt[j++];
        };
        number();
        if (j < fmt.size() && fmt[j] == '.') {
            spec += fmt[j++];
            number();
        }
        while (j < fmt.size() && std::strchr("hlLqjzt", fmt[j])) ++j;  // every value is a word
        if (j >= fmt.size())
            break;
        const char type = fmt[j];
        char piece[512];
        switch (type) {
            case 'd': case 'i':
                std::snprintf(piece, sizeof piece, (spec + "d").c_str(), static_cast<int32_t>(word()));
                break;
            case 'u': case 'x': case 'X': case 'o':
                std::snprintf(piece, sizeof piece, (spec + type).c_str(), word());
                break;
            case 'c':
                std::snprintf(piece, sizeof piece, (spec + "c").c_str(), static_cast<int>(word() & 0xFF));
                break;
            case 's':
                std::snprintf(piece, sizeof piece, (spec + "s").c_str(), guest_string(word()).c_str());
                break;
            case 'p':
                std::snprintf(piece, sizeof piece, "%08x", word());
                break;
            case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': {
                const uint32_t w = word();
                float f;
                std::memcpy(&f, &w, 4);
                std::snprintf(piece, sizeof piece, (spec + type).c_str(), static_cast<double>(f));
                break;
            }
            case '%':
                std::snprintf(piece, sizeof piece, "%%");
                break;
            default:
                std::snprintf(piece, sizeof piece, "%s%c", spec.c_str(), type);
                break;
        }
        out += piece;
        i = j;
    }
    for (size_t k = 0; k < out.size(); ++k)
        guest::store<uint8_t>(buffer.a + static_cast<uint32_t>(k), static_cast<uint8_t>(out[k]));
    guest::store<uint8_t>(buffer.a + static_cast<uint32_t>(out.size()), 0);
    return static_cast<int>(out.size());
}

// The game's vsprintf reads the values from where the machine code's caller left them on the stack.
// The text goes through the buffer at 0x8C1CAF38. On the rotated screen, x runs down the screen
// from 0x8C06B154's width.
void text_printf(float x, float y, uint32_t format, const uint32_t* values, size_t count) {
    guest::Frame frame(12 + 4 * 8);
    const uint32_t position = frame.top - 12, words = position - 4 * 8;
    for (size_t i = 0; i < count && i < 8; ++i)
        guest::store<uint32_t>(words + 4u * static_cast<uint32_t>(i), values[i]);
    const gptr<char> buffer(0x8C1CAF38u);
    if (screenHorizontal != 0)
        clipOff();
    vsprintf(buffer, gptr<char>(format), gptr<char>(words));
    const float cell = guest::load<float>(0x8C06B150u);
    if (screenHorizontal == 0) {
        guest::store<float>(position, guest::load<float>(0x8C06B154u) - y * cell);
        guest::store<float>(position + 4, x * cell);
    }
    else {
        guest::store<float>(position, x * cell);
        guest::store<float>(position + 4, y * cell);
    }
    guest::store<uint32_t>(position + 8, guest::load<uint32_t>(0x8C06B158u));
    textDraw(0x4000000, buffer, gptr<float>(position), 0);
    if (screenHorizontal != 0)
        clipPlayArea();
}

// The tutorial's background task keeps its scroll, a float, at the start of its data, but the game
// asks taskCreate for 0 data bytes. syMalloc rounds 0x18 and 0x1C bytes up to the same block, so
// asking for the 4 bytes changes no memory, and the float stays inside the block under any other
// allocator.
void tutBg() {
    diff::Check check("tutBg", 0x8C0C6BBCu);
    const gptr<float> scroll = gptr<float>(taskCreate(gptr<code>(0x8C0C6B9Cu), gptr<code>(0x8C0C6B24u), 4, 0));
    if (scroll)
        *scroll = 0.0f;
}

}  // namespace game
