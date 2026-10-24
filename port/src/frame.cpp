#include "frame.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <utility>

namespace frame {
namespace {

// The lists' ranges: 4 MB of store-queue addresses each from 0xE2000000, where nothing else in the
// game flushes. A frame sends at most a few hundred kilobytes to a list.
constexpr uint32_t kBase = 0xE2000000u, kSpan = 0x00400000u;

Lists lists_;

}  // namespace

void begin() {
    for (auto& l : lists_.list) l.clear();
}

uint32_t start(unsigned n) { return kBase + kSpan * n; }

bool in_lists(uint32_t address) { return address >= kBase && address < kBase + kSpan * render::kLists; }

void flush(uint32_t address, const uint8_t* words) {
    const unsigned n = (address - kBase) / kSpan;
    std::vector<uint32_t>& l = lists_.list[n];
    // Ninja's pointer runs on through the list's range, so a parameter lands at its offset.
    if ((address & ~0x1Fu) - start(n) != l.size() * 4) {
        std::fprintf(stderr, "frame: list %u's parameter at %08x after %zu bytes\n", n, address, l.size() * 4);
        std::abort();
    }
    l.resize(l.size() + 8);
    std::memcpy(&l[l.size() - 8], words, 32);
}

Lists take() { return std::exchange(lists_, Lists{}); }

std::vector<uint32_t> stream(const Lists& lists) {
    std::vector<uint32_t> out;
    for (const auto& l : lists.list) {
        if (l.empty())
            continue;
        out.insert(out.end(), l.begin(), l.end());
        // Kamui closes a list with a parameter of type 0. The list that went to the tile
        // accelerator directly got its end by a transfer instead, so add one where none came.
        if (l[l.size() - 8] >> 29 != 0)
            out.insert(out.end(), 8, 0u);
    }
    return out;
}

uint64_t hash(const render::DisplayList& list) {
    uint64_t h = 0xCBF29CE484222325ull;
    const auto add = [&h](const void* data, size_t bytes) {
        const auto* p = static_cast<const uint8_t*>(data);
        for (size_t i = 0; i < bytes; ++i) h = (h ^ p[i]) * 0x100000001B3ull;
    };
    for (unsigned n = 0; n < render::kLists; ++n) {
        add(&n, sizeof n);
        for (const render::Strip& s : list.lists[n]) {
            const uint32_t state[] = {s.count, s.pcw, s.isp, s.tsp, s.tcw, s.tsp1, s.tcw1, s.clip_mode,
                                      s.clip_x0, s.clip_y0, s.clip_x1, s.clip_y1, s.sprite};
            add(state, sizeof state);
            add(&list.vertices[s.first], s.count * sizeof(render::Vertex));
        }
    }
    add(list.modifiers.data(), list.modifiers.size() * sizeof(render::ModifierTriangle));
    return h;
}

}  // namespace frame
