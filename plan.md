# Plan: finish the Linux and Windows port

Written 2026-11-05 after reviewing `progress.md` and running the Linux build myself.

## 1. What `progress.md` is, and how far to trust it

`progress.md` is a 4 MB export of a previous agent session, not a status file. Its closing summary
says the launcher, the AppImage and the Vulkan renderer "work". The README repeats that the Linux
build "passes a 95-second headless run through the title, menu and mode select with no crash".

The crash claim holds. The graphics claim does not: the harness only counts crash lines, and nobody
compared the screenshots with the Dreamcast or Metal output.

## 2. Verified state (my own run, 2026-11-05)

Run: `DISPLAY_NUM=93 tools/linux/headless_run.sh smoke 75 20:Return ... 58:z`
(the harness's default `:91` has a stale lock file from the earlier session; I did not touch it).
Result: 0 crash lines, 0 real pads opened, 4438 renders in 4462 frames, on the RX 6600 (RADV).
Screenshots are in `/tmp/ikaruga-headless-jason-93/out/`.

| Area | Status | Evidence |
|---|---|---|
| Boot, disc read, game logic, no crash | Works | clean run, 4438 renders |
| Title and main menu text | Mostly works, picture is washed out | `smoke_1_30.png`, `smoke_1_38.png` |
| Mode-select boxes | Broken: text clipped at x about 480 | `smoke_1_45.png`, `smoke_1_52.png` |
| Stage 1 | Broken: geometry stretched or misplaced, sprites tiny, HUD partly overwritten ("P-ESS START") | `smoke_1_end.png` |
| Graphical launcher (Linux) | Built, reported working; not re-checked by me | |
| AppImage | Built (about 20 MB); not re-checked by me | |
| Windows build | Cross-compiles with MinGW; only run under Proton and lavapipe, never on real hardware | |
| Audio | Never tested: every headless run passes `--no-audio` | |
| Performance, soak, controller, fullscreen | Never measured | AGENTS.md checkpoints are all `pending` |

All the Linux and Windows work is uncommitted (15 modified files, plus new `gpu_vulkan.cpp`,
shaders, launcher and `third_party/`).

## 3. Diagnosis of the graphics problem

The decompiled game logic is shared with macOS, where Metal renders it correctly. The bugs sit in
the Vulkan backend, `port/src/render/gpu_vulkan.cpp` (2137 lines), and its 11 GLSL shaders, which
were ported from `gpu_metal.mm` (1175 lines) and `shaders/geometry.metal`.

Suspects, most likely first:

1. **Depth and sort.** The stage is mostly opaque depth-tested 3D, and it looks wrong. Check the
   depth compare mapping (`compare()`, line 139), the depth format and clear value, reversed-Z
   handling, and that the depth buffer is cleared per band. Metal has a `depth_state()` cache that
   the Vulkan code must match.
2. **Clip and scissor.** The mode-select text clips at a fixed x. `ta.cpp` computes the user tile
   clip in 32-pixel tiles and both backends consume it, so the difference is in how
   `Clip::Inside` and `Clip::Outside` are applied, or in the band and scissor intersection at
   `gpu_vulkan.cpp:1071` and `1182`. Outside clipping needs two scissors or a shader test.
3. **Blend and translucency.** The washed-out title points to a wrong blend factor
   (`blend_factor()`, line 126), a premultiplied-alpha mismatch, or the added and translucent
   passes drawing over opaque pixels.
4. **Texture decode and sampling.** Wrong filter, mip, clamp or flip modes, the VQ and palette
   paths, and the `english_textures` replacement.
5. **Y flip and viewport.** Vulkan's clip space is flipped against Metal's; a wrong flip would show
   as mirrored or misplaced geometry.

## 4. Work plan

### Phase A: get a trustworthy way to compare (done, see below)
- A1. Add a harness mode that saves **frame-synchronised** screenshots at fixed frame numbers (the
  game already has `--screenshot-at` and `--screenshot-path`), so runs are comparable and not
  timing-dependent.
- A2. Use the existing `IKARUGA_DUMP_TA`, `render-view` and `render-compare` tools to dump the same
  display list to a PNG through the CPU path in `dream-recomp`, as a reference image. This gives a
  ground truth on Linux without a Mac.
- A3. Check `/tmp/ikaruga-headless-*` stale locks and make `headless_run.sh` detect a dead Xvfb
  lock automatically.

### Phase A: results (done 2026-11-05)

Tools, all going through `tools/linux/headless_run.sh` or needing no game window:
- `tools/linux/render_set.sh OUT FRAME...`: plays autoplay to each frame and writes `fN.ta` (display
  list, VRAM, PVR registers) and a frame-exact `fN.ppm`. The screenshot is taken at FRAME+2; it then
  equals `render-view` on the dump with **0 differing pixels** (checked on frame 3000), so the dump
  path is a faithful replay of the game's own rendering.
- `tools/linux/compare_set.sh OUT`: draws each dump with `render-view` (the port's renderer) and
  `dream_render_view` (dream-recomp's renderer, on a private Xvfb) and writes `fN.pair.png`.
- Harness fixes: a dead Xvfb's lock no longer blocks a display, `EXTRA=` now overrides the conf, and
  `RUN_ENV="A=1"` passes game variables.

**Decision (2026-11-05): no Mac is available, so dream-recomp's renderer is the reference; "exact" means pixel-close to it.**

Reference caveat: dream-recomp's renderer is the lineage the Metal one came from, not Metal. Exact
Metal parity still needs `render-view` run on a Mac against the same `.ta` dumps (they hold game data,
so they stay out of the repository). Until then dream's pictures are the working reference.

Bugs found and fixed while building the tools:
1. `render-view` and any windowless run crashed in `Gpu::create` (SDL extension query with no window).
   The backend now works with no surface or swapchain.
2. **Band viewport**: each band's viewport covered only the band's rows, squeezing the whole scene into
   every band. Metal uses the full target and a scissor. Fixed.
3. **Y flip**: Vulkan stored every target upside down, so screenshots and `read_target` were flipped and
   all scissor rows were mirrored. Every viewport is now flipped (negative height), as Metal's
   top-left origin.
4. `render-view`'s PNG had a bad IEND CRC.

Remaining difference after A, on frames 700, 3000, 9500 and 30000 (141k to 300k of 307k pixels differ):
- The opaque textured 3D is missing: stage backgrounds, bosses and the player's ship render black,
  while HUD, bullets and some translucent parts draw. Start B with the opaque depth path.
- The mode-select box is clipped at x = 320 (half the width).
- The menu's backdrop and highlight glow are missing.

### Phase B: progress (2026-11-05)

Pixels that differ by more than 6% from the reference, on frames 700, 3000, 9500, 30000 (of 307200):

| Step | f700 | f3000 | f9500 | f30000 |
|---|---|---|---|---|
| Start of B (after A) | 299758 | 172486 | 155294 | 141013 |
| Opaque strips' depth compare (key lacked the depth bits) | 28867 | 163053 | 20347 | 32865 |
| Adding strips' one/one blend | 13129 | 100669 | 13382 | 14336 |
| Pre-sort pass ran added_top instead of added | 4417 | 5897 | 6448 | 5795 |

Also fixed: image barriers covered only mip level 0. In-window smoke (stage 1, title, menus): 0 crash
lines, 0 pads, the picture matches the reference by eye.

Wider set (2026-11-05, after the three fixes; differing pixels of 307200): f400 1345, f1500 12548,
f42000 6952, f60000 9038, f85000 4416, f107000 6028, plus the four above. Covers the title, the
mode select, stage 1 and later stages with bosses, and the name entry. By eye, no missing or misplaced
feature. A heat map of the remainder shows only one-pixel edge outlines (the reference renders at
960x720 and is resampled), so B1's tail is closed. Validation is clean in `render-view` and in a
60 s windowed run; the leaked present/overlay pipelines and the windowless PRESENT_SRC layout are fixed.

B6/B7 results: bloom draws (halo on the HUD and bullets) and 2x agrees with 1x apart from resampling
edges. A scale change (the menu's GRAPHICS page) now resamples the existing framebuffers, as Metal
does, instead of dropping them ("no scaled picture"); `--scale 4 --rescale 1` is within 0.5% of a plain
1x render and clean under validation. The same dumps through lavapipe and RADV differ by 0 to 60 of
307200 pixels, so the picture does not depend on the driver. Fullscreen and the desktop's own
compositor are not testable headless.

Still open in B (done: B1 tail, validation, warning, B6 except fullscreen, B7 on Linux):
- B1 tail: the remaining 2% (edges? filtering? bloom is off in render-view); find what differs with a
  per-pixel diff image, and check more frames (every stage, bosses, the ending, the ranking screens).
- Validation at shutdown: `VUID-vkDestroyDevice-device-05137` (a leaked object); the windowless swap pass
  asks for PRESENT_SRC without the swapchain extension (`make_swap_pass`).
- The unused `families` variable in `Gpu::create` (compiler warning).
- B6 (bloom, 2x and 4x scales, fullscreen) and B7 (lavapipe) are untouched.
- The title's cover page looks pale (frame 30 in the smoke run): check it against the reference with a
  dump from frame about 400 (`render_set.sh`) before calling it a bug.

### Phase B: fix the Vulkan renderer (one fix at a time, each checked against the reference)
- B1. Run with `VK_LAYER_KHRONOS_validation` and clear every validation error first.
- B2. Depth: fix the stage geometry (suspect 1). Re-check `smoke_1_end.png`.
- B3. Clip and scissor: fix the clipped mode-select boxes (suspect 2).
- B4. Blend and fade: fix the washed-out title (suspect 3).
- B5. Texture and flip issues found by the comparison; the "P-ESS START" corruption.
- B6. Bloom, scaling and letterboxing at 1x, 2x and 4x, and fullscreen.
- B7. Test on more than one Vulkan driver: RADV (real), lavapipe (software), and later a Windows
  driver.

### Phase C: broaden coverage of the game
- C1. Scripted runs through all stages and the boss fights, practice, ranking, config, tutorial,
  and the game-over and continue screens, with screenshots reviewed at each stop.
- C2. Audio: one run with a WAV capture (`--wav`) compared with the macOS output, then a real
  device check done by the user (the agent must never play sound).
- C3. Input: keyboard bindings, controller mapping and rumble, checked by the user (the agent must
  never open the controller).
- C4. Saves: card file created and reloaded across runs.

### Phase D: Windows
- D1. Make the MinGW build reproducible (`port/cmake/toolchain-mingw.cmake`) and document it.
- D2. Try an MSVC build, since the CMake already has `MSVC` branches.
- D3. Run on real Windows hardware (needs the user), then fix whatever shows up: Vulkan
  presentation, the path and quoting rules in the launcher (`--play` already hit a `cmd` quoting
  bug), the DLL layout, and the SDL3 gamepad path.
- D4. Package: a zip with `ikaruga.exe`, `ikaruga-launcher.exe` and `SDL3.dll`, or an installer.

### Phase E: launcher and packaging
- E1. Re-test the graphical launcher with a GDI and with a CHD, including unusual paths
  (spaces, non-ASCII) and an invalid file.
- E2. Re-test the AppImage with normal FUSE execution, not only `APPIMAGE_EXTRACT_AND_RUN=1`, on a
  clean machine or container, and add AppStream metadata to remove the packaging warning.
- E3. Confirm the launcher never ships or logs any game data (the repository must stay free of
  the disc, the extracted files and the boot ROM).

### Phase F: stability and performance (AGENTS.md checkpoints)
- C0: title with 0 crash lines and no real pad.
- C1: `RUNS=5 GDB=1 tools/linux/headless_run.sh c1 120`.
- C2: soak of at least 30 minutes with attract mode and a looping input script.
- C3: `IKARUGA_PACING=1` frame-pacing numbers at 1x and 4x on RADV and on lavapipe.
- C4: the user signs off after playing.

### Phase G: housekeeping
- G1. Commit in small steps once the user agrees (nothing is committed yet): renderer, launcher,
  AppImage, docs.
- G2. Correct the README's "works" claims to match what is verified. Move `progress.md` out of
  the repository root, or delete it; it is a 4 MB transcript, not documentation.
- G3. Update `docs/headless.md` with the display-lock tip.

## 5. Order and risk

Do A, then B. Nothing else is worth polishing until the picture is right. The main risk is B2:
depth bugs can need a per-pass rework, not a one-line fix. If the Vulkan code resists, the fallback
is to rewrite the geometry pass directly from `geometry.metal`, line by line.

Windows hardware and the audio and controller checks depend on the user. I will ask for them
at the end of phases D and C.

## 6. Rules I will follow
These come from `AGENTS.md`: every run goes through `tools/linux/headless_run.sh`; always
`--no-gamepads` and `--no-audio`; never touch the user's display, a game they started, or a
controller; announce each batch of runs; no commit or push unless asked.

## 7. Open questions for the user
1. Should the fix target match the Metal output exactly, or is "looks right to a player" enough?
2. Can you run the Windows build on real hardware, and do you have an MSVC setup?
3. May I commit the current uncommitted Linux and Windows work as a baseline before Phase B?
