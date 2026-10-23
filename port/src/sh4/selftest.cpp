// The interpreter against native code: the SHC helpers' division with div1 and modulo, a point and
// a vector through a matrix with ftrv, and Ninja's native functions (port/src/ninja.cpp) bit for
// bit from random states. Run with IKARUGA_SH4_TEST=1.
#include <cmath>
#include <cstdio>
#include <cstring>
#include <random>
#include <vector>

#include "../hw.h"
#include "decomp.h"
#include "sh4.h"

namespace sh4 {

namespace {
constexpr uint32_t kScratch = 0x8CF00000u;  // free main RAM before the game starts

int compare_division(const char* name, uint32_t entry, uint32_t (*native)(uint32_t, uint32_t)) {
    std::mt19937 rng(1);
    int bad = 0;
    for (int i = 0; i < 2000; ++i) {
        uint32_t divisor = rng(), dividend = rng();
        if (i % 3 == 0) divisor &= 0xFF;
        if (i % 5 == 0) divisor = static_cast<uint32_t>(-static_cast<int32_t>(divisor & 0xFFF));
        if (divisor == 0) divisor = 7;
        uint32_t r[16] = {};
        float fr[16] = {};
        r[0] = divisor;
        r[1] = dividend;
        r[15] = 0x8CFF0000u;
        const uint32_t got = run_regs(entry, r, fr).r0;
        const uint32_t want = native(divisor, dividend);
        if (got != want && bad++ < 4)
            std::printf("  %s(%d, %d): interpreted %d, native %d\n", name, static_cast<int32_t>(divisor),
                        static_cast<int32_t>(dividend), static_cast<int32_t>(got), static_cast<int32_t>(want));
    }
    return bad;
}
// Ninja's matrix functions, decompiled against interpreted, on a matrix in memory and on the
// current matrix in the xf bank (a null pointer).
const float kStart[16] = {0.8f, 0.1f, -0.5f, 0, -0.2f, 0.9f, 0.3f, 0, 0.55f, -0.35f, 0.75f, 0, 5, -7, 11, 1};
const float kOther[16] = {1.5f, -0.25f, 0.1f, 0, 0.3f, 0.6f, -0.9f, 0, -0.4f, 0.2f, 1.1f, 0, -3, 4, 2.5f, 1};
constexpr uint32_t kA = kScratch + 0x100, kB = kScratch + 0x200, kC = kScratch + 0x300;

void put(uint32_t at, const float (&m)[16]) {
    for (int i = 0; i < 16; ++i) guest::store<float>(at + 4u * static_cast<uint32_t>(i), m[i]);
}
void reset_xf() {
    for (int i = 0; i < 16; ++i) cpu::xf[i ^ 1] = kStart[i];
}

struct Test {
    const char* name;
    uint32_t entry;
    bool null_matrix;  // operate on the current matrix
    void (*decompiled)(uint32_t m);
    uint32_t r5, r6;
    float f[4];
};

int run_test(const Test& t) {
    put(kA, kStart);
    put(kB, kStart);
    put(kC, kOther);
    reset_xf();
    t.decompiled(t.null_matrix ? 0 : kA);
    float xf_c[16], mem_c[16];
    for (int i = 0; i < 16; ++i) {
        xf_c[i] = cpu::xf[i ^ 1];
        mem_c[i] = guest::load<float>(kA + 4u * static_cast<uint32_t>(i));
    }
    reset_xf();
    uint32_t r[16] = {};
    float fr[16] = {};
    r[4] = t.null_matrix ? 0 : kB;
    r[5] = t.r5;
    r[6] = t.r6;
    r[15] = 0x8CFF0000u;
    for (int i = 0; i < 4; ++i) fr[4 + i] = t.f[i];
    run_regs(t.entry, r, fr);
    int bad = 0;
    for (int i = 0; i < 16; ++i) {
        const float xi = cpu::xf[i ^ 1], mi = guest::load<float>(kB + 4u * static_cast<uint32_t>(i));
        auto differ = [](float a, float b) { return !(std::fabs(a - b) <= 1e-4f * (1 + std::fabs(a))); };
        if (differ(xi, xf_c[i]) || differ(mi, mem_c[i])) {
            if (bad++ < 3)
                std::printf("  %s%s [%d]: xf interpreted %g decompiled %g, memory interpreted %g decompiled %g\n",
                            t.name, t.null_matrix ? " (current)" : "", i, xi, xf_c[i], mi, mem_c[i]);
        }
    }
    return bad ? 1 : 0;
}

int matrix_tests() {
    using namespace game;
    auto M = [](uint32_t m) { return gptr<NJS_MATRIX>(m); };
    const Test tests[] = {
        {"njUnitMatrix", 0x8C015658u, false, [](uint32_t m) { njUnitMatrix(gptr<NJS_MATRIX>(m)); }, 0, 0, {}},
        {"njUnitMatrix", 0x8C015658u, true, [](uint32_t m) { njUnitMatrix(gptr<NJS_MATRIX>(m)); }, 0, 0, {}},
        {"njMultiMatrix", 0x8C0150BCu, false,
         [](uint32_t m) { njMultiMatrix(gptr<NJS_MATRIX>(m), gptr<NJS_MATRIX>(kC)); }, kC, 0, {}},
        {"njMultiMatrix", 0x8C0150BCu, true,
         [](uint32_t m) { njMultiMatrix(gptr<NJS_MATRIX>(m), gptr<NJS_MATRIX>(kC)); }, kC, 0, {}},
        {"njTranslate", 0x8C0155F0u, false, [](uint32_t m) { njTranslate(gptr<NJS_MATRIX>(m), 1.5f, -2, 3.25f); },
         0, 0, {1.5f, -2, 3.25f, 0}},
        {"njTranslate", 0x8C0155F0u, true, [](uint32_t m) { njTranslate(gptr<NJS_MATRIX>(m), 1.5f, -2, 3.25f); },
         0, 0, {1.5f, -2, 3.25f, 0}},
        {"njRotateX", 0x8C015310u, false, [](uint32_t m) { njRotateX(gptr<NJS_MATRIX>(m), 0x1234); }, 0x1234, 0, {}},
        {"njRotateX", 0x8C015310u, true, [](uint32_t m) { njRotateX(gptr<NJS_MATRIX>(m), 0x1234); }, 0x1234, 0, {}},
        {"njRotateY", 0x8C0153A4u, false, [](uint32_t m) { njRotateY(gptr<NJS_MATRIX>(m), 0x2345); }, 0x2345, 0, {}},
        {"njRotateY", 0x8C0153A4u, true, [](uint32_t m) { njRotateY(gptr<NJS_MATRIX>(m), 0x2345); }, 0x2345, 0, {}},
        {"njRotateZ", 0x8C015440u, false, [](uint32_t m) { njRotateZ(gptr<NJS_MATRIX>(m), 0x3456); }, 0x3456, 0, {}},
        {"njRotateZ", 0x8C015440u, true, [](uint32_t m) { njRotateZ(gptr<NJS_MATRIX>(m), 0x3456); }, 0x3456, 0, {}},
        {"njScale", 0x8C0154DCu, false, [](uint32_t m) { njScale(gptr<NJS_MATRIX>(m), 2, 0.5f, -1.5f); }, 0, 0,
         {2, 0.5f, -1.5f, 0}},
        {"njScale", 0x8C0154DCu, true, [](uint32_t m) { njScale(gptr<NJS_MATRIX>(m), 2, 0.5f, -1.5f); }, 0, 0,
         {2, 0.5f, -1.5f, 0}},
        {"njInvertMatrix", 0x8C0148B4u, false, [](uint32_t m) { njInvertMatrix(gptr<NJS_MATRIX>(m)); }, 0, 0, {}},
        {"njInvertMatrix", 0x8C0148B4u, true, [](uint32_t m) { njInvertMatrix(gptr<NJS_MATRIX>(m)); }, 0, 0, {}},
        {"njSetMatrix", 0x8C01557Cu, false,
         [](uint32_t m) { njSetMatrix(gptr<NJS_MATRIX>(m), gptr<NJS_MATRIX>(kC)); }, kC, 0, {}},
        {"njSetMatrix", 0x8C01557Cu, true,
         [](uint32_t m) { njSetMatrix(gptr<NJS_MATRIX>(m), gptr<NJS_MATRIX>(kC)); }, kC, 0, {}},
        {"njGetMatrix", 0x8C014714u, false, [](uint32_t m) { njGetMatrix(gptr<NJS_MATRIX>(m)); }, 0, 0, {}},
    };
    (void)M;
    int bad = 0;
    for (const Test& t : tests) bad += run_test(t);
    return bad;
}

// Ninja's native functions (port/src/ninja.cpp) against their machine code, bit for bit, from
// the same random states: the current matrix, a matrix and a point in memory, and the arguments,
// with zeros, infinities, NaNs, denormals and large values among them.
float random_float(std::mt19937& rng) {
    const uint32_t kind = rng() % 16;
    if (kind == 0) {
        const float specials[] = {0.0f, -0.0f, 1.0f, -1.0f, INFINITY, -INFINITY, NAN, 1e-40f, -1e-40f, 3e38f, -3e38f};
        return specials[rng() % (sizeof specials / sizeof specials[0])];
    }
    if (kind == 1) {
        const uint32_t b = rng();
        float f;
        std::memcpy(&f, &b, 4);
        return f;
    }
    return std::uniform_real_distribution<float>(-100.0f, 100.0f)(rng);
}

uint32_t as_bits(float f) {
    uint32_t b;
    std::memcpy(&b, &f, 4);
    return b;
}

// Two words are the same, or both NaNs. A NaN's payload depends on which operand the host's
// multiply takes first, which the compiler chooses, and neither build models the SH-4's NaNs.
bool same(uint32_t a, uint32_t b) {
    auto nan = [](uint32_t w) { return (w & 0x7F800000u) == 0x7F800000u && (w & 0x007FFFFFu) != 0; };
    return a == b || (nan(a) && nan(b));
}

// Each test runs the native function and then the machine code from the same state, and compares
// the current matrix, the scratch memory (the matrices, the point, the matrix stack and Ninja's
// context) and the result.
enum class Result { kNone, kFloat, kWord };

struct Exact {
    const char* name;
    uint32_t entry;
    Result result;
    bool current;  // also with a null matrix: on the current matrix
    // Calls the native function; m is the matrix or point argument (0 for the current matrix).
    double (*native)(uint32_t m, int32_t angle, const float (&f)[3]);
    // The machine code's argument registers r4 and r5, for the same call.
    uint32_t (*r4)(uint32_t m, int32_t angle);
    uint32_t (*r5)(uint32_t m, int32_t angle);
};

constexpr uint32_t kM = kScratch + 0x100, kM2 = kScratch + 0x200, kContext = kScratch + 0x400,
                   kStack = kScratch + 0x800, kScratchEnd = kScratch + 0xC00;

int ninja_exact(uint32_t fpscr) {
    using namespace game;
    cpu::set_fpscr(fpscr);
    const uint32_t gbr = cpu::gbr;
    cpu::gbr = kContext;
    auto zero = [](uint32_t, int32_t) { return 0u; };
    auto matrix = [](uint32_t m, int32_t) { return m; };
    auto angle = [](uint32_t, int32_t a) { return static_cast<uint32_t>(a); };
    auto second = [](uint32_t, int32_t) { return kM2; };
    const Exact tests[] = {
        {"njSin", 0x8C015740u, Result::kFloat, false, [](uint32_t, int32_t a, const float (&)[3]) -> double { return njSin(a); }, angle, zero},
        {"njCos", 0x8C015730u, Result::kFloat, false, [](uint32_t, int32_t a, const float (&)[3]) -> double { return njCos(a); }, angle, zero},
        {"njSqrt", 0x8C01574Cu, Result::kFloat, false, [](uint32_t, int32_t, const float (&f)[3]) -> double { return njSqrt(f[0]); }, zero, zero},
        {"njUnitMatrix", 0x8C015658u, Result::kNone, true,
         [](uint32_t m, int32_t, const float (&)[3]) -> double { njUnitMatrix(gptr<NJS_MATRIX>(m)); return 0; }, matrix, zero},
        {"njSetMatrix", 0x8C01557Cu, Result::kNone, true,
         [](uint32_t m, int32_t, const float (&)[3]) -> double { njSetMatrix(gptr<NJS_MATRIX>(m), gptr<NJS_MATRIX>(kM2)); return 0; },
         matrix, second},
        {"njGetMatrix", 0x8C014714u, Result::kNone, false,
         [](uint32_t m, int32_t, const float (&)[3]) -> double { njGetMatrix(gptr<NJS_MATRIX>(m)); return 0; }, matrix, zero},
        {"njRotateX", 0x8C015310u, Result::kNone, true,
         [](uint32_t m, int32_t a, const float (&)[3]) -> double { njRotateX(gptr<NJS_MATRIX>(m), a); return 0; }, matrix, angle},
        {"njRotateY", 0x8C0153A4u, Result::kNone, true,
         [](uint32_t m, int32_t a, const float (&)[3]) -> double { njRotateY(gptr<NJS_MATRIX>(m), a); return 0; }, matrix, angle},
        {"njRotateZ", 0x8C015440u, Result::kNone, true,
         [](uint32_t m, int32_t a, const float (&)[3]) -> double { njRotateZ(gptr<NJS_MATRIX>(m), a); return 0; }, matrix, angle},
        {"njScale", 0x8C0154DCu, Result::kNone, true,
         [](uint32_t m, int32_t, const float (&f)[3]) -> double { njScale(gptr<NJS_MATRIX>(m), f[0], f[1], f[2]); return 0; },
         matrix, zero},
        {"njTranslate", 0x8C0155F0u, Result::kNone, true,
         [](uint32_t m, int32_t, const float (&f)[3]) -> double { njTranslate(gptr<NJS_MATRIX>(m), f[0], f[1], f[2]); return 0; },
         matrix, zero},
        {"njTranslateEx", 0x8C0144F0u, Result::kNone, false,
         [](uint32_t, int32_t, const float (&)[3]) -> double { njTranslateEx(gptr<NJS_VECTOR>(kM2)); return 0; }, second, zero},
        {"njRotateEx", 0x8C014418u, Result::kNone, false,
         [](uint32_t, int32_t a, const float (&)[3]) -> double { njRotateEx(gptr<Angle>(kM2), a); return 0; }, second, angle},
        {"njScaleEx", 0x8C0144B0u, Result::kNone, false,
         [](uint32_t, int32_t, const float (&)[3]) -> double { njScaleEx(gptr<NJS_VECTOR>(kM2)); return 0; }, second, zero},
        {"njCalcPointEx", 0x8C014250u, Result::kNone, false,
         [](uint32_t m, int32_t, const float (&)[3]) -> double {
             njCalcPointEx(gptr<NJS_POINT3>(kM2), gptr<NJS_POINT3>(m));
             return 0;
         }, second, matrix},
        {"njUnitRotPortion", 0x8C0156C4u, Result::kNone, true,
         [](uint32_t m, int32_t, const float (&)[3]) -> double { njUnitRotPortion(gptr<NJS_MATRIX>(m)); return 0; }, matrix, zero},
        {"njGetTranslation", 0x8C014740u, Result::kNone, true,
         [](uint32_t m, int32_t, const float (&)[3]) -> double {
             njGetTranslation(gptr<NJS_MATRIX>(m), gptr<NJS_POINT3>(kM2));
             return 0;
         }, matrix, second},
        {"njCalcScreen", 0x8C02F654u, Result::kWord, false,
         [](uint32_t, int32_t, const float (&)[3]) -> double {
             return njCalcScreen(gptr<NJS_POINT3>(kM2), gptr<Float>(kM2 + 16), gptr<Float>(kM2 + 20));
         }, second, [](uint32_t, int32_t) { return kM2 + 16; }},
        {"njCollisionCheckSS", 0x8C0159E8u, Result::kWord, false,
         [](uint32_t m, int32_t, const float (&)[3]) -> double {
             return njCollisionCheckSS(gptr<NJS_SPHERE>(m), gptr<NJS_SPHERE>(kM2));
         }, matrix, second},
        {"njPushMatrixEx", 0x8C01429Cu, Result::kWord, false,
         [](uint32_t, int32_t, const float (&)[3]) -> double { return njPushMatrixEx(); }, zero, zero},
        {"njPopMatrixEx", 0x8C01426Cu, Result::kWord, false,
         [](uint32_t, int32_t, const float (&)[3]) -> double { return njPopMatrixEx(); }, zero, zero},
    };
    std::mt19937 rng(7);
    std::vector<uint8_t> before(kScratchEnd - kScratch), after(kScratchEnd - kScratch);
    int bad = 0;
    for (const Exact& t : tests) {
        int failures = 0;
        for (int round = 0; round < 4000; ++round) {
            const bool current = t.current && round % 2 == 0;
            float x0[16], f[3];
            for (float& v : x0) v = random_float(rng);
            for (float& v : f) v = random_float(rng);
            for (uint32_t a = kScratch; a < kM2 + 64; a += 4) guest::store<float>(a, random_float(rng));
            // A stack of four matrices, with the last pushed one anywhere from empty to full.
            for (uint32_t a = kStack; a < kStack + 6 * 64; a += 4) guest::store<float>(a, random_float(rng));
            guest::store<uint32_t>(kContext + 384, kStack);
            guest::store<uint32_t>(kContext + 388, kStack + 64 * (rng() % 5));
            guest::store<uint32_t>(kContext + 392, kStack + 64 * 4);
            // Ninja's projection and screen bounds, which njCalcScreen reads.
            for (uint32_t a = 0x8C10AF34u; a < 0x8C10AF8Cu; a += 4) guest::store<float>(a, random_float(rng));
            int32_t ang = static_cast<int32_t>(rng());
            if (round % 7 == 0) ang = static_cast<int32_t>((rng() % 4) * 0x4000u);
            const uint32_t m = current ? 0 : kM;
            for (int i = 0; i < 16; ++i) cpu::xf[i ^ 1] = x0[i];
            for (uint32_t a = kScratch; a < kScratchEnd; ++a) before[a - kScratch] = guest::load<uint8_t>(a);

            const double native = t.native(m, ang, f);
            uint32_t xf_n[16];
            for (int i = 0; i < 16; ++i) xf_n[i] = as_bits(cpu::xf[i ^ 1]);
            for (uint32_t a = kScratch; a < kScratchEnd; ++a) after[a - kScratch] = guest::load<uint8_t>(a);

            for (uint32_t a = kScratch; a < kScratchEnd; ++a) guest::store<uint8_t>(a, before[a - kScratch]);
            for (int i = 0; i < 16; ++i) cpu::xf[i ^ 1] = x0[i];
            uint32_t r[16] = {};
            float fr[16] = {};
            r[4] = t.r4(m, ang);
            r[5] = t.r5(m, ang);
            r[6] = kM2 + 20;  // njCalcScreen's second result
            r[15] = 0x8CFF0000u;
            for (int i = 0; i < 3; ++i) fr[4 + i] = f[i];
            const CallResult res = run_regs(t.entry, r, fr);

            bool differs = (t.result == Result::kFloat && !same(as_bits(res.fr0), as_bits(static_cast<float>(native)))) ||
                           (t.result == Result::kWord && res.r0 != static_cast<uint32_t>(native));
            for (int i = 0; i < 16; ++i) differs |= !same(as_bits(cpu::xf[i ^ 1]), xf_n[i]);
            for (uint32_t a = kScratch; a < kScratchEnd; a += 4) {
                uint32_t nv;
                std::memcpy(&nv, &after[a - kScratch], 4);
                differs |= !same(guest::load<uint32_t>(a), nv);
            }
            if (differs && failures++ < 3) {
                std::printf("  %s%s, round %d: differs from the machine code\n", t.name, current ? " (current)" : "", round);
                for (int i = 0; i < 16; ++i)
                    if (as_bits(cpu::xf[i ^ 1]) != xf_n[i])
                        std::printf("    xf%d: machine code %08x, native %08x, before %08x\n", i, as_bits(cpu::xf[i ^ 1]),
                                    xf_n[i], as_bits(x0[i]));
                for (uint32_t a = kScratch; a < kScratchEnd; a += 4) {
                    const uint32_t mc = guest::load<uint32_t>(a);
                    uint32_t nv, bf;
                    std::memcpy(&nv, &after[a - kScratch], 4);
                    std::memcpy(&bf, &before[a - kScratch], 4);
                    if (mc != nv)
                        std::printf("    %08x: machine code %08x, native %08x, before %08x\n", a, mc, nv, bf);
                }
                std::printf("    arguments %08x %08x %08x\n", as_bits(f[0]), as_bits(f[1]), as_bits(f[2]));
            }
        }
        if (failures)
            std::printf("  %s: %d of 4000 rounds differ (FPSCR %08x)\n", t.name, failures, fpscr);
        bad += failures ? 1 : 0;
    }
    cpu::gbr = gbr;
    return bad;
}

// Ninja's native drawing (port/src/ninja_draw.cpp) against its machine code: the store-queue
// flushes both make, with their blocks, from the same random vertices.
int ninja_draw_exact() {
    using namespace game;
    constexpr uint32_t kVertices = kScratch + 0x600, kNext = 0x8C10B360u;
    std::mt19937 rng(11);
    int failures = 0;
    for (int round = 0; round < 2000; ++round) {
        const int count = 2 + static_cast<int>(rng() % 7);
        for (uint32_t a = kVertices; a < kVertices + 24u * static_cast<uint32_t>(count); a += 4)
            guest::store<float>(a, random_float(rng));
        const uint32_t start = 0xE0000000u + (rng() & 0x03FFFFE0u);
        std::vector<hw::SqFlush> native, machine;
        guest::store<uint32_t>(kNext, start);
        hw::capture_flushes(&native);
        njDrawTexture2DExSetData(gptr<NJS_TEXTURE_VTX>(kVertices), count);
        hw::capture_flushes(nullptr);
        const uint32_t next_native = guest::load<uint32_t>(kNext);

        guest::store<uint32_t>(kNext, start);
        uint32_t r[16] = {};
        float fr[16] = {};
        r[4] = kVertices;
        r[5] = static_cast<uint32_t>(count);
        r[15] = 0x8CFF0000u;
        hw::capture_flushes(&machine);
        run_regs(0x8C0305ACu, r, fr);
        hw::capture_flushes(nullptr);
        bool differs = guest::load<uint32_t>(kNext) != next_native || native.size() != machine.size();
        for (std::size_t i = 0; !differs && i < native.size(); ++i)
            differs = native[i].address != machine[i].address ||
                      std::memcmp(native[i].words, machine[i].words, sizeof native[i].words) != 0;
        if (differs && failures++ < 3)
            std::printf("  njDrawTexture2DExSetData, round %d (%d vertices): %zu flushes natively, %zu by the machine code\n",
                        round, count, native.size(), machine.size());
    }
    if (failures)
        std::printf("  njDrawTexture2DExSetData: %d of 2000 rounds differ\n", failures);
    return failures ? 1 : 0;
}

}  // namespace

int self_test() {
    int bad = 0;
    bad += compare_division("_divls", 0x8C0615B0u, [](uint32_t d, uint32_t n) {
        return static_cast<uint32_t>(game::_divls(static_cast<int>(d), static_cast<int>(n)));
    });
    bad += compare_division("_divlu", 0x8C061664u, [](uint32_t d, uint32_t n) { return game::_divlu(d, n); });
    bad += compare_division("_modls", 0x8C06175Cu, [](uint32_t d, uint32_t n) {
        return static_cast<uint32_t>(game::_modls(static_cast<int>(d), static_cast<int>(n)));
    });

    // A matrix, a point and a vector in scratch RAM; njCalcPoint and njCalcVector, interpreted and
    // decompiled, against the product computed here.
    const uint32_t m = kScratch, ps = kScratch + 0x40, pd1 = kScratch + 0x50, pd2 = kScratch + 0x60;
    const float mat[16] = {0.5f, 1.0f, -2.0f, 0, 3.0f, 0.25f, 1.5f, 0, -1.0f, 2.0f, 0.75f, 0, 10.0f, -20.0f, 30.0f, 1};
    const float point[3] = {1.5f, -2.0f, 4.0f};
    for (int i = 0; i < 16; ++i) guest::store<float>(m + 4u * static_cast<uint32_t>(i), mat[i]);
    for (int i = 0; i < 3; ++i) guest::store<float>(ps + 4u * static_cast<uint32_t>(i), point[i]);
    for (int vector = 0; vector < 2; ++vector) {
        const char* name = vector ? "njCalcVector" : "njCalcPoint";
        float want[3];
        for (int i = 0; i < 3; ++i)
            want[i] = mat[i] * point[0] + mat[4 + i] * point[1] + mat[8 + i] * point[2] + (vector ? 0 : mat[12 + i]);
        if (vector)
            game::njCalcVector(gptr<game::NJS_MATRIX>(m), gptr<game::NJS_VECTOR>(ps), gptr<game::NJS_VECTOR>(pd1));
        else
            game::njCalcPoint(gptr<game::NJS_MATRIX>(m), gptr<game::NJS_POINT3>(ps), gptr<game::NJS_POINT3>(pd1));
        uint32_t r[16] = {};
        float fr[16] = {};
        r[4] = m;
        r[5] = ps;
        r[6] = pd2;
        r[15] = 0x8CFF0000u;
        run_regs(vector ? 0x8C014564u : 0x8C014508u, r, fr);
        for (int i = 0; i < 3; ++i) {
            const float c = guest::load<float>(pd1 + 4u * static_cast<uint32_t>(i));
            const float s = guest::load<float>(pd2 + 4u * static_cast<uint32_t>(i));
            if (std::fabs(s - want[i]) > 1e-3f || std::fabs(c - want[i]) > 1e-3f) {
                ++bad;
                std::printf("  %s[%d]: interpreted %g, decompiled %g, expected %g\n", name, i, s, c, want[i]);
            }
        }
    }
    bad += matrix_tests();
    bad += ninja_exact(0x00040000u);  // the game's mode: round to nearest, denormals flushed
    bad += ninja_exact(0x00000000u);
    bad += ninja_draw_exact();
    std::printf("sh4 self-test: %d differences\n", bad);
    return bad;
}

}  // namespace sh4
