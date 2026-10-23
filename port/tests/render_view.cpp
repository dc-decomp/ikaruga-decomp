// Draws a display list dump from IKARUGA_DUMP_TA with the port's renderer, without a window, and
// writes the picture as a PNG.
//   render-view DUMP OUT.png [--scale N] [--list]
// --list prints each strip that draws, with its list, words and vertex count.
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <zlib.h>

#include "render/gpu.h"
#include "render/menu.h"
#include "render/overlay.h"
#include "render/ta.h"
#include "render/texture.h"

namespace {

std::vector<uint8_t> read_file(const std::string& path) {
    std::vector<uint8_t> data;
    if (std::FILE* f = std::fopen(path.c_str(), "rb")) {
        uint8_t buffer[1 << 16];
        size_t n;
        while ((n = std::fread(buffer, 1, sizeof buffer, f)) > 0) data.insert(data.end(), buffer, buffer + n);
        std::fclose(f);
    }
    return data;
}

void chunk(std::FILE* f, const char* tag, const std::vector<uint8_t>& body) {
    auto be = [f](uint32_t v) {
        const uint8_t b[4] = {static_cast<uint8_t>(v >> 24), static_cast<uint8_t>(v >> 16), static_cast<uint8_t>(v >> 8),
                              static_cast<uint8_t>(v)};
        std::fwrite(b, 1, 4, f);
    };
    be(static_cast<uint32_t>(body.size()));
    std::fwrite(tag, 1, 4, f);
    std::fwrite(body.data(), 1, body.size(), f);
    uLong crc = crc32(0, reinterpret_cast<const Bytef*>(tag), 4);
    crc = crc32(crc, body.data(), static_cast<uInt>(body.size()));
    be(static_cast<uint32_t>(crc));
}

bool write_png(const std::string& path, const std::vector<uint32_t>& rgba, uint32_t w, uint32_t h) {
    std::vector<uint8_t> raw;
    for (uint32_t y = 0; y < h; ++y) {
        raw.push_back(0);
        for (uint32_t x = 0; x < w; ++x) {
            const uint32_t p = rgba[static_cast<size_t>(y) * w + x];
            raw.push_back(static_cast<uint8_t>(p));
            raw.push_back(static_cast<uint8_t>(p >> 8));
            raw.push_back(static_cast<uint8_t>(p >> 16));
        }
    }
    uLongf size = compressBound(static_cast<uLong>(raw.size()));
    std::vector<uint8_t> packed(size);
    compress(packed.data(), &size, raw.data(), static_cast<uLong>(raw.size()));
    packed.resize(size);
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f)
        return false;
    std::fwrite("\x89PNG\r\n\x1a\n", 1, 8, f);
    std::vector<uint8_t> ihdr = {static_cast<uint8_t>(w >> 24), static_cast<uint8_t>(w >> 16), static_cast<uint8_t>(w >> 8),
                                 static_cast<uint8_t>(w), static_cast<uint8_t>(h >> 24), static_cast<uint8_t>(h >> 16),
                                 static_cast<uint8_t>(h >> 8), static_cast<uint8_t>(h), 8, 2, 0, 0, 0};
    chunk(f, "IHDR", ihdr);
    chunk(f, "IDAT", packed);
    chunk(f, "IEND", {});
    std::fclose(f);
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    // --menu OUT.png: the controller screen with the default bindings, over black, at 1280x960.
    if (argc == 3 && !std::strcmp(argv[1], "--menu")) {
        render::Menu menu;
        menu.open(render::Bindings::defaults());
        render::Overlay overlay;
        overlay.begin(1280, 960);
        menu.draw(overlay, 1280, 960);
        std::vector<uint32_t> rgb(overlay.pixels());
        for (uint32_t& p : rgb) p |= 0xFF000000u;  // premultiplied over black
        return write_png(argv[2], rgb, 1280, 960) ? 0 : 1;
    }
    if (argc < 3) {
        std::fprintf(stderr, "usage: render-view DUMP OUT.png [--scale N] [--list]\n");
        return 2;
    }
    const std::string dump = argv[1], out = argv[2];
    unsigned scale = 1;
    bool list = false;
    for (int i = 3; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--scale") && i + 1 < argc)
            scale = static_cast<unsigned>(std::atoi(argv[++i]));
        else if (!std::strcmp(argv[i], "--list"))
            list = true;
    }
    const std::vector<uint8_t> stream = read_file(dump), vram = read_file(dump + ".vram"),
                               regs = read_file(dump + ".vram.regs");
    if (stream.empty() || vram.empty() || regs.size() < 0x2000) {
        std::fprintf(stderr, "%s: the dump, its .vram or its .vram.regs is missing\n", dump.c_str());
        return 1;
    }
    std::vector<uint32_t> words(stream.size() / 4), registers(regs.size() / 4);
    std::memcpy(words.data(), stream.data(), words.size() * 4);
    std::memcpy(registers.data(), regs.data(), registers.size() * 4);
    render::TaDecoder decoder;
    decoder.reset();
    decoder.feed(words.data(), words.size());
    // --skip LIST:INDEX leaves a strip out, LIST being the list's number (0 opaque, 2 translucent).
    render::DisplayList dl = decoder.list();
    std::vector<std::pair<unsigned, size_t>> skip;
    for (int i = 3; i + 1 < argc; ++i)
        if (!std::strcmp(argv[i], "--skip")) {
            unsigned l = 0;
            size_t index = 0;
            if (std::sscanf(argv[i + 1], "%u:%zu", &l, &index) == 2 && l < render::kLists)
                skip.push_back({l, index});
        }
    std::sort(skip.rbegin(), skip.rend());
    for (const auto& [l, index] : skip)
        if (index < dl.lists[l].size())
            dl.lists[l].erase(dl.lists[l].begin() + static_cast<std::ptrdiff_t>(index));
    if (list) {
        const char* names[] = {"opaque", "opaque modifier", "translucent", "translucent modifier", "punch-through"};
        for (unsigned l = 0; l < render::kLists; ++l)
            for (size_t i = 0; i < dl.lists[l].size(); ++i) {
                const render::Strip& s = dl.lists[l][i];
                const render::Vertex& v = dl.vertices[s.first];
                float x0 = v.x, x1 = v.x, y0 = v.y, y1 = v.y;
                for (uint32_t k = 1; k < s.count; ++k) {
                    const render::Vertex& q = dl.vertices[s.first + k];
                    x0 = std::min(x0, q.x), x1 = std::max(x1, q.x), y0 = std::min(y0, q.y), y1 = std::max(y1, q.y);
                }
                std::printf("%s %zu: pcw %08x isp %08x tsp %08x tcw %08x, %u vertices in (%.1f, %.1f)-(%.1f, %.1f) "
                            "base %08x offset %08x clip %u %u,%u-%u,%u%s\n",
                            names[l], i, s.pcw, s.isp, s.tsp, s.tcw, s.count, x0, y0, x1, y1, v.base, v.offset, s.clip_mode,
                            s.clip_x0, s.clip_y0, s.clip_x1, s.clip_y1,
                            s.sprite ? " sprite" : "");
                if (std::getenv("RENDER_VIEW_VERTICES"))
                    for (uint32_t k = 0; k < s.count; ++k) {
                        const render::Vertex& q = dl.vertices[s.first + k];
                        std::printf("    %.2f %.2f z %g uv %g %g base %08x offset %08x\n", q.x, q.y, q.z, q.u, q.v, q.base,
                                    q.offset);
                    }
            }
    }
    render::Gpu gpu;
    std::string error;
    if (!gpu.create(nullptr, scale, error)) {
        std::fprintf(stderr, "render: %s\n", error.c_str());
        return 1;
    }
    const render::Hardware hw{registers.data(), vram.data(), vram.size()};
    gpu.render(dl, hw);
    // Again, with the textures decoded, for the GPU time of a render alone.
    gpu.render(dl, hw);
    double gpu_ms = 0;
    uint32_t fragments = 0;
    gpu.last_render(gpu_ms, fragments);
    std::printf("render: %.2f ms on the GPU, %u translucent fragments\n", gpu_ms, fragments);
    std::vector<uint32_t> pixels;
    uint32_t w = 0, h = 0;
    if (!gpu.read_target(registers[0x060 / 4] & 0xFFFFFF, pixels, w, h) || !write_png(out, pixels, w, h)) {
        std::fprintf(stderr, "render: no picture\n");
        return 1;
    }
    // --texel TCW TSP X Y: a texel of a texture as the GPU holds it after the render.
    for (int i = 3; i + 4 < argc; ++i)
        if (!std::strcmp(argv[i], "--texel")) {
            const uint32_t tcw = std::strtoul(argv[i + 1], nullptr, 16), tsp = std::strtoul(argv[i + 2], nullptr, 16);
            std::vector<uint32_t> texels;
            uint32_t tw = 0, th = 0;
            if (gpu.read_texture(tcw, tsp, registers[0x108 / 4], texels, tw, th)) {
                const uint32_t x = std::strtoul(argv[i + 3], nullptr, 0), y = std::strtoul(argv[i + 4], nullptr, 0);
                std::printf("texture %08x %ux%u: texel (%u, %u) %08x\n", tcw, tw, th, x, y, texels[y * tw + x]);
                render::TextureFormat f;
                uint32_t palette[1024];
                render::unpack_palette(registers.data() + 0x1000 / 4, registers[0x108 / 4], palette);
                std::vector<uint32_t> cpu;
                if (render::describe_texture(tcw, tsp, (registers[0xE4 / 4] & 31) * 32, f) &&
                    render::decode_texture(f, 0, vram.data(), vram.size(), palette, cpu)) {
                    size_t differ = 0, nonzero = 0, first = cpu.size();
                    for (size_t k = 0; k < cpu.size() && k < texels.size(); ++k) {
                        nonzero += cpu[k] != 0;
                        if (cpu[k] != texels[k] && differ++ == 0)
                            first = k;
                    }
                    // The texture over a checkerboard, so its alpha shows: written next to the output.
                    std::vector<uint32_t> shown(cpu.size());
                    for (size_t k = 0; k < cpu.size(); ++k) {
                        const uint32_t c = cpu[k], a = c >> 24, x = k % f.width, y = k / f.width;
                        const uint32_t board = ((x / 8 + y / 8) & 1) ? 0x60 : 0xA0;
                        uint32_t o = 0;
                        for (int ch = 0; ch < 3; ++ch) {
                            const uint32_t v = ((c >> (8 * ch)) & 255) * a / 255 + board * (255 - a) / 255;
                            o |= v << (8 * ch);
                        }
                        shown[k] = o;
                    }
                    char suffix[16];
                    std::snprintf(suffix, sizeof suffix, ".%08x.png", tcw);
                    write_png(out + suffix, shown, f.width, f.height);
                    std::printf("  CPU decode: %zu texels not zero, %zu differ from the GPU's", nonzero, differ);
                    if (differ)
                        std::printf(", first at %zu: CPU %08x GPU %08x", first, cpu[first], texels[first]);
                    std::printf("\n");
                }
            }
            else {
                std::printf("texture %08x: not in the cache\n", tcw);
            }
        }
    std::printf("%s: %ux%u, %llu textures decoded, %llu strips without their texture\n", out.c_str(), w, h,
                static_cast<unsigned long long>(gpu.textures_decoded), static_cast<unsigned long long>(gpu.untextured));
    return 0;
}
