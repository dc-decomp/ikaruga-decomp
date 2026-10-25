#!/usr/bin/env python3
"""Print a chapter's stage program or a stage script, with the names from game/symbols.tsv and
game/globals.tsv. docs/game.md describes both.

    tools/game/stage.py program 1          # chapter 1's stage program (1 to 5, or tutorial)
    tools/game/stage.py script 0x8c0d4d98  # the stage script at this address

It reads game/extracted/1ST_READ.plain.bin, which dcdisc writes from your disc.
"""
import argparse
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
BASE = 0x8C010000

# The stage program that each chapter's start gives stageStart.
PROGRAMS = {"1": 0x8C0D4234, "2": 0x8C0DAFB8, "3": 0x8C0E1868, "4": 0x8C0E736C, "5": 0x8C0EAA30,
            "tutorial": 0x8C108078}

# The stage commands: name, and the kinds of their words after the number. "f" is a float in units
# of 1/10,000, "p" an address, "w" a word, "c" a function.
COMMANDS = {
    0: ("stop", ""), 1: ("path", "p"), 2: ("lock", ""), 3: ("next phase", ""),
    4: ("wait lock", "w"), 5: ("wait scroll", "f"), 6: ("next part", ""), 7: ("end", ""),
    8: ("call", "c"), 9: ("call", "cw"), 10: ("call", "cww"), 11: ("call", "cwww"),
    12: ("call", "cwwww"), 13: ("call at", "fc"), 14: ("call at", "fcw"), 15: ("call at", "fcww"),
    16: ("call at", "fcwww"), 17: ("call at", "fcwwww"), 18: ("set float", "pf"),
    19: ("set float at", "fpf"), 20: ("ease float", "wpwf"), 21: ("ease float at", "fwpwf"),
    22: ("set word", "pw"), 23: ("set word at", "fpw"), 24: ("ease word", "wpww"),
    25: ("ease word at", "fwpww"), 26: ("stop scroll", "ff"),
}

# A stage script's condition flags.
FLAGS = [(0x0001, "run after scroll"), (0x0002, "branch after scroll"), (0x0004, "run after frames"),
         (0x0008, "branch after frames"), (0x2000, "run after script frames"),
         (0x1000, "branch after script frames"), (0x0080, "run when group empty"),
         (0x0800, "add to group")]


def names() -> dict[int, str]:
    out = {}
    for tsv in ("game/symbols.tsv", "game/globals.tsv"):
        for line in (ROOT / tsv).read_text().splitlines():
            if not line or line.startswith("#"):
                continue
            addr, name = line.split("\t")[:2]
            out[(int(addr, 16) & 0x1FFFFFFF) | 0x80000000] = name.lstrip("_")
    return out


class Image:
    def __init__(self, path: Path):
        self.data = path.read_bytes()

    def word(self, address: int) -> int:
        return struct.unpack_from("<i", self.data, address - BASE)[0]

    def bytes(self, address: int, n: int) -> bytes:
        return self.data[address - BASE:address - BASE + n]


def show(value: int, kind: str, named: dict[int, str]) -> str:
    if kind == "f":
        return f"{value / 10000:g}"
    if kind in "pc":
        a = value & 0xFFFFFFFF
        return named.get(a, f"0x{a:08x}")
    return str(value)


def program(image: Image, start: int, named: dict[int, str]) -> None:
    at = start
    while True:
        op = image.word(at)
        if op not in COMMANDS:
            sys.exit(f"0x{at:08x}: no command {op}")
        name, kinds = COMMANDS[op]
        words = [image.word(at + 4 * (i + 1)) for i in range(len(kinds))]
        args = ", ".join(show(v, k, named) for v, k in zip(words, kinds))
        print(f"0x{at:08x}  {op:2}  {name:14} {args}")
        if op in (0, 7):
            return
        at += 4 * (len(kinds) + 1)


def script(image: Image, start: int, named: dict[int, str]) -> None:
    at = start
    while True:
        flags, step, branch, value, function = struct.unpack("<HBBiI", image.bytes(at, 12))
        if flags & 0x8000:
            print(f"0x{at:08x}  end")
            return
        conditions = []
        for bit, text in FLAGS:
            if flags & bit:
                group = (flags >> 4) & 3 if bit == 0x0080 else (flags >> 8) & 3 if bit == 0x0800 else None
                conditions.append(text if group is None else f"{text} {group}")
        data = image.bytes(at + 12, 64).rstrip(b"\0").hex()
        print(f"0x{at:08x}  {', '.join(conditions) or 'wait'}: {value}, next {step}, branch {branch}, "
              f"{show(function, 'c', named)} {data}")
        at += 0x4C


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    sub = ap.add_subparsers(dest="what", required=True)
    p = sub.add_parser("program", help="a chapter's stage program")
    p.add_argument("chapter", choices=sorted(PROGRAMS))
    s = sub.add_parser("script", help="the stage script at an address")
    s.add_argument("address", type=lambda v: int(v, 0))
    ap.add_argument("--image", type=Path, default=ROOT / "game/extracted/1ST_READ.plain.bin")
    args = ap.parse_args()
    image, named = Image(args.image), names()
    if args.what == "program":
        program(image, PROGRAMS[args.chapter], named)
    else:
        script(image, args.address, named)
    return 0


if __name__ == "__main__":
    sys.exit(main())
