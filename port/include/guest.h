// The guest's view of memory for the decompiled game code.
//
// The game keeps 32-bit pointers in its data: object lists, callback tables, display list buffers.
// macOS on arm64 reserves the low 4 GB of every process, so these cannot be host pointers. A guest
// pointer is therefore a 32-bit guest address (gptr<T>), and a dereference is a reference into the
// guest's memory (gref<T>): main RAM is a host buffer, and everything else (video and sound
// memory, hardware registers) goes through the port's hardware layer.
//
// gptr<T> is exactly 4 bytes, so structures that hold pointers keep the game's layout and can be
// read from and written to guest memory as they are.
#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <type_traits>
#include <utility>

// ---- Ghidra's primitive types, at the sizes the SH-4 gives them ---------------------------------

template <class T>
struct gptr;
template <class T>
struct gref;
template <class T>
struct garray;
struct CallResult;
namespace guest {
uint32_t address_of(const void* host);
}

// Ghidra's undefined4 and undefined8: a word whose type the decompiler did not find. A value goes
// in with its bits, as into the register or the memory word that the variable stands for: a
// float stored in one keeps its bits. Ghidra prints no cast there, where C would convert. Read,
// the word is an unsigned integer.
template <class U, class F>
struct raw_word {
    U v;

    raw_word() = default;
    template <class T, std::enable_if_t<std::is_integral_v<T> || std::is_enum_v<T>, int> = 0>
    constexpr raw_word(T x) : v(static_cast<U>(x)) {}
    raw_word(F f) { std::memcpy(&v, &f, sizeof(U)); }
    template <class T>
    raw_word(const gptr<T>& p) : v(p.a) {}
    template <class T>
    raw_word(const garray<T>& g) : v(g.a) {}
    // A host pointer into guest memory, as an array or a string decays to one.
    template <class T>
    raw_word(T* host) : v(host ? guest::address_of(host) : 0) {}
    template <class T, std::enable_if_t<!std::is_same_v<T, raw_word>, int> = 0>
    raw_word(const gref<T>& r) : raw_word(static_cast<T>(r)) {}
    template <class R, std::enable_if_t<std::is_same_v<R, CallResult>, int> = 0>
    raw_word(const R& r) : v(static_cast<U>(r.r0 | static_cast<uint64_t>(r.r1) << 32)) {}
    template <class U2, class F2, std::enable_if_t<!std::is_same_v<U2, U>, int> = 0>
    raw_word(const raw_word<U2, F2>& o) : v(static_cast<U>(o.v)) {}

    // Read as an integer, the word is its bits. Read as a float, also: Ghidra prints no cast where
    // an undefined word meets a float, and a conversion would have been an INT2FLOAT with a cast.
    template <class T, std::enable_if_t<std::is_integral_v<T> || std::is_enum_v<T>, int> = 0>
    constexpr operator T() const { return static_cast<T>(v); }
    operator F() const {
        F f;
        std::memcpy(&f, &v, sizeof(F));
        return f;
    }
    explicit constexpr operator bool() const { return v != 0; }

    raw_word& operator+=(U x) { v += x; return *this; }
    raw_word& operator-=(U x) { v -= x; return *this; }
    raw_word& operator*=(U x) { v *= x; return *this; }
    raw_word& operator/=(U x) { v /= x; return *this; }
    raw_word& operator%=(U x) { v %= x; return *this; }
    raw_word& operator&=(U x) { v &= x; return *this; }
    raw_word& operator|=(U x) { v |= x; return *this; }
    raw_word& operator^=(U x) { v ^= x; return *this; }
    raw_word& operator<<=(U x) { v <<= x; return *this; }
    raw_word& operator>>=(U x) { v >>= x; return *this; }
    raw_word& operator++() { ++v; return *this; }
    raw_word& operator--() { --v; return *this; }
    raw_word operator++(int) { raw_word r = *this; ++v; return r; }
    raw_word operator--(int) { raw_word r = *this; --v; return r; }
};
struct not_raw_word {};
template <class T>
struct raw_word_int {
    using type = not_raw_word;
};
template <class U, class F>
struct raw_word_int<raw_word<U, F>> {
    using type = U;
};
template <class T>
constexpr bool is_raw_word_v = !std::is_same_v<typename raw_word_int<T>::type, not_raw_word>;

typedef uint8_t undefined;
typedef uint8_t undefined1;
typedef uint16_t undefined2;
typedef uint32_t undefined3;
typedef raw_word<uint32_t, float> undefined4;
typedef uint64_t undefined5;
typedef uint64_t undefined6;
typedef uint64_t undefined7;
typedef raw_word<uint64_t, double> undefined8;
typedef uint8_t byte;
typedef uint8_t uchar;
typedef uint16_t ushort;
typedef uint16_t word;
typedef uint32_t uint;
typedef uint32_t dword;
typedef uint32_t ulong;
typedef int32_t slong;
typedef uint64_t qword;
typedef int64_t longlong;
typedef uint64_t ulonglong;
typedef double float10;
typedef uint16_t wchar16;
typedef uint32_t uint3;
typedef int32_t int3;
typedef uint64_t uint5;
typedef uint64_t uint6;
typedef uint64_t uint7;

// A function, as the target of a code pointer. Never instantiated.
struct code;

namespace guest {

constexpr uint32_t kRamSize = 16u << 20;
extern uint8_t* ram;

// The SH-4's P0 to P3 segments all map the same 512 MB physical space; P4 is the on-chip area.
inline bool is_ram(uint32_t a) {
    return a < 0xE0000000u && (a & 0x1F000000u) == 0x0C000000u;
}
inline uint8_t* ram_at(uint32_t a) {
    return ram + (a & (kRamSize - 1));
}

// Memory other than main RAM: video memory and sound memory have host buffers too; registers do
// not, and their reads and writes have effects.
uint8_t* direct(uint32_t a);  // nullptr for registers
uint32_t io_read(uint32_t a, unsigned size);
void io_write(uint32_t a, uint32_t value, unsigned size);
void io_read_block(uint32_t a, void* out, unsigned size);
void io_write_block(uint32_t a, const void* in, unsigned size);

// Sound memory is on the G2 bus: an access takes time, which lets a loop that polls the sound
// driver's mailbox see the driver answer.
inline bool is_aram(uint32_t a) {
    return a < 0xE0000000u && (a & 0x1FE00000u) == 0x00800000u;
}
void g2_wait();

// The guest address of a host pointer into guest memory: what `&local` and `&global` turn into
// when the decompiled code passes them on. Aborts on a host pointer outside guest memory.
uint32_t address_of(const void* host);

template <class T>
inline T load(uint32_t a) {
    T v;
    if (is_ram(a)) {
        std::memcpy(&v, ram_at(a), sizeof(T));
    }
    else if (uint8_t* p = direct(a)) {
        std::memcpy(&v, p, sizeof(T));
        if (is_aram(a))
            g2_wait();
    }
    else if constexpr (sizeof(T) <= 4) {
        const uint32_t r = io_read(a, sizeof(T));
        std::memcpy(&v, &r, sizeof(T));
    }
    else {
        io_read_block(a, &v, sizeof(T));
    }
    return v;
}

template <class T>
inline void store(uint32_t a, const T& v) {
    if (is_ram(a)) {
        std::memcpy(ram_at(a), &v, sizeof(T));
    }
    else if (uint8_t* p = direct(a)) {
        std::memcpy(p, &v, sizeof(T));
        if (is_aram(a))
            g2_wait();
    }
    else if constexpr (sizeof(T) <= 4) {
        uint32_t r = 0;
        std::memcpy(&r, &v, sizeof(T));
        io_write(a, r, sizeof(T));
    }
    else {
        io_write_block(a, &v, sizeof(T));
    }
}

// A host reference to guest memory that has a host buffer. For structures and arrays, which the
// decompiled code reaches with `.` and `[]`.
template <class T>
inline T& host(uint32_t a) {
    uint8_t* p = is_ram(a) ? ram_at(a) : direct(a);
    return *reinterpret_cast<T*>(p);
}

}  // namespace guest

template <class T>
struct gptr;
template <class T>
struct gref;
template <class T>
struct garray;
struct gfunc;

template <class T>
struct is_gptr : std::false_type {};
template <class T>
struct is_gptr<gptr<T>> : std::true_type {};
template <class T>
struct is_gref : std::false_type {};
template <class T>
struct is_gref<gref<T>> : std::true_type {};

// The size a pointer step moves: 1 for void and for functions, like GCC.
template <class T>
constexpr uint32_t step_of() {
    if constexpr (std::is_void_v<T> || std::is_same_v<std::remove_cv_t<T>, code>)
        return 1;
    else
        return sizeof(T);
}

// Converts any value the decompiled code holds into the 32-bit word the guest would hold.
template <class V>
inline uint32_t to_word(const V& v);

// A call through a code pointer. Arguments go where the SH-4 calling convention puts them: integers
// and pointers in r4 to r7 then on the stack, floats in fr4 to fr11.
struct CallArgs {
    uint32_t r[4] = {};
    float fr[8] = {};
    uint32_t stack[16] = {};
    int nr = 0, nfr = 0, nstack = 0;
};
struct CallResult {
    uint32_t r0 = 0, r1 = 0;
    float fr0 = 0, fr1 = 0;

    template <class T, std::enable_if_t<std::is_integral_v<T> || std::is_enum_v<T>, int> = 0>
    operator T() const {
        return static_cast<T>(r0);
    }
    operator float() const { return fr0; }
    template <class T>
    operator gptr<T>() const {
        return gptr<T>(r0);
    }
    operator uint64_t() const { return (static_cast<uint64_t>(r1) << 32) | r0; }
};
using Thunk = CallResult (*)(const CallArgs&);

namespace guest {
CallResult call(uint32_t address, const CallArgs& args);
// Whether a function the port runs as C++ (or a boot ROM vector) starts at this address.
bool native(uint32_t address);
// The end of the game's executable in memory (it starts at 0x8C010000).
extern uint32_t image_end;
}  // namespace guest

template <class T>
struct gptr {
    uint32_t a = 0;

    gptr() = default;
    constexpr gptr(std::nullptr_t) : a(0) {}
    // Integers and pointers convert into each other without a cast in the decompiled code, as
    // they do in C on a 32-bit machine.
    template <class I, std::enable_if_t<std::is_integral_v<I> || std::is_enum_v<I>, int> = 0>
    constexpr gptr(I v) : a(static_cast<uint32_t>(v)) {}
    template <class U, class F>
    constexpr gptr(raw_word<U, F> w) : a(static_cast<uint32_t>(w.v)) {}
    // A float cast to a pointer is a field of the wrong type in Ghidra's structures: the word holds
    // a pointer, so its bits are the address.
    explicit gptr(float f) { std::memcpy(&a, &f, 4); }
    // C converts between pointer types with at most a warning, and the decompiled code assigns
    // one pointer type to another freely.
    template <class U>
    constexpr gptr(gptr<U> o) : a(o.a) {}
    template <class U>
    gptr(const gref<gptr<U>>& r);
    // `&local` and `&global` in the decompiled code are host pointers into guest memory.
    template <class U>
    gptr(U* host) : a(host ? guest::address_of(host) : 0) {}

    // One conversion to an integer, so that an expression mixing a pointer with integers has one
    // best match; any other integer type follows from it.
    constexpr operator uint32_t() const { return a; }
    explicit constexpr operator bool() const { return a != 0; }

    // A structure or an array is reached as a host reference, so `.field` and `[i]` work on it.
    // Scalars go through gref, because their address can be a register.
    // An array is reached as a garray, which also gives its address where C lets it decay.
    using ref = std::conditional_t<
        std::is_array_v<T>, garray<T>,
        std::conditional_t<(std::is_class_v<T> || std::is_union_v<T>) &&
                               !std::is_same_v<std::remove_cv_t<T>, code> && !is_gptr<T>::value &&
                               !is_raw_word_v<T>,
                           std::add_lvalue_reference_t<T>, gref<T>>>;
    ref operator*() const {
        if constexpr (std::is_reference_v<ref>)
            return guest::host<T>(a);
        else
            return ref{a};
    }
    ref operator[](int64_t i) const {
        const uint32_t at = a + static_cast<uint32_t>(i * static_cast<int64_t>(step_of<T>()));
        if constexpr (std::is_reference_v<ref>)
            return guest::host<T>(at);
        else
            return ref{at};
    }
    template <class U = T, std::enable_if_t<!std::is_void_v<U>, int> = 0>
    U* operator->() const {
        return &guest::host<U>(a);
    }

    template <class I, std::enable_if_t<std::is_integral_v<I> || std::is_enum_v<I>, int> = 0>
    gptr operator+(I i) const {
        return gptr(a + static_cast<uint32_t>(static_cast<int64_t>(i) * step_of<T>()));
    }
    template <class I, std::enable_if_t<std::is_integral_v<I> || std::is_enum_v<I>, int> = 0>
    gptr operator-(I i) const {
        return gptr(a - static_cast<uint32_t>(static_cast<int64_t>(i) * step_of<T>()));
    }
    int32_t operator-(gptr o) const {
        return static_cast<int32_t>(a - o.a) / static_cast<int32_t>(step_of<T>());
    }
    template <class I, std::enable_if_t<std::is_integral_v<I>, int> = 0>
    gptr operator+(const gref<I>& i) const {
        return *this + static_cast<I>(i);
    }
    template <class I, std::enable_if_t<std::is_integral_v<I>, int> = 0>
    gptr operator-(const gref<I>& i) const {
        return *this - static_cast<I>(i);
    }
    template <class I>
    gptr& operator+=(I i) {
        return *this = *this + i;
    }
    template <class I>
    gptr& operator-=(I i) {
        return *this = *this - i;
    }
    gptr& operator++() { return *this = *this + 1; }
    gptr& operator--() { return *this = *this - 1; }
    gptr operator++(int) {
        gptr o = *this;
        ++*this;
        return o;
    }
    gptr operator--(int) {
        gptr o = *this;
        --*this;
        return o;
    }
    bool operator!() const { return a == 0; }

    // A call through the pointer, which C allows without the `*`.
    template <class... A>
    CallResult operator()(const A&... args) const;
};

// Pointers of any two types compare by address, as in the decompiled C.
#define GPTR_COMPARE(op)                                                          \
    template <class T, class U>                                                   \
    bool operator op(gptr<T> x, gptr<U> y) {                                      \
        return x.a op y.a;                                                        \
    }                                                                             \
    template <class T, class U>                                                   \
    bool operator op(const gref<gptr<T>>& x, gptr<U> y) {                         \
        return guest::load<uint32_t>(x.a) op y.a;                                  \
    }                                                                             \
    template <class T, class U>                                                   \
    bool operator op(gptr<T> x, const gref<gptr<U>>& y) {                         \
        return x.a op guest::load<uint32_t>(y.a);                                  \
    }                                                                             \
    template <class T, class U>                                                   \
    bool operator op(const gref<gptr<T>>& x, const gref<gptr<U>>& y) {            \
        return guest::load<uint32_t>(x.a) op guest::load<uint32_t>(y.a);            \
    }
GPTR_COMPARE(==)
GPTR_COMPARE(!=)
GPTR_COMPARE(<)
GPTR_COMPARE(>)
GPTR_COMPARE(<=)
GPTR_COMPARE(>=)
#undef GPTR_COMPARE

template <class I, class T, std::enable_if_t<std::is_integral_v<I>, int> = 0>
gptr<T> operator+(I i, gptr<T> p) {
    return p + i;
}

static_assert(sizeof(gptr<int>) == 4, "a guest pointer must keep the guest's layout");

// An argument as the caller holds it, before it is placed: its word, whether it is a float, and
// whether it is the first of the two words of a 64-bit value.
struct ArgWord {
    uint32_t w;
    bool is_float;
    bool wide;
};

namespace guest {
// Puts the arguments where the function at `address` takes them: where its storage puts them for
// the functions in kArgLayouts, in the convention's order for the others.
void place(uint32_t address, const ArgWord* args, int n, CallArgs& out);
}  // namespace guest

struct gfunc {
    uint32_t a;

    // False for a value without a word, a structure larger than one.
    template <class V>
    static bool add(ArgWord* list, int& n, const V& v) {
        if constexpr (is_gref<V>::value) {
            // A float in memory is a float argument.
            if constexpr (std::is_floating_point_v<typename V::value_type>)
                return add(list, n, static_cast<typename V::value_type>(v));
            else
                return add(list, n, to_word(v));
        }
        else if constexpr (std::is_floating_point_v<V>) {
            const float f = static_cast<float>(v);
            uint32_t w;
            std::memcpy(&w, &f, 4);
            list[n++] = {w, true, false};
            return true;
        }
        else if constexpr (std::is_same_v<V, CallResult> || std::is_pointer_v<V> || std::is_array_v<V> ||
                           is_gptr<V>::value || sizeof(V) <= 4) {
            // An array in guest memory, such as a string's label, passes its address.
            list[n++] = {to_word(v), false, false};
            return true;
        }
        else if constexpr (sizeof(V) == 8 && std::is_trivially_copyable_v<V>) {
            uint32_t w[2];
            std::memcpy(w, &v, 8);
            list[n++] = {w[0], false, true};
            list[n++] = {w[1], false, false};
            return true;
        }
        else
            return false;
    }
    template <class... A>
    CallResult operator()(const A&... args) const {
        ArgWord list[2 * sizeof...(A) + 1];
        int n = 0;
        (add(list, n, args), ...);
        CallArgs c;
        guest::place(a, list, n, c);
        return guest::call(a, c);
    }
};

template <class T>
struct gref {
    uint32_t a;

    // An array in guest memory has no value of its own; it is reached through operator[].
    struct no_value {};
    using value_type = std::conditional_t<std::is_array_v<T>, no_value, T>;
    operator value_type() const {
        if constexpr (std::is_array_v<T>)
            return {};
        else
            return guest::load<T>(a);
    }
    // A raw word reads as its integer in one conversion, so arithmetic on it works.
    operator typename raw_word_int<T>::type() const {
        if constexpr (!is_raw_word_v<T>)
            return {};
        else
            return guest::load<T>(a);
    }
    template <class X,
              std::enable_if_t<!std::is_same_v<X, T> && !std::is_reference_v<X> &&
                                   (std::is_arithmetic_v<X> || std::is_enum_v<X> || is_gptr<X>::value),
                               int> = 0>
    explicit operator X() const {
        if constexpr (is_gptr<T>::value && std::is_arithmetic_v<X>)
            return static_cast<X>(static_cast<T>(*this).a);
        // A float slot cast to a pointer holds a pointer, as for gptr(float): a conversion would
        // show as (int) before the pointer cast.
        else if constexpr (is_gptr<X>::value && std::is_same_v<T, float>)
            return X(guest::load<uint32_t>(a));
        else if constexpr (is_gptr<X>::value && std::is_arithmetic_v<T>)
            return X(static_cast<uint32_t>(static_cast<T>(*this)));
        else
            return (X) static_cast<T>(*this);
    }
    template <class U = T, std::enable_if_t<!std::is_same_v<U, bool>, int> = 0>
    explicit operator bool() const {
        if constexpr (is_gptr<T>::value)
            return static_cast<T>(*this).a != 0;
        else if constexpr (std::is_class_v<T> && !is_raw_word_v<T>)
            return true;
        else
            return static_cast<T>(*this) != T{};
    }

    const gref& operator=(const T& v) const {
        guest::store<T>(a, v);
        return *this;
    }
    const gref& operator=(const gref& o) const { return *this = static_cast<T>(o); }
    template <class U, std::enable_if_t<!std::is_same_v<U, T> && std::is_convertible_v<U, T>, int> = 0>
    const gref& operator=(const U& v) const {
        return *this = static_cast<T>(v);
    }

    gptr<T> operator&() const { return gptr<T>(a); }

    // Pointer stored in guest memory.
    template <class U = T, std::enable_if_t<is_gptr<U>::value, int> = 0>
    auto operator*() const {
        return *static_cast<T>(*this);
    }
    template <class U = T, std::enable_if_t<is_gptr<U>::value, int> = 0>
    auto operator->() const {
        return static_cast<T>(*this).operator->();
    }
    template <class I, class U = T, std::enable_if_t<is_gptr<U>::value, int> = 0>
    auto operator[](I i) const {
        return static_cast<T>(*this)[i];
    }
    // Pointer arithmetic on a pointer stored in guest memory.
    template <class I, class U = T, std::enable_if_t<is_gptr<U>::value && std::is_integral_v<I>, int> = 0>
    U operator+(I i) const {
        return static_cast<U>(*this) + i;
    }
    template <class I, class U = T, std::enable_if_t<is_gptr<U>::value && std::is_integral_v<I>, int> = 0>
    U operator-(I i) const {
        return static_cast<U>(*this) - i;
    }
    // Array stored in guest memory.
    template <class I, class U = T, std::enable_if_t<std::is_array_v<U>, int> = 0>
    gref<std::remove_extent_t<U>> operator[](I i) const {
        return gref<std::remove_extent_t<U>>{
            a + static_cast<uint32_t>(i) * static_cast<uint32_t>(sizeof(std::remove_extent_t<U>))};
    }
    // A call. Through a code pointer stored here, the pointer's target is called, as C calls a
    // function pointer value. Anywhere else this is the function itself, as `(*p)(...)` in C.
    template <class... A>
    CallResult operator()(const A&... args) const {
        if constexpr (is_gptr<T>::value)
            return gfunc{static_cast<T>(*this).a}(args...);
        else
            return gfunc{a}(args...);
    }

#define GREF_COMPOUND(op)                                  \
    template <class V>                                     \
    const gref& operator op##=(const V& v) const {         \
        T x = static_cast<T>(*this);                       \
        x op## = v;                                        \
        return *this = x;                                  \
    }
    GREF_COMPOUND(+)
    GREF_COMPOUND(-)
    GREF_COMPOUND(*)
    GREF_COMPOUND(/)
    GREF_COMPOUND(%)
    GREF_COMPOUND(&)
    GREF_COMPOUND(|)
    GREF_COMPOUND(^)
    GREF_COMPOUND(<<)
    GREF_COMPOUND(>>)
#undef GREF_COMPOUND
    const gref& operator++() const { return *this += 1; }
    const gref& operator--() const { return *this -= 1; }
    T operator++(int) const {
        T o = *this;
        *this += 1;
        return o;
    }
    T operator--(int) const {
        T o = *this;
        *this -= 1;
        return o;
    }
};

// A code pointer is called through the port's table of functions.
template <>
struct gref<code> {
    uint32_t a;
    template <class... A>
    CallResult operator()(const A&... args) const {
        return gfunc{a}(args...);
    }
    gptr<code> operator&() const { return gptr<code>(a); }
    // A code pointer that the C reads through is a byte pointer in disguise: Ghidra typed a data
    // word that points at a table as a function pointer.
    operator uint8_t() const { return guest::load<uint8_t>(a); }
};

template <class T>
template <class U>
gptr<T>::gptr(const gref<gptr<U>>& r) : a(guest::load<uint32_t>(r.a)) {}

template <class T>
template <class... A>
CallResult gptr<T>::operator()(const A&... args) const {
    return gfunc{a}(args...);
}

template <class V>
inline uint32_t to_word(const V& v) {
    if constexpr (is_gptr<V>::value)
        return v.a;
    else if constexpr (std::is_array_v<V>)
        return guest::address_of(&v[0]);
    else if constexpr (std::is_pointer_v<V>)
        return guest::address_of(v);
    else if constexpr (std::is_same_v<V, float>) {
        uint32_t r;
        std::memcpy(&r, &v, 4);
        return r;
    }
    else if constexpr (std::is_arithmetic_v<V> || std::is_enum_v<V>)
        return static_cast<uint32_t>(v);
    else if constexpr (std::is_same_v<V, CallResult>)
        return v.r0;
    else if constexpr ((std::is_class_v<V> || std::is_union_v<V>) && std::is_trivially_copyable_v<V> && sizeof(V) <= 4) {
        uint32_t r = 0;  // a small structure or union passed in a register
        std::memcpy(&r, &v, sizeof(V));
        return r;
    }
    else
        return to_word(static_cast<decltype(+std::declval<V>())>(v));
}

template <class T>
inline uint32_t to_word(const gref<T>& r) {
    return to_word(static_cast<T>(r));
}

// The guest address of an lvalue: what the decompiled code's unary & becomes. A host reference is
// into guest memory (a guest-stack local or a global); a gref already carries its address.
template <class T>
inline gptr<T> gaddr(T& r) {
    return gptr<T>(guest::address_of(&r));
}
template <class T>
inline gptr<T> gaddr(const gref<T>& r) {
    return gptr<T>(r.a);
}
inline gptr<code> gaddr(const gref<code>& r) {
    return gptr<code>(r.a);
}

uint32_t cpu_unknown_word(const char* what);  // cpu::unknown for a value with no function name

// ---- Ghidra's operators on pieces of values -------------------------------------------------------

template <class V>
inline uint64_t piece_bits(const V& v) {
    if constexpr (std::is_same_v<V, float>) {
        uint32_t r;
        std::memcpy(&r, &v, 4);
        return r;
    }
    else if constexpr (std::is_same_v<V, double>) {
        // A double in the C++ is a float on the console (SHC's double is single precision): an
        // expression like `x * k - 1.0` is a double here only because of the literal.
        const float f = static_cast<float>(v);
        uint32_t r;
        std::memcpy(&r, &f, 4);
        return r;
    }
    else if constexpr (is_gptr<V>::value)
        return v.a;
    else if constexpr ((std::is_class_v<V> || std::is_union_v<V>) && std::is_trivially_copyable_v<V> && sizeof(V) <= 8) {
        uint64_t r = 0;
        std::memcpy(&r, &v, sizeof(V));
        return r;
    }
    else
        return static_cast<uint64_t>(v);
}
template <class T>
inline uint64_t piece_bits(const gref<T>& v) {
    return piece_bits(static_cast<T>(v));
}

// pun<T>(x): x's bits as a T, for the casts Ghidra prints where a value moves between a float
// and an integer register or word unchanged, and for the low piece of a register pair. A float
// stays a float: the sin and cos of fsca are host floats already.
template <class T, class V>
inline T pun(const V& v) {
    if constexpr (std::is_floating_point_v<T> && std::is_floating_point_v<V>)
        return static_cast<T>(v);
    else {
        const uint64_t bits = piece_bits(v);
        if constexpr (std::is_same_v<T, float>) {
            const uint32_t w = static_cast<uint32_t>(bits);
            float f;
            std::memcpy(&f, &w, 4);
            return f;
        }
        else if constexpr (std::is_same_v<T, double>) {
            double d;
            std::memcpy(&d, &bits, 8);
            return d;
        }
        else if constexpr (is_gptr<T>::value)
            return T(static_cast<uint32_t>(bits));
        else if constexpr (std::is_arithmetic_v<T> || std::is_enum_v<T>)
            return static_cast<T>(bits);
        else {
            T t{};
            std::memcpy(&t, &bits, sizeof(T) < 8 ? sizeof(T) : 8);
            return t;
        }
    }
}

// CONCATnm(a, b): a's low n bytes above b's low m bytes.
template <unsigned N, unsigned M, class A, class B>
inline uint64_t concat(const A& a, const B& b) {
    const uint64_t lo_mask = M >= 8 ? ~0ull : ((1ull << (8 * M)) - 1);
    const uint64_t hi_mask = N >= 8 ? ~0ull : ((1ull << (8 * N)) - 1);
    return ((piece_bits(a) & hi_mask) << (8 * M)) | (piece_bits(b) & lo_mask);
}
#define CONCAT11(a, b) ((uint16_t)concat<1, 1>(a, b))
#define CONCAT12(a, b) ((uint32_t)concat<1, 2>(a, b))
#define CONCAT13(a, b) ((uint32_t)concat<1, 3>(a, b))
#define CONCAT14(a, b) concat<1, 4>(a, b)
#define CONCAT15(a, b) concat<1, 5>(a, b)
#define CONCAT16(a, b) concat<1, 6>(a, b)
#define CONCAT17(a, b) concat<1, 7>(a, b)
#define CONCAT21(a, b) ((uint32_t)concat<2, 1>(a, b))
#define CONCAT22(a, b) ((uint32_t)concat<2, 2>(a, b))
#define CONCAT23(a, b) concat<2, 3>(a, b)
#define CONCAT24(a, b) concat<2, 4>(a, b)
#define CONCAT25(a, b) concat<2, 5>(a, b)
#define CONCAT26(a, b) concat<2, 6>(a, b)
#define CONCAT31(a, b) ((uint32_t)concat<3, 1>(a, b))
#define CONCAT32(a, b) concat<3, 2>(a, b)
#define CONCAT33(a, b) concat<3, 3>(a, b)
#define CONCAT34(a, b) concat<3, 4>(a, b)
#define CONCAT35(a, b) concat<3, 5>(a, b)
#define CONCAT41(a, b) concat<4, 1>(a, b)
#define CONCAT42(a, b) concat<4, 2>(a, b)
#define CONCAT43(a, b) concat<4, 3>(a, b)
#define CONCAT44(a, b) concat<4, 4>(a, b)
#define CONCAT51(a, b) concat<5, 1>(a, b)
#define CONCAT52(a, b) concat<5, 2>(a, b)
#define CONCAT53(a, b) concat<5, 3>(a, b)
#define CONCAT61(a, b) concat<6, 1>(a, b)
#define CONCAT62(a, b) concat<6, 2>(a, b)
#define CONCAT71(a, b) concat<7, 1>(a, b)
// SUBnm(x, o): the m bytes of x's n at byte offset o.
template <unsigned M, class V>
inline uint64_t subpiece(const V& v, unsigned o) {
    const uint64_t bits = piece_bits(v) >> (8 * o);
    return M >= 8 ? bits : bits & ((1ull << (8 * M)) - 1);
}
#define SUB11(x, o) ((uint8_t)subpiece<1>(x, o))
#define SUB12(x, o) ((uint16_t)subpiece<2>(x, o))
#define SUB13(x, o) ((uint32_t)subpiece<3>(x, o))
#define SUB14(x, o) ((uint32_t)subpiece<4>(x, o))
#define SUB15(x, o) ((uint64_t)subpiece<5>(x, o))
#define SUB16(x, o) ((uint64_t)subpiece<6>(x, o))
#define SUB17(x, o) ((uint64_t)subpiece<7>(x, o))
#define SUB18(x, o) ((uint64_t)subpiece<8>(x, o))
#define SUB21(x, o) ((uint8_t)subpiece<1>(x, o))
#define SUB22(x, o) ((uint16_t)subpiece<2>(x, o))
#define SUB23(x, o) ((uint32_t)subpiece<3>(x, o))
#define SUB24(x, o) ((uint32_t)subpiece<4>(x, o))
#define SUB25(x, o) ((uint64_t)subpiece<5>(x, o))
#define SUB26(x, o) ((uint64_t)subpiece<6>(x, o))
#define SUB27(x, o) ((uint64_t)subpiece<7>(x, o))
#define SUB28(x, o) ((uint64_t)subpiece<8>(x, o))
#define SUB31(x, o) ((uint8_t)subpiece<1>(x, o))
#define SUB32(x, o) ((uint16_t)subpiece<2>(x, o))
#define SUB33(x, o) ((uint32_t)subpiece<3>(x, o))
#define SUB34(x, o) ((uint32_t)subpiece<4>(x, o))
#define SUB35(x, o) ((uint64_t)subpiece<5>(x, o))
#define SUB36(x, o) ((uint64_t)subpiece<6>(x, o))
#define SUB37(x, o) ((uint64_t)subpiece<7>(x, o))
#define SUB38(x, o) ((uint64_t)subpiece<8>(x, o))
#define SUB41(x, o) ((uint8_t)subpiece<1>(x, o))
#define SUB42(x, o) ((uint16_t)subpiece<2>(x, o))
#define SUB43(x, o) ((uint32_t)subpiece<3>(x, o))
#define SUB44(x, o) ((uint32_t)subpiece<4>(x, o))
#define SUB45(x, o) ((uint64_t)subpiece<5>(x, o))
#define SUB46(x, o) ((uint64_t)subpiece<6>(x, o))
#define SUB47(x, o) ((uint64_t)subpiece<7>(x, o))
#define SUB48(x, o) ((uint64_t)subpiece<8>(x, o))
#define SUB51(x, o) ((uint8_t)subpiece<1>(x, o))
#define SUB52(x, o) ((uint16_t)subpiece<2>(x, o))
#define SUB53(x, o) ((uint32_t)subpiece<3>(x, o))
#define SUB54(x, o) ((uint32_t)subpiece<4>(x, o))
#define SUB55(x, o) ((uint64_t)subpiece<5>(x, o))
#define SUB56(x, o) ((uint64_t)subpiece<6>(x, o))
#define SUB57(x, o) ((uint64_t)subpiece<7>(x, o))
#define SUB58(x, o) ((uint64_t)subpiece<8>(x, o))
#define SUB61(x, o) ((uint8_t)subpiece<1>(x, o))
#define SUB62(x, o) ((uint16_t)subpiece<2>(x, o))
#define SUB63(x, o) ((uint32_t)subpiece<3>(x, o))
#define SUB64(x, o) ((uint32_t)subpiece<4>(x, o))
#define SUB65(x, o) ((uint64_t)subpiece<5>(x, o))
#define SUB66(x, o) ((uint64_t)subpiece<6>(x, o))
#define SUB67(x, o) ((uint64_t)subpiece<7>(x, o))
#define SUB68(x, o) ((uint64_t)subpiece<8>(x, o))
#define SUB71(x, o) ((uint8_t)subpiece<1>(x, o))
#define SUB72(x, o) ((uint16_t)subpiece<2>(x, o))
#define SUB73(x, o) ((uint32_t)subpiece<3>(x, o))
#define SUB74(x, o) ((uint32_t)subpiece<4>(x, o))
#define SUB75(x, o) ((uint64_t)subpiece<5>(x, o))
#define SUB76(x, o) ((uint64_t)subpiece<6>(x, o))
#define SUB77(x, o) ((uint64_t)subpiece<7>(x, o))
#define SUB78(x, o) ((uint64_t)subpiece<8>(x, o))
#define SUB81(x, o) ((uint8_t)subpiece<1>(x, o))
#define SUB82(x, o) ((uint16_t)subpiece<2>(x, o))
#define SUB83(x, o) ((uint32_t)subpiece<3>(x, o))
#define SUB84(x, o) ((uint32_t)subpiece<4>(x, o))
#define SUB85(x, o) ((uint64_t)subpiece<5>(x, o))
#define SUB86(x, o) ((uint64_t)subpiece<6>(x, o))
#define SUB87(x, o) ((uint64_t)subpiece<7>(x, o))
#define SUB88(x, o) ((uint64_t)subpiece<8>(x, o))
#define ZEXT14(x) ((uint32_t)(uint8_t)(x))
#define ZEXT24(x) ((uint32_t)(uint16_t)(x))
#define ZEXT48(x) ((uint64_t)(uint32_t)(x))
#define SEXT14(x) ((int32_t)(int8_t)(x))
#define SEXT24(x) ((int32_t)(int16_t)(x))
#define SEXT48(x) ((int64_t)(int32_t)(x))
#define CARRY4(a, b) ((uint32_t)(a) + (uint32_t)(b) < (uint32_t)(a))
#define SCARRY4(a, b) ((((int32_t)(a) + (int32_t)(b)) < (int32_t)(a)) != ((int32_t)(b) < 0))
#define SBORROW4(a, b) ((((int32_t)(a) - (int32_t)(b)) > (int32_t)(a)) != ((int32_t)(b) < 0))
#define NAN_CHECK(x) std::isnan(x)
#define SQRT(x) std::sqrt(x)
#define ABS(x) std::fabs(x)

// A float's bits as an integer and back, for Ghidra's `*(float *)&local` idiom on host locals.
inline float bits_float(uint32_t v) {
    float f;
    std::memcpy(&f, &v, 4);
    return f;
}
inline uint32_t float_bits(float f) {
    uint32_t v;
    std::memcpy(&v, &f, 4);
    return v;
}

// Zero in a comparison, as the machine code makes it (ExportDecompiled prints which). fcmp/eq follows
// FPSCR.DN, which flushes a denormal to 0, so a float with a zero exponent equals dn_zero. Clang
// would turn x == 0.0f into a test of x's bits. An integer test of a word that Ghidra typed as a
// float compares all 32 bits with int_zero.
struct DnZero {};
inline constexpr DnZero dn_zero{};
inline bool operator==(float x, DnZero) {
    uint32_t b;
    std::memcpy(&b, &x, 4);
    return (b & 0x7F800000u) == 0;
}
inline bool operator!=(float x, DnZero z) { return !(x == z); }
inline bool operator==(DnZero z, float x) { return x == z; }
inline bool operator!=(DnZero z, float x) { return !(x == z); }
struct IntZero {};
inline constexpr IntZero int_zero{};
inline bool operator==(float x, IntZero) {
    uint32_t b;
    std::memcpy(&b, &x, 4);
    return b == 0;
}
inline bool operator!=(float x, IntZero z) { return !(x == z); }
inline bool operator==(IntZero z, float x) { return x == z; }
inline bool operator!=(IntZero z, float x) { return !(x == z); }

// The SH-4's fmac, which rounds once. An operand that is not a float, such as a word in memory that
// Ghidra typed as an int, holds a float's bits.
template <class A, class B, class C>
inline float fmac(const A& a, const B& b, const C& c) {
    auto value = [](const auto& v) -> float {
        using V = std::decay_t<decltype(v)>;
        if constexpr (std::is_floating_point_v<V>)
            return static_cast<float>(v);
        else if constexpr (is_gref<V>::value) {
            if constexpr (std::is_floating_point_v<typename V::value_type>)
                return static_cast<typename V::value_type>(v);
            else
                return bits_float(to_word(v));
        }
        else
            return bits_float(to_word(v));
    };
    return std::fmaf(value(a), value(b), value(c));
}

// An array inside a structure. Where C lets the array decay, this gives its guest address.
template <class T, size_t N>
struct inarray {
    T v[N];
    T& operator[](int64_t i) { return v[i]; }
    const T& operator[](int64_t i) const { return v[i]; }
    operator uint32_t() const { return guest::address_of(this); }
    template <class U>
    operator gptr<U>() const {
        return gptr<U>(guest::address_of(this));
    }
};

// A call to a function that Ghidra decided returns nothing, at a place that uses its result. The
// result was in r0 on the console; here it is unknown, and the port counts it.
template <class Fn>
inline auto void_value(Fn f, const char* name) {
    return [f, name](const auto&... args) {
        f(args...);
        return CallResult{cpu_unknown_word(name), 0, 0, 0};
    };
}

// ---- The guest stack ------------------------------------------------------------------------------

// An array on the guest stack. It stands for the array where the decompiled code uses the name:
// [] reaches an element, and the name alone is the array's guest address, as an integer or as
// a pointer of any type.
template <class T>
struct garray {
    uint32_t a;
    using elem = std::remove_extent_t<T>;
    elem& operator[](int64_t i) const {
        return guest::host<elem>(a + static_cast<uint32_t>(i * static_cast<int64_t>(sizeof(elem))));
    }
    template <class U>
    operator gptr<U>() const {
        return gptr<U>(a);
    }
    operator uint32_t() const { return a; }
    elem& operator*() const { return (*this)[0]; }
    template <class I, std::enable_if_t<std::is_integral_v<I>, int> = 0>
    gptr<elem> operator+(I i) const {
        return gptr<elem>(a) + i;
    }
    template <class I, std::enable_if_t<std::is_integral_v<I>, int> = 0>
    gptr<elem> operator-(I i) const {
        return gptr<elem>(a) - i;
    }
    template <class I>
    gptr<elem> operator+(const gref<I>& i) const {
        return gptr<elem>(a) + static_cast<I>(i);
    }
};
template <class T>
inline gptr<T> gaddr(const garray<T>& r) {
    return gptr<T>(r.a);
}
template <class T>
inline uint32_t to_word(const garray<T>& r) {
    return r.a;
}

namespace guest {
// The guest stack pointer. A decompiled function whose locals need guest addresses (a local whose
// address is taken, or an array or structure passed on) gets a frame here, at the offsets Ghidra
// found on the console's stack.
extern uint32_t sp;

struct Frame {
    // Hand-written SDK code pushes registers below its frame before a call (Ninja's clippers save
    // 0x40 bytes there), so a callee's frame starts this far below the caller's.
    static constexpr uint32_t kPushRoom = 0x100;
    uint32_t top;
    explicit Frame(uint32_t size) : top(sp) { sp -= ((size + 7u) & ~7u) + kPushRoom; }
    ~Frame() { sp = top; }
    Frame(const Frame&) = delete;
    Frame& operator=(const Frame&) = delete;
    template <class T>
    T& at(int32_t offset) const {
        return host<T>(top + static_cast<uint32_t>(offset));
    }
    template <class T>
    garray<T> array(int32_t offset) const {
        return garray<T>{top + static_cast<uint32_t>(offset)};
    }
};
}  // namespace guest

// ---- Registers the decompiled code reads or writes outside any variable -------------------------

namespace cpu {
extern uint32_t gbr, vbr, sr, fpul, fpscr, macl, mach;
extern uint32_t sgr, ssr, spc, dbr;
// The FPU back bank, in Ghidra's pair-swapped order: xf1 xf0 xf3 xf2 ...
extern float xf[16];
// r8 to r14 of machine code that calls a C++ function, for the ones that take an object in a
// callee-saved register (tools/ghidra/ContextParams.java). Set by the interpreter at each call.
extern uint32_t ctx[16];
// A register the decompiled code reads without any value: Ghidra's in_, unaff_ and extraout_.
// The port counts them, so a run can tell which functions depend on one.
uint32_t unknown(const char* function, const char* name);
// ldc to SR, GBR and VBR and lds to FPSCR, which the decompiled code keeps as calls; and the
// fields of SR and FPSCR by name. FPSCR's rounding mode and denormal flushing go to the host FPU.
void set_sr(uint32_t value);
void set_gbr(uint32_t value);
void set_vbr(uint32_t value);
void set_fpscr(uint32_t value);
uint32_t sr_field(const char* name);
uint32_t fpscr_field(const char* name);
}  // namespace cpu

#define xf0 cpu::xf[1]
#define xf1 cpu::xf[0]
#define xf2 cpu::xf[3]
#define xf3 cpu::xf[2]
#define xf4 cpu::xf[5]
#define xf5 cpu::xf[4]
#define xf6 cpu::xf[7]
#define xf7 cpu::xf[6]
#define xf8 cpu::xf[9]
#define xf9 cpu::xf[8]
#define xf10 cpu::xf[11]
#define xf11 cpu::xf[10]
#define xf12 cpu::xf[13]
#define xf13 cpu::xf[12]
#define xf14 cpu::xf[15]
#define xf15 cpu::xf[14]
#define xd0 (*reinterpret_cast<uint64_t*>(&cpu::xf[0]))
#define xd2 (*reinterpret_cast<uint64_t*>(&cpu::xf[2]))
#define xd4 (*reinterpret_cast<uint64_t*>(&cpu::xf[4]))
#define xd6 (*reinterpret_cast<uint64_t*>(&cpu::xf[6]))
#define xd8 (*reinterpret_cast<uint64_t*>(&cpu::xf[8]))
#define xd10 (*reinterpret_cast<uint64_t*>(&cpu::xf[10]))
#define xd12 (*reinterpret_cast<uint64_t*>(&cpu::xf[12]))
#define xd14 (*reinterpret_cast<uint64_t*>(&cpu::xf[14]))

// ---- Instructions without a C equivalent ----------------------------------------------------------

namespace hw {
void prefetch(uint32_t address);  // a store-queue flush when address is in 0xE0000000-0xE3FFFFFF
[[noreturn]] void bad_data(const char* function);
[[noreturn]] void unimplemented(const char* function);
void trap(uint32_t number);  // trapa
void sleep();  // waits for the next interrupt
}  // namespace hw

// Cache operations do nothing here: the port has no cache between the CPU and memory.
template <class V>
inline void CacheBlockInvalidate(const V&) {}
inline void LoadTranslationLookasideBuffer() {}

// The calls the decompiled code makes for pref, for ldc to SR, GBR and VBR and for lds to FPSCR,
// with whatever value the expression has.
namespace hw {
template <class V>
inline void prefetch_any(const V& v) {
    prefetch(to_word(v));
}
}  // namespace hw
namespace cpu {
template <class V>
inline void set_sr_any(const V& v) {
    set_sr(to_word(v));
}
template <class V>
inline void set_gbr_any(const V& v) {
    set_gbr(to_word(v));
}
template <class V>
inline void set_vbr_any(const V& v) {
    set_vbr(to_word(v));
}
template <class V>
inline void set_fpscr_any(const V& v) {
    set_fpscr(to_word(v));
}
}  // namespace cpu
template <class V>
inline void TrapAlways(const V& n) {
    hw::trap(to_word(n));
}
template <class V>
inline void CacheBlockPurge(const V&) {}
template <class V>
inline void CacheBlockWriteBack(const V&) {}

// mac.w and mac.l: multiply and accumulate into MACH:MACL. Ghidra passes the two operands' addresses.
template <class A, class B>
inline void mac_wOp(const A& x, const B& y) {
    int64_t acc = (static_cast<int64_t>(cpu::mach) << 32) | cpu::macl;
    acc += static_cast<int64_t>(guest::load<int16_t>(to_word(x))) * guest::load<int16_t>(to_word(y));
    cpu::mach = static_cast<uint32_t>(acc >> 32);
    cpu::macl = static_cast<uint32_t>(acc);
}
template <class A, class B>
inline void mac_lOp(const A& x, const B& y) {
    int64_t acc = (static_cast<int64_t>(cpu::mach) << 32) | cpu::macl;
    acc += static_cast<int64_t>(guest::load<int32_t>(to_word(x))) * guest::load<int32_t>(to_word(y));
    cpu::mach = static_cast<uint32_t>(acc >> 32);
    cpu::macl = static_cast<uint32_t>(acc);
}

// ---- Arithmetic on raw words ----------------------------------------------------------------------
// With an integer a raw word is an integer, with a float a float. These match exactly, so they win
// over the built-in operators, which could reach either through the word's two conversions.

template <class T>
inline auto raw_operand(const T& b) {
    if constexpr (is_gref<T>::value)
        return static_cast<typename T::value_type>(b);
    else if constexpr (std::is_same_v<T, CallResult>)
        return b.r0;
    else if constexpr (std::is_class_v<T> && !is_gptr<T>::value)
        return static_cast<uint64_t>(b);
    else
        return b;
}

#define RAW_WORD_OPERATOR(op, floats)                                                                  \
    template <class U, class F, class T, std::enable_if_t<!is_raw_word_v<T>, int> = 0>                 \
    inline auto operator op(const raw_word<U, F>& a, const T& b) {                                     \
        if constexpr (std::is_floating_point_v<T> && floats)                                           \
            return static_cast<F>(a) op b;                                                             \
        else if constexpr (std::is_integral_v<T> || std::is_enum_v<T> || is_gptr<T>::value)           \
            return a.v op b;                                                                           \
        else                                                                                           \
            return a op raw_operand(b);                                                                \
    }                                                                                                  \
    template <class U, class F, class T, std::enable_if_t<!is_raw_word_v<T>, int> = 0>                 \
    inline auto operator op(const T& a, const raw_word<U, F>& b) {                                     \
        if constexpr (std::is_floating_point_v<T> && floats)                                           \
            return a op static_cast<F>(b);                                                             \
        else if constexpr (std::is_integral_v<T> || std::is_enum_v<T> || is_gptr<T>::value)           \
            return a op b.v;                                                                           \
        else                                                                                           \
            return raw_operand(a) op b;                                                                \
    }                                                                                                  \
    template <class U, class F, class U2, class F2>                                                    \
    inline auto operator op(const raw_word<U, F>& a, const raw_word<U2, F2>& b) {                      \
        return a.v op b.v;                                                                             \
    }
RAW_WORD_OPERATOR(+, true)
RAW_WORD_OPERATOR(-, true)
RAW_WORD_OPERATOR(*, true)
RAW_WORD_OPERATOR(/, true)
RAW_WORD_OPERATOR(==, true)
RAW_WORD_OPERATOR(!=, true)
RAW_WORD_OPERATOR(<, true)
RAW_WORD_OPERATOR(>, true)
RAW_WORD_OPERATOR(<=, true)
RAW_WORD_OPERATOR(>=, true)
RAW_WORD_OPERATOR(%, false)
RAW_WORD_OPERATOR(&, false)
RAW_WORD_OPERATOR(|, false)
RAW_WORD_OPERATOR(^, false)
RAW_WORD_OPERATOR(<<, false)
RAW_WORD_OPERATOR(>>, false)
#undef RAW_WORD_OPERATOR

template <class U, class F>
inline auto operator-(const raw_word<U, F>& a) {
    return static_cast<U>(-a.v);
}
template <class U, class F>
inline auto operator~(const raw_word<U, F>& a) {
    return static_cast<U>(~a.v);
}
template <class U, class F>
inline bool operator!(const raw_word<U, F>& a) {
    return a.v == 0;
}

// A compound assignment with a raw word on the right: a float takes its bits, an integer its value.
#define RAW_WORD_ASSIGN(op)                                                                            \
    template <class L, class U, class F, std::enable_if_t<std::is_arithmetic_v<L>, int> = 0>           \
    inline L& operator op(L& a, const raw_word<U, F>& b) {                                             \
        if constexpr (std::is_floating_point_v<L>)                                                     \
            return a op static_cast<F>(b);                                                             \
        else                                                                                           \
            return a op b.v;                                                                           \
    }
RAW_WORD_ASSIGN(+=)
RAW_WORD_ASSIGN(-=)
RAW_WORD_ASSIGN(*=)
RAW_WORD_ASSIGN(/=)
#undef RAW_WORD_ASSIGN
#define RAW_WORD_ASSIGN_INT(op)                                                                        \
    template <class L, class U, class F, std::enable_if_t<std::is_integral_v<L>, int> = 0>             \
    inline L& operator op(L& a, const raw_word<U, F>& b) {                                             \
        return a op b.v;                                                                               \
    }
RAW_WORD_ASSIGN_INT(%=)
RAW_WORD_ASSIGN_INT(&=)
RAW_WORD_ASSIGN_INT(|=)
RAW_WORD_ASSIGN_INT(^=)
RAW_WORD_ASSIGN_INT(<<=)
RAW_WORD_ASSIGN_INT(>>=)
#undef RAW_WORD_ASSIGN_INT
