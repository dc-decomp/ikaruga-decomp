// The console the decompiled game runs on (hw.h), built from dream-recomp's runtime. The devices
// are wired as dream-recomp's launcher wires them (runtime/boot/boot_main.cpp). What differs is the
// CPU: the game is C++ here, not translated code, so the port advances the clock at its safe points
// and delivers the interrupts that the SH-4's controller selects to the game's handlers itself.
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <set>

#include "dream/runtime/aica/aica.h"
#include "dream/runtime/aica/rtc.h"
#include "dream/runtime/holly/g2dma.h"
#include "dream/runtime/holly/sysblock.h"
#include "dream/runtime/maple/maple.h"
#include "dream/runtime/pvr/core.h"
#include "dream/runtime/sh4/dmac.h"
#include "dream/runtime/system.h"
#include "diff.h"
#include "hw.h"

namespace trace {
void print_stack(std::FILE* out);
}

namespace hw {

std::map<uint32_t, uint64_t>& interrupt_counts();

namespace {

// The serial port Katana's debug output goes to (SCIF): what the game sends is printed, and the
// status says the FIFO is empty, so a sender never waits.
struct SerialPort final : dream::mem::MmioHandler {
    uint32_t read(uint32_t addr, unsigned) override { return (addr & 0xFFu) == 0x10 ? 0x0060u : 0; }
    void write(uint32_t addr, uint32_t value, unsigned) override {
        if ((addr & 0xFFu) == 0x0C)
            std::fputc(static_cast<int>(value & 0xFFu), stderr);
    }
};

struct Machine {
    dream::System sys;
    dream::pvr::Core pvr{sys.sched, sys.holly, sys.spg, sys.memory};
    // Channel-2 DMA is how Kamui sends display lists and textures.
    dream::sh4::Dmac dmac{sys.memory};
    dream::holly::SysBlock sysblock{dmac, sys.holly, sys.memory, pvr};
    dream::holly::G2Dma g2{sys.sched, sys.holly, sys.memory};
    // Registers the game programs and nothing needs to model: stored, so that the writes neither
    // fault nor get lost.
    dream::mem::RegisterFile g1{0x005F7400u, 0x100 / 4}, pvrdma{0x005F7C00u, 0x100 / 4},
        dma_triggers{0x005F6940u, 0x20 / 4};
    dream::mem::RegisterFile bsc{0xFF800000u, 0x50 / 4}, ubc{0xFF200000u, 0x24 / 4},
        cpg{0xFFC00000u, 0x10 / 4}, rtc{0xFFC80000u, 0x40 / 4};
    dream::mem::NullRegion cache_arrays;
    SerialPort scif;
    // Maple's registers: the controller and the memory card are native (input.cpp, saves.cpp), and
    // nothing starts a transfer.
    dream::mem::RegisterFile maple{dream::maple::Bus::kBase, (dream::maple::Bus::kEnd - dream::maple::Bus::kBase) / 4};
    dream::maple::ControllerState pad;
    dream::aica::Aica aica{sys.sched, sys.holly, sys.memory};
    // The clock's registers: the game reads the time natively (clock.cpp).
    dream::mem::RegisterFile aica_rtc{dream::aica::Rtc::kBase, (dream::aica::Rtc::kEnd - dream::aica::Rtc::kBase) / 4};
    std::vector<std::function<void()>> vblank_hooks;
    std::vector<std::function<void(const std::vector<uint32_t>&)>> render_hooks;
    Stats stats;
    int tick_depth = 0;

    Machine() {
        guest::ram = sys.memory.ram();
        auto& mem = sys.memory;
        mem.map_mmio(dream::pvr::Core::kRegBase, dream::pvr::Core::kRegEnd, &pvr);
        mem.map_mmio(dream::pvr::Core::kFifoBase, dream::pvr::Core::kFifoEnd, &pvr);
        mem.map_p4(dream::sh4::Dmac::kBase, dream::sh4::Dmac::kEnd, &dmac);
        mem.map_mmio(dream::holly::SysBlock::kBase, dream::holly::SysBlock::kEnd, &sysblock);
        mem.map_mmio(0x005F6940u, 0x005F6960u, &dma_triggers);  // SB_PDTNRM .. SB_G2DTEXT
        mem.map_mmio(0x005F7400u, 0x005F7500u, &g1);
        mem.map_mmio(dream::holly::G2Dma::kBase, dream::holly::G2Dma::kEnd, &g2);
        mem.map_mmio(0x005F7C00u, 0x005F7D00u, &pvrdma);
        mem.map_p4(0xF0000000u, 0xF6000000u, &cache_arrays);  // the UTLB's arrays are DcMemory's
        mem.map_p4(0xFF800000u, 0xFF800050u, &bsc);
        mem.map_p4(0xFF200000u, 0xFF200024u, &ubc);
        mem.map_p4(0xFFC00000u, 0xFFC00010u, &cpg);
        mem.map_p4(0xFFC80000u, 0xFFC80040u, &rtc);
        mem.map_p4(0xFFE80000u, 0xFFE80028u, &scif);
        mem.map_mmio(dream::maple::Bus::kBase, dream::maple::Bus::kEnd, &maple);
        mem.map_mmio(dream::aica::Aica::kRegBase, dream::aica::Aica::kRegEnd, &aica);
        mem.map_mmio(dream::aica::Rtc::kBase, dream::aica::Rtc::kEnd, &aica_rtc);
        sys.spg.on_vblank_out = [this] {
            for (auto& h : vblank_hooks)
                h();
        };
        pvr.on_render = [this](const std::vector<uint32_t>& stream) {
            for (auto& h : render_hooks)
                h(stream);
        };
        // IKARUGA_UNMAPPED: the functions active at the first access to each address that nothing
        // maps. The first one is usually where a run went wrong.
        if (std::getenv("IKARUGA_UNMAPPED"))
            mem.on_unmapped = [this](uint32_t addr, unsigned size, bool is_write) {
                if (!unmapped_seen.insert(addr).second)
                    return;
                std::fprintf(stderr, "unmapped %s of 0x%08x (size %u) at frame %llu\n", is_write ? "write" : "read",
                             addr, size, static_cast<unsigned long long>(sys.spg.frames()));
                trace::print_stack(stderr);
            };
    }
    std::set<uint32_t> unmapped_seen;
};

Machine& m() {
    static Machine machine;
    return machine;
}

// Delivers what the SH-4's interrupt controller selects against SR. A handler clears its source
// through the registers; the bound stops one that does not.
void deliver() {
    for (int n = 0; n < 8; ++n) {
        const auto selected = m().sys.intc.select(cpu::sr);
        if (!selected)
            return;
        const uint64_t before = m().stats.interrupts;
        interrupt(selected->intevt, static_cast<int>(selected->priority));
        if (m().stats.interrupts == before)
            return;
    }
}

// Where a store-queue flush lands: through the UTLB with MMUCR.AT set, otherwise through QACR.
uint32_t flush_destination(uint32_t address) {
    auto& mem = m().sys.memory;
    uint32_t pa = 0;
    if ((mem.read32(0xFF000010u) & 1u) && mem.utlb().translate(address, pa))
        return pa;
    return (mem.qacr((address >> 5) & 1) << 24) | (address & 0x03FFFFE0u);
}

}  // namespace

// ---- memory ------------------------------------------------------------------------------------

uint32_t read(uint32_t a, unsigned size) {
    diff::on_access();
    tick(100);
    auto& mem = m().sys.memory;
    return size == 1 ? mem.read8(a) : size == 2 ? mem.read16(a) : mem.read32(a);
}

void write(uint32_t a, uint32_t v, unsigned size) {
    diff::on_access();
    if (diff::g_shadow)
        return;
    tick(100);
    auto& mem = m().sys.memory;
    if (size == 1)
        mem.write8(a, static_cast<uint8_t>(v));
    else if (size == 2)
        mem.write16(a, static_cast<uint16_t>(v));
    else
        mem.write32(a, v);
}

uint32_t vram32_read(uint32_t offset, unsigned size) { return read(0xA5000000u + offset, size); }
void vram32_write(uint32_t offset, uint32_t value, unsigned size) { write(0xA5000000u + offset, value, size); }

std::vector<SqFlush>* g_capture = nullptr;
void capture_flushes(std::vector<SqFlush>* into) { g_capture = into; }

void prefetch(uint32_t address) {
    if (address < 0xE0000000u || address >= 0xE4000000u)
        return;
    const uint32_t dest = flush_destination(address) & 0x1FFFFFFFu;
    if (g_capture) {
        SqFlush f{address, {}};
        std::memcpy(f.words, guest::store_queues() + (address & 0x20u), 32);
        g_capture->push_back(f);
        return;
    }
    if (diff::g_shadow && dest >= 0x10000000u && dest < 0x14000000u)
        return;  // a shadow keeps to memory: its display list would be drawn twice
    ++m().stats.sq_flushes;
    auto& mem = m().sys.memory;
    const uint8_t* q = guest::store_queues() + (address & 0x20u);
    for (uint32_t i = 0; i < 8; ++i) {
        uint32_t w;
        std::memcpy(&w, q + 4 * i, 4);
        mem.sq_write32((address & ~0x1Fu) + 4 * i, w);
    }
    mem.sq_flush(address);
}

// ---- time and interrupts -----------------------------------------------------------------------

uint64_t now() { return m().sys.ctx.cycles; }

void tick(uint32_t cycles) {
    Machine& mm = m();
    if (mm.tick_depth > 4 || diff::g_shadow)
        return;
    ++mm.tick_depth;
    mm.sys.ctx.cycles += cycles;
    if (mm.sys.ctx.cycles >= mm.sys.sched.next_deadline()) {
        ++mm.stats.events;
        mm.sys.sched.advance_to(mm.sys.ctx.cycles);
    }
    deliver();
    --mm.tick_depth;
}

// The SH-4's sleep: nothing happens until the next event.
void sleep() {
    Machine& mm = m();
    const uint64_t next = mm.sys.sched.next_deadline();
    if (next != dream::sched::kNever && next > mm.sys.ctx.cycles)
        mm.sys.ctx.cycles = next;
    tick(0);
}

void raise(int bank, int bit) { m().sys.holly.raise(static_cast<dream::holly::Irq>(bank * 32 + bit)); }

void clear(int bank, uint32_t bits) {
    for (int i = 0; i < 32; ++i)
        if (bits & (1u << i))
            m().sys.holly.clear(static_cast<dream::holly::Irq>(bank * 32 + i));
}

void report_interrupts(std::FILE* out) {
    for (const auto& [evt, n] : interrupt_counts()) {
        const uint32_t handler = guest::load<uint32_t>(cpu::vbr + 0x200u + ((evt - 0x200u) >> 3));
        std::fprintf(out, "  interrupt 0x%03x x%llu, handler 0x%08x\n", evt, static_cast<unsigned long long>(n),
                     handler);
    }
    const auto& h = m().sys.holly;
    std::fprintf(out, "  ISTNRM %08x ISTEXT %08x ISTERR %08x, IML6 %08x IML4 %08x IML2 %08x, SR %08x\n", h.istnrm,
                 h.istext, h.isterr, h.iml6nrm, h.iml4nrm, h.iml2nrm, cpu::sr);
}

// ---- devices -----------------------------------------------------------------------------------

uint64_t frames() { return m().sys.spg.frames(); }
Stats& stats() { return m().stats; }

dream::System& system() { return m().sys; }
dream::pvr::Core& pvr() { return m().pvr; }
dream::aica::Aica& aica() { return m().aica; }
dream::maple::ControllerState& pad() { return m().pad; }
void on_vblank(std::function<void()> hook) { m().vblank_hooks.push_back(std::move(hook)); }
void on_render(std::function<void(const std::vector<uint32_t>&)> hook) {
    m().render_hooks.push_back(std::move(hook));
}

void report_unhandled(std::FILE* out, size_t limit) {
    const auto records = m().sys.memory.faults().records();
    std::fprintf(out, "unmapped accesses: %zu addresses\n", records.size());
    for (size_t i = 0; i < records.size() && i < limit; ++i)
        std::fprintf(out, "  %s 0x%08x size %u x%llu\n", records[i].write ? "write" : "read ", records[i].addr,
                     records[i].size, static_cast<unsigned long long>(records[i].count));
}

}  // namespace hw

namespace guest {
uint8_t* vram() { return hw::m().sys.memory.vram(); }
uint8_t* aram() { return hw::m().sys.memory.aram(); }
uint8_t* rom() { return hw::m().sys.memory.bios(); }
}  // namespace guest
