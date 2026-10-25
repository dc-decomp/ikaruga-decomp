// Kamui natively, as far as it goes: its register access, memory copies, callbacks, DMA handles
// and queues, the scenes, which render from the host's display list (frame.h), the renderer's
// settings, the region arrays and passes, the background plane, the display's frame buffers, the
// palette and the fog, the system configuration, the device with its driver modules and
// interrupts and their unload, the VGA display mode, the transfers that reached the hardware and
// the wait for the vertical blank. Its texture memory is in kamui_texture.cpp. Its decompiled code still keeps the
// television display modes and the functions that the game does not call. Each function does what
// its machine code does, and IKARUGA_DIFF tests each against its machine code on the game's own
// calls.
#include <cstdio>
#include <deque>
#include <cstdlib>

#include "decomp.h"
#include "diff.h"
#include "frame.h"
#include "hw.h"

namespace game {
namespace {

uint32_t word(uint32_t a) { return guest::load<uint32_t>(a); }
void put(uint32_t a, uint32_t v) { guest::store<uint32_t>(a, v); }

// The interrupt level that Kamui's interrupt entry sets while it runs, 0 outside it.
constexpr uint32_t kInterruptLevel = 0x8C10D6D0u;

// Masks the interrupts and returns the mask before.
uint32_t mask_interrupts() {
    const uint32_t mask = (cpu::sr >> 4) & 15;
    cpu::set_sr((cpu::sr & 0xFFFFFF0Fu) | 0xF0);
    return mask;
}
void unmask(uint32_t mask) { cpu::set_sr((cpu::sr & 0xFFFFFF0Fu) | ((mask & 15) << 4)); }

}  // namespace

// The PowerVR's register at `param_1` from 0xA05F8000.
undefined4 kmiWriteRegister(int param_1, undefined4 param_2) {
    diff::Check check("kmiWriteRegister", 0x8C034940u, param_1, param_2);
    put(static_cast<uint32_t>(param_1) + 0xA05F8000u, param_2);
    return check.returns(1u);
}

undefined4 kmiReadRegister(int param_1) {
    diff::Check check("kmiReadRegister", 0x8C03494Au, param_1);
    return check.returns(word(static_cast<uint32_t>(param_1) + 0xA05F8000u));
}

// `param_3` bytes from `param_2` to `param_1`: in pairs of words when all three are multiples of 8,
// as the machine code's pair moves do, then in words, halfwords or bytes. A count of 0 copies 4 GB.
void kmyMemCopy(gptr<undefined8> param_1, gptr<undefined8> param_2, uint param_3) {
    diff::Check check("kmyMemCopy", 0x8C034700u, param_1, param_2, param_3);
    uint32_t to = param_1.a, from = param_2.a;
    const uint32_t align = to | from | param_3;
    if (!(align & 7)) {
        uint32_t n = param_3 >> 3;
        do {
            const uint32_t a = word(from), b = word(from + 4);
            put(to, a);
            put(to + 4, b);
            from += 8;
            to += 8;
        } while (--n != 0);
    }
    else if (!(align & 3)) {
        uint32_t n = param_3 >> 2;
        do {
            put(to, word(from));
            from += 4;
            to += 4;
        } while (--n != 0);
    }
    else if (align & 1) {
        uint32_t n = param_3;
        do guest::store<uint8_t>(to++, guest::load<uint8_t>(from++));
        while (--n != 0);
    }
    else {
        uint32_t n = param_3 >> 1;
        do {
            guest::store<uint16_t>(to, guest::load<uint16_t>(from));
            from += 2;
            to += 2;
        } while (--n != 0);
    }
}

// `param_2` zero bytes at `param_1`, by the widths of kmyMemCopy.
void kmyZeroMemory(gptr<dword> param_1, uint param_2) {
    diff::Check check("kmyZeroMemory", 0x8C034754u, param_1, param_2);
    uint32_t to = param_1.a;
    const uint32_t align = to | param_2;
    if (!(align & 7)) {
        uint32_t n = param_2 >> 3;
        do {
            put(to, 0);
            put(to + 4, 0);
            to += 8;
        } while (--n != 0);
    }
    else if (!(align & 3)) {
        uint32_t n = param_2 >> 2;
        do {
            put(to, 0);
            to += 4;
        } while (--n != 0);
    }
    else if (align & 1) {
        uint32_t n = param_2;
        do guest::store<uint8_t>(to++, 0);
        while (--n != 0);
    }
    else {
        uint32_t n = param_2 >> 1;
        do {
            guest::store<uint16_t>(to, 0);
            to += 2;
        } while (--n != 0);
    }
}

// Kamui's callback `param_1` from its table at 0x8C10D64C: the function, then its argument, which it
// gets with the callback's number and the table entry.
undefined4 kmyRequestCallback(int param_1) {
    diff::Check check("kmyRequestCallback", 0x8C03CD60u, param_1);
    const uint32_t entry = 0x8C10D64Cu + 8 * static_cast<uint32_t>(param_1);
    CallArgs args;
    args.r[0] = word(entry + 4);
    args.r[1] = static_cast<uint32_t>(param_1);
    args.r[2] = entry;
    args.nr = 3;
    guest::call(word(entry), args);
    return check.returns(0u);
}

// The callback that every entry of the table starts with.
void kmyiDefaultCallback() { diff::Check check("kmyiDefaultCallback", 0x8C03B320u); }

undefined4 kmyGetInterruptLevel() {
    diff::Check check("kmyGetInterruptLevel", 0x8C03A920u);
    return check.returns(word(kInterruptLevel));
}

undefined4 kmyGetLastTextureDMAID() {
    diff::Check check("kmyGetLastTextureDMAID", 0x8C03A960u);
    return check.returns(word(0x8C10E978u));
}

KMSTATUS kmQueryFinishLastTextureDMA() {
    diff::Check check("kmQueryFinishLastTextureDMA", 0x8C03DC30u);
    const uint32_t state = kmyQueryFinishDMA(static_cast<int>(kmyGetLastTextureDMAID()));
    return check.returns(state == 0x101 ? KMSTATUS_SUCCESS : KMSTATUS_NOT_IMPLEMENTED);
}

// Whether a texture copy may go by DMA while the CPU goes on (0x8C10E974). Not inside an interrupt.
dword kmyEnableASyncCopy(int param_1) {
    diff::Check check("kmyEnableASyncCopy", 0x8C0392A0u, param_1);
    if (kmyGetInterruptLevel() != 0u)
        return check.returns(dword(0xEEEE));
    put(0x8C10E974u, param_1 != 0 ? 1 : 0);
    return check.returns(dword(0));
}

undefined4 kmyIsEnabledASyncCopy() {
    diff::Check check("kmyIsEnabledASyncCopy", 0x8C03C280u);
    return check.returns(word(0x8C10E974u));
}

void kmySetDMAValue(int param_1, undefined4 param_2) {
    diff::Check check("kmySetDMAValue", 0x8C03D7A0u, param_1, param_2);
    put(static_cast<uint32_t>(param_1) + 0x2C, param_2);
}

undefined4 kmyGetDMAValue(int param_1) {
    diff::Check check("kmyGetDMAValue", 0x8C03D7A4u, param_1);
    return check.returns(word(static_cast<uint32_t>(param_1) + 0x2C));
}

void kmySetDMACallback(int param_1, undefined4 param_2) {
    diff::Check check("kmySetDMACallback", 0x8C03D720u, param_1, param_2);
    put(static_cast<uint32_t>(param_1) + 0x18, param_2);
}

// Whether `param_1` is one of Kamui's native buffers: the 0x8C10EA50 buffers of 0x8C bytes from
// 0x8C10EA5C.
undefined4 kmyiCheckNativeHandle(gptr<undefined> param_1) {
    diff::Check check("kmyiCheckNativeHandle", 0x8C03AAA0u, param_1);
    const uint32_t count = word(0x8C10EA50u);
    for (uint32_t i = 0; i < count; ++i)
        if (param_1.a == 0x8C10EA5Cu + 0x8C * i)
            return check.returns(1u);
    return check.returns(0u);
}

// The pass `param_2` of the native buffer `param_1`, without its bit 28.
dword kmyGetPassInfo(gptr<undefined> param_1, uint param_2, gptr<uint> param_3) {
    diff::Check check("kmyGetPassInfo", 0x8C03AA20u, param_1, param_2, param_3);
    if (!kmyiCheckNativeHandle(param_1) || word(param_1.a + 12) <= param_2)
        return check.returns(dword(0xEEEE));
    put(param_3.a, word(param_1.a + 8 * param_2 + 0x48));
    put(param_3.a, word(param_3.a) & 0xEFFFFFFFu);
    return check.returns(dword(0));
}

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
    if (kmyGetInterruptLevel() != 0u)
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

// A user clip in the list `nListType`: a polygon header with the clip mode `nPrevUserClipMode`, two
// dummy sprites and the tile clip object of the rectangle `pRect`, from Kamui's templates at
// 0x8C0CA7B0, through the store queues at the list's current pointer. The parts are built in the
// frame where the decompiled code kept them.
KMSTATUS kmSetUserClipping(PKMVERTEXBUFFDESC pVertexBuffDesc, KMUSERCLIPMODE nPrevUserClipMode,
                           KMLISTTYPE nListType, PKMRECT pRect) {
    diff::Check check("kmSetUserClipping", 0x8C036680u, pVertexBuffDesc, nPrevUserClipMode, nListType, pRect);
    guest::Frame frame(0xA0);
    const uint32_t sprites = frame.top - 0xA0, header = frame.top - 0x60, clip = frame.top - 0x40;
    for (uint32_t i = 0; i < 32; i += 4) put(clip + i, word(0x8C0CA7B0u + i));
    for (uint32_t i = 4; i < 32; i += 4) put(header + i, word(0x8C0CA7D0u + i));
    for (uint32_t i = 0; i < 64; i += 4) put(sprites + i, word(0x8C0CA7F0u + i));
    const uint32_t kind = (nListType & KM_OPAQUE_MODIFIER) == KM_OPAQUE_POLYGON ? 0xA0000000u : 0x81000040u;
    put(header, (nPrevUserClipMode << 16) | 0x800000u | kind);
    const uint32_t current = word(pVertexBuffDesc.a) + 4 * nListType;
    const auto send = [current](uint32_t from, uint32_t bytes) {
        const uint32_t to = word(current);
        kmyMemCopy(gptr<undefined8>(to), gptr<undefined8>(from), bytes);
        for (uint32_t i = 0; i < bytes; i += 32) hw::prefetch(to + i);
        put(current, to + bytes);
    };
    send(header, 32);
    send(sprites, 64);
    send(sprites, 64);
    for (uint32_t i = 0; i < 16; i += 4) put(clip + 16 + i, word(pRect.a + i));
    send(clip, 32);
    return check.returns(KMSTATUS_SUCCESS);
}

// ---- Flow control ----------------------------------------------------------------------------

namespace {

// Kamui's render packets: 8 of 0x230 bytes from 0x8C10C02C. The flow control's start clears them,
// and a closed scene keeps its settings in one (Scene below).
constexpr uint32_t kPackets = 0x8C10C02Cu;
// The flow control's state, which its start sets: the render's packet and the last one rendered, and
// the packet on display with its frame buffer.
constexpr uint32_t kRendering = 0x8C10D1DCu, kRenderPacket = 0x8C10D1E0u, kRendered = 0x8C10D1E4u;
constexpr uint32_t kDisplaying = 0x8C10D1E8u, kDisplayBuffer = 0x8C10D1ECu;
// The number of passes, and the id of the last DMA of a pass's lists.
constexpr uint32_t kPasses = 0x8C10D1F8u, kListDMA = 0x8C10D200u;
// With the flag at 0x8C10D210, the two native buffers alternate; 0x8C10D20C is the next one.
constexpr uint32_t kAlternate = 0x8C10D210u, kNextBuffer = 0x8C10D20Cu;
// The timer that Kamui takes its times from (TCNT0).
constexpr uint32_t kTimer = 0xFFD8000Cu;

// The scenes that kmRender closed, oldest first: each one's lists, the dither mode, and the packet
// whose memory keeps its frame buffer's number at 40, its clip from 420 and, when the background
// plane changed, the flag at 456 and the plane from 460. The renderer draws one at each vertical
// blank, and the display shows its frame buffer at the next one.
struct Scene {
    frame::Lists lists;
    uint32_t packet, dither;
};
std::deque<Scene> g_scenes;
// The frame buffer that the last render drew, for the next vertical blank to show, or 0.
uint32_t g_rendered = 0;

void render_registers(uint32_t b, uint32_t frame);

// Kamui's three sets of buffers: their count, the flag that defers a free while the set's mask
// changes, the mask of the deferred ones and the mask of the ones in use.
struct BufferSet {
    uint32_t count, busy, freed, used;
};
constexpr BufferSet kVertexBuffers = {0x8C10BFFCu, 0x8C10C020u, 0x8C10C014u, 0x8C10C008u};
constexpr BufferSet kNativeBuffers = {0x8C10C000u, 0x8C10C024u, 0x8C10C018u, 0x8C10C00Cu};
constexpr BufferSet kFrameBuffers = {0x8C10C004u, 0x8C10C028u, 0x8C10C01Cu, 0x8C10C010u};

// The SH-4's shad of 1: 1 << n for n from 0, else 0.
uint32_t bit(uint32_t n) { return static_cast<int32_t>(n) < 0 ? 0 : 1u << (n & 31); }

}  // namespace

// The flow control's start: the first time Kamui's flow object, else the renderer's reset and the
// release of the native buffers and the frame buffer handles. Then no packet, no buffer in use, and
// the handlers of the render's end and the vertical blank, and of the two overflows.
void kmiFLWCTRLInitFLWControl() {
    diff::Check check("kmiFLWCTRLInitFLWControl", 0x8C031940u);
    if (!word(0x8C0CF1A0u)) {
        put(0x8C0CF1A0u, 0x8C10BFFCu);
        for (uint32_t i = 0; i < 8; i += 4) put(0x8C10BE60u + i, 0);
        for (uint32_t i = 0; i < 16; i += 4) put(0x8C10BE68u + i, 0);
    }
    else {
        kmResetRenderer();
        for (uint32_t i = 0; i < 8; i += 4)
            if (word(0x8C10BE60u + i))
                kmyDeleteNativeBuffer(word(0x8C10BE5Cu), gptr<uint>(word(0x8C10BE60u + i)));
        for (uint32_t i = 0; i < 16; i += 4) {
            put(0x8C10D1C8u + i, 0xFFFFFFFFu);
            if (word(0x8C10BE68u + i))
                kmyReleaseFrameBufferHandle(gptr<undefined>(word(0x8C10BE68u + i)));
        }
    }
    put(kRendering, 0);
    put(kDisplaying, 0);
    put(kDisplayBuffer, 0xFFFFFFFFu);
    for (const BufferSet* set : {&kVertexBuffers, &kNativeBuffers, &kFrameBuffers}) {
        put(set->used, 0);
        put(set->freed, 0);
        put(set->busy, 0);
    }
    put(0x8C10D1FCu, 0);
    put(kListDMA, 0);
    put(0x8C10D208u, 0);
    for (uint32_t p = kPackets; p < kPackets + 0x1180; p += 0x230) {
        put(p, 0);
        put(p + 12, 0);
        put(p + 16, 0);
    }
    put(kRenderPacket, 0);
    put(kRendered, 0);
    put(kNextBuffer, 0);
    kmyConnectInterrupt(0x14, 0x8C031FF6u, 0);
    kmyConnectInterrupt(0x1D, 0x8C031F26u, 0);
    kmySetDMAErrorCallback(0, 0x8C031820u);
    kmySetDMAErrorCallback(1, 0x8C031860u);
}

// 0xEEEE while a scene waits for its render or its display. With alternating native buffers it
// first waits for a vertical blank and ends the alternation.
dword kmiQueryFinishAllPacket() {
    diff::Check check("kmiQueryFinishAllPacket", 0x8C0318E4u);
    if (word(kAlternate)) {
        kmWaitVBlank_8c037080();
        put(kAlternate, 0);
    }
    if (!g_scenes.empty() || g_rendered) {
        // Its callers poll it in a loop until the scenes are on display: a safe point lets time
        // pass, so the vertical blanks that render and show them come in.
        diff::own_state();
        hw::tick();
        return check.returns(dword(0xEEEE));
    }
    return check.returns(dword(0));
}

// ---- The scene and its passes

// A scene's start, after the texture DMA with flag 0x10 of the configuration. A change of the
// display set-up (0x8C10B6A4) waits for the scenes before and sets the native buffers up again.
// Then the frame's lists start. Returns 0xEEEE when a scene is open.
KMSTATUS kmBeginScene(PKMSYSTEMCONFIGSTRUCT pSystemConfigStruct) {
    guest::Frame frame(0x2C);
    const uint32_t config = pSystemConfigStruct.a;
    if (word(config + 4) & 0x10) {
        while (kmQueryFinishLastTextureDMA() != KMSTATUS_SUCCESS) {
        }
    }
    if (word(0x8C10B6D4u) & 3)
        return KMSTATUS(0xEEEE);
    put(0x8C10B6D4u, (word(0x8C10B6D4u) & ~3u) | 1);
    if (word(0x8C10B6A4u)) {
        while (kmiQueryFinishAllPacket() != 0) {
        }
        if (word(0x8C10B6A4u) & 1) {
            const uint32_t width = frame.top - 0x2C, height = frame.top - 0x28;
            for (uint32_t i = 0; i < 8; i += 4) {
                kmyGetDisplayRegionSize(word(0x8C10B694u), gptr<undefined4>(width), gptr<undefined4>(height),
                                        static_cast<int>(word(0x8C10B69Cu)));
                kmySetPassDepth(gptr<undefined>(word(0x8C10BE60u + i)), static_cast<int>(word(width)),
                                static_cast<int>(word(height)), word(config + 52));
                kmyCreateRegionArray(gptr<undefined>(word(0x8C10BE60u + i)), 0, 0, 0, 0);
                kmySetBGPlane(gptr<undefined>(word(0x8C10BE60u + i)));
            }
        }
        if (word(0x8C10B6A4u) & 2) {
            for (uint32_t i = 0; i < 8; i += 4) {
                const gptr<undefined> buffer(word(0x8C10BE60u + i));
                kmyEnableHolizontalScaler(buffer, static_cast<int>(word(0x8C10B69Cu)));
                kmySetVirticalFilterMode(buffer, word(0x8C10B6A8u));
            }
        }
        put(0x8C10B6A4u, 0);
    }
    put(0x8C10B6BCu, 0);
    put(0x8C10B6CCu, 0);
    put(word(config + 36) + 24, config + 56);
    put(0x8C10BE78u, 0);
    const uint32_t state = word(word(config + 36) + 8);
    if (word(config + 4) & 0x10) {
        put(0x8C10BE78u, 1);
        if (word(config + 4) & 0x100000)
            put(state + 24, word(state + 24) | 4);
    }
    else {
        put(state + 24, word(state + 24) & ~4u);
    }
    if (config)
        put(0x8C10BFDCu, word(config + 4) & 0x20 ? 0 : 1);
    frame::begin();
    return KMSTATUS_SUCCESS;
}

// A pass's start: its lists' current pointers at the starts of the host frame's lists (frame.h),
// then a polygon header for each list that the pass has, through the store queues.
KMSTATUS kmBeginPass(PKMVERTEXBUFFDESC pVertexBuffDesc) {
    guest::Frame frame(0x34);
    const uint32_t desc = pVertexBuffDesc.a;
    const uint32_t pass = word(word(desc + 8) + 16);
    const uint32_t current = 0x8C10B738u + 20 * pass;
    put(desc, current);
    if (word(word(desc + 8) + 24) & 4) {
        while (kmQueryFinishLastTextureDMA() != KMSTATUS_SUCCESS) {
        }
        kmyEnableASyncCopy(0);
    }
    for (uint32_t i = 0; i < 5; ++i) put(current + 4 * i, frame::start(i));
    const uint32_t info = frame.top - 0x34, lists = frame.top - 0x28;
    kmyGetPassInfo(gptr<undefined>(word(0x8C10BE60u)), word(word(desc + 8) + 16), gptr<uint>(info));
    kmySeparatePassInfo(word(info), gptr<uint>(lists), gptr<uint>(lists + 4), gptr<uint>(lists + 8),
                        gptr<uint>(lists + 12), gptr<uint>(lists + 16));
    uint32_t to = word(desc);
    for (uint32_t i = 0; i < 5; ++i, to += 4) {
        if (!word(lists + 4 * i))
            continue;
        const uint32_t at = word(to);
        put(at, (i << 24) | 0x808C0002u);
        put(at + 4, 0x80000000u);
        put(at + 8, 0x20800000u);
        hw::prefetch(at + 12);
        put(to, word(to) + 32);
    }
    return KMSTATUS_SUCCESS;
}

// A pass's end: the next pass and its block of pass information.
KMSTATUS kmEndPass(PKMVERTEXBUFFDESC pVertexBuffDesc) {
    const uint32_t desc = pVertexBuffDesc.a;
    const uint32_t state = word(desc + 8);
    put(state + 16, word(state + 16) + 1);
    put(desc + 24, word(desc + 24) + 56);
    if (word(word(desc + 8) + 24) & 4)
        kmyEnableASyncCopy(1);
    return KMSTATUS_SUCCESS;
}

// The five 2-bit fields of a pass's word, one for each list.
undefined4 kmySeparatePassInfo(uint param_1, gptr<uint> param_2, gptr<uint> param_3, gptr<uint> param_4,
                               gptr<uint> param_5, gptr<uint> param_6) {
    diff::Check check("kmySeparatePassInfo", 0x8C03D560u, param_1, param_2, param_3, param_4, param_5, param_6);
    const gptr<uint> out[5] = {param_2, param_3, param_4, param_5, param_6};
    for (uint32_t i = 0; i < 5; ++i) put(out[i].a, (param_1 >> (4 * i)) & 3);
    return check.returns(0u);
}

// The scene's end. Without flag 0x20 of the configuration and while 0x8C10BFE0, which kmRender sets
// for flip mode 1, is clear, it waits for a vertical blank, and for a second one when no native
// buffer is free.
KMSTATUS kmEndScene(PKMSYSTEMCONFIGSTRUCT pSystemConfigStruct) {
    diff::Check check("kmEndScene", 0x8C0317C0u, pSystemConfigStruct);
    put(0x8C10B6D4u, word(0x8C10B6D4u) & ~3u);
    if (pSystemConfigStruct.a && !(word(pSystemConfigStruct.a + 4) & 0x20) && !word(0x8C10BFE0u)) {
        kmWaitVBlank_8c037080();
        if (!kmiFLWCTRLCheckFreeNBuf())
            kmWaitVBlank_8c037080();
    }
    return check.returns(KMSTATUS_SUCCESS);
}

namespace {

// The render of the scene `s`: the palette, the fog and, when it changed, the background plane,
// then native buffer 0's settings for the scene's frame buffer, and the lists drawn.
void render(const Scene& s) {
    const uint32_t p = s.packet;
    kmyUpdatePalette();
    kmyUpdateFogParameter();
    if (word(p + 456)) {
        put(0x8C10B5ECu, word(0x8C10B5ECu) | 1);
        put(0x8C10B5FCu, word(p + 484));
        kmyRegisterBGPlane(gptr<uint>(p + 460));
        for (uint32_t i = 0; i < 8; i += 4) put(0x8C10B5F4u + i, word(0x8C10B5F4u + i) | 1);
        put(0x8C10B5ECu, word(0x8C10B5ECu) & ~1u);
    }
    const gptr<undefined> buffer(word(0x8C10D1B0u));
    if (word(0x8C10BEB4u) == 1)
        kmyResetRenderer();
    const uint32_t frame_buffer = word(0x8C10D1B8u + 4 * word(p + 40));
    kmySetDitherMode(buffer, static_cast<int>(s.dither));
    kmySetPixelClippingX(buffer, word(p + 420), word(p + 428) - 1);
    kmySetPixelClippingY(buffer, word(p + 424), word(p + 432) - 1);
    kmiSetupRender(0, buffer);
    render_registers(buffer.a, frame_buffer);
    hw::render(frame::stream(s.lists));
    g_rendered = frame_buffer;
}

}  // namespace

// Closes the scene that is open (state 1 in the low bits of 0x8C10B6D4): it waits for the next
// vertical blank to render, with a flip to the next frame buffer unless `dwFlipMode` is 1. Without
// the wait for the vertical blank (0x8C10BFDC) it renders now. The scene keeps its clip and, when
// it changed, the background plane in a packet's memory. The lists' end calls Kamui's callback 5
// at once, as the end of their transfer did. Returns the scene's id, -1 when no scene is open.
KMINT32 kmRender(KMDWORD dwFlipMode) {
    if ((word(0x8C10B6D4u) & 3) != 1)
        return KMINT32(-1);
    put(0x8C10B6D4u, (word(0x8C10B6D4u) & ~3u) | 2);
    put(0x8C10BFE0u, dwFlipMode == 1 ? 1 : 0);
    const uint32_t id = word(0x8C10D208u) & 0x7FFFFFFFu;
    put(0x8C10D208u, word(0x8C10D208u) + 1);
    Scene s{frame::take(), kPackets + 0x230 * (id % 8), word(0x8C10B698u)};
    const uint32_t p = s.packet;
    for (uint32_t i = 0; i < 32; i += 4) put(p + 420 + i, word(0x8C10BE90u + i));
    const uint32_t plane = word(0x8C10B5F0u) & 0x100;
    put(p + 456, plane);
    if (plane) {
        put(0x8C10B5F0u, word(0x8C10B5F0u) & ~0x100u);
        for (uint32_t i = 0; i < 100; i += 4) put(p + 460 + i, word(0x8C10B600u + i));
    }
    put(p + 40, word(0x8C10D1FCu));
    const bool flip = dwFlipMode != 1;
    if (flip) {
        put(0x8C10D1FCu, word(0x8C10D1FCu) + 1);
        if (word(0x8C10D1FCu) >= word(kFrameBuffers.count))
            put(0x8C10D1FCu, 0);
        if (word(kAlternate))
            put(0x8C10D1FCu, word(0x8C10D1FCu) & 1);
    }
    kmyRequestCallback(5);
    if (word(0x8C10BFDCu) && flip)
        g_scenes.push_back(std::move(s));
    else {
        render(s);
        kmyRequestCallback(0);
    }
    put(0x8C10B6C8u, word(0x8C10B6C8u) + 1);
    put(0x8C10B6C8u, _modlu(word(0x8C10B6B8u), word(0x8C10B6C8u)));
    return KMINT32(id);
}

// ---- The packets' way through the hardware, mostly from the interrupts

// The vertical blank: the frame buffer that the last render drew goes on display, the oldest scene
// renders, then Kamui's callback 1, the frame count at 0x8C10BFC8 and, after a render, callback 0,
// which the end of the render called.
undefined4 kmiFLWCTRLVsyncCallBackFunc(float arg_fr4, float arg_fr5, float arg_fr6, float arg_fr7) {
    (void)arg_fr4, (void)arg_fr5, (void)arg_fr6, (void)arg_fr7;
    if (g_rendered) {
        if (word(0x8C10B6B0u))
            kmySetDisplayFrameBuffer(static_cast<int>(g_rendered), static_cast<int>(g_rendered));
        else if (!word(0x8C10B6B4u))
            kmySetDisplayFrameBuffer(static_cast<int>(g_rendered), 0);
        if (word(0x8C10BFC4u)) {
            put(0x8C10BFC4u, 0);
            kmyBlankScreen(0);
        }
        g_rendered = 0;
    }
    const bool rendering = !g_scenes.empty();
    if (rendering) {
        render(g_scenes.front());
        g_scenes.pop_front();
    }
    kmyRequestCallback(1);
    put(0x8C10BFC8u, word(0x8C10BFC8u) + 1);
    if (rendering)
        kmyRequestCallback(0);
    return 0u;
}

// ---- The renderer and the display -------------------------------------------------------------

// The renderer's settings that changed since the last render: the mask at 0x8C10B5F0 without the
// ones that 0x8C10B5EC holds back, with their values from 0x8C10B5EC. The background plane of the
// native buffer `param_1` goes into `param_2` when its flag at 0x8C10B5F4 is set.
undefined4 kmiSetupRender(int param_1, gptr<undefined> param_2) {
    diff::Check check("kmiSetupRender", 0x8C0338E0u, param_1, param_2);
    const uint32_t changed = word(0x8C10B5F0u) & ~word(0x8C10B5ECu);
    put(0x8C10B5F0u, 0);
    const uint32_t plane = 0x8C10B5F4u + 4 * static_cast<uint32_t>(param_1);
    if (word(plane) & 1) {
        kmySetBackGroundDepth(param_2, guest::load<float>(0x8C10B5FCu));
        kmySetBGPlane(param_2);
        put(plane, word(plane) & ~1u);
    }
    if (changed & 2)
        kmySetCullingValue(guest::load<float>(0x8C10B670u));
    if (changed & 4)
        kmySetCheapShadowMode(static_cast<int>(word(0x8C10B668u)), word(0x8C10B66Cu));
    if (changed & 8)
        kmySetStrideWidth(word(0x8C10B664u));
    if (changed & 0x10)
        kmySetColorClampMAX(word(0x8C10B674u));
    if (changed & 0x20)
        kmySetColorClampMIN(word(0x8C10B678u));
    if (changed & 0x40)
        kmySetPunchThroughThreshold(word(0x8C10B67Cu));
    if (changed & 0x80)
        for (uint32_t i = 0; i < 8; i += 4)
            kmySetYCoefficient(gptr<undefined>(word(0x8C10BE60u + i)), word(0x8C10B680u), word(0x8C10B684u));
    return check.returns(0u);
}

namespace {

// The renderer's registers for a render of the native buffer `b` into the frame buffer `frame`.
void render_registers(uint32_t b, uint32_t frame) {
    const uint32_t regs = word(b + 136);
    const uint32_t scaler = word(0x8C10EDF8u);
    put(0x8C10EDF8u, word(regs + 36) & 0xFFFF);
    if (word(0x8C10EDF8u) != scaler)
        kmyResetRenderer();
    kmiWriteRegister(0x2C, word(b + 36));
    kmiWriteRegister(0x20, word(b + 32));
    kmiWriteRegister(0xF4, word(regs + 36));
    kmiWriteRegister(0x118, word(regs + 40));
    kmiWriteRegister(0x48, word(regs + 4));
    kmiWriteRegister(0x8C, word(regs + 8));
    kmiWriteRegister(0x88, word(regs + 12) & ~15u);
    kmiWriteRegister(0x68, word(regs + 20));
    kmiWriteRegister(0x6C, word(regs + 24));
    kmiWriteRegister(0x4C, word(frame + 44));
    if (!word(0x8C10D2FCu))
        kmiWriteRegister(0x60, word(frame + 28));
}

}  // namespace

dword kmySetDitherMode(gptr<undefined> param_1, int param_2) {
    diff::Check check("kmySetDitherMode", 0x8C03CB66u, param_1, param_2);
    if (!kmyiCheckNativeHandle(param_1))
        return check.returns(dword(0xEEEE));
    const uint32_t mode = word(param_1.a + 136) + 4;
    put(mode, param_2 ? word(mode) | 8 : word(mode) & ~8u);
    return check.returns(dword(0));
}

// The renderer's clip, from `param_2` to `param_3`, into the native buffer's registers.
dword kmySetPixelClippingX(gptr<undefined> param_1, uint param_2, uint param_3) {
    diff::Check check("kmySetPixelClippingX", 0x8C037C40u, param_1, param_2, param_3);
    if (!kmyiCheckNativeHandle(param_1))
        return check.returns(dword(0xEEEE));
    put(word(param_1.a + 136) + 20, (param_3 & 0x7FF) << 16 | (param_2 & 0x7FF));
    return check.returns(dword(0));
}

dword kmySetPixelClippingY(gptr<undefined> param_1, uint param_2, uint param_3) {
    diff::Check check("kmySetPixelClippingY", 0x8C037C7Eu, param_1, param_2, param_3);
    if (!kmyiCheckNativeHandle(param_1))
        return check.returns(dword(0xEEEE));
    put(word(param_1.a + 136) + 24, (param_3 & 0x7FF) << 16 | (param_2 & 0x7FF));
    return check.returns(dword(0));
}

// The frame buffers `param_1` and `param_2` on display, as the two fields' addresses. Unless the
// display is interlaced (bit 0 of 0x8C10D244, or its bits 0x2C are 0x20) or 0x8C10D2FC is 1, one
// of them gives both fields, with the fields' offsets at 0x8C10D2D8. With 0x8C10D2FC at 1 the
// renderer's addresses follow.
dword kmySetDisplayFrameBuffer(int param_1, int param_2) {
    diff::Check check("kmySetDisplayFrameBuffer", 0x8C0397B6u, param_1, param_2);
    const uint32_t mode = word(0x8C10D244u);
    uint32_t odd, even;
    if (((mode & 1) == 1 || (mode & 0x2C) != 0x20) && word(0x8C10D2FCu) != 1) {
        const uint32_t from = param_1 ? static_cast<uint32_t>(param_1) : static_cast<uint32_t>(param_2);
        if (!from)
            return check.returns(dword(0xEEEE));
        odd = word(from + 28) + word(0x8C10D2D8u);
        even = word(from + 28) + word(0x8C10D2DCu);
    }
    else {
        if (!param_1 || !param_2)
            return check.returns(dword(0xEEEE));
        odd = word(static_cast<uint32_t>(param_1) + 28);
        even = word(static_cast<uint32_t>(param_2) + 28);
        if (word(0x8C10D2FCu) == 1) {
            kmiWriteRegister(0x60, odd);
            kmiWriteRegister(0x64, even);
        }
    }
    kmiWriteRegister(0x50, odd);
    kmiWriteRegister(0x54, even);
    return check.returns(dword(0));
}

// The palette's changes, from 0x8C10E938: 1 the palette's format, 2 all 1,024 entries, else with 4
// the banks of 16 entries in the masks at 0x8C10E93C and 0x8C10E940, from Kamui's copy at
// 0x8C10D938.
undefined4 kmyUpdatePalette() {
    diff::Check check("kmyUpdatePalette", 0x8C03C478u);
    constexpr uint32_t changes = 0x8C10E938u, palette = 0x8C10D938u;
    if (word(changes) & 1)
        kmiWriteRegister(0x108, word(0x8C10E944u));
    if (word(changes) & 2) {
        for (uint32_t i = 0; i < 0x1000; i += 4) kmiWriteRegister(static_cast<int>(0x1000 + i), word(palette + i));
    }
    else if (word(changes) & 4) {
        const auto copy = [](uint32_t banks, uint32_t first) {
            if (!word(banks))
                return;
            for (uint32_t i = 0; i < 32; ++i)
                if (bit(i) & word(banks)) {
                    const uint32_t at = 64 * (first + i);
                    kmyMemCopy(gptr<undefined8>(0xA05F9000u + at), gptr<undefined8>(palette + at), 64);
                }
        };
        copy(0x8C10E93Cu, 0);
        copy(0x8C10E940u, 32);
        put(0x8C10E93Cu, 0);
        put(0x8C10E940u, 0);
    }
    put(changes, 0);
    return check.returns(0u);
}

// The fog's changes, from 0x8C10D618: 4 the table, 8 the vertex fog's colour, 1 the table's colour
// and 2 the density.
void kmyUpdateFogParameter() {
    diff::Check check("kmyUpdateFogParameter", 0x8C039520u);
    constexpr uint32_t changes = 0x8C10D618u;
    if (word(changes) & 4)
        kmyWriteFogTable();
    const auto update = [](uint32_t flag, int reg, uint32_t value) {
        if (!(word(changes) & flag))
            return;
        put(changes, word(changes) & ~flag);
        kmiWriteRegister(reg, word(value));
    };
    update(8, 0xB4, 0x8C10D61Cu);
    update(1, 0xB0, 0x8C10D620u);
    update(2, 0xB8, 0x8C10D624u);
}

// The fog table from Kamui's copy at 0x8C10D418 into the PowerVR's, when it changed.
undefined4 kmyWriteFogTable() {
    diff::Check check("kmyWriteFogTable", 0x8C0394AEu);
    if (word(0x8C10D618u) & 4) {
        put(0x8C10D618u, word(0x8C10D618u) & ~4u);
        for (uint32_t i = 0; i < 0x200; i += 4) kmiWriteRegister(static_cast<int>(0x200 + i), word(0x8C10D418u + i));
    }
    return check.returns(0x78u);
}

// The fog table of 128 densities `pfFogTable`, from 0 to 1, into Kamui's copy: each entry holds its
// density and the next one as bytes.
KMSTATUS kmSetFogTable(PKMFLOAT pfFogTable) {
    diff::Check check("kmSetFogTable", 0x8C0364E0u, pfFogTable);
    const float scale = guest::load<float>(0x8C036544u);
    const uint32_t table = pfFogTable.a;
    uint32_t next = cpu::ftrc(guest::load<float>(table + 508) * scale) & 0xFF;
    for (uint32_t i = 128; i-- != 0;) {
        const uint32_t density = cpu::ftrc(guest::load<float>(table + 4 * i) * scale) & 0xFF;
        put(0x8C10D418u + 4 * i, ((density << 8) | next) & 0xFFFF);
        next = density;
    }
    put(0x8C10D618u, word(0x8C10D618u) | 4);
    return check.returns(KMSTATUS_SUCCESS);
}

KMSTATUS kmSetFogDensity(KMDWORD fogDensity) {
    diff::Check check("kmSetFogDensity", 0x8C0364A0u, fogDensity);
    put(0x8C10D624u, fogDensity);
    put(0x8C10D618u, word(0x8C10D618u) | 2);
    return check.returns(KMSTATUS_SUCCESS);
}

KMSTATUS kmSetFogTableColor(undefined4 arg_r4, undefined4 arg_r5, undefined4 arg_r6, undefined4 arg_r7,
                            KMPACKEDARGB FogTableColor) {
    diff::Check check("kmSetFogTableColor", 0x8C036560u, arg_r4, arg_r5, arg_r6, arg_r7, FogTableColor);
    put(0x8C10D620u, FogTableColor.dwPacked);
    put(0x8C10D618u, word(0x8C10D618u) | 1);
    return check.returns(KMSTATUS_SUCCESS);
}

// ---- The region array and the passes ----------------------------------------------------------

// The pass `param_2` of the native buffer `param_1`, `param_3` with its bit 28 set unless it is
// the last pass.
dword kmySetPassInfo(gptr<undefined> param_1, uint param_2, uint param_3) {
    diff::Check check("kmySetPassInfo", 0x8C03DA00u, param_1, param_2, param_3);
    if (!kmyiCheckNativeHandle(param_1) || word(param_1.a + 12) <= param_2)
        return check.returns(dword(0xEEEE));
    const uint32_t info = param_2 + 1 == word(param_1.a + 12) ? param_3 & 0xEFFFFFFFu : param_3 | 0x10000000u;
    put(param_1.a + 8 * param_2 + 0x48, info);
    return check.returns(dword(0));
}

// A pass's word from the modes of its five lists, `param_2` to `param_6`.
undefined4 kmyGeneratePassInfo(gptr<uint> param_1, uint param_2, uint param_3, uint param_4, uint param_5,
                               uint param_6) {
    diff::Check check("kmyGeneratePassInfo", 0x8C03A0E0u, param_1, param_2, param_3, param_4, param_5, param_6);
    put(param_1.a, (param_3 & 3) << 4 | (param_4 & 3) << 8 | (param_5 & 3) << 12 | (param_6 & 3) << 16 |
                       (param_2 & 3) | 0x100000u);
    return check.returns(0u);
}

void kmyEnablePassInfoZClear(gptr<uint> param_1) {
    diff::Check check("kmyEnablePassInfoZClear", 0x8C039420u, param_1);
    put(param_1.a, word(param_1.a) & 0xBFFFFFFFu);
}

void kmyDisablePassInfoPreSort(gptr<uint> param_1) {
    diff::Check check("kmyDisablePassInfoPreSort", 0x8C038680u, param_1);
    put(param_1.a, word(param_1.a) & 0xDFFFFFFFu);
}

// The display's size in tiles of 32 pixels for the display mode `param_1`, with `param_4` for the
// horizontal scaler.
undefined4 kmyGetDisplayRegionSize(uint param_1, gptr<undefined4> param_2, gptr<undefined4> param_3, int param_4) {
    diff::Check check("kmyGetDisplayRegionSize", 0x8C03A540u, param_1, param_2, param_3, param_4);
    uint32_t width, height = 15;
    if (!param_4) {
        width = param_1 & 0x10 ? 20 : 10;
        if (!(param_1 & 0x20))
            height = 8;
    }
    else {
        width = param_1 & 0x10 ? 40 : 20;
    }
    if (param_2.a)
        put(param_2.a, width);
    if (param_3.a)
        put(param_3.a, height);
    return check.returns(0u);
}

// The native buffer `param_1`'s tiles (`param_2` by `param_3`) and passes (`param_4`): the region
// array's place at the end of its vertex buffer, below the background plane.
dword kmySetPassDepth(gptr<undefined> param_1, int param_2, int param_3, undefined4 param_4) {
    diff::Check check("kmySetPassDepth", 0x8C03D900u, param_1, param_2, param_3, param_4);
    if (!kmyiCheckNativeHandle(param_1))
        return check.returns(dword(0xEEEE));
    const uint32_t b = param_1.a, regs = word(b + 136);
    put(b + 12, param_4);
    put(b + 4, static_cast<uint32_t>(param_2));
    put(b + 8, static_cast<uint32_t>(param_3));
    put(b + 56, static_cast<uint32_t>(param_3 - 1) << 16 | static_cast<uint32_t>(param_2 - 1));
    put(b + 32, word(b + 24));
    put(b + 36, word(b + 24) + word(b + 28) - word(b + 4) * word(b + 8) * word(b + 12) * 24);
    if (word(0x8C10E964u) & 0x80)
        put(b + 36, word(b + 36) - 24);
    put(b + 60, word(b + 36) - 0x100);
    put(regs + 20, word(0x8C10D250u));
    put(regs + 24, word(0x8C10D254u));
    put(regs + 16, word(0x8C10D260u));
    put(regs + 4, word(0x8C10D268u));
    return check.returns(dword(0));
}

// The renderer's tile skip for the display mode `param_2`.
dword kmySetSkipCount(gptr<undefined> param_1, uint param_2) {
    diff::Check check("kmySetSkipCount", 0x8C03A140u, param_1, param_2);
    if (!kmyiCheckNativeHandle(param_1))
        return check.returns(dword(0xEEEE));
    uint32_t skip = 1;
    if (param_2 & 0x2000000) {
        skip = param_2 & 0x1000000 ? 3 : 2;
        if (!(param_2 & 0x400000))
            ++skip;
    }
    const uint32_t regs = word(param_1.a + 136);
    put(regs + 8, (word(regs + 8) & 0xFFFFFF) | skip << 24);
    return check.returns(dword(0));
}

dword kmyGetBGPlaneBase(gptr<undefined> param_1, gptr<undefined4> param_2) {
    diff::Check check("kmyGetBGPlaneBase", 0x8C03A198u, param_1, param_2);
    if (!kmyiCheckNativeHandle(param_1))
        return check.returns(dword(0xEEEE));
    put(param_2.a, word(param_1.a + 60));
    return check.returns(dword(0));
}

// The callback that gives the region array's tiles their order.
dword kmySetRegionArrayCallback(gptr<undefined> param_1, int param_2) {
    diff::Check check("kmySetRegionArrayCallback", 0x8C03DAC0u, param_1, param_2);
    if (!kmyiCheckNativeHandle(param_1) || !param_2)
        return check.returns(dword(0xEEEE));
    put(param_1.a + 20, static_cast<uint32_t>(param_2));
    return check.returns(dword(0));
}

// The address in the 32-bit path of video memory for the address `param_1` in the 64-bit path.
int kmyConvertAddress64to32(uint param_1) {
    diff::Check check("kmyConvertAddress64to32", 0x8C037E00u, param_1);
    uint32_t a = ((param_1 & 0xFFFFFFF8u) >> 1) + (param_1 & 3);
    if (param_1 & 4)
        a += 0x400000;
    if (param_1 > 0x7FFFFF)
        a += 0x400000;
    return check.returns(static_cast<int>(a));
}

// The tiles in right-to-left columns: the tile `param_1[0]` of a display `param_1[3]` by
// `param_1[4]` tiles is at column `param_1[1]`, row `param_1[2]`. A tile outside the clip at
// 0x8C10BFCC is empty (`param_1[5]`).
void kmiVRightArray(gptr<uint> param_1) {
    diff::Check check("kmiVRightArray", 0x8C03E680u, param_1);
    const uint32_t t = param_1.a;
    put(t + 4, word(t + 12) - word(t) / word(t + 16) - 1);
    put(t + 8, word(t) % word(t + 16));
    const uint32_t row = word(t + 8), column = word(t + 4);
    if (row > word(0x8C10BFD8u) || row < word(0x8C10BFD0u) || column > word(0x8C10BFD4u) || column < word(0x8C10BFCCu))
        put(t + 20, 1);
}

// An empty first tile at `param_1`, for the renderer's flag 0x80 at 0x8C10E964.
undefined4 FUN_8c0380c0(int param_1) {
    diff::Check check("FUN_8c0380c0", 0x8C0380C0u, param_1);
    guest::Frame frame(0x1C);
    const uint32_t tile = frame.top - 0x1C;
    put(tile, 0x10000000u);
    for (uint32_t i = 4; i < 24; i += 4) put(tile + i, 0x80000000u);
    kmyCopyToFrameBuffer(param_1, gptr<undefined8>(tile), 24);
    return check.returns(24u);
}

// The region array of the native buffer `param_1`: for each tile, in the order of its callback at
// 20, an entry for each pass with the tile's position and its five lists' addresses. A list's size
// a tile comes from 0x8C0CA8EC by the list's mode in the pass's word, 0 for no list. A tile that
// the callback marks empty gets empty lists, and a pass's flags give a list the address of another
// list or of the pass before. The entries go to the frame buffer at the native buffer's word 36.
void kmyGenerateRegionArray(int param_1, gptr<dword> arg_r5, uint arg_r6, float arg_fr4, float arg_fr5, float arg_fr6,
                            float arg_fr7) {
    diff::Check check("kmyGenerateRegionArray", 0x8C0380EAu, param_1, arg_r5, arg_r6, arg_fr4, arg_fr5, arg_fr6,
                      arg_fr7);
    guest::Frame frame(0x3C8);
    const uint32_t b = static_cast<uint32_t>(param_1);
    const uint32_t tile = frame.top - 928, entries = frame.top - 896, lists = frame.top - 704;
    const uint32_t tiles = word(b + 4) * word(b + 8);
    for (uint32_t pass = 0; pass < word(b + 12); ++pass) {
        const uint32_t d = lists + 84 * pass, info = word(b + 72 + 8 * pass);
        put(d, info & 0x7F000000u);
        uint32_t offset = 0;
        for (uint32_t i = 0, l = d + 24; l < d + 84; ++i, l += 12) {
            const uint32_t mode = (info >> (4 * i)) & 3;
            put(l + 4, word(b + 76 + 8 * pass) + offset);
            put(l + 8, word(0x8C0CA8ECu + 4 * mode) * 4);
            offset += word(l + 8) * tiles;
            put(l, mode ? 0 : 0x80000000u);
        }
    }
    uint32_t base = word(b + 36);
    if (word(0x8C10E964u) & 0x80)
        base += FUN_8c0380c0(static_cast<int>(word(b + 36)));
    put(tile + 12, word(b + 4));
    put(tile + 16, word(b + 8));
    const auto send = [&](uint32_t n) {
        const uint32_t passes = word(b + 12);
        kmyCopyToFrameBuffer(static_cast<int>(base + n * passes * 24), gptr<undefined8>(entries), passes * 24);
    };
    for (uint32_t n = 0; n < tiles; ++n) {
        put(tile, n);
        put(tile + 20, 0);
        put(tile + 24, 0);
        if (n == tiles - 1)
            put(tile + 24, word(tile + 24) | 1);
        gptr<code>(word(b + 20))(tile);
        const uint32_t x = word(tile + 4), y = word(tile + 8);
        const uint32_t position = (x & 63) << 2 | (y & 63) << 8, at = word(tile + 12) * y + x;
        for (uint32_t pass = 0; pass < word(b + 12); ++pass) {
            const uint32_t d = lists + 84 * pass, e = entries + 24 * pass, flags = b + 72 + 8 * pass;
            put(e, word(d) | position);
            for (uint32_t i = 0; i < 5; ++i) {
                const uint32_t l = d + 24 + 12 * i;
                const uint32_t end = word(tile + 20) == 1 ? 0x80000000u : word(l);
                put(e + 4 + 4 * i, (at * word(l + 8) + word(l + 4)) | end);
            }
            if (word(flags) & 0x40)
                put(e + 8, word(e + 16));
            if (word(flags) & 0x4000)
                put(e + 16, word(e + 8));
            if (word(flags) & 0x40000)
                put(e + 20, word(e + 12));
            if (pass > 0) {
                if (word(flags) & 0x80)
                    put(e + 8, word(e - 16));
                if (word(flags) & 0x8000)
                    put(e + 16, word(e - 8));
            }
        }
        if (word(tile + 24) & 1)
            break;
        send(n);
    }
    const uint32_t last = entries + 24 * (word(b + 12) - 1);
    put(last, word(last) | 0x80000000u);
    send(word(tile));
}

// The region array of the native buffer `param_1`, after the lists' room: each pass's lists start
// at word 40, below the region array, and the object buffer takes three quarters of the rest.
// Returns 0xEEEE for no passes, more than 8 or too little room.
dword kmyCreateRegionArray(gptr<undefined> param_1, float arg_fr4, float arg_fr5, float arg_fr6, float arg_fr7) {
    diff::Check check("kmyCreateRegionArray", 0x8C0384C0u, param_1, arg_fr4, arg_fr5, arg_fr6, arg_fr7);
    guest::Frame frame(0x40);
    if (!kmyiCheckNativeHandle(param_1))
        return check.returns(dword(0xEEEE));
    const uint32_t b = param_1.a;
    if (!word(b + 12) || word(b + 12) > 8)
        return check.returns(dword(0xEEEE));
    const uint32_t tiles = word(b + 4) * word(b + 8), entries = word(b + 12) * tiles * 24;
    const uint32_t sizes = frame.top - 0x40;
    uint32_t total = 0;
    for (uint32_t pass = 0; pass < word(b + 12); ++pass) {
        const uint32_t info = word(b + 72 + 8 * pass);
        uint32_t per_tile = 0;
        for (const uint32_t shift : {4u, 8u, 12u, 16u, 0u}) per_tile += word(0x8C0CA8ECu + 4 * ((info >> shift) & 3));
        put(sizes + 4 * pass, per_tile * tiles * 4);
        total += word(sizes + 4 * pass);
    }
    if (total + entries > word(b + 28))
        return check.returns(dword(0xEEEE));
    put(b + 40, (word(b + 36) - total - 0x100) & ~127u);
    put(b + 48, (((word(b + 28) - total - entries) >> 2) * 3 + word(b + 24)) & ~31u);
    put(b + 52, word(b + 40));
    put(b + 44, word(b + 48) - 0x100);
    const uint32_t regs = word(b + 136);
    put(regs + 8, (word(regs + 8) & 0xFF000000u) | (word(b + 60) - word(b + 32)) << 1);
    uint32_t offset = 0;
    for (uint32_t pass = 0; pass < word(b + 12); ++pass) {
        put(b + 76 + 8 * pass, word(b + 40) + offset);
        offset += word(sizes + 4 * pass);
    }
    kmyGenerateRegionArray(static_cast<int>(b), gptr<dword>(b + 24), b, arg_fr4, arg_fr5, arg_fr6, arg_fr7);
    return check.returns(dword(0));
}

// ---- The background plane and the renderer's set-up ---------------------------------------------

namespace {

// Kamui's background plane for the region array, in the parameter format: its size in bytes at
// 0x8C10E980, its words from 0x8C10E984.
constexpr uint32_t kPlaneSize = 0x8C10E980u, kPlane = 0x8C10E984u;

// A colour component from 0 to 1 as a byte: the machine code's unsigned conversion, then the low
// byte.
uint32_t colour_byte(float f) {
    float v = f * guest::load<float>(0x8C034FF0u);
    if (!(2147483648.0f > v))
        v -= 4294967296.0f;
    return cpu::ftrc(v) & 0xFF;
}

}  // namespace

// The background plane's render state from the vertex context `pVertexContext`: its four
// parameter words and its face and offset colours as packed ARGB.
KMSTATUS kmSetBackGroundRenderState(PKMVERTEXCONTEXT pVertexContext) {
    diff::Check check("kmSetBackGroundRenderState", 0x8C034EC0u, pVertexContext);
    const uint32_t c = pVertexContext.a;
    for (uint32_t i = 0; i < 16; i += 4) put(0x8C10D214u + i, word(c + 144 + i));
    const auto argb = [c](uint32_t at) {
        uint32_t packed = colour_byte(guest::load<float>(c + at)) << 24;
        packed |= colour_byte(guest::load<float>(c + at + 4)) << 16;
        packed |= colour_byte(guest::load<float>(c + at + 8)) << 8;
        return packed | colour_byte(guest::load<float>(c + at + 12));
    };
    put(0x8C10D224u, argb(112));
    put(0x8C10D228u, argb(128));
    return check.returns(KMSTATUS_SUCCESS);
}

// The background plane's three vertices `vertex1` to `vertex3` with the parameter words `head`,
// into Kamui's copy from 0x8C10B600: each vertex's position and its colour.
undefined4 FUN_8c035080(gptr<void> head, gptr<void> vertex1, gptr<void> vertex2, gptr<void> vertex3) {
    diff::Check check("FUN_8c035080", 0x8C035080u, head, vertex1, vertex2, vertex3);
    for (uint32_t i = 0; i < 16; i += 4) put(0x8C10B600u + i, word(head.a + i));
    const uint32_t vertices[3] = {vertex1.a, vertex2.a, vertex3.a};
    for (uint32_t i = 0; i < 3; ++i) {
        const uint32_t to = 0x8C10B610u + 28 * i, v = vertices[i];
        put(to, word(v + 4));
        put(to + 4, word(v + 8));
        put(to + 8, word(v + 12));
        put(to + 20, word(v + 24));
    }
    return check.returns(0u);
}

// The background plane from the vertices `pVertex` of the type `VertexType`, by the type's function
// in the table at 0x8C0CF1A8. The plane goes to the next render.
KMSTATUS kmSetBackGroundPlane(gptr<PVOID> pVertex, KMVERTEXTYPE VertexType, KMINT32 StructSize) {
    diff::Check check("kmSetBackGroundPlane", 0x8C035000u, pVertex, VertexType, StructSize);
    const uint32_t type = static_cast<uint32_t>(VertexType), v = pVertex.a;
    const KMSTATUS result = gptr<code>(word(0x8C0CF1A8u + 4 * type))(0x8C10D214u, word(v), word(v + 4),
                                                                      word(v + 8), VertexType);
    put(0x8C10B5F0u, word(0x8C10B5F0u) | 0x100);
    return check.returns(result);
}

// The background plane `param_1` in Kamui's format into the parameter format at 0x8C10E984: the
// control word with the mode bits of word 0, the ISP and TSP words, then the three vertices. A
// vertex has its texture coordinates (one word when packed), its colour and its offset colour only
// as the mode asks.
undefined4 kmyRegisterBGPlane(gptr<uint> param_1) {
    diff::Check check("kmyRegisterBGPlane", 0x8C03C820u, param_1);
    const uint32_t in = param_1.a;
    const uint32_t control = (word(in + 4) & 0xFC300000u) | (word(in) & 15) << 22;
    const bool textured = control >> 25 & 1, packed = control >> 22 & 1, offset = control >> 24 & 1;
    uint32_t n = 0;
    const auto add = [&](uint32_t from) {
        put(kPlane + 4 * n, word(in + 4 * from));
        ++n;
    };
    put(kPlane, control);
    n = 1;
    add(2);
    add(3);
    for (uint32_t v = 0; v < 3; ++v) {
        const uint32_t w = 4 + 7 * v;
        add(w);
        add(w + 1);
        add(w + 2);
        if (textured) {
            add(w + 3);
            if (!packed)
                add(w + 4);
            add(w + 5);
            if (offset)
                add(w + 6);
        }
        else {
            add(w + 5);
        }
    }
    put(kPlaneSize, 4 * n);
    return check.returns(0u);
}

dword kmySetBackGroundDepth(gptr<undefined> param_1, float param_2) {
    diff::Check check("kmySetBackGroundDepth", 0x8C03D5A0u, param_1, param_2);
    if (!kmyiCheckNativeHandle(param_1))
        return check.returns(dword(0xEEEE));
    guest::store<float>(word(param_1.a + 136) + 12, param_2);
    return check.returns(dword(0));
}

// The background plane into the native buffer `param_1`'s place for it, with its skip count.
void kmySetBGPlane(gptr<undefined> param_1) {
    diff::Check check("kmySetBGPlane", 0x8C03D600u, param_1);
    guest::Frame frame(0x10);
    const uint32_t base = frame.top - 0x10;
    put(frame.top - 0xC, param_1.a);
    kmyGetBGPlaneBase(param_1, gptr<undefined4>(base));
    kmyCopyToFrameBuffer(static_cast<int>(word(base)), gptr<undefined8>(kPlane), word(kPlaneSize));
    kmySetSkipCount(gptr<undefined>(word(frame.top - 0xC)), word(kPlane));
}

// A black background plane at the far depth for the native buffer `param_1`.
void kmyInitializeBGPlane(gptr<undefined> param_1) {
    diff::Check check("kmyInitializeBGPlane", 0x8C03B260u, param_1);
    guest::Frame frame(0x10);
    static constexpr uint32_t kDefault[15] = {0x80800000u, 0x20800000u, 0,           0x40000000u, 0x43820000u,
                                              0x3727C5ACu, 0xFF000000u, 0x43160000u, 0x40000000u, 0x3727C5ACu,
                                              0xFF000000u, 0x440C0000u, 0x43820000u, 0x3727C5ACu, 0xFF000000u};
    for (uint32_t i = 0; i < 15; ++i) put(kPlane + 4 * i, kDefault[i]);
    put(kPlaneSize, 0x3C);
    kmySetBackGroundDepth(param_1, pun<float>(0x3727C5ACu));
    const uint32_t base = frame.top - 0x10;
    kmyGetBGPlaneBase(param_1, gptr<undefined4>(base));
    kmyCopyToFrameBuffer(static_cast<int>(word(base)), gptr<undefined8>(kPlane), word(kPlaneSize));
    kmySetSkipCount(param_1, word(kPlane));
}

void kmyResetRenderer() {
    diff::Check check("kmyResetRenderer", 0x8C03D214u);
    kmiWriteRegister(8, 2);
    kmiWriteRegister(8, 0);
}

// The renderer's culling threshold: the absolute value of `param_1`.
undefined4 kmySetCullingValue(float param_1) {
    diff::Check check("kmySetCullingValue", 0x8C03D6E0u, param_1);
    kmiWriteRegister(0x78, pun<uint32_t>(param_1) & 0x7FFFFFFFu);
    return check.returns(0u);
}

// The vertical filter's coefficient for the mode `param_2` in the native buffer `param_1`'s scaler
// word.
dword kmySetVirticalFilterMode(gptr<undefined> param_1, uint param_2) {
    diff::Check check("kmySetVirticalFilterMode", 0x8C03DD40u, param_1, param_2);
    if (!kmyiCheckNativeHandle(param_1))
        return check.returns(dword(0xEEEE));
    uint32_t coefficient;
    switch (param_2) {
    case 1: coefficient = 0x401; break;
    case 2:
    case 4: coefficient = 0x800; break;
    case 3: coefficient = 0x472; break;
    case 10: coefficient = 0x8E3; break;
    case 0x14: coefficient = 0x3DE; break;
    case 0x15: coefficient = 0x3BF; break;
    case 0x16: coefficient = 0x3A2; break;
    case 0x17: coefficient = 0x387; break;
    case 0x18: coefficient = 0x36D; break;
    case 0x1E: coefficient = 0x7BD; break;
    case 0x1F: coefficient = 0x780; break;
    case 0x20: coefficient = 0x745; break;
    case 0x21: coefficient = 0x70F; break;
    case 0x22: coefficient = 0x6DB; break;
    default: coefficient = 0x400; break;
    }
    const uint32_t scaler = word(param_1.a + 136) + 36;
    put(scaler, (word(scaler) & 0xFFFF0000u) | coefficient);
    return check.returns(dword(0));
}

dword kmyEnableHolizontalScaler(gptr<undefined> param_1, int param_2) {
    diff::Check check("kmyEnableHolizontalScaler", 0x8C039300u, param_1, param_2);
    if (!kmyiCheckNativeHandle(param_1))
        return check.returns(dword(0xEEEE));
    const uint32_t scaler = word(param_1.a + 136) + 36;
    put(scaler, param_2 ? word(scaler) | 0x10000 : word(scaler) & 0xFFFEFFFFu);
    return check.returns(dword(0));
}

// The renderer's strip buffer: on with bit 0 of `param_1`, `param_2` lines high, into the display's
// word at 0x8C10D264.
undefined4 kmySetStripBuffer(uint param_1, int param_2) {
    diff::Check check("kmySetStripBuffer", 0x8C03DB60u, param_1, param_2);
    const int32_t blocks = (param_2 < 0 ? param_2 + 31 : param_2) >> 5;
    put(0x8C10D264u, (word(0x8C10D264u) & 0xFFA0FFFFu) | (static_cast<uint32_t>(blocks) & 0x1F) << 16 |
                         (param_1 & 1) << 22);
    put(0x8C10D2FCu, param_1);
    put(0x8C10D300u, static_cast<uint32_t>(param_2));
    return check.returns(0u);
}

// A free one of the 8 render packets of 44 bytes from 0x254 of the work area `param_1`, by its mask
// at 0x248, into `param_2`.
dword kmyGetRenderPacket(int param_1, gptr<int> param_2) {
    diff::Check check("kmyGetRenderPacket", 0x8C03CC42u, param_1, param_2);
    if (!param_1)
        return check.returns(dword(0xEEEE));
    const uint32_t w = static_cast<uint32_t>(param_1);
    for (uint32_t i = 0; i < 8; ++i)
        if (!((1u << i) & word(w + 0x248))) {
            put(w + 0x248, word(w + 0x248) | 1u << i);
            put(w + 0x254 + 44 * i, i);
            put(w + 0x24C, word(w + 0x24C) + 1);
            put(param_2.a, w + 0x254 + 44 * i);
            break;
        }
    return check.returns(dword(0));
}

// The callback of the DMA error `param_1`, 0 or 1.
dword kmySetDMAErrorCallback(int param_1, undefined4 param_2) {
    diff::Check check("kmySetDMAErrorCallback", 0x8C03D740u, param_1, param_2);
    if (param_1 == 0)
        put(0x8C122C50u, param_2);
    else if (param_1 == 1)
        put(0x8C122C54u, param_2);
    else
        return check.returns(dword(0xEEEE));
    return check.returns(dword(0));
}

// The display off for a moment, with the border shown over the screen when `param_1` is 1.
undefined4 kmyBlankScreen(int param_1) {
    diff::Check check("kmyBlankScreen", 0x8C037840u, param_1);
    kmiWriteRegister(0x44, word(0x8C10D264u) & 0xFFFFFFFEu);
    kmiWriteRegister(0xE8, param_1 == 1 ? word(0x8C10D2A0u) | 8 : word(0x8C10D2A0u));
    kmiWriteRegister(0x44, word(0x8C10D264u));
    return check.returns(0u);
}

undefined4 kmySetPaletteMode(undefined4 param_1) {
    diff::Check check("kmySetPaletteMode", 0x8C03C460u, param_1);
    kmiWriteRegister(0x108, param_1);
    return check.returns(0u);
}

undefined4 kmySetBorderColor(uint param_1) {
    diff::Check check("kmySetBorderColor", 0x8C03D660u, param_1);
    kmiWriteRegister(0x40, param_1 & 0xFFFFFF);
    return check.returns(0u);
}

// Whether `param_1` is Kamui's driver object.
uint FUN_8c03e180(dword param_1) {
    diff::Check check("FUN_8c03e180", 0x8C03E180u, param_1);
    return check.returns(param_1 == 0x8C10EA44u ? 1u : 0u);
}

// ---- The system configuration's set-up --------------------------------------------------------

// 0 when the configuration `param_1` fits the display: without the strip buffer (flag 2) on a
// progressive display, or with 2 frame buffers and without flag 0x20.
dword kmiCheckSysConfigSetting(int param_1) {
    diff::Check check("kmiCheckSysConfigSetting", 0x8C03464Cu, param_1);
    const uint32_t config = static_cast<uint32_t>(param_1);
    if ((!(word(config + 4) & 2) && !word(0x8C10B6B0u)) || (word(config + 12) == 2 && !(word(config + 4) & 0x20)))
        return check.returns(dword(0));
    return check.returns(dword(0xEEEE));
}

// The parts of the video memory (0x8C10BE80): the frame buffers' (0x8C10BE8C) for the
// configuration `param_1`'s number of frame buffers, rounded up to pairs, the texture memory
// (0x8C10BE84, at least 8 MB of 16) and the rest for the native buffers (0x8C10BE88).
undefined4 kmiCalculateWorkSize(int param_1) {
    diff::Check check("kmiCalculateWorkSize", 0x8C034360u, param_1);
    const uint32_t config = static_cast<uint32_t>(param_1);
    constexpr uint32_t total = 0x8C10BE80u, textures = 0x8C10BE84u, buffers = 0x8C10BE88u, frames = 0x8C10BE8Cu;
    put(textures, word(config + 20));
    if (word(total) == 0x1000000 && word(textures) < word(total) >> 1)
        put(textures, word(total) >> 1);
    const uint32_t size = static_cast<uint32_t>(kmyGetFrameBufferSize(static_cast<int>(word(0x8C10B6A0u))));
    put(frames, size * ((word(config + 12) + 1) & ~1u));
    if (word(textures) + word(frames) >= word(total))
        put(textures, word(total) - word(frames) - 0x40000);
    put(buffers, word(total) - word(frames) - word(textures));
    return check.returns(0u);
}

// The clip rectangle of the whole display.
undefined4 kmiSetDisplayClipSize() {
    diff::Check check("kmiSetDisplayClipSize", 0x8C0342F0u);
    put(0x8C10BE90u, 0);
    put(0x8C10BE94u, 0);
    kmyGetDisplayParametersInfo(7, 0, gptr<int>(0x8C10BE98u));
    kmyGetDisplayParametersInfo(8, 0, gptr<int>(0x8C10BE9Cu));
    if (word(0x8C10B69Cu))
        put(0x8C10BE9Cu, word(0x8C10BE9Cu) - 1);
    return check.returns(0u);
}

// Two frame buffers, and two more without the strip buffer and with more than 2 in the
// configuration `param_1`: their handles from 0x8C10BE68 and their surfaces' addresses and sizes.
undefined4 kmiAllocateFB(int param_1) {
    diff::Check check("kmiAllocateFB", 0x8C034420u, param_1);
    guest::Frame frame(0x20);
    const uint32_t config = static_cast<uint32_t>(param_1), first = frame.top - 0x20, second = frame.top - 0x1C;
    const uint32_t size = static_cast<uint32_t>(kmyGetFrameBufferSize(static_cast<int>(word(0x8C10B6A0u))));
    const auto pair = [&](uint32_t handles, uint32_t surface) {
        kmyAllocateFrameBuffer(size, gptr<int>(first), gptr<int>(second));
        kmyReleaseFrameBufferHandle(gptr<undefined>(word(handles)));
        kmyReleaseFrameBufferHandle(gptr<undefined>(word(handles + 4)));
        put(handles, kmyGetFrameBufferHandle(word(first)).a);
        put(handles + 4, kmyGetFrameBufferHandle(word(second)).a);
        const uint32_t a = word(word(config + 8) + surface);
        put(a + 28, word(first));
        put(word(word(config + 8) + surface) + 20, size);
        if (word(config + 12) > surface / 4 + 1) {
            put(word(word(config + 8) + surface + 4) + 28, word(second));
            put(word(word(config + 8) + surface + 4) + 20, size);
        }
    };
    pair(0x8C10BE68u, 0);
    if (!(word(config + 4) & 2) && word(config + 12) > 2)
        pair(0x8C10BE70u, 8);
    return check.returns(0u);
}

// The vertex buffer's state `param_1`'s descriptor: its lists' pointers, the pass state and the
// passes, and Kamui's scene state at 0.
undefined4 kmiInitVertexBufferDesc(int param_1) {
    diff::Check check("kmiInitVertexBufferDesc", 0x8C034520u, param_1);
    const uint32_t config = static_cast<uint32_t>(param_1), desc = word(config + 36);
    for (const uint32_t a : {0x8C10B6C8u, 0x8C10B6C4u, 0x8C10B6C0u, 0x8C10B6CCu, 0x8C10B6BCu, 0x8C10B6D4u}) put(a, 0);
    put(desc, 0x8C10B738u);
    put(desc + 16, 0x8C10B738u);
    put(desc + 4, 0x8C10B6F8u & ~31u);
    put(desc + 8, 0x8C10B6BCu);
    put(desc + 12, 0x8C10B7D8u);
    put(desc + 24, config + 56);
    return check.returns(0u);
}

// The frame buffers' surface descriptors of the configuration `param_1`: 2 with the strip buffer,
// else its number. Each gets the display's colour mode, its pixel size from the table at
// 0x8C0CA730 and the display's size.
undefined4 kmiSetFBSurfaceDesc(int param_1) {
    diff::Check check("kmiSetFBSurfaceDesc", 0x8C03456Cu, param_1);
    guest::Frame frame(0x3C);
    const uint32_t config = static_cast<uint32_t>(param_1), sizes = frame.top - 0x38;
    for (uint32_t i = 0; i < 24; i += 4) put(sizes + i, word(0x8C0CA730u + i));
    const uint32_t count = word(config + 4) & 2 ? 2 : word(config + 12);
    put(frame.top - 0x3C, 0);
    uint32_t surfaces = word(config + 8);
    for (uint32_t i = 0; i < count; ++i, surfaces += 4) {
        const uint32_t s = word(surfaces);
        put(s, 1);
        put(s + 8, word(0x8C10B6A0u));
        put(s + 4, word(sizes + 4 * word(0x8C10B6A0u)));
        put(s + 24, 0);
        if (word(0x8C10B6B4u))
            put(s + 24, word(s + 24) | 1);
        kmyGetDisplayParametersInfo(7, 0, gptr<int>(s + 12));
        kmyGetDisplayParametersInfo(8, 0, gptr<int>(s + 16));
        put(frame.top - 0x3C, i + 1);
    }
    return check.returns(0u);
}

KMSTATUS kmGetFreeTextureMem(PKMUINT32 pSizeOfTexture, PKMUINT32 pMaxBlockSizeOfTexture) {
    diff::Check check("kmGetFreeTextureMem", 0x8C0336A0u, pSizeOfTexture, pMaxBlockSizeOfTexture);
    return check.returns(KMSTATUS(kmyGetFreeSize(gptr<int>(pSizeOfTexture.a), gptr<uint>(pMaxBlockSizeOfTexture.a))));
}

// The vertical blanks a frame waits for (0x8C10BE7C) and the render's time-out (0x8C10BEB4): 30, or
// for a count below 1 its negative plus 1.
KMSTATUS kmSetWaitVsyncCount(KMINT32 nCount) {
    diff::Check check("kmSetWaitVsyncCount", 0x8C031140u, nCount);
    put(0x8C10BEB4u, 30);
    if (nCount < 1) {
        nCount = -nCount;
        put(0x8C10BEB4u, static_cast<uint32_t>(nCount + 1));
    }
    put(0x8C10BE7Cu, static_cast<uint32_t>(nCount - 1));
    return check.returns(KMSTATUS_SUCCESS);
}

// The area of `param_2` bytes at the address in `param_3`, in the bank of that address.
undefined4 kmyAllocateAreaWithAddress(ushort param_1, uint param_2, gptr<uint> param_3) {
    diff::Check check("kmyAllocateAreaWithAddress", 0x8C037420u, param_1, param_2, param_3);
    return check.returns(
        kmytex_ABSalloc(param_1, param_2, param_3, static_cast<short>(word(0x8C0CF288u) <= word(param_3.a))));
}

// A frame buffer of `param_1` bytes, rounded up to 32, as two fields in the 64-bit path: their
// addresses in the 32-bit path into `param_2` and `param_3`.
int kmyAllocateFrameBuffer(uint param_1, gptr<int> param_2, gptr<int> param_3) {
    diff::Check check("kmyAllocateFrameBuffer", 0x8C0374C0u, param_1, param_2, param_3);
    guest::Frame frame(0x1C);
    const uint32_t size = param_1 & 0x1F ? (param_1 + 32) & ~31u : param_1;
    const uint32_t area = frame.top - 0x1C;
    put(frame.top - 0x18, param_3.a);
    put(frame.top - 0x14, param_2.a);
    put(frame.top - 0x10, size);
    const int result = static_cast<int>(kmyAllocateArea(2, size << 1, gptr<uint>(area)));
    if (!result) {
        put(param_2.a, static_cast<uint32_t>(kmyConvertAddress64to32(word(area))));
        put(param_3.a, static_cast<uint32_t>(kmyConvertAddress64to32(word(area) + 4)));
    }
    return check.returns(result);
}

// A native command buffer of `param_1` bytes, rounded up to 32, at the address in `param_2`, as two
// halves: their addresses in the 32-bit path into `param_2` and `param_3`.
int kmyAllocateNativeCommandBuffer(uint param_1, gptr<uint> param_2, gptr<int> param_3) {
    diff::Check check("kmyAllocateNativeCommandBuffer", 0x8C037540u, param_1, param_2, param_3);
    const uint32_t size = param_1 & 0x1F ? (param_1 + 32) & ~31u : param_1;
    put(param_2.a, 0);
    const int result = static_cast<int>(kmyAllocateAreaWithAddress(2, size << 1, param_2));
    if (!result) {
        const uint32_t area = word(param_2.a);
        put(param_2.a, static_cast<uint32_t>(kmyConvertAddress64to32(area)));
        put(param_3.a, static_cast<uint32_t>(kmyConvertAddress64to32(area + 4)));
    }
    return check.returns(result);
}

// A free one of the 4 frame buffer handles of 52 bytes from 0x8C10D30C for the address `param_1`,
// with the display's colour mode, its line's bytes from the table at 0x8C0CA93C and its size.
gptr<undefined4> kmyGetFrameBufferHandle(undefined4 param_1) {
    diff::Check check("kmyGetFrameBufferHandle", 0x8C039636u, param_1);
    guest::Frame frame(0x1C);
    const uint32_t bytes = frame.top - 0x1C;
    for (uint32_t i = 0; i < 24; i += 4) put(bytes + i, word(0x8C0CA93Cu + i));
    for (uint32_t h = 0x8C10D30Cu; h < 0x8C10D3DCu; h += 52) {
        if (word(h + 40))
            continue;
        put(h + 40, 1);
        put(h + 28, param_1);
        put(h + 36, param_1);
        put(h, 0);
        put(h + 4, word(bytes + 4 * word(0x8C10D248u)) << 3);
        put(h + 8, word(0x8C10D248u));
        put(h + 12, word(0x8C10D2B4u));
        put(h + 16, word(0x8C10D2B8u));
        put(h + 44, word(0x8C10D2B4u) * word(h + 4) >> 6);
        return check.returns(gptr<undefined4>(h));
    }
    return check.returns(gptr<undefined4>(0));
}

undefined4 kmyReleaseFrameBufferHandle(gptr<undefined> param_1) {
    diff::Check check("kmyReleaseFrameBufferHandle", 0x8C039780u, param_1);
    if (!param_1.a)
        return check.returns(0u);
    bool known = false;
    for (uint32_t h = 0x8C10D30Cu; h < 0x8C10D3DCu; h += 52)
        if (h == param_1.a)
            known = true;
    if (known && word(param_1.a + 40))
        put(param_1.a + 40, 0);
    return check.returns(0u);
}

// The bytes of a frame buffer in the colour mode `param_1`: its pixel size from the table at
// 0x8C0CA954 by the display's width and height.
int kmyGetFrameBufferSize(int param_1) {
    diff::Check check("kmyGetFrameBufferSize", 0x8C039888u, param_1);
    guest::Frame frame(0x28);
    const uint32_t sizes = frame.top - 0x1C, width = frame.top - 0x24, height = frame.top - 0x28;
    for (uint32_t i = 0; i < 24; i += 4) put(sizes + i, word(0x8C0CA954u + i));
    put(frame.top - 0x20, word(sizes + 4 * static_cast<uint32_t>(param_1)));
    kmyGetDisplayParametersInfo(7, 0, gptr<int>(width));
    kmyGetDisplayParametersInfo(8, 0, gptr<int>(height));
    return check.returns(static_cast<int>(word(frame.top - 0x20) * word(width) * word(height)));
}

// The native buffer `param_1`'s vertex buffer (`param_2`, `param_3` bytes) and renderer words,
// with the horizontal scaler on when `param_4` is set.
undefined4 kmyiInitializeNativeBuffer(int param_1, undefined4 param_2, undefined4 param_3, int param_4) {
    diff::Check check("kmyiInitializeNativeBuffer", 0x8C03E278u, param_1, param_2, param_3, param_4);
    const uint32_t b = static_cast<uint32_t>(param_1), regs = word(b + 136);
    put(regs + 40, word(0x8C10D298u));
    put(regs + 36, word(0x8C10D29Cu));
    put(b + 28, param_3);
    put(b + 24, param_2);
    put(word(b + 136) + 28, static_cast<uint32_t>(param_4));
    if (param_4)
        put(word(b + 136) + 36, word(word(b + 136) + 36) | 0x10000);
    return check.returns(0u);
}

// A free one of Kamui's 4 native buffers of 0x8C bytes from 0x8C10EA5C, with a render packet of
// the driver object `param_1`, into `param_2`. Returns 0xEEEE when there is none.
dword kmyRegisterNativeBuffer(dword param_1, gptr<undefined4> param_2, undefined4 param_3, undefined4 param_4,
                              int param_5) {
    diff::Check check("kmyRegisterNativeBuffer", 0x8C03E300u, param_1, param_2, param_3, param_4, param_5);
    put(param_2.a, 0);
    if (!FUN_8c03e180(param_1))
        return check.returns(dword(0xEEEE));
    bool registered = false;
    if (word(0x8C10EA4Cu) < word(0x8C10EA50u)) {
        for (uint32_t i = 0; i < 4; ++i) {
            if ((1u << i) & word(0x8C10EA48u))
                continue;
            put(0x8C10EA48u, word(0x8C10EA48u) | 1u << i);
            put(0x8C10EA4Cu, word(0x8C10EA4Cu) + 1);
            if (!kmyGetRenderPacket(static_cast<int>(0x8C10EA44u), gptr<int>(0x8C10EAE4u + 0x8C * i))) {
                kmyiInitializeNativeBuffer(static_cast<int>(0x8C10EA5Cu + 0x8C * i), param_3, param_4, param_5);
                put(param_2.a, 0x8C10EA5Cu + 0x8C * i);
                registered = true;
            }
            break;
        }
    }
    return check.returns(registered ? dword(0) : dword(0xEEEE));
}

namespace {

// The SH-4's float of an unsigned word: the signed conversion, plus 2^32 for a word from 2^31.
float unsigned_float(uint32_t v) {
    float f = static_cast<float>(static_cast<int32_t>(v));
    if (static_cast<int32_t>(v) < 0)
        f += 4294967296.0f;
    return f;
}

// The machine code's conversion of a float to an unsigned word.
uint32_t float_unsigned(float f) {
    if (!(2147483648.0f > f))
        f -= 4294967296.0f;
    return cpu::ftrc(f);
}

}  // namespace

// The two native buffers of the configuration `param_1` in the native command buffer: their region
// array's order, tiles and passes. A pass's list gets mode 0 without room, 3 when it is the direct
// list (flag 0x10), 2 or 3 for the opaque, translucent and punch-through lists by their room above
// 50, else 1. Each buffer gets its region array and background plane.
int kmiSetNativeBufferInfo(int param_1, float arg_fr4, float arg_fr5, float arg_fr6, float arg_fr7) {
    diff::Check check("kmiSetNativeBufferInfo", 0x8C033A00u, param_1, arg_fr4, arg_fr5, arg_fr6, arg_fr7);
    guest::Frame frame(0x70);
    const uint32_t config = static_cast<uint32_t>(param_1), sp = frame.top - 0x70;
    const uint32_t info = sp, width = sp + 28, height = sp + 32, modes = sp + 48, halves = sp + 68;
    for (uint32_t i = 0; i < 8; i += 4) put(0x8C10BE60u + i, 0);
    const int failed = kmyAllocateNativeCommandBuffer(word(0x8C10BE88u) >> 1, gptr<uint>(halves), gptr<int>(halves + 4));
    if (failed)
        return check.returns(failed);
    const float threshold = guest::load<float>(0x8C033AD0u);
    for (uint32_t n = 0; n < 2; ++n) {
        const uint32_t buffer = 0x8C10BE60u + 4 * n;
        kmyRegisterNativeBuffer(word(0x8C10BE5Cu), gptr<undefined4>(buffer), word(halves + 4 * n),
                                word(0x8C10BE88u) >> 1, static_cast<int>(word(0x8C10B69Cu)));
        kmySetRegionArrayCallback(gptr<undefined>(word(buffer)),
                                  static_cast<int>(word(config + 4) & 2 ? 0x8C03E600u : 0x8C03E680u));
        kmyGetDisplayRegionSize(word(0x8C10B694u), gptr<undefined4>(width), gptr<undefined4>(height),
                                static_cast<int>(word(0x8C10B69Cu)));
        kmySetPassDepth(gptr<undefined>(word(buffer)), static_cast<int>(word(width)), static_cast<int>(word(height)),
                        word(config + 52));
        for (uint32_t pass = 0; pass < word(config + 52); ++pass) {
            const uint32_t p = config + 56 + 56 * pass;
            for (uint32_t i = 0; i < 5; ++i) {
                const float room = guest::load<float>(p + 16 + 4 * i);
                uint32_t mode;
                if (room == 0.0f)
                    mode = (word(config + 4) & 0x10) && word(p + 12) == i ? 3 : 0;
                else if (i == 0 || i == 4 || i == 2)
                    mode = room > threshold ? 3 : 2;
                else
                    mode = 1;
                put(modes + 4 * i, mode);
            }
            kmyGeneratePassInfo(gptr<uint>(info), word(modes), word(modes + 4), word(modes + 8), word(modes + 12),
                                word(modes + 16));
            if (pass == 0)
                kmyEnablePassInfoZClear(gptr<uint>(info));
            else
                kmyDisablePassInfoZClear(gptr<uint>(info));
            if (word(p) & 1)
                kmyEnablePassInfoPreSort(gptr<uint>(info));
            else
                kmyDisablePassInfoPreSort(gptr<uint>(info));
            if (word(p) & 0x80) {
                if (word(p) & 0x10) {
                    if (word(modes + 12))
                        kmyUseAnotherList(gptr<uint>(info), 1);
                }
                else if ((word(p) & 0x20) && word(modes + 4)) {
                    kmyUseAnotherList(gptr<uint>(info), 3);
                }
                if ((word(p) & 0x40) && word(modes + 16))
                    kmyUseAnotherList(gptr<uint>(info), 4);
            }
            kmySetPassInfo(gptr<undefined>(word(buffer)), pass, word(info));
        }
        kmyCreateRegionArray(gptr<undefined>(word(buffer)), arg_fr4, arg_fr5, arg_fr6, arg_fr7);
        kmyInitializeBGPlane(gptr<undefined>(word(buffer)));
    }
    if (word(0x8C10B6B4u))
        for (uint32_t i = 0; i < 8; i += 4) kmySetVirticalFilterMode(gptr<undefined>(word(0x8C10BE60u + i)), 0);
    return check.returns(0);
}

// The vertex buffers of the configuration `param_1`: the banks (one with flag 0x10, else the
// configuration's count, each its share of the buffer) and, for each pass, the lists' starts and
// rooms by their percentages. Without flag 0x40 the passes share each list's room, by the
// passes' mean percentage, else each pass has its own part of the buffer. The direct list (flag
// 0x10) of a pass goes to the store queues instead.
undefined4 kmiSetVertexBufferBase(int param_1) {
    diff::Check check("kmiSetVertexBufferBase", 0x8C033D60u, param_1);
    guest::Frame frame(0x5C);
    const uint32_t config = static_cast<uint32_t>(param_1), desc = word(config + 36);
    const bool direct = word(config + 4) & 0x10;
    constexpr uint32_t banks = 0x8C10B6B8u, starts = 0x8C10B7F8u, base = 0x8C10BCF8u, rooms = 0x8C10BDB8u;
    put(0x8C10B6D0u, word(config + 52));
    uint32_t at = (sySqStart(word(config + 44)) + 31) & ~31u;
    uint32_t size = word(config + 48);
    if (direct) {
        put(desc + 20, 1);
        put(banks, 1);
    }
    else {
        put(desc + 20, word(config + 40));
        put(banks, word(config + 40));
        size = (size / word(config + 40)) & ~31u;
    }
    for (uint32_t bank = 0; bank < word(banks); ++bank) {
        put(0x8C10B7D8u + 4 * bank, starts + 160 * bank);
        for (uint32_t pass = 0; pass < word(config + 52); ++pass)
            for (uint32_t i = 0; i < 20; i += 4) put(starts + 160 * bank + 20 * pass + i, 0);
    }
    const float hundred = guest::load<float>(0x8C033F0Cu);
    float ratios[5];
    const auto is_direct = [&](uint32_t pass, uint32_t i) { return direct && word(config + 68 + 56 * pass) == i; };
    if (!(word(config + 4) & 0x40)) {
        put(0x8C10BE58u, 0);
        for (float& r : ratios) r = 0.0f;
        for (uint32_t pass = 0; pass < word(config + 52); ++pass) {
            put(0x8C10BD98u + 4 * pass, word(config + 68 + 56 * pass));
            for (uint32_t i = 0; i < 5; ++i)
                if (!is_direct(pass, i))
                    ratios[i] = ratios[i] + guest::load<float>(config + 72 + 56 * pass + 4 * i);
        }
        for (float& r : ratios) r = r / unsigned_float(word(config + 52));
        for (uint32_t bank = 0; bank < word(banks); ++bank)
            for (uint32_t i = 0; i < 5; ++i) {
                const float count = unsigned_float(size);
                if (is_direct(0, i)) {
                    put(starts + 160 * bank + 4 * i, sySqStart(0x10000000u));
                    put(starts + 160 * bank + 20 + 4 * i, at);
                }
                else {
                    put(starts + 160 * bank + 4 * i, at);
                }
                put(base + 20 * bank + 4 * i, at);
                const uint32_t room = float_unsigned(ratios[i] * count / hundred) & ~31u;
                put(rooms + 4 * i, room);
                at += room;
            }
        for (uint32_t i = 0; i < 20; i += 4) put(0x8C10B738u + i, word(starts + i));
        return check.returns(0u);
    }
    put(0x8C10BE58u, 1);
    for (uint32_t pass = 0; pass < word(config + 52); ++pass) {
        const uint32_t p = config + 56 + 56 * pass;
        put(0x8C10BD98u + 4 * pass, word(p + 12));
        for (float& r : ratios) r = 0.0f;
        for (uint32_t i = 0; i < 5; ++i)
            if (!is_direct(pass, i))
                ratios[i] = guest::load<float>(p + 16 + 4 * i);
        uint32_t next = sySqStart(word(p + 4));
        uint32_t part = word(p + 8);
        if (!direct)
            part = part / word(config + 40);
        part &= ~31u;
        for (uint32_t i = 0; i < 5; ++i)
            put(rooms + 20 * pass + 4 * i, float_unsigned(ratios[i] * unsigned_float(part) / hundred) & ~31u);
        for (uint32_t bank = 0; bank < word(banks); ++bank)
            for (uint32_t i = 0; i < 5; ++i) {
                const uint32_t to = starts + 160 * bank + 20 * pass + 4 * i;
                if (is_direct(pass, i)) {
                    put(to, sySqStart(0x10000000u));
                }
                else {
                    put(to, next);
                    next += word(rooms + 20 * pass + 4 * i);
                }
            }
        for (uint32_t i = 0; i < 20; i += 4) put(0x8C10B738u + 20 * pass + i, word(starts + 20 * pass + i));
    }
    return check.returns(0u);
}

// Kamui's system configuration `pSystemConfigStruct`: the texture memory's allocator, the copy
// mode, the strip buffer, the flow control, video memory's parts, the native buffers, the frame
// buffers, the vertex buffers and the display's clip. With flag 1 the frame buffers are cleared.
KMSTATUS kmSetSystemConfiguration(PKMSYSTEMCONFIGSTRUCT pSystemConfigStruct) {
    diff::Check check("kmSetSystemConfiguration", 0x8C030F20u, pSystemConfigStruct);
    const uint32_t config = pSystemConfigStruct.a;
    kmiCheckSysConfigSetting(static_cast<int>(config));
    put(0x8C10BFE8u, word(config + 4) & 0x100 ? 1 : 0);
    kmyInitAllocator(word(0x8C10BE80u), word(config + 24), word(config + 32), word(config + 28),
                     word(config + 32) + word(config + 24) * 24);
    kmyEnableASyncCopy(word(config + 4) & 0x100000 ? 1 : 0);
    if (word(config + 4) & 2) {
        put(0x8C10B6B4u, 1);
        kmySetStripBuffer(1, static_cast<int>(word(config + 16)));
    }
    else {
        put(0x8C10B6B4u, 0);
        kmySetStripBuffer(0, 0);
    }
    kmiFLWCTRLInitFLWControl();
    kmiCalculateWorkSize(static_cast<int>(config));
    kmiSetNativeBufferInfo(static_cast<int>(config), 0, 0, 0, 0);
    kmiAllocateFB(static_cast<int>(config));
    kmiSetVertexBufferBase(static_cast<int>(config));
    kmiInitVertexBufferDesc(static_cast<int>(config));
    kmiSetFBSurfaceDesc(static_cast<int>(config));
    kmiSetDisplayClipSize();
    for (uint32_t i = 0; i < 8; i += 4) put(0x8C10D1B0u + i, word(0x8C10BE60u + i));
    for (uint32_t i = 0; i < word(config + 12); ++i) {
        put(0x8C10D1B8u + 4 * i, word(0x8C10BE68u + 4 * i));
        if (word(config + 4) & 1) {
            const uint32_t surface = word(word(config + 8) + 4 * i);
            kmyZeroMemory(gptr<dword>(word(surface + 28) + 0xA5000000u), word(surface + 20));
        }
    }
    put(0x8C10BEB4u, 30);
    put(0x8C10BE7Cu, 0);
    put(kVertexBuffers.count, word(config + 40));
    put(kNativeBuffers.count, 2);
    put(kFrameBuffers.count, word(config + 12));
    put(kPasses, word(0x8C10B6D0u));
    for (uint32_t i = 0; i < 12; i += 4) put(0x8C10BEB8u + i, 0);
    for (uint32_t i = 0; i < 160; i += 4) put(0x8C10BEDCu + i, 0);
    if (word(0x8C10B6B4u) || word(0x8C10B6B0u))
        kmySetDisplayFrameBuffer(static_cast<int>(word(0x8C10D1B8u)), static_cast<int>(word(0x8C10D1BCu)));
    put(0x8C10BFC4u, 1);
    return check.returns(KMSTATUS_SUCCESS);
}

// ---- DMA handles -----------------------------------------------------------------------------

namespace {

// Kamui's DMA queues at 0x8C122C40: the next handle id at 4, the nesting of the functions that mask
// the interrupts at 8 and the mask they found at 12, the two transfer slots at 24 and 28 (the
// second for handles with flag 8), each channel's current handle from 32. A channel's ring of 32
// handles of 64 bytes has its head index and count in halfwords from 44, 6 bytes a channel.
constexpr uint32_t kQueues = 0x8C122C40u;
struct Channel {
    uint32_t ring, head, count, current;
};
constexpr Channel kChannels[3] = {{kQueues + 0x40, 46, 48, 32}, {kQueues + 0x840, 52, 54, 36},
                                  {kQueues + 0x1040, 58, 60, 40}};
// The index halfword that kmyGetDMAHANDLE takes the next handle from, before the head.
constexpr uint32_t kTail[3] = {44, 50, 56};

int32_t half(uint32_t offset) { return guest::load<int16_t>(kQueues + offset); }
void set_half(uint32_t offset, int32_t v) { guest::store<int16_t>(kQueues + offset, static_cast<int16_t>(v)); }

void leave() {
    put(kQueues + 8, word(kQueues + 8) - 1);
    if (word(kQueues + 8) == 0)
        unmask(word(kQueues + 12));
}

}  // namespace

// A free handle of the channel `param_1` (0 to 2), cleared, marked in use, with the channel and the
// next id. The interrupts stay masked until the handle is requested. Returns 0 when the channel's
// ring is full.
gptr<dword> kmyGetDMAHANDLE(dword param_1) {
    diff::Check check("kmyGetDMAHANDLE", 0x8C03A580u, param_1);
    if (word(kQueues + 8) == 0)
        put(kQueues + 12, mask_interrupts());
    put(kQueues + 8, word(kQueues + 8) + 1);
    uint32_t handle = 0;
    if (param_1 <= 2) {
        const Channel& c = kChannels[param_1];
        if (half(c.count) < 32) {
            set_half(c.count, half(c.count) + 1);
            const int32_t at = half(kTail[param_1]);
            set_half(kTail[param_1], at + 1);
            handle = c.ring + (static_cast<uint32_t>(at) << 6);
            set_half(kTail[param_1], half(kTail[param_1]) & 31);
        }
    }
    if (!handle) {
        leave();
        return check.returns(gptr<dword>(0));
    }
    kmyZeroMemory(gptr<dword>(handle), 0x40);
    put(handle + 40, 1);
    put(handle + 32, param_1);
    put(kQueues + 4, word(kQueues + 4) + 1);
    put(handle, word(kQueues + 4) - 1);
    if (word(kQueues + 4) >= 0x80000000u)
        put(kQueues + 4, 1);
    return check.returns(gptr<dword>(handle));
}

// Starts the first waiting handle of channel 0, or else of channel 1, then of channel 2, where the
// channel has no current transfer and the handle's slot is free. Returns 0x102 while a caller still
// holds the interrupts masked.
undefined4 kmiStartupDMA() {
    diff::Check check("kmiStartupDMA", 0x8C03D060u);
    if (word(kQueues + 8) != 0)
        return check.returns(0x102u);
    const auto start = [](const Channel& c) {
        uint32_t handle = c.ring + (static_cast<uint32_t>(half(c.head)) << 6);
        const uint32_t slot = word(handle + 4) & 8 ? 28 : 24;
        if (word(kQueues + slot))
            handle = 0;
        else
            put(kQueues + slot, handle);
        if (handle) {
            put(kQueues + c.current, handle);
            kmiLaunchQ(static_cast<int>(handle));
        }
    };
    const auto ready = [](const Channel& c) { return half(c.count) > 0 && word(kQueues + c.current) == 0; };
    if (ready(kChannels[0]))
        start(kChannels[0]);
    else if (ready(kChannels[1]))
        start(kChannels[1]);
    if (ready(kChannels[2]))
        start(kChannels[2]);
    return check.returns(0u);
}

// Queues the handle `param_1` and starts what can start, then unmasks the interrupts when no
// other caller holds them. Returns the handle's id.
undefined4 kmyRequestDMA(gptr<undefined4> param_1) {
    diff::Check check("kmyRequestDMA", 0x8C03D1A0u, param_1);
    const uint32_t id = word(param_1.a);
    put(param_1.a + 40, 2);
    put(kQueues + 8, word(kQueues + 8) - 1);
    kmiStartupDMA();
    if (word(kQueues + 8) == 0)
        unmask(word(kQueues + 12));
    return check.returns(id);
}

// 0x100 while a handle with the id `param_1` is in one of the rings, else 0x101.
undefined4 kmyQueryFinishDMA(int param_1) {
    diff::Check check("kmyQueryFinishDMA", 0x8C03C6A0u, param_1);
    if (param_1 < 0)
        return check.returns(0x101u);
    const uint32_t mask = mask_interrupts();
    for (const Channel& c : kChannels) {
        const uint32_t count = static_cast<uint32_t>(half(c.count));
        uint32_t at = static_cast<uint32_t>(half(c.head));
        for (uint32_t i = 0; i < count; ++i) {
            if (word(c.ring + (at << 6)) == static_cast<uint32_t>(param_1)) {
                unmask(mask);
                return check.returns(0x100u);
            }
            at = (at + 1) & 31;
        }
    }
    unmask(mask);
    return check.returns(0x101u);
}

// The end of a transfer, Kamui's event `*param_1`: 0x11 for the handle in the slot at 24, the
// others for the one at 28. Each event sets its bit in the handle's halfword at 30. When all bits
// of its halfword at 28 are in, a handle of type 4 with parts left (its count at 48) starts the next
// part, by the PowerVR's DMA or, with flag 8, by channel 2. Else the handle leaves its channel's
// ring and its slot, gets its time, and its callback runs. Then the next transfers start.
undefined4 kmiCallbackEndOf(gptr<int> param_1, undefined4 arg_r5, undefined4 arg_r6, undefined4 arg_r7, float arg_fr4,
                            float arg_fr5, float arg_fr6, float arg_fr7) {
    diff::Check check("kmiCallbackEndOf", 0x8C0378C0u, param_1, arg_r5, arg_r6, arg_r7, arg_fr4, arg_fr5, arg_fr6,
                      arg_fr7);
    const uint32_t event = word(param_1.a);
    const uint32_t h = word(kQueues + (event == 0x11 ? 24 : 28));
    if (!h)
        return check.returns(1u);
    const auto mark = [h](uint32_t bits) {
        guest::store<uint16_t>(h + 30, static_cast<uint16_t>(guest::load<uint16_t>(h + 30) | bits));
    };
    switch (event) {
    case 0x11:
        mark(0x4000);
        break;
    case 0x12:
        mark(0x8000);
        break;
    case 0x13:
        mark(0x100);
        break;
    case 0x15:
    case 0x16:
    case 0x17:
    case 0x18:
    case 0x19:
        mark(1u << (0x19 - event));
        kmiGetITPCurrent(static_cast<int>(word(h + 48)));
        break;
    }
    const uint32_t wanted = guest::load<uint16_t>(h + 28);
    const uint32_t done = wanted & guest::load<uint16_t>(h + 30);
    if (done != wanted)
        return check.returns(1u);
    if (word(h + 8) == 4 && word(h + 48)) {
        put(h + 48, word(h + 48) - 1);
        put(h + 16, word(h + 16) + word(h + 52));
        put(h + 20, word(h + 20) + word(h + 56));
        guest::store<uint16_t>(h + 30, 0);
        if (word(h + 4) & 8) {
            word(0xFFA0002Cu);
            put(0xFFA0002Cu, 0);
            put(0xFFA00020u, word(h + 16));
            put(0xFFA00028u, word(h + 12) >> 5);
            put(0xFFA0002Cu, 0x12C1);
            put(0xFFA00040u, 0x8201);
            put(0xA05F6800u, word(h + 20));
            put(0xA05F6804u, word(h + 12));
            put(0xA05F6808u, 1);
        }
        else {
            put(0xA05F7C80u, 0x6702007Fu);
            put(0xA05F7C00u, word(h + 20));
            put(0xA05F7C04u, word(h + 16));
            put(0xA05F7C08u, word(h + 12));
            put(0xA05F7C0Cu, word(h + 60));
            put(0xA05F7C10u, 0);
            put(0xA05F7C14u, 1);
            put(0xA05F7C18u, 1);
        }
        return check.returns(1u);
    }
    const uint32_t channel = word(h + 32);
    if (channel <= 2) {
        const Channel& c = kChannels[channel];
        set_half(c.count, half(c.count) - 1);
        set_half(c.head, half(c.head) + 1);
        set_half(c.head, half(c.head) & 31);
        put(kQueues + c.current, 0);
    }
    put(kQueues + (word(h + 4) & 8 ? 28 : 24), 0);
    const uint32_t now = word(kTimer);
    put(h + 36, ((word(h + 36) - now) << 7) / 100);
    put(h + 40, 0);
    if (word(h + 24))
        gptr<code>(word(h + 24))(h, done, 8u, arg_r7, arg_fr4, arg_fr5, arg_fr6, arg_fr7);
    kmiStartupDMA();
    return check.returns(1u);
}

// Kamui's event `param_1`: for the events 16 to 31 its status bit is cleared and its handler from
// the table at 0x8C10D6FC runs at the event's interrupt level. For event 18, the end of the
// channel-2 DMA, the DMA's enable and end bits are cleared first. A larger event hangs.
undefined4 FUN_8c03b3e0(uint param_1) {
    diff::Check check("FUN_8c03b3e0", 0x8C03B3E0u, param_1);
    const uint32_t saved = word(kInterruptLevel);
    put(kInterruptLevel, 15);
    if (param_1 > 15) {
        if (param_1 >= 32)
            for (;;) {
            }
        const uint32_t clear = 0x8C0CF1F0u + 8 * (param_1 - 16);
        put(kInterruptLevel, param_1);
        if (param_1 == 18)
            put(0xFFA0002Cu, word(0xFFA0002Cu) & ~3u);
        put(word(clear), word(clear + 4));
        const uint32_t handler = 0x8C10D6FCu + 16 * param_1;
        put(0x8C10D91Cu, param_1);
        CallArgs args;
        args.r[0] = 0x8C10D91Cu;
        args.r[1] = word(handler + 8);
        args.nr = 2;
        guest::call(word(handler), args);
    }
    put(kInterruptLevel, saved);
    return check.returns(0u);
}

// A DMA request for a texture: `param_4` bytes from `param_3` to the texture memory at `param_2`,
// through the PowerVR's 64-bit path at 0x11000000.
void kmySetTextureDMA(int param_1, uint param_2, undefined4 param_3, undefined4 param_4) {
    diff::Check check("kmySetTextureDMA", 0x8C030EE0u, param_1, param_2, param_3, param_4);
    const uint32_t request = static_cast<uint32_t>(param_1);
    guest::store<uint32_t>(request + 16, param_3);
    guest::store<uint32_t>(request + 20, param_2 | 0x11000000u);
    guest::store<uint32_t>(request + 12, param_4);
    guest::store<uint32_t>(request + 8, 0);
    guest::store<uint32_t>(request + 4, 8);
    guest::store<uint16_t>(request + 28, 0x8000);
    guest::store<uint16_t>(request + 30, 0);
}

// ---- The device, its driver modules and its interrupts ---------------------------------------

namespace {

// Kamui's driver object at 0x8C10D3DC: its list of modules, and at 20 the function that registers
// a module.
constexpr uint32_t kDriver = 0x8C10D3DCu;

void register_module(uint32_t module, uint32_t data) {
    gptr<code>(word(kDriver + 20))(kDriver, module, data);
}

// Polls the PowerVR's status at 0x10C until `done` holds or a timer span of 0x3D08 has passed.
// The first poll that does not hold ends the first wait, and then the second waits for `done`.
void poll_status(bool (*done)(uint32_t)) {
    const uint32_t start = word(kTimer);
    bool first = true;
    uint32_t now = word(kTimer);
    while (start - now <= 0x3D08) {
        if (!done(kmiReadRegister(0x10C))) {
            first = false;
            break;
        }
        now = word(kTimer);
    }
    if (first)
        return;
    now = word(kTimer);
    while (start - now < 0x3D09 && !done(kmiReadRegister(0x10C))) now = word(kTimer);
}

}  // namespace

// Kamui's driver object `param_1` with the module list `param_2`.
undefined4 kmyiInitializeDriverObject(gptr<int> param_1, int param_2) {
    diff::Check check("kmyiInitializeDriverObject", 0x8C03C41Eu, param_1, param_2);
    const uint32_t d = param_1.a, list = static_cast<uint32_t>(param_2);
    put(d, list);
    put(d + 8, 0x8C03C380u);
    put(d + 16, 0x8C03C394u);
    put(d + 4, 0);
    put(d + 20, 0x8C03C3DCu);
    put(list + 4, 0x8C0CA9D4u);
    put(list + 8, 0x8C0CAA34u);
    return check.returns(0u);
}

// Whether the module `param_2` with the data `param_3` is in the driver object `param_1`'s list.
undefined4 FUN_8c03c3b8(int param_1, int param_2, int param_3) {
    diff::Check check("FUN_8c03c3b8", 0x8C03C3B8u, param_1, param_2, param_3);
    uint32_t found = 0;
    for (uint32_t m = word(static_cast<uint32_t>(param_1) + 4); m; m = word(m + 4))
        if (m == static_cast<uint32_t>(param_2) && word(m + 8) == static_cast<uint32_t>(param_3))
            found = 1;
    return check.returns(found);
}

// Registers the module `param_2` with the data `param_3` in the driver object `param_1`, at the
// list's head, and runs its start function at 12. Returns 0xEEEE when it is registered already.
dword FUN_8c03c3dc(int param_1, gptr<int> param_2, int param_3, undefined4 arg_r7, float arg_fr4, float arg_fr5,
                   float arg_fr6, float arg_fr7) {
    diff::Check check("FUN_8c03c3dc", 0x8C03C3DCu, param_1, param_2, param_3, arg_r7, arg_fr4, arg_fr5, arg_fr6,
                      arg_fr7);
    if (FUN_8c03c3b8(param_1, static_cast<int>(param_2.a), param_3))
        return check.returns(dword(0xEEEE));
    const uint32_t d = static_cast<uint32_t>(param_1), m = param_2.a;
    put(m, d);
    const uint32_t next = word(d + 4);
    put(d + 4, m);
    put(m + 4, next);
    put(m + 8, static_cast<uint32_t>(param_3));
    if (word(m + 12))
        gptr<code>(word(m + 12))(param_2, param_2, param_3, arg_r7, arg_fr4, arg_fr5, arg_fr6, arg_fr7);
    return check.returns(dword(0));
}

// The display module's and the PowerVR module's registrations.
undefined4 kmyInitializeDriver() {
    diff::Check check("kmyInitializeDriver", 0x8C03AB24u);
    kmyiInitializeDriverObject(gptr<int>(kDriver), static_cast<int>(0x8C10D3F4u));
    put(0x8C10D238u, 0x8C038706u);
    register_module(0x8C10D22Cu, 0x8C10D244u);
    put(0x8C10E954u, 0x8C03AB12u);
    register_module(0x8C10E948u, 0x8C10E960u);
    return check.returns(0u);
}

undefined4 kmyiInitializePVR2Hardware(int param_1) {
    diff::Check check("kmyiInitializePVR2Hardware", 0x8C03AB12u, param_1);
    const uint32_t data = word(static_cast<uint32_t>(param_1) + 8);
    if (data) {
        put(data + 24, 0);
        put(data + 20, 0);
    }
    return check.returns(0u);
}

// The wait until the PowerVR's status at 0x10C shows no line in progress, or the time-out.
undefined4 FUN_8c03abe0() {
    diff::Check check("FUN_8c03abe0", 0x8C03ABE0u);
    poll_status([](uint32_t status) { return (status & 0x3FF) == 0; });
    return check.returns(0u);
}

// The wait for the vertical blank by the PowerVR's status at 0x10C, or the time-out.
undefined4 kmyWaitVBlank() {
    diff::Check check("kmyWaitVBlank", 0x8C03E560u);
    poll_status([](uint32_t status) { return (status & 0x2000) != 0; });
    return check.returns(0u);
}

// The PowerVR for the device `param_1`: its core type from the ID and revision registers (6 for
// the Dreamcast's, the only one that works), its fog and texture settings, its renderer's start
// values. Hangs for another core.
undefined4 kmyInitializeHardware(uint param_1) {
    diff::Check check("kmyInitializeHardware", 0x8C03AC80u, param_1);
    uint32_t spare = 0, timing = 0x0DD18957u;
    put(0x8C10E970u, param_1 & 0xFFFF0000u);
    const uint32_t id = kmiReadRegister(0);
    const uint32_t revision = kmiReadRegister(4);
    put(0x8C10E960u, 5);
    if (id == 0x17FD11DBu && revision != 1 && revision > 0x10) {
        spare = 0x800408;
        put(0x8C10E960u, 6);
        timing = param_1 & 0xFFFF0000u ? 0x15D1C955u : 0x15D1C951u;
    }
    put(0x8C10E96Cu, word(0xA05F689Cu));
    uint32_t mode = 0x101;
    uint32_t features;
    switch (word(0x8C10E96Cu)) {
    case 0: features = 0, mode = 0; break;
    case 1: features = 1, mode = 0; break;
    case 2: features = 3; break;
    case 4:
    case 5:
    case 6: features = 0x7F, mode = 0x10101; break;
    case 8: features = 0x7F; break;
    case 9: features = 0x6F; break;
    default: features = 0xEF; break;
    }
    put(0x8C10E964u, features);
    kmiWriteRegister(0xA8, timing);
    kmiWriteRegister(0xA0, 0x20);
    kmiWriteRegister(8, 0);
    kmiWriteRegister(0xE4, 3);
    kmiWriteRegister(0x48, 9);
    FUN_8c03abe0();
    kmiWriteRegister(0x44, kmiReadRegister(0x44) & 0x800000);
    kmiWriteRegister(0x5C, 0);
    kmiWriteRegister(0x68, 0x2800000);
    kmiWriteRegister(0x6C, 0x1E00000);
    if (word(0x8C10E960u) == 6) {
        kmiWriteRegister(0x110, 0x93F39);
        kmiWriteRegister(0x98, spare);
    }
    else {
        kmiWriteRegister(0x110, 0x92039);
    }
    kmiWriteRegister(0x4C, 0xA0);
    kmiWriteRegister(0x74, 0);
    kmiWriteRegister(0x78, 0x3F800000);
    kmiWriteRegister(0x84, 0);
    kmiWriteRegister(0x30, mode);
    kmiWriteRegister(0xB0, 0x7F7F7F);
    kmiWriteRegister(0xB4, 0x7F7F7F);
    kmiWriteRegister(0xC0, 0);
    kmiWriteRegister(0xBC, 0xFFFFFFFFu);
    kmiWriteRegister(0x80, 7);
    kmiWriteRegister(0x88, 0x3727C5A0);
    kmiWriteRegister(0x8C, 0x1203000);
    kmiWriteRegister(0x74, 1);
    if (word(0x8C10E960u) != 6)
        for (;;) {
        }
    kmiWriteRegister(0x7C, 0x27DF77);
    return check.returns(0u);
}

// The DMA queues' state from 0x8C122C40, cleared, with the next handle id 1, the end-of-transfer
// and error handlers for Kamui's DMA events and the channel-2 DMA's set-up, with the interrupts
// masked.
undefined4 kmyInitDMADevice() {
    diff::Check check("kmyInitDMADevice", 0x8C03B160u);
    const uint32_t mask = mask_interrupts();
    put(0x8C10D628u, kQueues);
    kmyZeroMemory(gptr<dword>(kQueues), 0x1840);
    put(kQueues, 0x1840);
    put(kQueues + 4, 1);
    for (const int event : {0x11, 0x12, 0x13, 0x19, 0x18, 0x17, 0x16, 0x15})
        kmyConnectInterrupt(event, 0x8C0378C0u, 0);
    kmyConnectInterrupt(0x1C, 0x8C037B68u, 0);
    kmyConnectInterrupt(0x1B, 0x8C037B68u, 0);
    put(0xA05F6884u, 0);
    put(0xA05F6888u, 1);
    unmask(mask);
    return check.returns(0u);
}

// The callback module `param_1`'s table: 16 entries of the default callback.
void kmyiInitializeCallback(int param_1) {
    diff::Check check("kmyiInitializeCallback", 0x8C03B348u, param_1);
    const uint32_t m = static_cast<uint32_t>(param_1);
    put(m + 20, 0x8C03B324u);
    const uint32_t table = word(m + 8);
    put(table, 0x10);
    put(table + 4, 0x8C03B320u);
    for (uint32_t i = 0; i < 0x80; i += 8) {
        put(table + 8 + i, word(table + 4));
        put(table + 12 + i, 0);
    }
}

void kmyInitializeCallback() {
    diff::Check check("kmyInitializeCallback", 0x8C03B378u);
    put(0x8C10D638u, 0x8C03B348u);
    register_module(0x8C10D62Cu, 0x8C10D644u);
}

// Kamui's callback `param_1`: the function `param_2` with the argument `param_3`, or none for 0.
undefined4 kmyRegisterCallback(int param_1, int param_2, undefined4 param_3) {
    diff::Check check("kmyRegisterCallback", 0x8C03CA80u, param_1, param_2, param_3);
    if (!param_2)
        return check.returns(kmyUnregisterCallback(param_1));
    const uint32_t entry = 0x8C10D64Cu + 8 * static_cast<uint32_t>(param_1);
    put(entry, static_cast<uint32_t>(param_2));
    put(entry + 4, param_3);
    return check.returns(0u);
}

dword kmyRegisterPALEXTCallback(int param_1) {
    diff::Check check("kmyRegisterPALEXTCallback", 0x8C03C098u, param_1);
    if (!param_1) {
        put(0x8C10D308u, 0x8C038700u);
        return check.returns(dword(0xEEEE));
    }
    put(0x8C10D308u, static_cast<uint32_t>(param_1));
    return check.returns(dword(0));
}

KMSTATUS kmSetEndOfVertexCallback(PKMCALLBACKFUNC pEndOfVertexCallback, PKMVOID pCallbackArguments) {
    diff::Check check("kmSetEndOfVertexCallback", 0x8C036420u, pEndOfVertexCallback, pCallbackArguments);
    return check.returns(
        KMSTATUS(kmyRegisterCallback(5, static_cast<int>(pEndOfVertexCallback.a), pCallbackArguments.a)));
}

KMSTATUS kmSetEORCallback(PKMCALLBACKFUNC pEORCallback, PVOID pCallbackArguments) {
    diff::Check check("kmSetEORCallback", 0x8C036460u, pEORCallback, pCallbackArguments);
    return check.returns(KMSTATUS(kmyRegisterCallback(0, static_cast<int>(pEORCallback.a), pCallbackArguments.a)));
}

KMSTATUS kmSetHSyncCallback(PKMCALLBACKFUNC pHSyncCallback, PVOID pCallbackArguments) {
    diff::Check check("kmSetHSyncCallback", 0x8C0365A0u, pHSyncCallback, pCallbackArguments);
    return check.returns(KMSTATUS(kmyRegisterCallback(2, static_cast<int>(pHSyncCallback.a), pCallbackArguments.a)));
}

// The interrupt module `param_1`'s table: 32 events of the empty handler, the current mask, and
// Kamui's event dispatch on the PowerVR's and the DMA's interrupts by Shinobi's chain, with the
// interrupts masked. Then the system bus's interrupt masks.
undefined4 kmyiInitializeInterrupt(int param_1) {
    diff::Check check("kmyiInitializeInterrupt", 0x8C03B4E0u, param_1);
    const uint32_t m = static_cast<uint32_t>(param_1);
    put(m + 20, 0x8C03B458u);
    const uint32_t t = word(m + 8);
    memset(gptr<undefined1>(t), 0, 0x238);
    put(t + 36, 0x8C03B454u);
    put(t, 0x20);
    for (int32_t i = 0; i < static_cast<int32_t>(word(t)); ++i) {
        const uint32_t e = t + 48 + 16 * static_cast<uint32_t>(i);
        put(e, 0x8C03B454u);
        put(e + 12, 0);
        put(e + 4, 0);
        put(e + 8, 0);
    }
    put(t + 8, (cpu::sr >> 4) & 15);
    cpu::set_sr((cpu::sr & 0xFFFFFF0Fu) | 0xF0);
    struct Chain {
        short vector;
        uint32_t event, slot;
    };
    static constexpr Chain kChains[14] = {{0xC10, 0x1A, 0x77}, {0xC20, 0x1C, 0x7F}, {0xC30, 0x1B, 0x7B},
                                          {0x950, 0x14, 0x5F}, {0x960, 0x13, 0x5B}, {0x970, 0x19, 0x73},
                                          {0x980, 0x18, 0x6F}, {0x990, 0x17, 0x6B}, {0x9A0, 0x16, 0x67},
                                          {0x9B0, 0x11, 0x53}, {0x9C0, 0x12, 0x57}, {0x9E0, 0x15, 0x63},
                                          {0x900, 0x1D, 0x83}, {0x920, 0x1E, 0x87}};
    for (const Chain& c : kChains) put(t + 4 * c.slot, syChainAddHandler(c.vector, 0x8C03B3E0u, 0x10, c.event));
    put(t + 0x230, 0xE);
    put(t + 0x234, 0);
    put(0xA05F6900u, 0x280FEC);
    put(0xA05F6908u, 0xE);
    unmask(word(t + 8));
    return check.returns(0u);
}

undefined4 FUN_8c03b454() {
    diff::Check check("FUN_8c03b454", 0x8C03B454u);
    return check.returns(1u);
}

void kmyInitializeInterrupt() {
    diff::Check check("kmyInitializeInterrupt", 0x8C03B6E8u);
    put(0x8C10D910u, 0x8C03B4E0u);
    register_module(0x8C10D904u, 0x8C10D6CCu);
}

// The handler `param_2` with the argument `param_3` for Kamui's event `param_1`, with the
// interrupts masked.
undefined4 kmyConnectInterrupt(int param_1, undefined4 param_2, undefined4 param_3) {
    diff::Check check("kmyConnectInterrupt", 0x8C037CE0u, param_1, param_2, param_3);
    const uint32_t mask = mask_interrupts();
    const uint32_t handler = 0x8C10D6FCu + 16 * static_cast<uint32_t>(param_1);
    put(handler, param_2);
    put(handler + 8, param_3);
    put(handler + 4, static_cast<uint32_t>(param_1));
    unmask(mask);
    return check.returns(0u);
}

// The interrupts on at the module's mask (0x8C10D6D4), unless an interrupt is running.
dword kmyEnableInterrupt() {
    diff::Check check("kmyEnableInterrupt", 0x8C039380u);
    if (word(kInterruptLevel))
        return check.returns(dword(0xEEEE));
    unmask(word(0x8C10D6D4u));
    return check.returns(dword(0));
}

// The frame buffer handles free.
void kmiInitFBControl() {
    diff::Check check("kmiInitFBControl", 0x8C039620u);
    for (uint32_t h = 0x8C10D30Cu; h < 0x8C10D3DCu; h += 52) put(h + 40, 0);
}

// The display module `param_1`'s functions, and the frame buffer handles free.
void kmiInitializeDisplayModule(int param_1) {
    diff::Check check("kmiInitializeDisplayModule", 0x8C038706u, param_1);
    const uint32_t data = word(static_cast<uint32_t>(param_1) + 8);
    put(data + 8, 0x8C038F80u);
    put(data + 184, 0);
    put(data + 196, 0x8C038700u);
    kmiInitFBControl();
}

// The tile accelerator module, whose data `param_1` points to.
void kmyInitializeTA(gptr<dword> param_1) {
    diff::Check check("kmyInitializeTA", 0x8C03E25Eu, param_1);
    put(param_1.a, 0x8C10EA44u);
    put(0x8C10EE08u, 0x8C03E220u);
    register_module(0x8C10EDFCu, 0x8C10EA44u);
}

// The tile accelerator module `param_1`'s start: 4 render packets of 0x8C bytes, numbered.
undefined4 FUN_8c03e220(int param_1) {
    diff::Check check("FUN_8c03e220", 0x8C03E220u, param_1);
    const uint32_t m = static_cast<uint32_t>(param_1);
    put(m + 20, 0x8C03E1BEu);
    const uint32_t d = word(m + 8);
    put(d, 0x3B8);
    put(d + 4, 0);
    put(d + 8, 0);
    put(d + 16, 0x8C03E18Au);
    put(d + 20, 0x8C03E1A4u);
    put(d + 12, 4);
    for (uint32_t i = 0; i < word(d + 12); ++i) put(d + 24 + 0x8C * i, i);
    return check.returns(0u);
}

gptr<char> sbGetVersionString() {
    diff::Check check("sbGetVersionString", 0x8C03EFA0u);
    return check.returns(gptr<char>(word(0x8C0CF2C8u)));
}

// The fog table cleared, in Kamui's copy and in the PowerVR.
undefined4 kmyInitializeFogTable() {
    diff::Check check("kmyInitializeFogTable", 0x8C039460u);
    for (uint32_t i = 128; i-- != 0;) {
        put(0x8C10D418u + 4 * i, 0);
        kmiWriteRegister(static_cast<int>(0x200 + 4 * i), 0);
    }
    return check.returns(0u);
}

undefined4 kmyInitializeFog() {
    diff::Check check("kmyInitializeFog", 0x8C039498u);
    memset(gptr<undefined1>(0x8C10D418u), 0, 0x210);
    kmyInitializeFogTable();
    return check.returns(0x78u);
}

// The device `nDevice`: Kamui's state, its driver modules, callbacks, hardware, fog, interrupts and
// DMA, and the tile accelerator.
KMSTATUS kmInitDevice(KMDEVICE nDevice) {
    diff::Check check("kmInitDevice", 0x8C0347EEu, nDevice);
    const uint32_t device = static_cast<uint32_t>(nDevice);
    put(0x8C10B688u, device & 0xFFFF0000u);
    put(0x8C10B68Cu, 0x8C0CA748u);
    put(0x8C10B690u, 0x8C0CA7A4u);
    put(0x8C10BE80u, word(0x8C10B688u) == 0x10000 ? 0x1000000u : 0x800000u);
    kmyInitializeDriver();
    kmyInitializeCallback();
    kmyRegisterCallback(8, static_cast<int>(0x8C0347DCu), 0x8C10BFE4u);
    kmyRegisterCallback(0xB, static_cast<int>(0x8C0347E0u), 0x8C10BFECu);
    kmyRegisterPALEXTCallback(static_cast<int>(0x8C0347E6u));
    kmyInitializeHardware(device);
    kmyInitializeFog();
    put(0x8C10BFD8u, 0xF);
    put(0x8C10BFD4u, 0x28);
    put(0x8C10BFD0u, 0);
    put(0x8C10BFCCu, 0);
    kmyInitializeInterrupt();
    kmyConnectInterrupt(0x1E, 0x8C0347C0u, 0);
    kmyConnectInterrupt(0x1A, 0x8C0347CEu, 0);
    kmyInitDMADevice();
    kmyEnableInterrupt();
    kmyInitializeTA(gptr<dword>(0x8C10BE5Cu));
    put(0x8C10BFC8u, 0);
    put(0x8C10BFF8u, 0);
    return check.returns(KMSTATUS_SUCCESS);
}

// ---- The device's unload, at the soft reset ---------------------------------------------------

// The callback module `param_1`'s table back to 16 entries of the default callback.
void kmyiUnloadCallback(int param_1) {
    diff::Check check("kmyiUnloadCallback", 0x8C03B324u, param_1);
    const uint32_t table = word(static_cast<uint32_t>(param_1) + 8);
    for (uint32_t i = 0; i < 0x80; i += 8) {
        put(table + 8 + i, word(table + 4));
        put(table + 12 + i, 0);
    }
}

// The interrupt module `param_1`'s handlers off Shinobi's chains: each event's handler is at 12 in
// its entry of 16 bytes from 48.
undefined4 kmyiUnloadInterrupt(int param_1) {
    diff::Check check("kmyiUnloadInterrupt", 0x8C03B458u, param_1);
    const uint32_t t = word(static_cast<uint32_t>(param_1) + 8);
    for (int32_t i = 0; i < static_cast<int32_t>(word(t)); ++i) {
        const uint32_t handler = word(t + 60 + 16 * static_cast<uint32_t>(i));
        if (handler)
            syChainDeleteHandler(SYCALLBACK(handler));
    }
    return check.returns(0u);
}

// The tile accelerator module `param_1`'s render packets back to their start.
void FUN_8c03e1be(int param_1) {
    diff::Check check("FUN_8c03e1be", 0x8C03E1BEu, param_1);
    const uint32_t d = word(static_cast<uint32_t>(param_1) + 8);
    put(d, 0x3B8);
    put(d + 4, 0);
    put(d + 8, 0);
    put(d + 16, 0x8C03E18Au);
    put(d + 20, 0x8C03E1A4u);
    put(d + 12, 4);
    for (uint32_t i = 0; i < word(d + 12); ++i) put(d + 24 + 0x8C * i, i);
}

// The driver object `param_1`'s unload: each module's unload function at 20, if it has one. The
// machine code passes the module alone.
undefined4 FUN_8c03c394(int param_1, undefined4 arg_r5, undefined4 arg_r6, undefined4 arg_r7, float arg_fr4,
                        float arg_fr5, float arg_fr6, float arg_fr7) {
    diff::Check check("FUN_8c03c394", 0x8C03C394u, param_1);
    (void)arg_r5, (void)arg_r6, (void)arg_r7, (void)arg_fr4, (void)arg_fr5, (void)arg_fr6, (void)arg_fr7;
    for (uint32_t m = word(static_cast<uint32_t>(param_1) + 4); m; m = word(m + 4))
        if (word(m + 20))
            gptr<code>(word(m + 20))(m);
    return check.returns(0u);
}

// The driver object's unload function at 16, if any, then the object cleared.
undefined4 kmyUnloadDevice() {
    diff::Check check("kmyUnloadDevice", 0x8C03AB5Eu);
    if (word(kDriver + 16))
        gptr<code>(word(kDriver + 16))(kDriver);
    memset(gptr<undefined1>(kDriver), 0, 24);
    return check.returns(0u);
}

// The tile accelerator reset by its soft reset register.
void kmyResetTA() {
    diff::Check check("kmyResetTA", 0x8C03D200u);
    kmiWriteRegister(8, 1);
    kmiWriteRegister(8, 0);
}

// The renderer reset once every render packet has ended.
KMSTATUS kmResetRenderer() {
    diff::Check check("kmResetRenderer", 0x8C034C00u);
    while (kmiQueryFinishAllPacket() != 0) {
    }
    kmyResetRenderer();
    kmyResetTA();
    return check.returns(KMSTATUS_SUCCESS);
}

// Kamui's device unloaded, unless a scene is at work (the low bits of 0x8C10B6D4): 0xEEEE then.
KMSTATUS kmUnloadDevice() {
    diff::Check check("kmUnloadDevice", 0x8C037020u);
    if (word(0x8C10B6D4u) & 3)
        return check.returns(KMSTATUS(0xEEEE));
    kmResetRenderer();
    return check.returns(KMSTATUS(kmyUnloadDevice()));
}

// The callback `param_1` back to the default callback, without an argument.
undefined4 kmyUnregisterCallback(int param_1) {
    diff::Check check("kmyUnregisterCallback", 0x8C03E440u, param_1);
    const uint32_t entry = 0x8C10D64Cu + 8 * static_cast<uint32_t>(param_1);
    put(entry, word(0x8C10D648u));
    put(entry + 4, 0);
    return check.returns(0u);
}

// ---- The display modes -------------------------------------------------------------------------

// The display's width (0x8C10D2B4) and height (0x8C10D2B8) for the display mode `param_1`.
void kmyiSetScreenSize(uint param_1) {
    diff::Check check("kmyiSetScreenSize", 0x8C0391D8u, param_1);
    const bool pal = (param_1 & 0x42) == 0x42;
    if (!(param_1 & 0x20))
        put(0x8C10D2B8u, pal ? 0x100 : 0xF0);
    else
        put(0x8C10D2B8u, pal ? 0x200 : 0x1E0);
    put(0x8C10D2B4u, param_1 & 0x10 ? 0x280 : 0x140);
}

// The display's timing words from 0x8C10D250 into the PowerVR, with the display off.
void FUN_8c03871c() {
    diff::Check check("FUN_8c03871c", 0x8C03871Cu);
    kmiWriteRegister(0x68, word(0x8C10D250u));
    kmiWriteRegister(0x6C, word(0x8C10D254u));
    kmiWriteRegister(0x5C, word(0x8C10D25Cu));
    kmiWriteRegister(0x4C, word(0x8C10D260u));
    kmiWriteRegister(0x118, word(0x8C10D298u));
    kmiWriteRegister(0xF4, word(0x8C10D29Cu));
    kmiWriteRegister(0x48, word(0x8C10D268u));
    kmiWriteRegister(0x44, word(0x8C10D264u) & 0xFFFFFFFEu);
    kmiWriteRegister(0xE8, word(0x8C10D2A0u) | 8);
}

// The display's timing words at 0x8C10D250 from the 29 words that the machine code passes on the
// stack: the parameters are its words 0 to 8 and 10 to 28, the last as a byte.
void FUN_8c0387c0(uint param_1, uint param_2, uint param_3, uint param_4, uint param_5, uint param_6, uint param_7,
                  uint param_8, uint param_9, uint param_10, uint param_11, uint param_12, uint param_13, uint param_14,
                  uint param_15, uint param_16, uint param_17, uint param_18, uint param_19, uint param_20,
                  uint param_21, uint param_22, uint param_23, uint param_24, uint param_25, uint param_26,
                  uint param_27, byte param_28) {
    diff::Check check("FUN_8c0387c0", 0x8C0387C0u, param_1, param_2, param_3, param_4, param_5, param_6, param_7,
                      param_8, param_9, param_10, param_11, param_12, param_13, param_14, param_15, param_16, param_17,
                      param_18, param_19, param_20, param_21, param_22, param_23, param_24, param_25, param_26,
                      param_27, param_28);
    put(0x8C10D250u, (param_22 & 0x7FF) | (param_21 & 0x7FF) << 16);
    put(0x8C10D254u, (param_24 & 0x7FF) | (param_23 & 0x7FF) << 16);
    put(0x8C10D258u, (param_18 & 0xF) | (param_19 & 0x1F) << 4 | (param_20 & 0xF) << 9);
    put(0x8C10D2C4u, param_1);
    put(0x8C10D2C8u, param_2);
    uint32_t line = param_2;
    if (static_cast<int32_t>(word(0x8C10D2C0u)) > 0)
        line -= word(0x8C10D2C0u);
    put(0x8C10D25Cu, (param_1 & 0x3FF) | (line & 0x3FF) << 10 | (param_3 & 0x3FF) << 20);
    put(0x8C10D298u, (param_6 & 0xFF) | (param_7 & 0xFF) << 8);
    put(0x8C10D29Cu, (param_5 & 0xFFFF) | (param_4 & 1) << 16 | (param_8 & 1) << 18);
    put(0x8C10D260u, param_17 & 0x1FF);
    put(0x8C10D268u, (param_12 & 7) | (param_11 & 1) << 3 | (param_13 & 0xFF) << 8 | (param_14 & 0xFF) << 16);
    put(0x8C10D264u, (param_9 & 1) | (param_10 & 1) << 1 | (param_15 & 3) << 2 | (param_16 & 7) << 4 |
                         static_cast<uint32_t>(param_28) << 8 | (param_26 & 0x1F) << 16 | (param_25 & 1) << 22 |
                         (param_27 & 1) << 23);
}

// The display's timing for the colour mode `param_1` in the display mode at 0x8C10D244, with the
// fields' offsets and the frame buffer's strides, from its width, height and depth. `param_2` asks
// for the vertical filter's settings.
undefined4 FUN_8c038960(int param_1, int param_2) {
    diff::Check check("FUN_8c038960", 0x8C038960u, param_1, param_2);
    guest::Frame frame(0x1E0);
    const uint32_t mode = word(0x8C10D244u);
    const uint32_t bits[6] = {word(0x8C0CA8FCu), word(0x8C0CA900u), word(0x8C0CA904u),
                              word(0x8C0CA908u), word(0x8C0CA90Cu), word(0x8C0CA910u)};
    uint32_t pal[5], half_pal[5];
    for (uint32_t i = 0; i < 5; ++i) {
        pal[i] = word(0x8C0CA914u + 4 * i);
        half_pal[i] = word(0x8C0CA928u + 4 * i);
    }
    const int32_t depth = static_cast<int32_t>(bits[param_1]);
    const bool interlaced = (mode & 1) != 1 && (mode & 0x2C) != 0;
    const int32_t width = static_cast<int32_t>(word(0x8C10D2B4u)), height = static_cast<int32_t>(word(0x8C10D2B8u));
    const int32_t fields = param_2 && !(mode & 0x20) ? 2 : 1;
    uint32_t s[29] = {};
    s[8] = 1;
    int32_t lines;
    int32_t extra;
    if ((mode & 1) == 1) {
        s[27] = 1;
        lines = 1;
        extra = 0;
    }
    else if (interlaced) {
        lines = 2;
        extra = 0;
    }
    else {
        lines = 1;
        extra = 1;
    }
    if (depth == 16) {
        if (param_1 == 3)
            s[12] = 3, s[15] = 0;
        else if (param_1 == 0 || param_1 != 1)
            s[12] = 1, s[15] = 1;
        else
            s[12] = 0, s[15] = 0;
        s[11] = 0;
    }
    else if (depth == 24) {
        s[12] = 4, s[15] = 2, s[11] = 0;
    }
    else if (depth == 32) {
        s[12] = 6, s[15] = 3, s[11] = 0;
    }
    s[10] = (mode & 1) == 1 && !(mode & 0x20) ? 1 : 0;
    const int32_t line_bits = width * depth;
    s[9] = interlaced ? 1 : 0;
    s[7] = 0;
    s[17] = static_cast<uint32_t>((line_bits < 0 ? line_bits + 63 : line_bits) >> 6);
    s[0] = static_cast<uint32_t>(_divls(32, line_bits) - 1);
    const int32_t half = ((height + (height < 0 ? 1 : 0)) >> 1) - 1, full = height - 1;
    const auto stride = [&] { put(0x8C10D2DCu, static_cast<uint32_t>(_divls(8, depth) * width)); };
    if (interlaced && (mode & 0x20)) {
        s[1] = static_cast<uint32_t>(half);
        s[2] = static_cast<uint32_t>(_divls(32, line_bits) + 1);
        put(0x8C10D2D8u, 0);
        stride();
    }
    else if (!interlaced && !(mode & 1) && (mode & 0x20)) {
        s[1] = static_cast<uint32_t>(half);
        put(0x8C10D2D8u, 0);
        stride();
        s[2] = static_cast<uint32_t>(_divls(32, line_bits) + 1);
    }
    else {
        s[1] = static_cast<uint32_t>(full);
        put(0x8C10D2D8u, 0);
        stride();
        s[2] = 1;
    }
    int32_t scale;
    if (param_2) {
        s[3] = 1;
        s[1] -= 2;
        scale = 2;
        if (mode & 1) {
            s[4] = 0x401;
        }
        else {
            switch (mode & ~0x13u) {
            case 0x64:
            case 0x68:
                s[4] = pal[word(0x8C10D304u)];
                s[2] = static_cast<uint32_t>(_divls(32, line_bits) + 1);
                break;
            case 0x60:
            case 0x40:
            case 0x44:
                put(0x8C10D2D8u, 0);
                put(0x8C10D2DCu, 0);
                s[4] = half_pal[word(0x8C10D304u)];
                s[2] = 1;
                break;
            case 0x24:
            case 0x28:
                s[4] = 0x401;
                s[2] = static_cast<uint32_t>(_divls(32, line_bits) + 1);
                break;
            case 0x20:
            case 0:
            case 4:
                put(0x8C10D2D8u, 0);
                put(0x8C10D2DCu, 0);
                s[4] = 0x800;
                s[2] = 1;
                break;
            }
        }
    }
    else {
        s[3] = 0;
        s[1] -= 1;
        if ((mode & 1) != 1 && (mode & 0x2C) == 0x20) {
            s[4] = (mode & 0x42) == 0x42 ? half_pal[word(0x8C10D304u)] : 0x800;
            put(0x8C10D2D8u, 0);
            put(0x8C10D2DCu, 0);
            s[2] = 1;
            s[1] -= 1;
        }
        else if (mode & 1) {
            s[4] = 0x400;
        }
        else if ((mode & 0x42) == 0x42) {
            s[4] = pal[word(0x8C10D304u)];
        }
        else {
            s[4] = mode & 0x20 ? 0x401 : 0x400;
        }
        scale = 1;
    }
    s[1] -= 1;
    s[5] = 0x40;
    s[6] = 0x80;
    put(0x8C10D2E0u, static_cast<uint32_t>(_divls(32, width) * scale));
    put(0x8C10D2E4u, static_cast<uint32_t>(_divls(32, height * lines * fields) + extra));
    if (word(0x8C10D2E4u) > 15)
        put(0x8C10D2E4u, 15);
    s[19] = 9;
    s[18] = 6;
    s[20] = depth == 16 ? 8 : 11;
    s[22] = 0;
    s[21] = static_cast<uint32_t>(width - 1);
    s[24] = 0;
    s[23] = static_cast<uint32_t>((mode & 1) != 1 && (mode & 0x2C) == 0x20 ? half : full);
    FUN_8c0387c0(s[0], s[1], s[2], s[3], s[4], s[5], s[6], s[7], s[8], s[10], s[11], s[12], s[13], s[14], s[15], s[16],
                 s[17], s[18], s[19], s[20], s[21], s[22], s[23], s[24], s[25], s[26], s[27], static_cast<byte>(s[28]));
    put(0x8C10D2E8u, 0);
    put(0x8C10D2ECu, 0);
    return check.returns(0u);
}

// The colour mode `param_1` of the display, with its timing.
undefined4 kmySetDisplayBpp(int param_1) {
    diff::Check check("kmySetDisplayBpp", 0x8C03926Cu, param_1);
    put(0x8C10D248u, static_cast<uint32_t>(param_1));
    FUN_8c038960(param_1, 0);
    FUN_8c03871c();
    return check.returns(0u);
}

// The display's synchronisation words from the timing `param_1` into Kamui's copies at 0x8C10D27C
// and the PowerVR's video registers, with the display off.
void FUN_8c038f80(gptr<uint> param_1) {
    diff::Check check("FUN_8c038f80", 0x8C038F80u, param_1);
    const auto t = [&](uint32_t i) { return word(param_1.a + 4 * i); };
    put(0x8C10D2BCu, 0);
    put(0x8C10D2C0u, 0);
    put(0x8C10D2A4u, t(0x3E));
    put(0x8C10D2D0u, t(0x3C));
    put(0x8C10D2D4u, t(0x3D));
    put(0x8C10D27Cu, t(0x15) << 1 | t(0x14) | t(0x17) << 2 | t(0x1A) << 3 | t(0xC) << 4 | t(0x18) << 5 | t(10) << 6 |
                         t(0xB) << 7 | t(0x12) << 8 | t(0x13) << 9);
    put(0x8C10D288u, (t(1) & 0x7FF) | (t(0) & 0x7FF) << 16);
    put(0x8C10D280u, (t(1) & 0x7FF) << 16);
    put(0x8C10D28Cu, (t(2) & 0x7FF) | (t(5) & 0x7FF) << 16);
    put(0x8C10D290u, (t(4) & 0x7FF) | (t(3) & 0x7FF) << 16);
    put(0x8C10D284u, (t(0x1D) & 0x7FF) | (t(0x1E) & 0x7FF) << 16);
    put(0x8C10D294u, (t(6) & 0x7F) | (t(7) & 0xF) << 8 | (t(8) & 0x3FF) << 12 | (t(9) & 0x7F) << 22);
    put(0x8C10D2A0u, (t(0xD) & 1) | (t(0xE) & 1) << 1 | (t(0xF) & 1) << 2 | (t(0x10) & 1) << 3 | (t(0x11) & 0xF) << 4 |
                         (t(0x1B) & 1) << 8 | (t(0x1C) & 0x3F) << 16);
    put(0x8C10D2A8u, word(0x8C10D2D0u) | word(0x8C10D2D4u) * 0x10000);
    put(0x8C10D2CCu, word(0x8C10D2A4u));
    kmiWriteRegister(0x44, kmiReadRegister(0x44) & 0x800000);
    kmiWriteRegister(0x5C, 0);
    kmiWriteRegister(0xCC, word(0x8C10D284u));
    kmiWriteRegister(0xD0, word(0x8C10D27Cu));
    kmiWriteRegister(0xD4, word(0x8C10D288u));
    kmiWriteRegister(0xD8, word(0x8C10D28Cu));
    kmiWriteRegister(0xDC, word(0x8C10D290u));
    kmiWriteRegister(0xE0, word(0x8C10D294u));
    kmiWriteRegister(0xEC, word(0x8C10D2A4u));
    kmiWriteRegister(0xF0, word(0x8C10D2A8u));
    kmiWriteRegister(0xE8, word(0x8C10D2A0u) | 8);
    kmiWriteRegister(200, word(0x8C10D280u));
}

// The VGA display mode `param_1`: its size and its synchronisation timing for the display module.
undefined4 kmyInitializeVGA(uint param_1) {
    diff::Check check("kmyInitializeVGA", 0x8C03C160u, param_1);
    guest::Frame frame(0x108);
    const uint32_t t = frame.top - 0x108;
    put(0x8C10D244u, param_1);
    kmyiSetScreenSize(param_1);
    static constexpr uint32_t kTiming[31] = {0x7E, 0x345, 0x359, 0x28, 0x208, 0x20C, 0x3F, 3,    0x319, 0xF, 0,
                                             0,    0,     0,     0,    0,     0,     0,    1,    0,     0,   0,
                                             0,    0,     0,     0x1E0, 0,    0,     0x16, 0x208, 0x15};
    for (uint32_t i = 0; i < 31; ++i) put(t + 4 * i, kTiming[i]);
    put(t + 4 * 27, param_1 & 0x10 ? 0 : 1);
    put(t + 4 * 60, 0x28);
    put(t + 4 * 61, 0x28);
    put(t + 4 * 62, 0xA8);
    put(0x8C10D2F8u, 0x1E0);
    put(0x8C10D2F0u, word(0x8C10D2F8u) + word(t + 12));
    put(0x8C10D2F4u, word(t + 12));
    gptr<code>(word(0x8C10D24Cu))(t);
    return check.returns(0u);
}

// The default vertical filter mode `param_4` of the display mode `param_1`, with the anti-alias
// flag `param_2` into `param_3`.
undefined4 kmyGetDefaultFilterMode(uint param_1, int param_2, gptr<int> param_3, gptr<undefined4> param_4) {
    diff::Check check("kmyGetDefaultFilterMode", 0x8C03A200u, param_1, param_2, param_3, param_4);
    guest::Frame frame(0x40);
    const uint32_t interlaced = frame.top - 0x2C, field = frame.top - 0x40;
    for (uint32_t i = 0; i < 20; i += 4) {
        put(interlaced + i, word(0x8C0CA96Cu + i));
        put(field + i, word(0x8C0CA980u + i));
    }
    const bool pal = (param_1 & 0x42) == 0x42;
    put(param_3.a, static_cast<uint32_t>(param_2));
    uint32_t ratio = param_4.a;
    if (pal)
        ratio = static_cast<uint32_t>(kmyiGetPALExtRatio(param_1));
    const bool progressive = param_1 & 1;
    const bool frame_mode = (param_1 & 0x2C) == 0x20;
    uint32_t value;
    if (!param_2) {
        if (progressive)
            value = 0;
        else if (!pal)
            value = frame_mode ? 2 : 1;
        else if (!frame_mode)
            value = word(interlaced + 4 * ratio);
        else
            value = word(field + 4 * ratio);
    }
    else if (pal) {
        value = progressive || !frame_mode ? word(interlaced + 4 * ratio) : word(field + 4 * ratio);
    }
    else if (!(param_1 & 0x20)) {
        value = 4;
    }
    else {
        value = progressive || !frame_mode ? 1 : 2;
    }
    put(param_4.a, value);
    return check.returns(0u);
}

// The display parameter `param_1` of the frame buffer `param_2`, or of the display for 0, into
// `param_3`: its width, height, colour mode, pixel size, region size, line bytes and so on.
undefined4 kmyGetDisplayParametersInfo(uint param_1, int param_2, gptr<int> param_3) {
    diff::Check check("kmyGetDisplayParametersInfo", 0x8C03A360u, param_1, param_2, param_3);
    guest::Frame frame(0x30);
    const uint32_t sizes = frame.top - 0x28, a = frame.top - 0x30, b = frame.top - 0x2C;
    put(frame.top - 0xC, 0);
    for (uint32_t i = 0; i < 24; i += 4) put(sizes + i, word(0x8C0CA994u + i));
    if (param_1 > 0xB)
        return check.returns(0u);
    const uint32_t out = param_3.a, fb = static_cast<uint32_t>(param_2), mode = word(0x8C10D244u);
    const uint32_t bpp = word(0x8C10D248u);
    switch (param_1) {
    case 0: put(out, word(0x8C10D2B4u)); break;
    case 1: put(out, word(0x8C10D2B8u)); break;
    case 2: put(out, bpp); break;
    case 3: put(out, word(sizes + 4 * bpp)); break;
    case 4: kmyGetDisplayRegionSize(mode, gptr<undefined4>(out), gptr<undefined4>(0), param_2); break;
    case 5: kmyGetDisplayRegionSize(mode, gptr<undefined4>(0), gptr<undefined4>(out), param_2); break;
    case 6:
        kmyGetDisplayRegionSize(mode, gptr<undefined4>(a), gptr<undefined4>(b), param_2);
        put(a, word(a) * word(b));
        put(out, word(a));
        break;
    case 7: put(out, fb ? word(fb + 12) : word(0x8C10D2B4u)); break;
    case 8:
        if (fb) {
            put(out, word(fb + 16));
        }
        else if (word(0x8C10D2FCu)) {
            put(out, word(0x8C10D300u));
        }
        else if ((mode & 1) == 1 || (mode & 0x2C) != 0x20) {
            put(out, word(0x8C10D2B8u));
        }
        else {
            const int32_t h = static_cast<int32_t>(word(0x8C10D2B8u));
            put(a, static_cast<uint32_t>((h + (h < 0 ? 1 : 0)) >> 1));
            put(out, word(a));
        }
        break;
    case 9: put(out, fb ? word(fb + 8) : bpp); break;
    case 10: put(out, fb ? word(fb + 4) : word(sizes + 4 * bpp)); break;
    case 11:
        if (fb) {
            put(out, word(fb + 44) << 3);
        }
        else {
            const int32_t s = static_cast<int32_t>(word(sizes + 4 * bpp));
            put(out, static_cast<uint32_t>(static_cast<int32_t>(word(0x8C10D2B4u)) * ((s < 0 ? s + 7 : s) >> 3)));
        }
        break;
    }
    return check.returns(0u);
}

// The display mode `nDisplayMode` with the colour mode `nBpp`: its timing by the device's table
// of mode functions, its filter, and the display blank until the next frame.
KMSTATUS kmSetDisplayMode(KMDISPLAYMODE nDisplayMode, KMBPPMODE nBpp, KMBOOLEAN fDither, KMBOOLEAN fAntiAlias) {
    diff::Check check("kmSetDisplayMode", 0x8C0362A0u, nDisplayMode, nBpp, fDither, fAntiAlias);
    guest::Frame frame(0x20);
    const uint32_t mode = static_cast<uint32_t>(nDisplayMode), table = frame.top - 0x20;
    put(frame.top - 0xC, 1);
    if (!word(0x8C10B688u)) {
        put(table, 0x8C03BB20u);
        put(table + 4, 0x8C03C160u);
        put(table + 8, 0x8C03BD60u);
        put(table + 12, 0x8C03C160u);
        if ((mode & 0x42) == 0x42) {
            put(0x8C10BFECu, mode);
            kmyRequestCallback(0xB);
            put(0x8C10BFF4u, word(0x8C10BFF0u));
        }
    }
    else if (word(0x8C10B688u) == 0x10000) {
        put(table, 0x8C03B740u);
        put(table + 4, 0x8C03B9E0u);
        put(table + 8, 0x8C03B740u);
        put(table + 12, 0x8C03B9E0u);
    }
    else {
        return check.returns(KMSTATUS(KMSTATUS_INVALID_DISPLAY_MODE));
    }
    put(0x8C10B6ACu, 1);
    put(0x8C10B694u, mode);
    put(0x8C10B698u, static_cast<uint32_t>(fDither));
    put(0x8C10B6A0u, static_cast<uint32_t>(nBpp));
    kmyGetDefaultFilterMode(mode, static_cast<int>(fAntiAlias), gptr<int>(0x8C10B69Cu), gptr<undefined4>(0x8C10B6A8u));
    const int32_t filter = static_cast<int32_t>(word(0x8C10B6A8u));
    put(0x8C10B6B0u, filter == 2 || filter == 10 || (filter >= 30 && filter <= 34) ? 1 : 0);
    put(0x8C10B6A4u, word(0x8C10B6A4u) | 3);
    if (!word(0x8C10BFF8u))
        kmyWaitVBlank();
    gptr<code>(word(table + 4 * (mode & 3)))(mode);
    kmySetDisplayBpp(static_cast<int>(nBpp));
    kmyBlankScreen(1);
    put(0x8C10BFC4u, 1);
    return check.returns(KMSTATUS_SUCCESS);
}

// The PowerVR's horizontal-sync interrupt at the line `nInterruptLine` of the display, within its
// lines.
KMSTATUS kmSetHSyncLine(KMINT32 nInterruptLine) {
    diff::Check check("kmSetHSyncLine", 0x8C03D34Cu, nInterruptLine);
    const uint32_t mode = word(0x8C10D244u);
    const int32_t last = static_cast<int32_t>((mode & 1) == 1 || !(mode & 0x2C) ? word(0x8C10D28Cu) >> 16
                                                                                 : word(0x8C10D28Cu) >> 17);
    int32_t line = nInterruptLine + static_cast<int32_t>(word(0x8C10D2D0u) + word(0x8C10D2C0u));
    if ((mode & 1) != 1 && (mode & 0x20))
        line = ((nInterruptLine + (nInterruptLine < 0 ? 1 : 0)) >> 1) +
               static_cast<int32_t>(word(0x8C10D2D0u) + word(0x8C10D2C0u));
    if (line < 0)
        line += last;
    if (last < line)
        line = last;
    if (line < 0)
        line = 0;
    put(0x8C10D280u, (word(0x8C10D280u) & 0x3FF0000) | (static_cast<uint32_t>(line) & 0x3FF));
    kmiWriteRegister(200, word(0x8C10D280u));
    return check.returns(KMSTATUS_SUCCESS);
}

KMSTATUS kmSetCullingRegister(KMFLOAT fCullVal) {
    diff::Check check("kmSetCullingRegister", 0x8C036240u, fCullVal);
    guest::store<float>(0x8C10B670u, fCullVal);
    put(0x8C10B5F0u, word(0x8C10B5F0u) | 2);
    put(0x8C10B5ECu, word(0x8C10B5ECu) & ~2u);
    return check.returns(KMSTATUS(KMSTATUS_NOT_IMPLEMENTED));
}

KMSTATUS kmSetBorderColor(undefined4 arg_r4, undefined4 arg_r5, undefined4 arg_r6, undefined4 arg_r7,
                          KMPACKEDARGB BorderColor) {
    diff::Check check("kmSetBorderColor", 0x8C0360E0u, arg_r4, arg_r5, arg_r6, arg_r7, BorderColor);
    kmySetBorderColor(BorderColor.dwPacked);
    return check.returns(KMSTATUS_SUCCESS);
}

}  // namespace game
