// What the generated code needs from the port besides guest memory: the dispatch table that code
// pointers are called through, and Ghidra's sub-piece syntax.
#pragma once

#include "diff.h"
#include "guest.h"

struct DispatchEntry {
    uint32_t address;
    Thunk thunk;
};
extern const DispatchEntry* const kDispatch;
extern const size_t kDispatchCount;
// The functions the SH-4 interpreter runs (port/interpreted.txt), sorted.
extern const uint32_t kInterpreted[];
extern const size_t kInterpretedCount;
// For each of them, the argument registers from r4 and from fr4 and the stack words it takes.
extern const uint8_t kInterpretedArgs[][3];
// The functions the port replaces with its own C++, sorted: they stay native when the interpreter
// runs everything (IKARUGA_INTERPRET_ALL).
extern const uint32_t kNativeOnly[];
extern const size_t kNativeOnlyCount;
// The functions whose arguments are not where the convention's order puts them, sorted: each
// argument's place, 0 to 3 for r4 to r7, 4 to 11 for fr4 to fr11, 16 + k for stack word k.
struct ArgLayout {
    uint32_t address;
    int count;
    const char* slots;
};
extern const ArgLayout kArgLayouts[];
extern const size_t kArgLayoutCount;
namespace sh4 {
CallResult run(uint32_t entry, const CallArgs& args);
}

// Ghidra's x._1_3_: the n bytes of x at offset o, as an lvalue. Pieces of FPU register pairs are
// floats (PIECE_F); other pieces read and write exactly their n bytes.
template <unsigned N>
struct piece_ref {
    uint8_t* p;
    operator uint64_t() const {
        uint64_t v = 0;
        std::memcpy(&v, p, N);
        return v;
    }
    // (float)x._0_4_ reads the word's bits, as for an undefined4: the exporter writes a conversion
    // as (float)(int).
    template <unsigned M = N, std::enable_if_t<M == 4, int> = 0>
    explicit operator float() const {
        float f;
        std::memcpy(&f, p, 4);
        return f;
    }
    // A float keeps its bits, as a pointer keeps its address.
    template <class V>
    const piece_ref& operator=(const V& v) const {
        const uint64_t bits = piece_bits(v);
        std::memcpy(p, &bits, N);
        return *this;
    }
    const piece_ref& operator=(const piece_ref& o) const { return *this = static_cast<uint64_t>(o); }
    template <class U>
    operator gptr<U>() const {
        return gptr<U>(static_cast<uint32_t>(static_cast<uint64_t>(*this)));
    }
    template <class U, class F>
    operator raw_word<U, F>() const {
        return raw_word<U, F>(static_cast<U>(static_cast<uint64_t>(*this)));
    }
};
template <class V>
inline uint8_t* piece_base(V& x) {
    return reinterpret_cast<uint8_t*>(&x);
}
template <class T>
inline uint8_t* piece_base(const gref<T>& x) {
    return &guest::host<uint8_t>(x.a);
}
// A local array lives on the guest stack. The host object holds only its address, and a piece
// read past that object is undefined: Clang dropped the store that took it.
template <class T>
inline uint8_t* piece_base(garray<T>& x) {
    return &guest::host<uint8_t>(x.a);
}
template <class T>
inline uint8_t* piece_base(const garray<T>& x) {
    return &guest::host<uint8_t>(x.a);
}
#define PIECE(x, o, n) (piece_ref<n>{piece_base(x) + (o)})
#define PIECE_F(x, o, n) (*reinterpret_cast<float*>(piece_base(x) + (o)))

// A record of the decompiled functions entered, with their call depth, in a ring buffer: the
// port prints the last ones when the game exits or crashes.
namespace trace {
struct Scope {
    explicit Scope(const char* name);
    ~Scope();
};
}  // namespace trace
// The address and the arguments are for the differential tests (diff.h, IKARUGA_DIFF).
#define TRACE_FUNCTION(name, ...)    \
    trace::Scope _trace_scope(name); \
    diff::Check _diff_check(name, __VA_ARGS__)
