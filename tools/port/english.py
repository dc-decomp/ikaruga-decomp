#!/usr/bin/env python3
"""Lists the game's Japanese lines with the hashes that ENGLISH TEXT knows them by.

    tools/port/english.py [--program game/extracted/1ST_READ.plain.bin] [--missing]

port/src/text.cpp keys its English lines by the FNV-1a hash of each Japanese line's Shift-JIS
bytes, so that the repository holds none of the game's text. This script reads the lines from the
disc's program, which stays local, and prints each one's address, hash, Japanese and English.
--missing prints only the lines that have no English yet.
"""

from __future__ import annotations

import argparse
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
BASE = 0x8C010000
# The game's lines lie between these addresses of the program.
FIRST, LAST = 0x8C0D0A00, 0x8C0D3B00


def fnv1a(data: bytes) -> int:
    h = 0x811C9DC5
    for b in data:
        h = ((h ^ b) * 0x01000193) & 0xFFFFFFFF
    return h


def lead(b: int) -> bool:
    return 0x81 <= b <= 0x9F or 0xE0 <= b <= 0xEF


def lines(program: bytes) -> list[tuple[int, int, str]]:
    """Each line of double-byte characters before a 0 byte: from the 0 back over whole characters."""
    out = []
    lo, hi = FIRST - BASE, LAST - BASE
    for end in range(lo, hi):
        if program[end] != 0:
            continue
        start = end
        while start - 2 >= lo and lead(program[start - 2]) and 0x40 <= program[start - 1] <= 0xFC and program[start - 1] != 0x7F:
            start -= 2
        if end - start < 4:
            continue
        try:
            text = program[start:end].decode("shift_jis")
        except UnicodeDecodeError:
            continue
        out.append((BASE + start, fnv1a(program[start:end]), text))
    return out


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--program", type=Path, default=ROOT / "game" / "extracted" / "1ST_READ.plain.bin")
    ap.add_argument("--missing", action="store_true")
    args = ap.parse_args()
    known = {int(h, 16): e for h, e in re.findall(r'\{0x([0-9a-f]{8})u, "((?:[^"\\]|\\.)*)"\}',
                                                   (ROOT / "port" / "src" / "text.cpp").read_text())}
    for address, h, text in lines(args.program.read_bytes()):
        english = known.get(h)
        if args.missing and english is not None:
            continue
        print(f"{address:08x} {h:08x} {text}\n                  {english if english is not None else '(none)'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
