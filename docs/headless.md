# Headless testing

The port is tested on a private Xvfb display with software presentation, muted audio and no real
controller. The harness is `tools/linux/headless_run.sh`; settings are in
`tools/linux/headless.conf`.

## Hard rules

- Every run goes through the harness. Never launch the game on the user's display, capture the
  desktop, or play sound.
- Never open a real controller. `--no-gamepads` is passed on the command line; the harness also sets
  `SDL_GAMECONTROLLER_IGNORE_DEVICES=*`. A run that prints `input: opened gamepad` is a failure.
- Never build into or kill a game the user started. The harness runs an isolated copy from
  `/tmp/ikaruga-headless-$USER-<display>`.
- Tell the user when a batch starts and ends. Do not run during a playtest.
- Look at the PNGs, not only the crash tally.

## Commands

```sh
tools/linux/headless_run.sh boot 90
RUNS=5 GDB=1 tools/linux/headless_run.sh c1 120
DISPLAY_NUM=92 tools/linux/headless_run.sh smoke 60 20:Return 30:Return
```

Output is under `/tmp/ikaruga-headless-$USER-91/out`. `DISPLAY_NUM=92` is free for a second agent.

## Project specifics

- Executable and build: `build-port/ikaruga`; assets are the repository's `game/` directory.
- Disc: `game/disc/disc.gdi` (a symlink to the user's legally dumped GDI, with its tracks in the
  same directory). The launcher can prepare this from a `.chd`.
- Safety flags: `--no-audio --no-gamepads`; scale 1 for speed.
- Crash signatures: the crash handler's `signal N, backtrace:` line, plus `unimplemented`,
  `Aborted`, `Segmentation fault` and `Assertion`.
- Real-pad line: `input: opened gamepad`.
- Default key script: repeated Return, because the first boot has a cold shader cache.
- Perf: `IKARUGA_PACING=1` prints the frame-pacing summary at shutdown.

## Fullscreen and a rotated screen

`WM=xfwm4` runs a window manager on the private display, which fullscreen needs to behave as on a
desktop, and `SCREEN=1080x1920x24 SHOT_SIZE=540x960` makes the display portrait, like a monitor
stood on its side. `IKARUGA_POKE=0x8c1cd768=0,0x8c1cb150=0` puts the game on its VERTICAL SCREEN
option (the option and the game's copy of it) without the menu. With `--fullscreen` and
`--tate auto` the picture then fills the portrait screen upright.

```sh
SCREEN=1080x1920x24 SHOT_SIZE=540x960 WM=xfwm4 DISPLAY_NUM=93 \
  EXTRA="--window --disc $PWD/game/disc/disc.gdi --saves /tmp/s --scale 2 --fullscreen" \
  RUN_ENV="IKARUGA_AUTOPLAY=1 IKARUGA_POKE=0x8c1cd768=0,0x8c1cb150=0" tools/linux/headless_run.sh tate 70
```

## Render comparison

```sh
DISPLAY_NUM=91 tools/linux/render_set.sh /tmp/ikaruga-renderset 700 3000 9500 30000
tools/linux/compare_set.sh /tmp/ikaruga-renderset
```

`render_set.sh` plays the game to each frame through the harness and writes the display list
(`fN.ta`) and the frame-exact picture (`fN.ppm`, taken at N+2). `compare_set.sh` draws every dump
with `build-port/render-view` (the port's renderer) and dream-recomp's renderer, and writes
`fN.pair.png` (reference left, port right). Build the tools first:
`cmake --build build-port --target render-view dream_render_view`. The dumps hold game data: keep
them out of the repository. A stale lock file of a dead Xvfb no longer blocks a display.
