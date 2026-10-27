#!/bin/sh
# [acct2-decomp] Builds libxe_interp.a: this shim plus the SDK's interpreter.cpp (with patch
# 0101's two vertex fetch fixes, see below), register_file.cpp and format/ucode.cpp (the ALU
# opcode tables), against the SDK's installed headers. The Rust checker links it
# (build.rs, XE_INTERP_DIR) together with the SDK's librexruntime.so (rex::memory::Memory).
#   SDK=<rexglue-sdk checkout, built and installed>  OUT=<folder>  sh build.sh
set -e
: "${SDK:?the rexglue-sdk folder}"
: "${OUT:?the output folder}"
HERE=$(cd "$(dirname "$0")" && pwd)
INC="$SDK/out/install/linux-amd64/include"
mkdir -p "$OUT"
FLAGS="-std=c++23 -O2 -fPIC -ffp-contract=off -I$INC"
# The interpreter's vertex fetch has two bugs that SDK patch 0101
# (COD_3_WAW_Setup-tool/patches/0101-interpreter-packed-vertex-fields-read-from-their-offsets.patch)
# fixes: packed_offsets[0] is never set (the arrays are not initialized), and unsigned packed
# fields are masked without being shifted down by their offset. An SDK tree without 0101 gets
# the same two fixes here, in the checker's copy, by sed (counted; the build stops if they no
# longer apply). INTERPRETER=<file> checks another interpreter.cpp.
SRC="${INTERPRETER:-$SDK/src/graphics/pipeline/shader/interpreter.cpp}"
if grep -q 'packed_offsets\[4\] = {};' "$SRC" && grep -q '>> packed_offsets\[i\]) &' "$SRC"; then
  cp "$SRC" "$OUT/interpreter_checked.cpp"
  echo "interpreter.cpp has patch 0101: used as it is"
else
  sed -e 's/uint32_t packed_widths\[4\], packed_offsets\[4\];/uint32_t packed_widths[4] = {}, packed_offsets[4] = {};/' \
      -e 's/result\[i\] = float(packed_dwords\[i >> 1\] & ((UINT32_C(1) << packed_widths\[i\]) - 1));/result[i] = float((packed_dwords[i >> 1] >> packed_offsets[i]) \& ((UINT32_C(1) << packed_width) - 1));/' \
      "$SRC" > "$OUT/interpreter_checked.cpp"
  FIXES=$(diff "$SRC" "$OUT/interpreter_checked.cpp" | grep -c '^>' || true)
  if [ "$FIXES" != "2" ]; then echo "interpreter.cpp changed: the 2 fixes of patch 0101 no longer apply ($FIXES)"; exit 1; fi
  echo "interpreter.cpp without patch 0101: its 2 fixes applied to the checker's copy"
fi
g++ $FLAGS -c "$OUT/interpreter_checked.cpp" -o "$OUT/interpreter.o"
g++ $FLAGS -c "$SDK/src/graphics/register_file.cpp" -o "$OUT/register_file.o"
g++ $FLAGS -c "$SDK/src/graphics/format/ucode.cpp" -o "$OUT/ucode.o"
g++ $FLAGS -c "$HERE/xe_interp.cpp" -o "$OUT/xe_interp.o"
rm -f "$OUT/libxe_interp.a"
ar rcs "$OUT/libxe_interp.a" "$OUT/xe_interp.o" "$OUT/interpreter.o" "$OUT/register_file.o" "$OUT/ucode.o"
echo "built $OUT/libxe_interp.a"
