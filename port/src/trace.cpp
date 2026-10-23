// The record of decompiled functions entered (port.h, TRACE_FUNCTION).
#include <cstdio>
#include <cstdlib>
#include <map>

#include "port.h"

namespace trace {

namespace {
struct Entry {
    const char* name;
    int depth;
};
constexpr size_t kSize = 1 << 16;
Entry ring[kSize];
size_t next = 0;
int depth = 0;
const char* stack[4096];
}  // namespace

std::map<const char*, uint64_t>& counts() {
    static std::map<const char*, uint64_t> m;
    return m;
}
bool counting = std::getenv("IKARUGA_COUNT_FILE") != nullptr;

void print(std::FILE* out, size_t last);

// IKARUGA_WATCH_POINTERS=ADDR,N checks N words from ADDR at every function boundary and reports
// the first function after which one of them stopped looking like a pointer into RAM or the store
// queues: how a corrupted pointer was found.
namespace {
uint32_t watch_addr = 0, watch_words = 0;
bool watch_init = [] {
    if (const char* w = std::getenv("IKARUGA_WATCH_POINTERS")) {
        char* end;
        watch_addr = static_cast<uint32_t>(std::strtoul(w, &end, 0));
        watch_words = *end == ',' ? static_cast<uint32_t>(std::strtoul(end + 1, nullptr, 0)) : 1;
    }
    return true;
}();
bool pointer_like(uint32_t v) {
    const uint32_t seg = v >> 24;
    return (seg & 0x1F) == 0x0C || seg == 0xE0 || seg == 0xE1;
}
void watch_check(const char* where, bool leaving) {
    static uint32_t last[64];
    static bool reported = false;
    for (uint32_t i = 0; i < watch_words && i < 64 && !reported; ++i) {
        const uint32_t v = guest::load<uint32_t>(watch_addr + 4 * i);
        if (pointer_like(last[i]) && !pointer_like(v)) {
            reported = true;
            std::fprintf(stderr, "watch: %08x went from %08x to %08x %s %s\n", watch_addr + 4 * i, last[i], v,
                         leaving ? "after leaving" : "on entering", where);
            print(stderr, 48);
        }
        last[i] = v;
    }
}
}  // namespace

Scope::Scope(const char* name) {
    if (watch_addr)
        watch_check(name, false);
    if (counting)
        ++counts()[name];
    ring[next++ & (kSize - 1)] = {name, depth};
    if (depth < 4096)
        stack[depth] = name;
    ++depth;
}
Scope::~Scope() {
    --depth;
    if (watch_addr)
        watch_check(stack[depth < 4096 ? depth : 4095], true);
}

void print_stack(std::FILE* out) {
    std::fprintf(out, "functions active now, outermost first:\n");
    for (int i = 0; i < depth && i < 4096; ++i) std::fprintf(out, "  %*s%s\n", i > 40 ? 40 : i, "", stack[i]);
}

const char* current() { return next ? ring[(next - 1) & (kSize - 1)].name : "?"; }

void print(std::FILE* out, size_t last) {
    const size_t n = next < last ? next : last;
    std::fprintf(out, "last %zu of %zu functions entered:\n", n, next);
    for (size_t i = next - n; i < next; ++i) {
        const Entry& e = ring[i & (kSize - 1)];
        std::fprintf(out, "  %*s%s\n", e.depth > 40 ? 40 : e.depth, "", e.name);
    }
}

void dump_counts(const char* path) {
    std::FILE* f = std::fopen(path, "w");
    if (!f)
        return;
    for (const auto& [n, c] : counts()) std::fprintf(f, "%s %llu\n", n, static_cast<unsigned long long>(c));
    std::fclose(f);
}

void dump(const char* path) {
    std::FILE* f = std::fopen(path, "w");
    if (!f)
        return;
    const size_t n = next < kSize ? next : kSize;
    for (size_t i = next - n; i < next; ++i) {
        const Entry& e = ring[i & (kSize - 1)];
        std::fprintf(f, "%d %s\n", e.depth, e.name);
    }
    std::fclose(f);
}

}  // namespace trace
