#include "render/texture.h"

namespace render {
namespace {

// A twiddled texel's place: the bits of y and x interleaved, y in the lower bit of each pair. A
// texture that is not square is a row or a column of square blocks.
uint32_t twiddled(uint32_t x, uint32_t y, uint32_t width, uint32_t height) {
    const uint32_t side = width < height ? width : height;
    uint32_t index = 0;
    for (uint32_t bit = 0; (1u << bit) < side; ++bit)
        index |= ((y >> bit) & 1u) << (2 * bit) | ((x >> bit) & 1u) << (2 * bit + 1);
    return index + (x / side + y / side) * side * side;
}

// The channels widen by repeating their upper bits.
uint32_t rgba(uint32_t r, uint32_t g, uint32_t b, uint32_t a) { return r | g << 8 | b << 16 | a << 24; }
uint32_t five(uint32_t v) { return v << 3 | v >> 2; }
uint32_t six(uint32_t v) { return v << 2 | v >> 4; }

uint32_t from16(uint32_t p, PixelFormat f) {
    switch (f) {
        case PixelFormat::Rgb565:
            return rgba(five(p >> 11), six((p >> 5) & 63), five(p & 31), 255);
        case PixelFormat::Argb4444:
            return rgba(((p >> 8) & 15) * 17, ((p >> 4) & 15) * 17, (p & 15) * 17, (p >> 12) * 17);
        default:
            return rgba(five((p >> 10) & 31), five((p >> 5) & 31), five(p & 31), p & 0x8000 ? 255 : 0);
    }
}

// Where a mipmap level starts. A 16-bit level of side 2^k lies after three texels of padding and
// every smaller level. A VQ level's index bytes lie after the codebook and every smaller level's,
// the 1x1 and the 2x2 level taking one byte each.
uint32_t level_offset(const TextureFormat& f, uint32_t level) {
    const uint32_t full = f.levels() - 1;
    if (!f.mipmapped || level > full)
        return 0;
    const uint32_t k = full - level;  // the level's side is 2^k
    if (f.vq)
        return k == 0 ? 0 : ((1u << (2 * (k - 1))) - 1) / 3 + 1;
    const uint32_t texels = 3 + ((1u << (2 * k)) - 1) / 3;
    return f.pixel == PixelFormat::Palette8 ? texels : texels * 2;
}

}  // namespace

uint32_t TextureFormat::levels() const {
    if (!mipmapped)
        return 1;
    uint32_t n = 1;
    while ((1u << (n - 1)) < width) ++n;
    return n;
}

bool describe_texture(uint32_t tcw, uint32_t tsp, uint32_t stride_texels, TextureFormat& out) {
    TextureFormat f;
    f.address = (tcw & 0x1FFFFF) * 8;
    f.width = 8u << ((tsp >> 3) & 7);
    f.height = 8u << (tsp & 7);
    f.pixel = static_cast<PixelFormat>((tcw >> 27) & 7);
    f.mipmapped = tcw >> 31;
    f.vq = (tcw >> 30) & 1;
    const bool palette = f.pixel == PixelFormat::Palette4 || f.pixel == PixelFormat::Palette8;
    if (palette) {
        // Bits 26 to 21 select the palette: 16 entries a step for 4 bits, 256 for 8.
        f.palette_base = f.pixel == PixelFormat::Palette4 ? ((tcw >> 21) & 63) * 16 : ((tcw >> 25) & 3) * 256;
    }
    else if ((tcw >> 26) & 1) {
        f.twiddled = false;
        f.mipmapped = false;
        f.row_texels = (tcw >> 25) & 1 ? stride_texels : f.width;
    }
    if (f.mipmapped)
        f.height = f.width;
    if (f.pixel == PixelFormat::Yuv422 || f.pixel == PixelFormat::BumpMap || f.pixel == PixelFormat::Reserved)
        return false;
    if (palette && (f.vq || f.mipmapped))
        return false;  // Ikaruga has none, and their mipmap offsets are not known here
    if (!f.twiddled && (f.vq || f.row_texels == 0))
        return false;
    out = f;
    return true;
}

void texture_extent(const TextureFormat& f, uint32_t& first, uint32_t& size) {
    first = f.address;
    const uint32_t texels = f.twiddled ? f.width * f.height : f.row_texels * f.height;
    if (f.vq) {
        size = 2048 + level_offset(f, 0) + texels / 4;
        return;
    }
    const uint32_t bits = f.pixel == PixelFormat::Palette4 ? 4 : f.pixel == PixelFormat::Palette8 ? 8 : 16;
    size = level_offset(f, 0) + texels * bits / 8;
}

bool decode_texture(const TextureFormat& f, uint32_t level, const uint8_t* vram, size_t vram_size,
                    const uint32_t* palette, std::vector<uint32_t>& out) {
    uint32_t first, size;
    texture_extent(f, first, size);
    if (static_cast<size_t>(first) + size > vram_size || level >= f.levels())
        return false;
    const uint32_t w = f.width >> level ? f.width >> level : 1, h = f.height >> level ? f.height >> level : 1;
    out.resize(static_cast<size_t>(w) * h);
    auto u16 = [vram](uint32_t a) { return static_cast<uint32_t>(vram[a] | vram[a + 1] << 8); };
    const uint8_t* base = vram + f.address;
    if (f.vq) {
        const uint32_t indices = f.address + 2048 + level_offset(f, level);
        for (uint32_t y = 0; y < h; ++y)
            for (uint32_t x = 0; x < w; ++x) {
                // A level of side 1 still has a 2x2 block, of which its texel is the first.
                const uint32_t bw = w > 1 ? w / 2 : 1, bh = h > 1 ? h / 2 : 1;
                const uint32_t entry = vram[indices + twiddled(x / 2, y / 2, bw, bh)];
                const uint32_t texel = (x & 1) << 1 | (y & 1);
                out[y * w + x] = from16(u16(f.address + entry * 8 + texel * 2), f.pixel);
            }
        return true;
    }
    const uint32_t start = level_offset(f, level);
    for (uint32_t y = 0; y < h; ++y)
        for (uint32_t x = 0; x < w; ++x) {
            const uint32_t t = f.twiddled ? twiddled(x, y, w, h) : y * f.row_texels + x;
            uint32_t& px = out[y * w + x];
            switch (f.pixel) {
                case PixelFormat::Palette4: {
                    const uint8_t b = base[start + t / 2];
                    px = palette[f.palette_base + (t & 1 ? b >> 4 : b & 15)];
                    break;
                }
                case PixelFormat::Palette8:
                    px = palette[f.palette_base + base[start + t]];
                    break;
                default:
                    px = from16(u16(f.address + start + t * 2), f.pixel);
            }
        }
    return true;
}

void unpack_palette(const uint32_t* ram, uint32_t format, uint32_t* out) {
    for (int i = 0; i < 1024; ++i) {
        const uint32_t p = ram[i];
        switch (format & 3) {
            case 0: out[i] = from16(p & 0xFFFF, PixelFormat::Argb1555); break;
            case 1: out[i] = from16(p & 0xFFFF, PixelFormat::Rgb565); break;
            case 2: out[i] = from16(p & 0xFFFF, PixelFormat::Argb4444); break;
            default: out[i] = rgba((p >> 16) & 255, (p >> 8) & 255, p & 255, p >> 24);
        }
    }
}

}  // namespace render
