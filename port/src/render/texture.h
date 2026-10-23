// Textures in the PowerVR's video memory, decoded to RGBA8 for the GPU. The texture control word
// gives the address and the encoding, and the TSP instruction word the size. An encoding combines
// a pixel format, a layout (twiddled, in a Morton order, or in rows), vector quantization (a
// codebook of 2x2 blocks and one index byte per block) and mipmaps (the smaller levels before the
// full one). Ikaruga uses ARGB1555, RGB565, ARGB4444 and 4- and 8-bit palettes, but no YUV422 and
// no bump maps.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace render {

enum class PixelFormat : unsigned { Argb1555, Rgb565, Argb4444, Yuv422, BumpMap, Palette4, Palette8, Reserved };

struct TextureFormat {
    uint32_t address = 0;  // a byte offset in the 64-bit view of video memory
    uint32_t width = 8, height = 8;
    PixelFormat pixel = PixelFormat::Argb1555;
    bool twiddled = true, vq = false, mipmapped = false;
    uint32_t row_texels = 0;      // a texture in rows: the texels from one row to the next
    uint32_t palette_base = 0;    // a palette texture: its first palette entry
    uint32_t levels() const;      // the mipmap levels, the full size first
};

// `stride_texels` is the row length from the TEXT_CONTROL register, which a texture in rows with
// the stride bit uses. False for an encoding the decoder does not handle.
bool describe_texture(uint32_t tcw, uint32_t tsp, uint32_t stride_texels, TextureFormat& out);

// Decodes one level, level 0 being the full size, as RGBA8 with red in the lowest byte and the top
// row first. `palette` is the palette memory as RGBA8 (unpack_palette). False when the texture lies
// outside video memory.
bool decode_texture(const TextureFormat& format, uint32_t level, const uint8_t* vram, size_t vram_size,
                    const uint32_t* palette, std::vector<uint32_t>& out);

// The bytes the texture occupies in video memory, with every level and the codebook.
void texture_extent(const TextureFormat& format, uint32_t& first, uint32_t& size);

// The 1024 words of palette memory, in the format PAL_RAM_CTRL names, as RGBA8.
void unpack_palette(const uint32_t* palette_ram, uint32_t palette_format, uint32_t* out);

}  // namespace render
