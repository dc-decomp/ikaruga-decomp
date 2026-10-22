#!/usr/bin/env python3
"""Did a change alter what the game does? The regression gate for the reference build.

The launcher hashes every guest write per frame, so two runs with the same hash did the same thing,
bit for bit. A change to the config, the symbols, dream-recomp or a replaced function must keep the
hash, or the difference must be understood before the work continues.

    tools/reference/gate.py capture before
    # change something, rebuild
    tools/reference/gate.py capture after
    tools/reference/gate.py compare before after

Two scenarios, because they run different code:

  boot   no input. The game loads and waits on its save-file screen.
  play   Start and A pressed through the save-file screen, the logos and the mode select into
         stage 1, with a blank memory card that the game writes its save file to.

With a boot ROM at bios/dc_bios.bin, both scenarios run with it (--bios), as the console would.
The game copies font glyphs out of it, so a capture with the ROM and one without differ, and the
capture records which it is.

Captures go to .regress/<label>/, which git ignores. They are only comparable on one machine, with
one disc and one build of the launcher.
"""

from __future__ import annotations

import argparse
import hashlib
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
STORE = ROOT / ".regress"
LAUNCHER = ROOT / "build" / "game" / "ikaruga_boot"
CONFIG = ROOT / "game" / "ikaruga.toml"
BIOS = ROOT / "bios" / "dc_bios.bin"

# --rtc-seed pins the console clock. Without it, the clock comes from the host date and two runs
# of the same build differ after a few frames.
COMMON = ["--no-audio", "--rtc-seed", "1"]
SCENARIOS = {
    "boot": ["--max-seconds", "40"],
    "play": ["--max-seconds", "80",
             "--press", "start@600,a@700,start@1200,a@1300,start@2000,a@2100"],
}
CARD = {"play"}


def counters(report: str) -> str:
    """The report without its host timings, which differ between identical runs."""
    return "\n".join(line for line in report.splitlines()
                     if not line.startswith("host:") and "real time" not in line)


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()[:16]


def capture(label: str) -> int:
    if not LAUNCHER.exists():
        print(f"no launcher at {LAUNCHER}: build the target ikaruga_boot first", file=sys.stderr)
        return 2
    out = STORE / label
    out.mkdir(parents=True, exist_ok=True)
    bios = ["--bios", str(BIOS)] if BIOS.exists() else []
    (out / "bios").write_text(f"{BIOS.name} {digest(BIOS)}\n" if bios else "none\n")
    for name, extra in SCENARIOS.items():
        hash_file, report = out / f"{name}.hash", out / f"{name}.report"
        hash_file.unlink(missing_ok=True)
        args = [str(LAUNCHER), "--config", str(CONFIG), *COMMON, *bios, *extra,
                "--write-hash", str(hash_file), "--report", str(report)]
        if name in CARD:
            card = out / f"{name}.vmu"
            card.unlink(missing_ok=True)  # the launcher makes a blank card for a missing path
            args += ["--vmu", str(card)]
        proc = subprocess.run(args, capture_output=True, text=True, cwd=ROOT)
        if not hash_file.exists() or hash_file.stat().st_size == 0:
            tail = (proc.stderr or proc.stdout).strip().splitlines()[-1:]
            print(f"{label}/{name}: no write hash. {' '.join(tail)}", file=sys.stderr)
            return 2
        text = counters(report.read_text())
        (out / f"{name}.counters").write_text(text)
        frames = sum(1 for line in hash_file.open() if not line.startswith("#"))
        print(f"{label:12} {name:5} frames {frames:<6} write hash {digest(hash_file)} "
              f"counters {hashlib.sha256(text.encode()).hexdigest()[:16]}")
    return 0


def compare(before: str, after: str) -> int:
    changed = False
    roms = [(STORE / x / "bios").read_text().strip() if (STORE / x / "bios").exists() else "none"
            for x in (before, after)]
    if roms[0] != roms[1]:
        print(f"boot ROM differs ({roms[0]} / {roms[1]}): the captures are not comparable")
        return 1
    for name in SCENARIOS:
        a, b = STORE / before / f"{name}.hash", STORE / after / f"{name}.hash"
        missing = [p for p in (a, b) if not p.exists()]
        if missing:
            for p in missing:
                print(f"{name:5} missing {p.relative_to(ROOT)}: capture it first", file=sys.stderr)
            changed = True
            continue
        ca = (STORE / before / f"{name}.counters").read_text()
        cb = (STORE / after / f"{name}.counters").read_text()
        if digest(a) == digest(b) and ca == cb:
            print(f"{name:5} identical")
            continue
        changed = True
        first = next((i for i, (x, y) in enumerate(zip(a.open(), b.open())) if x != y), None)
        where = f", first difference in hash line {first}" if first is not None else ""
        print(f"{name:5} CHANGED: write hash {digest(a)} -> {digest(b)}"
              f"{'' if ca == cb else ', counters differ too'}{where}")
    return 1 if changed else 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("capture", help="run both scenarios and store the hashes").add_argument("label")
    c = sub.add_parser("compare", help="compare two captures")
    c.add_argument("before")
    c.add_argument("after")
    args = ap.parse_args()
    return capture(args.label) if args.cmd == "capture" else compare(args.before, args.after)


if __name__ == "__main__":
    sys.exit(main())
