// The CPU state the decompiled code keeps outside its variables, calls through code pointers,
// guest time, and the delivery of interrupts.
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <queue>
#include <string>
#include <thread>
#include <vector>

#include "diff.h"
#include "dream/runtime/fenv.h"
#include "hw.h"
#include "sh4/sh4.h"

namespace trace {
void print_stack(std::FILE* out);
}
#include "port.h"

namespace trace {
const char* current();
}

namespace cpu {

uint32_t gbr = 0x8C000000u, vbr = 0x8C000000u, sr = 0x400000F0u, fpul = 0, fpscr = 0x00040001u;
uint32_t macl = 0, mach = 0, sgr = 0, ssr = 0, spc = 0, dbr = 0;
float xf[16] = {};
uint32_t ctx[16] = {};

namespace {
std::map<std::string, uint64_t>& unknowns() {
    static std::map<std::string, uint64_t> m;
    return m;
}
}  // namespace

uint32_t unknown(const char* function, const char* name) {
    ++unknowns()[std::string(function) + " " + name];
    return 0;
}

void report_unknowns(std::FILE* out, size_t limit) {
    std::vector<std::pair<uint64_t, std::string>> v;
    for (const auto& [k, n] : unknowns()) v.push_back({n, k});
    std::sort(v.rbegin(), v.rend());
    std::fprintf(out, "registers read without a value: %zu distinct\n", v.size());
    for (size_t i = 0; i < v.size() && i < limit; ++i)
        std::fprintf(out, "  %8llu  %s\n", static_cast<unsigned long long>(v[i].first), v[i].second.c_str());
}

namespace {
struct MaskChange {
    uint32_t from, to;
    const char* where;
};
MaskChange mask_log[32];
size_t mask_next = 0;
}  // namespace

void report_mask_changes(std::FILE* out) {
    std::fprintf(out, "last changes of the interrupt mask:\n");
    for (size_t i = mask_next > 32 ? mask_next - 32 : 0; i < mask_next; ++i)
        std::fprintf(out, "  %u -> %u after entering %s\n", mask_log[i % 32].from, mask_log[i % 32].to, mask_log[i % 32].where);
}

// IKARUGA_LOG_INTERRUPTS=N prints, from frame N on, each interrupt taken, its return and each
// write to SR, with the function last entered.
bool log_interrupts() {
    static const uint64_t from = [] {
        const char* v = std::getenv("IKARUGA_LOG_INTERRUPTS");
        return v ? std::strtoull(v, nullptr, 0) : ~0ull;
    }();
    return hw::frames() >= from;
}

void set_sr(uint32_t value) {
    const uint32_t old_mask = (sr >> 4) & 15;
    if (log_interrupts())
        std::fprintf(stderr, "SR %08x -> %08x after entering %s\n", sr, value, trace::current());
    if (((value >> 4) & 15) != old_mask)
        mask_log[mask_next++ % 32] = {old_mask, (value >> 4) & 15, trace::current()};
    sr = value & 0x700083F3u;
    // Lowering the mask lets an interrupt that waited in.
    if (((sr >> 4) & 15) < old_mask)
        hw::tick();
}
void set_gbr(uint32_t value) { gbr = value; }
void set_vbr(uint32_t value) { vbr = value; }
void set_fpscr(uint32_t value) {
    fpscr = value & 0x003FFFFFu;
    dream::fenv::apply(dream::fenv::from_fpscr(fpscr));
}

uint32_t sr_field(const char* name) {
    const std::string n(name);
    if (n == "T") return sr & 1;
    if (n == "S") return (sr >> 1) & 1;
    if (n == "IMASK") return (sr >> 4) & 15;
    if (n == "Q") return (sr >> 8) & 1;
    if (n == "M") return (sr >> 9) & 1;
    if (n == "FD") return (sr >> 15) & 1;
    if (n == "BL") return (sr >> 28) & 1;
    if (n == "RB") return (sr >> 29) & 1;
    if (n == "MD") return (sr >> 30) & 1;
    return 0;
}

uint32_t fpscr_field(const char* name) {
    const std::string n(name);
    if (n == "RM") return fpscr & 3;
    if (n == "FLAG") return (fpscr >> 2) & 0x1F;
    if (n == "ENABLE") return (fpscr >> 7) & 0x1F;
    if (n == "CAUSE") return (fpscr >> 12) & 0x3F;
    if (n == "DN") return (fpscr >> 18) & 1;
    if (n == "PR") return (fpscr >> 19) & 1;
    if (n == "SZ") return (fpscr >> 20) & 1;
    if (n == "FR") return (fpscr >> 21) & 1;
    return 0;
}

}  // namespace cpu

uint32_t cpu_unknown_word(const char* what) { return cpu::unknown(what, "result"); }

// ---- calls through code pointers --------------------------------------------------------------

namespace bios {
bool call(uint32_t address, const CallArgs& args, CallResult& result);
}

namespace guest {

uint32_t image_end = 0x8C010000u;

namespace {
// The table is sorted by address in P1, where the game links its code.
Thunk find(uint32_t address) {
    const uint32_t a = (address & 0x1FFFFFFFu) | 0x80000000u;
    const DispatchEntry* lo = kDispatch;
    const DispatchEntry* hi = kDispatch + kDispatchCount;
    const DispatchEntry* it = std::lower_bound(lo, hi, a, [](const DispatchEntry& e, uint32_t v) {
        return e.address < v;
    });
    if (it != hi && it->address == a)
        return it->thunk;
    return nullptr;
}
std::map<uint32_t, uint64_t> missing_calls;
}  // namespace

CallResult call(uint32_t address, const CallArgs& args) {
    hw::tick();
    CallResult r;
    if (bios::call(address, args, r))
        return r;
    if (sh4::interpreted(address))
        return sh4::run(address, args);
    if (Thunk t = find(address))
        return t(args);
    // Code the analysis did not make a function of, such as the code after 0x8C0C0574 that it takes
    // for data: its machine code runs on the interpreter.
    const uint32_t p1 = (address & 0x1FFFFFFFu) | 0x80000000u;
    if (p1 >= 0x8C010000u && p1 < image_end) {
        if (missing_calls[address]++ == 0)
            std::fprintf(stderr, "call to 0x%08x: no C++ function, its machine code runs on the interpreter\n",
                         address);
        return sh4::run(address, args);
    }
    if (missing_calls[address]++ == 0) {
        std::fprintf(stderr, "call to 0x%08x: no function there (args %08x %08x %08x %08x)\n", address,
                     args.r[0], args.r[1], args.r[2], args.r[3]);
        trace::print_stack(stderr);
    }
    return r;
}

namespace {
void put_slot(CallArgs& out, int slot, const ArgWord& w) {
    if (slot < 4)
        out.r[slot] = w.w;
    else if (slot < 12)
        std::memcpy(&out.fr[slot - 4], &w.w, 4);
    else if (slot >= 16 && slot < 32)
        out.stack[slot - 16] = w.w;
    else if (slot >= 32 && slot < 48) {
        out.ctx[slot - 32] = w.w;
        out.nctx = static_cast<uint16_t>(out.nctx | 1u << (slot - 32));
    }
}
}  // namespace

void place(uint32_t address, const ArgWord* args, int n, CallArgs& out) {
    const uint32_t a = (address & 0x1FFFFFFFu) | 0x80000000u;
    const ArgLayout* lo = kArgLayouts;
    const ArgLayout* hi = kArgLayouts + kArgLayoutCount;
    const ArgLayout* l = std::lower_bound(lo, hi, a, [](const ArgLayout& e, uint32_t v) { return e.address < v; });
    if (l != hi && l->address == a) {
        int stack = 0;
        for (int i = 0, param = 0; i < n && param < l->count; ++i, ++param) {
            const int slot = static_cast<uint8_t>(l->slots[param]);
            put_slot(out, slot, args[i]);
            if (args[i].wide && i + 1 < n)
                put_slot(out, slot + 1, args[++i]);
            if (slot >= 16)
                stack = std::max(stack, slot - 16 + (args[i].wide ? 2 : 1));
        }
        out.nr = 4;
        out.nfr = 8;
        out.nstack = stack;
        return;
    }
    for (int i = 0; i < n; ++i) {
        if (args[i].is_float && out.nfr < 8) {
            std::memcpy(&out.fr[out.nfr++], &args[i].w, 4);
            continue;
        }
        if (out.nr < 4)
            out.r[out.nr++] = args[i].w;
        else if (out.nstack < 16)
            out.stack[out.nstack++] = args[i].w;
    }
}

bool native(uint32_t address) {
    return (address >= 0xA0000B00u && address < 0xA0000B20u) || (!sh4::interpreted(address) && find(address));
}

void report_missing_calls(std::FILE* out) {
    for (const auto& [a, n] : missing_calls)
        std::fprintf(out, "  missing function 0x%08x called %llu times\n", a, static_cast<unsigned long long>(n));
}

}  // namespace guest

// ---- interrupts --------------------------------------------------------------------------------

namespace hw {

std::map<uint32_t, uint64_t>& interrupt_counts() {
    static std::map<uint32_t, uint64_t> m;
    return m;
}

void interrupt(uint32_t intevt, int level) {
    const int imask = static_cast<int>((cpu::sr >> 4) & 15);
    if ((cpu::sr & 0x10000000u) || level <= imask)
        return;
    ++stats().interrupts;
    ++interrupt_counts()[intevt];
    const bool log = cpu::log_interrupts();
    if (log)
        std::fprintf(stderr, "interrupt %03x level %d at frame %llu after entering %s, SR %08x\n", intevt, level,
                     static_cast<unsigned long long>(hw::frames()), trace::current(), cpu::sr);
    // The handler runs with every interrupt masked. The mask goes up first: the store to INTEVT
    // below is a safe point, which would take the same interrupt again.
    const uint32_t saved_sr = cpu::sr;
    cpu::sr = (cpu::sr & ~0xF0u) | 0xF0u;
    // What the stub at VBR+0x600 does: INTEVT goes to the game's global, and the handler comes from
    // the table at VBR+0x200, one word per INTEVT step of 0x20.
    guest::store<uint32_t>(0xFF000028u, intevt);
    guest::store<uint32_t>(0x8C043ED8u, intevt);
    const uint32_t handler = guest::load<uint32_t>(cpu::vbr + 0x200u + ((intevt - 0x200u) >> 3));
    if (handler != 0) {
        CallArgs args;  // the stub passes INTEVT in r4
        args.r[0] = intevt;
        args.nr = 1;
        // The handler pushes below the interrupted code's frames. Code on the interpreter keeps its
        // stack pointer in r15, below the one the C++ code keeps.
        const uint32_t saved_sp = guest::sp;
        guest::sp = std::min(guest::sp, sh4::active_stack());
        guest::call(handler, args);
        guest::sp = saved_sp;
    }
    if (log)
        std::fprintf(stderr, "interrupt %03x returns, sr %08x -> %08x\n", intevt, cpu::sr, saved_sr);
    cpu::sr = saved_sr;
}

}  // namespace hw

// ---- instructions without a C equivalent ------------------------------------------------------

namespace hw {

void bad_data(const char* function) {
    std::fprintf(stderr, "%s: the decompiler found no instruction here (halt_baddata)\n", function);
    std::abort();
}

void unimplemented(const char* function) {
    std::fprintf(stderr, "%s: an instruction the decompiler could not express (halt_unimplemented)\n",
                 function);
    std::abort();
}

void trap(uint32_t number) { std::fprintf(stderr, "trapa #%u ignored\n", number); }

}  // namespace hw
