// The port's hardware layer: what the decompiled game and the SDK code inside it reach outside
// main RAM. dream-recomp's runtime models most of the console (port/src/machine.cpp): its memory
// map and registers, the scheduler, both interrupt controllers, the video timing, G2 DMA, the AICA
// with its ARM7 and the timers. The PowerVR is the port's own (pvr.h). The port drives the models
// from safe points, which are every register access and every call through a code pointer, and
// delivers their interrupts to the game's C++.
#pragma once

#include <cstdint>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

#include "guest.h"

namespace dream {
class System;
namespace aica {
class Aica;
}
namespace maple {
struct ControllerState;
}
}  // namespace dream

namespace guest {
uint8_t* vram();
uint8_t* aram();
uint8_t* rom();
uint8_t* store_queues();
}  // namespace guest

namespace hw {

constexpr uint32_t kVramSize = 8u << 20;
constexpr uint32_t kAramSize = 2u << 20;
constexpr uint32_t kRomSize = 2u << 20;
constexpr uint32_t kCpuHz = 200'000'000;

uint32_t read(uint32_t address, unsigned size);
void write(uint32_t address, uint32_t value, unsigned size);

// The 32-bit path into video memory.
uint32_t vram32_read(uint32_t offset, unsigned size);
void vram32_write(uint32_t offset, uint32_t value, unsigned size);

// ---- time and interrupts ----------------------------------------------------------------------

// Guest time in SH-4 cycles. It advances at safe points by the cost of what the safe point is:
// about 100 cycles for a register on the system bus, 50 for a call through a pointer.
uint64_t now();

// Called at every safe point: lets time pass, runs the devices' events that are due and delivers
// the interrupts that SR lets through.
void tick(uint32_t cycles = 50);

// Holly's interrupt status registers (ASIC). Raising a bit takes effect at the next safe point.
enum : int { kNormal = 0, kExternal = 1, kError = 2 };
void raise(int bank, int bit);
void clear(int bank, uint32_t bits);

// ---- the CPU's interrupts ---------------------------------------------------------------------

// Takes an interrupt now if SR allows it: stores INTEVT and calls the handler that the game put in
// its table at VBR+0x200, like the stub at VBR+0x600 does.
void interrupt(uint32_t intevt, int level);

// ---- devices -----------------------------------------------------------------------------------

uint64_t frames();

struct Stats {
    uint64_t interrupts = 0, sq_flushes = 0, gd_sectors = 0, events = 0, video_events = 0;
};
Stats& stats();

// Loads the boot ROM image (owner data, optional): the system font is read from it.
bool load_rom(const std::string& path);

// The disc's tracks, from the GDI that dcdisc chd2gdi writes (port/src/bios.cpp).
bool open_disc(const std::string& path, std::string& error);

// The console's devices, for the window, the sound and the controller (port/src/main.cpp).
dream::System& system();
dream::aica::Aica& aica();
dream::maple::ControllerState& pad();
// Called at each vertical blank, before the peripheral server reads the controller.
void on_vblank(std::function<void()> hook);
// Called with the display list of each render.
void on_render(std::function<void(const std::vector<uint32_t>& stream)> hook);

// The PowerVR (pvr.h): its registers from 0x005F8000, with the palette at offset 0x1000, and the
// renders started.
const uint32_t* pvr_registers();
uint64_t renders();
// Draws a frame's display list: the render hooks get it.
void render(const std::vector<uint32_t>& stream);
// A channel-2 DMA from main RAM to the PowerVR's address space, complete when it returns: to the
// tile accelerator at 0x10000000, or into texture memory at 0x11000000.
void pvr_transfer(uint32_t destination, uint32_t source, uint32_t bytes);

// A test's record of store-queue flushes: while one is set, each flush is appended to it instead of
// going to its destination (IKARUGA_SH4_TEST compares native drawing with Ninja's machine code).
struct SqFlush {
    uint32_t address;  // the store-queue address that pref named
    uint32_t words[8];
};
void capture_flushes(std::vector<SqFlush>* into);

// The registers that nothing models, with their access counts; and the interrupts taken.
void report_unhandled(std::FILE* out, size_t limit);
// IKARUGA_HW_USERS=FILE: writes the registers that each decompiled function accessed.
void write_users();
void report_interrupts(std::FILE* out);

}  // namespace hw
