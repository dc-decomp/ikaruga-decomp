# ikaruga-decomp

A native macOS port of Ikaruga, built from a decompilation of its Dreamcast version (Japan,
T38706M V1.002). The game's machine code is decompiled into C++, which the build compiles for the
Mac. A renderer of its own draws the picture with Metal, and SDL3 gives the window, the sound
output and the controllers. The sound chip, its ARM7 and some of the console's other hardware are
still emulated, by dream-recomp's runtime ([Native build](#native-build)).

Nothing in this repository contains game data or Sega's software: no disc image, no boot ROM and
no Katana SDK. You supply the disc image of a copy of the game that you own.

## Why this port

- **A quick reaction.** In fullscreen with VSYNC off, the ship's reaction to a key is on the screen
  16.7 ms after macOS has the key's event (median), and 38 ms with VSYNC on. The port shows each
  picture a frame earlier than the game itself would: the console's PowerVR draws a frame while the
  CPU runs the next, and the port draws it at once. So the reaction is likely quicker than on a real
  Dreamcast too, where it comes two frames (33 ms) after the game reads the controls, before the CRT
  draws it. No Dreamcast was measured for this. Published figures for other versions are few and
  come from a different method, from the button to a CRT: 4 frames (67 ms) handheld and 5 docked for
  the Switch version ([shmups wiki][lag-wiki]), and 2.5 frames for PS4 against 2.85 for Switch in a
  forum post ([shmups forum][lag-forum]). A camera test of the port from the button is still to do,
  and so is one of an emulator.
- **Smooth scrolling.** The display paces the frames. At 120 Hz each picture stays two refreshes,
  99.8% of them in fullscreen with VSYNC on and about 98.8% with VSYNC off. Without such pacing,
  60 frames a second judder on a ProMotion display, as developers report in [Apple's developer
  forum][apple-jitter], where a visible mouse pointer also shook a fullscreen picture. The port
  hides the pointer while the game runs.
  In a short try on the same MacBook, Flycast and Dolphin (with the GameCube version) showed
  stutter and skipped frames in fullscreen.
- **No slowdown.** The Dreamcast and the Switch version slow down in the same places, after a
  boss's destruction fills the screen with an explosion ([The Dreamcast Junkyard][junkyard]), and
  the GameCube version slows down too ([RetroPie forum][retropie]). The port draws every frame of
  the game in time: a whole autoplay game renders at 121,940 of its 122,000 vertical blanks, and
  the others fall at the boot and at changes of scene, such as a chapter's start, never in play.
  Autoplay kills no boss, so a test build with deadly shots played a whole game in which every
  boss explodes: at 2X and 3X every frame fits in its 16.7 ms, the last boss's explosion too. When
  a frame comes late, the frames after it make up the time, so the game and its sound keep their
  speed.
- **Up to 6X the console's resolution**, 3840x2880, averaged down to the window, which smooths
  the edges. Bloom and anisotropic filtering are on the menu's GRAPHICS page.
- **A Mac application.** An app bundle that macOS's Game Mode takes as a game, fullscreen,
  controller bindings, and pacing for ProMotion displays. The PC version on Steam lists only
  Windows in its requirements ([Steam][steam]).
- **Mods and tweaks.** Infinite lives, infinite laser charge, half the bullets and no blinking
  PRESS START on the menu's MODS AND TWEAKS page. The game's code is readable C++, which later mods
  can change.

[Performance](#performance) gives the measurements.

[lag-wiki]: https://shmups.wiki/library/Input_lag
[lag-forum]: https://shmups.system11.org/viewtopic.php?p=1517633
[apple-jitter]: https://developer.apple.com/forums/thread/733033
[junkyard]: https://www.thedreamcastjunkyard.co.uk/2018/06/a-quick-look-at-ikaruga-dreamcast.html
[retropie]: https://retropie.org.uk/forum/post/143791
[steam]: https://store.steampowered.com/app/253750

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
- Python 3.10 or later

## Steps

1. Put your disc image at `game/disc/Ikaruga (Japan).chd`.
2. Extract the disc's files into `game/extracted/`:

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
   because `--export` deletes `ghidra/out/c` first. The saved project does not give quite the C of
   a full run (some functions decompile differently), so use `--export` to look at a change and a
   full run for the C that you convert.

The first run also builds and installs the patched language. To rebuild it by hand, run
`tools/ghidra/build_language.py`. The names of the SDK functions that the game links are committed
in `game/symbols.tsv` and `game/globals.tsv`, and their prototypes and types in `game/sdk.h`.

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
   after the change. A capture takes about 35 seconds and goes to `.regress/`:

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
4. Before you commit, compare a whole game of the native build before and after the change, with
   `IKARUGA_STREAMS` as the native build's section shows. A name or a type changes Ghidra's whole
   analysis. Its guesses of types elsewhere can shift, and with them the C++ of functions far from
   the name.

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
plan in [docs/progress.md](docs/progress.md)). The AICA's final mix keeps one bit of headroom, as
MAME's does. At full master volume, Ikaruga's busy scenes reach twice the 16-bit range, and at
Flycast's level, which is dream-recomp's default, about 150 samples a second clip and distort the
explosions. The largest explosions reach four times the range, and a limiter brings their peaks
under it: its gain falls in the 1.5 ms before a peak and comes back over 150 ms. In a whole game
in which every boss explodes, no sample clips, and 99.9% of the samples pass the limiter as they
are, 1.5 ms later.

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
   Return is Start. A gamepad works by button position. Escape, F1 or a gamepad's back button
   opens the menu, and the game stops while it is open. The mouse works there too: a click on a
   row selects it, a click on a setting's value steps it down or up by the half it hits, and the
   right button goes back. With MOUSE on the CONTROLLER page, the mouse moves player 1's ship while it
   is in play and player 1 plays on the keyboard, within the game's bounds. The ship moves on the
   screen as far as the pointer would, from where the game shows it: from its place at a
   chapter's start, and with the camera when the camera moves. The window holds the pointer
   meanwhile, so macOS neither moves nor shows it, and gives it back for the menu. A press on
   player 1's gamepad gives the ship back to the gamepad until the next key. The left button
   shoots, the right switches polarity and the middle releases the homing lasers. The ship then
   moves faster than the game lets it, like a tweak. The port finds the ship's place through the view
   that the game draws it with: the flat view of most of the game, which the camera's moves do not
   change, or in the camera's sequences, such as a chapter's start, the stage's camera, which stands
   50 units from the ships' plane in stage 1 and 60 in stage 3. Its CONTROLLER page sets the
   bindings of each player, which go to `~/Library/Application Support/ikaruga/ikaruga/bindings.txt`
   and start as a copy of dream-recomp's. Player 1 takes the first gamepad that was plugged in and
   player 2 the second, and the page's GAMEPAD row swaps them. Player 2 has no keys at first. The
   second controller joins when player 2 presses Start during a game, as on the console. A
   scripted run has a second controller only when its presses use one (`2start@3000`). Its GRAPHICS page changes only the picture. Its settings
   go to `graphics.txt` in the same folder:

   | Setting | What it does |
   | --- | --- |
   | RESOLUTION | The size that the game is drawn at, from 1X (640x480) to 6X (3840x2880). The window averages a larger picture over each of its pixels, which smooths the edges as supersampling does. The page shows the size of the picture in the window. `--scale N` sets the resolution for one run. |
   | BLOOM | OFF, LOW or HIGH. The brightest parts of the picture glow. F12's screenshots show the glow. |
   | ANISOTROPIC FILTERING | OFF or 2X to 16X. Textures seen at a slant stay sharper. The camera looks at the stages almost straight on, so the picture changes little: at 16X, 0.5% of the pixels or fewer in the test frames. |
   | FULLSCREEN | ON or OFF. The window's green button and Control-Command-F change it too. |
   | VSYNC | OFF or ON, OFF at first. Off works only in fullscreen: the window server then shows each picture at once, without waiting for the display's refresh. That takes about 21 ms off the latency, and the picture can tear: a horizontal line where two pictures meet, for one refresh. At 120 Hz the two pictures differ by one frame of scrolling, and the line is hard to see. In a window the window server waits for the refresh anyway. |
   | METAL HUD | ON or OFF. Metal's performance HUD in a corner of the window: the frame rate, the frame times and the GPU's time. A scripted run has no HUD. |

   [Performance](#performance) gives what each resolution costs.

   The MODS AND TWEAKS page switches changes to the game, which go to `tweaks.txt`:

   | Tweak | What it does |
   | --- | --- |
   | INFINITE LIVES | A player in the game keeps at least 2 lives. |
   | INFINITE LASER CHARGE | The energy for the homing lasers stays full, so every release fires 12 lasers. In the prototype mode, where the shots spend energy too, it stays at 999. |
   | HALF THE BULLETS | Every second bullet ends in the frame it comes in, so the enemies fire half as many. Fewer bullets also give less energy to absorb. |
   | HIDE PRESS START | The blinking PRESS START on the side of a player who is not in the game goes away. GAME OVER and PLEASE WAIT stay. |

   The tweaks change the game's data at each vertical blank (`port/src/game_tweaks.cpp`), not its
   code. A scripted run (autoplay, `--replay-input`, `--press`, `--frames` or screenshots) does not
   read or write `graphics.txt` and `tweaks.txt` and ignores MOUSE, so the settings do not change a
   check. QUIT in the menu ends the game. F10 shows the frame rate, F11 writes the last display list
   for `render-view` and F12 writes a screenshot.

   At 60 Hz or 120 Hz the display paces the game, so that the scrolling stays smooth. Each frame
   stays on the screen for one refresh at 60 Hz and for two at 120 Hz. The game then runs at 60.00
   frames a second instead of the console's 59.94, and the sound plays 0.1% faster to stay with
   it. A frame that misses its refresh takes the next one, and the frames after it make up the
   time: a frame comes a refresh earlier, or the game runs a frame without showing it. Otherwise
   each late frame would put the game behind the sound, which then breaks up. At other refresh
   rates, and while the window is covered or the menu is open, the game paces itself by its own
   clock, and some frames stay on the screen longer than others. Fullscreen is smoother than a
   window ([Performance](#performance)).

   The build also makes `build-port/Ikaruga.app`, the same program in an app bundle whose
   Info.plist declares it a game. In fullscreen, macOS turns on Game Mode for it, which gives the
   game priority on the CPU and the GPU, and the menu bar shows a game controller. The plain
   `build-port/ikaruga` gets no Game Mode. The bundle's program also reads the disc relative to
   the working directory, so start `build-port/Ikaruga.app/Contents/MacOS/ikaruga` from the
   repository's root, with the same options. `open Ikaruga.app` starts it in `/`, where it finds
   no disc.

   `--saves DIR` names the memory card, a directory (default `saves`). The game's file in it,
   `IKARUGA_DATA.VMS`, is the image the game writes to a Visual Memory unit, in the VMS layout
   that Dreamcast save tools read. As on the console, the game saves on its own only with AUTO
   SAVE on in its options, which a new card has off, at the next change of scene. BACKUP in its
   main menu saves at once. The run prints a line for each load and save of the card.

Options for checking a change without playing:

| Option | Effect |
| --- | --- |
| `--press start@600,a@700-900` | Holds a button for 8 frames from each frame given, or over a range of frames. A `2` before the button, as in `2start@3000`, presses it on a second controller, which the run then has. |
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
frame 54,500, so that the functions get new tests in a later stage. The test cannot run a call
inside another tested call, so the report names the functions that the run calls only there: test
these alone with `IKARUGA_DIFF_ONLY`. [docs/progress.md](docs/progress.md) describes what the test
can and cannot find.

To check that a change still lets the game reach its milestones, whatever it does to the timing, run
`tools/port/check.py`. It plays the whole game with `IKARUGA_AUTOPLAY`, in about 15 minutes. It
fails when a stage, a boss or the ending does not come by a given frame, when the game stops
drawing, when a boot does not get back what the game saved after the ending, or when the soft
reset (A, B, X, Y and Start) does not end the game. A recording cannot check this: it replays
only in the build that made it. `--frames 18000` checks only the
first two milestones, in two minutes. `--pictures DIR` also takes screenshots a fixed time after
some milestones and compares them with the set that `--keep` kept. The pictures come from the
game, so keep `DIR` outside the repository.

To compare two builds, play a whole autoplay game with each and compare the lines that
`IKARUGA_STREAMS` writes. Two builds that play the same game give the same lines: the same
display lists, registers, video memory and sound. The first line that differs shows when the two
games part:

```sh
env IKARUGA_AUTOPLAY=1 \
  IKARUGA_STREAMS=before.txt \
  build-before/ikaruga \
  --unthrottled \
  --frames 122000
```

Do this for each change of names or types, and for each change of the converter or the scripts.
A naming batch once changed how a float was stored in three functions, and the game looked and
sounded different from stage 3 on.

To check a change to the renderer in seconds, run `tools/port/render_check.py`. It draws a set
of display lists with `render-view` and compares each picture with the one that `--keep` kept.
It prints the mean difference and the number of pixels that differ by more than 16 in a channel.
`--make` dumps the set first: nine display lists from the title to the name entry, into
`runs/render-set`. Build `render-view` with `cmake --build build-port --target render-view`.
`render-view DUMP OUT.png --bloom 2` draws a dump with the bloom, `--anisotropy 16` with that
filtering. `--scale 4 --rescale 1` changes
the scale after the first render, as the GRAPHICS page does, and writes the scaled framebuffer to
`OUT.png.scaled.png`. The render after the change must equal a render at the new scale.

These variables show what a run does:

| Variable | Effect |
| --- | --- |
| `IKARUGA_PEEK=0x8c1d1428:4,...` | Prints the value at each address when it changes, at each vertical blank. The size is 1 (the default), 2 or 4. A 4-byte value is printed as a float too. |
| `IKARUGA_TWEAKS=lives,charge,bullets,start` | Switches on the tweaks of the menu's MODS AND TWEAKS page, also for a run without a window. |
| `IKARUGA_LATENCY=1` | The window shows white instead of the game while a key or a button is held, from the frame that reads the press on: the picture in which the game can show its first reaction. At the end the run prints, for each press, the time from the key's event in macOS to the refresh that showed the white picture. A slow-motion camera on the keyboard and the screen adds the keyboard's and the panel's delays. |
| `IKARUGA_PACING=1` | Prints at the end of a window run how many refreshes each picture stayed on the screen, how many pictures came later than asked, how many frames went without a picture to make up for late ones, and how long before its refresh a frame started. A covered window shows no pictures, so keep it in front. |
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
| `IKARUGA_STREAMS=FILE` | Writes a line for each render: its number, the frame, the display list's words, and hashes of the display list, of the registers the renderer reads, of video memory, of the decoded display list and of the sound so far. Two builds that play the same game give the same lines. A change that moves the timing changes them. The run's summary ends with the hash of the whole sound. |
| `IKARUGA_SH4_PCS=FILE` | Writes, at the end, the interpreted instructions run at each address of the executable: with the symbols, the time each routine takes itself |
| `IKARUGA_SH4_PROFILE=1` | Prints, at the end, the interpreted instructions per function that the C++ called, nested calls included |
| `IKARUGA_INTERPRET_ALL=1` | Runs all game code on the SH-4 interpreter, except the functions that the port replaces (`port/replaced.txt`). The boot is slow. |

How the runtime works is in [docs/progress.md](docs/progress.md), "Native port".

## Performance

Measured on 2026-10-25 and 2026-10-26 on a MacBook Pro with an M4 Pro and its 120 Hz display, on
AC power, with autoplay in stage 1, and for the GPU's time in a whole game with deadly shots, in
which every boss explodes:

| Measure | Result |
| --- | --- |
| CPU at the console's speed | 27% to 39% of one core at 2X and at 4X, all threads together, in four runs of a minute with the sound on |
| Speed without a limit | About 500 frames a second at 2X, 400 at 4X and 260 at 6X in a window, 806 without a window, over 16,000 frames. The console runs 59.94. |
| GPU time of a frame | 1.5 ms in the median at 2X and 1.9 ms at 3X and 4X. The heaviest frames are those of the last boss's explosion, with about 30 translucent layers on every pixel: 9.7 ms at 2X, 13.3 ms at 3X and about 21 ms at 4X, of the 16.7 ms that a frame has. The stage 1 boss's explosion takes 1 to 3 ms. With the CPU's time, the heaviest frame takes 12.5 ms at 2X. At 3X one frame takes longer than a frame, when the pool of fragments grows for the first time. At 4X, 25 frames of the last explosion take longer. |
| GPU memory | About 110 MB at 2X, 450 MB at 4X and 630 MB at 6X, from the sizes of the renderer's buffers. The pool of translucent fragments stops growing at 4X (315 MB), and a larger picture is drawn in more bands. A render that needs more than four bands doubles the pool once, as the last bosses' explosions do. |
| Input latency | From a key's event in macOS to the refresh that shows the game's first reaction, measured with `IKARUGA_LATENCY=1`: 16.7 ms in the median in fullscreen with VSYNC off (15 presses, 9.3 to 24.3 ms), 38.0 ms in fullscreen with VSYNC on (27.5 to 46.0 ms) and 42.1 ms in a window (27.5 to 45.0 ms). The keyboard's or gamepad's own delay before macOS has the event, and the panel's after the refresh, add to it. |

So at 2X and 3X, the game keeps its speed and shows every picture through all of its explosions.
At 4X and more, the last boss's explosion takes the GPU longer than a frame for about half a
second, and the game falls behind there by a few frames. A slower GPU reaches that limit at a
lower resolution. Most of an explosion's layers only add to the pixel, and their sum does not
depend on their order, so the renderer sums them instead of sorting them, and the GPU's own
blending adds them where no other layer lies around them ([docs/todo.md](docs/todo.md)).

The game's code and the SDK's graphics functions that it calls run as native code. The window
queues each picture behind its render on the GPU, so the CPU goes on with the next frame while the
GPU draws. About half of
the CPU time that a frame needs without the renderer goes to the sound: dream-recomp's emulation
of the AICA's mixer, its DSP and its ARM7 ([docs/todo.md](docs/todo.md)).

The latency has three parts. Ikaruga reads the controls once a frame, so a press waits up to a
frame, 8.3 ms on average, and the game moves the ship in that frame already. The game shows each
frame's picture at the vertical blank after the next one, as the console's PowerVR draws a frame
while the CPU runs the next. The port draws it at once, and the window shows the newest render
instead of the scanned-out one: the same pictures in the same order, each a frame earlier. Over a
whole game, the newest render was always the picture that the display showed next. Then the
picture goes to the screen: with VSYNC on, the window server takes two refreshes for it, so a
frame reads the controls about 28 ms before its refresh. Without VSYNC, in fullscreen, the picture
is on the screen about 2 ms after its present.

No emulator was measured in the same way. An emulator can hide a game's own frames between a
press and its reaction with run-ahead, which runs the game ahead from saved states. The port runs
the game as native code on the host's stack, which it cannot save, so it has no run-ahead.

A frame of the game takes two refreshes of a 120 Hz display. A picture that stays one or three
refreshes on the screen is a judder in the scrolling. `IKARUGA_PACING=1` counts them:

| Pacing | Pictures that stay two refreshes | Judders a minute |
| --- | --- | --- |
| By the display, in a window | 98.8% | about 40 |
| By the display, in fullscreen with VSYNC on | 99.8% | about 7 |
| In fullscreen with VSYNC off | 98.6% to 98.9% | 40 to 50 |

The pointer is hidden while the game runs, as a visible pointer shakes a fullscreen picture. Game
Mode (`build-port/Ikaruga.app` in fullscreen) made no difference beyond what runs vary by, in two
runs of 70 seconds each way, and it can matter more while other programs load the machine.

## Layout

| Path | Content |
| --- | --- |
| `tools/ghidra/` | The analysis pipeline and the language patch |
| `tools/symbols/` | The tools for the symbol tables |
| `tools/reference/` | The regression gate for the reference build |
| `tools/decomp/` | The converter from Ghidra's C to C++ for the native build |
| `tools/game/` | `stage.py`, which prints a chapter's stage program or a stage script with names |
| `port/` | The native build's runtime: guest memory, the machine built from dream-recomp's runtime, the boot ROM's services, the SH-4 interpreter for Ninja, the differential tests |
| `decomp/` | The converted game code. Generated, ignored here, a local repository of its own. |
| `game/` | The title config and the symbol tables. `game/extracted/` holds the disc files. |
| `docs/progress.md` | Status, decisions, measurements and the plan |
| `docs/native-platform.md` | What the game needs from the platform, and the steps to a native one |
| `docs/game.md` | How the game runs: its frame, its scenes, its task system and its named data |
| `docs/todo.md` | Work that is understood and put off |
| `CMakeLists.txt` | Builds dream-recomp and the recompiled title, for the reference build |

## Rules

- Never commit anything from the disc or the console. `game/extracted/`, `ghidra/`, `bios/` and
  `*.chd` are ignored. Read `git status` before every commit.
- The decompiled code comes from Sega's copyrighted program. Keep it out of any public remote.
- `game/sdk.h` holds only the types and prototypes of the SDK functions that the analysis uses,
  without macros or comments.
