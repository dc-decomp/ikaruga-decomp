#!/usr/bin/env bash
# Import 1ST_READ.BIN into a fresh Ghidra project, analyse it, and export every function as C.
#
#   tools/ghidra/analyze.sh
#
# Writes ghidra/project/ (the Ghidra project) and ghidra/out/ (functions.json, c/<address>.c,
# c/index.tsv). Both hold game code and stay out of version control.
#
# The language is SuperH4:LE:32:katana from tools/ghidra/build_language.py, which this builds on
# first use. Function discovery is seeded from dream-recomp's own (dream-translate discover), which
# finds about twice as many entries in a Katana binary as Ghidra's analysis alone.
#
# Names come from game/symbols.tsv (functions) and game/globals.tsv (data). Both hold the names that
# tools/symbols/match_libs.py finds in a Katana SDK, and the names added by hand.
#
# Prototypes and types come from the Katana SDK headers in SDK_INCLUDE (default: the R2.00J copy
# unpacked under sdk/). Without them the functions keep the signatures Ghidra guessed.
#
# Overrides: GHIDRA_HOME, JAVA_HOME, GHIDRA_LANGUAGE, DREAM_TRANSLATE.
set -euo pipefail

root=$(cd "$(dirname "$0")/../.." && pwd)
ghidra=${GHIDRA_HOME:-/opt/homebrew/opt/ghidra/libexec}
language=${GHIDRA_LANGUAGE:-SuperH4:LE:32:katana}
binary=$root/game/extracted/fs/1ST_READ.BIN
translate=${DREAM_TRANSLATE:-$root/dream-recomp/build/translator/dream-translate}
sdk_include=${SDK_INCLUDE:-$root/sdk/r2.00j/is3/0x429C5D0/Group2}
project=$root/ghidra/project
out=$root/ghidra/out

export JAVA_HOME=${JAVA_HOME:-/opt/homebrew/opt/openjdk@21/libexec/openjdk.jdk/Contents/Home}
export GHIDRA_HEADLESS_MAXMEM=${GHIDRA_HEADLESS_MAXMEM:-8G}
# Ghidra keeps its settings, script bundle cache and temp files under user.home and java.io.tmpdir.
# Keep them in the tree, so a run needs no write access outside it.
export JAVA_TOOL_OPTIONS="-Duser.home=$root/ghidra/home -Djava.io.tmpdir=$root/ghidra/tmp ${JAVA_TOOL_OPTIONS:-}"

[ -f "$binary" ] || { echo "no $binary: run dcdisc new-game first" >&2; exit 1; }
mkdir -p "$project" "$out" "$root/ghidra/home" "$root/ghidra/tmp"
if [ "$language" = SuperH4:LE:32:katana ] \
    && ! ls "$root"/ghidra/home/Library/ghidra/*/Extensions/SuperH4Katana/data/languages/*.sla >/dev/null 2>&1; then
  "$root/tools/ghidra/build_language.py"
fi
rm -rf "$out/c"

# dream-recomp makes a branch target into its own unit. In Ghidra such an entry is inside a function,
# and a seed there would cut that function short.
"$translate" discover --image "$binary" --base 0x8C010000 --entry 0x8C010000 \
  --out "$out/recomp-functions.json"
# A switch case is not a function either: the pointer scan finds the table entries, but the cases
# belong to the dispatcher, which ApplySwitchTables gives Ghidra as a jump-table override.
python3 -c '
import json, sys
d = json.load(open(sys.argv[1]))
cases = {t for s in d["switch_tables"] for t in s["targets"]}
print("\n".join(f["entry"] for f in d["functions"]
                if f["origin"] != "branch" and f["entry"] not in cases))
with open(sys.argv[2], "w") as o:
    for s in d["switch_tables"]:
        o.write(" ".join([s["site"]] + s["targets"]) + "\n")
' "$out/recomp-functions.json" "$out/switch-tables.txt" > "$out/seeds.txt"

# MarkEntry takes the lowest address as the entry, so it runs before the memory map adds blocks.
"$ghidra/support/analyzeHeadless" "$project" ikaruga \
  -import "$binary" \
  -overwrite \
  -loader BinaryLoader \
  -loader-baseAddr 0x8C010000 \
  -processor "$language" \
  -scriptPath "$root/tools/ghidra;$root/dream-recomp/tools/ghidra" \
  -preScript MarkEntry.java \
  -preScript DreamcastMemoryMap.java \
  -preScript EnableAggressive.java \
  -postScript SeedFunctions.java "$out/seeds.txt" \
  -postScript ApplySwitchTables.java "$out/switch-tables.txt" \
  -postScript SeedFromPointers.java \
  -postScript ApplySymbols.java "$root/game/symbols.tsv" "labels:$root/game/globals.tsv" \
  -postScript ApplySdkHeaders.java "$sdk_include" shinobi.h,kamui2.h,Ninja.h,sg_sd.h -D_MACHINE \
  -postScript MarkConstantPools.java \
  -postScript FixupFlow.java \
  -postScript CommitPrototypes.java \
  -postScript ExportFunctions.java "$out/functions.json" \
  -postScript ExportTypes.java "$out/types.json" \
  -postScript ExportDecompiled.java "$out/c" \
  -log "$out/analyze.log" \
  -scriptlog "$out/scripts.log"
