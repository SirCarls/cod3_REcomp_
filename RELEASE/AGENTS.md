# Working on this project (for coding agents)

Read this first, whatever tool you are: a Claude chat, Claude Code, Copilot or another agent. People read it too.

## What the project is

A PC port of Call of Duty 3 (Xbox 360), built from the player's own disc, and, long term, a native Rust remake with World at War content. The goal and the rules are in [docs/REQUIREMENTS.md](docs/REQUIREMENTS.md); sections 1 and 2 are required reading.

| Folder | What's there | How to check a change |
|---|---|---|
| `COD_3_WAW_Setup-tool/` | The setup tool (Python, `c3w/`), the recomp project (`recomp/`, C++ on the ReXGlue SDK), SDK patches (`patches/`) | `python3 -m unittest discover -s COD_3_WAW_Setup-tool/tests`; recomp: a Linux build of the game |
| `engine/` | Rust: rippers (`crates/`, `tools/rip`) and the Rust decomp (`crates/cod3_game`) | `cargo fmt --check`, `cargo clippy`, `cargo test` in `engine/` |
| `tools/` | Developer tools: the symbol map (`name_functions.py`), the decomp checker (`decomp/`), release and repo checks | `python3 tools/check_repo.py` |
| `docs/` | How everything works: [ENGINE.md](docs/ENGINE.md), [DECOMP.md](docs/DECOMP.md), formats, [SOURCES.md](docs/SOURCES.md) | |

## Ground rules (from the owner; details in REQUIREMENTS section 2)

- **No game data and no personal information in anything published**: no files from a disc, no personal paths, user names or machine details. `tools/check_repo.py` enforces it. Game files for local work go in `dev/` (never published).
- **Never run** either game's executable, a patch, mod or map installer, and never touch DRM. Installers are unpacked, not run. Nothing depends on Wine or Proton. (The 2026-10-13 Xenia exception was withdrawn on 2026-10-16: multiplayer is PC only, REQUIREMENTS section 2.)
- **Ask before downloading or installing anything**, and say where it goes. Everything that can stays inside the project folder (`work/tools/` for tools).
- **No hard-coded paths**; Windows and Linux both work.
- **Readable code**: names that say what things are, comments that say why, every outside source recorded in `docs/SOURCES.md`. American spelling.
- **Generated code is never edited by hand** (`recomp/generated/`). SDK changes are numbered patches in `COD_3_WAW_Setup-tool/patches/`.
- **Performance work never changes game speed**: the game clock follows real time.
- **Quake III's source (GPL) is for understanding and names only.** Code here is MIT; never copy GPL code in.

## Working alongside other agents

Several agents often work on this folder at once and can't see each other. To avoid overwriting each other:

1. **Read the board** `dev/agents/BOARD.md` before you start and before every sync (it's private, so it exists only on the owner's machine). Add a claim: who you are, what you're doing, which files and folders you'll change. Stay out of what others claimed.
2. **Compare before you overwrite.** If a file changed since you last read it and you didn't change it, someone else did: merge their change into yours.
3. **Shared files get small additions**, not rewrites: `mod` lines, table rows, your own block in a list.
4. **One build at a time** on a shared machine (hold `flock dev/agents/.build.lock` around a build on Linux).
5. **When you stop**, mark your claim done and write your notes to `dev/agents/<your-name>/NOTES.md`.

## The Rust decomp

The game's functions become Rust in `engine/crates/cod3_game`, each checked against the original machine code on thousands of inputs. Two layers: `shared`, `math`, `script` are the exact record (with the Xbox 360's limits); `cod3_game::engine` has the same results without them, and the remake uses it. It splits into areas that agents can take one each. Workflow, rules and what's next: [docs/DECOMP.md](docs/DECOMP.md).

- **Multiplayer is PC only** (owner 2026-10-16: no Xbox 360 cross-play): the recomp on Linux and Windows and the Rust engine play in one match, with dedicated servers in the plan. The recomp runs the console's own network code, so its messages are the wire format the Rust engine speaks ([docs/MULTIPLAYER.md](docs/MULTIPLAYER.md); what crosses the network: [docs/MP_PROTOCOL.md](docs/MP_PROTOCOL.md)). The console layer stays exact as the correctness reference; the engine layer's lifted limits are for what stays on one machine.
- **Loops are fast and never choke**: no allocation per call in what runs every frame, nothing slower than linear, exact console arithmetic without slow library calls (`ppc::fmadds`, not `f32::mul_add`). Every engine function has a row in `engine/crates/cod3_game/examples/bench.rs`; run it before and after a change ([DECOMP.md, Speed](docs/DECOMP.md#speed)).
