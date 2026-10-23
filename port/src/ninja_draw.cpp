// Ninja's drawing, natively, as far as it goes: each function sends the tile accelerator the same
// blocks through the store queues as its machine code, so the display lists stay the same.
// IKARUGA_SH4_TEST captures both and compares them.
#include "decomp.h"

namespace game {
namespace {

// The block of one vertex: the control word, then the vertex's x, y, z, u, v and colour, then the
// offset colour. Ninja writes fr7 there without loading it, so a vertex gets what its caller left
// in fr7; calls from C++ leave 0.
void vertex(uint32_t at, uint32_t control, uint32_t v) {
    guest::store<uint32_t>(at, control);
    for (uint32_t i = 0; i < 6; ++i) guest::store<uint32_t>(at + 4 + 4 * i, guest::load<uint32_t>(v + 4 * i));
    guest::store<uint32_t>(at + 28, 0);
    hw::prefetch(at);
}

}  // namespace

// A strip of textured vertices for njDrawTexture2DExStart. The variable at 0x8C10B360 holds the
// store-queue address of the next block; the last vertex's control word ends the strip.
void njDrawTexture2DExSetData(gptr<NJS_TEXTURE_VTX> p, Int count) {
    constexpr uint32_t kTable = 0x8C030604u;  // the control words and the variable's address
    const uint32_t control = guest::load<uint32_t>(kTable), last = guest::load<uint32_t>(kTable + 4);
    const uint32_t next = guest::load<uint32_t>(kTable + 8);
    uint32_t at = guest::load<uint32_t>(next);
    for (Int i = 0; i < count; ++i, at += 32)
        vertex(at, i == count - 1 ? last : control, p.a + 24u * static_cast<uint32_t>(i));
    guest::store<uint32_t>(next, at);
}

}  // namespace game
