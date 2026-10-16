#!/bin/sh
# Decompile the PC Call of Duty 2 (1.3) and United Offensive (1.51) executables, in order:
# the multiplayer executables first (the CoD2 one names the others by matching), then the
# United Offensive server game module, then singleplayer and the CoD2 renderer.
#
#   GHIDRA_HOME=<ghidra> COD2X_SRC=<CoD2x checkout> Q3_SRC=<Quake-III-Arena checkout> \
#   [COD2REV_SRC=<CoD2rev_Server checkout>] \
#   sh tools/pc_decomp/run_all.sh <cod2 game dir> <uo game dir> <work dir> <out dir> [image...]
#
# images: cod2_mp uo_mp uo_game_mp cod2_sp uo_sp cod2_gfx_mp cod_mp uo_cgame_mp uo_ui_mp
# (default: all, in that order; cod_mp is Call of Duty 1.5's CoDMP.exe, found in the UO folder).
# Output: <out dir>/cod2-decomp/{mp,sp,gfx_mp}/, <out dir>/coduo-decomp/{mp,sp,game_mp,cgame_mp,ui_mp}/
# and <out dir>/cod-decomp/mp/.
# Reference sources give names only (see name_pc.py); nothing is copied from them.
set -eu
COD2=$1; UO=$2; WORK=$3; OUT=$4; shift 4
HERE=$(cd "$(dirname "$0")" && pwd)
: "${COD2X_SRC:?set COD2X_SRC to the CoD2x checkout}"
: "${Q3_SRC:?set Q3_SRC to the Quake-III-Arena checkout}"
REFS="$WORK/refs"
mkdir -p "$REFS/mac_src"
KS="$(cd "$HERE/../waw_decomp" && pwd)/kisak_strings.py"
if [ ! -f "$REFS/q3.tsv" ]; then
  python3 "$KS" "$Q3_SRC/code" "$REFS/q3.tsv"
fi
if [ ! -f "$REFS/mac.tsv" ]; then
  # only the Mac build's decompile (its folder also holds files with placeholder names)
  ln -sf "$COD2X_SRC/src/other/Call_of_Duty_2_Multiplayer_MAC_1.3.c" "$REFS/mac_src/"
  python3 "$KS" "$REFS/mac_src" "$REFS/mac.tsv"
fi
if [ -n "${COD2REV_SRC:-}" ] && [ ! -f "$REFS/cod2rev_tables.tsv" ]; then
  python3 "$KS" "$COD2REV_SRC/src" "$REFS/cod2rev.tsv"
fi
SYM="$COD2X_SRC/src/mss32"
[ $# -gt 0 ] || set -- cod2_mp uo_mp uo_game_mp cod2_sp uo_sp cod2_gfx_mp cod_mp uo_cgame_mp uo_ui_mp
for IMG in "$@"; do
  case $IMG in
    cod2_mp) sh "$HERE/run_pc.sh" "$COD2/CoD2MP_s.exe" "$WORK/$IMG" "$OUT/cod2-decomp/mp" \
      --cod2x "$SYM/symbols_cod2mp_s.h" --q3 "$REFS/q3.tsv" --mac "$REFS/mac.tsv" --tables "$REFS/q3_tables.tsv" ${COD2REV_SRC:+"$REFS/cod2rev_tables.tsv"} ;;
    uo_mp) sh "$HERE/run_pc.sh" "$UO/CoDUOMP.exe" "$WORK/$IMG" "$OUT/coduo-decomp/mp" \
      --var-prefix cvar --q3 "$REFS/q3.tsv" --match "$WORK/cod2_mp" --tables "$REFS/q3_tables.tsv" ${COD2REV_SRC:+"$REFS/cod2rev_tables.tsv"} ;;
    uo_game_mp) sh "$HERE/run_pc.sh" "$UO/uo/uo_game_mp_x86.dll" "$WORK/$IMG" "$OUT/coduo-decomp/game_mp" \
      --var-prefix cvar --q3 "$REFS/q3.tsv" --match "$WORK/cod2_mp" --tables "$REFS/q3_tables.tsv" ${COD2REV_SRC:+"$REFS/cod2rev_tables.tsv"} ;;
    cod2_sp) sh "$HERE/run_pc.sh" "$COD2/CoD2SP_s.exe" "$WORK/$IMG" "$OUT/cod2-decomp/sp" \
      --q3 "$REFS/q3.tsv" --match "$WORK/cod2_mp" --mac "$REFS/mac.tsv" --tables "$REFS/q3_tables.tsv" ${COD2REV_SRC:+"$REFS/cod2rev_tables.tsv"} ;;
    uo_sp) sh "$HERE/run_pc.sh" "$UO/CoDUOSP.exe" "$WORK/$IMG" "$OUT/coduo-decomp/sp" \
      --var-prefix cvar --q3 "$REFS/q3.tsv" --match "$WORK/uo_mp" "$WORK/cod2_sp" "$WORK/cod2_mp" --tables "$REFS/q3_tables.tsv" ${COD2REV_SRC:+"$REFS/cod2rev_tables.tsv"} ;;
    cod2_gfx_mp) sh "$HERE/run_pc.sh" "$COD2/gfx_d3d_mp_x86_s.dll" "$WORK/$IMG" "$OUT/cod2-decomp/gfx_mp" \
      --cod2x "$SYM/symbols_gfx_d3d_mp_x86_s.h" --q3 "$REFS/q3.tsv" --mac "$REFS/mac.tsv" --tables "$REFS/q3_tables.tsv" ${COD2REV_SRC:+"$REFS/cod2rev_tables.tsv"} ;;
    cod_mp) sh "$HERE/run_pc.sh" "$UO/CoDMP.exe" "$WORK/$IMG" "$OUT/cod-decomp/mp" \
      --var-prefix cvar --q3 "$REFS/q3.tsv" --match "$WORK/uo_mp" "$WORK/cod2_mp" --tables "$REFS/q3_tables.tsv" ${COD2REV_SRC:+"$REFS/cod2rev_tables.tsv"} ;;
    uo_cgame_mp) sh "$HERE/run_pc.sh" "$UO/uo/uo_cgame_mp_x86.dll" "$WORK/$IMG" "$OUT/coduo-decomp/cgame_mp" \
      --var-prefix cvar --q3 "$REFS/q3.tsv" --match "$WORK/cod2_mp" "$WORK/uo_game_mp" --tables "$REFS/q3_tables.tsv" ${COD2REV_SRC:+"$REFS/cod2rev_tables.tsv"} ;;
    uo_ui_mp) sh "$HERE/run_pc.sh" "$UO/uo/uo_ui_mp_x86.dll" "$WORK/$IMG" "$OUT/coduo-decomp/ui_mp" \
      --var-prefix cvar --q3 "$REFS/q3.tsv" --match "$WORK/cod2_mp" "$WORK/uo_cgame_mp" --tables "$REFS/q3_tables.tsv" ${COD2REV_SRC:+"$REFS/cod2rev_tables.tsv"} ;;
    *) echo "unknown image $IMG" >&2; exit 2 ;;
  esac
done
