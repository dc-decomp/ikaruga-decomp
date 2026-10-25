// Differential tests of the decompiled functions against the game's machine code (IKARUGA_DIFF=N).
//
// On the first N calls of each decompiled function, the SH-4 interpreter first runs the function's
// machine code from the same memory and registers. The port keeps the memory that run leaves,
// restores the memory from before, and then runs the C++ body. If memory differs after the body,
// the C does not do what the machine code does, and the port names the function and the words. A
// function that returns through Check::returns has its result compared too.
//
// The interpreter's run is a shadow: it drops register writes and store-queue flushes, and time
// does not pass in it. A body that accesses registers can start a DMA that the shadow did not, so
// a difference there is only listed. A shadow that reaches state outside guest memory (the drive, the flash, an
// exit) or does not return within its budget is abandoned before it takes effect. In the C++ body
// time passes, but the hardware's events and the interrupts wait until it returns. A body that
// reads registers, sleeps or runs for longer than a frame gets them, and a call during which an
// event happened proves nothing and does not count.
#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <type_traits>

#include "guest.h"

namespace diff {

struct Abort {
    const char* why;
};

extern bool g_on;          // IKARUGA_DIFF is set
// Functions whose first call the port notes and prints (IKARUGA_WATCH_CALLS), at most 16. Screenshots
// that follow the game's events (IKARUGA_SHOTS_AFTER) count their frames from these calls.
extern uint32_t g_watch_addresses[16];
extern uint64_t g_watch_frames[16];  // the frame of the first call, or kNotYet
extern int g_watch_count;
constexpr uint64_t kNotYet = ~0ull;
void watch(uint32_t address);
uint64_t first_call(uint32_t address);
void note_call(uint32_t address);

// IKARUGA_CALLED=FILE: each decompiled function that the run entered, with the frame of its first
// call and its calls, written to FILE at the end. Functions on the interpreter are not in it.
extern bool g_called;
void note_called(uint32_t address, const char* name);
void write_called();

// IKARUGA_LOG_CALLS=ADDR[,ADDR...]: the functions whose calls are printed, at most 16.
extern uint32_t g_log_addresses[16];
extern int g_log_count;
inline bool logged(uint32_t address) {
    for (int i = 0; i < g_log_count; ++i)
        if (g_log_addresses[i] == address)
            return true;
    return false;
}
extern int g_shadow;       // nonzero while the interpreter runs a shadow
extern bool g_testing;     // while the C++ body of a tested call runs
extern bool g_hold;        // the body's events wait until it returns
extern uint64_t g_hold_from;  // the clock's cycles when the body started
extern uint64_t g_hw_accesses;  // the register accesses of that body

// A port function with state outside guest memory calls this first: a shadow must not reach it.
inline void guard(const char* what) {
    if (g_shadow)
        throw Abort{what};
}

// Counts a register access or a call in a shadow, and abandons a shadow that polls the hardware.
void count_access();

// A store-queue flush to the tile accelerator during a test: the machine code's in the shadow, the
// C++'s in the body. The test compares the two, as it compares memory.
void note_ta_flush(uint32_t destination, const uint8_t* words);

// At each register access. A body that reads registers gets the hardware's events: it can poll for
// a DMA that its writes start, which the shadow's dropped writes do not.
inline void on_access(bool read) {
    if (g_shadow)
        count_access();
    else if (g_testing) {
        ++g_hw_accesses;
        if (read)
            g_hold = false;
    }
}

// A native body that answers from the port's own state, which its machine code cannot see, as the
// native renderer's scenes: the call does not count.
void own_state();

// Prints a call of a function that IKARUGA_LOG_CALLS names: the frame and the argument registers.
void log_arguments(const char* name, const CallArgs& args);

// Records a C++ function entered during a test, for the report of the first call that differs.
void log_call(const char* name, const CallArgs* args);
// A call inside the body of a tested call. The test cannot run it, and a shadow calls the same
// C++, so the report lists the functions that the run calls only there.
void note_nested(uint32_t address, const char* name);
// Records an entry into an interpreted function during a test.
void log_interpreted(uint32_t address, const CallArgs& args);

bool begin(uint32_t address);
bool shadow(uint32_t address, const char* name, const CallArgs& args);
void finish(bool unwinding);
// The result of the C++ body of the call under test, which the test compares with the machine
// code's r0, or for a float with its fr0.
void note_result(uint32_t bits, bool fpu);
void report(std::FILE* out);

class Check {
public:
    template <class... A>
    Check(const char* name, uint32_t address, const A&... args) {
        if (g_watch_count)
            note_call(address);
        if (g_called)
            note_called(address, name);
        if (g_log_count && logged(address)) {
            CallArgs a;
            if (arguments(a, address, args...))
                log_arguments(name, a);
        }
        if (!g_on)
            return;
        if (g_shadow || g_testing) {
            if (g_shadow)
                count_access();  // a wait loop in the C++ that the shadow calls
            else
                note_nested(address, name);
            CallArgs a;
            const bool placed = arguments(a, address, args...);
            log_call(name, placed ? &a : nullptr);
            return;
        }
        if (!begin(address))
            return;
        CallArgs a;
        if (!arguments(a, address, args...))
            return;
        // The words above the stack pointer that no parameter names: what the C reads as in_stack_.
        for (int i = a.nstack; i < 16; ++i)
            a.stack[i] = guest::load<uint32_t>(guest::sp + 4u * static_cast<uint32_t>(i));
        a.nr = 4;
        a.nfr = 8;
        a.caller_frame = true;
        active_ = shadow(address, name, a);
    }
    ~Check() {
        if (active_)
            finish(std::uncaught_exceptions() > 0);
    }
    // Returns v: a function that its machine code's callers read a result from returns through
    // this, and the test compares the result too.
    template <class T>
    T returns(T v) const {
        if (active_) {
            if constexpr (std::is_floating_point_v<T>) {
                uint32_t bits;
                std::memcpy(&bits, &v, 4);
                note_result(bits, true);
            }
            else {
                note_result(to_word(v), false);
            }
        }
        return v;
    }
    Check(const Check&) = delete;
    Check& operator=(const Check&) = delete;

private:
    // The arguments where the function takes them; false if one has no word.
    template <class... A>
    static bool arguments(CallArgs& a, uint32_t address, const A&... args) {
        ArgWord list[2 * sizeof...(A) + 1];
        int n = 0;
        if (!(gfunc::add(list, n, args) && ...))
            return false;
        guest::place(address, list, n, a);
        return true;
    }

    bool active_ = false;
};

}  // namespace diff
