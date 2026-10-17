# The decomp trace: play the game, map its functions

The **decomp trace version** of the recompiled Call of Duty 3 is the normal
game with one addition: every one of the game's ~19,000 functions (and the
~5,000-6,000 of each level's script library) notes the first time it runs,
what the game was doing then, and which functions call it. It also notes
what some well-known game functions are asked for (which dvar, which file,
which console command), keyed by the function that asked. You play as usual;
every minute and when you quit, it writes what it saw to a folder you can
send in. A merge tool then turns any number of those folders into a list of
functions nobody has named yet, with everything that was seen about each of
them, and a suggested name where the evidence is strong.

Most of the game's functions are still called `sub_82xxxxxx`. Knowing that a
function first ran "in Saint-Lô, just before I planted the bomb", that it is
called from `G_RunFrame`, and that it reads `g_gravity` is often enough to
name it, and every name makes the decompiled code easier to read.

## Getting it

**Setup tool:** tick **Also build the decomp trace version** (CLI:
`--trace-build`). After the normal game, the tool compiles the game a second
time in a build folder of its own (`work/cod3/out/build/trace`) and puts it in
`<game folder>/bin-trace/`, next to the normal one. It uses the same game
files, settings and saves. The normal game is exactly the same as without
the option. Starting it:

- Windows: `Call of Duty 3 (decomp trace).lnk` in the game folder;
- Linux: `Call of Duty 3 (decomp trace).sh` in the game folder.

**By hand** (in the recomp project, after a normal build has translated the code):

```
cmake -S . -B out/build/trace -G Ninja -DCMAKE_BUILD_TYPE=Release -DCOD3_DECOMP_TRACE=ON -DCOD3_BUILD_MP=OFF
cmake --build out/build/trace
```

Only the campaign program is traced; `-DCOD3_BUILD_MP=OFF` skips building the
multiplayer program a second time.

The game in `out/build/trace/` finds the game folder like the normal one
(copy it into `<game folder>/bin-trace/`).

## While you play

Play normally; the trace needs nothing from you. It helps most when you:

- **play different things**: every level, the menus and each options page,
  vehicles and turrets, cutscenes, dying and loading a checkpoint, saving,
  the credits. A function shows up once it has run once;
- **add markers.** Press **F8** right after something worth noting and type
  what just happened ("planted the bomb", "entered the tank", "opened the
  map", "died to a grenade"), then Enter (Backspace corrects, Esc cancels).
  The game doesn't get your keys while the box is open, and what you type
  is kept even while the game is busy (during a load the box can show up a
  moment late). Functions first seen from 30 seconds before you pressed F8
  to 10 seconds after you saved the marker get it, so a marker in a place
  you have already played gets few or none: that is normal. A marker never
  has to be exact: "something exploded" is useful too;
- in the **~ console**: `trace_mark <words>` does the same as F8 (handy in
  `cod3_autoexec`), `trace_status` shows how many functions were seen and the
  session's folder, `trace_write` writes the files now.

The trace also labels each first call itself, from what the game is doing:
the level (or `front end`) and `loading`, `playing`, `cutscene`, `intro`,
`movie`, `menu`, with `+vehicle` while you are in a vehicle or on a turret
(the game's interaction system) and `+menu` while a menu is open in a level.
It can't tell a firefight from a quiet walk: use a marker for that.

## Where the files go

`<game folder>/userdata/decomp_trace/<session>/`, one folder per start of
the game, named by when it started (UTC, e.g. `20261015-213000`). It is
written every minute and when you quit (a crash keeps the last minute's
files). Each is a tab-separated text file with a header line:

| File | What is in it |
|---|---|
| `functions.tsv` | Every function that ran: module (`default` = the game, else the level), guest address, name, first seen (seconds since the start), level and state then, the marker it got, approximate number of calls, up to 8 callers (`Com_Frame+0x3C`, `saint_lo!sub_89031000+0x44`) |
| `calls.tsv` | The same callers, one line each, with the caller's function address |
| `evidence.tsv` | What known game functions were asked by each caller: `print` (the game's own print formats), `dvar` / `dvar_register` / `dvar_set` (dvar names; for a registration also its default), `file` (game file paths), `command` (console commands run, their name only), `command_handler` (which function handles which command), `name` (names hashed for asset and table lookups), `level_slot` (the function behind each slot of the table the level scripts call) |
| `markers.tsv` | Your markers: number, when F8 was pressed and when it was saved, level, state, text, how many functions got it |
| `modules.tsv` | Per module: functions in it, seen, seen with a name, seen without |
| `trace.tsv` | The file format, the session's start (UTC), how long it ran, totals and the rules above |

A session is a few MB (the cloud test, 10 minutes of front end and
Saint-Lô, wrote 2.2 MB); the first ~20 minutes of play see most of the
functions, later minutes add little.

## Sharing it

Zip the session folders (or the whole `userdata/decomp_trace/` folder) and
attach the zip where you share logs. Old sessions can be deleted at any time;
nothing else reads them.

## Privacy: what is and isn't in them

In the files:

- the game's guest addresses and the names of its functions (from this
  project's symbol map), and seconds since the session started;
- the game's own text: dvar names and defaults, print formats (the game's
  constant strings, never what was printed), game file paths (inside the
  game's data), console command names, the names the game looks up, level
  names;
- the session's start date and time (UTC), in the folder name and `trace.tsv`;
- **your marker text**, exactly as you typed it, so don't type anything
  personal in a marker.

Not in the files: any path on your computer, your user name, the computer's
name, your hardware, your settings, your profile (gamertag) name, and
anything typed in the console except the names of commands the game has.
Text is only taken when it is the game's own: a dvar name or print format is
kept only when it sits in the game's read-only data, a file path only when
it is inside the game's folder, and every value that contains your user
name, the computer's name or the profile's name is dropped before writing,
just in case.

## What it costs

- **Speed.** Each game function call does a few more instructions (a call
  counter and one test); the first calls of a function and then one call in
  1,024 do a little more. In the cloud test the difference was smaller than
  the test machine's own noise (see [the measurement](#measured-overhead)
  below); the worst case works out to a few percent of one CPU core. The
  frame rate limit and game speed are unchanged.
- **Building.** The game is compiled a second time: about as long again as
  the normal build (20-90 minutes), and about 2 GB more in the work folder
  while it builds. The trace version itself takes about 370 MB in the game
  folder (Linux; the normal game's files are about 310 MB).

### Measured overhead

The same scripted session (front end, `devmap saint_lo`, 3 minutes in the
level, quit) on the project's cloud machine (2 cores shared with other jobs,
software rendering), normal game and trace version one after the other,
with the CPU time of the game's main thread read from the system:

| | Normal | Trace version |
|---|---|---|
| Saint-Lô load (wall clock) | 72.6 s | 69.6 s |
| Saint-Lô load (main thread CPU) | 38.9 s | 34.1 s |
| First 300 front-end frames (main thread CPU) | 75.7 s | 66.2 s |
| Whole session, all threads (CPU) | 852 s | 862 s |

The trace version came out no slower: its cost is below the noise of a
shared machine (an earlier pair of runs gave a 111.8 s normal load against
71.5 s traced). For a bound, a synthetic test with 16,384 small stand-in
functions, 10% of the calls going to random cold ones, measured about 9 ns
added per call on such cold calls and next to nothing on hot ones. The game
makes about 7 million function calls a second outside its two wait loops
(31 million with them; the wait loops only spin), so even at that worst
case for every call the trace costs under 7% of one core.

## From traces to names

```
python3 tools/trace/merge_traces.py <game folder>/userdata/decomp_trace -o trace_merged
```

`merge_traces.py` reads any number of session folders (or folders holding
them), the symbol map (`COD_3_WAW_Setup-tool/recomp/config/symbols.toml`,
found by itself; `--symbols` for another) and, when they are there, the
World at War / CoD2 pairing tables the decomp chats keep in
`dev/waw-decomp/cod3/` (`--pairs TABLE.tsv[:ADDRESS_COLUMN:NAME_COLUMN]` for
others). Python 3.8+, nothing to install. It writes:

| File | What is in it |
|---|---|
| `coverage.tsv` | Functions seen per module and per level they were first seen in (`(start-up)`: before the game knew where it was), with how many have a name |
| `functions.tsv` | Every function seen, all sessions together (sessions, first-seen levels and states, markers, calls, callers, named callees, evidence); callers and callees carry the symbol map's names where the build didn't have them |
| `candidates.tsv` | Every function seen that has no name yet, with all of that, the pairing tables' names, and a **suggested name only where the evidence is strong**: the handler of exactly one console command (`CL_Vid_Restart_f`, prefix from the function that added it), the function behind a level script slot whose builtin is known (`Broc_...`), a print that names its function (`R_LoadWorld: ...`, the Quake III habit), or two pairing tables that agree. A name already used by another address in the symbol map, or one two functions would get, is not suggested (the reason column says why) |
| `curated_suggestions.txt` | The suggested names for `default.xex` as `CURATED_FUNCTIONS` lines |
| `summary.txt` | The counts |

Nothing goes into the names by itself. A person checks each suggestion
against the code (the generated C++ in `recomp/generated/` and the evidence
above), then copies the good ones into their own block at the end of
`CURATED_FUNCTIONS` in `tools/name_functions.py` and regenerates the symbol
map and the codegen's names (`docs/DECOMP.md`, "Name what you found"). The
unnamed functions with evidence but no suggestion are the to-do list: their
callers, markers and dvars usually point at the right system.

## How it works (for developers)

- **The prologue.** Every recompiled function starts with
  `REX_FUNC_PROLOGUE()`. The trace build adds `src/game/trace/trace_prologue.h`
  to the precompiled header of every generated-code target, after the
  codegen's `cod3_pch.h`, and it redefines that macro: a constant-initialized
  static `Slot` per function (its name from `__func__`), a plain load and
  store of its call count (no atomic read-modify-write, so hot functions
  called from several threads don't fight over it; the count is
  approximate) and one test. The first 16 calls, and then one call in 1,024,
  go to the module's slow path (`trace_module.cpp`), which registers the
  slot with the game once and notes the caller (`ctx.lr`). The register
  save/restore helpers aren't traced.
- **No SDK change; normal builds untouched.** The generated code, its
  `cod3_pch.h` and the SDK are the same for both builds; only the trace build
  folder compiles them differently. (A new branch in the SDK's `pch_h.inja`
  would have changed every module's generated `cod3_pch.h`, and every normal
  build would have recompiled all of the generated code once.)
- **Modules.** The executable and every level library get their own copy
  of the slow path and a table of their functions (made at build time from
  `generated/<module>/cod3_init.cpp` by `gen_trace_names.cmake`). They reach
  the game's registry through `cod3_decomp_trace_registry()`, which the
  executable exports (Windows: `dllexport`; Linux: a dynamic list). The level
  libraries all load at one address, so a caller in level code also carries
  the id of the level library that ran it.
- **Evidence** (`src/game/trace/evidence.cpp`): a few game functions get a
  collector when their slot registers (by address: `Cvar_FindVar`,
  `Cvar_Get`, `Cvar_Set2`, the `FS_` open/read functions, `Cmd_ExecuteString`,
  `Cmd_Add*Command`, `NameHash_Init`, `SV_SpawnServer`, the loading screen,
  `Cbuf_Execute` for the context); their slot then takes the slow path on
  every call. The prints come from the port's own print hooks
  (`game/print_hooks.cpp`), since retail `Com_Printf` is empty. The level
  interface table is read once per level.
- **The writer** (`src/game/decomp_trace.cpp`): the list of seen functions,
  the context, markers and evidence, written atomically (a temporary file,
  then a rename) every minute and from `Cod3App::SaveAll` before the game
  ends. `src/game/trace/trace_format.{h,cpp}` holds the file rules, tested by
  `trace_format_test.cpp` (`cmake --build out/build/trace --target
  cod3_trace_format_test`).

### Limits

- Call counts are approximate (calls on several threads at once can be lost).
- Up to 8 distinct callers per function, from its first calls and every
  1,024th after that: a function called from many places shows some of them.
- A function the port replaces without calling the original (`Com_Printf`)
  doesn't appear; one the port wraps does.
- A tail call (`b` to another function) shows the caller's caller.
- The context is read four times a second, and between frames while a level
  loads.
