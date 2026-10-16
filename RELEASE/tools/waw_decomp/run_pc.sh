#!/bin/sh
# Decompile one World at War PC executable with Ghidra (headless), name it, and export C.
#
#   GHIDRA_HOME=<ghidra folder> [OTHER=<exe>:<raw>:<names.tsv>] sh tools/waw_decomp/run_pc.sh <exe> <sp|mp> <work dir> [kisak dir]
#
# <work dir> gets: proj/ (Ghidra project), raw/ (tables), names.tsv, decomp/ (C, one file
# per 64 KB of code), logs/, and with OTHER other_match.tsv (functions paired with the other
# executable's). Everything in it is derived from the game: keep it private.
# [kisak dir]: the output of kisak_strings.py (kisak_functions.tsv + _tables.tsv), optional.
set -eu
EXE=$1; WHICH=$2; WORK=$3; KISAK=${4:-}
HERE=$(cd "$(dirname "$0")" && pwd)
: "${GHIDRA_HOME:?set GHIDRA_HOME to the Ghidra install folder}"
AH="$GHIDRA_HOME/support/analyzeHeadless"
S="$HERE/ghidra_scripts"
NAME=$(basename "$EXE")
mkdir -p "$WORK/proj" "$WORK/logs"
P="waw_$WHICH"
step() { echo "[run_pc] $(date +%H:%M:%S) $*"; }

if [ ! -d "$WORK/proj/$P.rep" ]; then
  step "import + analysis"
  "$AH" "$WORK/proj" "$P" -import "$EXE" -processor x86:LE:32:default -cspec windows \
    -max-cpu 2 > "$WORK/logs/1-import.log" 2>&1
fi
if [ ! -f "$WORK/raw/functions.tsv" ] || [ -n "${REFIND:-}" ]; then
  step "find missed functions (+ analysis of what they add)"
  "$AH" "$WORK/proj" "$P" -process "$NAME" -scriptPath "$S" -postScript WawFindFunctions.java \
    > "$WORK/logs/2-find.log" 2>&1
  grep -h "WawFindFunctions:\|phase 1" "$WORK/logs/2-find.log" || true
  step "export tables"
  "$AH" "$WORK/proj" "$P" -process "$NAME" -noanalysis -scriptPath "$S" \
    -postScript WawExport.java "$WORK/raw" 0 > "$WORK/logs/3-export.log" 2>&1
fi
if [ ! -f "$WORK/.paramid" ]; then
  step "register parameters (Decompiler Parameter ID analysis)"
  "$AH" "$WORK/proj" "$P" -process "$NAME" -scriptPath "$S" -preScript WawEnableParamId.java \
    > "$WORK/logs/2b-paramid.log" 2>&1 && touch "$WORK/.paramid"
fi
step "naming"
set -- --exe "$EXE" --which "$WHICH" --raw "$WORK/raw" --public "$HERE/public_symbols.tsv" --out "$WORK/names.tsv"
if [ -n "$KISAK" ]; then
  set -- "$@" --kisak "$KISAK/kisak_functions_noradiant.tsv" --kisak-tables "$KISAK/kisak_functions_tables.tsv"
fi
if [ -n "${OTHER:-}" ]; then
  # the other PC executable's exe, Ghidra export and names.tsv, as "exe:raw dir:names.tsv"
  O_EXE=$(echo "$OTHER" | cut -d: -f1); O_RAW=$(echo "$OTHER" | cut -d: -f2); O_NAMES=$(echo "$OTHER" | cut -d: -f3)
  set -- "$@" --other "$O_EXE" "$O_RAW" "$O_NAMES" --matches "$WORK/other_match.tsv"
fi
python3 "$HERE/name_pc.py" "$@"
python3 "$HERE/script_api.py" --exe "$EXE" --names "$WORK/names.tsv" --out "$WORK/script_api.tsv"
step "apply names"
"$AH" "$WORK/proj" "$P" -process "$NAME" -noanalysis -scriptPath "$S" \
  -postScript WawApplyNames.java "$WORK/names.tsv" > "$WORK/logs/4-names.log" 2>&1
grep -h "WawApplyNames:" "$WORK/logs/4-names.log" || true
step "decompile"
rm -rf "$WORK/decomp.new"
"$AH" "$WORK/proj" "$P" -process "$NAME" -noanalysis -scriptPath "$S" \
  -postScript WawExport.java "$WORK/decomp.new" 1 > "$WORK/logs/5-decomp.log" 2>&1
grep -h "decompiled:" "$WORK/logs/5-decomp.log" || true
rm -rf "$WORK/decomp"; mv "$WORK/decomp.new" "$WORK/decomp"
step "done"
