# xenos_diff: the shader translator checked against the SDK's interpreter

[acct2-decomp] 2026-10-25. A developer tool, not part of the game.

`xenos_ucode::wgsl` turns each Xbox 360 shader into WGSL for the Rust renderer. This tool
checks that the WGSL computes what the SDK's own shader interpreter computes
(`src/graphics/pipeline/shader/interpreter.cpp` in rexglue-sdk, Xenia's):

- For each shader of the corpus (`shader_*.ucode.bin.vert|frag`, claude-5d's
  `COD3-rea-work/results/c5d-xenos-corpus`), it makes random inputs: 512 float constants
  (ordinary values, small integers, ±0, ±1, huge and tiny numbers, infinities, NaNs), the bool
  and loop constants, 96 vertex fetch constants with buffers in a random memory window, the
  input registers (the vertex index or the 16 interpolators).
- The same case runs through the SDK's interpreter (built from its source next to a small C
  shim, `interp/`) and through the translated WGSL, parsed by naga and run on the CPU
  (`src/eval.rs`, an executor for naga's IR).
- Everything both produce is compared bit for bit: every temporary register at the end, every
  exported interpolator, position, color and depth component. A NaN equals any NaN; +0 against
  -0 and denormal-only differences are counted separately.

**Result (2026-10-25, seed 20261025): all 307 shaders, 10,000 cases each, identical.**

## What the comparison can't see

- Texture sampling: the interpreter has no texture unit and writes zeros for every texture
  fetch, so the WGSL side samples zeros too (`eval.rs`). `getWeights2D` likewise: the tool
  translates with `wgsl::Options { zero_texture_weights: true }` (the renderer never does).
  Without that option the 52 shaders that use it differ, as they should.
- Pixel kill: the interpreter computes kill instructions but never kills; the executor treats
  `discard` as WGSL's demote (marks the pixel, goes on), so values stay comparable.
- GPU precision: the executor computes in exact IEEE f32 (no fused multiply-adds, `exp2` etc.
  from the C library, divisions as divisions); a real GPU may differ by an ulp. That is for the
  image comparisons later.
- The interpreter's loop stack looks off (LoopStart pushes twice; aL then reads the wrong
  slot); the one corpus shader with a loop matched anyway. Not used as evidence for loops.

## The interpreter's two vertex fetch bugs: SDK patch 0101

1. `packed_widths[4], packed_offsets[4]` are not initialized and `packed_offsets[0]` is never
   set, so the first field of every packed vertex format reads from a garbage bit offset.
2. Unsigned packed fields are masked without being shifted down by their offset
   (`packed_dwords[i >> 1] & mask`), so every field of 8_8_8_8, 2_10_10_10, 16_16 ... reads
   the lowest bits. The unused `packed_width` on that line is what is left of the shift.

Both are fixed in the SDK by `COD_3_WAW_Setup-tool/patches/0101-interpreter-packed-vertex-fields-read-from-their-offsets.patch`
(owner 2026-10-25 04:26 "yea a patch would be fine"). `interp/build.sh` uses an SDK tree's
interpreter.cpp as it is when it has 0101, and applies the same two fixes to its own copy by
`sed` when it doesn't (counted: the build stops if they no longer apply). The emulator draws
through the SPIR-V translator, not the interpreter, so neither changed what the game shows.

## What it found in the translator (fixed in xenos_ucode)

- A fetch that writes only constants (`vfetch r4.0001`, also texture fetches and
  `getGradients`) wrote nothing: the constant 0 / 1 components are now stored.
- Denormal ALU operands are flushed to (signed) zero, as the console and the interpreter do
  (`xe_ftz` in the WGSL prelude); fetch coordinates are not.
- A vertex fetch reads 0 outside the buffer its fetch constant names (address and size).
- Normalized vertex formats divide (`raw / 511.0`) rather than multiply by a reciprocal,
  matching the interpreter to the bit.

## Building and running it (on the PC, through `rea-build`)

```
SDK=/mnt/10TB_Linux_Prt3/COD3-rea-work/build/rexglue-sdk      # built and installed
OUT=<a folder>   SDK=$SDK sh interp/build.sh                    # libxe_interp.a (g++ 13, C++23)
XE_INTERP_DIR=<that folder> XE_RUNTIME_LIB=$SDK/out/install/linux-amd64/lib \
    cargo build --offline --release                             # needs naga 29.0.4 (vendored)
target/release/xenos_diff <corpus> --cases 10000 --seed 20261025 [--skip K --take M]
target/release/xenos_diff <corpus> --only <hash> --case K       # every difference of one case
```

- It links the SDK's `librexruntime.so` for guest memory (`rex::memory::Memory`); one per
  process (it maps at a fixed address), so run several processes for speed: 16 processes of
  10 shaders each do the corpus at 10,000 cases in about 40 s on the 24-core PC.
- naga isn't in the engine workspace: this package has its own `[workspace]`, and the REA build
  uses claude-5d's vendored crates (`results/c5d-vendor-naga.tar.gz`) through a
  `.cargo/config.toml` source replacement. The REA tree acct2-decomp used is
  `COD3-rea-work/acct2-decomp/xdiff/` (`sync.sh`, `build.sh`).
- Options: `--finite` (no infinities or NaNs in the inputs), `--show N`.
- The corpus and anything printed from it come from the game: they stay on the PC.
