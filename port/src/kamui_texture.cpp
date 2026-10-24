// Kamui's texture memory natively: the allocator of its areas and of the Small VQ textures' slots
// with its work area and set-up, and the copies into texture and frame buffer memory. Each function
// does what its machine code does, and IKARUGA_DIFF tests each against its machine code on the
// game's own calls.
#include "decomp.h"
#include "diff.h"

namespace game {
namespace {

uint32_t word(uint32_t a) { return guest::load<uint32_t>(a); }
void put(uint32_t a, uint32_t v) { guest::store<uint32_t>(a, v); }
uint32_t half(uint32_t a) { return guest::load<uint16_t>(a); }
void set_half(uint32_t a, uint32_t v) { guest::store<uint16_t>(a, static_cast<uint16_t>(v)); }

// The allocator's state from 0x8C0CF270: 1 at 0 when the areas need no balance between the two
// banks, the count of area entries at 8 and of Small VQ entries at 12, the address where the
// second bank starts at 24, the entries' bases at 32 and 72. Each bank has four list words from
// 36, 16 bytes a bank: the used areas' head and tail, then the free areas' head and tail.
constexpr uint32_t kAllocator = 0x8C0CF270u;
constexpr uint32_t kBanks = kAllocator + 36;
// The Small VQ entries in use, linked by their word at 4.
constexpr uint32_t kSmallVQList = 0x8C0CF2B4u;

// An area entry of 24 bytes: its flags in the halfword at 0 (1 in use), the entries before and
// after it at 4 and 8, the area's address at 12 and size at 16, and at 20 the word that holds the
// address for the area's owner. A Small VQ entry of 76 bytes: its flags and kind in the halfword
// at 0, the next entry at 4, its area at 8 and from 12 the owners' words of its slots.

uint32_t used_list(int32_t bank) { return kBanks + 16 * static_cast<uint32_t>(bank); }
uint32_t free_list(int32_t bank) { return used_list(bank) + 8; }

// The area from the free list `bank`'s entry that comes right after or right before the entry
// `e`, or 0.
uint32_t neighbour(int32_t bank, uint32_t e) {
    for (uint32_t f = word(free_list(bank)); f; f = word(f + 8))
        if (word(f + 12) == word(e + 16) + word(e + 12) || word(e + 12) == word(f + 16) + word(f + 12))
            return f;
    return 0;
}

}  // namespace

// The first free area entry, marked in use, or 0.
int kmytex_FBAllocEntry() {
    diff::Check check("kmytex_FBAllocEntry", 0x8C03EC44u);
    const uint32_t count = word(kAllocator + 8);
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t e = word(kAllocator + 32) + 24 * i;
        if (!(half(e) & 1)) {
            set_half(word(kAllocator + 32) + 24 * i, 1);
            return check.returns(static_cast<int>(word(kAllocator + 32) + 24 * i));
        }
    }
    return check.returns(0);
}

void kmytex_FBFreeEntry(int param_1) {
    diff::Check check("kmytex_FBFreeEntry", 0x8C03ECCCu, param_1);
    for (uint32_t i = 0; i < word(kAllocator + 8); ++i) {
        const uint32_t e = word(kAllocator + 32) + 24 * i;
        if (e == static_cast<uint32_t>(param_1))
            set_half(e, half(e) & 0xFFFE);
    }
}

// Puts the entry `param_3` at the head of the list with the head `param_1` and the tail `param_2`.
void kmytex_FBEnQueueTop(gptr<int> param_1, gptr<int> param_2, int param_3) {
    diff::Check check("kmytex_FBEnQueueTop", 0x8C03ED10u, param_1, param_2, param_3);
    const uint32_t head = param_1.a, e = static_cast<uint32_t>(param_3);
    if (!word(head)) {
        put(e + 4, 0);
        put(e + 8, 0);
        put(head, e);
        put(param_2.a, e);
        return;
    }
    const uint32_t first = word(head);
    put(e + 4, word(first + 4));
    put(e + 8, word(head));
    put(first + 4, e);
    put(head, e);
}

// Takes the entry `param_3` out of the list with the head `param_1` and the tail `param_2`.
void kmytex_FBDeQueueEntry(gptr<undefined4> param_1, gptr<undefined4> param_2, int param_3) {
    diff::Check check("kmytex_FBDeQueueEntry", 0x8C03ED50u, param_1, param_2, param_3);
    const uint32_t e = static_cast<uint32_t>(param_3);
    const uint32_t before = word(e + 4), after = word(e + 8);
    if (!word(e + 4))
        put(param_1.a, word(e + 8));
    else
        put(before + 8, word(e + 8));
    if (!word(e + 8))
        put(param_2.a, word(e + 4));
    else
        put(after + 4, word(e + 4));
}

// An area of `param_2` bytes, rounded up to 32, in the bank `param_4`: the first free area that is
// large enough, or its start. The area's address goes to `param_3`, and the entry gets the flags
// `param_1` and 0x10 for bank 0, 0x20 for bank 1. Returns 3 when no area is large enough.
undefined4 kmytex_TXalloc(ushort param_1, uint param_2, gptr<undefined4> param_3, short param_4) {
    diff::Check check("kmytex_TXalloc", 0x8C03E94Eu, param_1, param_2, param_3, param_4);
    const uint32_t size = param_2 & 0x1F ? (param_2 + 32) & ~31u : param_2;
    uint32_t e = 0;
    for (uint32_t f = word(free_list(param_4)); f; f = word(f + 8))
        if (word(f + 16) >= size) {
            e = f;
            break;
        }
    if (!e)
        return check.returns(3u);
    const auto own = [&](uint32_t entry) {
        put(param_3.a, word(entry + 12));
        put(entry + 20, param_3.a);
    };
    const uint32_t side = param_4 == 0 ? 0x10 : 0x20;
    if (word(e + 16) == size) {
        kmytex_FBDeQueueEntry(gptr<undefined4>(free_list(param_4)), gptr<undefined4>(free_list(param_4) + 4),
                              static_cast<int>(e));
        kmytex_FBEnQueueTop(gptr<int>(used_list(param_4)), gptr<int>(used_list(param_4) + 4), static_cast<int>(e));
        own(e);
        set_half(e, 1);
        set_half(e, half(e) | side);
        set_half(e, half(e) | param_1);
        return check.returns(0u);
    }
    const uint32_t n = static_cast<uint32_t>(kmytex_FBAllocEntry());
    if (!n)
        return check.returns(3u);
    put(n + 12, word(e + 12));
    put(n + 16, size);
    put(e + 16, word(e + 16) - size);
    put(e + 12, word(e + 12) + size);
    kmytex_FBEnQueueTop(gptr<int>(used_list(param_4)), gptr<int>(used_list(param_4) + 4), static_cast<int>(n));
    own(n);
    set_half(n, half(n) | side);
    set_half(n, half(n) | param_1);
    return check.returns(0u);
}

// An area of `param_2` bytes for `param_3`, with flag 2 of `param_1` as the entry's flag 2. With
// flag 1 it comes from the end of the free areas. Without the balance flag, the bank with more
// free memory gives it.
undefined4 kmyAllocateArea(uint param_1, uint param_2, gptr<uint> param_3) {
    diff::Check check("kmyAllocateArea", 0x8C037340u, param_1, param_2, param_3);
    guest::Frame frame(0x28);
    const ushort flags = param_1 & 2 ? 2 : 0;
    short bank = 0;
    if (word(kAllocator) != 1) {
        const uint32_t free = frame.top - 0x28, total = frame.top - 0x24;
        kmytex_GetfreememEx(gptr<uint>(free), gptr<uint>(total));
        bank = word(free) < word(total) ? 1 : 0;
    }
    if (param_1 & 1)
        return check.returns(kmytex_TXalloc2(flags, param_2, param_3, bank));
    return check.returns(kmytex_TXalloc(flags, param_2, gptr<undefined4>(param_3.a), bank));
}

// Puts the entry `param_2` after the entry `param_3` of the list with the tail `param_1`.
void kmytex_FBEnQueueEntry(gptr<int> param_1, int param_2, int param_3) {
    diff::Check check("kmytex_FBEnQueueEntry", 0x8C03ED34u, param_1, param_2, param_3);
    const uint32_t e = static_cast<uint32_t>(param_2), after = static_cast<uint32_t>(param_3);
    uint32_t next = 0;
    if (!word(after + 8)) {
        put(param_1.a, e);
        put(e + 4, after);
    }
    else {
        put(word(after + 8) + 4, e);
        put(e + 4, after);
        next = word(after + 8);
    }
    put(e + 8, next);
    put(after + 8, e);
}

// The area of `param_2` bytes, rounded up to 32, at the address in `param_3`, rounded up to 32, in
// the bank `param_4`: a free area that holds it gives it from its start, from its end or from its
// middle. The entry gets the flags `param_1` and 0x40 for bank 0, 0x80 for bank 1. Returns 3 when
// no free area holds it.
undefined4 kmytex_ABSalloc(ushort param_1, uint param_2, gptr<uint> param_3, short param_4) {
    diff::Check check("kmytex_ABSalloc", 0x8C03E700u, param_1, param_2, param_3, param_4);
    const uint32_t at = word(param_3.a) & 0x1F ? (word(param_3.a) + 32) & ~31u : word(param_3.a);
    const uint32_t size = param_2 & 0x1F ? (param_2 + 32) & ~31u : param_2;
    const uint32_t end = at + size;
    uint32_t e = 0;
    for (uint32_t f = word(free_list(param_4)); f; f = word(f + 8)) {
        const uint32_t start = word(f + 12);
        if (start > at || start + word(f + 16) <= at || start > end || start + word(f + 16) < end)
            continue;
        e = f;
        break;
    }
    if (!e)
        return check.returns(3u);
    const uint32_t side = param_4 == 0 ? 0x40 : 0x80;
    const auto own = [&](uint32_t entry) {
        put(param_3.a, word(entry + 12));
        put(entry + 20, param_3.a);
        set_half(entry, 1);
        set_half(entry, half(entry) | side);
        set_half(entry, half(entry) | param_1);
    };
    if (word(e + 16) == size) {
        kmytex_FBDeQueueEntry(gptr<undefined4>(free_list(param_4)), gptr<undefined4>(free_list(param_4) + 4),
                              static_cast<int>(e));
        kmytex_FBEnQueueTop(gptr<int>(used_list(param_4)), gptr<int>(used_list(param_4) + 4), static_cast<int>(e));
        own(e);
        return check.returns(0u);
    }
    if (word(e + 12) == at) {
        const uint32_t n = static_cast<uint32_t>(kmytex_FBAllocEntry());
        if (!n)
            return check.returns(3u);
        put(n + 12, word(e + 12));
        put(n + 16, size);
        put(e + 16, word(e + 16) - size);
        put(e + 12, word(e + 12) + size);
        kmytex_FBEnQueueTop(gptr<int>(used_list(param_4)), gptr<int>(used_list(param_4) + 4), static_cast<int>(n));
        own(n);
        return check.returns(0u);
    }
    if (word(e + 12) + word(e + 16) == end) {
        const uint32_t n = static_cast<uint32_t>(kmytex_FBAllocEntry());
        if (!n)
            return check.returns(3u);
        put(e + 16, word(e + 16) - size);
        put(n + 12, at);
        put(n + 16, size);
        kmytex_FBEnQueueTop(gptr<int>(used_list(param_4)), gptr<int>(used_list(param_4) + 4), static_cast<int>(n));
        own(n);
        return check.returns(0u);
    }
    const uint32_t n = static_cast<uint32_t>(kmytex_FBAllocEntry());
    if (!n)
        return check.returns(3u);
    const uint32_t rest = static_cast<uint32_t>(kmytex_FBAllocEntry());
    if (!rest) {
        kmytex_FBFreeEntry(static_cast<int>(n));
        return check.returns(3u);
    }
    const uint32_t whole = word(e + 16), before = at - word(e + 12);
    put(e + 16, before);
    put(n + 12, at);
    put(n + 16, size);
    kmytex_FBEnQueueTop(gptr<int>(used_list(param_4)), gptr<int>(used_list(param_4) + 4), static_cast<int>(n));
    put(rest + 12, end);
    put(rest + 16, whole - size - before);
    kmytex_FBEnQueueEntry(gptr<int>(free_list(param_4) + 4), static_cast<int>(rest),
                          static_cast<int>(word(free_list(param_4) + 4)));
    own(n);
    return check.returns(0u);
}

// The entries of the areas and of the Small VQ textures free, and the lists empty.
undefined4 FUN_8c03b0d2() {
    diff::Check check("FUN_8c03b0d2", 0x8C03B0D2u);
    for (uint32_t i = 0; i < word(kAllocator + 8); ++i) set_half(word(kAllocator + 32) + 24 * i, 0);
    for (uint32_t i = 0; i < 32; i += 4) put(kBanks + i, 0);
    for (uint32_t i = 0; i < word(kAllocator + 12); ++i) set_half(word(kAllocator + 72) + 76 * i, 0);
    put(kSmallVQList, 0);
    return check.returns(0u);
}

// The texture allocator's work area for the counts at `param_1`: words 0 to 2 count 24-byte entries,
// words 3 to 10 the Small VQ textures by size, which share the 76-byte blocks 16, 8, 4, 2 or 1 at a
// time. Word 11 gets the entries with the blocks and one more, word 12 the blocks and word 13 the
// bytes. 18 when one of words 0 to 2 is 0.
undefined4 kmuCalculateKamuiWorkareaSize(gptr<int> param_1) {
    diff::Check check("kmuCalculateKamuiWorkareaSize", 0x8C056620u, param_1);
    const uint32_t sizes = param_1.a;
    put(sizes + 44, 0);
    put(sizes + 48, 0);
    put(sizes + 52, 0);
    static constexpr uint32_t kShift[8] = {4, 4, 4, 4, 3, 2, 1, 0};
    uint32_t blocks = 0;
    for (uint32_t i = 0; i < 8; ++i) {
        const uint32_t n = word(sizes + 12 + 4 * i);
        blocks += (n + (1u << kShift[i]) - 1) >> kShift[i];
    }
    put(sizes + 48, blocks);
    undefined4 status = 0;
    for (uint32_t i = 0; i < 3; ++i) {
        if (word(sizes + 4 * i))
            put(sizes + 44, word(sizes + 44) + word(sizes + 4 * i));
        else
            status = 0x12;
    }
    const uint32_t entries = word(sizes + 44) + word(sizes + 48) + 1;
    put(sizes + 44, entries);
    put(sizes + 52, entries * 24 + word(sizes + 48) * 76);
    return check.returns(status);
}

// The allocator for the video memory of `param_1` bytes, with `param_2` area entries at `param_3`
// and `param_4` Small VQ entries at `param_5`: the texture memory below the reserved end
// (0x8C0CF2BC, rounded up to 32) as one free area, or with the balance off as two banks of 4 MB
// (8 MB of 16) each. The reserved part's address goes to the word at 0x8C0CF2C4. Returns 3 when
// the entries run out.
undefined4 kmyInitAllocator(dword param_1, undefined4 param_2, undefined4 param_3, undefined4 param_4,
                            undefined4 param_5) {
    diff::Check check("kmyInitAllocator", 0x8C03AF80u, param_1, param_2, param_3, param_4, param_5);
    constexpr uint32_t a = kAllocator;
    put(a + 8, param_2);
    put(a + 32, param_3);
    put(a + 12, param_4);
    put(a + 72, param_5);
    if (word(a + 76) & 0x1F)
        put(a + 76, (word(a + 76) + 32) & ~31u);
    put(a + 4, param_1);
    const auto first = [](uint32_t bank_size) {
        put(a + 80, bank_size - word(a + 76));
        put(a + 16, 0);
        put(a + 20, word(a + 80));
    };
    const auto add_free = [](uint32_t e, uint32_t from, uint32_t head) {
        put(e + 12, word(a + from));
        put(e + 16, word(a + from + 4));
        put(e + 8, word(a + head));
        put(a + head, e);
        put(e + 4, word(a + head + 4));
        put(a + head + 4, e);
    };
    if (word(a) == 1) {
        const uint32_t bank = param_1 == 0x1000000 ? 0x1000000u : 0x800000u;
        first(bank);
        put(a + 24, bank);
        put(a + 28, 0);
        kmiWriteRegister(0xE4, 0);
        FUN_8c03b0d2();
        const uint32_t e = static_cast<uint32_t>(kmytex_FBAllocEntry());
        if (!e)
            return check.returns(3u);
        add_free(e, 16, 44);
    }
    else {
        const uint32_t bank = param_1 == 0x1000000 ? 0x800000u : 0x400000u;
        first(bank);
        put(a + 24, bank);
        put(a + 28, bank);
        kmiWriteRegister(0xE4, 0);
        FUN_8c03b0d2();
        uint32_t e = static_cast<uint32_t>(kmytex_FBAllocEntry());
        if (!e)
            return check.returns(3u);
        add_free(e, 16, 44);
        e = static_cast<uint32_t>(kmytex_FBAllocEntry());
        if (!e)
            return check.returns(3u);
        add_free(e, 24, 60);
    }
    if (word(a + 76))
        put(word(a + 84), word(a + 80));
    return check.returns(0u);
}

// The free texture memory into `param_1`, and the largest free area into `param_2`: both banks'
// free areas and the Small VQ textures' free slots.
undefined4 kmyGetFreeSize(gptr<int> param_1, gptr<uint> param_2) {
    diff::Check check("kmyGetFreeSize", 0x8C03A760u, param_1, param_2);
    guest::Frame frame(0x28);
    uint32_t sums[2] = {}, largest[2] = {};
    for (uint32_t bank = 0; bank < 2; ++bank)
        for (uint32_t f = word(free_list(static_cast<int32_t>(bank))); f; f = word(f + 8)) {
            if (word(f + 16) > largest[bank])
                largest[bank] = word(f + 16);
            sums[bank] += word(f + 16);
        }
    uint32_t slots = 0;
    for (uint32_t e = word(kSmallVQList); e; e = word(e + 4)) {
        uint32_t bytes = 0, count = 0;
        switch (half(e) & 0xF000) {
        case 0x1000: bytes = 0x90, count = 16; break;
        case 0x2000: bytes = 0xA0, count = 16; break;
        case 0x3000: bytes = 0xC0, count = 16; break;
        case 0x4000: bytes = 0xE0, count = 16; break;
        case 0x5000: bytes = 0x200, count = 8; break;
        case 0x6000: bytes = 0x360, count = 4; break;
        case 0x7000: bytes = 0x800, count = 2; break;
        case 0x8000: bytes = 0xD60, count = 1; break;
        }
        for (uint32_t i = 0; i < count; ++i)
            if (!word(e + 12 + 4 * i))
                slots += bytes;
    }
    put(frame.top - 0x20, sums[0]);
    put(frame.top - 0x24, sums[1]);
    put(frame.top - 0x28, slots);
    put(param_1.a, sums[0] + sums[1] + slots);
    put(param_2.a, largest[0] < largest[1] ? largest[1] : largest[0]);
    return check.returns(0u);
}

// The entry of the used area at `param_1`, with its bank into `param_2`, or 0.
int FUN_8c039944(uint param_1, gptr<short> param_2) {
    diff::Check check("FUN_8c039944", 0x8C039944u, param_1, param_2);
    guest::store<int16_t>(param_2.a, param_1 >= word(kAllocator + 24) ? 1 : 0);
    for (uint32_t e = word(used_list(guest::load<int16_t>(param_2.a))); e; e = word(e + 8))
        if (word(e + 12) == param_1)
            return check.returns(static_cast<int>(e));
    return check.returns(0);
}

// Frees the used area `param_2` of the bank `param_1`: it joins the free areas next to it, and
// the result goes to the head of the free list.
undefined4 FUN_8c03997c(short param_1, int param_2) {
    diff::Check check("FUN_8c03997c", 0x8C03997Cu, param_1, param_2);
    uint32_t e = static_cast<uint32_t>(param_2);
    kmytex_FBDeQueueEntry(gptr<undefined4>(used_list(param_1)), gptr<undefined4>(used_list(param_1) + 4),
                          static_cast<int>(e));
    for (uint32_t f = neighbour(param_1, e); f; f = neighbour(param_1, e)) {
        if (word(f + 12) == word(e + 12) + word(e + 16))
            put(f + 12, word(e + 12));
        put(f + 16, word(f + 16) + word(e + 16));
        kmytex_FBFreeEntry(static_cast<int>(e));
        kmytex_FBDeQueueEntry(gptr<undefined4>(free_list(param_1)), gptr<undefined4>(free_list(param_1) + 4),
                              static_cast<int>(f));
        e = f;
    }
    kmytex_FBEnQueueTop(gptr<int>(free_list(param_1)), gptr<int>(free_list(param_1) + 4), static_cast<int>(e));
    return check.returns(0u);
}

// Frees the area at `param_1`. Returns 7 when no area starts there.
undefined4 kmyFreeArea(uint param_1) {
    diff::Check check("kmyFreeArea", 0x8C039920u, param_1);
    guest::Frame frame(0xC);
    const uint32_t bank = frame.top - 0xC;
    put(frame.top - 8, param_1);
    const int e = FUN_8c039944(param_1, gptr<short>(bank));
    if (!e)
        return check.returns(7u);
    return check.returns(FUN_8c03997c(guest::load<int16_t>(bank), e));
}

// The layout of the Small VQ kind `param_2` into `param_1`: its kind bits, its slot's size, two
// sizes of its data and the slots in an area of 2,048 bytes. Returns 0 for an unknown kind.
undefined4 kmytex_GetSmallVQprofile(gptr<dword> param_1, int param_2) {
    diff::Check check("kmytex_GetSmallVQprofile", 0x8C03EDA0u, param_1, param_2);
    struct Profile {
        uint32_t kind, slot, data, rest;
    };
    static constexpr Profile kProfiles[8] = {{0x1000, 0x80, 0x20, 0xF90},    {0x2000, 0x80, 0x20, 0xFA0},
                                             {0x3000, 0x80, 0x40, 0xFC0},    {0x4000, 0x80, 0x60, 0xFE0},
                                             {0x5000, 0x100, 0x100, 0x1000}, {0x6000, 0x200, 0x160, 0xF60},
                                             {0x7000, 0x400, 0x400, 0x1000}, {0x8000, 0x800, 0x560, 0xD60}};
    if (param_2 < 1 || param_2 > 8)
        return check.returns(0u);
    const Profile& p = kProfiles[param_2 - 1];
    const uint32_t out = param_1.a;
    put(out, p.kind);
    put(out + 4, p.slot);
    put(out + 8, p.data);
    put(out + 12, p.rest);
    put(out + 16, 0x800 / word(out + 4));
    return check.returns(1u);
}

// A free Small VQ entry, marked in use, or 0.
int FUN_8c0377ca() {
    diff::Check check("FUN_8c0377ca", 0x8C0377CAu);
    const uint32_t count = word(kAllocator + 12);
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t e = word(kAllocator + 72) + 76 * i;
        if (!(half(e) & 1)) {
            set_half(word(kAllocator + 72) + 76 * i, 1);
            return check.returns(static_cast<int>(word(kAllocator + 72) + 76 * i));
        }
    }
    return check.returns(0);
}

void kmytex_VQFreeEntry(int param_1) {
    diff::Check check("kmytex_VQFreeEntry", 0x8C03EEC8u, param_1);
    for (uint32_t i = 0; i < word(kAllocator + 12); ++i) {
        const uint32_t e = word(kAllocator + 72) + 76 * i;
        if (e == static_cast<uint32_t>(param_1))
            set_half(word(kAllocator + 72) + 76 * i, half(e) & 0xFFFE);
    }
}

// Puts the Small VQ entry `param_2` at the head of the list `param_1`.
void FUN_8c03780c(gptr<int> param_1, int param_2) {
    diff::Check check("FUN_8c03780c", 0x8C03780Cu, param_1, param_2);
    put(static_cast<uint32_t>(param_2) + 4, word(param_1.a) ? word(param_1.a) : 0);
    put(param_1.a, static_cast<uint32_t>(param_2));
}

// Takes the Small VQ entry `param_2` out of the list `param_1`.
void FUN_8c039c10(gptr<int> param_1, int param_2) {
    diff::Check check("FUN_8c039c10", 0x8C039C10u, param_1, param_2);
    const uint32_t e = static_cast<uint32_t>(param_2);
    uint32_t before = 0;
    for (uint32_t x = word(param_1.a); x; x = word(x + 4))
        if (word(x + 4) == e) {
            before = x;
            break;
        }
    if (word(param_1.a) == e)
        put(param_1.a, word(e + 4));
    else
        put(before + 4, word(e + 4));
}

// A slot of a Small VQ texture of the kind `param_3`, `param_4` bytes a slot and `param_6` slots in
// an area: a free slot of an entry of the kind, else a new entry with an area of `param_5` bytes,
// with flag `param_1` as kmyAllocateArea's flag 1. The slot's address goes to `param_2`. Returns 3
// when there is no room.
undefined4 FUN_8c037624(int param_1, gptr<int> param_2, uint param_3, uint param_4, uint param_5, uint param_6,
                        uint ctx_r11) {
    diff::Check check("FUN_8c037624", 0x8C037624u, param_1, param_2, param_3, param_4, param_5, param_6, ctx_r11);
    guest::Frame frame(0x2C);
    uint32_t entry = 0, slot = 0;
    for (uint32_t e = word(kSmallVQList); e && !entry; e = word(e + 4)) {
        if ((half(e) & 0xF000) != param_3)
            continue;
        for (uint32_t i = 0; i < param_6; ++i)
            if (!word(e + 12 + 4 * i)) {
                entry = e;
                slot = i;
                break;
            }
    }
    if (!entry) {
        entry = static_cast<uint32_t>(FUN_8c0377ca());
        if (!entry)
            return check.returns(3u);
        for (uint32_t at = entry + 12; at < entry + 76; at += 4) put(at, 0);
        slot = 0;
        FUN_8c03780c(gptr<int>(kSmallVQList), static_cast<int>(entry));
        set_half(entry, half(entry) | param_3);
        short bank = 0;
        if (word(kAllocator) != 1) {
            const uint32_t free = frame.top - 0x2C, total = frame.top - 0x28;
            kmytex_GetfreememEx(gptr<uint>(free), gptr<uint>(total));
            bank = word(free) < word(total) ? 1 : 0;
        }
        const undefined4 failed = param_1 ? kmytex_TXalloc2(4, param_5, gptr<uint>(entry + 8), bank)
                                          : kmytex_TXalloc(4, param_5, gptr<undefined4>(entry + 8), bank);
        if (failed) {
            kmytex_VQFreeEntry(static_cast<int>(entry));
            return check.returns(3u);
        }
    }
    put(param_2.a, slot * (param_4 >> 2) * 4 + word(entry + 8));
    put(entry + 12 + 4 * slot, param_2.a);
    set_half(entry, half(entry) | 2);
    return check.returns(0u);
}

// A slot of a Small VQ texture of the kind `param_2` for `param_3`, with flag 1 of `param_1` as
// kmyAllocateArea's flag 1.
undefined4 kmyAllocateSmallVQArea(uint param_1, int param_2, gptr<int> param_3, undefined4 ctx_r11) {
    diff::Check check("kmyAllocateSmallVQArea", 0x8C0375C0u, param_1, param_2, param_3, ctx_r11);
    guest::Frame frame(0x18);
    const uint32_t profile = frame.top - 0x18;
    kmytex_GetSmallVQprofile(gptr<dword>(profile), param_2);
    return check.returns(FUN_8c037624(param_1 & 1 ? 1 : 0, param_3, word(profile), word(profile + 4),
                                      word(profile + 12), word(profile + 16), ctx_r11));
}

// Frees the slot at `param_1` of a Small VQ texture of the kind `param_2`, `param_3` bytes a slot
// and `param_4` slots in an area. An entry without slots in use frees its area and leaves the
// list. Returns 7 when no slot is at `param_1`.
int FUN_8c039b3a(int param_1, uint param_2, uint param_3, uint param_4, uint ctx_r10) {
    diff::Check check("FUN_8c039b3a", 0x8C039B3Au, param_1, param_2, param_3, param_4, ctx_r10);
    uint32_t entry = 0, slot = 0;
    for (uint32_t e = word(kSmallVQList); e && !entry; e = word(e + 4)) {
        if ((half(e) & 0xF000) != param_2)
            continue;
        for (uint32_t i = 0; i < param_4; ++i)
            if (word(e + 12 + 4 * i) && i * (param_3 >> 2) * 4 + word(e + 8) == static_cast<uint32_t>(param_1)) {
                entry = e;
                slot = i;
                break;
            }
    }
    if (!entry)
        return check.returns(7);
    put(entry + 12 + 4 * slot, 0);
    bool used = false;
    for (uint32_t i = 0; i < param_4; ++i)
        if (word(entry + 12 + 4 * i))
            used = true;
    if (!used) {
        const int result = static_cast<int>(kmyFreeArea(word(entry + 8)));
        if (result)
            return check.returns(result);
        FUN_8c039c10(gptr<int>(kSmallVQList), static_cast<int>(entry));
        kmytex_VQFreeEntry(static_cast<int>(entry));
    }
    return check.returns(0);
}

int kmyFreeSmallVQArea(int param_1, int param_2, undefined4 ctx_r10) {
    diff::Check check("kmyFreeSmallVQArea", 0x8C039B00u, param_1, param_2, ctx_r10);
    guest::Frame frame(0x18);
    const uint32_t profile = frame.top - 0x18;
    kmytex_GetSmallVQprofile(gptr<dword>(profile), param_1);
    return check.returns(FUN_8c039b3a(param_2, word(profile), word(profile + 4), word(profile + 16), ctx_r10));
}

// The Small VQ kind of the surface `param_1`, by its size at 12 and its flag 0x80000000 at 24.
undefined4 kmtex_ConfirmSmallVQ(int param_1) {
    diff::Check check("kmtex_ConfirmSmallVQ", 0x8C036CEEu, param_1);
    const uint32_t size = word(static_cast<uint32_t>(param_1) + 12);
    const bool flag = word(static_cast<uint32_t>(param_1) + 24) & 0x80000000u;
    switch (size) {
    case 8:
        return check.returns(flag ? 2u : 1u);
    case 0x10:
        return check.returns(flag ? 4u : 3u);
    case 0x20:
        return check.returns(flag ? 6u : 5u);
    case 0x40:
        return check.returns(flag ? 8u : 7u);
    }
    return check.returns(5u);
}

KMSTATUS kmFreeTexture(PKMSURFACEDESC pSurfaceDesc) {
    diff::Check check("kmFreeTexture", 0x8C033600u, pSurfaceDesc);
    const uint32_t desc = pSurfaceDesc.a;
    if ((word(desc) & 0xFFFF) == 3)
        return check.returns(KMSTATUS(kmyFreeSmallVQArea(static_cast<int>(kmtex_ConfirmSmallVQ(static_cast<int>(desc))),
                                                         static_cast<int>(word(desc + 28)), 0)));
    return check.returns(KMSTATUS(kmyFreeArea(word(desc + 28))));
}

// `param_3` bytes from `param_2` to the frame buffer at `param_1`.
dword kmyCopyToFrameBuffer(int param_1, gptr<undefined8> param_2, uint param_3) {
    diff::Check check("kmyCopyToFrameBuffer", 0x8C037EC0u, param_1, param_2, param_3);
    if (!param_3)
        return check.returns(dword(0xEEEE));
    kmyMemCopy(gptr<undefined8>(static_cast<uint32_t>(param_1) + 0xA5000000u), param_2, param_3);
    return check.returns(dword(0));
}

// `param_3` bytes from `param_2` to the texture memory at `param_1`. With asynchronous copies, from
// 32-byte boundaries and with the interrupts on, the 32-byte blocks go by DMA, with `param_4` as the
// DMA's callback, and the rest by the CPU. Else the CPU copies all, and calls `param_4` after.
dword kmyCopyToTexture(uint param_1, gptr<undefined8> param_2, uint param_3, gptr<undefined> param_4, float arg_fr4,
                       float arg_fr5, float arg_fr6, float arg_fr7, int ctx_r13) {
    diff::Check check("kmyCopyToTexture", 0x8C037F20u, param_1, param_2, param_3, param_4, arg_fr4, arg_fr5, arg_fr6,
                      arg_fr7, ctx_r13);
    if (!param_3)
        return check.returns(dword(0xEEEE));
    const auto copy_all = [&] {
        kmyMemCopy(gptr<undefined8>(param_1 + 0xA4000000u), param_2, param_3);
        if (param_4.a)
            gptr<code>(param_4.a)(0u, param_2, param_3, param_4, arg_fr4, arg_fr5, arg_fr6, arg_fr7);
        return check.returns(dword(0));
    };
    if ((param_1 & 0x1F) || (param_2.a & 0x1F) || ((cpu::sr >> 4) & 15) || !kmyIsEnabledASyncCopy())
        return copy_all();
    const uint32_t blocks = param_3 & ~31u;
    int id = ctx_r13;
    if (blocks) {
        uint32_t handle;
        if (!kmyGetInterruptLevel()) {
            while (!(handle = kmyGetDMAHANDLE(1).a)) {
            }
        }
        else if (!(handle = kmyGetDMAHANDLE(1).a)) {
            return copy_all();
        }
        kmySetTextureDMA(static_cast<int>(handle), param_1, param_2.a, blocks);
        if (param_4.a)
            kmySetDMACallback(static_cast<int>(handle), param_4.a);
        id = static_cast<int>(kmyRequestDMA(gptr<undefined4>(handle)));
        put(0x8C10E978u, static_cast<uint32_t>(id));
    }
    if (param_3 & 0x1F)
        kmyMemCopy(gptr<undefined8>(param_1 + blocks + 0xA4000000u), gptr<undefined8>(param_2.a + blocks),
                   param_3 & 0x1F);
    if (!param_4.a && !kmyGetInterruptLevel() && !kmyIsEnabledASyncCopy())
        while (kmyQueryFinishDMA(id) != 0x101u) {
        }
    return check.returns(dword(0));
}

}  // namespace game
