# ikaruga-decomp

Readable C source for Ikaruga (Dreamcast, Japan, T38706M V1.002). The goal is a native macOS
program built from that source. Metal, a native audio API and native input take the place of the
Dreamcast hardware.

Nothing in this repository contains game data. You supply your own disc image.

## How it works

Ghidra decompiles the boot executable `1ST_READ.BIN` into one C file per function. Ghidra's stock
SH-4 language gets the Dreamcast FPU code wrong, so this repository builds a patched language
first. [docs/progress.md](docs/progress.md) records what was patched, and why.

[dream-recomp](../dream-recomp) supplies two things:

- Its function discovery seeds Ghidra. Ghidra alone finds 2,708 functions, and with the seeds it
  finds 3,918.
- Later, its recompiled build is the reference that each hand-cleaned function is tested against.
  See the plan in [docs/progress.md](docs/progress.md).

## Requirements

- Ghidra 12.2 and JDK 25: `brew install ghidra`
- A checkout of dream-recomp at `../dream-recomp`, built as its README describes, with `dcdisc`
  installed. `dream-recomp/` in this repository is a symbolic link to it.
- Python 3.9 or later

## Steps

1. Put your disc image at `game/disc/Ikaruga (Japan).chd`. Git ignores `game/disc/`.
2. Extract the disc's files into `game/extracted/`, which git also ignores:

   ```sh
   ../dream-recomp/.venv/bin/dcdisc extract "game/disc/Ikaruga (Japan).chd" game/extracted
   ```

3. Analyze and decompile. This takes about seven minutes:

   ```sh
   tools/ghidra/analyze.sh
   ```

   After a change to `tools/ghidra/ExportDecompiled.java` only, `tools/ghidra/analyze.sh --export`
   writes the C again from the analyzed project in about 20 seconds. To check such a change on a
   few functions first, set `EXPORT_ONLY=8c0a9664,...` and run the script into another directory,
   because `--export` deletes `ghidra/out/c` first.

The first run also builds and installs the patched language. To rebuild it by hand, run
`tools/ghidra/build_language.py`. The names of the SDK functions that the game links are committed
in `game/symbols.tsv` and `game/globals.tsv`.

## Reference build

The reference build is dream-recomp's recompiled program. It needs MoltenVK, the Vulkan loader
and headers, `glslc` and SDL3:

```sh
brew install molten-vk vulkan-loader vulkan-headers shaderc sdl3
```

1. Configure a development build. It runs untranslated code in an interpreter:

   ```sh
   cmake -S . -B build -DDREAM_DEV_INTERPRETER=ON -DDREAM_BUILD_TESTS=OFF
   ```

2. Build the launcher:

   ```sh
   cmake --build build --target ikaruga_boot --parallel
   ```

3. Run it in a window:

   ```sh
   build/game/ikaruga_boot \
     --config game/ikaruga.toml \
     --window \
     --bios bios/dc_bios.bin \
     --vmu runs/ikaruga.vmu
   ```

   `--bios` takes your 2 MB Dreamcast boot ROM image. The game draws its system text, such as
   the memory-card notices, with the ROM's font. Without it, that text is blank. The BIOS code does
   not run: dream-recomp answers the BIOS calls natively. Put the image in `bios/`, which git
   ignores. Like the disc, you supply it yourself.

4. To find entry points that discovery misses, run it headless with `--suggest-config`:

   ```sh
   build/game/ikaruga_boot \
     --config game/ikaruga.toml \
     --no-audio \
     --rtc-seed 1 \
     --max-seconds 30 \
     --suggest-config runs/suggest.toml
   ```

Read each suggestion before you add it to `game/ikaruga.toml`. The method is in
`dream-recomp/docs/per-game-bring-up.md`.

5. A release build has no interpreter and stops at the first code that is not translated. Build it
   in its own directory:

   ```sh
   cmake -S . -B build-release -DDREAM_DEV_INTERPRETER=OFF -DDREAM_BUILD_TESTS=OFF
   cmake --build build-release --target ikaruga_boot --parallel
   ```

6. To check that a change did not alter what the game does, capture the regression gate before and
   after the change. A capture takes about 35 seconds and goes to `.regress/`, which git ignores:

   ```sh
   tools/reference/gate.py capture before
   tools/reference/gate.py capture after
   tools/reference/gate.py compare before after
   ```

   The gate runs two scenarios: `boot` without input, and `play`, which presses Start and A into
   stage 1. A changed write hash must be understood before the work continues.

Without input, the game waits on its save-file screen. Press Start to continue.

## Naming functions and data

Names live in two files. Ghidra applies both on each run of `tools/ghidra/analyze.sh`, and the
reference build uses `game/symbols.tsv`.

| File | Content |
| --- | --- |
| `game/symbols.tsv` | Function names |
| `game/globals.tsv` | Data names. Ghidra makes labels from them, never functions. |

Each row is an address, a name and a source, separated by tabs. Either RAM spelling of the address
works (`0x0c013958` or `0x8c013958`).

1. When you know what a function does, add a row with the source `user`:

   ```text
   0x0c013958	queryLastTextureDmaDone	user
   ```

2. Run `tools/ghidra/analyze.sh`. The new name shows in the function's file and at each call.
3. Rebuild the reference build to use the name there too.

The SDK functions' rows have a source that starts with `sdk-`. Where a `user` row and an SDK row
have the same address, the `user` row wins.

## Output

| Path | Content |
| --- | --- |
| `ghidra/out/c/<address>.c` | One decompiled function per file |
| `ghidra/out/c/index.tsv` | Address, name, size and decompile result of each function |
| `ghidra/out/functions.json` | The function list |
| `ghidra/project/` | The Ghidra project. Open it in the Ghidra GUI to rename and retype. |

Ghidra keeps its settings, its cache and the patched language under `ghidra/home/`. The scripts
need no write access outside this directory.

## Native build

The native build is the decompiled game, compiled for the host. The game code is the C++ that
the converter writes from Ghidra's output, not dream-recomp's recompiled program. It draws with
its own renderer in Metal, in an SDL3 window that also reads the keyboard and gamepads and plays
the sound. For the rest of the console's hardware it links dream-recomp's runtime library: the
memory map and registers, the interrupt controllers, the video timing, G2 DMA, Maple and the AICA
with its ARM7. The PowerVR's registers and the tile accelerator's input are the port's own. These
stand in for the hardware until the SDK's graphics and sound libraries are replaced (stage 4 of the
plan in [docs/progress.md](docs/progress.md)).

It needs dream-recomp at `../dream-recomp` with its submodules, SDL3 and Xcode's command line tools.
The reference build's Vulkan tools are not needed.

1. Run the analysis (`tools/ghidra/analyze.sh`). It writes the C and, next to each function, the
   types and storage the converter needs.
2. Convert the C to C++:

   ```sh
   tools/decomp/convert.py
   ```

   This writes `decomp/gen/`. The code there is derived from the game: `decomp/` is ignored here
   and is a local repository of its own. Put a hand-corrected function into `decomp/fix/`, and the
   converter does not generate it again.
3. Write the disc's tracks as a GDI once:

   ```sh
   dcdisc chd2gdi "game/disc/Ikaruga (Japan).chd" game/disc
   ```

4. Build and run:

   ```sh
   cmake -S port -B build-port -DCMAKE_BUILD_TYPE=Release
   cmake --build build-port
   build-port/ikaruga --frames 600
   ```

5. Play in a window, with the boot ROM for the system font:

   ```sh
   build-port/ikaruga \
     --window \
     --bios bios/dc_bios.bin
   ```

   The arrow keys are the stick. Z shoots, X switches polarity, A releases the homing laser and
   Return is Start. A gamepad works by button position. F1 or a gamepad's back button opens the
   controller bindings, which go to `~/Library/Application Support/ikaruga/ikaruga/bindings.txt`
   and start as a copy of dream-recomp's. F10 shows the frame rate, F11 writes the last display
   list for `render-view`, F12 writes a screenshot and Escape quits.
   `--saves DIR` names the memory card, a directory (default `saves`). The game's file in it,
   `IKARUGA_DATA.VMS`, is the image the game writes to a Visual Memory unit, in the VMS layout
   that Dreamcast save tools read.

Options for checking a change without playing:

| Option | Effect |
| --- | --- |
| `--press start@600,a@700-900` | Holds a button for 8 frames from each frame given, or over a range of frames |
| `--screenshot-at 300,650` | Writes the frames drawn at these frames, as `ikaruga-300.ppm` and so on |
| `--wav FILE` | Records the sound |
| `--unthrottled` | Runs the window as fast as the host can |
| `--record-input FILE` | Writes the controller state of each frame where it changes |
| `--replay-input FILE` | Plays a recording back instead of the controls. A recording only replays the same game in the build that made it: the reference build's frames do not line up with the port's. |

To find a function whose C++ does not do what the game's machine code does, run with
`IKARUGA_DIFF=1`. The port then runs the first call of each function twice, once as machine code
on its SH-4 interpreter and once as C++, and prints each function whose results differ:

```sh
env IKARUGA_DIFF=1 \
  build-port/ikaruga \
  --frames 3200 \
  --press start@600,a@700,start@1200,a@1300,start@2000,a@2100
```

`IKARUGA_DIFF_ONLY=0x8c06a7a4` tests one function, `IKARUGA_DIFF_ONLY=0x8c06a7a4,0x8c0196fc` two. `IKARUGA_DIFF_FROM=54500` starts the tests at
frame 54,500, so that the functions get new tests in a later stage. [docs/progress.md](docs/progress.md) describes
what the test can and cannot find.

To check that a change still lets the game reach its milestones, whatever it does to the timing, run
`tools/port/check.py`. It plays the whole game with `IKARUGA_AUTOPLAY`, in about 15 minutes. It
fails when a stage, a boss or the ending does not come by a given frame, when the game stops
drawing, when a boot does not get back what the game saved after the ending, or when the soft
reset (A, B, X, Y and Start) does not end the game. A recording cannot check this: it replays
only in the build that made it. `--frames 18000` checks only the
first two milestones, in two minutes. `--pictures DIR` also takes screenshots a fixed time after
some milestones and compares them with the set that `--keep` kept. The pictures come from the
game, so keep `DIR` outside the repository.

To check a change to the renderer in seconds, run `tools/port/render_check.py`. It draws a set
of display lists with `render-view` and compares each picture with the one that `--keep` kept.
It prints the mean difference and the number of pixels that differ by more than 16 in a channel.
`--make` dumps the set first: nine display lists from the title to the name entry, into
`runs/render-set`. Build `render-view` with `cmake --build build-port --target render-view`.

These variables show what a run does:

| Variable | Effect |
| --- | --- |
| `IKARUGA_PEEK=0x8c1d1428:4,...` | Prints the value at each address when it changes, at each vertical blank. The size is 1 (the default), 2 or 4. A 4-byte value is printed as a float too. |
| `IKARUGA_POKE=0x8c1d13ec=2,...` | Writes these bytes at each vertical blank. `0x8c1d13ec=2` keeps two lives, so a replay gets past a section where the recording died. |
| `IKARUGA_DUMP=2400,3000,DIR` | Writes main RAM at these frames, as `DIR/ram-2400.bin` and so on |
| `IKARUGA_LOG_CALLS=0x8c079384,...` | Prints each call of these functions, with the frame and the argument registers |
| `IKARUGA_WATCH_CALLS=0x8c08fe04,...` | Prints the frame of the first call of each of these functions, also with `IKARUGA_INTERPRET_ALL` |
| `IKARUGA_SHOTS_AFTER=0x8c08fe04+600,...` | In a window, takes a screenshot 600 frames after the first call of the function. The file is the `--screenshot-path` with `-8c08fe04+600` added to its name. |
| `IKARUGA_AUTOPLAY=1` | Plays without a recording: Start and A in turns through the menus, then A held with two lives kept |
| `IKARUGA_UNKNOWNS=60` | Lists 60 of the registers that the C++ read without a value, instead of 30 |
| `IKARUGA_LOG_FILES=1` | Prints each call of the native file system: opens, directories and reads, with their results |
| `IKARUGA_UNMAPPED=1` | Prints the active functions at the first access to each address that nothing maps |
| `IKARUGA_DUMP_TA=950,FILE` | Writes the first display list from that frame on, with video memory and the PVR registers, as the reference build's `--dump-ta` does. `render-view FILE OUT.png` draws it, and `dream_render_view FILE --stats` or `--polygons` lists it. |
| `IKARUGA_CALLED=FILE` | Writes each decompiled function that the run entered, with the frame of its first call and its number of calls. Functions on the interpreter are not in it. |
| `IKARUGA_HW_USERS=FILE` | Writes the registers that each decompiled function read and wrote, and where its store-queue flushes went. Interpreted code counts for the C++ function that called it. |
| `IKARUGA_STREAMS=FILE` | Writes a line for each render: its number, the frame, the display list's words, and hashes of the display list, of the registers the renderer reads and of video memory. Two builds that play the same game give the same lines. A change that moves the timing changes them. |
| `IKARUGA_SH4_PCS=FILE` | Writes, at the end, the interpreted instructions run at each address of the executable: with the symbols, the time each routine takes itself |
| `IKARUGA_SH4_PROFILE=1` | Prints, at the end, the interpreted instructions per function that the C++ called, nested calls included |
| `IKARUGA_INTERPRET_ALL=1` | Runs all game code on the SH-4 interpreter, except the functions that the port replaces (`port/replaced.txt`). The boot is slow. |

How the runtime works is in [docs/progress.md](docs/progress.md), "Native port".

## Layout

| Path | Content |
| --- | --- |
| `tools/ghidra/` | The analysis pipeline and the language patch |
| `tools/symbols/` | The tools for the symbol tables |
| `tools/reference/` | The regression gate for the reference build |
| `tools/decomp/` | The converter from Ghidra's C to C++ for the native build |
| `port/` | The native build's runtime: guest memory, the machine built from dream-recomp's runtime, the boot ROM's services, the SH-4 interpreter for Ninja, the differential tests |
| `decomp/` | The converted game code. Generated, ignored here, a local repository of its own. |
| `game/` | The title config and the symbol tables. `game/extracted/` holds the disc files. |
| `docs/progress.md` | Status, decisions, measurements and the plan |
| `docs/native-platform.md` | What the game needs from the platform, and the steps to a native one |
| `CMakeLists.txt` | Builds dream-recomp and the recompiled title, for the reference build |

## Rules

- Never commit anything from the disc or the console. `game/extracted/`, `ghidra/`, `bios/` and
  `*.chd` are ignored.
  Read `git status` before every commit.
- The decompiled code comes from Sega's copyrighted program. Keep it out of any public remote.
- Never copy Sega's code into this repository, including into comments.