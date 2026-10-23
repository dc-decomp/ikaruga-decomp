// Decodes display lists with the port's decoder (port/src/render) and with dream-recomp's, and
// prints where they differ. dream-recomp is the reference here only: the port's renderer does not
// link it. Each argument is a dump that IKARUGA_DUMP_TA wrote.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <type_traits>
#include <vector>

#include "dream/render/display_list.h"
#include "dream/render/texture.h"
#include "render/ta.h"
#include "render/texture.h"

namespace {

std::vector<uint32_t> read_words(const std::string& path) {
    std::vector<uint32_t> words;
    if (std::FILE* f = std::fopen(path.c_str(), "rb")) {
        uint32_t w;
        while (std::fread(&w, 4, 1, f) == 1) words.push_back(w);
        std::fclose(f);
    }
    return words;
}

uint32_t bits(float f) {
    uint32_t b;
    std::memcpy(&b, &f, 4);
    return b;
}

int shown = 0;
void report(const std::string& what) {
    if (shown++ < 20)
        std::printf("  %s\n", what.c_str());
}

int compare(const std::string& path) {
    const std::vector<uint32_t> words = read_words(path);
    render::TaDecoder ours;
    ours.reset();
    ours.feed(words.data(), words.size());
    dream::render::DisplayList theirs;
    theirs.feed_stream(words.data(), words.size());
    const render::DisplayList& a = ours.list();
    const dream::render::Frame& b = theirs.frame();
    shown = 0;
    int differences = 0;
    // Strips that compare depth with "never" draw nothing. Kamui opens each list with some, whose
    // list-type field says 0: the hardware files them in the open list, the reference by the field.
    auto drawn = [](const auto& strips) {
        std::vector<std::decay_t<decltype(strips[0])>> out;
        for (const auto& s : strips)
            if (s.isp >> 29)
                out.push_back(s);
        return out;
    };
    size_t never = 0;
    for (unsigned l = 0; l < render::kLists; ++l) never += a.lists[l].size() - drawn(a.lists[l]).size();
    for (unsigned l = 0; l < render::kLists; ++l) {
        const auto sa = drawn(a.lists[l]);
        const auto sb = drawn(b.lists[l]);
        if (sa.size() != sb.size()) {
            report("list " + std::to_string(l) + ": " + std::to_string(sa.size()) + " strips, reference " +
                   std::to_string(sb.size()));
            ++differences;
        }
        for (size_t i = 0; i < std::min(sa.size(), sb.size()); ++i) {
            const render::Strip& x = sa[i];
            const dream::render::Polygon& y = sb[i];
            char line[256];
            if (x.count != y.count || x.pcw != y.pcw || x.isp != y.isp || x.tsp != y.tsp || x.tcw != y.tcw ||
                x.tsp1 != y.tsp1 || x.tcw1 != y.tcw1 || x.clip_mode != y.tile_clip ||
                (x.clip_mode && (x.clip_x0 != y.clip_x0 || x.clip_y0 != y.clip_y0 || x.clip_x1 != y.clip_x1 ||
                                 x.clip_y1 != y.clip_y1))) {
                std::snprintf(line, sizeof line,
                              "list %u strip %zu: count %u/%u pcw %08x/%08x isp %08x/%08x tsp %08x/%08x tcw %08x/%08x "
                              "clip %u %u,%u-%u,%u / %u %u,%u-%u,%u tsp1 %08x/%08x",
                              l, i, x.count, y.count, x.pcw, y.pcw, x.isp, y.isp, x.tsp, y.tsp, x.tcw, y.tcw, x.clip_mode,
                              x.clip_x0, x.clip_y0, x.clip_x1, x.clip_y1, y.tile_clip, y.clip_x0, y.clip_y0, y.clip_x1,
                              y.clip_y1, x.tsp1, y.tsp1);
                report(line);
                ++differences;
                continue;
            }
            for (uint32_t k = 0; k < x.count; ++k) {
                const render::Vertex& p = a.vertices[x.first + k];
                const dream::render::Vertex& q = b.vertices[y.first + k];
                if (bits(p.x) != bits(q.x) || bits(p.y) != bits(q.y) || bits(p.z) != bits(q.z) ||
                    bits(p.u) != bits(q.u) || bits(p.v) != bits(q.v) || p.base != q.base || p.offset != q.offset) {
                    std::snprintf(line, sizeof line,
                                  "list %u strip %zu pcw %08x tsp %08x%s vertex %u: xyz %g %g %g / %g %g %g uv %g %g / %g %g "
                                  "base %08x/%08x offset %08x/%08x",
                                  l, i, x.pcw, x.tsp, x.sprite ? " (sprite)" : "", k, p.x, p.y, p.z, q.x, q.y, q.z, p.u, p.v, q.u,
                                  q.v, p.base, q.base, p.offset, q.offset);
                    report(line);
                    ++differences;
                }
            }
        }
    }
    if (a.modifiers.size() != b.modifiers.size()) {
        report("modifier triangles " + std::to_string(a.modifiers.size()) + ", reference " +
               std::to_string(b.modifiers.size()));
        ++differences;
    }
    // Every texture the list uses, decoded at full size by both.
    std::vector<uint8_t> vram;
    if (std::FILE* f = std::fopen((path + ".vram").c_str(), "rb")) {
        vram.resize(8u << 20);
        vram.resize(std::fread(vram.data(), 1, vram.size(), f));
        std::fclose(f);
    }
    const std::vector<uint32_t> regs = read_words(path + ".vram.regs");
    int textures = 0, texture_differences = 0, unsupported = 0;
    if (!vram.empty() && regs.size() >= 0x2000 / 4) {
        uint32_t ours_palette[1024], theirs_palette[1024];
        render::unpack_palette(&regs[0x1000 / 4], regs[0x108 / 4], ours_palette);
        dream::render::unpack_palette(&regs[0x1000 / 4], static_cast<dream::render::PaletteFormat>(regs[0x108 / 4] & 3),
                                      theirs_palette);
        const uint32_t stride = (regs[0xE4 / 4] & 31) * 32;
        std::vector<std::pair<uint32_t, uint32_t>> seen;
        for (const auto& list : a.lists)
            for (const render::Strip& s : list) {
                if (!(s.pcw & 8) || !(s.isp >> 29))
                    continue;
                const std::pair<uint32_t, uint32_t> key{s.tcw, s.tsp & 0x3F};
                if (std::find(seen.begin(), seen.end(), key) != seen.end())
                    continue;
                seen.push_back(key);
                render::TextureFormat tf;
                dream::render::TextureInfo ti;
                if (!render::describe_texture(s.tcw, s.tsp, stride, tf)) {
                    ++unsupported;
                    continue;
                }
                std::vector<uint32_t> mine, ref;
                const bool ok = render::decode_texture(tf, 0, vram.data(), vram.size(), ours_palette, mine);
                const bool ref_ok = dream::render::describe_texture(s.tcw, s.tsp, stride, ti) &&
                                    dream::render::decode_texture(ti, vram.data(), vram.size(), theirs_palette, ref);
                ++textures;
                if (ok != ref_ok || mine != ref) {
                    size_t first = 0;
                    while (first < mine.size() && first < ref.size() && mine[first] == ref[first]) ++first;
                    char line[200];
                    std::snprintf(line, sizeof line, "texture tcw %08x tsp %08x %ux%u: decoded %d/%d, texel %zu %08x/%08x",
                                  s.tcw, s.tsp, tf.width, tf.height, ok, ref_ok, first,
                                  first < mine.size() ? mine[first] : 0, first < ref.size() ? ref[first] : 0);
                    report(line);
                    ++texture_differences;
                }
            }
    }
    differences += texture_differences;
    std::printf("%s: %d textures, %d differ, %d unsupported\n", path.c_str(), textures, texture_differences,
                unsupported);
    std::printf("%s: %zu vertices, %zu strips that never draw, %d differences%s\n", path.c_str(), a.vertices.size(),
                never, differences,
                a.unknown_parameters ? " (unknown parameters)" : "");
    return differences;
}

}  // namespace

int main(int argc, char** argv) {
    int bad = 0;
    for (int i = 1; i < argc; ++i) bad += compare(argv[i]) ? 1 : 0;
    return bad ? 1 : 0;
}
