#!/usr/bin/env bash
# compare_set.sh: draw the dumps of render_set.sh with the port's renderer and the reference one.
#
# usage: compare_set.sh OUTDIR [FRAME ...]     (default: every fFRAME.ta in OUTDIR)
#
# For each dump this writes, next to it:
#   fN.view.png    build-port/render-view: the port's renderer (Vulkan here, Metal on a Mac)
#   fN.dream.png   dream-recomp's own renderer (dream_render_view), on a private Xvfb display
#   fN.pair.png    the two side by side, the port's on the right
# and prints how many pixels differ by more than 6% in a channel. dream-recomp's renderer is the
# lineage the Metal one was built from, but it is NOT Metal: where they might differ (bloom, fog,
# the translucent order) a Mac's `render-view` picture of the same dump is the authority.
# Neither tool opens a controller or plays sound. dream_render_view needs a window: it gets its own
# Xvfb (DISPLAY_NUM, default 94), never the user's display.
set -eu
HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"
OUT="$(cd "${1:?usage: compare_set.sh OUTDIR [FRAME ...]}" && pwd)"; shift
VIEW="$REPO/build-port/render-view"; DREAM="$REPO/build-port/dream/render/dream_render_view"
[ -x "$VIEW" ] || { echo "build it: cmake --build build-port --target render-view"; exit 1; }
[ -x "$DREAM" ] || { echo "build it: cmake --build build-port --target dream_render_view"; exit 1; }
if [ $# -eq 0 ]; then set -- $(cd "$OUT" && ls f*.ta | sed 's/^f//; s/\.ta$//' | sort -n); fi
N="${DISPLAY_NUM:-94}"; X=":$N"
[ -e "/tmp/.X11-unix/X$N" ] && { echo "display $X is in use; set DISPLAY_NUM"; exit 1; }
Xvfb "$X" -screen 0 1024x768x24 -nolisten tcp >/dev/null 2>&1 & XP=$!
trap 'kill $XP 2>/dev/null; wait $XP 2>/dev/null' EXIT
for _ in $(seq 1 50); do [ -e "/tmp/.X11-unix/X$N" ] && break; sleep 0.1; done
for f in "$@"; do
  dump="$OUT/f$f.ta"
  "$VIEW" "$dump" "$OUT/f$f.view.png" >/dev/null 2>&1 || { echo "f$f: render-view failed"; continue; }
  env -u WAYLAND_DISPLAY DISPLAY="$X" SDL_VIDEODRIVER=x11 MESA_VK_WSI_DEBUG=sw SDL_JOYSTICK_HIDAPI=0 \
      SDL_GAMECONTROLLER_IGNORE_DEVICES='*' timeout 60 "$DREAM" "$dump" --vram "$dump.vram" \
      --width 640 --height 480 --frames 5 --screenshot "$OUT/f$f.dream.ppm" >/dev/null 2>&1 \
      || { echo "f$f: dream_render_view failed"; continue; }
  convert "$OUT/f$f.dream.ppm" -resize 640x480! "$OUT/f$f.dream.png"
  convert "$OUT/f$f.dream.png" "$OUT/f$f.view.png" +append "$OUT/f$f.pair.png"
  diff=$(compare -metric AE -fuzz 6% "$OUT/f$f.dream.png" "$OUT/f$f.view.png" null: 2>&1 | grep -oE '^[0-9]+' | head -1)
  echo "f$f: $diff of 307200 pixels differ from the reference -> $OUT/f$f.pair.png"
done
