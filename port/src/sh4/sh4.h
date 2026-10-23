// An SH-4 interpreter for the functions the decompiled C cannot express: hand-written SDK code
// that passes values in registers from one routine to the next, returns results in several
// registers, or pushes registers below its frame. The port runs these from the game's own
// machine code (port/interpreted.txt); everything else stays decompiled.
//
// The interpreter shares memory, the xf bank, GBR, MACH, MACL and the interrupt mask with the
// decompiled code. A call to a function outside the set goes to its C++ version with the arguments
// in r4 to r7, fr4 to fr11 and the words on the stack, as the SH-4 convention passes them.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>

#include "guest.h"

namespace sh4 {

// Runs the function at `entry` until it returns, with the arguments where the convention puts
// them: CallArgs' words in r4 to r7, its floats in fr4 to fr11 and its stack words at r15.
CallResult run(uint32_t entry, const CallArgs& args);

// Runs the function at `entry` from the given registers, for tests.
CallResult run_regs(uint32_t entry, const uint32_t (&r)[16], const float (&fr)[16]);

// Compares the interpreter with native and decompiled versions of a few functions; prints the
// differences and returns their number (IKARUGA_SH4_TEST).
int self_test();

// Prints where the innermost running interpreter is, if one runs.
void report(std::FILE* out);

// IKARUGA_SH4_PROFILE: prints the instructions run per function that C++ called.
// IKARUGA_SH4_PCS=FILE: writes the instructions run at each address, for each routine's own time.
void report_profile(std::FILE* out);

// The stack pointer (r15) of the innermost running interpreter, or ~0u if none runs.
uint32_t active_stack();

// Whether the port interprets the function at this address.
bool interpreted(uint32_t address);

// IKARUGA_INTERPRET_ALL: the whole game runs as machine code on the interpreter, but for the
// functions the port replaces (the boot ROM's wrappers, the disc, the flash). A reference run on the
// port's own hardware: where it and the C++ differ, the C++ is wrong.
bool interpret_all();

}  // namespace sh4
