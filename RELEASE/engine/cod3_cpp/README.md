# Call of Duty 3 in C++: the decomp as source code

Call of Duty 3's functions written back as the source code they were compiled from, in C++20:
the game's own names (`AngleNormalize180`, `VectorNormalize`, `vectoyaw`), its types (`vec3_t`)
and its structure, the way its developers would have written them, rather than the machine-level
C++ the static recompilation runs (`recomp/generated`). Each function says where it is in the
executable and what it is called in the recompiled C++, so the two read side by side:

```
// 0x82526BF8; recompiled: AngleNormalize180
float AngleNormalize180(float angle);
```

The Rust crate `engine/crates/cod3_game` stays the full version (owner, 2026-10-15: "c++ is
needed, but the full versions need to be rust. c++ for developers"). Every function here gives
the same bits as the original, quirks included, and is checked against the game's own machine code
the same way as the Rust ports.

## Checking

- **The original's recorded answers** (no game files, no emulator): `ctest` runs `cod3_golden`
  on `engine/tools/decomp_check/golden/`, the answers the game's code gave in the checker's
  emulator, which `cargo test -p decomp_check` replays through the Rust ports too.
- **The original itself, on new random inputs**: `cod3_check_cpp` answers the checker's requests
  with the same protocol as the Rust answerer `decomp_check`:

  ```
  COD3_DECOMP_ANSWERER=<build>/cod3_check_cpp python3 tools/decomp/decomp.py check
  ```

  (the checker's one-time setup is in `docs/DECOMP.md`, "Setting up (once)").

## Reading the arithmetic

The Xbox 360 compiler turned `a*b + c` into one fused instruction (`fmadds`, one rounding instead of
two), and the game's last bits depend on it (every program in a multiplayer match must compute the
same positions and directions). A PC compiler fuses differently or not at all, so the fused steps
are written out with `ppc::fmadds` (`cod3/ppc.h`), each next to the expression it came from:

```
return fmadds(dx, dx, fmadds(dz, dz, dy * dy));  // dx*dx + dy*dy + dz*dz, y first
```

Likewise float-to-integer conversions are the console's `fctiwz` (`ppc::fctiwz`: it saturates where a
C++ cast would be undefined), and the C runtime's `sin`/`cos` are the console's
(`cod3/xbox_crt.h`), since the PC's own give other last bits. The build turns off the compiler's
own fusing (`-ffp-contract=off`, `/fp:precise`), lets signed integers wrap as the console's do (`-fwrapv`), and needs floats computed in their own precision
(any 64-bit build; not x87).

## What is here

| File | Functions |
|---|---|
| `cod3/q_shared.h`, `cod3/q_math.cpp` | angles (`AngleNormalize180`, `AngleSubtract`, `AngleDelta`, `AnglesSubtract`) and vectors (`VectorNormalize`, `VectorNormalize2`, `VectorNormalizeFast`, `Vec2Normalize`, `Vec2DistanceSq`, `Vec2Distance`, `Distance`) |
| `cod3/q_math_mp.cpp` | the multiplayer program's own: `AngleWrap180`, the fast yaw `vectoyaw` / `vectoyaw_signed` |
| `cod3/q_shared.cpp` | strings (`Q_stricmpn`, `Q_stricmp`, `Q_strlwr`, `Q_strcat`, `COM_GetExtension`, `COM_DefaultExtension`, `Com_sprintf`) and info strings (`Info_ValueForKey`, `Info_RemoveKey(_Big)`, `Info_SetValueForKey(_Big)`) |
| `cod3/name_hash.cpp`, `cod3/cvar.cpp`, `cod3/files.cpp` | the hashes: script names (`NameHash`, `NameHash_Init`), the dvar table (`generateHashValue`), the file system (`FS_HashFileName`) |
| `cod3/tr_public.h`, `cod3/tr_cmds.cpp`, `cod3/ngl_quad.cpp` | the renderer as the game sees it: the 2D color (`RE_SetColor`) and pictures (`RE_StretchPic`), Treyarch's NGL quad (`nglInitQuad`, `nglSetQuadRect`, `nglSetQuadUV`, `nglSetQuadColor`, `nglListAddQuad`), and the table of renderer calls `GetRefAPI` hands out (`refexport_t`, slot by slot). the free-form quad (`RE_DrawQuadPic`, slot 21) and the far plane (`RE_GetZFar`, `RE_SetZFarMin`, slots 9 and 14; names ours). `nglListAddQuad` hands each quad to `nglQuadSink` on a PC; the checker catches the console's call to it, so `RE_StretchPic` and `RE_DrawQuadPic` are checked by the quad they draw |
| `cod3/common.cpp` | `Com_Error`: the game jumps back to its frame loop, this throws a `ComError` the caller catches |
| `cod3/xbox_crt.{h,cpp}` | the Xbox 360 C runtime's `sin` and `cos`, its "C" locale `isalpha`/`tolower`, and `_vsnprintf`'s rules |
| `cod3/scr_math.{h,cpp}` | the level scripts' math built-ins (`Broc_Abs` ... `Broc_VectorNormalize`) and random numbers |
| `cod3/ppc.h` | the console's arithmetic rules (fused multiply-add, conversions, `fsel`) |
| `cod3_check/` | `cod3_check_cpp`, the checker's answerer |
| `tests/` | `cod3_golden` (the recorded answers) and `cod3_unit_tests` |

File names follow Quake III's (the game descends from it through Call of Duty 2); the console's own
file names aren't known. Quake III's source (GPL) is for names and understanding only: this is
written from the recompiled code and the checked Rust ports, never copied from it.

## Building

```
cmake -S engine/cod3_cpp -B <build folder> -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build <build folder>
ctest --test-dir <build folder> --output-on-failure
```

Standard C++20 and its library only. Windows (MSVC 2022 or clang-cl) is meant to work the same way.
