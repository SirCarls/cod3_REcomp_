#!/bin/sh
# Decompile one Xbox 360 executable (XEX) of World at War with Ghidra (headless).
#
#   GHIDRA_HOME=<ghidra> XEXLZX=<libxexlzx.so> sh tools/waw_decomp/run_x360.sh <xex> <name> <work dir> \
#       <rexglue-sdk/src/kernel> [kisak dir] [the PC build as "names.tsv:raw dir[:exe]"]
#
# Needs the Xenon language in Ghidra first: python3 tools/waw_decomp/ghidra_xenon/install_xenon.py <ghidra>
# <work dir> gets proj/, image.pe, imports.tsv, raw/, names.tsv, pc_match.tsv (with the PC build),
# decomp/, logs/ (all derived from the game: keep private). Set STOP_AFTER=analysis to stop after
# the import + analysis.
set -eu
XEX=$1; NAME=$2; WORK=$3; KERNEL=$4; KISAK=${5:-}; PC=${6:-}
HERE=$(cd "$(dirname "$0")" && pwd)
: "${GHIDRA_HOME:?set GHIDRA_HOME to the Ghidra install folder}"
AH="$GHIDRA_HOME/support/analyzeHeadless"
S="$HERE/ghidra_scripts"
mkdir -p "$WORK/proj" "$WORK/logs"
P="x360_$NAME"
step() { echo "[run_x360] $(date +%H:%M:%S) $*"; }

step "xex -> pe"
INFO=$(python3 -I "$HERE/xex2pe.py" "$XEX" "$WORK/image.pe" --imports "$WORK/imports.tsv" --exports "$KERNEL" | head -1)
echo "$INFO"
LOAD=$(echo "$INFO" | sed -n 's/.*load_addr=0x\([0-9a-f]*\).*/\1/p')
ENTRY=$(echo "$INFO" | sed -n 's/.*entry=0x\([0-9a-f]*\).*/\1/p')
if [ ! -d "$WORK/proj/$P.rep" ]; then
  step "import + setup + analysis (load 0x$LOAD)"
  # Loaded at 0 and moved to its address by WawXenonSetup: the Binary loader's own base address
  # misplaces high addresses (0x82000000) for this 64-bit language with 32-bit addressing.
  "$AH" "$WORK/proj" "$P" -import "$WORK/image.pe" -loader BinaryLoader \
    -processor PowerPC:BE:64:Xenon-32addr -cspec default -max-cpu 2 -scriptPath "$S" \
    -preScript WawXenonSetup.java "$WORK/imports.tsv" "${ENTRY:--}" "$LOAD" > "$WORK/logs/1-import.log" 2>&1
  grep -h "sections:\|imports named:\|pdata functions:" "$WORK/logs/1-import.log" || true
fi
[ "${STOP_AFTER:-}" = analysis ] && { step "stopped after analysis"; exit 0; }
step "export tables"
"$AH" "$WORK/proj" "$P" -process image.pe -noanalysis -scriptPath "$S" \
  -postScript WawExport.java "$WORK/raw" 0 > "$WORK/logs/3-export.log" 2>&1
step "naming"
set -- --raw "$WORK/raw" --image "$WORK/image.pe" --load "0x$LOAD" --out "$WORK/names.tsv"
if [ -n "$KISAK" ]; then
  set -- "$@" --kisak "$KISAK/kisak_functions_noradiant.tsv" --kisak-tables "$KISAK/kisak_functions_tables.tsv"
fi
if [ -n "$PC" ]; then
  PC_NAMES=$(echo "$PC" | cut -d: -f1); PC_RAW=$(echo "$PC" | cut -d: -f2); PC_EXE=$(echo "$PC" | cut -s -d: -f3)
  set -- "$@" --pc-names "$PC_NAMES" --pc-raw "$PC_RAW" --matches "$WORK/pc_match.tsv"
  [ -n "$PC_EXE" ] && set -- "$@" --pc-exe "$PC_EXE"
fi
python3 "$HERE/name_x360.py" "$@"
step "apply names"
"$AH" "$WORK/proj" "$P" -process image.pe -noanalysis -scriptPath "$S" \
  -postScript WawApplyNames.java "$WORK/names.tsv" > "$WORK/logs/4-names.log" 2>&1
grep -h "WawApplyNames:" "$WORK/logs/4-names.log" || true
step "decompile"
rm -rf "$WORK/decomp.new"
"$AH" "$WORK/proj" "$P" -process image.pe -noanalysis -scriptPath "$S" \
  -postScript WawExport.java "$WORK/decomp.new" 1 > "$WORK/logs/5-decomp.log" 2>&1
grep -h "decompiled:" "$WORK/logs/5-decomp.log" || true
rm -rf "$WORK/decomp"; mv "$WORK/decomp.new" "$WORK/decomp"
step "done"
