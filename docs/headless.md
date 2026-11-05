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
