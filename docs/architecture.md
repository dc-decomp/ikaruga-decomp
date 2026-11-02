# Architecture

How the analysis and the port are built, and why: the layers of the game's code, the decisions
behind the analysis and the native port, and the known gaps.

## The game's code and the SDK libraries

Each library is linked as one block. `tools/decomp/layers.py` assigns every function to a layer by
address and lists the library functions that the game's code calls directly. The startup code calls
the game's main loop, `FUN_8C056A20` (gameMain), which is linked between the sound library and the
second block of Shinobi.

| Layer | Functions | Bytes | Functions the game calls |
| --- | --- | --- | --- |
| Game | 1,695 | 363,895 | |
| Ninja | 469 | 122,634 | 123 |
| Kamui | 323 | 40,770 | 0 |
| Shinobi | 930 | 91,208 | 73 |
| Sound | 174 | 24,650 | 42 |
| C runtime | 67 | 11,876 | 7 |

The game draws only through Ninja. Calls through pointers are not counted, so the lists are a
lower bound.

## Decisions

### dream-recomp as a submodule

`dream-recomp/` is a submodule since 2026-10-28, at a commit of the `ikaruga-bringup` branch of
kjyv's fork, as the game-repo template pins it: a clone builds with the dream-recomp that it was
tested with. Upstream merged the branch's first fixes on 2026-10-29, squashed into `51ca276`
together with Rez (pull request 11). The branch is now upstream's `main` and the fixes that are
not merged yet. Until 2026-10-28 it was a symbolic link to `../dream-recomp`, so that a fix
reached this build at once. Now a change to dream-recomp is made in the submodule, pushed to the
fork, and its commit recorded here. Once upstream merges the rest, the submodule moves to
upstream's commit.

### Base address 0x8C010000

Of the 32-bit words in the image, 14,959 point into `0x8C010000` plus the image size, and 4 point
into `0x0C010000`. The code assumes `0x8C010000`.

### A patched SH-4 language, SuperH4:LE:32:katana

`tools/ghidra/build_language.py` copies Ghidra's SuperH4 language and makes nine changes. Each
change fixed wrong output that was measured on this binary.

| Change | Defect in the stock language |
| --- | --- |
| `fschg` also flips `FPSCR_SZ` | Every `fmov` reads `FPSCR_SZ`, but `fschg` flips only the bit in `FPSCR`. The 64-bit pair moves decompile as 32-bit moves. The image has 588 `fschg` in 72 functions. |
| `frchg` swaps `fr0`-`fr15` with `xf0`-`xf15` | `ftrv` reads `xf0`-`xf15`, but `frchg` does not swap the banks. A matrix loaded through the back bank lands in the wrong registers. |
| The tracked set uses `space="ram"` | The stock set uses `space="register"`, where no code is. Its values never apply. The fixed set pins `FPSCR_PR`, `FPSCR_SZ` and `FPSCR_FR` to 0 at each function entry. |
| `fr12`-`fr15` and the FPSCR mode bits are unaffected by calls | Without this, the decompiler assumes that each call can change the FPU mode. It then keeps both widths of each `fmov` after the call. |
| `xf0`-`xf15` are global | The Ninja library keeps the current matrix in the `xf` bank from one call to the next. As plain registers, the decompiler drops each write to them as dead. |
| `xf` is stored pair-swapped, like `fr` | In little-endian mode Ghidra stores `fr` as `fr1 fr0 fr3 fr2 ...`, so that `dr0` is `fr0:fr1`, but `xf` in plain order. Each `xd` pair and each 8-byte move between the banks swapped its halves. |
| `ftrv` and `fipr` address element i at byte offset (i XOR 1) * 4 | Both read vector elements by byte offset, which the pair-swapped layout reorders. `njCalcPointEx` decompiled to `pd->z = xf3*y + xf7*x + xf11 + xf15*z`, and now to `pd->z = xf2*x + xf6*y + xf10*z + xf14`, which matches the disassembly. |
| `pref` is a call to `prefetch(address)` | The stock `pref` has no p-code. A store-queue flush, which sends 32 bytes to the TA or to texture memory, vanished from the C, and the stores before it read as plain memory writes. The C now has 1,051 `prefetch` calls. |
| The calling convention has no double pairs | Katana code is single precision (`-fpu=single`). The stock list also offered `dr4`-`dr10`, which overlap `fr4`-`fr11`, and the decompiler then put a float argument in the wrong half of its pair: `njTranslate(m, x, y, z)` read `x` and `y` both from `fr5`. |

Results of the same analysis with the stock and the patched language:

| Measure | Stock | Patched |
| --- | --- | --- |
| Functions with `extraout_fr` | 192 | 13 |
| Functions with `FPSCR` references | 13 | 7 |
| Functions with `in_xf` | 39 | 0 |
| Lines of C | 154,573 | 133,003 |

Example: `8c0144b0` scales the current matrix by a vector. The stock language decompiles it to one
wrong `return CONCAT44(...)`. The patched language gives six scaled `xd` pairs. A comparison with
the disassembly confirmed each pair.

### Prototypes from `game/sdk.h`

`ApplySdkHeaders.java` parses `game/sdk.h` into the Ghidra project, and gives each function whose
name has a prototype that signature: 527 functions get one. `game/sdk.h` holds the prototypes of
the 524 SDK functions that `game/symbols.tsv` names, and the 134 types that they, `game/types.h` and
`game/globals.tsv` need, as plain C without macros.

SH-4 code calls through the literal pool with `jsr @rN`. `MarkConstantPools.java` therefore types
each pool slot that points at a function as a pointer to that function's signature, so the
indirect call gets the prototype too. The effect on all 4,068 functions:

| Measure | Names only | With prototypes and the fixes above |
| --- | --- | --- |
| `CONCAT44` pair artifacts | 3,122 | 1,863 |
| Untyped `undefined4` parameters | 1,751 | 1,160 |
| Uses of SDK types (`NJS_*`, `KM*`, ...) | 0 | 1,696 |

### Constant literal pools

The image holds the initialized data as well as the code, so the image block must stay writable.
The decompiler then cannot treat a literal-pool word as a constant. `MarkConstantPools.java` types
each slot that a PC-relative `mov.l` or `mov.w` reads, and marks only these slots constant.
Indirect calls through the pool went from 4,865 to 9.

### Function discovery from dream-recomp

`dream-translate discover` finds 5,242 entries and 654 KB of code. Ghidra alone finds 2,708
functions and 385 KB. `SeedFunctions.java` adds the dream-recomp entries to Ghidra, with two limits:

- It skips entries that dream-recomp found as branch targets. These are inside functions.
- It runs after Ghidra's analysis, and it seeds only addresses that no function contains.

The first attempt seeded all entries before the analysis. Ghidra then made 378 switch-case labels
into functions and cut the switch functions short. Functions with `unaff_` went up from 84 to 516.
With the two limits, there are 4 case-label functions and 156 functions with `unaff_`.

Ghidra 12.2 resolves `mov.l @(disp,pc),rN; jmp @rN` during its analysis, before the seeds are
functions. It then follows such a jump, and a `bra` to a shared tail, into the target, and the
function before the target takes its code in. A seed inside a function whose code before the seed
belongs to another function is such a target. `SeedFunctions.java` makes the unconditional jumps to
it call-returns and makes it a function. This splits 8 seeds, `syMemSet2` and `FUN_8c091180`
(ch1BossMotion) among them, and Ghidra 12.2 then finds the same 3,696 functions with the same sizes
as 12.1.4.

### Switch tables from dream-recomp, prototypes from the decompiler

Ghidra lost SHC's absolute switch tables when a case target was seeded as a function before the
dispatcher was analysed. `FUN_8c08fe04` (ch1BossUpdate) decompiled as a 142-byte call through
`PTR_FUN_8c08fea4`, and each of its 27 cases became a function that read the dispatcher's `r12` as
`unaff_r12`. dream-recomp's discovery now writes its tables (`switch_tables` in the `discover`
JSON). `analyze.sh` leaves the case targets out of the seeds, and `ApplySwitchTables.java` writes
each table as a jump-table override on its dispatcher. `FUN_8c08fe04` is now one function of 3,582
bytes with a `switch` over its 27 cases.

`CommitPrototypes.java` decompiles each function without a header prototype, callees first, and
commits the parameters and any non-void return value. Before, the decompiler guessed the arguments
at each call site, so `FUN_8c07ba08` (hpGaugeSet) was called with no argument in one place and with
one in the next. A first version also committed `void` returns, which made 1,408 `extraout_` uses
where callers read `r0`. Without the `void` returns there are 551.

`FixupFlow.java` repairs three defects that the decompiler cannot get past:

- 18 call targets were typed as data. A constant pool before a function was read as an 8-byte value
  whose upper half was the function's first instruction, for example `0x8C08F150`. These are
  disassembled and made functions again.
- A `jmp` to a constant is marked as a call-return. Together with a jump-table override in the same
  function this kills the decompiler process (`FUN_8c08fe04`). Where the process dies, the flow
  overrides in that function are cleared.
- A computed jump to memory without bytes is marked as a call-return. The cache functions jump to
  their own code through the uncached P2 alias (`0xAC0...`), and `syBtExit` jumps into the boot ROM.
  Ghidra 12.2 follows such a jump, finds no instruction and ends the function in `halt_baddata`.
  As a call-return, the jump is a tail call to its target, as in 12.1.4.

| Measure, all functions | Before | After |
| --- | --- | --- |
| `unaff_` uses | 1,253 | 541 |
| `in_r` uses | 1,026 | 1,141 |
| `extraout_` uses | 347 | 551 |
| Unrecovered jump tables | 98 | 95 |

The `in_r` uses that remain are mostly SHC runtime helpers, which take arguments in `r0` to `r3`.

### Relocations for the exception and interrupt vectors

The first run reported 7,244 untranslated call targets, all at `0x8C000000` and `0x8C000600`. The
startup code sets VBR to `0x8C000000` and copies its handlers to VBR+0x000, +0x100, +0x400 and
+0x600. `game/ikaruga.toml` has these four as relocations.

`--suggest-config` also listed a 66-byte copy of `0x0C0CA7F0` at `0x8C00EEE0`, and in a later
run at `0x8C00EE70`. It was added as a relocation at first, and then removed: the bytes are float
constants (0.1, -0.5, -1.0, -1.5) that `kmSetUserClipping` copies into a local array on its stack,
so the address follows the stack. The launcher lists every image copy below the load address as
a candidate, executed or not. Removing it left both gate scenarios identical.

The relocations change the write hash from frame 76 on. Cycles and write counts stay identical in
all 1,797 frames, also with `--mask-segment`. In frame 76, exactly one of 116,118 stores differs:
the interrupted program counter that the handler saves at `0x8C129CB8`. The interpreted handler
saves `0x0C0653D4`, and the translated handler saves `0x0C0653CE`. Both are instruction boundaries
in the same wait loop. The first is after the poll call, and the second is the loop head, which
calls the poll again. The poll has no side effects, so both resume points give the same behavior.
This was checked for frame 76 only. The rolling hash cannot show whether later frames contain a
second difference.

### SDK names

The game links the SDK's libraries statically. Their banners in `1ST_READ.BIN` name 33 builds, for
example Shinobi 2.25, Ninja2 2.01.011 and sd2 2.50.18, all from March and April 2001, of the
Japanese SDK line. The names of their functions and data, 1,717, are in `game/symbols.tsv` and
`game/globals.tsv`, with a source that starts with `sdk-`. Only names and addresses go into the
repository.

### Hand-made names in the same files

Names found by reading the code go into `game/symbols.tsv` (functions) and `game/globals.tsv`
(data), with the source `user`. A `user` row wins over an SDK row at the same address. Ghidra makes a function for a
`user` row only where it already decoded an instruction. Otherwise it makes a label, so a data
name cannot turn data into code. The first hand-made name is `lastTextureDmaId`. A second one,
`queryLastTextureDmaDone`, turned out to be the SDK function `njLoadTextureFinishDMA`.

### The boot ROM supplies the system font

The game's system text needs the console's ROM font. dream-recomp's launcher now takes
`--bios FILE` (commit `8c7e359`) and copies the 2 MB boot ROM into the boot ROM area. The BIOS
calls stay native, so no BIOS code runs. Like the disc, the image is owner data: it is supplied
by the user, it lives in `bios/`, and git ignores it. The gate passes it when it exists.

The image used here is a development box's (HKT-0120, BIOS 1.011). Its font area is complete. The
flash image dumped with it is erased apart from a `KATANA_FLASH` header and has no factory
partition, so the launcher's synthetic flash, set to the disc's region (Japan), is used instead.
A retail console's flash would be the one to supply.

### A boss callback that only a RAM pointer reaches

A release build faulted at the first boss of stage 1 on the untranslated call target `0x0C08EC88`,
called from `FUN_8c066e38` (`taskDrawList`, [game.md](game.md)). That function walks an object list
at `0x8C15C594` and calls each object's callback at +8. `0x0C08EC88` is a hole: no function contains
it, and it starts with a prologue after the previous function's literal pool. Its address is in the
image once, at `0x8C091494`, and dream-recomp's discovery rejected that pointer. `game/ikaruga.toml`
seeds it.

The seed also moved four entries that were fragments: `0x0C083D0C` (a pointer target in the middle
of a call setup, which had cut `fn_0c083c3c` short), `0x0C083D18`, `0x0C08D02C` and the sweep
guess `0x0C08ED34`, which is the second half of the seeded function. Both gate scenarios are
identical with the seed. They do not reach the boss.

Later stages likely have more callbacks of this kind. dream-recomp accepted 8,608 of 14,686
pointer candidates, and the rejected ones that start a function cleanly are worth a review.

### Recorded input replays deterministically

`--record-input` writes the controller state at each vertical blank, before Maple reads it.
`--replay-input` sets the same state at the same point. Replays of `runs/session1.input` (13,180
frames, the post-boss crash) are identical with and without a window and between the development
and the release build. The console clock (`--rtc-seed`) changes the write hash from frame 2 on but
not the game.

The replay did not follow the live session: on the build that the session ran, the replay faulted
at frame 5,363 on a null object callback (`jsr @(r11+0x1B8)` at `0x0C08C69A`), which the live
session passed. The card was new and empty in both runs, and the window, the frame pacing and the
audio output do not write guest state. The cause was not found, and that build no longer exists.
On the build with the switch fix, the replay runs all 13,786 frames without a fault, and from
frame 1,749 on it differs from the build without the fix.

### Native port

The port compiles Ghidra's C as C++ against `port/include/guest.h`, so the code stays close to
what Ghidra prints.

- **Guest memory.** The game keeps 32-bit pointers in its data, and macOS on arm64 reserves the
  low 4 GB of every process. A guest pointer is therefore `gptr<T>`, a 4-byte guest address, so
  structures keep the game's layout. A dereference is a `gref<T>`: main RAM is a host buffer, and
  other memory goes to the hardware layer. Structures and arrays are reached as host references,
  so `.field` and `[i]` work on them.
- **The stack.** Every local with a stack slot lives on a guest stack at the slot's offset.
  Ghidra splits one buffer into several locals, and a callee that writes through a pointer to the
  first reaches the others only there (`gdFsMountG` reads the drive status into two of them).
  Below each frame the port leaves 256 bytes: Ninja's clippers push registers below their frame
  before a call, and a callee's frame there overwrote the saved loop counter, so the clipper
  never ended.
- **Calls through pointers** go through a table of typed thunks, one per function, with the
  arguments where the SH-4 convention puts them: `r4` to `r7`, then the stack, and `fr4` to
  `fr11`. For the 43 functions whose storage differs from the convention's order, such as a
  structure split into stack words with a gap, the converter writes each argument's place into
  `kArgLayouts`. Their thunks read the arguments from there, and a call through a pointer puts
  them there.
- **Ninja runs on an SH-4 interpreter** (`port/src/sh4`). Ninja is hand-written assembly. It keeps
  the current matrix in the FPU's back bank from one call to the next, passes values between its
  routines in register pairs, returns results in several registers and pushes registers below
  its frame. Ghidra's C loses all of that: compared with the machine code (`IKARUGA_SH4_TEST=1`),
  the C of `njTranslate`, `njRotateX` to `njRotateZ` and `njInvertMatrix` computed wrong
  matrices, and stage 1 drew NaN vertices. The interpreter runs the 470 Ninja functions and the
  SHC runtime's helpers from the game's machine code (`port/interpreted.txt`). It shares memory,
  the back bank, GBR, MACH and MACL with the C++. A call to a function outside the set goes to
  its C++ through the thunk table. 8,000 frames take 32 s headless, so the interpreter costs
  little. Its FPU instructions compute what the SH-4 computes, as dream-recomp's runtime does:
  `fsca` reads the hardware's sine table, and `fipr` and `ftrv` sum exact products and round once.
  With `std::sin` and `std::cos`, a rotation by 90 degrees left -4.4e-8 where the table gives 0.
  Stage 2's turrets take their bullets' size from such a coordinate when it is 0, and with the
  residue every bullet after the first was too small to see.
- **Differential tests** (`IKARUGA_DIFF=N`, `port/src/diff.cpp`). On the first N calls of each
  decompiled function, the interpreter first runs the function's machine code from the same memory
  and arguments. The port keeps the memory that this run leaves, restores the memory from before,
  and runs the C++. If main RAM, video memory or the back bank differ after it, the port names the
  function and the words, and the first call that the two runs made with different arguments. The
  machine code's run drops register writes, but for video memory's 32-bit path, which is memory
  that the test saves and restores. It keeps the display-list data it sends to the tile
  accelerator, which must be the same as the C++'s, and time does not pass in it. Native functions
  that draw take part with `diff::Check`, so they are tested on the game's own calls:
  `njDrawTexture2DExSetData` matches its machine code. The test compares memory, not a result in a
  register: a native caller of a native function uses the native result in both runs. A native
  function that returns through `diff::Check::returns` has its result compared with the machine
  code's `r0`, or `fr0` for a float. The native Ninja functions that return a value all do, and
  their results match. A run that would reach the drive or the flash, or that does not return
  within 0.25 s, is abandoned before it takes effect. In the C++ run time passes, but the
  hardware's events and the interrupts wait until it returns, as no time passes in the machine
  code's run. A C++ run that reads registers, sleeps or runs for longer than a frame gets them, and
  a call during which an event happened does not count. Nor does a call of a native function that
  answers from the port's own state, such as the native renderer's scenes. The
  machine code's run calls the C++ of the functions it calls, so a test covers one function's own
  code. A call inside another tested call is not tested, and the report names the functions that
  the run calls only there. `IKARUGA_DIFF_ONLY=ADDR,...` tests only these functions,
  `IKARUGA_DIFF_FROM=FRAME` starts the tests at a frame, such as a later stage's first, and
  `IKARUGA_DIFF_VERBOSE` prints each run with its time. The test cannot find a parameter that
  Ghidra dropped: both runs then lack the same input. The port's table of registers read without a
  value (`IKARUGA_UNKNOWNS=N` lists N of them) shows those.
- **The machine** is dream-recomp's runtime (`port/src/machine.cpp`), wired as dream-recomp's
  launcher wires it: `DcMemory` with its memory map and registers, the scheduler, the SH-4's and
  Holly's interrupt controllers, the timers, the video timing, G2 DMA, Maple with a controller
  and a memory card, and the AICA with its ARM7, mixer and effects DSP. The PowerVR is the port's
  own (below). Main RAM, video memory, sound memory and the boot
  ROM are `DcMemory`'s buffers, which the C++ reaches directly. These replaced the port's own
  hardware layer on 2026-10-22: dream-recomp's are more complete (the effects DSP gives the music
  its reverb, the renderer has fog, the background plane and the framebuffer formats), and both
  are placeholders until the port replaces the SDK's graphics and sound libraries.
- **Time** advances at safe points, which are each register access and each call through a
  pointer: 100 cycles for a register on the system bus, 50 for a call. Sound memory is on the G2
  bus, so an access to it lets 40 cycles pass: the sound library polls the driver's mailbox there.
  The scheduler runs the devices' events that are due.
- **Interrupts** are selected by dream-recomp's SH-4 interrupt controller against SR and delivered
  by the port: delivery does what the stub at VBR+0x600 does. It stores INTEVT where the game
  reads it and calls the handler from the game's table at VBR+0x200, with INTEVT in `r4`. The mask
  goes up before that store: the store to INTEVT is itself a safe point, and taking the same
  interrupt there again ran Kamui's handlers up to five times for one event. The handler's frames
  go below those of the interrupted code. Code on the interpreter keeps its stack pointer in `r15`,
  so delivery lowers the C++ stack pointer to it first. Before, a handler pushed over the frames
  of interpreted code, and with `IKARUGA_INTERPRET_ALL` the boot returned after 22 frames.
- **The window** is the port's own, in SDL3, with the native renderer, controls and sound output
  (`port/src/render`, below).
- **Code without a C++ function** runs on the SH-4 interpreter when a call reaches it inside the
  game's executable. dream-translate ends the code region at `0x8C0C0574`, but the game calls
  code after it: in a replay of `runs/session1.input`, 7 functions from `0x8C0C74F0` to
  `0x8C0C9738`. Skipped, the first of them left the screen black where the rankings should be
  after a game over. The analysis therefore ends the code at `0x8C0CA2F4`, after the literal pool
  of the function at `0x8C0CA2B4`. The SDK's data follows (`_kmtex_CodebookSize` at `0x8C0CA830`).
- **The file system is native** (`port/src/files.cpp`), the first library layer replaced. The
  17 `gdFs` functions that the game calls read the ISO 9660 file system of the GDI that `dcdisc
  chd2gdi` writes (`port/src/disc.cpp`), and a read is complete when it returns. Shinobi's own file
  system, its GD server, the boot ROM's drive commands and the GD-ROM DMA interrupt are no longer
  reached, and the port's model of the drive is gone. Directory tables and handles stay in the
  memory the game passes, in the SDK's layout. Native code in namespace `game` has to spell out
  `std::size_t`: there `size_t` is the guest's 32-bit type, and a path lookup that compared a
  truncated `npos` treated every file as a directory.
- **The controller is native** (`port/src/input.cpp`). Shinobi's peripheral server read each
  port from the Maple device layer at the vertical blank. The native server, called at the same
  point, converts the launcher's controller state into the same records: the buttons, the
  triggers as buttons above 192 and below 160 again, and the stick. At frame 3,000 of an autoplay
  run the records match the Maple path's word for word. The rest of the controller library that
  touches devices is native too: setup, the memory card's screen (accepted, not shown), its
  orientation and the vibration unit (none). The setup is fixed: a controller in port A and a
  memory card with a screen in its first slot. The Maple device layer never starts, and the port's
  model of the Maple bus is gone. The game never reads a record's name or device information.
- **The memory card is native** (`port/src/saves.cpp`, `--saves DIR`). The seven `bu` functions
  that the game calls, and the two unnamed ones that tell whether a file exists and count the free
  blocks, read and write files in a directory. A file holds the image that
  `buMakeBackupFileImage` built, so it is a VMS file. A request is complete when it returns. The
  library's file system no longer runs. On the emulated card, the save after the ending failed and
  the game stopped drawing.
- **Ninja's matrix and trigonometry functions are native** (`port/src/ninja.cpp`): the 21 that the
  game's C calls most, from the matrix stack to `njCalcScreen` and `njCollisionCheckSS`, and
  `njCalcPoint`, `njCalcVector` and `njTan`, with the camera (`njSetCamera`, `njCameraExLookAt`,
  `njRotateZCameraEx`) and the matrix product and push that it uses (`njPostMultiMatrix`,
  `njPushMatrix`), which the differential test checks instead. Each does its machine code's
  arithmetic in the same order: `ftrv`, `fipr` and `fmac` as the interpreter computes them, sine
  and cosine from `fsca`'s table. `IKARUGA_SH4_TEST` runs each against its machine code from 4,000
  random states, with infinities, NaNs and denormals among them, in the game's FPU mode and in the
  default one. They differ only in the payloads of NaNs. Ninja's own code calls only the matrix
  stack, `njGetMatrix`, the translations, `njRotateEx` and `njScaleEx` among them, and each of
  those calls sets the registers it reads afterwards. The other 449 Ninja functions still run on
  the interpreter. Over the whole game it runs about 705,000 instructions a frame, and the chunk
  models with their motions take 92% of them (`IKARUGA_SH4_PROFILE`): `njCnkEasyDrawObject` alone
  takes 43%.
- **Ninja's drawing starts to be native** (`port/src/ninja_draw.cpp`). A native drawing function
  must send the tile accelerator the same store-queue blocks as its machine code, so the display
  lists stay the same. `hw::capture_flushes` records the flushes of a test run instead of sending
  them, and `IKARUGA_SH4_TEST` compares the native function's blocks with its machine code's.
  `njDrawTexture2DExSetData`, which draws the game's text and sprites, is the first: 2,000 random
  strips give the same blocks. The strip starts are native too: the 17 `njStartVertex` functions
  that the game calls, which send a strip's global parameters, and `njEndVertex`. So are the six
  functions that change the strip context's TSP word (`njColorBlendingMode`,
  `njSetkmCnkBlendMode`, `njSetTextureNum`, `njFogMode`, `njTextureFilterMode`,
  `njTextureClampMode`), the start and end of the 2D sprites (`njDrawTexture2DExStart`,
  `njDrawTexture2DExEnd`), the 3D sprites (`njDrawTexture3DEx`, `njDrawTexture3DExSetData`, which
  compute their points as `njCalcPointT3DP` does, and their clips at the near plane,
  `njTextureClip3DP` and `njTextureNearClip3DP`), the 2D lines (`njCalcLine2D`,
  `njDrawLine2DExSetStrip`), the 3D lines (`njDrawLine3DExSetStrip`), the 2D and 3D untextured
  strips (`njDrawPolygon2DExSetData`, `njDrawPolygon`), the projection (`njSetPerspective`,
  `njSetAspect`), the 3D control flags
  (`njControl3D`), the fog table (`njGenerateFogTable3`), the texture list (`njSetTexture`), the
  fog and fade switches (`njFogEnable`, `njFogDisable`, `njFadeEnable`, `njFadeDisable`), the fog
  table, its colour and the fade (`njSetFogTable`, `njSetFogColor`, `njExecuteFade`), the constant
  attributes and material (`njSetConstantAttr`, `njSetConstantMaterial`), Ninja's wait for the
  vertical blank (`kmWaitVBlank`) and the user clip rectangle (`njUserClipping`). So are the
  frame's scene and passes (`njStartDraw`, `njRender`, `njBeginPass`, `njEndPass`,
  `njSetCurrentContext`, `njSetPunchThroughMode`, and the hooks `FUN_8c010910` and `FUN_8c010926`)
  and the starts and ends of the 2D strips, the lines and the 3D strips (`njDrawPolygon2DExStart`,
  `njDrawPolygon2DExEnd`, `njDrawLineExStart`, `njDrawLineExEnd`, `njDrawTexture3DExStart`,
  `njDrawTexture3DExEnd`, `njDrawPolygon3DExEnd`). The differential test runs each on 2,000 of the
  game's calls from the start and 2,000 from stage 4: all match. `njRender` starts a DMA through
  `kmRender`, and `kmWaitVBlank` polls the hardware, so the test cannot run them, and the full
  check shows their frames. The other eight strip starts stay on the interpreter.
- **Ninja's chunk models start to draw natively** (`port/src/ninja_chunk.cpp`). Over the whole
  game, the interpreter ran 645,000 instructions a frame, 92% of them in the chunk models
  (`IKARUGA_SH4_PCS`). Native now: the vertex functions of the Easy, Easy Multi, Simple and Simple
  Multi models for the vertex types that the game draws (`njCnkEasyVertexVD8`,
  `njCnkEasyVertexVND8`, `njCnkEasyVertexVN`, `njCnkEasyMultiVertexVN`, `njCnkSimpleVertexVN`,
  `njCnkSimpleVertexVD8`, `njCnkSimpleMultiVertexVN`), their textured strips (`njCnkEasyTexture`,
  `njCnkEasyTextureClip`, `njCnkEasyMultiTexture`, `njCnkEasyMultiTextureClip`,
  `njCnkSimpleTexture`, `njCnkSimpleTextureDb`, `njCnkSimpleTextureClip`, `njCnkSimpleTextureD8`,
  `njCnkSimpleTextureD8Clip`, `njCnkSimpleMultiTextureDiff`) with their near clips
  (`njCnkEasyTextureNearClip`, `njCnkEasyMultiTextureNearClip`, `njCnkSimpleTextureNearClip`,
  `njCnkSimpleTextureD8NearClip`), the Simple Multi model's environment-mapped strips
  (`njCnkSimpleMultiPolygonDiffEnv`), the Easy model's untextured strips (`njCnkEasyPolygon`,
  `njCnkEasyPolygonClip`, `njCnkEasyPolygonNearClip`) the Easy Multi model's
  (`njCnkEasyMultiPolygon`) and the Simple model's clipped ones (`njCnkSimplePolygonClip`), the
  vertex and polygon lists of the four models (`njCnkEasyVlist`, `FUN_8c019cd0`,
  `njCnkEasyMultiVlist`, `njCnkEasyMultiPlist`, `FUN_8c01ce1a`, `njCnkSimplePlist`,
  `njCnkSimpleMultiVlist`, `njCnkSimpleMultiPlist`), the functions that draw them (`FUN_8c01a23a`,
  `njCnkEasyMultiDrawModel`, `FUN_8c01cf3e`, `njCnkSimpleMultiDrawModel`) and the walk through an
  object tree without a motion (`njCnkTransformObject`, with `njCnkEasyDrawObject`,
  `FUN_8c01cf94`, `njCnkEasyMultiDrawObject` and `njCnkSimpleMultiDrawObject`). The model clip
  (`njCnkModelClip`), the material (`njCnkSetMaterial`) and two functions that set a strip's TSP
  and parameter control words (`njSetkmCnkTextureNum`, `njSetkmColorMode`) are native too, and so
  are the functions that set the lights: the Easy and Simple models' light (`FUN_8c019ca4`,
  `FUN_8c019cb2`, `FUN_8c019cc0`, `FUN_8c01c65c`, `FUN_8c01c66a`, `njCnkSetSimpleLightColor`) and
  the multi-light models' lights, which they switch, place, colour and turn into the view
  (`FUN_8c01a2c8` to `FUN_8c01a3ca`, `FUN_8c029838` to `FUN_8c02993a`). Each computes what its
  machine code computes, with the interpreter's roundings, and writes through the store queues at
  the same points, so the words a block does not write keep what the queue held. The strip
  functions that the polygon lists find in tables get the registers that the machine code passes.
  The differential test runs each on the game's own calls, 2,000 from the start and 2,000 from
  stage 4, and compares memory, the display-list data and the results: all match.
  `njCnkEasyMultiDrawObject` matches on its first 200 calls. The Simple model's clipped strips of both faces are native too
  (`njCnkSimpleTextureDbClip`, with their near clip `njCnkSimpleTextureDbNearClip`), and so is the
  untextured near clip (`njCnkSimplePolygonNearClip`). The near clip of both faces leaves no
  triangle out: it lights one from behind by the sign of its determinant, and it also takes a
  triangle whose third vertex is exactly 1 deep. The other vertex types and strip kinds still run
  on the interpreter, and the native functions call them where the machine code does. Over the
  first 9,000 frames the interpreter runs 343 instructions a frame instead of 689,000, over the
  whole game 74, and 3,000 frames take 4.0 s headless instead of 13.5 s. The full check reaches
  every milestone, and its pictures show the same scenes. With less code on the interpreter, guest
  time passes differently and the game skips fewer frames, so some pictures show a moment later,
  with a slightly different score.
- **Ninja's motions are native** (`port/src/ninja_motion.cpp`): the functions that start a motion
  (`njStartMotionObj`, `njStartMotionEx`), draw an object's tree in it (`njDrawMotionLink`,
  `_njPushPopMotionLink`), get each node's position, angles and scale (`njGetMotionNodeData`,
  `_njGetMotionFloat3`, `_njGetMotionSangle3`) and apply them (`njMotionTransformEx`), and the two
  interpolations that the game uses: Overhauser curves for positions and linear 16-bit angles,
  three or one at a time (`njMotionIntLinearSA1` through `_njGetMotionSangle1`), with the key
  searches for 16-bit and 32-bit frames (`_njGetMotionLinearKeys16`, `_njGetMotionLinearKeys`).
  The shape motions' vertex lists, which blend two keys, are native too (`njSetShapeVlist`), and
  so are the functions that draw a motion with a chunk model's draw function (`FUN_8c016118`,
  `FUN_8c016128`, `FUN_8c016138`, `FUN_8c016148`, `njCnkEasyMultiDrawShapeMotion`) and an empty
  motion callback (`FUN_8c0173dc`). The camera motions are native (`FUN_8c016170`,
  `FUN_8c0162a8`), and so are the shape motions: their state (`njInitShape`, `njSetShapeList`,
  `njSetCurShapeInfoSlot`, `njStartShapeEx`, `njSetNextShapeNodeEx`), the keys of a vertex group
  (`njGetShapeGroupID`), the blend of a model's vertex chunks (`njCalcShapeVlist`) and the walk
  through the object tree (`njCnkDrawShapeMotion`, `_njCnkPushPopShape`). The linear
  interpolation of three floats is native too (`njMotionIntLinearF3`). It passes the frame in fr4
  on without reading it, so its prototype comes from `tools/ghidra/prototypes.txt`. Ninja calls
  the getters, the interpolations and the models' draw function through pointers in the motion
  state, and the native code calls them with the registers that the machine code passes. The
  differential test runs each on 2,000 of the game's calls from the start and 2,000 from stage 4:
  all match. `FUN_8c016118` matches on its first 200 calls. `_njMotionSearchKey` stays on
  the interpreter: only the Bezier interpolation calls it now, and the game does not use that.
- **Ninja's texture management is native** (`port/src/ninja_texture.cpp`): the manage and system
  tables (`njInitTexManage`, `njInitTexSystem`, `njInitTextureEx`) and their searches
  (`njSearchTexManage`, `njSearchTexManageBank`, `njGetTexManageNum`), the entries of a texture
  list (`njEntryTextureManageNum`, `njSearchTextureManageNum`, `njSetTextureManageNum`,
  `njCheckAttrGlobalIndex`, `njLinkTexlist`), the surfaces and their sizes
  (`njCreateTextureSurface`, `njCheckTextureFormatLow`, `njGetTexSurfaceEx`,
  `njSetTextureParamEx`, `ConvertUVnum`, `njGetTextureLevel`, `njTextureSizeAddDummy` and the
  five size functions it picks from), the loads (`njLoadPvrHeader`, `njLoadTexturePart`,
  `njConvertSmallVQToMem`, `njLoadTextureManageNum`, `njLoadTexturePartManageNum`,
  `njLoadTextureFinishDMA`), the releases (`njReleaseTexture`, `njReleaseTextureLN`,
  `njFreeTexManage`), the palettes (`njSetPaletteMode`, `njSetPaletteData`, `njLoadPaletteData`,
  `njSetPaletteBank`, `njCheckPaletteType`), the file names (`njCheckFileName`, with its upper-case
  pass `FUN_8c01327c`), `njSetTextureNumG`, `njSetTextureName`, `njCalcTexture` and
  `njInitTextureErrStat`. Kamui's texture DMA request is native too (`kmySetTextureDMA`,
  `port/src/kamui.cpp`). On the interpreter, a search took about 1,500 instructions. The
  differential test runs each on 2,000 of the game's calls from the start and 2,000 from stage 4:
  all match.
- **Ninja's initialization is native** (`port/src/ninja_init.cpp`): its globals and strip
  contexts (`njInitGlobal`, `njInitContext`, `FUN_8c01098a`), the matrix stack (`njInitMatrix`),
  the 3D state (`njInit3D`), the clip depths (`njClipZ`), the background plane (`njSetBackColor`),
  the size of the vertex buffers (`njCalcVertexBufferSize`) and two setters (`FUN_8c0108f8`,
  `FUN_8c01126a`). The differential test runs each on all of the game's calls: all match but
  `njSetBackColor`. It leaves most of the vertex context that Kamui reads as the stack held it, and
  in the test its machine code finds other stale words than in the game. Its frame is where the
  machine code's is when C calls it, and Kamui's background state is the same as before. The
  display set-up (`FUN_8c0109de`) and the vertex buffers' set-up with Kamui's system
  configuration (`njInitVertexBufferEx`) are native too. Their machine code polls the hardware, so
  the test cannot run them. After the boot, main RAM is the same as before but for the stack, timer
  counts, two saved stack pointers and Kamui's background texture word, which `njSetBackColor`
  takes from the stack.
- **The renderer is native** (`port/src/render`), in place of dream-recomp's renderer, window
  and audio output. It draws with Metal, in an SDL3 window that also reads the keyboard and
  gamepads. `ta.cpp` decodes the tile accelerator's parameter stream into strips with their
  render state. `texture.cpp` decodes textures from video memory, with every mipmap level. Both
  follow the hardware's formats and the SDK's vertex structures.
  `render-compare` decodes dumps from `IKARUGA_DUMP_TA` with them and with dream-recomp's decoder.
  Over 8 dumps from the title to the ending, every strip and 1,200 textures agree. The comparison
  first found two errors in the reference, which dream-recomp's `ikaruga-bringup` branch now fixes
  (`1c29aab`, `3bb2eb1`):
  - It read the intensity of an untextured intensity vertex from word 4, which `KMVERTEX2`
    reserves, instead of word 6. The game mode menu's background strip uses such vertices.
  - It started a 16-bit mipmap chain after one texel of padding instead of three, so its full-size
    level was shifted by two texels. With three, each level is a reduced copy of the next (a mean
    error of 1.6 of 255, against 11 to 21 with one), and a 32x32 chain ends 8-byte aligned.

  The dumps also set the scope. No polygon is affected by modifier volumes, and no render goes
  into a texture.

  `gpu_metal.mm` draws a decoded list with Metal, and the SDL3 window (`window.cpp`) shows it.
  The vertex shader gives clip space w = 1 / (1/w), so the GPU interpolates every attribute
  perspective-correct, as the PowerVR does. The depth buffer holds 1/w scaled into
  0..1, which keeps the order and the ties of the hardware's depth compare. The fragment shader
  does the texture, the four shading instructions, the offset colour, table and vertex fog and the
  punch-through alpha test. A texture is decoded again only when a hash of its bytes in video
  memory changes. The shaders are Metal source (`shaders/geometry.metal`), which the renderer
  compiles when it starts: Xcode's offline Metal compiler is a separate download.
  `render-view DUMP OUT.png` draws a dump without a window and prints the GPU time of a render.
  `tools/port/render_check.py` draws nine dumps from the title to the name entry and compares
  them with kept pictures. Two draws of the same dump give the same picture to the bit.

  Ikaruga runs the PowerVR in autosort mode (`ISP_FEED_CFG` bit 0 clear). For each pixel, the
  hardware blends the translucent polygons from the farthest to the nearest, whatever their order
  in the list. The renderer does the same. Each translucent fragment that passes the depth test is
  shaded and added to its pixel's list in GPU memory. A second pass sorts each list by 1/w, the
  earlier strip first on a tie, and blends the fragments with their own blend factors, at 8 bits a
  channel after each step. A pixel keeps its nearest 64 layers. A render takes 0.9 to 1.7 ms of
  GPU time at 640x480 and 1.5 to 2.6 ms at 1280x960 on an Apple M4 Pro, with up to 1.4 million
  translucent fragments in gameplay. A translucent polygon does not write depth, whatever its ISP
  word says. In list order, stage 4's chapter panel
  lay under clouds far behind it, and with depth writes, glow quads hid the explosions behind them
  under dark rectangles. The first version of the renderer, with SDL3's GPU API and a sort by
  triangle, is in the history as a start for a renderer on other systems: SDL3's GPU API cannot
  write GPU memory from a fragment shader, so it cannot sort per pixel.

  The fragment pool is fixed at four fragments for each pixel, 79 MB at 1280x960. The screen is
  drawn in bands of rows, one after another, and each band's fragments are sorted and blended before
  the next band's fill the pool, as the PowerVR finishes a tile before the next. The shaders count
  what each slice of 16 rows needed, the fragments that did not fit included, and the next render's
  bands take half the pool each. Explosions appear at once, with up to 37 times the fragments of the
  render before, so a render waits for the GPU, and if a band overflowed, it draws again with bands
  from its own counts. Over the whole game, 8 renders draw twice. The wait costs the overlap of the
  CPU and the GPU: 3,000 frames in the window take 27 to 29 s instead of 26 s. A boss's death
  explosion stacks about 240 large fire sprites: 17 screens of translucent area, up to 106 layers in
  a pixel and 19.8 million fragments at 1280x960. It draws in 9 bands in 26 ms of GPU time. Before
  the bands, the pool grew to 39 million fragments, 630 MB, and the render took 33 ms. Apple's tile
  memory cannot hold this: an imageblock has at most 64 bytes a pixel on the M4 Pro, 4 layers of 12
  bytes.

  The window sends a picture to the screen at most every 10 ms, and none while it cannot be seen.
  macOS takes a background window's pictures only at the display's rate, and the wait for each held
  the whole run to 120 frames a second. 3,000 frames in the window took 13 s more than without it,
  and take 7 s more now. The run declares itself an activity the user started, which exempts it from
  App Nap: macOS would otherwise slow its timers while the window cannot be seen. An unthrottled
  run, which nobody watches, also keeps the Mac from sleeping when idle. The window shows only
  frames a render drew. Over the whole game with autoplay, Ikaruga displays a buffer that no render
  drew once, at startup before it sets its framebuffers up, so the renderer does not decode frames
  from video memory. `IKARUGA_RENDER_DEBUG` reports each such vertical blank, and each strip drawn
  without its texture.
- **The native window has its own controls** (`port/src/render`). `controls.cpp` reads the
  keyboard and every gamepad through bindings in SDL3's preference folder, in the format of
  dream-recomp's launcher, whose file seeds ours on a first run. A stick counts past its dead
  zone, rescaled from the zone's edge, and a key on a trigger rises to full over 0.15 s. The F1
  screen (`menu.cpp`) changes the bindings while the game stops, and draws with Core Text into an
  overlay that the renderer blends over the picture. F10 shows the frame rate, F11 writes the last
  display list for `render-view`, and F12 writes a screenshot. `render-view --menu OUT.png` draws
  the menu's main page without a window.
- **Sound output goes through SDL3** (`port/src/render/audio.cpp`). The AICA's samples go to the
  default playback device in blocks of 512. Samples that would make the queue longer than about a
  fifth of a second are dropped and counted, so the sound stays with the picture when the run is
  ahead of the device. dream-recomp's audio library is gone from the port.
- **The PowerVR is the port's own** (`port/src/pvr.cpp`), in place of dream-recomp's PowerVR core
  and channel-2 DMA. Its registers are memory that the renderer reads, and the video timing
  registers go to dream-recomp's video timing. What the game sends to the tile accelerator
  collects into the display list, which `STARTRENDER` draws. The render's end goes to the game at
  once, as the three render-done interrupts. Kamui's DMA queue starts each transfer in
  `kmiLaunchQ`, which is native (`port/src/kamui.cpp`). It copies a display list into the TA or a
  texture into video memory, and runs Kamui's end-of-DMA handler at once. The rest of Kamui is
  still decompiled. The DMA and list-end interrupts are gone: over the whole game, 244,076
  interrupts are taken instead of 856,128, nearly all of them the vertical blank.

  A transfer now ends when it starts. So the end-of-vertex callback comes right after `kmRender`,
  and Kamui starts the render at the next vertical blank, one frame earlier than with
  dream-recomp's DMA times. Textures arrive earlier too. `IKARUGA_STREAMS` compares the renders of
  two builds. Up to render 571, the display lists are the same as before. From the title on, they
  differ: its animation reads the time. At frame 500, video memory is the same but for 9 words of
  the background plane, which Kamui writes into the parameter buffer of each frame. The check
  passes, stages 1 to 4 come one frame earlier, and the pictures show the same scenes with every
  effect: autoplay presses its buttons by frame, so it plays a slightly different game.
- **Kamui's wait for the vertical blank is native** (`kmWaitVBlank_8c037080`, `port/src/kamui.cpp`).
  Kamui's code spins on two callbacks, which do nothing in this game, until its vertical blank
  handler counts the frame: about 50,000 calls a frame, each a safe point. The native wait makes
  the same checks, and between them the clock goes to the next device event, as the SH-4's sleep
  does. 3,000 frames take 13.3 s instead of 18.6 s.
- **Kamui's register access, memory copies and callbacks are native** (`port/src/kamui.cpp`):
  `kmiWriteRegister`, `kmiReadRegister`, `kmyMemCopy`, `kmyZeroMemory`, `kmyRequestCallback` with
  the default callback `kmyiDefaultCallback`, and the state of its DMA handles and native buffers
  (`kmyGetInterruptLevel`, `kmyGetLastTextureDMAID`, `kmQueryFinishLastTextureDMA`,
  `kmyEnableASyncCopy`, `kmySetDMAValue`, `kmyGetDMAValue`, `kmySetDMACallback`,
  `kmyiCheckNativeHandle`, `kmyGetPassInfo`, `kmyGetNativeBufferStatus`). The copies move pairs of
  words where the machine code uses pair moves. The differential test runs each on 2,000 of the
  game's calls from the start and 2,000 from stage 4: all match.
- **Kamui's DMA handles and queues are native** (`port/src/kamui.cpp`): a free handle of a
  channel (`kmyGetDMAHANDLE`), its request and the start of the next transfers (`kmyRequestDMA`,
  `kmiStartupDMA`), the query whether a transfer is done (`kmyQueryFinishDMA`), the display-list
  request (`kmySetTADMA`) and Kamui's event dispatch (`FUN_8c03b3e0`). From a handle to its
  request the interrupts stay masked, with a nesting count, as in the machine code. The dispatch
  sets the interrupt level to 15 before it looks at the event, which the decompiled C left out.
  The differential test runs each on 2,000 of the game's calls from the start and 2,000 from stage
  4: all match. The dispatch runs Kamui's handlers, and the vertical blank's handler
  (`kmiFLWCTRLVsyncCallBackFunc`) writes the background plane through video memory's 32-bit path.
  The two functions that start a transfer cannot be tested.
- **Kamui's user clip is native** (`kmSetUserClipping`, `port/src/kamui.cpp`): a polygon header
  with the clip mode, two dummy sprites and the tile clip object of the rectangle, from Kamui's
  templates, through the store queues. Its parts are built in the frame where the decompiled code
  kept them. The differential test runs it on 2,000 of the game's calls from the start and 2,000
  from stage 4: all match.
- **Kamui's flow control is native** (`port/src/kamui.cpp`): the render packets that carry each
  scene through the tile accelerator, the renderer and the display. Native are the starts and ends
  of the scene and its passes (`kmBeginScene`, `kmBeginPass`, `kmEndPass`, `kmEndScene`,
  `kmiFLWCTRLGetRenderPacket`, `kmiFLWCTRLStartPass`, `kmiFLWCTRLFinalizePass`, `kmySetupTA`,
  `kmySeparatePassInfo`), the launch (`kmRender`, `kmiFLWCTRLLaunchPacket`), the searches for a
  packet and a buffer, the buffers' locks (`kmiFLWCTRLLockNBuf`, `kmiFLWCTRLFindFreeNBuf`,
  `kmiFLWCTRLCheckFreeNBuf`, `kmiFLWCTRLGetFBLockStatus`, `kmiQueryFinishAllPacket`) and the
  checks of a finished packet and of a vertex buffer's overflow (`kmiFLWCTRLCheckFinishPacket`,
  `kmiFLWCTRLCheckVBufOverflow`). So are the packets' way through the hardware
  (`kmiFLWCTRLRotateReadyQueue`, `kmiFLWCTRLStartDMAEntry`, `kmiFLWCTRLStartRenderEntry`,
  `kmiFLWCTRLStartDisplayEntry`, `kmiGuaranteeFFModeProcessOver`) and the handlers of the vertical
  blank, the render's end, the lists' DMA and the render's time-out
  (`kmiFLWCTRLVsyncCallBackFunc`, `kmiFLWCTRLEORCallBackFunc`, `kmiFLWCTRLEOVCallBackFunc`,
  `kmiFLWCTRLEoDMACallBackFunc`, `kmiFLWCTRLCheckTimeOut`). While a set of buffers changes its
  mask, it sets the flag that makes the interrupts defer their frees (`0x8C10C020`, `0x8C10C024`,
  `0x8C10C028`). The decompiled C drops these stores: a store of 0 follows each without a read
  between them, so Ghidra takes them for dead stores. The differential test runs each function on
  2,000 of the game's calls from the start and 2,000 from stage 4: all match. Most calls of the
  vertical blank's handler meet a hardware event, since its writes to the background plane let
  time pass. `kmiFLWCTRLLaunchPacket` starts a DMA and `kmiFLWCTRLStartRenderEntry` meets a
  hardware event at every call, so the test did not run them. A caller's test does not cover
  them: it runs their C++ in both runs.
- **Kamui's renderer, display, palette and fog settings are native** (`port/src/kamui.cpp`): the
  end of a transfer (`kmiCallbackEndOf`), the render's start and settings (`kmyStartRender`,
  `kmiSetupRender`, `kmySetDitherMode`, `kmySetPixelClippingX`, `kmySetPixelClippingY`,
  `kmiGetITPCurrent`), the frame buffers on display (`kmySetDisplayFrameBuffer`), the palette
  (`kmyUpdatePalette`), the fog (`kmSetFogTable`, `kmSetFogDensity`, `kmSetFogTableColor`,
  `kmyUpdateFogParameter`, `kmyWriteFogTable`) and the query of the texture copy's mode
  (`kmyIsEnabledASyncCopy`). The differential test runs each on 2,000 of the game's calls from the
  start and 2,000 from stage 4: all match.
- **Kamui's texture memory is native** (`port/src/kamui_texture.cpp`): the allocator of its areas
  (`kmyAllocateArea`, `kmyFreeArea`, `kmytex_TXalloc`, `FUN_8c039944`, `FUN_8c03997c`) and their
  lists (`kmytex_FBAllocEntry`, `kmytex_FBFreeEntry`, `kmytex_FBEnQueueTop`,
  `kmytex_FBDeQueueEntry`), the slots of the Small VQ textures (`kmyAllocateSmallVQArea`,
  `kmyFreeSmallVQArea`, `FUN_8c037624`, `FUN_8c039b3a`, `kmytex_GetSmallVQprofile`,
  `FUN_8c0377ca`, `kmytex_VQFreeEntry`, `FUN_8c03780c`, `FUN_8c039c10`, `kmtex_ConfirmSmallVQ`),
  the release of a texture (`kmFreeTexture`) and the copies into texture and frame buffer memory
  (`kmyCopyToTexture`, `kmyCopyToFrameBuffer`). The differential test runs each on 2,000 of the
  game's calls from the start and 2,000 from stage 4: all match. `kmyCopyToTexture` starts a DMA,
  so the test cannot run it.
- **Kamui's region arrays and passes are native** (`port/src/kamui.cpp`): the region array of a
  native buffer (`kmyCreateRegionArray`, `kmyGenerateRegionArray`, the tile order
  `kmiVRightArray` and the empty first tile `FUN_8c0380c0`), the passes' words
  (`kmySetPassInfo`, `kmyGeneratePassInfo`, `kmyEnablePassInfoZClear`,
  `kmyDisablePassInfoPreSort`), the tiles and passes of a buffer (`kmySetPassDepth`,
  `kmyGetDisplayRegionSize`, `kmySetSkipCount`), and three small ones (`kmyGetBGPlaneBase`,
  `kmySetRegionArrayCallback`, `kmyConvertAddress64to32`). The differential test runs each on all
  of the game's calls: all match. Video memory, the display list and the PowerVR's registers at
  frame 600 are also the same as with the decompiled code.
- **Kamui's background plane and the renderer's set-up are native** (`port/src/kamui.cpp`): the
  plane's render state and vertices (`kmSetBackGroundRenderState`, `kmSetBackGroundPlane`,
  `FUN_8c035080`), its parameter words (`kmyRegisterBGPlane`, `kmyInitializeBGPlane`,
  `kmySetBGPlane`, `kmySetBackGroundDepth`), the renderer's reset, culling, filters and strip
  buffer (`kmyResetRenderer`, `kmySetCullingValue`, `kmySetVirticalFilterMode`,
  `kmyEnableHolizontalScaler`, `kmySetStripBuffer`), the display's blank and border
  (`kmyBlankScreen`, `kmySetBorderColor`), the palette's mode (`kmySetPaletteMode`) and three
  small ones (`kmyGetRenderPacket`, `kmySetDMAErrorCallback`, `FUN_8c03e180`). The differential
  test runs each on all of the game's calls: all match. Video memory at frame 600 is the same as
  before.
- **Part of Kamui's system configuration is native** (`port/src/kamui.cpp`): the check of the
  configuration (`kmiCheckSysConfigSetting`), the parts of video memory (`kmiCalculateWorkSize`),
  the frame buffers and their handles (`kmiAllocateFB`, `kmyAllocateFrameBuffer`,
  `kmyGetFrameBufferHandle`, `kmyReleaseFrameBufferHandle`, `kmyGetFrameBufferSize`), the surface
  and vertex buffer descriptors (`kmiSetFBSurfaceDesc`, `kmiInitVertexBufferDesc`), the native
  buffers (`kmyAllocateNativeCommandBuffer`, `kmyAllocateAreaWithAddress`,
  `kmyRegisterNativeBuffer`, `kmyiInitializeNativeBuffer`), the display's clip
  (`kmiSetDisplayClipSize`), the vertical blanks a frame waits for (`kmSetWaitVsyncCount`) and
  the free texture memory (`kmGetFreeTextureMem`). The differential test runs each on all of the
  game's calls: all match. Main RAM at frame 100 is the same as with the decompiled code.
- **Kamui's allocator set-up and the flow control's start are native**: the texture memory's
  allocator (`kmyInitAllocator`, `FUN_8c03b0d2`), the areas at fixed addresses (`kmytex_ABSalloc`,
  `kmytex_FBEnQueueEntry`) and the free memory (`kmyGetFreeSize`) in `port/src/kamui_texture.cpp`,
  and the flow control's start (`kmiFLWCTRLInitFLWControl`) in `port/src/kamui.cpp`. The
  differential test runs each on all of the game's calls: all match. The boot calls
  `kmytex_ABSalloc` once, so its splits of a free area at its end and in its middle are not tested.
  Main RAM at frame 100 and video memory at frame 600 are the same as before, but for the clock's
  count of seconds.
- **Kamui's system configuration is native** (`port/src/kamui.cpp`): its entry
  (`kmSetSystemConfiguration`), the native buffers' set-up with their passes' list modes
  (`kmiSetNativeBufferInfo`) and the vertex buffers' banks and lists' rooms
  (`kmiSetVertexBufferBase`), with the machine code's conversions between floats and unsigned
  words. The differential test matches each on its one call. Video memory, the display list and
  the PowerVR's registers at frame 600 are the same as with the decompiled code, and so is main
  RAM at frame 100, but for 15 bytes of dead stack that the decompiled code's locals left.
- **Kamui's device, driver modules and interrupts are native** (`port/src/kamui.cpp`): the device's
  start (`kmInitDevice`), the driver object and its modules (`kmyInitializeDriver`,
  `kmyiInitializeDriverObject`, `FUN_8c03c3b8`, `FUN_8c03c3dc`, `kmyiInitializePVR2Hardware`,
  `kmiInitializeDisplayModule`, `kmiInitFBControl`, `kmyInitializeTA`, `FUN_8c03e220`), the
  PowerVR's set-up (`kmyInitializeHardware`, `FUN_8c03abe0`, `kmyWaitVBlank`), the fog's
  (`kmyInitializeFog`, `kmyInitializeFogTable`), the DMA device's (`kmyInitDMADevice`), the
  callbacks (`kmyInitializeCallback`, `kmyiInitializeCallback`, `kmyRegisterCallback`,
  `kmyRegisterPALEXTCallback`, `kmSetEndOfVertexCallback`, `kmSetEORCallback`,
  `kmSetHSyncCallback`), the interrupts (`kmyInitializeInterrupt`, `kmyiInitializeInterrupt`,
  `FUN_8c03b454`, `kmyConnectInterrupt`, `kmyEnableInterrupt`) and `sbGetVersionString`. The
  differential test runs each on all of the game's calls: all match, `kmyInitializeInterrupt` in a
  run that held the events through its register reads. The machine code of `kmInitDevice` and
  `kmyInitializeHardware` polls the hardware, so the test cannot run them. Main RAM at frame 100 and video memory at frame
  600 are the same as before.
- **Kamui's VGA display mode is native** (the display modes in `port/src/kamui.cpp`): the mode's
  set-up (`kmSetDisplayMode`, `kmyInitializeVGA`, `kmyiSetScreenSize`,
  `kmyGetDisplayParametersInfo`, `kmyGetDefaultFilterMode`), its timing (`FUN_8c038960`,
  `FUN_8c0387c0`, `FUN_8c03871c`, `FUN_8c038f80`), the colour depth (`kmySetDisplayBpp`) and the
  registers that the game sets once (`kmSetHSyncLine`, `kmSetCullingRegister`, `kmSetBorderColor`).
  The differential test runs each on all of the game's calls: all match. `kmSetDisplayMode` polls
  the hardware, so the test does not run it. Main RAM at frame 100 is the same as before, except for
  dead stack. Video memory, the display list and the PowerVR's registers at frame 600 are the same.
  The television modes (`kmyInitializeNTSC`, `kmyInitializePAL`) stay decompiled, because the game
  takes the VGA mode in the port.
- **Kamui's work area size is native** (`kmuCalculateKamuiWorkareaSize` in
  `port/src/kamui_texture.cpp`): the texture allocator's 24-byte entries and 76-byte Small VQ
  blocks for the counts that Ninja's set-up gives. The differential test matches it on the boot's
  two calls. With it, every Ninja and Kamui function that autoplay and the two recordings
  (`runs/port1.input`, `runs/port2.input`) call is native, and the interpreter runs no instruction
  in them (`IKARUGA_SH4_PROFILE`).
- **The soft reset ends the game** (A, B, X, Y and Start, which `pdIsPadReset` reads): the game
  leaves its main loop, unloads Ninja and Kamui, and `syBtExit` ends the run. The port hung in
  Kamui's wait for the render packets (`kmResetRenderer`). The port's time passes only at safe
  points, and a loop in C++ that reads main RAM has none, so the interrupt that ends a packet never
  came. The native `kmiQueryFinishAllPacket` now has a safe point while a packet is at work. The
  game calls it once in autoplay, with no packet at work. Autoplay never takes this path, so
  `tools/port/check.py` now checks it: autoplay presses Start on the title screen while the check
  holds A, B, X and Y.
- **Ninja's and Kamui's end is native**: Ninja's (`njExitSystem` in `port/src/ninja_init.cpp`,
  `njExitTexture` and `njReleaseTextureAll` in `port/src/ninja_texture.cpp`) and Kamui's device
  unload with its modules' unload functions (`kmUnloadDevice`, `kmResetRenderer`, `kmyResetTA`,
  `kmyUnloadDevice`, `FUN_8c03c394`, `kmyiUnloadCallback`, `kmyiUnloadInterrupt`, `FUN_8c03e1be`,
  `kmyUnregisterCallback` in `port/src/kamui.cpp`). The exit path ran 32,314 instructions on the
  interpreter, and now none. The differential test matches `njExitTexture`, `njReleaseTextureAll`,
  `kmyResetTA`, `kmyiUnloadCallback`, `FUN_8c03e1be` and `kmyUnregisterCallback`. The others poll
  the hardware or change the interrupt mask, so main RAM and video memory at `syBtExit` were
  compared with a build that runs the exit path on the interpreter and as decompiled code: the
  same, except for dead stack and the clock's seconds.
- **The display list is on the host** (`port/src/frame.cpp`, [native-platform.md](native-platform.md)):
  each list's pointer starts in its own range of store-queue addresses, and a flush there adds the
  parameter to the host's list instead of the tile accelerator or a vertex buffer. The scene
  renders from those lists at the vertical blank and goes on display at the next one, as before.
  Kamui's vertex buffers, the lists' DMA, the render packets and the renderer's interrupts are no
  longer used: a run takes 6,007 interrupts in 6,000 frames instead of 11,982. Before the switch, a
  build gathered the host's lists beside the old path, and its decoded lists matched the display
  list that reached the tile accelerator on all 121,940 renders of a whole game. After it,
  `IKARUGA_STREAMS` gives the same frame, registers and video memory for every render of a whole
  game as before, and the same decoded display list for all but two. In those two, after a death
  in stage 4, the lives digit shows 1 instead of 2: autoplay puts the lives back at the vertical
  blank, and the frame's fewer register accesses move that point against the game's drawing. The
  full check passes, its pictures are the same, and so is the count of sound samples.
- **The renderer draws typed frames** (`port/src/render/draw.h`): `translate()` turns each decoded
  display list and the registers into draws with plain fields (list, depth test, blend factors,
  shading, fog, texture format and sampling, clip) and the frame's settings (target, clip,
  background, fog, palette). The Metal renderer reads only those, and `Window::on_frame` gets each
  frame before it is drawn, for mods. The nine pictures of `tools/port/render_check.py` are the
  same bytes as before.
- **Kamui's flow control is gone**: the 27 functions of the render packets, the lists' DMA, the
  tile accelerator's set-up and the render's, display's and DMA's callbacks are no longer native,
  and the PowerVR's model has no tile accelerator input. A whole game gives the same
  `IKARUGA_STREAMS` lines as before the removal.
- **The frame clock is the port's** (`port/src/video.cpp`): the console's video timing, but it wakes
  only on the vertical blank's lines, five a frame instead of 263, and it calls the blank's handlers
  directly through Shinobi's chain at level 6. The SH-4's interrupt controller and dream-recomp's
  video timing no longer take part in the frame. dream-recomp maps the timing registers to its own
  timing first, so the port's register path sends them to the frame clock. A whole game gives the
  same `IKARUGA_STREAMS` lines as before, and the same count of sound samples. Only the sound's
  G2 DMA end still comes as an interrupt: 143 in a whole game, where there were 122,144.
- **The game's own types** (`game/types.h`, `tools/ghidra/ApplyGameTypes.java`): the analysis
  parses a header of the game's types after `game/sdk.h`. The task system's functions get
  their prototypes from it, and a fourth column in `game/globals.tsv` types data, such as
  `TaskList[7]` and `PDS_PERIPHERAL *[2]`. The generated code then reads `task->next` and
  `gamePads[0]->press` instead of offsets. A task list calls each task's functions through a
  pointer, and such a call passes every argument register. With the typed prototypes, the lists
  have no r5 to r7 and fr4 to fr7 of their callers to pass on, and pass 0. A whole game gives the
  same `IKARUGA_STREAMS` lines as before. The player's ship (`Player`) and its buttons followed,
  with the same result. `TypeIndexedPools.java` then types a literal-pool slot that holds the
  start of typed data as a pointer to its element, where every use stays inside the data. An
  access by index then reads `players[i]->position` instead of
  `*(int *)(i * 4 + 0x8c1d1548) + 0x60`: all 214 of those for `players` went. A slot whose uses
  leave its data keeps a plain word, as where SHC uses one global's address as the base of its
  neighbours. None of the game's 509 slots needed that.
- **The GPU draws while the game runs** (`port/src/render/gpu_metal.mm`). The renderer waited for
  each render to end before the game went on, to check that the render's translucent fragments fit
  their pool. A profile of a window run at scale 2 showed 46% of the main thread in that wait. Now a
  render goes to the GPU, and the next render, a readback or the display of that render waits for
  it and checks it. A render that overflowed is drawn again, as before, so the pictures stay the
  same. 6,000 frames of autoplay in a window ran at 270 frames a second before and 480 after. A
  run without a window runs at about 840: the sound's emulation (the AICA's mixer and DSP and the
  ARM7) is the largest part of what is left.
- **The real-time clock is native** (`port/src/clock.cpp`): Shinobi's read of the AICA's clock
  registers gives the host's local time, as seconds since 1950. Shinobi converts it into the date
  itself. The port's model of the clock is gone.
- **The boot ROM's services** are the SDK's wrappers around its vectors, replaced natively
  (`port/src/bios.cpp`): the flash is synthetic, and the font comes from a boot ROM image when one
  is given. Without one, macOS's Hiragino Sans draws each glyph that the game asks for, 24 dots
  high (`port/src/render/font.cpp`), so the save file's notice and the main menu's help line show.
  The game copies a glyph into its text texture as ARGB1555, so a dot is on or off. A glyph that the
  port draws goes in as ARGB4444 instead, with how much of each dot the character covers as its
  alpha, so that its edges blend (`port/src/text.cpp`).
  SYSINFO_INIT copies
  the console ID and the flash's factory settings to `0x8C000068`, as the boot ROM does.
  `syVideoInit` reads the broadcast standard there. With only the ID in place, it rejected the
  block and the game drew interlaced TV frames instead of VGA ones.

Making Ghidra's C correct took these changes in the analysis, each found by a run that failed:

| Change | What failed without it |
| --- | --- |
| `ldc` to SR, GBR and VBR are calls (`setSR`, `setGBR`, `setVBR`) | The C lost every change of the interrupt mask, and the port ran with interrupts masked. |
| Return values for tail calls and for functions whose callers use r0 (`FixReturns`, and in the converter) | `gdGdcReqCmd`'s request id never reached the GD file system, so the mount failed. |
| Literal-pool slots and data words that point at functions are typed with the target's signature (`TypeFunctionPointers`) | `gdFsInit2Ovl` passed its fifth argument as the first, and the GD driver table was empty. |
| Calls through unknown pointers pass r4 to r7 and fr4 to fr7 (`OverrideIndirectCalls`) | The GD driver's read lost its buffer argument, which the caller passed on in r6. |
| Each call of a variadic function passes r4 to r7 and the stack words that its format takes (`OverrideIndirectCalls`) | SHC passes `sprintf`'s format and values on the stack. With `sprintf`'s own prototype the decompiler took those words as arguments at some calls only, and the main menu's DIFFICULTY and EXTEND lines and the continue countdown printed garbage. Eight words at every call were too many: `FUN_8c071418` writes the CONTINUE text's position into the words above its `sprintf` arguments, and the decompiler dropped those writes. |
| A cast to `float` of a word read from memory, with no conversion behind it, is a bit cast (`ExportDecompiled`) | A float table that Ghidra typed as pointers, read with `fmov.s`, printed as `(float)PTR_DAT_8c0d17e0[i]`, and the C++ converted 1.8 numerically: sprites were drawn at x = 6e10. The same op printed twice, as in `x * x`, is a bit cast at both prints. |
| The SHC runtime's helpers get their registers (`ApplyRegisterPrototypes`): division in r0 and r1, structure copies in r0 to r2 | Every division used the wrong operands and every structure copy did nothing. |
| Hand-written prototypes for the boot ROM wrappers (`tools/ghidra/prototypes.txt`) | Callers dropped the command and parameter block of every GD-ROM request. |
| A seed that undefined data covers is cleared and disassembled (`SeedFunctions`) | The analyzer reads a 4-byte `fmov.s` from a literal pool as 8 bytes, and the data runs over the code after the pool. The stub at `0x8C0A01A0`, which stage 2 calls through a table, was never decompiled and ran on the interpreter. With the fix, 17 more seeds became functions, the stub among them. |
| Functions past the code region are removed (`DropDataFunctions`) | Pointers to the C library's character table were typed as code. |
| A pair move (`fmov` with `FPSCR.SZ` set) swaps the two words of its 8-byte access | Kamui's vertex writer stores a vertex with pair moves, and the TA got every word pair swapped: no textured polygon had its place. |
| The C is printed from the decompiler's tokens, and a cast that moves bits is printed as `pun<T>(x)` (`ExportDecompiled`) | Ghidra prints `(uint)fVar1` for a float moved to an integer register and for a conversion alike. As a C++ cast, 4,900 of them converted the value: `1.0 / (float)DAT_8c10af34` read a float's bits as an integer. |
| A cast in an `if` or `while` condition is printed by the same rules (`ExportDecompiled`) | The printer checked the casts in statements only. Stage 3's ring boss keeps an angle in a slot that Ghidra typed as a float, and tests `(int)param_1[0x18] < 0x8000`. As a C++ conversion, the bits of each small integer gave 0. The boss never left its entrance, moved in and out of view every 256 frames, and the stage waited for its end. The fix added 368 bit casts in 118 functions, 46 of them the game's. |
| A cast's op is the conversion after its operand, not one inside it (`ExportDecompiled`) | In `(float)(*(byte *)p + 0x11000)` the search for the op behind the cast found the byte's zero extension first, which the addition reads, and kept a C conversion. `FUN_8c0bf3c4`, `FUN_8c0b5368` and `FUN_8c07e144` stored 69,636.0 where the machine code stores the integer's bits. 10 casts in 5 functions changed. |
| `fmac`'s result in a variable typed as an integer keeps its bits (`tools/decomp/convert.py`) | Ghidra merged several uses of one register into an `int` in `FUN_8c0aa988`, and the C++ converted -0.0023 to 0 before storing it. |
| A comparison with zero follows the machine code's compare (`ExportDecompiled`, with `dn_zero` and `int_zero` in `guest.h`) | Ghidra prints `x == 0.0` for `fcmp/eq` and for an integer test of a word typed as a float. `fcmp/eq` runs under FPSCR.DN, where an integer in a float slot is a denormal and equals 0. Clang turned `x == 0.0f` into a test of all of x's bits, with or without `-fdenormal-fp-math=preserve-sign`, and `FUN_8c0b5b38` kept 27 where the machine code takes its default. An integer test compares all 32 bits: with the denormal rule there, a small counter in `FUN_8c088eb8` equalled 0, and stage 1's boss came 130 frames early. 25 comparisons are `fcmp/eq`, 14 integer tests. |
| A place on the stack without a variable that Ghidra writes as eight bytes is eight bytes (`tools/decomp/convert.py`) | Ghidra names such a place `stack0x...`, and the converter declared each one a byte. `FUN_8c0c357c`, in the ending, sets a point's x to 0 in a store that Ghidra merged with the z before it, and the x kept what the stack held. |
| `FUN_8c07e144` gets its two stack parameters by hand (`tools/ghidra/prototypes.txt`) | It reads an angle and a word from its callers' stack, where the decompiler found no parameters, and the C++ read 0 for the angle: objects of stages 3 and 4 started at angle 0. It is the only function of the game that reads stack arguments without parameters. |
| An equality test of a signed byte or halfword with a constant from 0x80 up gives the constant the operand's type (`ExportDecompiled`) | The machine code compares the low bits, and C promotes a `char` with its sign: `param_1[0x1c] != 0xff` in `FUN_8c083bf0`, a menu function, was always true, and the C++ called `FUN_8c068280(0xff, 0, 0, 0)` where the machine code does not. It is the only such test in the game. |
| A cast to a value does not take a pointer cast of the same address as its op (`ExportDecompiled`) | `FUN_8c0ae1a8`, in stage 4, counts frames in the halfword at `param_1 + 0x10` and tests `(uint)param_1[0x10] & 8`. The search for the op behind `(uint)` found the cast of that address to `ushort *`, and the C++ converted the word as a float, which gave 0. Every 16 frames the machine code calls `FUN_8c0ae148` twice, from two points beside the object. The C++ always called it once, at the object. 2 casts in 2 functions changed. |
| A cast takes the op of another print of the same expression when no other op is left (`ExportDecompiled`) | Ghidra prints `(float *)param_1[0x29]` twice in `FUN_8c07db84`, in a test and in an assignment, both from one op. The assignment's cast took the op, so the test's cast found none and stayed a C++ conversion. A pointer in a float slot reads as a tiny negative float, which converts to 0. The C++ never followed the links between stage 3's chained objects and drew each link with the angle and length it started with. `FUN_8c0b5578`, in stage 4, tested a pointer the same way. 4 casts in 3 functions changed. |
| A piece of a local array, `PIECE(local_38, 8, 4)`, is a piece of the array on the guest stack, and a 4-byte piece cast to `float` keeps its bits (`piece_base` and `piece_ref` in `port.h`) | A local array is a `garray`, a host object that holds the array's guest address, and a piece of it read that object. In `FUN_8c0c357c`, in the ending, the read went past the object's four bytes, and Clang dropped the eight-byte store that also sets a point's x to 0. The point kept x = 1.0, and the ending's trail moved one unit off. `FUN_8c07e490` tested two points beside an object at offsets that it read as pieces, and converted their bits as integers: the points lay about 2.3e9 away. Stage 5 starts at frame 79,276 now, 10 frames from the original code, where it started 257 frames later. |
| A parameter's C++ type follows its register (`tools/decomp/convert.py`): a word in `r5` that the function uses as a float, or a word in `fr4` that it reads as an integer, is a raw word, and the thunk reads it from its register | `FUN_8c07b5c8` takes a float from a table typed as pointers in `r5`. Declared `float`, its thunk read `fr4`. |
| A piece of a variable that holds a float is printed as `x._4_4f_` (`ExportDecompiled`), which the converter reads as a float | The logo's screen position read `DAT_8c06b090._4_4_` as an integer and drew the logo at x = 1,134,559,232. |
| Argument registers that a function reads without a parameter become parameters (`InputParams`) | `FUN_8c06b02c` reads x and y as `dr4`. The convention has no pairs, so the decompiler showed an unknown input and every caller dropped both arguments. `FUN_8c068da0` passes r7 and fr4 to fr7 on to a draw function, and its sprites lost their positions. The pass repeats, so callers that only pass registers on get them too. It runs again after `TypeFunctionPointers`: a call through a literal-pool slot passes the arguments of the slot's type, and before that `FUN_8c082918` passed no floats on to the function in its slot. |
| Prototypes with custom storage in the argument registers get ordinary storage, with unused parameters filling the gaps and stack words after `r7` (`NormalizeStorage`) | `njCnkSimpleTextureDbNearClip` reads its floats in `fr10` and `fr11`. With custom storage, the pool slots that point at it stayed untyped, and the calls through them lost the vertex pointer in `r7`: Ninja wrote vertices to address 0, and Kamui sent a 500 MB display list. `FUN_8c080578` takes a float in `fr6` and a stack word. Kept custom, its callers passed the float as a word, and the C++ converted 1.0 to 1,065,353,216.0. |
| Undefined bytes that hold a function's address are typed as a pointer to its signature too (`TypeFunctionPointers`) | Ninja reaches its strip functions through a table that no `mov.l` names. Untyped, `njEndVertex` was called without its argument, and the opaque list's write pointer became 0. |
| A structure passed by value goes on the stack (`StackStructParams`) | `kmSetFogTableColor` takes a `KMPACKEDARGB` union. SHC passes it at the stack pointer, the SDK prototype in `r4`. The C read an unknown word, and the fog color was 0. |
| A float in memory that is passed through a code pointer is a float argument (`gfunc::add` in `guest.h`) | `FUN_8c093930` passes a vector's squared length to `njSqrt` through a pointer. As a memory reference it went to `r4`, and `njSqrt` returned 0. |
| A string's label passed through a code pointer passes its address (`gfunc::add` in `guest.h`) | A label is a host array. The pointer call dropped it, and every later argument moved up one place: the main menu printed USER SELECT without DIFFICULTY/, and the score was missing. |
| A structure's array field passed through a code pointer passes its address (`gfunc::add` in `guest.h`) | `scriptStep` calls a stage script record's function with the record's 64 data bytes, a field of `ScriptRecord`. The pointer call passed the bytes by value, and the record function read its data from the wrong place: in stage 1 the game called 0x40000000 and ran data at 0x8C0D48FE. |
| Arithmetic on a structure's array field steps by its elements (`inarray` in `guest.h`) | C lets the array decay to a pointer. The field converted to its guest address as an integer, so `hitInit`'s `hit->shapes + 1` pointed one byte past the first hit shape instead of one shape: the player's hit shape was set up from misaligned bytes, and the port crashed at chapter 1's start. |
| A word that a comparison reads keeps its sign, through its type (`pathAngles` in `game/globals.tsv`) | `FUN_8c09fb10` in chapter 2 tests the camera's angle against 5,000 with the signed `cmp/gt`. Ghidra printed the word as a piece of an 8-byte global, `DAT_8c1d1524._4_4_ < 0x1389`, and a piece converts as unsigned. A negative angle took the other branch, and 234 renders from frame 17,088 drew differently from the machine code. Typed as `int`, the test is signed. No other comparison on a piece is left in the generated code. |
| A sound slot's pan compares as a byte, through its type (`SoundSlot` in `game/types.h`) | `FUN_8c06c8b4` sends an effect's pan and level to the sound driver only when the pan changes. The machine code compares the stored byte and the new pan, both sign-extended. Ghidra typed the stored byte as `undefined`, which is a `uint8_t` in C++, and compared it with `(char)pan`. A pan left of the center never matched, and the port sent both commands again at every frame: a whole autoplay game took 143 sound DMA interrupts, and 128 with the typed slot. The sound hash of `IKARUGA_STREAMS` shows the sound change from frame 6,510, after the first resend at 6,509, and no later batch changes it. The differential test did not see it, because the commands reach the hardware. |
| An equality test of an `undefined` byte global with a negative constant, a character from `'\x80'` up or a `(char)` cast compares bytes (`byte_compares` in `tools/decomp/convert.py`) | Ghidra compares such a byte as a byte and prints 0xff as `-1`. In C++ the byte is a `uint8_t`, and the test never matched. The ranking display `FUN_8c07235c` printed the entries without a place as "0th", where the machine code skips them, and `FUN_8c06f940` let a player's count of continues wrap from 255 to 0, where the machine code stops it at 255. No other site is left in the generated code. |
| A local that the C stores a pair into, `_local_a4 = CONCAT44(z, y)`, is eight bytes (`tools/decomp/convert.py`) | `_X` is `X` at a wider size, and the converter declared each one a word. The name entry after the ending stores a text's y and z as one pair, and the C++ kept only y: the place of each score (Top, 13th) got the stack's old bytes as its depth and was not drawn. `FUN_8c08d804` in stage 1 starts explosions with a vector whose z it stores the same way. |
| A cast to an integer type of a value that float arithmetic makes keeps the float's bits, unless an `ftrc` reads the value (`isBitsOfFloat` in `ExportDecompiled`) | Where Ghidra typed a word as an integer, it printed a float stored into it as `param_1[0xed] = (int)(a + b)`, and the C++ cast truncated the sum. The stage 4 ring boss (`FUN_8c0b0fbc`) grows a value by 0.04375 a frame while it dies. In the C++ the value stayed at 0, and the ring stayed whole, where the machine code breaks it apart. A reanalysis for new names gave `FUN_8c0bc01c` in stage 5 and `FUN_8c0a5428` in stage 3 the same pattern. With an `ftrc` before the store, as in `FUN_8c093e04`, the cast stays a conversion. |
| A C++ `double` in a bit operation is narrowed to a float first (`piece_bits` in `guest.h`) | `FUN_8c06d394` returns a random number as `CONCAT44(x * k - 1.0, 0x3f800000)`. The literal `1.0` makes the expression a C++ double, and its bits are not the float's. SHC's double is single precision. |
| A 64-bit result in an FPU pair comes back in `fr0` and `fr1` too (`return_storage` in the sidecar) | `FUN_8c06d394` returns its float in `fr0`, and Ghidra typed the result as the pair. Its thunk returned it in `r0` and `r1`, where machine code that calls it does not look. |
| Kamui's background-plane setters get their four parameters (`prototypes.txt`), and the converter restores the spills the decompiler drops (`tools/decomp/spills.txt`) | Each setter stores its vertex pointers into a stack array and reads them back through a pointer. Ghidra drops those stores, so the background plane was garbage: red instead of black, with a depth that hid every other polygon. |
| A function that reads a callee-saved register (r8 to r14) before it writes it gets that register as a parameter, `ctx_r12` and so on (`ContextParams`). The interpreter passes what the machine code holds there. | The stage 1 boss's state machine `FUN_8c08fe04` keeps the boss in r12 for its helpers. `FUN_8c091180` animates it from there. As C++ it animated whatever was at address 0, the animation never ended, and the boss never came. |
| A result that the decompiler joins from r1 and r0 becomes 4 bytes in r0 when no caller reads its upper word. A read of r0 after a call to a void function counts as a use of its result (`FixReturns`). | `njCnkModelClip` returns 0 or 1 in r0 and leaves a pointer in r1. Its callers read an unknown value, so no model was ever clipped. The two towers of stage 1 delete themselves only when off screen, so they stayed after the game freed their model bank, and Ninja looped on a vertex count from freed memory: the game hung before the boss. The game's modulo helpers had the same fault, so tests such as `counter % 6` read unknown values. |
| A function that leaves r1, r2 or r3 alone, or saves and restores it, keeps it for its callers (`KeptRegisters`, with the calling conventions `__keep_r1` to `__keep_r1r2r3` that `build_language.py` adds) | `FUN_8c09b634` keeps the object it creates in r3 while `FUN_8c09b60c` fills it in, then returns r3. The convention kills r3 at a call, so the C++ returned an unknown value. Callers of `__divls` keep values in r2 and r3, which it saves on the stack. |
| `lds` to FPSCR is a call, `setFPSCR` (`build_language.py`), and the port sets the host FPU's rounding and denormal mode from it (`cpu::set_fpscr`). Reads of FPSCR's fields give the port's FPSCR. | `syHwInitSub` sets FPSCR to `0x00040000`: round to nearest, denormals flushed. Without the call, the C++ ran in the mode that dream-recomp's reset leaves, round toward zero. Over 1,500 frames stage 1's scroll fell 0.12 short and stopped at 2,799.90 in the cloud section, where the stage script waits for 2,800. No enemy came, and nothing could be hit, until a later event started the scroll again. |
| `fmac` is a call, `fmac(a, b, c)`, which rounds once (`std::fmaf`). The game code is compiled with `-ffp-contract=off`, so a multiply and an add round twice, as on the SH-4. | Clang fused 537 multiplies and adds into single operations. With this and the rounding mode, seven functions no longer differ from the machine code in the last bit of a float. A scroll position that the stage script waits for can depend on that bit. |
| Floating literals are single precision (`1.0f`), except in the 4 functions of the C library that switch the FPU to double precision (`tools/decomp/convert.py`) | SHC's double is a float. As a C++ double, `1.0` in `x * k + 1.0` kept the sum unrounded, and `FUN_8c07be04` differed in the last bit. |
| An equality test against a denormal literal compares the word's bits (`tools/decomp/convert.py`) | A denormal is an integer in a word that Ghidra typed as a float. `FUN_8c06ff30` tests `param_1[7] == 9.86724e-41`, which the machine code compares as the integer `0x1130F`. Under FPSCR.DN a float compare flushes both sides to 0, so it matches 0 and every other denormal. A denormal literal is also why the game code is compiled without `-frounding-math`: converted at run time instead of compile time, the count 32 in `FUN_8c077794` flushed to 0, and Ninja's texture loop ran 4 billion times. |
| A seed that a function took in through a tail jump is split from it (`SeedFunctions`) | Ghidra 12.2 followed the jump from `FUN_8c06ce2c` into `syMemSet2` and from `FUN_8c090e70` into `FUN_8c091180`, and both callees were lost as functions. |
| A computed jump to memory without bytes is a call-return (`FixupFlow`) | Ghidra 12.2 followed `syCachePurgeAll`'s jump to its P2 alias, found no instruction and ended the function in `halt_baddata`. The port stopped at boot. Five more functions had the same jump. |
| A stack argument that the body uses only through its address is its place on the guest stack (`tools/decomp/convert.py`) | Ghidra 12.2 makes the first value of a variadic function an argument, where 12.1.4 wrote `stack0x00000004`. `textPrintf`, which prints the loading screen's text, took the address of a host variable, and the port stopped at boot. |
| `FUN_8c07e794` takes the length that it starts from in fr4 (`tools/ghidra/prototypes.txt`) | Its caller passes a line's last length, the object's word at 0x1c, where the decompiler found no parameter, so the C++ searched from 0 for the point of the line nearest its target. The search finds the same point from any start, so the game played the same, in more steps. |
| `lensFlareCreate` takes its two callbacks as `TaskFunction`s (`game/types.h`), so it calls `init` with the new task, as the machine code does | Its C called `init` with no argument, so the callback wrote the light's angles to guest address 0x14 instead of the task. The sun's lens flare in the intro of chapter 1 never showed. The differential test found it once it tested the functions that run only inside other tested calls. |
| `textPrintf` is native, as `sprintf` is, and its calls pass x and y in fr4 and fr5, then r4 to r7, the format and its values (`game/types.h`, `OverrideIndirectCalls`). The disc error screen `discErrorScreen` takes its message as a string. | A variadic function reads its values from its caller's stack, where the C does not put them. Ghidra also typed the error screen's parameter as a `char`, which cut the message's address to its low byte. The screen printed garbage for `DISC READ ERROR` and `FILE READ ERROR`. The boot screen did the same for `Ikaruga Ver %s`, which it shows while controller 2 holds A. |

Two of Ghidra's types keep their meaning in `guest.h`. `undefined4` and `undefined8` are raw
words: a float assigned to one keeps its bits, as in the register or memory word that the
variable stands for. Ghidra prints no cast there, because an undefined type takes any value.
`uVar1 = p->z` copies a vertex's z, and as `uint32_t` C++ converted it to an integer.

## Known gaps

- **Shinobi's operand cache functions are native** (`port/src/native.cpp`) and change nothing:
  the port has no cache, and DMA reads and writes guest memory itself. Their machine code masks the
  interrupts, saves the old mask in a stack word and jumps to an uncached copy of itself, which
  restores the mask from that word. The decompiled C called the copy without the word. So
  `syCacheOCWB`, `syCacheOCBI`, `syCacheOCBP` and `syCachePurgeAll` set the mask from what the
  stack held, mostly 0, and turned the interrupts on inside the sound library's critical sections.
  With Ninja's texture functions native, a vertical blank came in there at boot. Its sound server
  started a transfer while the transfer before it still ran, and the game waited for the first one
  for ever. Only the interpreter advances the clock by the instructions it runs, so each native
  function moves the interrupts against the decompiled code.
- **No scene that a run reaches uses the interpreter.** Over the whole autoplay, the interpreter
  runs no instruction: none of the 219 functions on it runs. The same holds for a whole game in
  which a second player joins, loses its ships and continues (`--press` with `2start`), a single
  player's game over and continue, the CONFIG menu, the attract demo and the disc error screen
  (`IKARUGA_SH4_PROFILE`, 2026-10-26). The two-player game calls 30 game functions that autoplay
  does not, and all 30 match their machine code. Scenes that no run reaches, such as the
  `FILE READ ERROR` screen, can still need one of the 219.
- **About 10% of the code region is not decoded.** `dream-translate game` limits the code region
  to the first 722,292 bytes, and the rest of the image is data. In the code region, 86,890 bytes
  are neither instructions nor literal pools, in 624 runs of 16 bytes or more. Runs of the
  reference build find the code in these runs that the game executes.
- **dream-recomp does not build a title without Vulkan.** `runtime/boot/boot_main.cpp` (commit
  `51a148c`, lines 1237 to 1249) uses `present_mode` outside `#ifdef DREAM_WITH_RENDERER`, where
  it is declared. A build without MoltenVK, the Vulkan loader and `glslc` fails there. Report this
  upstream. Do not patch it in the dream-recomp checkout.
- **Some functions read registers that their callers leave.** The C++ cannot repeat these, because
  it has no registers between calls. Ninja's vertex writer stores `fr7` as each vertex's offset
  colour without loading it. `nameEntryDraw`, the name entry after the ending, reads `fr15` before
  writing it only for a player byte other than 0 and 1, which the game never passes. Stage 4's
  `FUN_8c0b0fbc` passes the upper half of `dr14` as a float argument in one branch before it sets
  it. The machine code gets its callers' values there, the C++ gets 0. 16 game functions read FPU
  registers without a value in all, some of them only in a first pass of a loop.
- **A callee that keeps `r1` does not keep it at every call in the C.** `FUN_8c064f1c` keeps the
  flag 0x1000000 in `r1` across its call of `FUN_8c064f04`, which writes only `r0`, `r4` and `r5`,
  and ORs it into a list entry. KeptRegisters gives `FUN_8c064f04` the convention `__keep_r1r2r3`,
  but at this call the decompiler still takes `r1` as killed, and the C++ ORs 0. The branch, for
  an entry with the flags 0x400 and 8, does not run in a whole game. `FUN_8c043bac`, in Shinobi's
  return from an interrupt, reads `r1` after a call the same way.
- **The C++ and the machine code differ only in words without a defined value.** The differential
  test over the whole game (`IKARUGA_DIFF=10` with autoplay, 2026-10-27) finds 1,742 functions that
  match and 9 that differ. The 296 functions that the run calls only inside other tested calls,
  tested alone in four more runs (`IKARUGA_DIFF_ONLY`), add 274 that match and 14 that differ. None
  of the 23 has a value to match:
  - 20 asset loaders, mostly a stage's wrappers of `FUN_8c067bd0`, store words of their record
    that the machine code leaves undefined for an empty list: the stack word `local_2c`, which it
    sets only for a list that is not empty, and its caller's `r10` and `r13`. The two runs hold
    other leftovers there, and the loader reads none of them for an empty list.
  - `FUN_8c088798` leaves the fourth parameter word of the background plane's vertex context
    unset, as `njSetBackColor` does, and Kamui copies the stack's leftover.
  - `FUN_8c0387c0` takes 28 arguments, 24 on the stack, and its caller `FUN_8c038960` passes them
    to it. The machine code's run passes at most 16 stack words to the C++ of a function it calls.

  With the original code on the interpreter, autoplay reaches each milestone within 12 frames of the
  C++: stage 5 at 79,286 for 79,276, the ending at 106,169 for 106,157. The interpreter lets one
  cycle pass per instruction and the C++ none, so the original code slows down in heavy scenes where
  the C++ does not: from frame 39,370, at stage 3's boss, its scroll speed changes every second
  frame only. The score drifts apart too.
- **GBR is unknown at function entry.** SDK code reads its globals through GBR, and 66
  instructions load GBR. These are mostly a local save and restore, so no one global value is
  correct. This affects SDK code, which the port replaces.
- **64-bit FPU pair moves show as `CONCAT44`.** The result is correct but hard to read.
