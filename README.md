# ikaruga-decomp

A native macOS, Linux and Windows port of Ikaruga, built from a decompilation of its Dreamcast
version (Japan, T38706M V1.002). The game's machine code is decompiled into C++, which the build
compiles for the host. A renderer of its own draws the picture with Metal on macOS and Vulkan on
Linux and Windows, and SDL3 gives the window, the sound output and the controllers. The sound chip,
its ARM7 and a few other parts of the console are still emulated, by dream-recomp's runtime
([How it works](#how-it-works)).

This repository holds no disc image, no game code, data or text, no boot ROM and no file of the
Katana SDK. You supply the disc image of a copy of the game that you own, and the build decompiles
it on your Mac ([Legal](#legal)).

## Why this port

* Native playing of Ikaruga on macOS, Linux and Windows (PC version exists only for Windows)
* Very smooth and solid 60 fps: no jitters, no slowdowns (smoother than on original hardware)
* Low input latency (16 ms with vsync off, lower than with the original hardware or PC versions)
* Up to 4K resolution, anisotropic filtering for better texture quality
* Optional mouse mode for a more direct game experience
* Mods and tweaks: infinite lives and laser charge, half the bullets, an instant polarity switch,
  no music, no bullets from destroyed enemies, English text and more, on the menu's MODS AND
  TWEAKS page. The game's code is readable C++, which makes modding easier.

[Performance](#performance) gives the measurements.

## Status

The game plays from the boot through all five chapters to the ending, for one or two players, with
the menus, the tutorial and the memory card. The port was built and tested on macOS 26 with an
Apple M4 Pro, from the Japanese GD-ROM (T38706M, version 1.002).
The analysis expects this disc's program: other releases, such as the PC, GameCube or Switch
versions, do not work. The renderer draws with Metal on macOS and Vulkan on Linux and Windows; the
launcher and the full game build on both. The macOS build is the one with the longest play testing.

The Linux build passes a 95-second headless run through the title, menu and mode select with no
crash, and its graphical launcher starts the game from both a GDI and a CHD. The Linux AppImage
contains that launcher and the game. The Windows build cross-compiles with MinGW-w64; its graphical
launcher prepares both formats under Wine, and the game still needs a run on Windows hardware to
check Vulkan presentation.

## Download

The [releases](https://github.com/dc-decomp/ikaruga-decomp/releases) have `Ikaruga.app` for macOS
15 or later on Apple silicon, with SDL3 inside, so it needs no build and no Homebrew. It needs the
disc image of your own copy of the game, the GD-ROM that [Status](#status) names, as a `.gdi` with
its tracks. dream-recomp's `dcdisc chd2gdi` converts a CHD ([Building](#building), step 1). At
its first start the app asks for the `.gdi`, and [Playing](#playing) describes the rest.

The app has no Developer ID signature, so macOS blocks it at its first start. Then open System
Settings → Privacy & Security and choose Open Anyway. Its build checks that it uses nothing newer
than macOS 15, but it was played on macOS 26 only.

## How it works

Ghidra decompiles the boot executable `1ST_READ.BIN` into one C file per function. Ghidra's stock
SH-4 language gets the Dreamcast FPU code wrong, so this repository builds a patched language
first. `tools/decomp/convert.py` turns the C into C++ that runs on the host, with the console's
memory as an array that the C++ reads and writes. [docs/architecture.md](docs/architecture.md)
records what was patched and converted, and why.

The port gives the game what the console and its system software gave it:

- **Its own:** the PowerVR's registers and video timing, the tile accelerator's input and a
  renderer in Metal, the GD-ROM and its file system, the boot ROM's services, the memory card, the
  controllers, and the SDK's graphics libraries Ninja and Kamui as native code. An SH-4
  interpreter runs the few functions that stay machine code, and tests the C++ against the machine
  code.
- **From dream-recomp's runtime:** the memory map and the timing of the console's events, Holly's
  interrupt registers, the SH-4's DMA channel and G2's DMA, and the AICA with its ARM7, which plays
  the sound. The game's own sound library runs as decompiled code and talks to this AICA.

[dream-recomp](https://github.com/phobos665/dream-recomp) also gives the tools: `dcdisc` reads the
disc, and its function discovery seeds Ghidra, which alone finds 2,708 functions and with the
seeds 3,918.

## Requirements

- macOS with Xcode's command line tools, or Linux with GCC/Clang, or Windows with Visual Studio
  2022 or MinGW-w64
- CMake 3.24 or later: `brew install cmake` on macOS, `sudo apt install cmake` on Debian/Ubuntu,
  or the CMake installer on Windows
- Vulkan on Linux and Windows: the loader, headers and `glslc` (`vulkan-tools` and
  `glslang-tools` on Debian/Ubuntu, or the Vulkan SDK on Windows). macOS uses Metal and needs none.
- Ghidra 12.2 and JDK 25: `brew install ghidra` (a newer version than 12.2 might break things and need readjusting)
- SDL3: `brew install sdl3` on macOS, `sudo apt install libsdl3-dev` on Debian/Ubuntu, or the SDL3
  development package on Windows
- For the Linux AppImage: `appimagetool` (the build downloads it into `.deps/` if needed),
  `mksquashfs`, `curl`, and `patchelf`
- Python 3.10 or later
- dream-recomp via the submodule `dream-recomp/`: a commit of the `ikaruga-bringup` branch of
  [kjyv's fork](https://github.com/kjyv/dream-recomp). Upstream
  [dream-recomp](https://github.com/phobos665/dream-recomp) has Ikaruga's support since its pull
  request 11. The fork's branch adds pull request 15, the sound's headroom and limiter, which the
  port needs until upstream merges it. Clone this repository with
  `--recurse-submodules`, or run `git submodule update --init --recursive` in a clone. The
  analysis runs dream-recomp's `dream-translate` from its `build/`, and the steps below run its
  `dcdisc` from a virtual environment in its `.venv`. From this repository's directory:

  ```sh
  cmake -S dream-recomp -B dream-recomp/build
  cmake --build dream-recomp/build --parallel
  python3 -m venv dream-recomp/.venv
  dream-recomp/.venv/bin/pip install -e dream-recomp/tools/dcdisc
  ```

## Building

1. Put your disc image at `game/disc/Ikaruga (Japan).chd`. With a GDI dump, put its `.gdi` and
   its tracks into `game/disc/`, name the `.gdi` in place of the `.chd` below, and leave out
   `chd2gdi`.
2. Extract the disc's files into `game/extracted/`, and write its tracks as a GDI:

   ```sh
   dream-recomp/.venv/bin/dcdisc extract \
     "game/disc/Ikaruga (Japan).chd" \
     game/extracted
   dream-recomp/.venv/bin/dcdisc chd2gdi \
     "game/disc/Ikaruga (Japan).chd" \
     game/disc
   ```

3. Analyze and decompile. This takes about ten minutes, and the first run also builds the patched
   language:

   ```sh
   tools/ghidra/analyze.sh
   ```

4. Convert the C to C++. This writes `decomp/gen/`. The code there is derived from the game, so
   `decomp/` is ignored here and is a local repository of its own:

   ```sh
   python3 tools/decomp/convert.py
   ```

5. Build:

   ```sh
   cmake -S port -B build-port -DCMAKE_BUILD_TYPE=Release
   cmake --build build-port
   ```

The build makes `build-port/ikaruga` on Linux and Windows, and also `build-port/Ikaruga.app` on
macOS. It builds `build-port/ikaruga-launcher` at the same time. On Windows, CMake copies
`SDL3.dll` next to `ikaruga.exe`. The launcher uses SDL3 and a vendored Dear ImGui, so it does not
need GTK or Qt. The cross-compile toolchain is
[`port/cmake/toolchain-mingw.cmake`](port/cmake/toolchain-mingw.cmake).
[docs/development.md](docs/development.md) describes the analysis in more detail, the names and
types, and the checks for a change.

### Launcher (Linux and Windows)

`ikaruga-launcher` is a graphical launcher. Start it with no arguments, or double-click it, then
choose your `.gdi` or `.chd` with **Browse...** or by dropping it onto the window. The launch
options set the window, resolution, frame limit, audio, gamepads, extraction, screenshot, WAV and
replay settings that are passed to the game. The launcher stores the disc, folders and options in
`launcher.ini` in SDL's per-user preferences folder (on Linux
`~/.local/share/ikaruga/ikaruga/launcher.ini`; on Windows
`%APPDATA%\ikaruga\ikaruga\launcher.ini`). It prepares the disc in the background and minimizes
itself while the game runs.

A single `.gdi` or `.chd` argument opens the graphical launcher with that disc selected. The same
program keeps its command-line interface for automation and headless runs:

```sh
# Linux
./build-port/ikaruga-launcher --info "ROM/Ikaruga (Japan).chd"
./build-port/ikaruga-launcher --play "ROM/Ikaruga (Japan).chd"
./build-port/ikaruga-launcher --play "ROM/Ikaruga v1.002 (2002)(ESP)(JP)[!].gdi"

# Windows (from the port folder)
build-win\ikaruga-launcher.exe --play "ROM\Ikaruga (Japan).chd"
```

Without `--play` the launcher prepares and extracts the disc, then stops. Without a disc argument it
lists the `.gdi` and `.chd` files in `ROM/`, `game/disc/` and `game/prepared/` and asks which one
to use. `--out DIR` chooses where a CHD is unpacked, `--extract DIR` where the files go, and
`--no-extract` skips the extraction. Arguments after `--` are passed to the game.

For a gamepad-free or scripted run, pass `--no-gamepads` and `--no-audio` after `--`, for example:
`ikaruga-launcher --play ROM/game.chd -- --no-gamepads --no-audio`.

### AppImage (Linux)

After the build, package the launcher and the game with SDL3 into one file:

```sh
cmake --build build-port --target appimage
./build-port/Ikaruga-x86_64.AppImage
```

The target runs [`tools/linux/make_appimage.sh`](tools/linux/make_appimage.sh) and writes
`build-port/Ikaruga-x86_64.AppImage`. It downloads `appimagetool` into `.deps/` if it is not there,
and bundles the C++ runtime as well as SDL3. The launcher keeps its settings and its prepared disc
files in the per-user data folder, so the AppImage can run from anywhere and the user supplies the
disc. If FUSE is unavailable, run it with `APPIMAGE_EXTRACT_AND_RUN=1`.

## Playing

Start `build-port/ikaruga-launcher` (or the AppImage) and choose your disc there. On macOS start
`build-port/Ikaruga.app` from Finder, or start `build-port/ikaruga --window` from a terminal.

A window that is not scripted is a played run. It takes the disc that `--disc` names, else the one
that `disc.txt` names. Without one, it asks for the disc's `.gdi` in an open dialog and keeps the
answer in `disc.txt`. It reads the game's program and `IP.BIN` from the disc. The game draws its
system text, such as the memory card's notices, with the boot ROM's font: a played run takes the
boot ROM image `dc_bios.bin` from the settings folder or from `bios/`, or the one that `--bios`
names. Without an image, macOS's Hiragino Sans draws that text. Its memory card is the folder
`saves` in the settings folder, which its first run copies from the working directory's `saves`. A
played run shows its errors in a message box. `disc.txt`, `saves`, `dc_bios.bin` and the settings
go to `~/Library/Application Support/ikaruga/ikaruga`.

In fullscreen, macOS turns on Game Mode for `Ikaruga.app`, whose Info.plist declares it a game.
Game Mode gives the game priority on the CPU and the GPU. The plain `build-port/ikaruga` gets no
Game Mode.

### Controls and the menu

The arrow keys are the stick. Z shoots, X switches polarity, A releases the homing lasers and
Return is Start. A gamepad works by button position, and its left stick moves the ship and the menu's cursor like the
D-pad: the game reads only the direction buttons, so a stick pushed past its dead zone and a quarter
of the way on presses its direction (the CONTROLLER page's DEAD ZONE sets the zone). Escape, F1 or a gamepad's back button opens
the menu, and the game stops while it is open. The mouse works in the menu too.

The menu's CONTROLLER page sets the keys and the gamepad of each of two players. Player 1 takes the
first gamepad that was plugged in and player 2 the second, and the page's GAMEPAD row swaps them.
Player 2 has no keys at first. The second controller joins when player 2 presses Start during a
game, as on the console. With MOUSE on, the mouse moves player 1's ship while it is in play and
player 1 plays on the keyboard, within the game's bounds. The tutorial and the attract demo steer
the ship themselves. A press on player 1's gamepad gives the
ship back to the gamepad until the next key. The left button shoots, the right switches polarity
and the middle releases the homing lasers. The ship moves faster with the mouse than the game
usually lets it, so consider it a tweak.

QUIT in the menu ends the game. F10 shows the frame rate, F11 writes the last display list for
`render-view` and F12 writes a screenshot.

### Graphics

The GRAPHICS page changes only the picture. Its settings go to `graphics.txt`:

| Setting | What it does |
| --- | --- |
| RESOLUTION | The size that the game is drawn at, from 1X (640x480) to 6X (3840x2880). The window averages a larger picture over each of its pixels, which smooths the edges as supersampling does. The page shows the size of the picture in the window. `--scale N` sets the resolution for one run. |
| BLOOM | OFF, LOW or HIGH. The brightest parts of the picture glow. F12's screenshots show the glow. |
| ANISOTROPIC FILTERING | OFF or 2X to 16X. Textures seen at a slant stay sharper. The camera looks at the stages almost straight on, so the picture changes little: at 16X, 0.5% of the pixels or fewer in the test frames. |
| FULLSCREEN | ON or OFF. The window's green button and Control-Command-F change it too. `--fullscreen` starts one run in fullscreen, and so does the launcher's Fullscreen box. |
| TATE (ROTATED MONITOR) | AUTO, OFF, LEFT or RIGHT: how the picture turns for a monitor stood on its side. See [Tate](#tate-a-rotated-monitor). |
| VSYNC | OFF or ON, OFF at first. Off works only in fullscreen: the window server then shows each picture at once, without waiting for the display's refresh. That takes about 21 ms off the latency, and the picture can tear: a horizontal line where two pictures meet, for one refresh. At 120 Hz the two pictures differ by one frame of scrolling, and the line is hard to see. In a window the window server waits for the refresh anyway. |
| METAL HUD | ON or OFF. Metal's performance HUD in a corner of the window: the frame rate, the frame times and the GPU's time. |

At 60 Hz or 120 Hz the display paces the game, so that the scrolling stays smooth. Each frame stays
on the screen for one refresh at 60 Hz and for two at 120 Hz. The game then runs at 60.00 frames a
second instead of the console's 59.94, and the sound plays 0.1% faster to stay with it. A frame
that misses its refresh takes the next one, and the frames after it make up the time: a frame
comes a refresh earlier, or the game runs a frame without showing it. Otherwise each late frame
would put the game behind the sound, which then breaks up. A stall longer than the later frames
can make up, as when the system holds the program, leaves one gap in the sound: silence fills the
sound's queue when it ran dry, and a queue that the catch-up filled to its limit skips to its
usual length. At other refresh rates, and while the window is covered or the menu is open, the
game paces itself by its own clock, and some frames stay on the screen longer than others.
Fullscreen is smoother than a window ([Performance](#performance)).

### Tate: a rotated monitor

Ikaruga is an upright arcade game. The Dreamcast version has a VERTICAL SCREEN option in its
options that draws the whole picture, menus included, sideways in the 640x480 frame, for a
television that its player turned a quarter. A monitor stood on its side needs the port to turn the
picture back:

1. Turn the monitor in the desktop's display settings (Linux: Settings, Displays, Orientation:
   Portrait left or right; Windows: Display settings, Display orientation), so that the window is
   taller than wide.
2. In the game's options, set the screen to VERTICAL. If the stick must move the ship along the
   picture instead of along the television, set the game's own button-turning option as well.
3. Leave TATE on AUTO. The picture then turns a quarter counter-clockwise as soon as the window is
   taller than wide and the game is on its vertical screen, and it fills the monitor's width, upright.

LEFT and RIGHT turn the picture whatever the window's shape, counter-clockwise or clockwise, for a
monitor turned the other way or a picture the wrong way up. OFF never turns it. `--tate
auto|off|left|right` sets the choice for one run, and the launcher's Rotated monitor box does too.
With LEFT or RIGHT the window opens upright (720x960). The menu and the frame counter are not
turned: they are drawn for the window as you see it. The mouse follows the turn.

The turn is made by the Vulkan renderer (Linux and Windows). The Metal renderer does not turn the
picture yet. On a monitor stood on its side the game in its normal HORIZONTAL screen shows
its 4:3 picture across the monitor's width with bars above and below, as it would on any display.

### Mods and tweaks

The MODS AND TWEAKS page switches changes to the game, which go to `tweaks.txt`. The page shows a
line about the chosen tweak.

| Tweak | What it does |
| --- | --- |
| INFINITE LIVES | A player in the game keeps at least 2 lives. |
| INFINITE LASER CHARGE | The energy for the homing lasers stays full, so every release fires 12 lasers. In the prototype mode, where the shots spend energy too, it stays at 999. |
| HALF THE BULLETS | Every second bullet ends in the frame it comes in, so the enemies fire half as many. Fewer bullets also give less energy to absorb. |
| HIDE PRESS START | The blinking PRESS START on the side of a player who is not in the game goes away. GAME OVER and PLEASE WAIT stay. |
| SWITCH AT ONCE | The polarity changes on the switch's press. In the game, the ship turns over for 14 frames and changes its color halfway, 7 frames after the press, and until then bullets of both colors hit it. The ship also takes no shots and no other switch during the turn. With the tweak, the turn only shows, and the ship shoots and can switch back during it. The attract demo keeps the game's switch, which its recorded input expects. |
| NO MUSIC | The music is silent, and the sound effects play. The music's sequences still run, at the volume at which the game silences an effect, so the game's timing stays. |
| NO DEATH BULLETS | A destroyed enemy fires no bullets back at the player who destroyed it. The game fires them (`enemyDeathBullets`) on normal after a kill with the enemy's own color, and on hard after every kill, fewer and slower after a kill with the other color. The attract demo keeps them. |
| ENGLISH TEXT | The game's Japanese in English: the help of the menus, the settings of CONFIG, and the messages about the memory card, the rankings and the disc, which Hiragino Sans draws where the game would draw its Japanese line (`port/src/text.cpp`). Also the words that the game holds as pictures: the choice of the game mode and the difficulty, the motto at the start of a game, the chapters' cards, the words of the final chapter and the ending, the tutorial, and the characters' pages of APPENDIX, in Hiragino Mincho (`port/src/render/english_textures.cpp`). The game's own English words, such as the menus' items, stay as they are. |

Most tweaks change the game's data at each vertical blank (`port/src/game_tweaks.cpp`). The others
wrap game functions that `port/hooked.txt` lists, and with the tweak off, the wrapper only calls
the game's function.

### Memory card

`--saves DIR` names the memory card, a directory (default `saves` in the working directory, or for
a played run the settings folder's `saves`). The game's file in it, `IKARUGA_DATA.VMS`, is the image
the game writes to a Visual Memory unit, in the VMS layout that Dreamcast save tools read. As on
the console, the game saves on its own only with AUTO SAVE on in its options, which a new card has
off, at the next change of scene. BACKUP in its main menu saves at once. The run prints a line for
each load and save of the card.

## Performance

- **Latencies:** In fullscreen with VSYNC off, the ship's reaction to a key is on the screen
  16.7 ms after macOS has the key's event (median), and 38 ms with VSYNC on. The port shows each
  picture a frame earlier than the game itself would: the console's PowerVR draws a frame while the
  CPU runs the next, and the port draws it at once. So the reaction is likely quicker than on a real
  Dreamcast too, where it comes two frames (33 ms) after the game reads the controls, before the CRT
  draws it. No Dreamcast was measured for this. Published figures for other versions are few and
  come from a different method, from the button to a CRT: 4 frames (67 ms) handheld and 5 docked for
  the Switch version ([shmups wiki][lag-wiki]), and 2.5 frames for PS4 against 2.85 for Switch in a
  forum post ([shmups forum][lag-forum]).

[lag-wiki]: https://shmups.wiki/library/Input_lag
[lag-forum]: https://shmups.system11.org/viewtopic.php?p=1517633

Measured on a MacBook Pro with M4 Pro and its 120 Hz display with autoplay:

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
blending adds them where no other layer lies around them.

The game's code and the SDK's graphics functions that it calls run as native code. The window
queues each picture behind its render on the GPU, so the CPU goes on with the next frame while the
GPU draws. About half of the CPU time that a frame needs without the renderer goes to the sound:
dream-recomp's emulation of the AICA's mixer, its DSP and its ARM7.

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

## Layout

| Path | Content |
| --- | --- |
| `tools/ghidra/` | The analysis pipeline and the language patch |
| `tools/symbols/` | The tools for the symbol tables |
| `tools/decomp/` | The converter from Ghidra's C to C++ |
| `tools/port/` | The port's checks, the release build, and the list of the game's Japanese lines |
| `tools/reference/` | The regression gate for dream-recomp's reference build |
| `tools/game/` | `stage.py`, which prints a chapter's stage program or a stage script with names |
| `port/` | The port: guest memory, the machine built from dream-recomp's runtime, the boot ROM's services, Ninja and Kamui, the renderer, the tweaks, the SH-4 interpreter and the differential tests |
| `port/packaging/` | The launcher's icon and desktop entry, used by the AppImage |
| `tools/linux/` | The headless test harness and the AppImage builder |
| `decomp/` | The converted game code. Generated, ignored here, a local repository of its own. |
| `dream-recomp/` | dream-recomp, a submodule at the commit that this repository builds and runs with |
| `game/` | The title config, the symbol tables and the game's types. `game/extracted/` holds the disc files. |
| `docs/development.md` | Working on the port: the analysis, names and types, the reference build and the checks |
| `docs/architecture.md` | The design: the layers of the code, the decisions, the native port and the known gaps |
| `docs/native-platform.md` | What the game needs from the platform, and the steps to a native one |
| `docs/game.md` | How the game runs: its frame, its scenes, its task system and its named data |
| `CMakeLists.txt` | Builds dream-recomp and the recompiled title, for the reference build |

## Legal

Ikaruga is © Treasure and ESP. This project is not affiliated with or endorsed by Treasure, ESP or
Sega. Dreamcast is a trademark of Sega.

The repository holds tools, names, types and documentation, and the port's own code. The port
reimplements the functions of the Katana SDK's graphics libraries, Ninja and Kamui, that the game
calls: each computes what its machine code computes, so that the decompiled game runs on it. The
analysis uses only the names, prototypes and types of the SDK functions that the game links.

The game's code that `tools/decomp/convert.py` writes into `decomp/`, and every program built from
it, derive from the game.

## License

The files of this repository are licensed under the GNU General Public License, version 2 or (at
your option) any later version: see [LICENSE](LICENSE). The port links dream-recomp's runtime,
which is licensed under the GPL, version 2, with parts ported from Flycast, so a built program is
under version 2. The releases' app also holds SDL3, under the zlib license. The app keeps the three
licenses in `Contents/Resources`.
