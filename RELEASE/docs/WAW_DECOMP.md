# Decompiling World at War

World at War's code, turned into readable C with names, as the reference for porting
its systems (weapons, zombies, scripts, menus) to the Rust engine. Two builds are
decompiled and matched against each other: the **PC** version and the **Xbox 360**
version.

The output is derived from the game, so it stays private: it is written to `dev/waw-decomp/`
(never published). Only the tools and this document are in the repository.

## Ground rules

- Executables are only **read**. Neither game is run, and nothing is patched.
- **No DRM removal.** The Steam `CoDWaW.exe` keeps its code encrypted by Steam's wrapper
  (a `.bind` section; the code section has the entropy of random data), so it can't be read
  as is, and our tools never decrypt it. The decompile reads an unwrapped copy of the same
  1.7 build that the owner already has. It is the same build: identical timestamp and
  identical `.rdata`, `.data` and `.rsrc`.
- Names from other projects are **facts (name + address)** taken with their source recorded
  in [SOURCES.md](SOURCES.md); no code is copied from them.

## What is decompiled

| Build | File | Status |
|---|---|---|
| PC 1.7, singleplayer + zombies | `CoDWaW.exe` (timestamp 2009-10-29, image base 0x400000) | 14,980 functions found, 14,836 decompiled (the rest are import thunks); 3,734 named + 2,071 named globals; register parameters recovered; types (`dvar_s *` globals, 107 prototypes). |
| PC 1.7, multiplayer | `CoDWaWmp.exe` | 14,994 functions, 14,851 decompiled; 3,356 named + 1,887 named globals; 10,968 functions paired with SP. |
| Xbox 360, singleplayer + zombies | `default.xex` | 13,708 functions (10,658 from `.pdata`), 13,699 decompiled (with the script handlers the table pass found), every instruction decoded (VMX128 included); 2,369 named + 1,593 named globals (1,522 typed `dvar_s *`); 5,484 functions paired with their PC function (paired functions without a name carry the PC address as a note). |
| Xbox 360, multiplayer | `default_mp.xex` | 13,003 functions (10,137 from `.pdata`), 12,985 decompiled, every instruction decoded; 2,038 named + 1,516 named globals (1,480 typed `dvar_s *`); 4,565 paired with PC. |

The Xbox 360 executables also hold the console's own libraries (Direct3D, XAudio, the C runtime,
Bink), which have no PC counterpart, so a good part of their functions will never pair with PC.

## Tools (`tools/waw_decomp/`)

| Tool | What it does |
|---|---|
| `xiso_get.py` | Copies only the program files (`*.xex`, `*.dll`) out of an Xbox 360 disc image (read-only). |
| `xex2pe.py` | XEX2 → plain PE image: retail AES decryption, basic and LZX compression, import list (`--imports`, named from the ReXGlue SDK's kernel export tables). |
| `xexlzx/xexlzx.c` | Small LZX helper for `xex2pe.py`, built on libmspack's decoder. |
| `ghidra_xenon/` | Adds the Xbox 360 CPU to Ghidra: language `PowerPC:BE:64:Xenon-32addr` with the 75 VMX128 vector instructions (128 vector registers) and Xenon's `lvlx`/`lvrx`/`stvlx`/`stvrx`. Stock Ghidra stops at the first VMX128 instruction. `install_xenon.py <ghidra>` installs it. |
| `ghidra_scripts/` | Headless Ghidra scripts: `WawFindFunctions` (functions reached only through pointer tables), `WawXenonSetup` (Xbox 360: places the image, sections, `.pdata` functions, kernel import names), `WawEnableParamId`, `WawApplyNames` (names and notes), `WawApplyTypes`, `WawExport` (tables + the C of every function), `WawStats` (functions, undecodable words, VMX128 count), `WawFixBadInstructions` (disassembles again where the language learned an instruction). |
| `run_pc.sh`, `run_x360.sh` | The whole pipeline for one executable: import, analysis, naming, decompile. |
| `name_pc.py`, `name_x360.py`, `naming_common.py` | The naming passes (below). |
| `match_builds.py` | Pairs the functions of two builds (Xbox 360 and PC, or SP and MP) by shared strings, named globals, the call graph and tables of function pointers; `--holdout` measures how often it agrees with pairs it was not shown. |
| `crossref.py` | One table of every function across the four builds: its name and its address in each. |
| `method_names.py` | Runs the `method` pass alone on any export (used for CoD3, whose names come from its symbol map). |
| `carry_addresses.py` | Finds known addresses of one Xbox 360 build in another (CoD3 campaign -> multiplayer): functions through the pairs, places inside functions by lining up the instructions, globals by the addresses the aligned instructions form, strings and RTTI vtables. |
| `script_usage.py` | Which builtins a set of scripts calls; with `--crossref`, each handler's address in all four builds. |
| `t4sp_types.py` | The T4SP plugin's headers → one C header + typed symbols, for the private decompile. |
| `script_api.py` | The script API: every `{ "name", handler }` table entry, its table and the function that reads it. |
| `kisak_strings.py` | Lists each KisakCOD function's strings and callees, its `{ "name", handler }` tables (with the array each entry is in) and the functions that read each array, for the naming passes. |
| `public_symbols.tsv` | 812 names with addresses for the PC 1.7 executables from public projects, each checked against the executable's bytes; 794 are used. T4M's six `*_GetMethod` addresses are not: each reads another class's table than its label says (0x4DEEA0, labelled `Player_GetMethod`, reads the 107 actor methods), so the `reader` pass names them instead. |

## How functions get their names

Each name records the pass that gave it and its evidence; in the decompiled C it is the
comment above the function (`name: kisak (12 string(s) shared with KisakCOD UI_SetActiveMenu)`).
Passes run strongest first; an address or a name is taken by the first pass that claims it.

| Pass | Evidence | PC SP / MP | Xbox 360 SP / MP |
|---|---|---|---|
| `runtime` (Xbox 360) | The compiler's register save/restore routines (`__savegprlr_14` ... `__restfpr_31`), by their instructions. They have hundreds of callers and look alike, so no other pass may name them; the call-graph pass named six of them wrongly before. | – | 47 / 47 |
| `public` | Published addresses for this exact build (Plutonium's T4SP plugin, t4-rtx, T4M source, and others), verified against the bytes. | 552 / 46 (+ 131 / 60 globals) | – |
| `pc` | The PC function that is the only user of the same strings. | – | 895 / 793 |
| `selfname` | The function's own name in its messages: `"BG_ParseCommands: expected animation"`. | 54 / 64 | 5 / 5 |
| `cmd` | Console commands. On PC, `Cmd_AddCommand` is inlined: the handler and name are stored straight into the `cmd_function_s` that is linked to `cmd_functions`. On Xbox 360 it is a call. | 191 / 183 handlers | 100 / 85 |
| `dvar` | `g = Dvar_RegisterX("name", ...)`: the global becomes `dvar_<name>`. Only functions that reach the dvar system's core within a few calls count as registration functions: `Material_RegisterHandle` and a sound alias lookup are called the same way (a name string, the result stored in a global) and gave about 130 false `dvar_` names per build before this check. | 1,682 / 1,594 globals | 1,406 / 1,353 globals |
| `kisak` | KisakCOD (a reconstructed CoD4 source; WaW is built on that engine) uses the same distinctive strings in a function of that name. Names only. | 1,039 / 1,011 | 37 / 35 |
| `table` | `{ "name", handler }` arrays: script functions and methods (`GScr_`, `PlayerCmd_`, `ActorCmd_`, `HECmd_`, `ScriptEntCmd_`), menu commands and keywords, effect parsers; names from the KisakCOD array with the most of the same entries, then from its SP/MP twin, then from other arrays when a World at War table holds two of CoD4's; else that table's prefix. A table ends where the loop that reads it stops (World at War SP keeps the 20 hud element methods and the 114 player methods back to back, with no name repeated). On Xbox 360 a PC table with the same entries gives its names, so both builds name a builtin the same way. | 1,088 / 793 | 800 / 612 |
| `reader` | The function that looks a name up in a table, named after KisakCOD's function reading the same array (`Player_GetMethod` loops over the player methods), when it is the only function whose loop stops at the table's size. | 5 / 7 | (through `pc-graph`) |
| `sp` / `mp` | The other PC executable: functions with the same code (addresses masked out) or the only users of the same strings. | 420 / 639 | – |
| `sp-graph` / `mp-graph`, `pc-graph` | Names of the other build through matched functions (`match_builds.py`, below). The PC's x86 compiler helpers (`__SEH_epilog4`, `__allmul`) and ambiguous library matches give no names to Xbox 360 functions. | 100 / 347 | 351 / 326 |
| `kisak-graph` | Call graph against KisakCOD: the one unnamed callee of a named function matches the one leftover callee of its KisakCOD counterpart, only when the two agree on all their other callees (and the candidate's own callees agree too). Library code (unaligned functions on PC; import thunks and register routines on Xbox 360) is skipped. Weakest pass; a looser first version gave wrong names and was tightened. | 5 / 2 | 28 / 24 |
| `method` (last) | C++ methods that name themselves: Demonware's log lines start with the method (`bdCore::init`), and assertions carry the compiler's function signature (`bool __cdecl bdUnicastConnection::handleData(...)`). A name several functions share is an inlined helper's (`bdLinkedList::GetHead`) and names none. Named `Class__method` (`Class__ctor`, `Class__dtor`). | 275 / 260 | 99 / 104 |

Ghidra's own library signatures (Function ID) name part of the C runtime.

## Xbox 360 in Ghidra

`run_x360.sh` does all of this; the notes are for anyone using the pipeline on another Xbox 360 game.

- `xex2pe.py` turns the XEX into a flat image at its load address (0x82000000). The image is imported
  with Ghidra's Binary loader at address 0 and moved to its load address by `WawXenonSetup.java`:
  with this 64-bit language using 32-bit addresses, a base address given to the loader lands in the
  wrong place.
- Xbox 360 sections don't start on page boundaries, so the memory is split at every section start
  (an earlier, page-rounded split left `.pdata` unreadable: 128 functions found instead of 10,658).
- Every function in `.pdata` is created; import thunks are patched to `blr` in the analysis copy only
  and named from the kernel's export tables.
- The language includes `vadduws` (the unsigned saturating word add), which stock Ghidra's AltiVec
  file has commented out: it was the only instruction left undecoded in both World at War builds
  (37 places each). `WawFixBadInstructions.java` disassembles those places again in an existing project.

## Matching the builds (`match_builds.py`)

The Xbox 360 and PC builds are the same source compiled for different CPUs, so their code can't
be compared byte for byte; their strings, call graph and data tables can.

1. **Seeds:** functions with the same name in both builds, and functions that are the only users
   of the same string, the same named global (`dvar_cg_fov`), or the same combination of them.
2. **Call graph:** for every matched pair, the two ordered call lists are aligned on the callees
   already matched; a gap holding unmatched callees in the same pattern on both sides pairs them.
   An unmatched function also pairs with the one function on the other side that shares the most
   matched callers and callees with it (at least two, at least half of them, nothing else as close
   on either side).
3. **Pointer tables:** a matched function stored at one place in each build's data anchors the
   tables around it (script builtins, entity callbacks); the function pointers at the same offsets
   pair up, until the two layouts disagree: a pointer on one side only, a pair already matched to
   something else, or different strings next to it (two script tables whose entries differ).
   Steps 2 and 3 repeat until nothing new is found.
4. **Checks:** a pair is dropped when the sizes are far outside the usual ratio between the two
   builds, or when both functions use strings and share none.

`--holdout` hides half of the string seeds and compares the result with them:

| Builds | Hidden pairs found again | Of the found, the same pair |
|---|---|---|
| Xbox 360 SP → PC SP | 551 of 723 | 98.6% |
| Xbox 360 MP → PC MP | 444 of 606 | 97.4% |
| PC SP → PC MP | 936 of 1,028 | 99.9% |

A second test uses the script builtins: a handler with the same script name in both builds (and
only one) is the same function. With those handlers' names removed, the matcher pairs Xbox 360 SP
with PC SP for 513 of 557 of them, one wrongly (99.8%), and PC SP with MP for 370 of 467, three
wrongly (99.2%). Its typical mistake is pairing two functions that each exist in one build only
(an SP-only builtin with an MP-only one in the same place).

With all seeds: Xbox 360 SP pairs 5,484 functions with PC SP, Xbox 360 MP 4,565 with PC MP, and
PC SP 10,968 with PC MP. `crossref.py` joins the three tables: 13,425 functions, 3,689 of them
found in all four builds. In the Xbox 360 C, a paired function that has no name yet carries its PC
address as a note (`note: pc-graph (PC 005EEE20: the same place in the call graph (3))`), so the
PC function, which has recovered register parameters and types, can be read next to it.

## Script API (for the Rust script VM)

`script_api.py` lists every builtin the game's scripts can call, from the `{ "name", handler }` tables:
the table, the function that reads it (`BuiltIn_GetFunction`, `Player_GetMethod`, `CScr_GetFunction`...),
the script name, the handler's address and C name, and the developer-only flag. The singleplayer
executable has two script VMs with their own builtins: the server scripts (GSC) and the client scripts
(CSC); the table's reader tells them apart. `script_usage.py` then lists what a set of scripts calls,
most used first: Der Riese's zombies scripts (31 files) call 244 different builtins, which is the list
the Rust VM needs for that map; with `--crossref`, each handler's address in all four builds (181 of
the 244 are paired with the Xbox 360 SP build).

| Build | Table entries | Tables |
|---|---|---|
| PC SP | 1,380 | 17 (GSC functions and methods; player, actor, hud element, script entity and vehicle methods; CSC functions and methods; menu, effect and spawn keyword parsers) |
| PC MP | 1,122 | 16 |

## Types and parameters

- **Register parameters:** World at War PC was built with whole-program optimization, so many
  functions take arguments in registers. Ghidra's "Decompiler Parameter ID" analysis
  (`WawEnableParamId.java`) recovers them.
- **Types (private output only):** the T4SP server plugin's structure headers (AGPL-3.0, with offsets
  checked for this build) are turned into one C header by `t4sp_types.py` and parsed into Ghidra by
  `WawApplyTypes.java`: every `dvar_*` global becomes a `dvar_s *`, and the plugin's typed globals and
  prototypes are applied. The C then reads `dvar_cg_debugInfoCornerOffset->current.value`. Nothing from
  those headers goes into a published file.

## Output (private, `dev/waw-decomp/<build>/`)

- `decomp/<address>.c`: the C of every function, one file per 64 KB of code, in address order.
- `names.tsv`: address, name, kind (`function`, `global`, or `note` for a comment only), pass, evidence.
- `pc_match.tsv` (Xbox 360): every function paired with a PC function, and how;
  `other_match.tsv` (PC): the same between SP and MP.
- `crossref.tsv` (top of the output folder): every function's name and address in all four builds;
  `other_names` lists the builds that name it differently (mostly script builtins that KisakCOD
  doesn't have, named from each table's prefix).
- `cod3/` and `cod2_360/` (see below): Call of Duty 3 paired with World at War and Call of Duty 2,
  and Call of Duty 2's Xbox 360 builds.
- `functions.tsv`, `calls.tsv`, `strings.tsv`, `globals.tsv`: the tables the passes use.

## Call of Duty 3, and Call of Duty 2 on Xbox 360

World at War is decompiled to learn how to do CoD3 well, so the same tools were run on CoD3's Xbox
360 executables and on its other relative, Call of Duty 2's Xbox 360 executables (CoD2's
`default.xex` 12,915 functions and `default_mp.xex` 12,809, every instruction decoded; CoD3's
`default.xex` 13,550 and `codmp_xenonf.xex` 17,047, one undecodable word).

- **CoD2 Xbox 360 named from CoD2 PC.** The Call of Duty 2 / United Offensive decomp named CoD2's
  PC multiplayer program from CoD2x's symbol table and the Mac build's symbols (2,462 names);
  `name_pc.py --other` carries them to the PC singleplayer program (1,951 named, 3,743 pairs), and
  `name_x360.py` to the Xbox 360 builds: 1,354 named in SP (2,974 paired with PC, the hidden-seed
  test finding 223 of 310 again, all right) and 1,305 in MP (2,404 paired).
- **CoD3 against both relatives.** CoD3 shares 1,213 strings with CoD2's Xbox 360 build and 958
  with World at War's (CoD2 and World at War share 3,541: they are the Infinity Ward line, CoD3 is
  Treyarch's). CoD3 SP pairs 1,175 functions with World at War SP and 1,027 with CoD2 SP; CoD3 MP
  1,653 with World at War MP and 1,370 with CoD2 MP. Where a CoD3 function is paired with both
  relatives and those two are paired with each other, the three pairs agree 423 times out of 454
  in SP and 415 of 432 in MP (93-96%; a disagreement means at least one of the three is wrong).
- **CoD3's two programs.** CoD3 MP pairs 8,304 functions with CoD3 SP (98.1% in the hidden-seed
  test); about 1,100 of them are named in the project's CoD3 symbol map, so the multiplayer
  program, whose names the CoD3 decomp needs before porting its network code, gets 914 new names
  from the campaign (the World at War passes had already named most of the others).
  `carry_addresses.py` then carries the campaign game code's addresses (hooks, globals, places
  inside functions) over to the multiplayer program for the PC multiplayer port: 292 of 390 found,
  and 12,551 globals in all (99.5% come back to the same address when run the other way).

`crossref.py` joins all ten builds of the three games (27,767 functions; 172 rows where the pairs
disagree are flagged). From it, `dev/waw-decomp/cod3/cod3_relatives.tsv` gives each CoD3 function
its relatives' addresses in all eight other builds, and a candidate name for the 221 that the
project's symbol map doesn't name yet, with which sources agree (82 with two or three: CoD2,
World at War, KisakCOD's strings). The project's own CoD3 names stay in its symbol map.

What it shows: CoD3's AI shares code with CoD2's and CoD4's (`Actor_Grenade_Think`,
`Actor_Alloc`, `ActorCmd_PickUpGrenade`), CoD3's "Broc" script builtins pair with World at War's
(`Broc_ShellShock` with `GScr_ShellShock`), and the Quake III layer is shared (`Cvar_Set2` with
`Dvar_SetVariant`, `Netchan_Process`, `NET_AdrToString`, `MSG_WriteString`).

Network facts found along the way: World at War (PC and Xbox 360, SP and MP) builds its message
Huffman coder from CoD4's frequency table (the one KisakCOD has), not Quake III's; CoD2's Xbox 360
MP has Quake III's (as CoD2 PC does); CoD3 has neither (its `msg_t` writes bytes with no Huffman
coding; see [MP_PROTOCOL.md](MP_PROTOCOL.md)).

Floating point: World at War PC never changes the x87 precision itself (the C runtime's 53-bit
default stays), but creates its Direct3D 9 device without `D3DCREATE_FPU_PRESERVE` (flags 0x44;
KisakCOD's CoD4 passes 0x46, which includes it), so Direct3D puts the thread that creates the
device at 24-bit. A bit-exact port of World at War's float code has to know which thread it ran
on.

Server lists: World at War 1.7 speaks protocol 101 (MP) and 62 (SP co-op). It keeps CoD4's master
settings (`masterServerName` and `authServerName` default to `cod4master.activision.com`,
`masterPort` to 20810) and the client still reads `getserversResponse`, but neither executable
sends master heartbeats (no `heartbeat %s`, `getservers %s` or `flatline`, which CoD1-CoD2 have):
internet games are found through Demonware's matchmaking (`CL_MP_FindSessions`), LAN games with
`localservers`. So the community master servers of the older games (cod.pm's, see
[DECOMP_SHARED_NOTES.md](DECOMP_SHARED_NOTES.md)) don't list World at War, and a Rust World at War
server reaches a stock client over LAN or Demonware's protocol.

## Next

1. More types: type the big globals and function parameters (`gentity_s *`, `playerState_s *`) from
   the plugin's headers, the MP-only structures, and carry checked layouts to the Xbox 360 builds.
2. United Offensive: the same pairing against CoD2 PC and CoD 1.5 could name more of its
   multiplayer program (the weakest-named build of the Call of Duty 2 / UO decomp), if that chat
   wants it.
3. Per-system write-ups (weapons, zombies rounds and perks, the script VM) for the Rust port.
