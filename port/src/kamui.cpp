// Kamui natively, as far as it goes. Its decompiled code still keeps its DMA queues and render
// packets; the transfers that reached the hardware and the wait for the vertical blank are here.
#include <cstdio>
#include <cstdlib>

#include "decomp.h"
#include "diff.h"
#include "hw.h"

namespace game {

// Channel-2 DMA, complete when it starts. Kamui's queue starts each transfer here: a display list
// to the tile accelerator or a texture into texture memory. Its end then goes through Kamui's
// interrupt entry, as the end-of-DMA interrupt would. A display list's request also waits for the
// TA's end of that list, which is in the TA with the transfer, so its bit is set first.
void kmiLaunchQ(int param_1) {
    diff::guard("starts a DMA");
    const uint32_t request = static_cast<uint32_t>(param_1);
    const uint32_t type = guest::load<uint32_t>(request + 8), flags = guest::load<uint32_t>(request + 4);
    if (type != 0 || !(flags & 8)) {
        std::fprintf(stderr, "kmiLaunchQ: a request of type %u with flags 0x%x, not channel-2 DMA\n", type, flags);
        std::abort();
    }
    hw::pvr_transfer(guest::load<uint32_t>(request + 0x14), guest::load<uint32_t>(request + 0x10),
                     guest::load<uint32_t>(request + 0xc));
    guest::store<uint32_t>(request + 0x24, hw::read(0xFFD8000Cu, 4));  // TCNT0, for Kamui's timing
    guest::store<uint32_t>(request + 0x28, 3);
    guest::store<uint16_t>(request + 0x1e, guest::load<uint16_t>(request + 0x1c) & 0x7FFF);
    FUN_8c03b3e0(0x12);
}

// The wait for the next vertical blank, whose handler counts the frames at 0x8C10BFC8. Kamui's code
// spins on two callbacks, which do nothing in this game, about 50,000 times a frame; here the
// clock goes to the next device event between the checks, as the SH-4's sleep does.
KMSTATUS kmWaitVBlank_8c037080() {
    if (kmyGetInterruptLevel() != 0)
        return KMSTATUS_SUCCESS;
    const int frame = guest::load<int>(0x8C10BFC8u);
    for (uint32_t n = 0; n < 0x100000; ++n) {
        kmyRequestCallback(7);
        kmyRequestCallback(0xC);
        if (guest::load<int>(0x8C10BFC8u) != frame)
            return KMSTATUS_SUCCESS;
        hw::sleep();
    }
    return 0xEEEE;
}

}  // namespace game
