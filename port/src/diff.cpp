// Differential tests of the decompiled functions against the machine code (diff.h).
#include "diff.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

#include "hw.h"
#include "port.h"
#include "sh4/sh4.h"

namespace diff {

bool g_on = false;
uint32_t g_log_addresses[16] = {};
int g_log_count = [] {
    int n = 0;
    if (const char* v = std::getenv("IKARUGA_LOG_CALLS"))
        for (const char* p = v; *p && n < 16;) {
            char* end = nullptr;
            g_log_addresses[n++] = static_cast<uint32_t>(std::strtoul(p, &end, 0));
            if (end == p)
                break;
            p = *end == ',' ? end + 1 : end;
        }
    return n;
}();
int g_shadow = 0;
bool g_testing = false;
uint64_t g_hw_accesses = 0;

namespace {

// The stack grows down from 0x8C00F000. Below the caller's stack pointer, the interpreter and the
// C++ keep different frames, which the comparison skips.
constexpr uint32_t kStackLow = 0x8C004000u;
constexpr uint64_t kAccessBudget = 100'000;
constexpr double kSecondsBudget = 0.25;
// Calls during which a hardware event happened before a function counts as untestable.
constexpr int kInconclusiveLimit = 8;
// Shadows abandoned before a function counts as untestable.
constexpr int kAbandonLimit = 4;

struct Registers {
    float xf[16];
    uint32_t sr, gbr, fpul, fpscr, macl, mach;
    void save() {
        std::memcpy(xf, cpu::xf, sizeof xf);
        sr = cpu::sr;
        gbr = cpu::gbr;
        fpul = cpu::fpul;
        fpscr = cpu::fpscr;
        macl = cpu::macl;
        mach = cpu::mach;
    }
    void load() const {
        std::memcpy(cpu::xf, xf, sizeof xf);
        cpu::sr = sr;
        cpu::gbr = gbr;
        cpu::fpul = fpul;
        cpu::set_fpscr(fpscr);
        cpu::macl = macl;
        cpu::mach = mach;
    }
};

struct Function {
    const char* name = nullptr;
    int tested = 0, inconclusive = 0, abandoned = 0;
    bool failed = false, hardware = false;
    const char* aborted = nullptr;
};

int g_calls = 1;  // conclusive tests per function
const auto g_process_start = std::chrono::steady_clock::now();
bool g_verbose = false;  // IKARUGA_DIFF_VERBOSE: each shadow with its time
std::vector<uint32_t> g_only;  // IKARUGA_DIFF_ONLY: the functions to test, or all
uint64_t g_from = 0;  // IKARUGA_DIFF_FROM: the first frame with tests
std::unordered_map<uint32_t, Function> g_functions;

// The test in progress.
bool g_active = false;
uint32_t g_address = 0, g_entry_sp = 0;
CallArgs g_args;

// A saved stack pointer differs between the two runs: the interpreter and the C++ keep their
// frames at different depths.
bool stack_address(uint32_t v) { return v >= kStackLow && v < 0x8C00F000u; }

struct Call {
    const char* name;
    uint32_t address;  // 0 for a C++ function, named by `name`
    bool placed;
    CallArgs args;
    // The first bytes behind each argument register that holds a RAM address.
    uint8_t mem[4][128];
    uint8_t has_mem;
};
constexpr size_t kCallLog = 4096;
std::vector<Call> g_shadow_calls, g_native_calls;

struct Flush {
    uint32_t destination;
    uint32_t words[8];
    bool operator==(const Flush& o) const {
        return destination == o.destination && std::memcmp(words, o.words, 32) == 0;
    }
};
std::vector<Flush> g_shadow_ta, g_native_ta;
bool g_skip_next_run = false;  // the shadow's own entry into the interpreter

bool ram_address(uint32_t v) { return (v & 0x1F000000u) == 0x0C000000u; }

bool same_word(uint32_t a, uint32_t b) { return a == b || (stack_address(a) && stack_address(b)); }

// `b` is the C++ run's record: its counts say which registers hold arguments.
bool same_args(const Call& a, const Call& b) {
    if (a.name != b.name || a.address != b.address || a.placed != b.placed)
        return false;
    if (!a.placed)
        return true;
    for (int i = 0; i < b.args.nr && i < 4; ++i)
        if (!same_word(a.args.r[i], b.args.r[i]))
            return false;
    for (int i = 0; i < b.args.nstack && i < 16; ++i)
        if (!same_word(a.args.stack[i], b.args.stack[i]))
            return false;
    for (int i = 0; i < b.args.nfr && i < 8; ++i)
        if (std::memcmp(&a.args.fr[i], &b.args.fr[i], 4) != 0)
            return false;
    for (int i = 0; i < b.args.nr && i < 4; ++i)
        if ((a.has_mem & b.has_mem & (1 << i)) && std::memcmp(a.mem[i], b.mem[i], 128) != 0)
            return false;
    return true;
}

void print_call(const char* who, size_t i, const Call& c, const Call& counts) {
    std::fprintf(stderr, "      %s call %zu: ", who, i);
    if (c.address)
        std::fprintf(stderr, "machine code at %08x", c.address);
    else
        std::fprintf(stderr, "%s", c.name);
    if (c.placed) {
        for (int k = 0; k < counts.args.nr && k < 4; ++k)
            std::fprintf(stderr, " r%d=%08x", 4 + k, c.args.r[k]);
        for (int k = 0; k < counts.args.nfr && k < 8; ++k)
            std::fprintf(stderr, " fr%d=%g", 4 + k, c.args.fr[k]);
        for (int k = 0; k < counts.args.nstack && k < 16; ++k)
            std::fprintf(stderr, " stack%d=%08x", k, c.args.stack[k]);
    }
    std::fprintf(stderr, "\n");
    for (int k = 0; k < counts.args.nr && k < 4; ++k)
        if (c.has_mem & (1 << k)) {
            std::fprintf(stderr, "        at r%d:", 4 + k);
            for (int w = 0; w < 32; ++w) {
                uint32_t v;
                std::memcpy(&v, c.mem[k] + 4 * w, 4);
                std::fprintf(stderr, " %08x", v);
            }
            std::fprintf(stderr, "\n");
        }
}

void print_first_different_call() {
    const size_t n = std::min(g_shadow_calls.size(), g_native_calls.size());
    for (size_t i = 0; i < n; ++i)
        if (!same_args(g_shadow_calls[i], g_native_calls[i])) {
            std::fprintf(stderr, "    the calls differ first at call %zu:\n", i);
            print_call("machine code", i, g_shadow_calls[i], g_native_calls[i]);
            print_call("C++", i, g_native_calls[i], g_native_calls[i]);
            return;
        }
    if (g_shadow_calls.size() != g_native_calls.size())
        std::fprintf(stderr, "    the machine code makes %zu calls, the C++ %zu\n", g_shadow_calls.size(),
                     g_native_calls.size());
    else
        std::fprintf(stderr, "    the %zu calls are the same\n", n);
}

void record(const char* name, uint32_t address, const CallArgs* args) {
    std::vector<Call>& log = g_shadow ? g_shadow_calls : g_native_calls;
    if (log.size() >= kCallLog)
        return;
    log.emplace_back();
    Call& c = log.back();
    c.name = name;
    c.address = address;
    c.placed = args != nullptr;
    c.args = args ? *args : CallArgs{};
    c.has_mem = 0;
    // Only for the interpreted functions, where vertex arrays go; a C++ callee is tested on its own,
    // and the memory behind an output pointer is not set yet.
    if (!args || !address)
        return;
    for (int k = 0; k < 4; ++k)
        if (ram_address(args->r[k]) && (args->r[k] & 0x00FFFFFFu) + 128 <= guest::kRamSize) {
            std::memcpy(c.mem[k], guest::ram_at(args->r[k]), 128);
            c.has_mem |= 1 << k;
        }
}

uint64_t g_activity = 0;
uint64_t g_accesses = 0;
std::chrono::steady_clock::time_point g_started;
Registers g_expected_regs;
std::vector<uint8_t> g_before_ram, g_before_vram, g_before_aram, g_expected_ram, g_expected_vram;
uint8_t g_before_sq[64];

uint64_t activity() {
    const hw::Stats& s = hw::stats();
    return s.interrupts + s.events;
}

struct Init {
    Init() {
        const char* v = std::getenv("IKARUGA_DIFF");
        if (!v)
            return;
        g_on = true;
        g_calls = std::max(1, std::atoi(v));
        g_verbose = std::getenv("IKARUGA_DIFF_VERBOSE") != nullptr;
        if (const char* o = std::getenv("IKARUGA_DIFF_ONLY"))
            for (const char* q = o; *q;) {
                char* end = nullptr;
                const auto address = static_cast<uint32_t>(std::strtoul(q, &end, 0));
                if (end == q)
                    break;
                g_only.push_back(address);
                q = *end == ',' ? end + 1 : end;
            }
        if (const char* f = std::getenv("IKARUGA_DIFF_FROM"))
            g_from = std::strtoull(f, nullptr, 0);
        g_before_ram.resize(guest::kRamSize);
        g_expected_ram.resize(guest::kRamSize);
        g_before_vram.resize(hw::kVramSize);
        g_expected_vram.resize(hw::kVramSize);
        g_before_aram.resize(hw::kAramSize);
    }
} init;

// Counts the words that differ, and prints the first ones if `shown` allows.
void compare_words(const char* area, uint32_t base, const uint8_t* expected, const uint8_t* got, size_t size,
                   int& shown, size_t& count) {
    for (size_t o = 0; o + 4 <= size; o += 4) {
        if (std::memcmp(expected + o, got + o, 4) == 0)
            continue;
        uint32_t e, g;
        float ef, gf;
        std::memcpy(&e, expected + o, 4);
        std::memcpy(&g, got + o, 4);
        if (stack_address(e) && stack_address(g))
            continue;
        ++count;
        if (shown >= 12)
            continue;
        ++shown;
        std::memcpy(&ef, &e, 4);
        std::memcpy(&gf, &g, 4);
        std::fprintf(stderr, "    %s %08x: machine code %08x (%g), C++ %08x (%g)\n", area,
                     base + static_cast<uint32_t>(o), e, ef, g, gf);
    }
}

}  // namespace

void log_call(const char* name, const CallArgs* args) { record(name, 0, args); }

void log_interpreted(uint32_t address, const CallArgs& args) {
    if (g_skip_next_run) {
        g_skip_next_run = false;
        return;
    }
    record("", address, &args);
}

uint32_t g_watch_addresses[16] = {};
uint64_t g_watch_frames[16] = {};
int g_watch_count = 0;

void watch(uint32_t address) {
    for (int i = 0; i < g_watch_count; ++i)
        if (g_watch_addresses[i] == address)
            return;
    if (g_watch_count < 16) {
        g_watch_addresses[g_watch_count] = address;
        g_watch_frames[g_watch_count++] = kNotYet;
    }
}

uint64_t first_call(uint32_t address) {
    for (int i = 0; i < g_watch_count; ++i)
        if (g_watch_addresses[i] == address)
            return g_watch_frames[i];
    return kNotYet;
}

void note_call(uint32_t address) {
    for (int i = 0; i < g_watch_count; ++i)
        if (g_watch_addresses[i] == address && g_watch_frames[i] == kNotYet) {
            g_watch_frames[i] = hw::frames();
            std::fprintf(stderr, "first call of 0x%08x at frame %llu\n", address,
                         static_cast<unsigned long long>(g_watch_frames[i]));
        }
}

// IKARUGA_WATCH_CALLS=ADDR[,ADDR...]: prints the frame of each function's first call.
const bool g_watch_init = [] {
    if (const char* v = std::getenv("IKARUGA_WATCH_CALLS"))
        for (const char* p = v; *p;) {
            char* end = nullptr;
            const auto address = static_cast<uint32_t>(std::strtoul(p, &end, 0));
            if (end == p)
                break;
            watch(address);
            p = *end == ',' ? end + 1 : end;
        }
    return true;
}();

struct Called {
    const char* name;
    uint64_t first, calls;
};
std::unordered_map<uint32_t, Called> g_called_functions;

bool g_called = std::getenv("IKARUGA_CALLED") != nullptr;

void note_called(uint32_t address, const char* name) {
    // A shadow's calls do not happen: the port drops what it did.
    if (g_shadow)
        return;
    ++g_called_functions.try_emplace(address, Called{name, hw::frames(), 0}).first->second.calls;
}

// One line for each function, by address: the address, the name, the first call's frame, the calls.
void write_called() {
    const char* path = std::getenv("IKARUGA_CALLED");
    if (!path)
        return;
    std::FILE* out = std::fopen(path, "w");
    if (!out) {
        std::fprintf(stderr, "IKARUGA_CALLED: cannot write %s\n", path);
        return;
    }
    const std::map<uint32_t, Called> sorted(g_called_functions.begin(), g_called_functions.end());
    for (const auto& [address, c] : sorted)
        std::fprintf(out, "%08x %s %llu %llu\n", address, c.name, static_cast<unsigned long long>(c.first),
                     static_cast<unsigned long long>(c.calls));
    std::fclose(out);
}

void note_ta_flush(uint32_t destination, const uint8_t* words) {
    Flush f{destination, {}};
    std::memcpy(f.words, words, 32);
    if (g_shadow)
        g_shadow_ta.push_back(f);
    else if (g_testing)
        g_native_ta.push_back(f);
}

void log_arguments(const char* name, const CallArgs& a) {
    std::fprintf(stderr, "call %s at frame %llu: r4 %08x r5 %08x r6 %08x r7 %08x fr4 %g fr5 %g fr6 %g fr7 %g\n", name,
                 static_cast<unsigned long long>(hw::frames()), a.r[0], a.r[1], a.r[2], a.r[3], a.fr[0], a.fr[1],
                 a.fr[2], a.fr[3]);
}

void count_access() {
    if (++g_accesses > kAccessBudget)
        throw Abort{"polls the hardware"};
    if ((g_accesses & 4095) == 0 &&
        std::chrono::duration<double>(std::chrono::steady_clock::now() - g_started).count() > kSecondsBudget)
        throw Abort{"does not return within 0.25 seconds"};
}

bool begin(uint32_t address) {
    if (g_active || g_shadow)
        return false;
    if (!g_only.empty() && std::find(g_only.begin(), g_only.end(), address) == g_only.end())
        return false;
    if (hw::frames() < g_from)
        return false;
    const Function& f = g_functions[address];
    return !f.failed && !f.aborted && f.tested < g_calls;
}

bool shadow(uint32_t address, const char* name, const CallArgs& args) {
    Function& f = g_functions[address];
    f.name = name;
    Registers before;
    before.save();
    std::memcpy(g_before_ram.data(), guest::ram, guest::kRamSize);
    std::memcpy(g_before_vram.data(), guest::vram(), hw::kVramSize);
    std::memcpy(g_before_aram.data(), guest::aram(), hw::kAramSize);
    std::memcpy(g_before_sq, guest::store_queues(), 64);
    const uint32_t sp = guest::sp;

    g_shadow_calls.clear();
    g_native_calls.clear();
    g_shadow_ta.clear();
    g_native_ta.clear();
    ++g_shadow;
    g_accesses = 0;
    g_started = std::chrono::steady_clock::now();
    const char* why = nullptr;
    try {
        g_skip_next_run = true;
        sh4::run(address, args);
    }
    catch (const Abort& a) {
        why = a.why;
    }
    --g_shadow;
    guest::sp = sp;
    if (g_verbose)
        std::fprintf(stderr, "diff: %.3f s, frame %llu: shadow of %s took %.1f ms%s%s\n",
                     std::chrono::duration<double>(g_started - g_process_start).count(),
                     static_cast<unsigned long long>(hw::frames()), name,
                     std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - g_started).count(),
                     why ? ", abandoned: " : "", why ? why : "");

    if (!why) {
        g_expected_regs.save();
        std::memcpy(g_expected_ram.data(), guest::ram, guest::kRamSize);
        std::memcpy(g_expected_vram.data(), guest::vram(), hw::kVramSize);
    }
    before.load();
    std::memcpy(guest::ram, g_before_ram.data(), guest::kRamSize);
    std::memcpy(guest::vram(), g_before_vram.data(), hw::kVramSize);
    std::memcpy(guest::aram(), g_before_aram.data(), hw::kAramSize);
    std::memcpy(guest::store_queues(), g_before_sq, 64);
    if (why) {
        // Another call may take a path that stays in guest memory.
        if (++f.abandoned >= kAbandonLimit)
            f.aborted = why;
        return false;
    }
    g_active = true;
    g_address = address;
    g_args = args;
    g_entry_sp = sp;
    g_activity = activity();
    g_testing = true;
    g_hw_accesses = 0;
    return true;
}

void finish(bool unwinding) {
    g_active = false;
    g_testing = false;
    Function& f = g_functions[g_address];
    if (unwinding)
        return;
    if (activity() != g_activity) {
        if (++f.inconclusive >= kInconclusiveLimit && !f.tested)
            f.aborted = "meets a hardware event at every call";
        return;
    }
    ++f.tested;
    // Main RAM without the stack below the entry, video memory and the FPU's back bank.
    const size_t low = kStackLow - 0x8C000000u, high = g_entry_sp - 0x8C000000u;
    const bool ram_differs =
        std::memcmp(g_expected_ram.data(), guest::ram, low) != 0 ||
        std::memcmp(g_expected_ram.data() + high, guest::ram + high, guest::kRamSize - high) != 0;
    const bool vram_differs = std::memcmp(g_expected_vram.data(), guest::vram(), hw::kVramSize) != 0;
    const bool xf_differs = std::memcmp(g_expected_regs.xf, cpu::xf, sizeof cpu::xf) != 0;
    const bool ta_differs = g_shadow_ta != g_native_ta;
    if (!ram_differs && !vram_differs && !xf_differs && !ta_differs)
        return;
    // Twice: once to count, once to print.
    for (int pass = 0; pass < 2; ++pass) {
        size_t count = 0;
        int shown = pass == 0 ? 12 : 0;
        if (pass == 1)
            std::fprintf(stderr, "diff: %s (0x%08x) differs from the machine code at call %d, frame %llu:\n",
                         f.name, g_address, f.tested + f.inconclusive,
                         static_cast<unsigned long long>(hw::frames()));
        if (pass == 1)
            std::fprintf(stderr, "    called with r4 %08x r5 %08x r6 %08x r7 %08x fr4 %g fr5 %g fr6 %g fr7 %g\n",
                         g_args.r[0], g_args.r[1], g_args.r[2], g_args.r[3], g_args.fr[0], g_args.fr[1],
                         g_args.fr[2], g_args.fr[3]);
        if (ram_differs) {
            compare_words("RAM", 0x8C000000u, g_expected_ram.data(), guest::ram, low, shown, count);
            compare_words("RAM", g_entry_sp, g_expected_ram.data() + high, guest::ram + high,
                          guest::kRamSize - high, shown, count);
        }
        if (vram_differs)
            compare_words("VRAM", 0xA4000000u, g_expected_vram.data(), guest::vram(), hw::kVramSize, shown, count);
        if (ta_differs) {
            ++count;
            if (pass == 1) {
                size_t i = 0;
                while (i < g_shadow_ta.size() && i < g_native_ta.size() && g_shadow_ta[i] == g_native_ta[i]) ++i;
                std::fprintf(stderr, "    TA: %zu flushes by the machine code, %zu by the C++, the first different #%zu\n",
                             g_shadow_ta.size(), g_native_ta.size(), i);
                for (const auto* list : {&g_shadow_ta, &g_native_ta})
                    if (i < list->size()) {
                        const Flush& f = (*list)[i];
                        std::fprintf(stderr, "      %s %08x:", list == &g_shadow_ta ? "machine code" : "C++         ",
                                     f.destination);
                        for (uint32_t w : f.words) std::fprintf(stderr, " %08x", w);
                        std::fprintf(stderr, "\n");
                    }
            }
        }
        for (int i = 0; i < 16; ++i)
            if (std::memcmp(&g_expected_regs.xf[i], &cpu::xf[i], 4) != 0) {
                ++count;
                if (pass == 1)
                    std::fprintf(stderr, "    xf%d: machine code %g, C++ %g\n", i ^ 1, g_expected_regs.xf[i],
                                 cpu::xf[i]);
            }
        if (count == 0)
            return;
        f.failed = true;
        if (g_hw_accesses) {
            f.hardware = true;
            return;
        }
        if (pass == 1) {
            std::fprintf(stderr, "    %zu words differ\n", count);
            print_first_different_call();
        }
    }
}

void report(std::FILE* out) {
    if (!g_on)
        return;
    int tested = 0, failed = 0, inconclusive = 0;
    std::string hardware;
    std::map<std::string, int> aborted;
    for (const auto& [a, f] : g_functions) {
        if (f.failed && f.hardware)
            hardware += std::string(" ") + f.name;
        else if (f.failed)
            ++failed;
        else if (f.tested)
            ++tested;
        else if (f.aborted)
            ++aborted[f.aborted];
        else if (f.inconclusive)
            ++inconclusive;
    }
    std::fprintf(out, "diff: %d functions match the machine code, %d differ, %d only inconclusive\n", tested, failed,
                 inconclusive);
    if (!hardware.empty())
        std::fprintf(out, "  differ, but access registers:%s\n", hardware.c_str());
    for (const auto& [why, n] : aborted)
        std::fprintf(out, "  %d not tested: the machine code %s\n", n, why.c_str());
}

}  // namespace diff
