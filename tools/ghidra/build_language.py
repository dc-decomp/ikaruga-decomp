#!/usr/bin/env python3
"""Build and install the SuperH4:LE:32:katana Ghidra language.

Ghidra's SH-4 language gets the FPU wrong in several ways that Katana code runs into:

- fschg flips FPSCR.SZ in FPSCR but not in the FPSCR_SZ register that every fmov tests, so the
  64-bit pair moves between two fschg decompile as 32-bit moves.
- frchg flips FPSCR.FR without swapping the fr and xf banks, so a matrix loaded into the back bank
  for ftrv lands in fr0-fr15 and ftrv reads uninitialised xf registers.

- In little-endian mode the xf bank is not pair-swapped like fr, and fipr and ftrv address vector
  elements in the wrong order (see XF_LAYOUT_OLD and swap_vector_elements).
- The calling convention offers double pairs that overlap the single-float argument registers.
- pref is an empty instruction, so the store-queue flushes that send data to the TA disappear.
- Writes to SR, GBR and VBR are dead to the decompiler, so interrupt masking disappears.
- Pair moves (fmov with FPSCR.SZ set) put the odd register at the lower address.

This copies the installed SuperH4 language, patches all of this, and installs the result as
a Ghidra extension in the settings directory that tools/ghidra/analyze.sh gives Ghidra. The stock
language stays untouched. The patch asserts the exact text it replaces, so a Ghidra upgrade that
changes either instruction fails here instead of silently producing an unpatched language.

    tools/ghidra/build_language.py
"""

from __future__ import annotations

import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
GHIDRA = Path(os.environ.get("GHIDRA_HOME", "/opt/homebrew/opt/ghidra/libexec"))


def ghidra_properties() -> dict[str, str]:
    lines = (GHIDRA / "Ghidra" / "application.properties").read_text().splitlines()
    return dict(line.split("=", 1) for line in lines if "=" in line)


# The oldest JDK that Ghidra runs on, which Homebrew installs with it.
JAVA_HOME = os.environ.get(
    "JAVA_HOME",
    f"/opt/homebrew/opt/openjdk@{ghidra_properties()['application.java.min']}/libexec/openjdk.jdk/Contents/Home")
NAME = "SuperH4Katana"

# Little-endian SH-4 keeps a double in a register pair with the even register as the high half.
# Ghidra stores fr pair-swapped for that (fr1 fr0 fr3 fr2 ...), but not xf, so every xd pair and every
# 8-byte move between the banks swapped its halves.
XF_LAYOUT_OLD = """\
	fr1  fr0  fr3  fr2  fr5  fr4  fr7  fr6  fr9  fr8  fr11 fr10 fr13 fr12 fr15 fr14
	xf0  xf1  xf2  xf3  xf4  xf5  xf6  xf7  xf8  xf9  xf10 xf11 xf12 xf13 xf14 xf15"""
XF_LAYOUT_NEW = """\
	fr1  fr0  fr3  fr2  fr5  fr4  fr7  fr6  fr9  fr8  fr11 fr10 fr13 fr12 fr15 fr14
	xf1  xf0  xf3  xf2  xf5  xf4  xf7  xf6  xf9  xf8  xf11 xf10 xf13 xf12 xf15 xf14"""

# fipr and ftrv address the elements of a vector register by byte offset. In the pair-swapped
# layout, element i is at offset (i ^ 1) * 4: ftrv multiplied the matrix with a permuted vector and
# stored the result permuted, and fipr stored its dot product in FR[n+2] instead of FR[n+3].
VECTOR_ELEMENT = re.compile(r"\(&:4 (FV[NM]_2t) \+\s*(\d+)\)")


def swap_vector_elements(sinc: str) -> str:
    def element(m: re.Match) -> str:
        return f"(&:4 {m.group(1)} + {int(m.group(2)) ^ 4:2d})"
    out, n = VECTOR_ELEMENT.subn(element, sinc)
    if n != 29:  # fipr: 9, ftrv: 20
        sys.exit(f"vector elements: expected 29 in fipr and ftrv, found {n}; review the patch")
    return out


FSCHG_OLD = """\
	if (!( $(FPSCR_PR) == 0 )) goto <skip>;
	FPSCR = FPSCR ^ 0x00100000;
	<skip>"""
FSCHG_NEW = """\
	if (!( $(FPSCR_PR) == 0 )) goto <skip>;
	FPSCR = FPSCR ^ 0x00100000;
	$(FPSCR_SZ) = !$(FPSCR_SZ);
	<skip>"""

# pref to the store-queue area is how Katana code sends 32 bytes to the TA or to texture memory.
# As an empty instruction the decompiler drops it, and the stores before it look like plain memory
# writes. As a call it stays in the C, where a port can flush the queue.
PREF_OLD = ":pref N_0tjmp               is OP_0=0x0 & N_0tjmp & OP_4=0x83 { } # Empty on purpose"
PREF_NEW = ("define pcodeop prefetch;\n"
            ":pref N_0tjmp               is OP_0=0x0 & N_0tjmp & OP_4=0x83 { prefetch(N_0tjmp); }")

# fmac rounds once: the SH-4's multiply-add unit is fused. As f* and f+ the C shows a multiply and an
# add, and a port compiled without contraction rounds twice. A call keeps the single rounding.
FMAC_OLD = "\tFRN_0t = ( FR0_t f* FRM_0t ) f+ FRN_0t;\n"
FMAC_NEW = "\tFRN_0t = fmac(FR0_t, FRM_0t, FRN_0t);\n"

# fmov with FPSCR.SZ set moves a register pair as two 32-bit words, the even register at the lower
# address. Ghidra stores fr pair-swapped (fr1 fr0 ...), so its 8-byte access of a pair put the odd
# register first: Kamui's vertex writer loads a vertex into fr0-fr6 and stores the pairs, and the
# TA got every word pair swapped. Each pair access swaps the two words. It stays one 8-byte access:
# fmov.s shares its constructor with the pair form, and the analyzer follows both forms. A second
# 4-byte read at address + 4 makes it put data on the code after every float constant pool.
def swapped(v: str) -> str:
    return f"(({v} << 32) | ({v} >> 32))"


PAIR_MOVES = {
    "XDRN = *:8 M_0t_at1;": f"local pair:8 = *:8 M_0t_at1; XDRN = {swapped('pair')};",
    "*:8 N_0t_at1 = XDRM;": f"*:8 N_0t_at1 = {swapped('XDRM')};",
    "XDRN = *:8 M_0t_at;": f"local pair:8 = *:8 M_0t_at; XDRN = {swapped('pair')};",
    "*:8 N_0t_at_neg = XDRM;": f"*:8 N_0t_at_neg = {swapped('XDRM')};",
    "XDRN = *:8 (r0 + M_0t_at_with_r0);": f"local pair:8 = *:8 (r0 + M_0t_at_with_r0); XDRN = {swapped('pair')};",
    "*:8 (r0 + N_0t_at_with_r0) = XDRM;": f"*:8 (r0 + N_0t_at_with_r0) = {swapped('XDRM')};",
}


def pair_moves_in_order(sinc: str) -> str:
    for old, new in PAIR_MOVES.items():
        if sinc.count(old) != 1:
            sys.exit(f"pair move {old!r}: expected exactly one; review the patch")
        sinc = sinc.replace(old, new)
    return sinc


# ldc to SR, GBR and VBR write registers that nothing in the function reads afterwards, so the
# decompiler drops the writes: the C loses every change of the interrupt mask and of GBR. A call
# for each keeps them, so a port can follow them. lds to FPSCR is the same: syHwInitSub sets
# rounding to nearest, and without the call the C kept the boot default, rounding toward zero.
CONTROL_LOADS = {
    ":ldc N_0t_sr ": ("setSR", "N_0t_sr"),
    ":ldc N_0t_gbr ": ("setGBR", "N_0t_gbr"),
    ":ldc N_0t_vbr ": ("setVBR", "N_0t_vbr"),
    ":ldc.l N_0t_sr1 ": ("setSR", "*:4 N_0t_sr1"),
    ":ldc.l N_0t_gbr1 ": ("setGBR", "*:4 N_0t_gbr1"),
    ":ldc.l N_0t_vbr1 ": ("setVBR", "*:4 N_0t_vbr1"),
    ":lds N_0t_fpscr ": ("setFPSCR", "N_0t_fpscr"),
    ":lds.l N_0t_fpscr1 ": ("setFPSCR", "*:4 N_0t_fpscr1"),
}


def visible_control_loads(sinc: str) -> str:
    # The language file must start with its endianness; declare the ops before the first ldc.
    first = sinc.index(":ldc N_0t_sr ")
    out = (sinc[:first] + "define pcodeop setSR;\ndefine pcodeop setGBR;\ndefine pcodeop setVBR;\n"
           + "define pcodeop setFPSCR;\n"
           + sinc[first:])
    for head, (op, value) in CONTROL_LOADS.items():
        i = out.find(head)
        if i < 0 or out.count(head) != 1:
            sys.exit(f"{head.strip()}: expected exactly one constructor; review the patch")
        brace = out.index("{", i) + 1
        # Read the value before the constructor's own p-code changes the register it comes from.
        out = out[:brace] + f"\n\t{op}({value});" + out[brace:]
    return out


FRCHG_OLD = """\
	if (!( $(FPSCR_PR) == 0 )) goto <skip>;
	$(FPSCR_FR) = !$(FPSCR_FR);
	FPSCR = FPSCR ^ 0x00200000;
	<skip>"""
# Swapping the register contents is equivalent to switching which bank the names refer to.
FRCHG_NEW = FRCHG_OLD.replace(
    "\t<skip>",
    "".join(f"\tbank = fr{i}; fr{i} = xf{i}; xf{i} = bank;\n" for i in range(16)) + "\t<skip>",
).replace("\tif (", "\tlocal bank:4 = 0;\n\tif (", 1)

LDEFS = """\
<?xml version="1.0" encoding="UTF-8"?>
<language_definitions>
  <language processor="SuperH4"
            endian="little"
            size="32"
            variant="katana"
            version="1.01"
            slafile="SuperH4_katana.sla"
            processorspec="SuperH4_katana.pspec"
            id="SuperH4:LE:32:katana">
    <description>SuperH-4 little endian, with fschg and frchg modelled (Dreamcast Katana)</description>
    <compiler name="default" spec="SuperH4_katana.cspec" id="default"/>
  </language>
</language_definitions>
"""

# Katana code runs single precision with 32-bit fmov and the front bank selected. Pinning these at
# every function entry lets the decompiler resolve each FPU instruction to one width. The stock set
# is declared for the register space, where no code lives, so it never applied; it has to be ram.
PSPEC_TRACKED_OLD = """\
    <tracked_set space="register">
      <set name="FPSCR" val="0x40001"/>"""
PSPEC_TRACKED_NEW = """\
    <tracked_set space="ram">
      <set name="FPSCR" val="0x40001"/>
      <set name="FPSCR_PR" val="0"/>
      <set name="FPSCR_SZ" val="0"/>
      <set name="FPSCR_FR" val="0"/>"""


# The SH ABI preserves fr12-fr15 across calls, and Katana code never returns with a different FPU
# mode. Without these the decompiler assumes every call may change the mode and keeps both widths of
# each fmov after it.
CSPEC_UNAFFECTED_OLD = """\
        <register name="r8"/>
      </unaffected>"""
CSPEC_UNAFFECTED_NEW = CSPEC_UNAFFECTED_OLD.replace(
    "      </unaffected>",
    "".join(f'        <register name="{r}"/>\n'
            for r in ("fr12", "fr13", "fr14", "fr15", "FPSCR_PR", "FPSCR_SZ", "FPSCR_FR"))
    + "      </unaffected>")

# The Ninja library keeps the current matrix in the xf bank (XMTRX) from one call to the next. As
# plain registers the decompiler drops every write to them at function exit as dead.
CSPEC_GLOBAL_OLD = """\
  <global>
    <range space="ram"/>
  </global>"""
CSPEC_GLOBAL_NEW = CSPEC_GLOBAL_OLD.replace(
    "  </global>",
    "".join(f'    <register name="xf{i}"/>\n' for i in range(16)) + "  </global>")

# Katana code is compiled single precision (SHC -fpu=single), so no argument is a double. The stock
# list also offers the pairs dr4-dr10 for doubles, overlapping fr4-fr11. With the pair-swapped
# little-endian layout, the decompiler then places a 4-byte float in the wrong half of its pair:
# njTranslate(m, x, y, z) read x and y both from fr5. Without the pairs, fr4-fr11 map one to one.
CSPEC_DOUBLE_ARGS_OLD = "".join(f"""\
      <pentry minsize="5" maxsize="8" metatype="float">
        <register name="dr{n}"/>
      </pentry>
""" for n in (4, 6, 8, 10))
CSPEC_DOUBLE_ARGS_NEW = ""


# SHC knows which registers a function in the same unit leaves alone, and a caller keeps a value in
# r1, r2 or r3 across a call to such a function. The default convention kills r0-r3 at every call,
# so the decompiler reads such a value as extraout_r3, which the C++ does not have. These models
# keep some of r1-r3 across a call; tools/ghidra/KeptRegisters.java gives them to the functions
# that need them.
KEPT_REGISTERS = ("r1", "r2", "r3")


def kept_register_models(cspec: str) -> str:
    start, end = cspec.index("<default_proto>"), cspec.index("</default_proto>")
    proto = cspec[start + len("<default_proto>"):end]
    models = []
    for mask in range(1, 1 << len(KEPT_REGISTERS)):
        kept = [r for i, r in enumerate(KEPT_REGISTERS) if mask & (1 << i)]
        m = proto.replace('name="__stdcall"', f'name="__keep_{"".join(kept)}"')
        for r in kept:
            m = patch(m, f'         <register name="{r}"/>\n', "", f"killedbycall {r}")
        m = patch(m, "      </unaffected>",
                  "".join(f'        <register name="{r}"/>\n' for r in kept) + "      </unaffected>",
                  "unaffected")
        models.append(m.strip("\n"))
    tail = end + len("</default_proto>")
    return cspec[:tail] + "\n" + "\n".join(models) + cspec[tail:]

def patch(text: str, old: str, new: str, what: str) -> str:
    if text.count(old) != 1:
        sys.exit(f"{what}: expected exactly one match in the installed SuperH4 language, "
                 f"found {text.count(old)}; the Ghidra version changed it, review the patch")
    return text.replace(old, new)


def settings_dir() -> tuple[Path, str]:
    kv = ghidra_properties()
    version, release = kv["application.version"], kv["application.release.name"]
    return ROOT / "ghidra" / "home" / "Library" / "ghidra" / f"ghidra_{version}_{release}", version


def main() -> int:
    src = GHIDRA / "Ghidra" / "Processors" / "SuperH4" / "data" / "languages"
    settings, version = settings_dir()
    ext = settings / "Extensions" / NAME
    lang = ext / "data" / "languages"
    shutil.rmtree(ext, ignore_errors=True)
    lang.mkdir(parents=True)

    sinc = (src / "SuperH4.sinc").read_text()
    sinc = patch(sinc, FSCHG_OLD, FSCHG_NEW, "fschg")
    sinc = patch(sinc, FRCHG_OLD, FRCHG_NEW, "frchg")
    sinc = patch(sinc, XF_LAYOUT_OLD, XF_LAYOUT_NEW, "xf layout")
    sinc = patch(sinc, PREF_OLD, PREF_NEW, "pref")
    sinc = patch(sinc, FMAC_OLD, FMAC_NEW, "fmac")
    sinc = patch(sinc, ":fmac FR0_t,", "define pcodeop fmac;\n:fmac FR0_t,", "fmac op")
    sinc = visible_control_loads(sinc)
    sinc = pair_moves_in_order(sinc)
    sinc = swap_vector_elements(sinc)
    (lang / "SuperH4_katana.sinc").write_text(sinc)

    slaspec = (src / "SuperH4_le.slaspec").read_text()
    slaspec = patch(slaspec, '@include "SuperH4.sinc"', '@include "SuperH4_katana.sinc"', "slaspec")
    (lang / "SuperH4_katana.slaspec").write_text(slaspec)

    pspec = (src / "SuperH4.pspec").read_text()
    (lang / "SuperH4_katana.pspec").write_text(
        patch(pspec, PSPEC_TRACKED_OLD, PSPEC_TRACKED_NEW, "pspec"))
    cspec = (src / "SuperH4_le.cspec").read_text()
    (lang / "SuperH4_katana.cspec").write_text(
        patch(patch(patch(cspec, CSPEC_UNAFFECTED_OLD, CSPEC_UNAFFECTED_NEW, "cspec unaffected"),
                    CSPEC_GLOBAL_OLD, CSPEC_GLOBAL_NEW, "cspec global"),
              CSPEC_DOUBLE_ARGS_OLD, CSPEC_DOUBLE_ARGS_NEW, "cspec double arguments"))
    (lang / "SuperH4_katana.cspec").write_text(
        kept_register_models((lang / "SuperH4_katana.cspec").read_text()))
    (lang / "SuperH4_katana.ldefs").write_text(LDEFS)

    (ext / "Module.manifest").write_text("")
    (ext / "extension.properties").write_text(
        f"name={NAME}\n"
        "description=SuperH4 little endian with fschg and frchg modelled, for Dreamcast Katana code\n"
        "author=ikaruga-decomp\n"
        "createdOn=\n"
        f"version={version}\n")

    env = {**os.environ, "JAVA_HOME": JAVA_HOME,
           "JAVA_TOOL_OPTIONS": f"-Duser.home={ROOT / 'ghidra' / 'home'} "
                                f"-Djava.io.tmpdir={ROOT / 'ghidra' / 'tmp'}"}
    (ROOT / "ghidra" / "tmp").mkdir(parents=True, exist_ok=True)
    subprocess.run([str(GHIDRA / "support" / "sleigh"), str(lang / "SuperH4_katana.slaspec")],
                   check=True, env=env)
    if not (lang / "SuperH4_katana.sla").exists():
        sys.exit("sleigh produced no SuperH4_katana.sla")
    print(f"installed SuperH4:LE:32:katana in {ext}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
