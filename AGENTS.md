# Working on this port

The repository contains no game data. The disc, extracted files, Ghidra project and generated
`decomp/` are local and ignored by git.

## Headless testing

Agents test `ikaruga` headless while the user works on this machine. Full method and project
settings: [`docs/headless.md`](docs/headless.md). Harness: `tools/linux/headless_run.sh`, settings
in `tools/linux/headless.conf`.

### Hard rules

1. Every run goes through `tools/linux/headless_run.sh`. Never launch the game on the user's
   display, never take desktop screenshots, and never play sound.
2. Never open the user's controller. `--no-gamepads` is always passed on the command line. If a run
   prints `input: opened gamepad`, stop and tell the user.
3. Do not build into, overwrite, or kill a game the user started. Headless runs use an isolated
   copy under `/tmp/ikaruga-headless-$USER-*`; pick processes by working directory, never
   `pgrep -n`.
4. No headless runs during a playtest. Tell the user when a batch starts and ends.
5. Do not commit or push unless the user asks.

### Day to day

```sh
tools/linux/headless_run.sh boot 90
RUNS=5 GDB=1 tools/linux/headless_run.sh c1 120
DISPLAY_NUM=92 tools/linux/headless_run.sh smoke 60 20:Return 30:Return
```

Look at the PNGs in `/tmp/ikaruga-headless-$USER-91/out`, not only the crash tally.

### Checkpoints

| ID | What | Last result |
|---|---|---|
| C0 | Boots to the title with 0 crash lines and no real pad | pending |
| C1 | Stable over repeated runs | pending |
| C2 | Soak | pending |
| C3 | Performance | pending |
| C4 | User sign-off | pending |
