#!/usr/bin/env bash
# Write game/sdk.h, the part of the Katana SDK's headers that the analysis uses, from an SDK copy.
#
#   tools/ghidra/sdk_header.sh
#
# The analysis (analyze.sh) reads game/sdk.h, so it needs no SDK. Run this again when
# game/symbols.tsv names more SDK functions, or game/types.h or game/globals.tsv use more SDK types.
#
# The headers come from SDK_INCLUDE (default: the R2.00J copy unpacked under sdk/). The script
# parses them in a scratch Ghidra project without analysis, and ExportSdkHeader.java writes the
# prototypes that game/symbols.tsv names and the types that they, game/types.h and game/globals.tsv
# need.
#
# Overrides: GHIDRA_HOME, JAVA_HOME, GHIDRA_LANGUAGE, SDK_INCLUDE.
set -euo pipefail

root=$(cd "$(dirname "$0")/../.." && pwd)
ghidra=${GHIDRA_HOME:-/opt/homebrew/opt/ghidra/libexec}
language=${GHIDRA_LANGUAGE:-SuperH4:LE:32:katana}
binary=$root/game/extracted/fs/1ST_READ.BIN
sdk_include=${SDK_INCLUDE:-$root/sdk/r2.00j/is3/0x429C5D0/Group2}
project=$root/ghidra/tmp/sdk-header

java_min=$(sed -n 's/^application.java.min=//p' "$ghidra/Ghidra/application.properties")
export JAVA_HOME=${JAVA_HOME:-/opt/homebrew/opt/openjdk@$java_min/libexec/openjdk.jdk/Contents/Home}
export GHIDRA_HEADLESS_MAXMEM=${GHIDRA_HEADLESS_MAXMEM:-8G}
export JAVA_TOOL_OPTIONS="-Duser.home=$root/ghidra/home -Djava.io.tmpdir=$root/ghidra/tmp ${JAVA_TOOL_OPTIONS:-}"

[ -f "$binary" ] || { echo "no $binary: run dcdisc new-game first" >&2; exit 1; }
[ -d "$sdk_include" ] || { echo "no SDK headers in $sdk_include" >&2; exit 1; }
mkdir -p "$project" "$root/ghidra/home" "$root/ghidra/tmp"

"$ghidra/support/analyzeHeadless" "$project" sdk \
  -import "$binary" \
  -overwrite \
  -loader BinaryLoader \
  -loader-baseAddr 0x8C010000 \
  -processor "$language" \
  -noanalysis \
  -deleteProject \
  -scriptPath "$root/tools/ghidra;$root/dream-recomp/tools/ghidra" \
  -preScript DreamcastMemoryMap.java \
  -postScript ApplySdkHeaders.java "$sdk_include" shinobi.h,kamui2.h,Ninja.h,sg_sd.h -D_MACHINE \
  -postScript ExportSdkHeader.java "$root/game/sdk.h" "$root/game/symbols.tsv" "$root/game/types.h" \
    "$root/game/globals.tsv" \
  2>&1 | grep -E "ApplySdkHeaders:|ExportSdkHeader:|ERROR" || true
