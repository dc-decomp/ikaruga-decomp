// The SH-4 interpreter: the integer and single-precision FPU instruction set, with delay slots,
// the 64-bit pair moves of FPSCR.SZ, the bank switch of FPSCR.FR, fsca, fsrra, fipr and ftrv.
// Double precision (FPSCR.PR) is not modelled: the game is built for single precision.
#include "sh4.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <vector>

#include "../hw.h"
#include "diff.h"
#include "dream/runtime/sh4/ops.h"
#include "port.h"

namespace sh4 {

namespace {

constexpr uint32_t kReturn = 0xFFFFFFF0u;  // PR of the outermost call: run() returns there
constexpr uint32_t kSZ = 1u << 20, kPR = 1u << 19, kFR = 1u << 21;

// The last branches taken, for reports: from, to.
uint32_t g_branches[64][2];
unsigned g_branch_next = 0;

// The xf bank lives in cpu::xf for the decompiled code too, in Ghidra's pair-swapped order.
float& xf(int n) { return cpu::xf[n ^ 1]; }

uint32_t bits(float f) {
    uint32_t b;
    std::memcpy(&b, &f, 4);
    return b;
}
float from_bits(uint32_t b) {
    float f;
    std::memcpy(&f, &b, 4);
    return f;
}

struct Cpu {
    uint32_t r[16] = {};
    float fr[16] = {};
    uint32_t pc = 0, pr = kReturn, fpul = 0, fpscr = 0;
    bool t = false, s = false, q = false, m = false;
    uint64_t executed = 0;

    // ---- memory ------------------------------------------------------------------------------
    template <class T>
    T rd(uint32_t a) {
        return guest::load<T>(a);
    }
    template <class T>
    void wr(uint32_t a, T v) {
        guest::store<T>(a, v);
    }

    // ---- registers ---------------------------------------------------------------------------
    uint32_t sr() const {
        return (cpu::sr & ~0x303u) | (t ? 1u : 0) | (s ? 2u : 0) | (q ? 0x100u : 0) | (m ? 0x200u : 0);
    }
    void set_sr(uint32_t v) {
        t = v & 1;
        s = v & 2;
        q = v & 0x100;
        m = v & 0x200;
        cpu::set_sr(v);
    }
    // The C++ has one FPSCR, whose rounding mode is the host FPU's: an lds sets both.
    void set_fpscr(uint32_t v) {
        if ((v ^ fpscr) & kFR)
            swap_banks();
        fpscr = v & 0x003FFFFFu;
        cpu::set_fpscr(fpscr);
    }
    void swap_banks() {
        for (int i = 0; i < 16; ++i)
            std::swap(fr[i], xf(i));
    }
    // A pair register for the 64-bit moves: an even field is DRn, an odd one XDn-1.
    float& pair(int field, int half) {
        const int n = (field & 0xE) + half;
        return (field & 1) ? xf(n) : fr[n];
    }

    // ---- calls out of the interpreter ----------------------------------------------------------
    // A call to a function outside the set: its C++ version, with the convention's arguments.
    void native_call(uint32_t target) {
        CallArgs a;
        for (int i = 0; i < 4; ++i)
            a.r[i] = r[4 + i];
        for (int i = 0; i < 8; ++i)
            a.fr[i] = fr[4 + i];
        for (int i = 0; i < 16; ++i)
            a.stack[i] = rd<uint32_t>(r[15] + 4u * static_cast<uint32_t>(i));
        a.nr = 4;
        a.nfr = 8;
        a.nstack = 16;
        const uint32_t saved_sp = guest::sp;
        guest::sp = r[15];
        for (int i = 8; i < 15; ++i)
            cpu::ctx[i] = r[i];
        const CallResult res = guest::call(target, a);
        guest::sp = saved_sp;
        r[0] = res.r0;
        r[1] = res.r1;
        fr[0] = res.fr0;
        fr[1] = res.fr1;
    }

    // A call into another interpreted function, for the differential tests' call log, with the
    // registers its parameters take.
    void log_call_to(uint32_t target) {
        const uint32_t* e = std::lower_bound(kInterpreted, kInterpreted + kInterpretedCount, target);
        const uint8_t* counts = kInterpretedArgs[e - kInterpreted];
        CallArgs a;
        for (int i = 0; i < 4; ++i)
            a.r[i] = r[4 + i];
        for (int i = 0; i < 8; ++i)
            a.fr[i] = fr[4 + i];
        for (int i = 0; i < 16; ++i)
            a.stack[i] = rd<uint32_t>(r[15] + 4u * static_cast<uint32_t>(i));
        a.nr = counts[0];
        a.nfr = counts[1];
        a.nstack = counts[2];
        diff::log_interpreted(target, a);
    }

    void tick() {
        const uint32_t saved_sp = guest::sp;
        guest::sp = r[15];
        hw::tick(256);
        guest::sp = saved_sp;
    }

    void unknown(uint16_t op);

    void run_until_return();
    // Executes one instruction at pc. A branch with a delay slot executes the slot too.
    void step();
    void execute(uint16_t op, uint32_t at, bool in_slot);
    // Transfers control after the delay slot at `at + 2`.
    void delayed(uint32_t at, uint32_t target, bool call, bool link);
};

uint64_t g_limit = 0;  // run_regs: a test's bound on the instructions
Cpu* g_active = nullptr;  // the innermost running interpreter, for reports

void Cpu::unknown(uint16_t op) {
    static int reported = 0;
    if (reported++ < 2) {
        std::fprintf(stderr, "sh4: unknown instruction %04x at %08x; last branches:\n", op, pc);
        for (unsigned i = g_branch_next > 24 ? g_branch_next - 24 : 0; i < g_branch_next; ++i)
            std::fprintf(stderr, "  %08x -> %08x\n", g_branches[i % 64][0], g_branches[i % 64][1]);
    }
}

void Cpu::run_until_return() {
    Cpu* const outer = g_active;
    g_active = this;
    struct Restore {
        Cpu* outer;
        ~Restore() { g_active = outer; }
    } restore{outer};
    while (pc != kReturn) {
        step();
        if ((++executed & 255) == 0) {
            tick();
            if (diff::g_shadow)
                diff::count_access();
        }
        if (g_limit && executed > g_limit) {
            std::fprintf(stderr, "sh4: no return after %llu instructions, pc %08x pr %08x r0 %08x r1 %08x\n",
                         static_cast<unsigned long long>(executed), pc, pr, r[0], r[1]);
            return;
        }
    }
}

void Cpu::step() {
    const uint32_t at = pc;
    const uint16_t op = rd<uint16_t>(at);
    pc = at + 2;
    execute(op, at, false);
}

void Cpu::delayed(uint32_t at, uint32_t target, bool call, bool link) {
    g_branches[g_branch_next % 64][0] = at;
    g_branches[g_branch_next % 64][1] = target;
    ++g_branch_next;
    const uint32_t ret = at + 4;
    const uint16_t slot = rd<uint16_t>(at + 2);
    if (link)
        pr = ret;
    execute(slot, at + 2, true);
    // Calls and jumps into watched functions count as their calls (IKARUGA_WATCH_CALLS), for a run
    // with the game's code on the interpreter.
    if (call && diff::g_watch_count)
        diff::note_call(target);
    // Ninja ends at 0x8C031000; the SHC runtime's helpers above it are native in the C++ code.
    if (call && (diff::g_shadow || diff::g_testing) && interpreted(target) && target < 0x8C031000u)
        log_call_to(target);
    if (call && guest::native(target)) {
        // The callee runs as C++ and returns here; a jmp out of the set returns to our caller. Any
        // other target, such as a computed jump into an unrolled loop, stays interpreted.
        native_call(target);
        pc = link ? ret : pr;
        return;
    }
    pc = target;
}

void Cpu::execute(uint16_t op, uint32_t at, bool in_slot) {
    const int n = (op >> 8) & 15, mm = (op >> 4) & 15;
    uint32_t& rn = r[n];
    const uint32_t rmv = r[mm];
    const int32_t imm8 = static_cast<int8_t>(op & 0xFF);
    const bool sz = fpscr & kSZ;
    (void)in_slot;
    switch (op >> 12) {
        case 0x0:
            switch (op & 0xF) {
                case 0x2:
                    switch (mm) {
                        case 0x0: rn = sr(); return;
                        case 0x1: rn = cpu::gbr; return;
                        case 0x2: rn = cpu::vbr; return;
                        default: unknown(op); return;
                    }
                case 0x3:
                    switch (mm) {
                        case 0x0: delayed(at, at + 4 + rn, true, true); return;   // bsrf
                        case 0x2: delayed(at, at + 4 + rn, false, false); return;  // braf
                        case 0x8: hw::prefetch(rn); return;                        // pref
                        case 0x9: case 0xA: case 0xB: return;                       // ocbi, ocbp, ocbwb
                        case 0xC: wr<uint32_t>(rn, r[0]); return;                  // movca.l
                        default: unknown(op); return;
                    }
                case 0x4: wr<uint8_t>(r[0] + rn, static_cast<uint8_t>(rmv)); return;
                case 0x5: wr<uint16_t>(r[0] + rn, static_cast<uint16_t>(rmv)); return;
                case 0x6: wr<uint32_t>(r[0] + rn, rmv); return;
                case 0x7: cpu::macl = rn * rmv; return;  // mul.l
                case 0x8:
                    switch (op) {
                        case 0x0008: t = false; return;  // clrt
                        case 0x0018: t = true; return;   // sett
                        case 0x0028: cpu::mach = cpu::macl = 0; return;
                        case 0x0048: s = false; return;
                        case 0x0058: s = true; return;
                        default: unknown(op); return;
                    }
                case 0x9:
                    switch (mm) {
                        case 0x0: return;  // nop
                        case 0x1: m = q = t = false; return;  // div0u
                        case 0x2: rn = t ? 1 : 0; return;     // movt
                        default: unknown(op); return;
                    }
                case 0xA:
                    switch (mm) {
                        case 0x0: rn = cpu::mach; return;
                        case 0x1: rn = cpu::macl; return;
                        case 0x2: rn = pr; return;
                        case 0x5: rn = fpul; return;
                        case 0x6: rn = fpscr; return;
                        default: unknown(op); return;
                    }
                case 0xB:
                    if (op == 0x000B) {  // rts
                        delayed(at, pr, false, false);
                        return;
                    }
                    unknown(op);
                    return;
                case 0xC: rn = static_cast<uint32_t>(static_cast<int8_t>(rd<uint8_t>(r[0] + rmv))); return;
                case 0xD: rn = static_cast<uint32_t>(static_cast<int16_t>(rd<uint16_t>(r[0] + rmv))); return;
                case 0xE: rn = rd<uint32_t>(r[0] + rmv); return;
                case 0xF: {  // mac.l
                    const int64_t a = static_cast<int32_t>(rd<uint32_t>(rmv));
                    r[mm] += 4;
                    const int64_t b = static_cast<int32_t>(rd<uint32_t>(r[n]));
                    r[n] += 4;
                    int64_t acc = static_cast<int64_t>((static_cast<uint64_t>(cpu::mach) << 32) | cpu::macl);
                    acc += a * b;
                    cpu::mach = static_cast<uint32_t>(acc >> 32);
                    cpu::macl = static_cast<uint32_t>(acc);
                    return;
                }
                default: unknown(op); return;
            }
        case 0x1: wr<uint32_t>(rn + (op & 0xF) * 4u, rmv); return;  // mov.l rm,@(disp,rn)
        case 0x2:
            switch (op & 0xF) {
                case 0x0: wr<uint8_t>(rn, static_cast<uint8_t>(rmv)); return;
                case 0x1: wr<uint16_t>(rn, static_cast<uint16_t>(rmv)); return;
                case 0x2: wr<uint32_t>(rn, rmv); return;
                case 0x4: rn -= 1; wr<uint8_t>(rn, static_cast<uint8_t>(rmv)); return;
                case 0x5: rn -= 2; wr<uint16_t>(rn, static_cast<uint16_t>(rmv)); return;
                case 0x6: rn -= 4; wr<uint32_t>(rn, rmv); return;
                case 0x7: q = rn >> 31; m = rmv >> 31; t = q != m; return;  // div0s
                case 0x8: t = (rn & rmv) == 0; return;
                case 0x9: rn &= rmv; return;
                case 0xA: rn ^= rmv; return;
                case 0xB: rn |= rmv; return;
                case 0xC: {  // cmp/str
                    const uint32_t x = rn ^ rmv;
                    t = !(x & 0xFF) || !(x & 0xFF00) || !(x & 0xFF0000) || !(x & 0xFF000000u);
                    return;
                }
                case 0xD: rn = (rn >> 16) | (rmv << 16); return;  // xtrct
                case 0xE: cpu::macl = (rn & 0xFFFF) * (rmv & 0xFFFF); return;  // mulu.w
                case 0xF:
                    cpu::macl = static_cast<uint32_t>(static_cast<int16_t>(rn) * static_cast<int16_t>(rmv));
                    return;
                default: unknown(op); return;
            }
        case 0x3:
            switch (op & 0xF) {
                case 0x0: t = rn == rmv; return;
                case 0x2: t = rn >= rmv; return;
                case 0x3: t = static_cast<int32_t>(rn) >= static_cast<int32_t>(rmv); return;
                case 0x4: {  // div1, as the programming manual gives it
                    const bool old_q = q;
                    q = (rn & 0x80000000u) != 0;
                    const uint32_t tmp2 = rmv;
                    rn = (rn << 1) | (t ? 1u : 0u);
                    const uint32_t tmp0 = rn;
                    bool tmp1;
                    if (old_q == m) {
                        rn -= tmp2;
                        tmp1 = rn > tmp0;
                    }
                    else {
                        rn += tmp2;
                        tmp1 = rn < tmp0;
                    }
                    // The manual's four cases come to: subtract when old Q equals M, add otherwise,
                    // and the new Q is the shifted-out bit XOR the carry or borrow XOR M.
                    q = q ^ tmp1 ^ m;
                    t = q == m;
                    return;
                }
                case 0x5: {
                    const uint64_t p = static_cast<uint64_t>(rn) * rmv;
                    cpu::mach = static_cast<uint32_t>(p >> 32);
                    cpu::macl = static_cast<uint32_t>(p);
                    return;
                }
                case 0x6: t = rn > rmv; return;
                case 0x7: t = static_cast<int32_t>(rn) > static_cast<int32_t>(rmv); return;
                case 0x8: rn -= rmv; return;
                case 0xA: {  // subc
                    const uint32_t tmp1 = rn - rmv, tmp0 = rn;
                    rn = tmp1 - (t ? 1 : 0);
                    t = tmp0 < tmp1 || tmp1 < rn;
                    return;
                }
                case 0xB: {  // subv
                    const int64_t d = static_cast<int64_t>(static_cast<int32_t>(rn)) - static_cast<int32_t>(rmv);
                    rn -= rmv;
                    t = d != static_cast<int32_t>(rn);
                    return;
                }
                case 0xC: rn += rmv; return;
                case 0xD: {
                    const int64_t p = static_cast<int64_t>(static_cast<int32_t>(rn)) * static_cast<int32_t>(rmv);
                    cpu::mach = static_cast<uint32_t>(static_cast<uint64_t>(p) >> 32);
                    cpu::macl = static_cast<uint32_t>(p);
                    return;
                }
                case 0xE: {  // addc
                    const uint32_t tmp1 = rn + rmv, tmp0 = rn;
                    rn = tmp1 + (t ? 1 : 0);
                    t = tmp0 > tmp1 || tmp1 > rn;
                    return;
                }
                case 0xF: {  // addv
                    const int64_t s2 = static_cast<int64_t>(static_cast<int32_t>(rn)) + static_cast<int32_t>(rmv);
                    rn += rmv;
                    t = s2 != static_cast<int32_t>(rn);
                    return;
                }
                default: unknown(op); return;
            }
        case 0x4:
            switch (op & 0xFF) {
                case 0x00: t = rn >> 31; rn <<= 1; return;  // shll
                case 0x01: t = rn & 1; rn >>= 1; return;    // shlr
                case 0x02: rn -= 4; wr<uint32_t>(rn, cpu::mach); return;
                case 0x03: rn -= 4; wr<uint32_t>(rn, sr()); return;
                case 0x04: t = rn >> 31; rn = (rn << 1) | (t ? 1 : 0); return;  // rotl
                case 0x05: t = rn & 1; rn = (rn >> 1) | (t ? 0x80000000u : 0); return;  // rotr
                case 0x06: cpu::mach = rd<uint32_t>(rn); rn += 4; return;
                case 0x07: { const uint32_t v = rd<uint32_t>(rn); rn += 4; set_sr(v); return; }
                case 0x08: rn <<= 2; return;
                case 0x09: rn >>= 2; return;
                case 0x0A: cpu::mach = rn; return;
                case 0x0B: {  // jsr
                    delayed(at, rn, true, true);
                    return;
                }
                case 0x0E: set_sr(rn); return;
                case 0x10: --rn; t = rn == 0; return;  // dt
                case 0x11: t = static_cast<int32_t>(rn) >= 0; return;
                case 0x12: rn -= 4; wr<uint32_t>(rn, cpu::macl); return;
                case 0x13: rn -= 4; wr<uint32_t>(rn, cpu::gbr); return;
                case 0x15: t = static_cast<int32_t>(rn) > 0; return;
                case 0x16: cpu::macl = rd<uint32_t>(rn); rn += 4; return;
                case 0x17: { const uint32_t v = rd<uint32_t>(rn); rn += 4; cpu::set_gbr(v); return; }
                case 0x18: rn <<= 8; return;
                case 0x19: rn >>= 8; return;
                case 0x1A: cpu::macl = rn; return;
                case 0x1B: {  // tas.b
                    const uint8_t v = rd<uint8_t>(rn);
                    t = v == 0;
                    wr<uint8_t>(rn, static_cast<uint8_t>(v | 0x80));
                    return;
                }
                case 0x1E: cpu::set_gbr(rn); return;
                case 0x20: t = rn >> 31; rn <<= 1; return;  // shal
                case 0x21: t = rn & 1; rn = static_cast<uint32_t>(static_cast<int32_t>(rn) >> 1); return;  // shar
                case 0x22: rn -= 4; wr<uint32_t>(rn, pr); return;
                case 0x23: rn -= 4; wr<uint32_t>(rn, cpu::vbr); return;
                case 0x24: {  // rotcl
                    const bool c = rn >> 31;
                    rn = (rn << 1) | (t ? 1 : 0);
                    t = c;
                    return;
                }
                case 0x25: {  // rotcr
                    const bool c = rn & 1;
                    rn = (rn >> 1) | (t ? 0x80000000u : 0);
                    t = c;
                    return;
                }
                case 0x26: pr = rd<uint32_t>(rn); rn += 4; return;
                case 0x27: { const uint32_t v = rd<uint32_t>(rn); rn += 4; cpu::set_vbr(v); return; }
                case 0x28: rn <<= 16; return;
                case 0x29: rn >>= 16; return;
                case 0x2A: pr = rn; return;
                case 0x2B: delayed(at, rn, true, false); return;  // jmp
                case 0x2E: cpu::set_vbr(rn); return;
                case 0x52: rn -= 4; wr<uint32_t>(rn, fpul); return;
                case 0x56: fpul = rd<uint32_t>(rn); rn += 4; return;
                case 0x5A: fpul = rn; return;
                case 0x62: rn -= 4; wr<uint32_t>(rn, fpscr); return;
                case 0x66: { const uint32_t v = rd<uint32_t>(rn); rn += 4; set_fpscr(v); return; }
                case 0x6A: set_fpscr(rn); return;
                default: break;
            }
            switch (op & 0xF) {
                case 0xC: {  // shad
                    const int32_t sh = static_cast<int32_t>(rmv);
                    if (sh >= 0)
                        rn <<= (sh & 31);
                    else if ((sh & 31) == 0)
                        rn = static_cast<int32_t>(rn) < 0 ? 0xFFFFFFFFu : 0;
                    else
                        rn = static_cast<uint32_t>(static_cast<int32_t>(rn) >> ((~sh & 31) + 1));
                    return;
                }
                case 0xD: {  // shld
                    const int32_t sh = static_cast<int32_t>(rmv);
                    if (sh >= 0)
                        rn <<= (sh & 31);
                    else if ((sh & 31) == 0)
                        rn = 0;
                    else
                        rn >>= ((~sh & 31) + 1);
                    return;
                }
                case 0xF: {  // mac.w
                    const int32_t a = static_cast<int16_t>(rd<uint16_t>(rmv));
                    r[mm] += 2;
                    const int32_t b = static_cast<int16_t>(rd<uint16_t>(r[n]));
                    r[n] += 2;
                    int64_t acc = static_cast<int64_t>((static_cast<uint64_t>(cpu::mach) << 32) | cpu::macl);
                    acc += static_cast<int64_t>(a) * b;
                    cpu::mach = static_cast<uint32_t>(acc >> 32);
                    cpu::macl = static_cast<uint32_t>(acc);
                    return;
                }
                default: unknown(op); return;
            }
        case 0x5: rn = rd<uint32_t>(rmv + (op & 0xF) * 4u); return;  // mov.l @(disp,rm),rn
        case 0x6:
            switch (op & 0xF) {
                case 0x0: rn = static_cast<uint32_t>(static_cast<int8_t>(rd<uint8_t>(rmv))); return;
                case 0x1: rn = static_cast<uint32_t>(static_cast<int16_t>(rd<uint16_t>(rmv))); return;
                case 0x2: rn = rd<uint32_t>(rmv); return;
                case 0x3: rn = rmv; return;
                case 0x4: {
                    const uint32_t v = static_cast<uint32_t>(static_cast<int8_t>(rd<uint8_t>(rmv)));
                    if (n != mm) r[mm] += 1;
                    rn = v;
                    return;
                }
                case 0x5: {
                    const uint32_t v = static_cast<uint32_t>(static_cast<int16_t>(rd<uint16_t>(rmv)));
                    if (n != mm) r[mm] += 2;
                    rn = v;
                    return;
                }
                case 0x6: {
                    const uint32_t v = rd<uint32_t>(rmv);
                    if (n != mm) r[mm] += 4;
                    rn = v;
                    return;
                }
                case 0x7: rn = ~rmv; return;
                case 0x8: rn = (rmv & 0xFFFF0000u) | ((rmv & 0xFF) << 8) | ((rmv >> 8) & 0xFF); return;
                case 0x9: rn = (rmv << 16) | (rmv >> 16); return;
                case 0xA: {  // negc
                    const uint32_t tmp = 0u - rmv;
                    rn = tmp - (t ? 1 : 0);
                    t = 0 < tmp || tmp < rn;
                    return;
                }
                case 0xB: rn = 0u - rmv; return;
                case 0xC: rn = rmv & 0xFF; return;
                case 0xD: rn = rmv & 0xFFFF; return;
                case 0xE: rn = static_cast<uint32_t>(static_cast<int8_t>(rmv)); return;
                case 0xF: rn = static_cast<uint32_t>(static_cast<int16_t>(rmv)); return;
                default: unknown(op); return;
            }
        case 0x7: rn += static_cast<uint32_t>(imm8); return;
        case 0x8:
            switch (n) {
                case 0x0: wr<uint8_t>(r[mm] + (op & 0xF), static_cast<uint8_t>(r[0])); return;
                case 0x1: wr<uint16_t>(r[mm] + (op & 0xF) * 2u, static_cast<uint16_t>(r[0])); return;
                case 0x4: r[0] = static_cast<uint32_t>(static_cast<int8_t>(rd<uint8_t>(r[mm] + (op & 0xF)))); return;
                case 0x5:
                    r[0] = static_cast<uint32_t>(static_cast<int16_t>(rd<uint16_t>(r[mm] + (op & 0xF) * 2u)));
                    return;
                case 0x8: t = r[0] == static_cast<uint32_t>(imm8); return;
                case 0x9: if (t) pc = at + 4 + imm8 * 2; return;  // bt
                case 0xB: if (!t) pc = at + 4 + imm8 * 2; return;  // bf
                case 0xD: {  // bt/s
                    const bool take = t;
                    const uint32_t target = at + 4 + imm8 * 2;
                    execute(rd<uint16_t>(at + 2), at + 2, true);
                    pc = take ? target : at + 4;
                    return;
                }
                case 0xF: {  // bf/s
                    const bool take = !t;
                    const uint32_t target = at + 4 + imm8 * 2;
                    execute(rd<uint16_t>(at + 2), at + 2, true);
                    pc = take ? target : at + 4;
                    return;
                }
                default: unknown(op); return;
            }
        case 0x9: rn = static_cast<uint32_t>(static_cast<int16_t>(rd<uint16_t>(at + 4 + (op & 0xFF) * 2u))); return;
        case 0xA: {  // bra
            const int32_t d = static_cast<int32_t>(static_cast<uint32_t>(op & 0xFFF) << 20) >> 20;
            delayed(at, at + 4 + d * 2, false, false);
            return;
        }
        case 0xB: {  // bsr
            const int32_t d = static_cast<int32_t>(static_cast<uint32_t>(op & 0xFFF) << 20) >> 20;
            delayed(at, at + 4 + d * 2, true, true);
            return;
        }
        case 0xC: {
            const uint32_t gbr = cpu::gbr, d = op & 0xFF;
            switch (n) {
                case 0x0: wr<uint8_t>(gbr + d, static_cast<uint8_t>(r[0])); return;
                case 0x1: wr<uint16_t>(gbr + d * 2, static_cast<uint16_t>(r[0])); return;
                case 0x2: wr<uint32_t>(gbr + d * 4, r[0]); return;
                case 0x3: unknown(op); return;  // trapa
                case 0x4: r[0] = static_cast<uint32_t>(static_cast<int8_t>(rd<uint8_t>(gbr + d))); return;
                case 0x5: r[0] = static_cast<uint32_t>(static_cast<int16_t>(rd<uint16_t>(gbr + d * 2))); return;
                case 0x6: r[0] = rd<uint32_t>(gbr + d * 4); return;
                case 0x7: r[0] = ((at + 4) & ~3u) + d * 4; return;  // mova
                case 0x8: t = (r[0] & d) == 0; return;
                case 0x9: r[0] &= d; return;
                case 0xA: r[0] ^= d; return;
                case 0xB: r[0] |= d; return;
                case 0xC: t = (rd<uint8_t>(gbr + r[0]) & d) == 0; return;
                case 0xD: wr<uint8_t>(gbr + r[0], static_cast<uint8_t>(rd<uint8_t>(gbr + r[0]) & d)); return;
                case 0xE: wr<uint8_t>(gbr + r[0], static_cast<uint8_t>(rd<uint8_t>(gbr + r[0]) ^ d)); return;
                default: wr<uint8_t>(gbr + r[0], static_cast<uint8_t>(rd<uint8_t>(gbr + r[0]) | d)); return;
            }
        }
        case 0xD: rn = rd<uint32_t>(((at + 4) & ~3u) + (op & 0xFF) * 4u); return;
        case 0xE: rn = static_cast<uint32_t>(imm8); return;
        default: break;
    }

    // ---- the FPU (1111) ---------------------------------------------------------------------------
    float& fn = fr[n];
    const float fm = fr[mm];
    switch (op & 0xF) {
        case 0x0: fn += fm; return;
        case 0x1: fn -= fm; return;
        case 0x2: fn *= fm; return;
        case 0x3: fn /= fm; return;
        case 0x4: t = fn == fm; return;
        case 0x5: t = fn > fm; return;
        case 0x6:  // fmov.s @(r0,rm),frn
            if (sz) {
                const uint32_t a = r[0] + rmv;
                pair(n, 0) = from_bits(rd<uint32_t>(a));
                pair(n, 1) = from_bits(rd<uint32_t>(a + 4));
            }
            else {
                fn = from_bits(rd<uint32_t>(r[0] + rmv));
            }
            return;
        case 0x7:  // fmov.s frm,@(r0,rn)
            if (sz) {
                const uint32_t a = r[0] + rn;
                wr<uint32_t>(a, bits(pair(mm, 0)));
                wr<uint32_t>(a + 4, bits(pair(mm, 1)));
            }
            else {
                wr<uint32_t>(r[0] + rn, bits(fm));
            }
            return;
        case 0x8:  // fmov.s @rm,frn
            if (sz) {
                pair(n, 0) = from_bits(rd<uint32_t>(rmv));
                pair(n, 1) = from_bits(rd<uint32_t>(rmv + 4));
            }
            else {
                fn = from_bits(rd<uint32_t>(rmv));
            }
            return;
        case 0x9:  // fmov.s @rm+,frn
            if (sz) {
                pair(n, 0) = from_bits(rd<uint32_t>(rmv));
                pair(n, 1) = from_bits(rd<uint32_t>(rmv + 4));
                r[mm] += 8;
            }
            else {
                fn = from_bits(rd<uint32_t>(rmv));
                r[mm] += 4;
            }
            return;
        case 0xA:  // fmov.s frm,@rn
            if (sz) {
                wr<uint32_t>(rn, bits(pair(mm, 0)));
                wr<uint32_t>(rn + 4, bits(pair(mm, 1)));
            }
            else {
                wr<uint32_t>(rn, bits(fm));
            }
            return;
        case 0xB:  // fmov.s frm,@-rn
            if (sz) {
                rn -= 8;
                wr<uint32_t>(rn, bits(pair(mm, 0)));
                wr<uint32_t>(rn + 4, bits(pair(mm, 1)));
            }
            else {
                rn -= 4;
                wr<uint32_t>(rn, bits(fm));
            }
            return;
        case 0xC:  // fmov frm,frn
            if (sz) {
                const float a = pair(mm, 0), b = pair(mm, 1);
                pair(n, 0) = a;
                pair(n, 1) = b;
            }
            else {
                fn = fm;
            }
            return;
        case 0xD:
            switch (mm) {
                case 0x0: fn = from_bits(fpul); return;  // fsts
                case 0x1: fpul = bits(fn); return;       // flds
                case 0x2: fn = static_cast<float>(static_cast<int32_t>(fpul)); return;  // float
                case 0x3: {  // ftrc
                    if (std::isnan(fn))
                        fpul = 0x80000000u;
                    else if (fn >= 2147483648.0f)
                        fpul = 0x7FFFFFFFu;
                    else if (fn < -2147483648.0f)
                        fpul = 0x80000000u;
                    else
                        fpul = static_cast<uint32_t>(static_cast<int32_t>(fn));
                    return;
                }
                case 0x4: fn = -fn; return;
                case 0x5: fn = std::fabs(fn); return;
                case 0x6: fn = std::sqrt(fn); return;
                case 0x7: fn = 1.0f / std::sqrt(fn); return;  // fsrra
                case 0x8: fn = 0.0f; return;
                case 0x9: fn = 1.0f; return;
                case 0xA: case 0xB: unknown(op); return;  // fcnvsd, fcnvds: double precision
                case 0xE: {  // fipr fvm,fvn: exact products summed in double, rounded once
                    const int vn = (op >> 10) & 3, vm = (op >> 8) & 3;
                    double sum = 0;
                    for (int i = 0; i < 4; ++i)
                        sum += static_cast<double>(fr[vn * 4 + i]) * fr[vm * 4 + i];
                    fr[vn * 4 + 3] = static_cast<float>(sum);
                    return;
                }
                case 0xF:
                    if ((op & 0x0300) == 0x0100) {  // ftrv xmtrx,fvn: each row as fipr
                        const int vn = (op >> 10) & 3;
                        double v[4], out[4];
                        for (int i = 0; i < 4; ++i)
                            v[i] = fr[vn * 4 + i];
                        for (int i = 0; i < 4; ++i)
                            out[i] = xf(i) * v[0] + xf(4 + i) * v[1] + xf(8 + i) * v[2] + xf(12 + i) * v[3];
                        for (int i = 0; i < 4; ++i)
                            fr[vn * 4 + i] = static_cast<float>(out[i]);
                        return;
                    }
                    if (op == 0xFBFD) {  // frchg
                        fpscr ^= kFR;
                        swap_banks();
                        return;
                    }
                    if (op == 0xF3FD) {  // fschg
                        fpscr ^= kSZ;
                        return;
                    }
                    // fsca fpul,drn reads the hardware's table, not sin and cos: at 90 degrees the
                    // table's cosine is 0, where std::cos gave -4.4e-8. The turrets of stage 2 take
                    // their bullets' size from such a coordinate when it is 0, and with the
                    // residue the bullets were too small to see.
                    if ((op & 0x00FF) == 0xFD && !(op & 0x0100)) {
                        const dream::sh4::detail::SinCos& e = dream::sh4::detail::fsca_table()[fpul & 0xFFFF];
                        fr[n & 0xE] = e.s;
                        fr[(n & 0xE) + 1] = e.c;
                        return;
                    }
                    unknown(op);
                    return;
                default: unknown(op); return;
            }
        case 0xE: fn = std::fma(fr[0], fm, fn); return;  // fmac: the SH-4 rounds once
        default: unknown(op); return;
    }
}

}  // namespace

// IKARUGA_SH4_PROFILE: per function that C++ calls on the interpreter, its calls and the
// instructions they ran, nested calls included.
std::map<uint32_t, std::pair<uint64_t, uint64_t>>& profile() {
    static std::map<uint32_t, std::pair<uint64_t, uint64_t>> m;
    return m;
}
const bool g_profiling = std::getenv("IKARUGA_SH4_PROFILE") != nullptr;

void report_profile(std::FILE* out) {
    if (!g_profiling)
        return;
    std::vector<std::pair<uint32_t, std::pair<uint64_t, uint64_t>>> rows(profile().begin(), profile().end());
    std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) { return a.second.second > b.second.second; });
    uint64_t total = 0;
    for (const auto& r : rows) total += r.second.second;
    std::fprintf(out, "interpreted instructions by function called from C++ (%llu in all):\n",
                 static_cast<unsigned long long>(total));
    for (size_t i = 0; i < rows.size() && i < 40; ++i)
        std::fprintf(out, "  %08x %12llu instructions in %10llu calls\n", rows[i].first,
                     static_cast<unsigned long long>(rows[i].second.second),
                     static_cast<unsigned long long>(rows[i].second.first));
}

CallResult run(uint32_t entry, const CallArgs& args) {
    if (diff::g_shadow || diff::g_testing)
        diff::log_interpreted(entry, args);
    Cpu c;
    c.fpscr = cpu::fpscr & ~(kFR | kSZ);
    // A function that reads an object in a callee-saved register finds the C++ caller's there.
    for (int i = 8; i < 15; ++i)
        c.r[i] = cpu::ctx[i];
    for (int i = 0; i < 4; ++i)
        c.r[4 + i] = args.r[i];
    for (int i = 0; i < 8; ++i)
        c.fr[4 + i] = args.fr[i];
    // The stack arguments go below the guest stack's top, where the callee finds them at r15.
    const uint32_t top = (guest::sp - guest::Frame::kPushRoom) & ~7u;
    c.r[15] = top - 4u * static_cast<uint32_t>(args.nstack);
    for (int i = 0; i < args.nstack; ++i)
        guest::store<uint32_t>(c.r[15] + 4u * static_cast<uint32_t>(i), args.stack[i]);
    c.pc = entry;
    c.pr = kReturn;
    c.run_until_return();
    if (g_profiling) {
        auto& row = profile()[entry];
        ++row.first;
        row.second += c.executed;
    }
    CallResult res;
    res.r0 = c.r[0];
    res.r1 = c.r[1];
    res.fr0 = c.fr[0];
    res.fr1 = c.fr[1];
    return res;
}

CallResult run_regs(uint32_t entry, const uint32_t (&r)[16], const float (&fr)[16]) {
    g_limit = 1000000;
    Cpu c;
    c.fpscr = cpu::fpscr & ~(kFR | kSZ);
    std::copy(r, r + 16, c.r);
    std::copy(fr, fr + 16, c.fr);
    c.pc = entry;
    c.pr = kReturn;
    c.run_until_return();
    g_limit = 0;
    CallResult res;
    res.r0 = c.r[0];
    res.r1 = c.r[1];
    res.fr0 = c.fr[0];
    res.fr1 = c.fr[1];
    return res;
}

void report(std::FILE* out) {
    if (g_active)
        std::fprintf(out, "sh4: interpreting at %08x (pr %08x, r15 %08x, %llu instructions in this call)\n",
                     g_active->pc, g_active->pr, g_active->r[15], static_cast<unsigned long long>(g_active->executed));
}

uint32_t active_stack() { return g_active ? g_active->r[15] : ~0u; }

bool interpret_all() {
    static const bool on = std::getenv("IKARUGA_INTERPRET_ALL") != nullptr;
    return on;
}

bool interpreted(uint32_t address) {
    if (interpret_all()) {
        const uint32_t p1 = (address & 0x1FFFFFFFu) | 0x80000000u;
        return p1 >= 0x8C010000u && p1 < guest::image_end &&
               !std::binary_search(kNativeOnly, kNativeOnly + kNativeOnlyCount, p1);
    }
    return std::binary_search(kInterpreted, kInterpreted + kInterpretedCount, address);
}

}  // namespace sh4
