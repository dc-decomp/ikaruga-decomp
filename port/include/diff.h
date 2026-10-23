// Differential tests of the decompiled functions against the game's machine code (IKARUGA_DIFF=N).
//
// On the first N calls of each decompiled function, the SH-4 interpreter first runs the function's
// machine code from the same memory and registers. The port keeps the memory that run leaves,
// restores the memory from before, and then runs the C++ body. If memory differs after the body,
// the C does not do what the machine code does, and the port names the function and the words.
//
// The interpreter's run is a shadow: it drops register writes and store-queue flushes, and time
// does not pass in it. A body that accesses registers can start a DMA that the shadow did not, so
// a difference there is only listed. A shadow that reaches state outside guest memory (the drive, the flash, an
// exit) or does not return within its budget is abandoned before it takes effect. A call during
// which an interrupt or another hardware event happened proves nothing and does not count.
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
extern uint64_t g_hw_accesses;  // the register accesses of that body

// A port function with state outside guest memory calls this first: a shadow must not reach it.
inline void guard(const char* what) {
    if (g_shadow)
        throw Abort{what};
}

// Counts a register access or a call in a shadow, and abandons a shadow that polls the hardware.
void count_access();

// At each register access.
inline void on_access() {
    if (g_shadow)
        count_access();
    else if (g_testing)
        ++g_hw_accesses;
}

// Prints a call of a function that IKARUGA_LOG_CALLS names: the frame and the argument registers.
void log_arguments(const char* name, const CallArgs& args);

// Records a C++ function entered during a test, for the report of the first call that differs.
void log_call(const char* name, const CallArgs* args);
// Records an entry into an interpreted function during a test.
void log_interpreted(uint32_t address, const CallArgs& args);

bool begin(uint32_t address);
bool shadow(uint32_t address, const char* name, const CallArgs& args);
void finish(bool unwinding);
void report(std::FILE* out);

class Check {
public:
    template <class... A>
    Check(const char* name, uint32_t address, const A&... args) {
        if (g_watch_count)
            note_call(address);
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
        a.nstack = 16;
        active_ = shadow(address, name, a);
    }
    ~Check() {
        if (active_)
            finish(std::uncaught_exceptions() > 0);
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
