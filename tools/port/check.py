#!/usr/bin/env python3
"""Plays the port without a recording and checks that the game gets to its milestones.

    tools/port/check.py [--binary build-port/ikaruga] [--frames N]
    tools/port/check.py --pictures DIR [--keep]

A recording replays only in the build that made it: when a change moves the game's timing, a
replay goes somewhere else and proves nothing. This check uses the launcher's autoplay instead
(IKARUGA_AUTOPLAY): it gets through the menus with their defaults, then shoots and keeps two lives.
A stage that stops, for example because its script waits for a scroll position that a rounding
change moved, fails here.

Each milestone is the first call of a function (IKARUGA_WATCH_CALLS), and it has to come by a
given frame. The check also fails when the game stops drawing, which a hang does. Exit status 0
means every milestone came in time.

The run plays the whole game, in about 15 minutes. Autoplay turns autosave on, so the game saves
to the memory card (--saves, a temporary directory) on its way back to the title. A second run
boots with that card. Only the ending sets the game-clear flags, so they must be there before the
stage starts. --frames N checks only the milestones due by frame N: --frames 18000 takes two
minutes.

With --pictures the run opens a window and takes a screenshot a fixed number of frames after
some of the milestones (IKARUGA_SHOTS_AFTER), so the pictures show the same scenes when a change
moves the timing. They go to DIR/new. When DIR/kept holds an earlier set, the check prints how many
pixels of each picture differ from it, and how many by more than 16 in a channel. It writes
DIR/compare.png with the kept pictures beside the new ones, at a quarter of their size.
The pictures do not fail the check: a missing effect is for a person to see. --keep makes the new
set the kept one. The pictures come from the game, so DIR belongs outside the repository.
"""

from __future__ import annotations

import argparse
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]

# (what, the function whose first call marks it, the frame it must come by). The frames are those
# of an autoplay run with about 10% added.
MILESTONES = [
    ("stage 1 boss", 0x8C08FE04, 12000),  # the boss's state machine
    ("stage 2", 0x8C096644, 18000),  # loads S02_BG00
    ("stage 2 boss", 0x8C09668C, 32000),  # loads S02_BOSS
    ("stage 3", 0x8C0A157C, 40000),  # loads S03_OBJ01
    ("stage 3 boss", 0x8C0A1594, 45000),  # loads S03_BOSS
    ("stage 3 ring", 0x8C0A1624, 52000),  # the boss's second part
    ("stage 4", 0x8C0AD494, 60000),  # scene 9
    ("stage 5", 0x8C0B7A08, 87000),  # scene 10
    ("ending", 0x8C0C9F98, 116000),  # scene 12, which sets the game-clear flags
]

# The game's file on the card, the frame by which the game has saved after the ending (on the way
# back to the title, with autosave on), the game-clear flags that only the ending sets, and the
# frames that a boot takes to load them.
SAVE_FILE = "IKARUGA_DATA.VMS"
SAVE_BY = 122000
CLEAR_FLAGS = 0x8C1CD778
LOAD_FRAMES = 1500

# (name, the milestone's function, frames after its first call)
PICTURES = [
    ("stage 1 boss", 0x8C08FE04, 600),
    ("stage 2", 0x8C096644, 1500),
    ("stage 2 boss", 0x8C09668C, 900),
    ("stage 3", 0x8C0A157C, 1500),
    ("stage 3 boss", 0x8C0A1594, 600),
    ("stage 3 ring", 0x8C0A1624, 700),
    ("stage 4", 0x8C0AD494, 1500),
    ("stage 5", 0x8C0B7A08, 1500),
    ("ending", 0x8C0C9F98, 600),
]


def read_ppm(path: Path) -> tuple[int, int, bytes]:
    data = path.read_bytes()
    fields, i = [], 0
    while len(fields) < 4:
        while data[i:i + 1].isspace():
            i += 1
        j = i
        while not data[j:j + 1].isspace():
            j += 1
        fields.append(data[i:j])
        i = j
    width, height = int(fields[1]), int(fields[2])
    return width, height, data[i + 1:i + 1 + width * height * 3]


def quarter(width: int, height: int, rgb: bytes) -> tuple[int, int, bytearray]:
    w, h = width // 4, height // 4
    out = bytearray(w * h * 3)
    for y in range(h):
        row = (y * 4) * width * 3
        for x in range(w):
            o = row + x * 12
            out[(y * w + x) * 3:(y * w + x) * 3 + 3] = rgb[o:o + 3]
    return w, h, out


def write_png(path: Path, width: int, height: int, rgb: bytes) -> None:
    raw = b"".join(b"\0" + rgb[y * width * 3:(y + 1) * width * 3] for y in range(height))

    def chunk(tag: bytes, body: bytes) -> bytes:
        return struct.pack(">I", len(body)) + tag + body + struct.pack(">I", zlib.crc32(tag + body) & 0xFFFFFFFF)

    path.write_bytes(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
                     + chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b""))


def picture_file(address: int, frames: int) -> str:
    return f"shot-{address:08x}+{frames}.ppm"


def compare(directory: Path) -> None:
    rows = []
    for name, address, frames in PICTURES:
        kept, new = directory / "kept" / picture_file(address, frames), directory / "new" / picture_file(address, frames)
        if not new.exists():
            print(f"     picture {name}: not taken")
            continue
        if not kept.exists():
            print(f"     picture {name}: nothing kept to compare with")
            continue
        a, b = read_ppm(kept), read_ppm(new)
        if a[:2] != b[:2]:
            print(f"     picture {name}: the size changed")
            continue
        # Every pixel counts: a missing bullet is a few pixels, which an average hides.
        if a[2] == b[2]:
            print(f"     picture {name}: the same as the kept one")
        else:
            x, y = a[2], b[2]
            changed = over = 0
            for k in range(0, len(x), 3):
                d = max(abs(x[k] - y[k]), abs(x[k + 1] - y[k + 1]), abs(x[k + 2] - y[k + 2]))
                changed += d > 0
                over += d > 16
            mean = sum(abs(p - q) for p, q in zip(x, y)) / len(x)
            print(f"     picture {name}: {changed} pixels differ from the kept one, {over} by more than 16 in a "
                  f"channel, {mean:.3f} of 255 on average")
        rows.append((quarter(*a), quarter(*b)))
    if not rows:
        return
    w, h = rows[0][0][0], rows[0][0][1]
    out = bytearray(2 * w * h * 3 * len(rows))
    for r, (a, b) in enumerate(rows):
        for y in range(h):
            line = ((r * h + y) * 2 * w) * 3
            out[line:line + w * 3] = a[2][y * w * 3:(y + 1) * w * 3]
            out[line + w * 3:line + 2 * w * 3] = b[2][y * w * 3:(y + 1) * w * 3]
    write_png(directory / "compare.png", 2 * w, h * len(rows), bytes(out))
    print(f"     {directory / 'compare.png'}: kept on the left, new on the right")


def run(args: argparse.Namespace, card: Path, frames: int, env: dict[str, str], window: list[str]) -> str:
    cmd = [str(args.binary), "--unthrottled", "--no-audio", "--frames", str(frames),
           "--saves", str(card)]
    bios = ROOT / "bios" / "dc_bios.bin"
    if bios.exists():
        cmd += ["--bios", str(bios)]
    done = subprocess.run(cmd + window, cwd=ROOT, env={**os.environ, "IKARUGA_AUTOPLAY": "1", **env},
                          capture_output=True, text=True)
    return done.stdout + done.stderr


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--binary", type=Path, default=ROOT / "build-port" / "ikaruga")
    ap.add_argument("--frames", type=int, default=max(SAVE_BY, *(limit for _, _, limit in MILESTONES)))
    ap.add_argument("--pictures", type=Path, help="take and compare screenshots in this directory")
    ap.add_argument("--keep", action="store_true", help="keep the new pictures for later comparisons")
    args = ap.parse_args()

    milestones = [m for m in MILESTONES if m[2] <= args.frames]
    env = {"IKARUGA_WATCH_CALLS": ",".join(f"0x{address:08x}" for _, address, _ in milestones)}
    window = []
    if args.pictures:
        new = args.pictures / "new"
        shutil.rmtree(new, ignore_errors=True)
        new.mkdir(parents=True)
        env["IKARUGA_SHOTS_AFTER"] = ",".join(f"0x{a:08x}+{n}" for _, a, n in PICTURES)
        window = ["--window", "--screenshot-path", str(new / "shot.ppm")]
    with tempfile.TemporaryDirectory() as tmp:
        card = Path(tmp)
        out = run(args, card, args.frames, env, window)
        saved = (card / SAVE_FILE).exists()
        # The game loads its file at the boot. Only the ending sets the game-clear flags, so flags
        # that are there before the stage starts came from the card.
        loaded = None
        if saved and args.frames >= SAVE_BY:
            flags = run(args, card, LOAD_FRAMES, {"IKARUGA_PEEK": f"0x{CLEAR_FLAGS:08x}:2"}, [])
            loaded = re.search(rf"^peek frame \d+: 0x{CLEAR_FLAGS:08x} = 0x0*[1-9a-f]", flags, re.M) is not None

    first = {int(a, 16): int(f) for a, f in re.findall(r"^first call of (0x[0-9a-f]+) at frame (\d+)$", out, re.M)}
    failed = False
    for what, address, limit in milestones:
        frame = first.get(address)
        if frame is None or frame > limit:
            print(f"FAIL {what}: {'not reached' if frame is None else f'frame {frame}'}, needed by frame {limit}")
            failed = True
        else:
            print(f"ok   {what}: frame {frame} (needed by {limit})")

    m = re.search(r"^renders (\d+)", out, re.M)
    renders = int(m.group(1)) if m else 0
    if renders < args.frames - 100:
        print(f"FAIL drawing: {renders} renders in {args.frames} frames")
        failed = True
    else:
        print(f"ok   drawing: {renders} renders in {args.frames} frames")

    if args.frames >= SAVE_BY:
        if not saved:
            print("FAIL save: the memory card holds no game file")
            failed = True
        elif not loaded:
            print("FAIL load: a boot with the saved card did not get the game-clear flags back")
            failed = True
        else:
            print("ok   save and load: the card holds the game file, and a boot gets its flags back")

    if args.pictures:
        compare(args.pictures)
        if args.keep:
            kept = args.pictures / "kept"
            shutil.rmtree(kept, ignore_errors=True)
            shutil.copytree(args.pictures / "new", kept)
            print(f"     kept the new pictures in {kept}")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
