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

- Ghidra 12.1 and JDK 21: `brew install ghidra`
- A checkout of dream-recomp at `../dream-recomp`, built as its README describes, with `dcdisc`
  installed. `dream-recomp/` in this repository is a symbolic link to it.
- Python 3.9 or later

## Steps

1. Put your disc image at `Ikaruga (Japan).chd`.
2. Extract the disc:

   ```sh
   ../dream-recomp/.venv/bin/dcdisc new-game "Ikaruga (Japan).chd" ikaruga --dir . --title Ikaruga
   mv ikaruga game
   ```

3. Analyze and decompile. This takes about two minutes:

   ```sh
   tools/ghidra/analyze.sh
   ```

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

## Layout

| Path | Content |
| --- | --- |
| `tools/ghidra/` | The analysis pipeline and the language patch |
| `tools/symbols/` | The tools for the symbol tables |
| `tools/reference/` | The regression gate for the reference build |
| `game/` | The title config and the symbol tables. `game/extracted/` holds the disc files. |
| `docs/progress.md` | Status, decisions, measurements and the plan |
| `CMakeLists.txt` | Builds dream-recomp and the recompiled title, for the reference build |

## Rules

- Never commit anything from the disc or the console. `game/extracted/`, `ghidra/`, `bios/` and
  `*.chd` are ignored.
  Read `git status` before every commit.
- The decompiled code comes from Sega's copyrighted program. Keep it out of any public remote.
- Never copy Sega's code into this repository, including into comments.