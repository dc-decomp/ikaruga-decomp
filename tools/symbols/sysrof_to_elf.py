#!/usr/bin/env python3
"""Convert the Hitachi (SYSROF) libraries of a Katana SDK to ELF objects for match_libs.py.

SDKs ship their libraries as SYSROF .lib files, and some also ship ELF copies (Lib/Gnu). The ELF
copies are the output of the SDK's own converter: for R10.1, libsplit.exe and elfcnv.exe reproduce
all 108 members of libshinobi.a byte for byte. This runs the same two tools under wine on every
.lib and .obj below SDK_DIR:

    tools/symbols/sysrof_to_elf.py SDK_DIR OUT_DIR

OUT_DIR/<library>/<module>.obj.elf for each library, OUT_DIR/_objects/ for the loose .obj files.
The tools fail on paths with spaces, so OUT_DIR must have none. Keep OUT_DIR under sdk/, which git
ignores: the objects are SDK code.

The Hitachi tools come from SDK_DIR, or else from --tools (default: the R10.1 copies in sdk/).
Environment: WINE (default: Whisky's wine64), WINEPREFIX (default: sdk/wineprefix).
"""

from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
R10_TOOLS = (ROOT / "sdk" / "dreamcast-docs" / "SDK" / "EXES" / "INSTALL KATANA SDK" / "INPUT"
             / "R10.1_000518" / "Utl" / "Dev" / "Hitachi")
WHISKY_WINE = (Path.home() / "Library" / "Application Support" / "com.isaacmarovitz.Whisky"
               / "Libraries" / "Wine" / "bin" / "wine64")


def find_tool(name: str, *dirs: Path) -> Path:
    for d in dirs:
        for p in d.rglob("*"):
            if p.name.lower() == name and p.is_file():
                return p
    sys.exit(f"no {name} in {', '.join(map(str, dirs))}")


def is_sysrof(p: Path) -> bool:
    with open(p, "rb") as f:
        head = f.read(8)
    return bool(head) and not head.startswith(b"!<arch>") and not head.startswith(b"\x7fELF")


def run_batch(wine: str, cwd: Path, lines: list[str]) -> None:
    # One wine session for the whole batch: starting wine per file costs seconds each. libsplit
    # exits with 2 after a good split, so the caller checks the files, not the exit status.
    (cwd / "run.bat").write_text("\r\n".join(lines) + "\r\n")
    subprocess.run([wine, "cmd", "/c", "run.bat"], cwd=cwd,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    (cwd / "run.bat").unlink()


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("sdk", type=Path)
    ap.add_argument("out", type=Path)
    ap.add_argument("--tools", type=Path, default=R10_TOOLS,
                    help="where libsplit.exe and elfcnv.exe are if SDK_DIR has none")
    args = ap.parse_args()
    if " " in str(args.out.resolve()):
        sys.exit("OUT_DIR must not contain spaces: the Hitachi tools fail on them")

    wine = os.environ.get("WINE", str(WHISKY_WINE))
    os.environ.setdefault("WINEPREFIX", str(ROOT / "sdk" / "wineprefix"))
    os.environ.setdefault("WINEDEBUG", "-all")
    libsplit = find_tool("libsplit.exe", args.sdk, args.tools)
    elfcnv = find_tool("elfcnv.exe", args.sdk, args.tools)

    libs = sorted(p for p in args.sdk.rglob("*") if p.suffix.lower() == ".lib" and is_sysrof(p))
    objs = sorted(p for p in args.sdk.rglob("*") if p.suffix.lower() == ".obj" and is_sysrof(p))
    converted = failed = 0
    for lib in libs:
        work = args.out / lib.stem.lower()
        shutil.rmtree(work, ignore_errors=True)
        work.mkdir(parents=True)
        shutil.copy(libsplit, work / "libsplit.exe")
        shutil.copy(elfcnv, work / "elfcnv.exe")
        shutil.copy(lib, work / "in.lib")
        run_batch(wine, work, ["libsplit.exe in.lib"])
        modules = sorted(p.name for p in work.glob("*.obj"))
        run_batch(wine, work, [f"elfcnv.exe {m} {m}.elf" for m in modules])
        for m in modules:
            if (work / f"{m}.elf").exists():
                converted += 1
                (work / m).unlink()
            else:
                failed += 1
        for tool in ("libsplit.exe", "elfcnv.exe", "in.lib"):
            (work / tool).unlink()
        print(f"{lib.relative_to(args.sdk)}: {len(modules)} modules")

    if objs:
        work = args.out / "_objects"
        shutil.rmtree(work, ignore_errors=True)
        work.mkdir(parents=True)
        shutil.copy(elfcnv, work / "elfcnv.exe")
        names = []
        for o in objs:
            name = o.name.lower().replace(" ", "_")
            shutil.copy(o, work / name)
            names.append(name)
        run_batch(wine, work, [f"elfcnv.exe {n} {n}.elf" for n in names])
        for n in names:
            if (work / f"{n}.elf").exists():
                converted += 1
            else:
                failed += 1
            (work / n).unlink()
        (work / "elfcnv.exe").unlink()

    print(f"{len(libs)} libraries and {len(objs)} objects: {converted} modules converted, {failed} failed "
          f"-> {args.out}")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
