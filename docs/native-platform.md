# A native platform under the game

The game's own code stays as Ghidra decompiled it, and it keeps its tests against the machine
code. What it calls goes native: Ninja draws into a display list on the host that the Metal
renderer draws, a host frame loop replaces the console's video timing and interrupts, and only the
sound keeps the emulated AICA with its ARM7. This page records what the game needs from the
platform, which the inventories below measured, and the steps to get there.

## What the game uses

Measured on 2026-10-24 over a whole autoplay game (122,000 frames), the two recordings, every menu
item and the other modes.

**Code that runs.** Of the machine code that the game runs, the game itself is 330 KB in 1,563
functions, 76%. Ninja is 45 KB in 240 functions, the SDK (Shinobi, the sound library, the GD-ROM,
the memory card and the controller) 30 KB in 255, and Kamui 26 KB in 185.

**The interface.** The game's code calls 90 Ninja functions (drawing, textures, matrices, the
camera, fog, motions and models), one Kamui function (`kmWaitVBlank`) and 47 SDK functions, of
which 27 belong to the sound library. It does not touch the hardware itself: no register, store
queue or video memory access comes from the game's code. Everything goes through Ninja, Kamui or
the SDK.

**Interrupt context.** The video interrupt (INTEVT 0x320, two a frame) carries:

- Kamui's flow control: the render's start, the display's flip and the render packets.
- The sound library's servers at each vertical blank (`sdSysVsyncServer` and about 50 functions
  under it), which send data to the AICA by G2 DMA.
- Shinobi's timer and real-time clock servers.
- One game function, `FUN_8c06b28c`, which Ninja calls at the end of each render. It sets the fog
  table or runs the fade for the next frame.

The G2 DMA interrupt (0x360) ends the sound library's transfers.

**The PowerVR.** Over the whole game:

- One pass a frame, two frame buffers, one background plane, no render to texture and no copy of
  the frame buffer.
- The opaque and translucent lists. The punch-through list holds only Kamui's dummy sprites for
  the user clip.
- Modifier volumes come, but no polygon has its shadow bit, so they change nothing.
- Blending with source alpha, additive blending and one rare blend with the inverted other colour.
- Fog by the table, which changes often, and a palette.
- Textures in ARGB1555, RGB565 and 4-bit palettes, with VQ, mipmaps, twiddled and stride layouts.

The Metal renderer handles all of it.

## The steps

1. **The display list on the host** (done). Ninja and Kamui write each parameter into the store
   queues at a list's pointer and flush it, as they did for the tile accelerator. Each list's
   pointer now starts in its own range of store-queue addresses, and a flush there adds the
   parameter to the host's list (`port/src/frame.cpp`). The scene renders from those lists at the
   vertical blank, as the console did, and the display shows it at the next one. Kamui's vertex
   buffers, the lists' DMA, the render packets and the renderer's interrupts are gone.
2. **The port's frame clock** (done, `port/src/video.cpp`). The port keeps the console's video
   timing, because the input hooks and the sound's device time depend on it, but owns it: it wakes
   only on the lines where something happens (five a frame instead of 263), answers the timing
   registers, and calls the vertical blank's handlers directly. Shinobi's chain of the event
   (`syChainMasterHandler`) runs at level 6, as Shinobi's video interrupt handler ran it, when the
   game's mask lets level 6 in and Holly's level-6 mask has the event. Holly's status bits stay,
   because Shinobi's boot polls them. The SH-4's interrupt controller and dream-recomp's video
   timing no longer take part. `kmWaitVBlank` waits for the next of these events.
3. **The sound keeps its emulation** (decided). The sound library, the AICA with its ARM7 and the
   G2 DMA stay as they are, on the device time that the frame clock drives. The G2 DMA's end is the
   only interrupt left: 143 in a whole game, against 121,990 vertical blanks.
4. **The dead machinery out.** Kamui's flow control (27 functions: the render packets, the lists'
   DMA, the tile accelerator's set-up and the render's, display's and DMA's callbacks) and the
   tile accelerator's input in the PowerVR's model are gone (done). Their decompiled code is back
   in the generated code and unreachable. What dream-recomp's runtime no longer needs to model
   follows with steps 2 and 3.
5. **Draw records instead of PowerVR blocks.** The renderer draws typed records (done,
   `port/src/render/draw.h`): each draw's list, depth test, blend factors, shading, fog, texture
   format, sampling, clip and vertices, and the frame's target, clip, background, fog, palette and
   punch-through threshold. `translate()` makes them from the decoded parameters and the registers,
   and `Window::on_frame` gets each frame between the two, where a mod reads and changes it. Ninja
   still writes each draw as the tile accelerator's 32-byte parameter. Its helpers can fill the
   records directly next, with the decoded parameters as the reference: both must give the same
   records.
6. **The game's code named and typed**, module by module, through Ghidra's types and names and
   `tools/decomp/convert.py`, starting with the task system and the game's objects.

Each step keeps the game the same: `tools/port/check.py` with its pictures, and
`IKARUGA_STREAMS`, whose lines give for each render the frame, the registers' hash, video memory's
hash and the decoded display list's hash. Two builds that render the same give the same lines.
