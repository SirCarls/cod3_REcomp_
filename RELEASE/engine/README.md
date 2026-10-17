# CoD3 × WaW engine workspace (Rust)

This is the native Rust side of the project. It builds on Linux and Windows with `cargo`, and there's no Wine/Proton anywhere. It holds:

- the rippers, which turn the user's own game files into open formats;
- the engine (IW4L-based), which comes next.

| Crate | What it does |
|---|---|
| `crates/inflate` | DEFLATE/zlib decoder with no dependencies. It reads WaW fastfiles and `.iwd` zip members. |
| `crates/lzo` | LZO1X decoder with no dependencies. Call of Duty 3 packs its archives' chunks with it ("NCH" blocks). |
| `crates/kapf` | Call of Duty 3 `.cod` archive reader. `kapf` covers the KAPF header, sections, chunk table and NCH/LZO1X unpacking. `kapf::apkf` covers the APKF directory, data areas, and pointer and import tables; `Chunk::link` follows a field to what it points at. |
| `crates/xenos` | Xbox 360 GPU texture data: fetch constants, untiling, endian swap, DDS output (also with mip chains), BC1/2/3/5 → RGBA (`bc`), RGBA → BC1/2/3/5 (`bc_encode`), and the HD texture key (XXH64, `xxh64`). |
| `crates/png_write` | RGBA → PNG (uses `miniz_oxide` for zlib). |
| `crates/png_read` | 8-bit PNG (gray, RGB, gray + alpha, RGBA; not interlaced) → RGBA, using `inflate`. |
| `crates/cod3_assets` | CoD3 materials (MAT), models (MLTI) and texture sets. |
| `crates/cod3_mesh` | CoD3 meshes: surfaces, vertex declarations, Xbox 360 buffers, LODs → glTF 2.0 with materials. |
| `crates/ff_t4` | World at War (T4) fastfile reader for PC and Xbox 360 (unsigned and signed): container, zone header, script strings, asset table. Raw files (GSC/CSC) are found today; the full asset loader is next. Formats are in `../docs/formats/waw.md`. |
| `crates/cod3_game` | Call of Duty 3's own code decompiled into Rust, one function at a time: `shared`, `math` and `script` are the exact console behavior, checked against the original; `engine` is the layer the remake uses, with the same results and none of the Xbox 360's limits (see `../docs/DECOMP.md`). No dependencies. |
| `tools/rip` | The command-line ripper (below). |
| `tools/decomp_check` | Runs the `cod3_game` ports for the decomp checker (`../tools/decomp/decomp.py check`). |
| `crates/iw_pc` | Call of Duty 2 (1.3) and United Offensive (1.51) PC code decompiled into Rust: each function checked against both games' x86 originals (`../tools/decomp/x86/`). The exact "original layer"; where a function behaves like Call of Duty 3's, it reuses `cod3_game`'s port. No dependencies besides `cod3_game`. |
| `crates/game_sources` | Finds Call of Duty, United Offensive and Call of Duty 2 installs (Steam on Windows and Linux, or any folder) and makes new copies of their data archives for the engine; executables are never read (`../docs/NEW_COPY.md`). No dependencies. |
| `tools/newcopy` | `newcopy find / make / check`, the command line of `game_sources`. |
| `tools/pc_check` | Runs the `iw_pc` ports for the x86 checker (`../tools/decomp/x86/pcdecomp.py check`). |
| `tools/iw_server` | `iw-server`: a LAN server for Call of Duty 2 and United Offensive that the games' own PC clients can find and join, built on `iw_pc::server` ([PC_SERVER.md](../docs/PC_SERVER.md)). |
| `cpp/` | The C++ version of `iw_pc`, for developers (CMake, C++20, standard library only): every ported function of the two PC games also in C++, checked against the same originals (`../tools/decomp/x86/pcdecomp.py check --impl cpp`). The Rust crate stays the full version ([cpp/README.md](cpp/README.md)). |
| `crates/machine` | What the machine has — logical CPUs, the ones this process may use (affinity, Linux cgroup quota), physical cores, shared-L3 groups, efficiency cores, RAM, Wine — read once, and the thread budget from it. The same profile and the same budget as the ReXGlue port's `game/machine_profile.cpp`, checked against the one table `crates/machine/budget_cases.tsv` (see `../docs/SCALING.md`). Standard library only. |
| `crates/jobs` | The engine's job pool, sized from `machine`: a parallel loop the asking thread helps with (the renderer records draws this way, so a frame's work spreads over every core instead of one GPU thread) and three priorities (frame, now, background; background never takes every worker). No dependencies besides `machine`. |

Formats are documented in `../docs/formats/cod3.md`; sources are in `../docs/SOURCES.md`.

```
cargo build --release
cargo test

rip cod3-check <file.cod | folder>    check every layout rule (all CPU cores)
rip cod3-all <folder> <out>           textures (DDS + PNG), meshes (glTF), materials (JSON)
rip cod3-tex <file.cod> <out>         one archive's textures
rip cod3-mesh <file.cod> <out>        one archive's meshes and materials
rip cod3-split <file.cod> <out>       one archive's resources as raw object/GPU files
rip cod3-chunks <file.cod> <out>      one archive's unpacked chunks (research)
rip cod3-hd-pack <textures> <upscaled> <out>
                                      the HD texture pack: upscaled PNGs (any scale) as DDS in
                                      the game's formats with full mip chains, plus index.tsv
rip waw-ff <file.ff> <out>            a WaW fastfile's asset list and raw files
rip version                           version, and the output layout number the setup tool checks
```

`cod3-check`, `cod3-all` and `cod3-hd-pack` use every CPU core; `--jobs N` limits them. The output of `cod3-all` is:

- `textures/<name>.dds` and `.png`, shared by all archives (the game finds textures by name);
- `textures/hd_keys.tsv`: the key of every texture instance for the HD texture pack, with the PNG that holds its own picture (`cod3-tex` writes it too); other pictures saved under a name another archive already used are `textures/<name>__<key>.png`;
- `meshes/<archive path>/<mesh>.gltf` + `.bin`, linking `../textures/*.png`;
- `materials/<archive path>.json`: every material's parameters and every model's parts.

Files whose content didn't change are not rewritten, so their modification times stay. The HD texture pack (`hd_keys.tsv`, `cod3-hd-pack`, `index.tsv`) is described in `../docs/formats/cod3.md` and in `tools/rip/src/hd.rs`. `cod3-hd-pack` doesn't assume a size for the upscaled PNGs: any scale with the original's aspect ratio is packed (1x, 2x, 3x, 4x, 2.5x ...); BC textures get sides that are multiples of 4, the longest side is capped at 8192, and the summary counts the textures per scale.

The setup tool (`../COD_3_WAW_Setup-tool/c3w/rippers.py`) builds `rip` into its work folder with `cargo build --release --locked`, or uses a prebuilt one from `bin/<platform>/` (release downloads carry them; see `../.github/workflows/release.yml`). When the output layout changes, bump `OUTPUT_LAYOUT` in `tools/rip/src/main.rs` and `RIP_OUTPUT_VERSION` in `rippers.py` together.

`../tools/run_local.sh` (Linux) and `../tools/run_local.ps1` / `../tools/run_local.bat` (Windows) run the build, the tests, the checks and the rips on a developer's computer, and write their results to `../dev/`.

## Writing code people can debug

- **Names say what a thing is.** Write `bit_buffer`, `header_ptr`, `AssetType::XModel`, not `b`, `p2` or `t5`. A short name is fine only for a small, obvious scope, such as a loop index.
- **No unexplained numbers.** File format constants get a named `const` with a comment naming where the value comes from, for example `VERSION_PC = 387`.
- **Each module starts with a doc comment.** It describes the format or job, and the source it follows (see `../dist/docs/SOURCES.md`).
- **Comments explain why, not what.** Examples: "structs are aligned to their block offset, not the stream offset, so every byte position is a candidate".
- **Errors carry context.** Say what was being read, for example `Error::Truncated("asset table")`, and print the file path at the top level.
- **Game paths are untrusted input.** They go through `safe_join`, so a crafted name can't write outside the output folder.
- **Formatting:** `cargo fmt` (rustfmt defaults, 100 columns) before every commit. `cargo clippy` should be clean.
- **Tests:** every decoder gets a small test with known input and output. Tests use generated data, never retail game data.
- **Recompiled code:** the generated C++ in the recomp (`recomp/generated/`) is machine output and is never edited by hand. Everything written by hand, in both the recomp and here, follows these rules.
