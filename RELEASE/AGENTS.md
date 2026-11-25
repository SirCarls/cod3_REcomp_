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
- **Native rendering is required for every game** (owner 2026-11-07 18:06: "Yes native rendering is required for all
  of these"): World at War, Call of Duty 4, Call of Duty 2 and the rest each get a native renderer (as Big Red One and
  Call of Duty 3 have, through `ngl_vk/`); the SDK's GPU emulation is the reference and a fallback, not how a game
  ships. Plan and sources: `dev/agents/acct2-bro/NATIVE-RENDERERS.md`.
- **Every outside project is for information only, whatever its license** (owner 2026-11-07): KisakCOD, KisakBlack,
  OpenCoD1, IW4x, and IW4L although it is Apache-2.0. Read them for names, formats and behavior; never copy their code.
- **Search the project before you brute-force** (owner 2026-11-09 19:24: "Please really search the project regarding
  these values before bruteforcing something"; 19:35: made a project-wide rule). Before you write a probe, a logging
  build or a test run, or guess at an address, value, constant or layout, look for what the project already knows:
  the decompiled code and its tables (`dev/waw-decomp/*/decomp/`, `names.tsv`, `globals.tsv`, `strings.tsv`), the
  symbol maps (`recomp/config/symbols.toml`, `names.toml`), `docs/`, other agents' notes and test logs
  (`dev/agents/*/NOTES.md`, `TEST-RUNS.md`, the board), saved shader dumps and run results, and the claude.ai Project's
  docs. Write in your notes what you searched and what it gave. Probe only for what is still unknown, and say so.
- **Builds and test runs go side by side** (owner 2026-11-24 03:02: "get multiple sessions working at the same time
  make this a project wide rule"). Every game and thread runs its own builds and untimed runs in parallel, each in its
  own REA session, through `dev/tools/rea/runq.sh` (how: `dev/agents/PARALLEL-RUNS.md`). Only three things wait for
  each other: timed or measuring runs (`results/game.lock` exclusive; untimed runs take it shared), system-link tests
  whose copies must share an address, and builds into the same work tree. Each run gets its own loopback address
  (`net_interface`), so games never fight over a port.
- **Look at the outside projects for help** (owner 2026-11-24 03:46: "Please continue to look at the other github
  projects for help or solutions. Please make this a rule for everychat that has a recomp or PC files."). Every chat on
  a recomp or PC files checks the outside reference projects in `docs/SOURCES.md` for help and solutions, especially
  while builds and tests run (ZoneTool, OpenAssetTools, KisakCOD, FinestHourRemastered, the CoD2-BRO native loader,
  FrankHUN's CoD3Recomp, and any the owner adds). Information only, never copied: facts are re-derived in our own
  disassembly or decomp; each one used gets a SOURCES.md row with its license, and notes say what was looked at.
- **Every game works properly at any frame rate** (owner 2026-11-24 04:37: "Yeah so every game needs to be able to
  function propery regardless of FPS. This is a project wide rule."). Campaign and multiplayer, every game works the
  same at 20, 30, 60, 144 and uncapped: no timing, drops, drift, physics, animation, input, fade or other visual bug
  that depends on the frame rate. Per-frame steps are scaled by the frame's time; nothing counts frames or vertical
  blanks as time. Anything touching timing is tested at a low and a high cap (at least 30 and 144, plus uncapped)
  before it lands.

## Working alongside other agents

Several agents often work on this folder at once and can't see each other. To avoid overwriting each other:

1. **Read the board** `dev/agents/BOARD.md` before you start and before every sync (it's private, so it exists only on the owner's machine). Add a claim: who you are, what you're doing, which files and folders you'll change. Stay out of what others claimed.
2. **Compare before you overwrite.** If a file changed since you last read it and you didn't change it, someone else did: merge their change into yours.
3. **Shared files get small additions**, not rewrites: `mod` lines, table rows, your own block in a list.
   **The board itself is written only through `dev/tools/board_write.py`** (it locks, checks, writes a temp file and
   renames it) **or, from a session that can't write the project drive, a JSON drop in
   `COD3-rea-work/board-drop/pending/`** (`dev/tools/board_drop.py` applies it). Never with device_commit_files, the
   Edit or Write tools or any whole-file copy, mtime guard or not: on this drive such a write doubled the board and
   lost an hour of entries on 2026-11-24. Other files keep the usual stage, compare and commit protocol.
   **Never touch the Windows (NTFS) drive at the disk level** (owner 2026-11-24 05:18: "no DO NOT FUCK WITH MY
   WINDOWS DRIVE I WILL KILL YOU"): on the project drive (sde2) or any NTFS volume, no ntfsfix, chkdsk, fsck,
   mount, umount, remount, repair, partition or disk tool, never suggested to the owner, and no volume write
   tests. Only normal file reads and writes inside the project through the approved tools.
- **Solve it by math and logic first, compile and run last** (owner 2026-11-24 06:54: "Please make this a project
  rule: attempt to solve issues with math or logically checking rather than compiling, is the issue can't be solve
  this way then you can test. This should speed things up."). Settle a question first by reading the code or the
  decompile, working the numbers, and static or syntax checks (-fsyntax-only, glslc, unit tests that don't need
  the game). Build and run the game only when that can't settle it, or once to confirm the final change before
  landing (rule 9's landing checks still apply). Batch what still needs running into one build.
4. **Builds into the same work tree go one at a time**; other builds run side by side (`dev/tools/rea/runq.sh build`;
   a build that writes into the project folder also holds `dev/agents/.build.lock`).
5. **When you stop**, mark your claim done and write your notes to `dev/agents/<your-name>/NOTES.md`.

## The Rust decomp

The game's functions become Rust in `engine/crates/cod3_game`, each checked against the original machine code on thousands of inputs. Two layers: `shared`, `math`, `script` are the exact record (with the Xbox 360's limits); `cod3_game::engine` has the same results without them, and the remake uses it. It splits into areas that agents can take one each. Workflow, rules and what's next: [docs/DECOMP.md](docs/DECOMP.md).

- **Multiplayer is PC only** (owner 2026-10-16: no Xbox 360 cross-play): the recomp on Linux and Windows and the Rust engine play in one match, with dedicated servers in the plan. The recomp runs the console's own network code, so its messages are the wire format the Rust engine speaks ([docs/MULTIPLAYER.md](docs/MULTIPLAYER.md); what crosses the network: [docs/MP_PROTOCOL.md](docs/MP_PROTOCOL.md)). The console layer stays exact as the correctness reference; the engine layer's lifted limits are for what stays on one machine.
- **Loops are fast and never choke**: no allocation per call in what runs every frame, nothing slower than linear, exact console arithmetic without slow library calls (`ppc::fmadds`, not `f32::mul_add`). Every engine function has a row in `engine/crates/cod3_game/examples/bench.rs`; run it before and after a change ([DECOMP.md, Speed](docs/DECOMP.md#speed)).
