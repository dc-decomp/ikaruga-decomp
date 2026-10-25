#!/usr/bin/env python3
"""Find Katana SDK library code in the boot executable and name it.

The SDK's GNU libraries (Lib/Gnu/*.a) hold SHC objects converted to ELF, so their code is the same
code the game linked. This reads every object, masks the bytes that relocations fill in, and
searches the executable for the rest:

1. Each object's code section as a whole. The linker places a section in one piece, so a whole
   section match is strong evidence and names every function in it.
2. Each function on its own, for sections that differ as a whole between SDK versions. A function
   must be at least MIN_FUNC bytes and must match at exactly one place.
3. The relocations of every matched function. A literal-pool word that the linker filled with the
   address of a symbol names that symbol, even if its own code did not match. A name needs at
   least two agreeing references, or one from a whole-section match, and no disagreeing one.

    tools/symbols/match_libs.py [LABEL=]DIR ... [--out game/symbols.tsv] [--globals game/globals.tsv]

Each DIR holds ELF libraries (ar archives of any name, such as an SDK's Lib/Gnu/*.a or *.elf.lib)
or ELF objects (*.elf, such as the output of sysrof_to_elf.py), at any depth. With several DIRs, the first one that matches an address
names it, so list the SDK closest to the game first. LABEL goes into the source column as
sdk-LABEL (default: sdk-sdk).

Function names go to symbols.tsv, and SDK data outside the image goes to globals.tsv. Both files
also hold names from other sources, such as the hand-made names with the source "user". A run
rewrites all rows whose source starts with sdk-, and a row from another source wins where an
address has both.
Addresses are written in link space (0x0C...), the spelling dream-recomp's symbols.tsv uses.
"""

from __future__ import annotations

import argparse
import collections
import struct
import sys
from dataclasses import dataclass, field
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
IMAGE = ROOT / "game" / "extracted" / "fs" / "1ST_READ.BIN"
LINK_BASE = 0x0C010000
LOAD_BASE = 0x8C010000
MIN_FUNC = 16
MIN_ANCHOR = 6

SHT_SYMTAB, SHT_RELA, SHT_REL = 2, 4, 9
SHF_EXECINSTR = 0x4
# SH relocation types and the bytes each one fills in. The relaxation markers (R_SH_USES and up)
# fill in nothing.
R_SH_DIR32, R_SH_REL32 = 1, 2
RELOC_BYTES = {1: 4, 2: 4, 3: 2, 4: 2, 5: 2, 6: 2, 7: 2, 8: 2, 9: 2, 25: 2, 26: 4}
RELAX_MARKERS = range(27, 33)


@dataclass
class Reloc:
    offset: int
    rtype: int
    symbol: str
    defined_at: int | None   # section offset when the symbol is in this section, else None
    addend: int


@dataclass
class Section:
    obj: str
    data: bytes
    mask: bytearray
    symbols: list[tuple[int, str]]
    relocs: list[Reloc] = field(default_factory=list)


def ar_members(path: Path):
    data = path.read_bytes()
    if data[:8] != b"!<arch>\n":
        return
    pos, longnames = 8, b""
    while pos + 60 <= len(data):
        hdr = data[pos:pos + 60]
        name = hdr[:16].decode("latin1").rstrip()
        size = int(hdr[48:58].decode("ascii").strip() or "0")
        body = data[pos + 60:pos + 60 + size]
        pos += 60 + size + (size & 1)
        if name in ("/", "/SYM64/"):
            continue
        if name == "//":
            longnames = body
            continue
        if name.startswith("/") and name[1:].isdigit():
            off = int(name[1:])
            name = longnames[off:longnames.index(b"\n", off)].decode("latin1")
        yield name.rstrip("/"), body


def is_ar(path: Path) -> bool:
    with open(path, "rb") as f:
        return f.read(8) == b"!<arch>\n"


def cstr(tab: bytes, off: int) -> str:
    return tab[off:tab.index(b"\0", off)].decode("latin1")


def elf_sections(obj: str, elf: bytes) -> list[Section]:
    if elf[:4] != b"\x7fELF" or struct.unpack_from("<H", elf, 18)[0] != 42:
        return []
    shoff, = struct.unpack_from("<I", elf, 0x20)
    shentsize, shnum, shstrndx = struct.unpack_from("<HHH", elf, 0x2E)
    shdrs = [struct.unpack_from("<10I", elf, shoff + i * shentsize) for i in range(shnum)]

    def sect_bytes(i):
        _, stype, _, _, off, size, *_ = shdrs[i]
        return b"" if stype == 8 else elf[off:off + size]

    symtab = next((i for i, s in enumerate(shdrs) if s[1] == SHT_SYMTAB), None)
    if symtab is None:
        return []
    strtab = sect_bytes(shdrs[symtab][6])
    syms = []
    raw = sect_bytes(symtab)
    for k in range(0, len(raw), 16):
        name, value, _, info, _, shndx = struct.unpack_from("<IIIBBH", raw, k)
        syms.append((cstr(strtab, name) if name else "", value, info & 0xF, shndx))

    out = {}
    for i, s in enumerate(shdrs):
        if s[1] == 1 and s[2] & SHF_EXECINSTR and s[5]:
            named = sorted({(v, n) for n, v, t, sh in syms
                            if sh == i and n and t in (0, 2) and not n.startswith(".")})
            out[i] = Section(obj, sect_bytes(i), bytearray(s[5]), named)
    for s in shdrs:
        if s[1] not in (SHT_RELA, SHT_REL) or s[7] not in out:
            continue
        sec = out[s[7]]
        entsize = 12 if s[1] == SHT_RELA else 8
        raw = sect_bytes(shdrs.index(s))
        for k in range(0, len(raw), entsize):
            r_off, r_info = struct.unpack_from("<II", raw, k)
            addend = struct.unpack_from("<i", raw, k + 8)[0] if entsize == 12 else 0
            rtype, symi = r_info & 0xFF, r_info >> 8
            if rtype in RELAX_MARKERS:
                continue
            n = RELOC_BYTES.get(rtype, 4)
            sec.mask[r_off:r_off + n] = b"\1" * n
            name, value, stype, shndx = syms[symi]
            if stype == 3:   # a section symbol: the target is the section plus the addend
                name = ""
            sec.relocs.append(Reloc(r_off, rtype, name, value if shndx == s[7] else None, addend))
    return list(out.values())


def pattern_find(image: bytes, data: bytes, mask: bytearray, align: int = 2) -> list[int]:
    """Every offset where data matches the image outside the masked bytes."""
    best, start, run = (0, 0), 0, 0
    for i in range(len(data) + 1):
        if i < len(data) and not mask[i]:
            run += 1
            if run > best[0]:
                best = (run, i - run + 1)
        else:
            run = 0
    alen, astart = best
    if alen < MIN_ANCHOR:
        return []
    anchor = data[astart:astart + alen]
    hits, pos = [], image.find(anchor)
    while pos >= 0:
        base = pos - astart
        if base >= 0 and base % align == 0 and base + len(data) <= len(image):
            window = image[base:base + len(data)]
            if all(m or a == b for a, b, m in zip(window, data, mask)):
                hits.append(base)
        pos = image.find(anchor, pos + 1)
    return hits


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("inputs", nargs="+", metavar="[LABEL=]DIR",
                    help="ELF libraries or objects of one SDK, closest SDK first")
    ap.add_argument("--out", type=Path, default=ROOT / "game" / "symbols.tsv")
    ap.add_argument("--globals", type=Path, default=ROOT / "game" / "globals.tsv")
    args = ap.parse_args()

    image = IMAGE.read_bytes()
    sections = []
    for spec in args.inputs:
        label, d = spec.split("=", 1) if "=" in spec and not Path(spec).exists() else ("sdk", spec)
        d = Path(d)
        for lib in sorted(p for p in d.rglob("*") if p.is_file() and is_ar(p)):
            for member, body in ar_members(lib):
                sections += elf_sections(f"sdk-{label} {lib.name}:{member}", body)
        for obj in sorted(d.rglob("*.elf")):
            sections += elf_sections(f"sdk-{label} {obj.parent.name}:{obj.name}", obj.read_bytes())

    names: dict[int, tuple[str, str]] = {}          # image offset -> (name, how)
    claimed: set[int] = set()
    matched: list[tuple[Section, int, bool]] = []   # section, image offset of its start, whole
    whole = partial = ambiguous = 0
    for sec in sections:
        hits = pattern_find(image, sec.data, sec.mask)
        if len(hits) == 1:
            whole += 1
            matched.append((sec, hits[0], True))
            for v, n in sec.symbols:
                names.setdefault(hits[0] + v, (n, sec.obj))
            continue
        if len(hits) > 1:
            ambiguous += 1
            continue
        ends = [v for v, _ in sec.symbols[1:]] + [len(sec.data)]
        for (v, n), end in zip(sec.symbols, ends):
            if end - v < MIN_FUNC:
                continue
            fh = pattern_find(image, sec.data[v:end], sec.mask[v:end])
            if len(fh) == 1 and fh[0] not in claimed:
                partial += 1
                claimed.add(fh[0])
                names.setdefault(fh[0], (n, f"{sec.obj} (function)"))
                # Relocations of this function only, shifted to the section's start.
                piece = Section(sec.obj, sec.data, sec.mask, [],
                                [r for r in sec.relocs if v <= r.offset < end])
                matched.append((piece, fh[0] - v, False))

    votes: dict[str, collections.Counter] = collections.defaultdict(collections.Counter)
    strong: set[str] = set()
    ref_label: dict[str, str] = {}
    for sec, base, is_whole in matched:
        for r in sec.relocs:
            if r.rtype != R_SH_DIR32 or not r.symbol or r.defined_at is not None:
                continue
            word, = struct.unpack_from("<I", image, base + r.offset)
            target = (word - r.addend) & 0xFFFFFFFF
            if not LOAD_BASE <= target < LOAD_BASE + len(image) + 0x1000000:
                continue
            votes[r.symbol][target] += 1
            ref_label.setdefault(r.symbol, sec.obj.split(" ", 1)[0])
            if is_whole:
                strong.add(r.symbol)
    referenced = conflicts = 0
    globals_: dict[int, tuple[str, str]] = {}
    for sym, c in votes.items():
        (target, n), *rest = c.most_common()
        if rest:
            conflicts += 1
            continue
        if n < 2 and sym not in strong:
            continue
        off = target - LOAD_BASE
        if off < len(image):
            if off not in names:
                names[off] = (sym, f"{ref_label[sym]} reference")
                referenced += 1
        else:
            globals_[target] = (sym, f"{ref_label[sym]} reference")

    found = {LINK_BASE + off: (n, how) for off, (n, how) in names.items()}
    kept = write_merged(args.out, found, FUNCTIONS_HEADER)
    gfound = {(a & 0x1FFFFFFF) | 0x0C000000: row for a, row in globals_.items()}
    gkept = write_merged(args.globals, gfound, GLOBALS_HEADER)

    print(f"{len(sections)} code sections in {len(args.inputs)} input(s)")
    print(f"  {whole} matched whole, {ambiguous} matched in more than one place")
    print(f"  {partial} functions matched alone in the other sections")
    print(f"  {referenced} more functions named from relocations, {conflicts} names with conflicting targets")
    print(f"{len(found)} SDK names -> {args.out}, {kept} other rows kept")
    print(f"{len(gfound)} SDK globals -> {args.globals}, {gkept} other rows kept")
    return 0


FUNCTIONS_HEADER = ("# address\tname\tsource  Function names for Ghidra and dream-recomp. Rows whose source\n"
                    "# starts with sdk- are rewritten by tools/symbols/match_libs.py. Add your own with\n"
                    "# the source user.\n")
GLOBALS_HEADER = ("# address\tname\tsource  Data names, for Ghidra only. Rows whose source starts with\n"
                  "# sdk- are rewritten by tools/symbols/match_libs.py. Add your own with the source user. A fourth\n"
                  "# column types the data: a C type from game/types.h or the SDK's headers.\n")


def write_merged(path: Path, found: dict[int, tuple[str, str]], header: str) -> int:
    """Write found to path. Rows not from an SDK stay, with their further columns, and win where an
    address has both."""
    rows = {a: (name, how, []) for a, (name, how) in found.items()}
    kept = 0
    if path.exists():
        for line in path.read_text().splitlines():
            if not line.strip() or line.startswith("#"):
                continue
            addr, name, how, *rest = line.split("\t") + [""] * (3 - len(line.split("\t")))
            if how.startswith("sdk-"):
                continue
            rows[(int(addr, 16) & 0x1FFFFFFF) | 0x0C000000] = (name, how, rest)
            kept += 1
    with open(path, "w") as f:
        f.write(header)
        for addr in sorted(rows):
            name, how, rest = rows[addr]
            f.write("\t".join([f"0x{addr:08x}", name, how, *rest]) + "\n")
    return kept


if __name__ == "__main__":
    sys.exit(main())
