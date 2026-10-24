// Ninja's matrix and trigonometry functions, natively. Ninja keeps the current matrix in the SH-4's
// back bank of FPU registers, xf0 to xf15 with a column in each four, from one call to the next.
// Here that bank is cpu::xf, which the interpreter and the decompiled code share. Each function
// does the arithmetic of its machine code in the same order. ftrv sums four products in double
// precision and rounds once, as the interpreter does, and sine and cosine come from fsca's table.
// Ninja code calls only the matrix stack, njGetMatrix, njTranslate, njTranslateEx, njRotateEx and
// njScaleEx, and each of those calls sets the registers it reads afterwards. IKARUGA_SH4_TEST compares each function with
// its machine code.
#include <cmath>
#include <cstring>

#include "decomp.h"
#include "diff.h"
#include "dream/runtime/sh4/ops.h"

namespace game {
namespace {

// Element n of the current matrix.
float& X(int n) { return cpu::xf[n ^ 1]; }

float get(gptr<NJS_MATRIX> m, int n) { return guest::load<float>(m.a + 4u * static_cast<uint32_t>(n)); }
void put(gptr<NJS_MATRIX> m, int n, float v) { guest::store<float>(m.a + 4u * static_cast<uint32_t>(n), v); }

// A word as a float and back, bit for bit: a copied matrix keeps every value, NaNs included.
uint32_t bits_of(float f) {
    uint32_t b;
    std::memcpy(&b, &f, 4);
    return b;
}
float from(uint32_t b) {
    float f;
    std::memcpy(&f, &b, 4);
    return f;
}

struct Vec4 {
    float v[4];
};

// ftrv: the matrix a times the vector (v0, v1, v2, v3).
Vec4 ftrv(const float (&a)[16], float v0, float v1, float v2, float v3) {
    Vec4 out;
    for (int i = 0; i < 4; ++i)
        out.v[i] = static_cast<float>(a[i] * static_cast<double>(v0) + a[4 + i] * static_cast<double>(v1) +
                                      a[8 + i] * static_cast<double>(v2) + a[12 + i] * static_cast<double>(v3));
    return out;
}

dream::sh4::detail::SinCos fsca(Angle angle) {
    return dream::sh4::detail::fsca_table()[static_cast<uint32_t>(angle) & 0xFFFFu];
}

// A rotation replaces two columns, a and b, with the matrix times the rotation's columns va and
// vb. For a matrix in memory, Ninja loads only those two columns over the current matrix, so the
// current matrix's other columns meet the rotation's zeros.
void rotate(gptr<NJS_MATRIX> m, int a, int b, const float (&va)[4], const float (&vb)[4]) {
    float mat[16];
    for (int i = 0; i < 16; ++i) mat[i] = X(i);
    if (m.a) {
        for (int i = 0; i < 4; ++i) {
            mat[4 * a + i] = get(m, 4 * a + i);
            mat[4 * b + i] = get(m, 4 * b + i);
        }
    }
    const Vec4 ra = ftrv(mat, va[0], va[1], va[2], va[3]);
    const Vec4 rb = ftrv(mat, vb[0], vb[1], vb[2], vb[3]);
    for (int i = 0; i < 4; ++i) {
        if (m.a) {
            put(m, 4 * a + i, ra.v[i]);
            put(m, 4 * b + i, rb.v[i]);
        }
        else {
            X(4 * a + i) = ra.v[i];
            X(4 * b + i) = rb.v[i];
        }
    }
}

constexpr uint32_t kIdentity = 0x8C0CF560u;  // Ninja's unit matrix

// The matrix stack, in Ninja's context at GBR: its base, the last matrix pushed, and its limit.
constexpr uint32_t kStackBase = 384, kStackTop = 388, kStackLimit = 392;

// fipr: the sum of four products in double precision, from +0, rounded once.
float fipr(const float (&a)[4], const float (&b)[4]) {
    double sum = 0;
    for (int i = 0; i < 4; ++i) sum += static_cast<double>(a[i]) * b[i];
    return static_cast<float>(sum);
}

}  // namespace

Float njSin(Angle n) { return fsca(n).s; }
Float njCos(Angle n) { return fsca(n).c; }

// The square root of a negative number is the negative root of its magnitude.
// The tangent as fsca's sine over its cosine.
Float njTan(Angle n) {
    diff::Check check("njTan", 0x8C01577Cu, n);
    const dream::sh4::detail::SinCos sc = fsca(n);
    return check.returns(sc.s / sc.c);
}
Float njSqrt(Float n) { return 0.0f > n ? -std::sqrt(std::fabs(n)) : std::sqrt(n); }

void njUnitMatrix(gptr<NJS_MATRIX> m) {
    for (int i = 0; i < 16; ++i) {
        const uint32_t b = guest::load<uint32_t>(kIdentity + 4u * static_cast<uint32_t>(i));
        if (m.a)
            guest::store<uint32_t>(m.a + 4u * static_cast<uint32_t>(i), b);
        else
            X(i) = from(b);
    }
}

void njSetMatrix(gptr<NJS_MATRIX> md, gptr<NJS_MATRIX> ms) {
    for (int i = 0; i < 16; ++i) {
        const uint32_t b = guest::load<uint32_t>(ms.a + 4u * static_cast<uint32_t>(i));
        if (md.a)
            guest::store<uint32_t>(md.a + 4u * static_cast<uint32_t>(i), b);
        else
            X(i) = from(b);
    }
}

void njRotateX(gptr<NJS_MATRIX> m, Angle ang) {
    const auto sc = fsca(ang);
    rotate(m, 1, 2, {0.0f, sc.c, sc.s, 0.0f}, {0.0f, -sc.s, sc.c, 0.0f});
}

void njRotateY(gptr<NJS_MATRIX> m, Angle ang) {
    const auto sc = fsca(ang);
    rotate(m, 0, 2, {sc.c, 0.0f, -sc.s, 0.0f}, {sc.s, 0.0f, sc.c, 0.0f});
}

void njRotateZ(gptr<NJS_MATRIX> m, Angle ang) {
    const auto sc = fsca(ang);
    rotate(m, 0, 1, {sc.c, sc.s, 0.0f, 0.0f}, {-sc.s, sc.c, 0.0f, 0.0f});
}

// The first three columns times sx, sy and sz.
void njScale(gptr<NJS_MATRIX> m, Float sx, Float sy, Float sz) {
    const float s[3] = {sx, sy, sz};
    for (int i = 0; i < 12; ++i) {
        if (m.a)
            put(m, i, get(m, i) * s[i / 4]);
        else
            X(i) = X(i) * s[i / 4];
    }
}

Bool njPushMatrixEx() {
    const uint32_t top = guest::load<uint32_t>(cpu::gbr + kStackTop);
    if (top == guest::load<uint32_t>(cpu::gbr + kStackLimit))
        return 0;
    const uint32_t at = top + 64;
    guest::store<uint32_t>(cpu::gbr + kStackTop, at);
    for (int i = 0; i < 16; ++i) guest::store<float>(at + 4u * static_cast<uint32_t>(i), X(i));
    return 1;
}

Bool njPopMatrixEx() {
    const uint32_t top = guest::load<uint32_t>(cpu::gbr + kStackTop);
    if (top == guest::load<uint32_t>(cpu::gbr + kStackBase))
        return 0;
    for (int i = 0; i < 16; ++i) X(i) = guest::load<float>(top + 4u * static_cast<uint32_t>(i));
    guest::store<uint32_t>(cpu::gbr + kStackTop, top - 64);
    return 1;
}

void njGetMatrix(gptr<NJS_MATRIX> m) {
    for (int i = 0; i < 16; ++i) put(m, i, X(i));
}

// The matrix times the translation: its last column becomes the matrix times (x, y, z, 1). Ninja
// uses ftrv on the current matrix and one fipr per row on a matrix in memory.
void njTranslate(gptr<NJS_MATRIX> m, Float x, Float y, Float z) {
    if (!m.a) {
        float mat[16];
        for (int i = 0; i < 16; ++i) mat[i] = X(i);
        const Vec4 r = ftrv(mat, x, y, z, 1.0f);
        for (int i = 0; i < 4; ++i) X(12 + i) = r.v[i];
        return;
    }
    const float v[4] = {x, y, z, 1.0f};
    for (int i = 0; i < 4; ++i) {
        const float row[4] = {get(m, i), get(m, 4 + i), get(m, 8 + i), get(m, 12 + i)};
        put(m, 12 + i, fipr(row, v));
    }
}

void njTranslateEx(gptr<NJS_VECTOR> v) {
    njTranslate(gptr<NJS_MATRIX>(0u), guest::load<float>(v.a), guest::load<float>(v.a + 4), guest::load<float>(v.a + 8));
}

// The rotations about z, y and x, or about y, x and z for a positive order. An angle of 0 is no
// rotation.
void njRotateEx(gptr<Angle> ang, Sint32 lv) {
    const Angle x = guest::load<Angle>(ang.a), y = guest::load<Angle>(ang.a + 4), z = guest::load<Angle>(ang.a + 8);
    const gptr<NJS_MATRIX> current(0u);
    if (lv > 0) {
        if (y) njRotateY(current, y);
        if (x) njRotateX(current, x);
        if (z) njRotateZ(current, z);
        return;
    }
    if (z) njRotateZ(current, z);
    if (y) njRotateY(current, y);
    if (x) njRotateX(current, x);
}

void njScaleEx(gptr<NJS_VECTOR> v) {
    njScale(gptr<NJS_MATRIX>(0u), guest::load<float>(v.a), guest::load<float>(v.a + 4), guest::load<float>(v.a + 8));
}

// The rotation part becomes the unit matrix; the fourth row and the translation stay.
void njUnitRotPortion(gptr<NJS_MATRIX> m) {
    for (int i : {0, 1, 2, 4, 5, 6, 8, 9, 10}) {
        const float v = i == 0 || i == 5 || i == 10 ? 1.0f : 0.0f;
        if (m.a)
            put(m, i, v);
        else
            X(i) = v;
    }
}

void njGetTranslation(gptr<NJS_MATRIX> m, gptr<NJS_POINT3> p) {
    for (uint32_t i = 0; i < 3; ++i) {
        const uint32_t b = m.a ? guest::load<uint32_t>(m.a + 48 + 4 * i) : bits_of(X(12 + static_cast<int>(i)));
        guest::store<uint32_t>(p.a + 4 * i, b);
    }
}

// The point on the screen, from Ninja's projection: the scale and the center at 0x8C10AF34. The
// result is 1 when the point lies within the depth range and the screen rectangle at 0x8C10AF64.
int njCalcScreen(gptr<NJS_POINT3> p, gptr<Float> sx, gptr<Float> sy) {
    constexpr uint32_t kProjection = 0x8C10AF34u, kBounds = 0x8C10AF64u;
    auto bound = [](uint32_t i) { return guest::load<float>(kBounds + 4 * i); };
    const float depth = -guest::load<float>(p.a + 8);
    const float w = 1.0f / depth;
    float x = guest::load<float>(p.a) * w, y = guest::load<float>(p.a + 4) * w;
    x = x * guest::load<float>(kProjection);
    y = y * guest::load<float>(kProjection + 4);
    x = x + guest::load<float>(kProjection + 8);
    y = y + guest::load<float>(kProjection + 12);
    *sx = x;
    *sy = y;
    if (depth > bound(0) || bound(1) > depth || bound(6) > x || x > bound(7) || bound(8) > y || y > bound(9))
        return 0;
    return 1;
}

// 1 when two spheres touch: the squared distance of their centers is not above the squared sum of
// their radii. The SH-4 adds two of the squares with fmac, which rounds once.
Sint32 njCollisionCheckSS(gptr<NJS_SPHERE> sphere1, gptr<NJS_SPHERE> sphere2) {
    auto f = [](gptr<NJS_SPHERE> s, uint32_t i) { return guest::load<float>(s.a + 4 * i); };
    const float dx = f(sphere2, 0) - f(sphere1, 0);
    const float dy = f(sphere2, 1) - f(sphere1, 1);
    const float dz = f(sphere2, 2) - f(sphere1, 2);
    const float r = f(sphere1, 3) + f(sphere2, 3);
    float d = dy * dy;
    const float rr = r * r;
    d = std::fmaf(dx, dx, d);
    d = std::fmaf(dz, dz, d);
    return d > rr ? 0 : 1;
}

void njCalcPointEx(gptr<NJS_POINT3> ps, gptr<NJS_POINT3> pd) {
    float mat[16];
    for (int i = 0; i < 16; ++i) mat[i] = X(i);
    const Vec4 r = ftrv(mat, guest::load<float>(ps.a), guest::load<float>(ps.a + 4), guest::load<float>(ps.a + 8), 1.0f);
    for (uint32_t i = 0; i < 3; ++i) guest::store<float>(pd.a + 4 * i, r.v[i]);
}

// The point ps through the matrix m, or for m 0 through the current matrix, to pd. Through m, each
// coordinate is the product of (x, y, z, 1) with a column, and the machine code stores x after it
// reads the second column and y after it reads the third.
void njCalcPoint(gptr<NJS_MATRIX> m, gptr<NJS_POINT3> ps, gptr<NJS_POINT3> pd) {
    diff::Check check("njCalcPoint", 0x8C014508u, m, ps, pd);
    const float v[4] = {guest::load<float>(ps.a), guest::load<float>(ps.a + 4), guest::load<float>(ps.a + 8), 1.0f};
    if (!m.a) {
        float mat[16];
        for (int i = 0; i < 16; ++i) mat[i] = X(i);
        const Vec4 r = ftrv(mat, v[0], v[1], v[2], v[3]);
        for (uint32_t i = 3; i-- > 0;) guest::store<float>(pd.a + 4 * i, r.v[i]);
        return;
    }
    const auto column = [m](int c, float (&out)[4]) {
        for (int i = 0; i < 4; ++i) out[i] = get(m, 4 * i + c);
    };
    float c0[4], c1[4], c2[4];
    column(0, c0);
    const float x = fipr(v, c0);
    column(1, c1);
    guest::store<float>(pd.a, x);
    const float y = fipr(v, c1);
    column(2, c2);
    guest::store<float>(pd.a + 4, y);
    guest::store<float>(pd.a + 8, fipr(v, c2));
}

// The vector vs through the matrix m without its translation, or for m 0 through the current
// matrix, to vd.
void njCalcVector(gptr<NJS_MATRIX> m, gptr<NJS_VECTOR> vs, gptr<NJS_VECTOR> vd) {
    diff::Check check("njCalcVector", 0x8C014564u, m, vs, vd);
    const float v[4] = {guest::load<float>(vs.a), guest::load<float>(vs.a + 4), guest::load<float>(vs.a + 8), 0.0f};
    float r[3];
    if (!m.a) {
        float mat[16];
        for (int i = 0; i < 16; ++i) mat[i] = X(i);
        const Vec4 t = ftrv(mat, v[0], v[1], v[2], v[3]);
        for (int i = 0; i < 3; ++i) r[i] = t.v[i];
    }
    else {
        for (int c = 0; c < 3; ++c) {
            const float column[4] = {get(m, c), get(m, 4 + c), get(m, 8 + c), 0.0f};
            r[c] = fipr(v, column);
        }
    }
    for (uint32_t i = 3; i-- > 0;) guest::store<float>(vd.a + 4 * i, r[i]);
}

namespace {

// The matrix `a` times each column of the current matrix, or of the matrix at `md`, which takes the
// products. The machine code reads all four columns before it stores the first.
void post_multiply(const float (&a)[16], gptr<NJS_MATRIX> md) {
    float r[16];
    for (int c = 0; c < 16; c += 4) {
        const auto in = [&](int i) { return md.a ? get(md, c + i) : X(c + i); };
        const Vec4 v = ftrv(a, in(0), in(1), in(2), in(3));
        for (int i = 0; i < 4; ++i) r[c + i] = v.v[i];
    }
    for (int i = 0; i < 16; ++i) {
        if (md.a)
            put(md, i, r[i]);
        else
            X(i) = r[i];
    }
}

}  // namespace

// The current matrix, or the matrix at md, becomes ms times it.
void njPostMultiMatrix(gptr<NJS_MATRIX> md, gptr<NJS_MATRIX> ms) {
    diff::Check check("njPostMultiMatrix", 0x8C0151BCu, md, ms);
    float a[16];
    for (int i = 0; i < 16; ++i) a[i] = get(ms, i);
    post_multiply(a, md);
}

// Pushes the current matrix and, for m other than 0, makes the matrix at m the current one.
Bool njPushMatrix(gptr<NJS_MATRIX> m) {
    diff::Check check("njPushMatrix", 0x8C0152ACu, m);
    if (!njPushMatrixEx())
        return check.returns(Bool(0));
    if (m.a)
        for (int i = 0; i < 16; ++i) X(i) = get(m, i);
    return check.returns(Bool(1));
}

// The view from `eye` to `center` with `up` as the up direction: the current matrix becomes the
// camera's matrix times it. The z axis points from the center to the eye, the y axis is `up` without
// its part along z, and the x axis is y times z, each normalized with fsrra.
void njCameraExLookAt(gptr<NJS_POINT3> eye, gptr<NJS_POINT3> center, gptr<NJS_VECTOR> up) {
    diff::Check check("njCameraExLookAt", 0x8C015EE2u, eye, center, up);
    const auto at = [](uint32_t base, uint32_t i) { return guest::load<float>(base + 4 * i); };
    const float ex = at(eye.a, 0), ey = at(eye.a, 1), ez = at(eye.a, 2);
    float zx = ex - at(center.a, 0), zy = ey - at(center.a, 1), zz = ez - at(center.a, 2);
    float inverse = 1.0f / std::sqrt(std::fmaf(zz, zz, std::fmaf(zx, zx, zy * zy)));
    zy *= inverse;
    zx *= inverse;
    zz *= inverse;
    const float ux = at(up.a, 0), uy = at(up.a, 1), uz = at(up.a, 2);
    const float along = std::fmaf(uz, zz, std::fmaf(ux, zx, zy * uy));
    float yx = ux - zx * along, yy = uy - zy * along, yz = uz - zz * along;
    inverse = 1.0f / std::sqrt(std::fmaf(yz, yz, std::fmaf(yx, yx, yy * yy)));
    yz *= inverse;
    yy *= inverse;
    yx *= inverse;
    const float xx = zz * yy - zy * yz, xy = zx * yz - zz * yx, xz = zy * yx - zx * yy;
    const float a[16] = {xx,
                         yx,
                         zx,
                         0.0f,
                         xy,
                         yy,
                         zy,
                         0.0f,
                         xz,
                         yz,
                         zz,
                         0.0f,
                         (-(ex * xx) - ey * xy) - ez * xz,
                         (-(ex * yx) - ey * yy) - ez * yz,
                         (-(ex * zx) - ey * zy) - ez * zz,
                         1.0f};
    post_multiply(a, gptr<NJS_MATRIX>(0));
}

// Turns the current matrix about z by `ang`: Ninja pops it, turns its x and y columns with fsca's
// sine and cosine and pushes the result as the current matrix.
void njRotateZCameraEx(Angle ang) {
    diff::Check check("njRotateZCameraEx", 0x8C015C74u, ang);
    const dream::sh4::detail::SinCos sc = fsca(ang);
    float b[16];
    for (int i = 0; i < 16; ++i) b[i] = X(i);
    njPopMatrixEx();
    guest::Frame frame(64);
    const gptr<NJS_MATRIX> a(frame.top - 64);
    for (int i = 0; i < 16; ++i) put(a, i, b[i]);
    // The first row rounds its cosine product before the sum, the others their sine product.
    for (int r = 0; r < 16; r += 4) {
        put(a, r, r == 0 ? std::fmaf(sc.s, b[1], b[0] * sc.c) : std::fmaf(sc.c, b[r], b[r + 1] * sc.s));
        put(a, r + 1, b[r + 1] * sc.c - b[r] * sc.s);
    }
    njPushMatrix(a);
}

// The camera's view: from its position (0) towards the point that its vector (12) points to, or to
// that vector itself for a camera of type 2 (28), with the up direction at 0x8C0CA660, then turned
// by its roll (24) and seen through its angle of view (26).
void njSetCamera(gptr<NJS_CAMERA> camera) {
    diff::Check check("njSetCamera", 0x8C016096u, camera);
    const uint32_t c = camera.a;
    guest::Frame frame(12);
    const uint32_t target = frame.top - 12;
    for (uint32_t i = 0; i < 12; i += 4) {
        if (guest::load<int32_t>(c + 28) == 2)
            guest::store<uint32_t>(target + i, guest::load<uint32_t>(c + 12 + i));
        else
            guest::store<float>(target + i, guest::load<float>(c + i) + guest::load<float>(c + 12 + i));
    }
    njCameraExLookAt(gptr<NJS_POINT3>(c), gptr<NJS_POINT3>(target), gptr<NJS_VECTOR>(0x8C0CA660u));
    njRotateZCameraEx(static_cast<Angle>(guest::load<int16_t>(c + 24)));
    njSetPerspective(static_cast<Angle>(guest::load<int16_t>(c + 26)));
}

}  // namespace game
