#!/bin/sh
# Decompile one PC Call of Duty 2 / United Offensive executable or DLL with Ghidra (headless),
# name it, and write readable C with tables.
#
#   GHIDRA_HOME=<ghidra folder> sh tools/pc_decomp/run_pc.sh <exe> <work dir> <out dir> [name_pc.py options]
#
# <work dir> gets proj/ (the Ghidra project), raw/ (tables before naming), names.tsv, final/
# (tables after naming), logs/. <out dir> gets decomp/*.c, functions.tsv, globals.tsv,
# strings.tsv, stats.txt. Both are derived from the game: keep them private.
# The name_pc.py options say which naming sources apply (see name_pc.py and run_all.sh).
# A step whose output exists is skipped; delete it to redo (REFIND=1 redoes the function search).
# MAXMEM (default 3G) is the Java heap for Ghidra.
set -eu
EXE=$1; WORK=$2; OUT=$3; shift 3
HERE=$(cd "$(dirname "$0")" && pwd)
: "${GHIDRA_HOME:?set GHIDRA_HOME to the Ghidra install folder}"
AH="$GHIDRA_HOME/support/analyzeHeadless"
S="$HERE/ghidra_scripts"
NAME=$(basename "$EXE")
export MAXMEM="${MAXMEM:-3G}"
mkdir -p "$WORK/proj" "$WORK/logs" "$OUT"
step() { echo "[run_pc] $(date +%H:%M:%S) $NAME: $*"; echo "$(date +%s) $*" >> "$WORK/logs/times.txt"; }
echo "$(date +%s) start" >> "$WORK/logs/times.txt"

if [ ! -d "$WORK/proj/pc.rep" ]; then
  step "import + analysis"
  "$AH" "$WORK/proj" pc -import "$EXE" -processor x86:LE:32:default -cspec windows -max-cpu 2 \
    -scriptPath "$S" -preScript PcSetup.java > "$WORK/logs/1-import.log" 2>&1
fi
if [ ! -f "$WORK/raw/functions.tsv" ] || [ -n "${REFIND:-}" ]; then
  step "find missed functions, export tables"
  "$AH" "$WORK/proj" pc -process "$NAME" -noanalysis -scriptPath "$S" \
    -postScript PcFindFunctions.java -postScript PcExport.java tables "$WORK/raw" > "$WORK/logs/2-find.log" 2>&1
  grep -h "PcFindFunctions:\|PcExport:" "$WORK/logs/2-find.log" | sed 's/^INFO  [^>]*> //; s/ (GhidraScript) *$//' || true
fi
echo "$EXE" > "$WORK/exe.txt"
step "naming"
python3 "$HERE/name_pc.py" --exe "$EXE" --raw "$WORK/raw" --out "$WORK/names.tsv" "$@" | tee "$WORK/logs/3-names.txt"
python3 "$HERE/finish_pc.py" skiplist --work "$WORK" > "$WORK/skip.tsv"
step "apply names, export tables, decompile"
rm -rf "$WORK/decomp.new"
"$AH" "$WORK/proj" pc -process "$NAME" -noanalysis -scriptPath "$S" \
  -postScript PcApplyNames.java "$WORK/names.tsv" \
  -postScript PcExport.java tables "$WORK/final" \
  -postScript PcExport.java decomp "$WORK/decomp.new" "$WORK/names.tsv" "$WORK/skip.tsv" 2500000 \
  > "$WORK/logs/4-decomp.log" 2>&1
grep -h "PcApplyNames: [0-9]\|PcExport: tables\|PcExport: decompiled" "$WORK/logs/4-decomp.log" | sed 's/^INFO  [^>]*> //; s/ (GhidraScript) *$//' || true
rm -rf "$OUT/decomp"; mv "$WORK/decomp.new" "$OUT/decomp"
step "done"
python3 "$HERE/finish_pc.py" finish --work "$WORK" --out "$OUT"
