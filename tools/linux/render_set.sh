#!/usr/bin/env bash
# render_set.sh: dump the display lists and take frame-exact screenshots of an autoplay run.
#
# usage: render_set.sh OUTDIR FRAME [FRAME ...]
#
# For each FRAME the game plays itself (IKARUGA_AUTOPLAY, a fresh memory card, unthrottled) up to
# that frame, then OUTDIR gets fFRAME.ta (the display list with video memory and PVR registers, for
# build-port/render-view) and fFRAME.ppm (the 640x480 framebuffer the game shows, with no window,
# desktop or scaling in it). The screenshot is taken at FRAME+2: the game shows each render two
# frames later, and the picture then equals build-port/render-view's of the dump to the pixel. Runs
# go through headless_run.sh, so its safety rules apply.
set -eu
HERE="$(cd "$(dirname "$0")" && pwd)"
OUT="$(mkdir -p "${1:?usage: render_set.sh OUTDIR FRAME...}" && cd "$1" && pwd)"; shift
[ $# -gt 0 ] || { echo "no frames"; exit 1; }
REPO="$(cd "$HERE/../.." && pwd)"
for frame in "$@"; do
  rm -rf "$OUT/saves-$frame"
  EXTRA="--window --disc $REPO/game/disc/disc.gdi --saves $OUT/saves-$frame --scale 1 --unthrottled \
--frames $((frame + 5)) --screenshot-at $((frame + 2)) --screenshot-path $OUT/f$frame.ppm" \
  RUN_ENV="IKARUGA_AUTOPLAY=1 IKARUGA_DUMP_TA=$frame,$OUT/f$frame.ta" \
  OUT_DIR="$OUT/logs" RUNS=1 "$HERE/headless_run.sh" "f$frame" 3000 999999:Return
  [ -s "$OUT/f$frame.ta" ] && [ -s "$OUT/f$frame.ppm" ] || echo "f$frame: MISSING dump or screenshot"
done
