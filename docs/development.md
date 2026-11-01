# Development

How to work on the port: the analysis in more detail, the reference build, names and types, and
the checks that show what a change does. The [README](../README.md) has the build and the game.
[progress.md](progress.md) has the status, the decisions and the measurements, and
[game.md](game.md) how the game runs.

## The analysis

`tools/ghidra/analyze.sh` analyzes and decompiles `1ST_READ.BIN` in about ten minutes. Back up
`ghidra/out` before a run: the script deletes `ghidra/out/c` at its start.

After a change to `tools/ghidra/ExportDecompiled.java` only, `tools/ghidra/analyze.sh --export`
writes the C again from the analyzed project in about 20 seconds. To check such a change on a few
functions first, set `EXPORT_ONLY=8c0a9664,...` and run the script into another directory,
because `--export` deletes `ghidra/out/c` first. The saved project does not give quite the C of a
full run (some functions decompile differently), so use `--export` to look at a change and a full
run for the C that you convert.

The first run also builds and installs the patched SH-4 language. To rebuild it by hand, run
`tools/ghidra/build_language.py`. The names of the SDK functions that the game links are committed
in `game/symbols.tsv` and `game/globals.tsv`, and their prototypes and types in `game/sdk.h`.

| Path | Content |
| --- | --- |
| `ghidra/out/c/<address>.c` | One decompiled function per file |
| `ghidra/out/c/index.tsv` | Address, name, size and decompile result of each function |
| `ghidra/out/functions.json` | The function list |
| `ghidra/project/` | The Ghidra project. Open it in the Ghidra GUI to rename and retype. |

Ghidra keeps its settings, its cache and the patched language under `ghidra/home/`. The scripts
need no write access outside this directory.

## Names and types

Names live in two files. Ghidra applies both on each run of `tools/ghidra/analyze.sh`, and the
reference build uses `game/symbols.tsv`.

| File | Content |
| --- | --- |
| `game/symbols.tsv` | Function names, and in a fourth column a comment on each |
| `game/globals.tsv` | Data names, in a fourth column their types and in a fifth a comment. Ghidra makes labels from them, never functions. |
| `game/types.h` | The game's types and the prototypes of its functions |

Each row is an address, a name and a source, separated by tabs. Either RAM spelling of the address
works (`0x0c013958` or `0x8c013958`).

1. When you know what a function does, add a row with the source `user`, and a comment:

   ```text
   0x0c06ff30	chainBonusLabelUpdate	user	Moves the label of a chain bonus on. ...
   ```

2. Run `python3 tools/decomp/convert.py`. It names the function itself, in its file and at each
   call, so a name needs no new analysis. It refuses a name that is no C++ identifier, that names
   two functions or that the code uses already: a local, a parameter, a field or a global. It also
   refuses to rename a function that `port/` names: rename it there in the same change.
3. Build, and compare a game of the port before and after the change, with `IKARUGA_STREAMS`
   (below). A name alone leaves the code as it was, so the streams stay the same.

A data name and a type change Ghidra's analysis: run `tools/ghidra/analyze.sh` before
`convert.py`, and compare a whole game. Ghidra's guesses of types elsewhere can shift, and with them
the C++ of functions far from the change.

The comments go above each function in `decomp/gen/`, above its declaration in `functions.h` and
above each global in `globals.h`. A comment says what the function does, when it runs and what its
values mean, in one to three sentences, from what the code, its callers or a run show. Keep the
game's systems in [game.md](game.md), and point there. Name only what the evidence shows: a wrong
name misleads more than `FUN_8c0a1234`. Where the purpose is unclear, a comment can still say who
calls the function and when.

The SDK functions' rows have a source that starts with `sdk-`. Where a `user` row and an SDK row
have the same address, the `user` row wins.

## The converted code

`tools/decomp/convert.py` writes `decomp/gen/` from Ghidra's output. Three lists in `port/` change
what it writes:

| File | Effect |
| --- | --- |
| `port/replaced.txt` | Functions that the port implements natively. The converter does not write them. |
| `port/interpreted.txt` | Functions that run as machine code on the port's SH-4 interpreter |
| `port/hooked.txt` | Functions that the port wraps for its tweaks. The converter writes each one as `NAME_game`, and the port defines `NAME`, which calls it. |

A function in `decomp/fix/` is corrected by hand, and the converter does not write it either.

## The reference build

The reference build is dream-recomp's recompiled program. The port does not need it. It needs
MoltenVK, the Vulkan loader and headers, `glslc` and SDL3:

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

   `--bios` takes your 2 MB Dreamcast boot ROM image. The game draws its system text, such as the
   memory-card notices, with the ROM's font. Without it, that text is blank. The BIOS code does not
   run: dream-recomp answers the BIOS calls natively. Put the image in `bios/`, which git ignores.
   Without input, the game waits on its save-file screen. Press Start to continue.

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

6. To check that a change did not alter what the reference build does, capture the regression
   gate before and after the change. A capture takes about 35 seconds and goes to `.regress/`:

   ```sh
   tools/reference/gate.py capture before
   tools/reference/gate.py capture after
   tools/reference/gate.py compare before after
   ```

   The gate runs two scenarios: `boot` without input, and `play`, which presses Start and A into
   stage 1. A changed write hash must be understood before the work continues.

## Runs without playing

These options script a run of the port:

| Option | Effect |
| --- | --- |
| `--frames 600` | Stops after this many frames |
| `--press start@600,a@700-900` | Holds a button for 8 frames from each frame given, or over a range of frames. A `2` before the button, as in `2start@3000`, presses it on a second controller, which the run then has. |
| `--screenshot-at 300,650` | Writes the frames drawn at these frames, as `ikaruga-300.ppm` and so on |
| `--wav FILE` | Records the sound |
| `--unthrottled` | Runs the window as fast as the host can |
| `--record-input FILE` | Writes the controller state of each frame where it changes |
| `--replay-input FILE` | Plays a recording back instead of the controls. A recording only replays the same game in the build that made it: the reference build's frames do not line up with the port's. |

A scripted run (autoplay, `--replay-input`, `--press`, `--frames` or screenshots) does not read or
write `graphics.txt` and `tweaks.txt` and ignores MOUSE, so the settings do not change a check.
`IKARUGA_TWEAKS` switches tweaks on for such a run.

## The checks

**The differential test.** To find a function whose C++ does not do what the game's machine code
does, run with `IKARUGA_DIFF=1`. The port then runs the first call of each function twice, once as
machine code on its SH-4 interpreter and once as C++, and prints each function whose results
differ:

```sh
env IKARUGA_DIFF=1 \
  build-port/ikaruga \
  --frames 3200 \
  --press start@600,a@700,start@1200,a@1300,start@2000,a@2100
```

`IKARUGA_DIFF_ONLY=0x8c06a7a4` tests one function, `IKARUGA_DIFF_ONLY=0x8c06a7a4,0x8c0196fc` two.
`IKARUGA_DIFF_FROM=54500` starts the tests at frame 54,500, so that the functions get new tests in a
later stage. The test cannot run a call inside another tested call, so the report names the
functions that the run calls only there: test these alone with `IKARUGA_DIFF_ONLY`.
[progress.md](progress.md) describes what the test can and cannot find.

**The milestones.** To check that a change still lets the game reach its milestones, whatever it
does to the timing, run `tools/port/check.py`. It plays the whole game with `IKARUGA_AUTOPLAY`, in
about 15 minutes. It fails when a stage, a boss or the ending does not come by a given frame, when
the game stops drawing, when a boot does not get back what the game saved after the ending, or when
the soft reset (A, B, X, Y and Start) does not end the game. A recording cannot check this: it
replays only in the build that made it. `--frames 18000` checks only the first two milestones, in
two minutes. `--pictures DIR` also takes screenshots a fixed time after some milestones and
compares them with the set that `--keep` kept. The pictures come from the game, so keep `DIR`
outside the repository.

**Two builds.** To compare two builds, play a whole autoplay game with each and compare the lines
that `IKARUGA_STREAMS` writes. Two builds that play the same game give the same lines: the same
display lists, registers, video memory and sound. The first line that differs shows when the two
games part:

```sh
env IKARUGA_AUTOPLAY=1 \
  IKARUGA_STREAMS=before.txt \
  build-before/ikaruga \
  --unthrottled \
  --saves "$(mktemp -d)" \
  --frames 122000
```

Autoplay saves to the memory card after the ending. A new, empty card for each run, as `--saves`
gives here, keeps the runs the same and leaves `saves/` as it is.

Do this for each change of names or types, and for each change of the converter or the scripts.
A naming batch once changed how a float was stored in three functions, and the game looked and
sounded different from stage 3 on.

**The renderer.** To check a change to the renderer in seconds, run `tools/port/render_check.py`.
It draws a set of display lists with `render-view` and compares each picture with the one that
`--keep` kept. It prints the mean difference and the number of pixels that differ by more than 16
in a channel. `--make` dumps the set first: nine display lists from the title to the name entry,
into `runs/render-set`. Build `render-view` with `cmake --build build-port --target render-view`.
`render-view DUMP OUT.png --bloom 2` draws a dump with the bloom, `--anisotropy 16` with that
filtering. `--scale 4 --rescale 1` changes the scale after the first render, as the GRAPHICS page
does, and writes the scaled framebuffer to `OUT.png.scaled.png`. The render after the change must
equal a render at the new scale. `--list` lists the draws, and with `RENDER_VIEW_VERTICES=1` each
vertex with its u and v.

## ENGLISH TEXT

`port/src/text.cpp` knows the game's Japanese lines by the FNV-1a hash of their Shift-JIS bytes, so
that the repository holds none of the game's text. `tools/port/english.py` lists the lines of the
disc's program with their hashes and their English, and `--missing` the lines without English.

The words that the game holds as pictures are in `port/src/render/english_textures.cpp`, by the
hash of each texture's bytes in video memory. To find a texture and its phrases:

1. Write each texture that a run uses with `IKARUGA_DUMP_TEXTURES=DIR`, as its RGBA8 bytes, named by
   the hash and the size.
2. Dump the display list of a frame that shows the words with `IKARUGA_DUMP_TA`, and list its draws
   with `RENDER_VIEW_VERTICES=1 render-view DUMP OUT.png --list`. The u and v of a draw give the
   texels of each piece of a phrase.

The file has a building block for each way that the game draws words. A phrase is one line across
pieces, and a block holds lines in one piece. A paragraph is a block with a piece to its right, which
goes on with the block's first lines, as in the tutorial. A caption is a column of lines in a
picture, over the art, as on the characters' pages. It erases the Japanese from the art first.
Words that show only for a moment are easier to find in display lists dumped every 30 frames: a
throwaway change to `IKARUGA_DUMP_TA` in `port/src/main.cpp` does it.

## Variables

These variables show what a run does:

| Variable | Effect |
| --- | --- |
| `IKARUGA_PEEK=0x8c1d1428:4,...` | Prints the value at each address when it changes, at each vertical blank. The size is 1 (the default), 2 or 4. A 4-byte value is printed as a float too. |
| `IKARUGA_TWEAKS=lives,charge,bullets,start,switch,music,death,english` | Switches on the tweaks of the menu's MODS AND TWEAKS page, also for a run without a window. |
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
| `IKARUGA_DUMP_TEXTURES=DIR` | Writes each texture that the renderer decodes, once, as RGBA8 bytes named by its hash and size |
| `IKARUGA_CALLED=FILE` | Writes each decompiled function that the run entered, with the frame of its first call and its number of calls. Functions on the interpreter are not in it. |
| `IKARUGA_HW_USERS=FILE` | Writes the registers that each decompiled function read and wrote, and where its store-queue flushes went. Interpreted code counts for the C++ function that called it. |
| `IKARUGA_STREAMS=FILE` | Writes a line for each render: its number, the frame, the display list's words, and hashes of the display list, of the registers the renderer reads, of video memory, of the decoded display list and of the sound so far. Two builds that play the same game give the same lines. A change that moves the timing changes them. The run's summary ends with the hash of the whole sound. |
| `IKARUGA_SH4_PCS=FILE` | Writes, at the end, the interpreted instructions run at each address of the executable: with the symbols, the time each routine takes itself |
| `IKARUGA_SH4_PROFILE=1` | Prints, at the end, the interpreted instructions per function that the C++ called, nested calls included |
| `IKARUGA_INTERPRET_ALL=1` | Runs all game code on the SH-4 interpreter, except the functions that the port replaces (`port/replaced.txt`). The boot is slow. |

How the runtime works is in [progress.md](progress.md), "Native port".
