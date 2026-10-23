#!/usr/bin/env python3
"""Draws a set of display lists with the port's renderer and compares the pictures with kept ones.

    tools/port/render_check.py --make [--binary build-port/ikaruga]
    tools/port/render_check.py [--view build-port/render-view] [--keep]

A change to the renderer shows here in seconds, without playing through the game. The set is a
folder of display lists with video memory and the PowerVR registers, as IKARUGA_DUMP_TA writes
them (runs/render-set, or --set DIR). --make plays the game headless with autoplay and dumps one
display list at each frame in FRAMES, from the title to the name entry. The dumps come from the
game, so the folder belongs outside the repository; runs/ is ignored.

Without --make, the check draws each dump with render-view (cmake --build build-port --target
render-view) into DUMP.new.png and compares it with DUMP.kept.png. It prints the mean difference
of each picture in steps of 255, and how many pixels differ by more than 16 in a channel. --keep
makes the new pictures the kept ones. A picture without a kept one is reported as new. Metal needs
the window server, so this does not run in a sandbox that blocks it.
"""

from __future__ import annotations

import argparse
import os
import struct
import subprocess
import sys
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]

# The title's game mode menu, each stage and its boss, stage 4's chapter screen, and the name entry.
FRAMES = [700, 3000, 9500, 30000, 42000, 56040, 60000, 85000, 107000]


def read_png(path: Path) -> tuple[int, int, bytes]:
    data = path.read_bytes()
    i, idat, width, height = 8, b"", 0, 0
    while i < len(data):
        n = struct.unpack(">I", data[i:i + 4])[0]
        tag, body = data[i + 4:i + 8], data[i + 8:i + 8 + n]
        if tag == b"IHDR":
            width, height = struct.unpack(">II", body[:8])
        elif tag == b"IDAT":
            idat += body
        i += 12 + n
    raw = zlib.decompress(idat)
    stride = 1 + width * 3
    # render-view writes every row with filter 0, so a row is its bytes after the filter byte.
    return width, height, b"".join(raw[y * stride + 1:(y + 1) * stride] for y in range(height))


def make(args: argparse.Namespace) -> int:
    args.set.mkdir(parents=True, exist_ok=True)
    bios = ROOT / "bios" / "dc_bios.bin"
    # One run for each frame, all at once: autoplay plays the same game in each.
    runs = []
    for frame in FRAMES:
        dump = args.set / f"f{frame}.ta"
        cmd = [str(args.binary), "--unthrottled", "--no-audio", "--frames", str(frame + 5),
               "--saves", str(args.set / f"saves-{frame}")]
        if bios.exists():
            cmd += ["--bios", str(bios)]
        env = {**os.environ, "IKARUGA_AUTOPLAY": "1", "IKARUGA_DUMP_TA": f"{frame},{dump}"}
        runs.append((dump, subprocess.Popen(cmd, cwd=ROOT, env=env, stdout=subprocess.DEVNULL,
                                            stderr=subprocess.DEVNULL)))
    failed = False
    for dump, run in runs:
        run.wait()
        failed |= not dump.exists()
        print(f"{'ok  ' if dump.exists() else 'FAIL'} {dump.name}")
    return 1 if failed else 0


def check(args: argparse.Namespace) -> int:
    # In the order of the game: f700 before f3000.
    dumps = sorted(args.set.glob("*.ta"), key=lambda d: (len(d.stem), d.stem))
    if not dumps:
        print(f"no dumps in {args.set}: run with --make first")
        return 1
    failed = False
    for dump in dumps:
        new, kept = dump.with_suffix(".new.png"), dump.with_suffix(".kept.png")
        done = subprocess.run([str(args.view), str(dump), str(new)], capture_output=True, text=True)
        if done.returncode != 0 or not new.exists():
            print(f"FAIL {dump.name}: {done.stderr.strip() or done.stdout.strip()}")
            failed = True
            continue
        timing = next((line for line in done.stdout.splitlines() if line.startswith("render:")), "")
        if not kept.exists():
            print(f"new  {dump.name}: nothing kept to compare with. {timing}")
        else:
            a, b = read_png(kept), read_png(new)
            if a[:2] != b[:2]:
                print(f"     {dump.name}: the size changed")
            else:
                diff = [abs(x - y) for x, y in zip(a[2], b[2])]
                over = sum(1 for k in range(0, len(diff), 3) if max(diff[k:k + 3]) > 16)
                print(f"     {dump.name}: {sum(diff) / len(diff):.2f} of 255 on average, {over} pixels "
                      f"differ by more than 16. {timing}")
        if args.keep:
            new.replace(kept)
    return 1 if failed else 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--set", type=Path, default=ROOT / "runs" / "render-set")
    ap.add_argument("--make", action="store_true", help="dump the display lists of the set")
    ap.add_argument("--binary", type=Path, default=ROOT / "build-port" / "ikaruga")
    ap.add_argument("--view", type=Path, default=ROOT / "build-port" / "render-view")
    ap.add_argument("--keep", action="store_true", help="keep the new pictures for later comparisons")
    args = ap.parse_args()
    return make(args) if args.make else check(args)


if __name__ == "__main__":
    sys.exit(main())
