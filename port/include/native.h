// Declarations that the generated code shares with the port. Declared in namespace game next to the
// generated ones.
#pragma once

#include "diff.h"
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

// textPrintf, as SHC calls it: x and y in fr4 and fr5, the format and the values on the stack. Each
// call in the C passes x, y, r4 to r7, then the format and the stack words it takes.
void text_printf(float x, float y, uint32_t format, const uint32_t* values, size_t count);
template <class R4, class R5, class R6, class R7, class F, class... A>
void textPrintf(float x, float y, const R4& r4, const R5& r5, const R6& r6, const R7& r7, const F& format,
                const A&... values) {
    diff::Check check("textPrintf", 0x8C06B09Cu, x, y, r4, r5, r6, r7, format, values...);
    const uint32_t words[] = {0u, to_word(values)...};
    text_printf(x, y, to_word(format), words + 1, sizeof...(A));
}

}  // namespace game
