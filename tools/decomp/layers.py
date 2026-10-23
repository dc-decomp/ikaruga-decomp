#!/usr/bin/env python3
"""The game's own code and the SDK libraries linked into it, and the library functions it calls.

    tools/decomp/layers.py [--ghidra ghidra/out]

Each library is linked as one block, so an address decides the layer: the SDK's symbol matches
(game/symbols.tsv) mark where each block starts. Reads the call lists that tools/ghidra/analyze.sh
exports next to each function (c/<address>.json) and prints, as Markdown:

    the functions and bytes of each layer
    the calls from each layer into each other layer
    every library function that the game's code calls directly, with how many game functions call it

The last list is what a native replacement of a library has to provide. Calls through pointers
are not in it: the export lists direct calls only.
"""

from __future__ import annotations

import argparse
import collections
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]

# Where each block starts. The startup code calls the game's main loop, FUN_8C056A20, which is
# linked with the rest of the game's main module between the sound library and the second block of
# Shinobi. The rest of the game's code follows the C runtime, whose last helper, FUN_8C06444E, is
# called from _xtostr. The code ends at 0x8C0CA2F4 (tools/ghidra/DropDataFunctions.java).
LAYERS = [
    (0x8C010000, "startup"),
    (0x8C010138, "ninja"),
    (0x8C030EE0, "kamui"),
    (0x8C03F064, "shinobi"),
    (0x8C04EEA8, "sound"),
    (0x8C056620, "kamui"),  # kmuCalculateKamuiWorkareaSize, Kamui's utility library
    (0x8C056748, "game"),  # the main module
    (0x8C056CF4, "shinobi"),  # backup, memory card file system, fibers and peripherals
    (0x8C061204, "runtime"),  # the SHC C library and its helpers
    (0x8C0644B8, "game"),
    (0x8C0CA2F4, None),
]
ORDER = ["game", "startup", "ninja", "kamui", "shinobi", "sound", "runtime"]


def layer(address: int) -> str | None:
    found = None
    for start, name in LAYERS:
        if address < start:
            break
        found = name
    return found


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--ghidra", type=Path, default=ROOT / "ghidra" / "out")
    args = ap.parse_args()

    functions = {}
    for path in sorted((args.ghidra / "c").glob("*.json")):
        side = json.loads(path.read_text())
        address = int(side["entry"], 16)
        functions[address] = side

    sizes = collections.Counter()
    counts = collections.Counter()
    for path in (args.ghidra / "c").glob("*.c"):
        first = path.read_text().split("\n", 1)[0]  # "// NAME @ 0xADDR, N bytes"
        address = int(path.stem, 16)
        if layer(address) and first.endswith(" bytes"):
            sizes[layer(address)] += int(first.rsplit(",", 1)[1].split()[0])
            counts[layer(address)] += 1

    edges = collections.Counter()
    api = collections.defaultdict(set)  # (library, name, address) -> the game functions calling it
    for address, side in functions.items():
        source = layer(address)
        if not source:
            continue
        for call in side.get("calls", []):
            target = int(call["entry"], 16)
            dest = layer(target)
            if not dest:
                continue
            edges[(source, dest)] += 1
            if source == "game" and dest != "game":
                api[(dest, call["name"], target)].add(side["name"])

    print("## Layers\n")
    print("| Layer | Functions | Bytes |")
    print("| --- | --- | --- |")
    for name in ORDER:
        print(f"| {name} | {counts[name]:,} | {sizes[name]:,} |")

    print("\n## Calls between layers\n")
    print("A cell counts the pairs of a function in the row's layer and one it calls in the column's.\n")
    print("| From \\ to | " + " | ".join(ORDER) + " |")
    print("| --- |" + " --- |" * len(ORDER))
    for source in ORDER:
        print(f"| {source} | " + " | ".join(str(edges[(source, d)] or "") for d in ORDER) + " |")

    print("\n## Library functions the game calls\n")
    for lib in ORDER[1:]:
        rows = sorted((name, target, len(c)) for (l, name, target), c in api.items() if l == lib)
        if not rows:
            continue
        print(f"### {lib} ({len(rows)} functions)\n")
        print("| Function | Address | Game functions calling it |")
        print("| --- | --- | --- |")
        for name, target, callers in rows:
            print(f"| `{name}` | `0x{target:08X}` | {callers} |")
        print()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
