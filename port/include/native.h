// Declarations that the generated code shares with the port. Declared in namespace game next to the
// generated ones.
#pragma once

#include "types.h"

namespace game {

// syBtExit: the game asks to return to the console's menu.
struct ExitRequested {};

// sprintf, as SHC calls it: the buffer in r4, the format and the values on the stack. Each call in
// the C passes r4 to r7, then the stack words its format takes (tools/ghidra/
// OverrideIndirectCalls.java), so the format is the fifth argument. The values are words; a float
// is its bits (the game's double is a float).
int format_words(gptr<char> buffer, uint32_t format, const uint32_t* words, size_t count);
template <class R5, class R6, class R7, class F, class... A>
int sprintf(gptr<char> buffer, const R5&, const R6&, const R7&, const F& format, const A&... values) {
    const uint32_t words[] = {0u, to_word(values)...};
    return format_words(buffer, to_word(format), words + 1, sizeof...(A));
}

}  // namespace game
