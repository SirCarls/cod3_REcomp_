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
| PC 1.7, singleplayer + zombies | `CoDWaW.exe` (timestamp 2009-10-29, image base 0x400000) | 14,980 functions found, 14,836 decompiled (the rest are import thunks); 4,111 named + 2,258 named globals; register parameters recovered; types (`dvar_s *` globals, 138 prototypes, 271 typed globals: the plugin's, 26 game globals such as `g_entities`, `level`, `cgArray`, `cls`, `sv`, and the zone loader's vars); a note on every function with zombie-mode code. |
| PC 1.7, multiplayer | `CoDWaWmp.exe` | 14,994 functions, 14,851 decompiled; 3,723 named + 1,887 named globals; 10,974 functions paired with SP. |
| Xbox 360, singleplayer + zombies | `default.xex` | 13,708 functions (10,658 from `.pdata`), 13,699 decompiled (with the script handlers the table pass found), every instruction decoded (VMX128 included); 2,465 named + 1,593 named globals (1,522 typed `dvar_s *`); 5,474 functions paired with their PC function (paired functions without a name carry the PC address as a note). |
| Xbox 360, multiplayer | `default_mp.xex` | 13,003 functions (10,137 from `.pdata`), 12,985 decompiled, every instruction decoded; 2,133 named + 1,516 named globals (1,480 typed `dvar_s *`); 4,569 paired with PC. |

The Xbox 360 executables also hold the console's own libraries (Direct3D, XAudio, the C runtime,
Bink), which have no PC counterpart, so a good part of their functions will never pair with PC.

## Tools (`tools/waw_decomp/`)

| Tool | What it does |
|---|---|
| `xiso_get.py` | Copies only the program files (`*.xex`, `*.dll`) out of an Xbox 360 disc image (read-only). |
| `ps3iso_get.py` | Lists or copies files from a plain PlayStation 3 disc image (ISO 9660; read-only, decrypts nothing): the program files (`EBOOT.BIN`, `*.self`, `*.sprx`), `PARAM.SFO` (title, title ID, versions) and `PS3_DISC.SFB`, and says what each program file is (a PS3 program is a SELF, signed and encrypted for the console, so it is only reported). |
| `xex2pe.py` | XEX2 → plain PE image: retail AES decryption, basic and LZX compression, import list (`--imports`, named from the ReXGlue SDK's kernel export tables). |
| `xexlzx/xexlzx.c` | Small LZX helper for `xex2pe.py`, built on libmspack's decoder. |
| `ghidra_xenon/` | Adds the Xbox 360 CPU to Ghidra: language `PowerPC:BE:64:Xenon-32addr` with the 75 VMX128 vector instructions (128 vector registers) and Xenon's `lvlx`/`lvrx`/`stvlx`/`stvrx`. Stock Ghidra stops at the first VMX128 instruction. `install_xenon.py <ghidra>` installs it. |
| `ghidra_scripts/` | Headless Ghidra scripts: `WawFindFunctions` (functions reached only through pointer tables), `WawXenonSetup` (Xbox 360: places the image, sections, `.pdata` functions, kernel import names), `WawEnableParamId`, `WawApplyNames` (names and notes), `WawApplyTypes` (a C header and any number of typed-symbol tables), `WawExport` (tables + the C of every function), `WawStats` (functions, undecodable words, VMX128 count), `WawFixBadInstructions` (disassembles again where the language learned an instruction). |
| `run_pc.sh`, `run_x360.sh` | The whole pipeline for one executable: import, analysis, naming, decompile. |
| `name_pc.py`, `name_x360.py`, `naming_common.py` | The naming passes (below). |
| `match_builds.py` | Pairs the functions of two builds (Xbox 360 and PC, or SP and MP) by shared strings, named globals, the call graph and tables of function pointers; `--holdout` measures how often it agrees with pairs it was not shown. |
| `crossref.py` | One table of every function across the four builds: its name and its address in each. |
| `method_names.py` | Runs the `method` pass alone on any export (used for CoD3, whose names come from its symbol map). |
| `carry_addresses.py` | Finds known addresses of one Xbox 360 build in another (CoD3 campaign -> multiplayer): functions through the pairs, places inside functions by lining up the instructions, globals by the addresses the aligned instructions form, strings and RTTI vtables. |
| `script_usage.py` | Which builtins a set of scripts calls; with `--crossref`, each handler's address in all four builds; `--column NAME=FOLDER` adds a column of calls per folder (one per map). |
| `script_signatures.py` | What each builtin takes and returns, read from its handler's C: arguments by position and type (`Scr_GetFloat(1)`, `Scr_GetVector(&v, 2)`), the argument counts it tests, what it returns (`Scr_AddInt`...), its own usage and error messages, the other names that share its handler, and KisakCOD's handler of the same name if there is one. |
| `field_tables.py` | The script field tables (`self.origin`, `ai.goalradius`, `hudelem.alpha`): finds the `{ "name", offset, type, ..., setter, getter }` arrays in a PC or Xbox 360 build by their field names, decodes every entry, pairs each table with the KisakCOD array that shares the most field names (read from KisakCOD's source text, entries spanning lines included), and names a setter or getter that sits in the slot where KisakCOD's entry for the same field names one. Writes an `--extra` table for the naming tools and, with `--tables`, every decoded entry. |
| `asset_fields.py` | The text keys of the asset files (weapons, vehicles, flame tables, physics presets and constraints, destructibles, vision sets): finds every `{ "key", offset, type }` table (Call of Duty 4's `cspField_t`) in a PC or Xbox 360 build, says what each type means, and, with the build's export, the names each weapon enumeration accepts (`weaponType`, `fireType`, ...) read from the weapon parser's own switch. |
| `zone_loader.py` | Reads the game's own fastfile loader (PC 1.7 SP) statically: names its `Load_`/`Mark_` functions and types its var globals (for `name_pc.py --extra` and `WawApplyTypes.java`), and writes each structure's load program (private). |
| `t4sp_types.py` | The T4SP plugin's headers → one C header + typed symbols, for the private decompile; `--check` compiles the header with clang under MSVC's 32-bit layout and checks every documented offset and size, as written and as Ghidra reads it. |
| `struct_globals.py` | Where a structure lives: each address the code references votes for "base = address − field offset" over the structure's fields (`vote`); `check` makes sure a table of typed globals doesn't overlap and agrees with the names. |
| `script_api.py` | The script API: every `{ "name", handler }` table entry, its table and the function that reads it. |
| `kisak_strings.py` | Lists each KisakCOD function's strings and callees, its `{ "name", handler }` tables (with the array each entry is in, including the 26 entries written with a cast, `(void(*)())Scr_PrecacheModel`) and the functions that read each array, for the naming passes. |
| `public_symbols.tsv` | 812 names with addresses for the PC 1.7 executables from public projects, each checked against the executable's bytes; 793 are used. t4-rtx's `cgs` (0x034732B8) is not: it declares it a `cg_s *`, which KisakCOD calls `cgArray` (the `cgs_t`, `cgsArray`, is at 0x03466578), so the decompile names it `cgArray`. T4M's six `*_GetMethod` addresses are not: each reads another class's table than its label says (0x4DEEA0, labelled `Player_GetMethod`, reads the 107 actor methods), so the `reader` pass names them instead. |

## How functions get their names

Each name records the pass that gave it and its evidence; in the decompiled C it is the
comment above the function (`name: kisak (12 string(s) shared with KisakCOD UI_SetActiveMenu)`).
Passes run strongest first; an address or a name is taken by the first pass that claims it.

| Pass | Evidence | PC SP / MP | Xbox 360 SP / MP |
|---|---|---|---|
| `runtime` (Xbox 360) | The compiler's register save/restore routines (`__savegprlr_14` ... `__restfpr_31`), by their instructions. They have hundreds of callers and look alike, so no other pass may name them; the call-graph pass named six of them wrongly before. | – | 47 / 47 |
| `public` | Published addresses for this exact build (Plutonium's T4SP plugin, t4-rtx, T4M source, and others), verified against the bytes. | 552 / 46 (+ 130 / 60 globals) | – |
| `extra` | Private tables of names found by hand with their evidence (`name_pc.py` and `name_x360.py --extra`). PC SP: the typed game globals (see *Types*) the four functions that set them up (`G_InitGame`: `level.clients = g_clients` ... in KisakCOD's order; `G_InitSentients`, `G_InitActors`, `G_InitScrVehicles`), `Com_DPrintf` (prints only while `com_developer` is set, then calls `Com_Printf`), `strstr`, the server's and client's script loaders (both have KisakCOD's `GScr_LoadScriptsAndAnims` strings; the client one loads the client callbacks and ends the client VM's load), the functions that run the player damage, killed and last stand callbacks, and two names the `kisak` pass got wrong from a single shared string: `G_RunFrame` (it had been `DebugDumpAnims`, whose dump is inlined at its end) and `WriteSaveToDevice` (it had been `CG_DispatchServerCommand`; found by checking every `kisak` name's named callees against its KisakCOD function's calls; the same check found MP's `ClearArray`, named `Scr_GetValueString`), and `Sentient_NearestNode`; the zombies table (`zombies` pass): `DB_BuildOSPath` and a note on each function that reads `zombiemode`, plus the network wait (see *Zombies and custom maps*). PC MP and Xbox 360: names checked against Black Ops (x360-family's list of functions with the same code in World at War and Black Ops, where the names differed), each kept only with its own evidence in the strings (`GPad_InitAll` registers the `gpad_*` dvars). PC MP also gets `G_RunFrame`, its player damage and killed callback runners, and notes on the World at War-only actor and vehicle callback runners. Since 2026-10-20 also the eight corrections the Black Ops cross-check found (`types/<build>_hand.tsv`, below). A row of kind `note` is a comment only and stays next to whatever name the function gets; a row of kind `inlined` keeps a KisakCOD name off every function (`BG_GetWeaponDef`: World at War reads the weapon table directly everywhere, and the call graph pass had given the name to two neighbors). | 15 (+ 15 globals, 39 notes) / 5 (+ 4 notes) | 6 / 5 |
| `fields` | `field_tables.py` on each build: the setters and getters of the script field tables (entity, sentient, actor, player, hud element, path node, vehicle node and a few smaller ones; 10 to 12 tables per build), named from the same slot of KisakCOD's table (`ActorScr_SetGoalRadius`, `ClientScr_SetScore`, `HudElem_SetAlpha`); KisakCOD's multiplayer tables are preferred for the MP builds. | 64 / 68 | 43 / 47 |
| `pmove` (PC SP) | The player movement functions (`Pmove`, `PmoveSingle`, `PM_GroundTrace`, `PM_StepSlideMove`, `Jump_Check`, `Mantle_Start`...), matched to KisakCOD's `bgame` functions by call graph, the dvars they read and their constants (private `zone/pmove.md`); two World at War-only ones are named after what they do (`PM_WaterMove`, `PM_GetWaterLevel`). | 66 / – (65 reach MP through `sp`) | – (40 / 36 through `pc-graph`) |
| `weapons` (PC SP, since 2026-10-20) | The weapon code: the functions of CoD4's `bg_weapons`, `cg_weapons`, `g_weapon`, `g_items`, `g_missile` and `bullet` files, matched to KisakCOD's by what each reads from the weapon definition (`asset_fields.py`'s table turns the offsets into key names such as `fireTime`, so the members can be compared across the two games' different layouts), its strings, dvars, callers and callees, and each checked by reading both. World at War's additions that Black Ops kept take KisakBlack's names (`Sprint_State_Raise`, `Deploy_State_Deploy`, `BG_SetupWeaponMountedVersions`, `BG_GetClipSize`); one has a descriptive name (`BG_SetupOverheatIndexes`). Private table `types/weapons_pc_sp.tsv`, one line of evidence per name. | 269 / 40 (218 reach MP through `sp`; since names v44 also the turrets) | (172 / 130 through `pc-graph`) |
| `actors` (PC SP, since 2026-10-20) | The AI code that zombies and hellhounds run: the functions of CoD4's `actor*.cpp`, `pathnode.cpp` and `sentient.cpp` files, matched to KisakCOD's by their callees, constants, strings and `actor_s` offsets, each checked by reading both, and checked against the AI state tables (`AIFuncTable` at 0x008DC3C8: the human and dog tables fill the same slots as KisakCOD's `actor_function_table.cpp`; the linker folded identical functions, so some slots share one). KisakCOD's template instances get C-safe names with the template argument written in (`Path_AStarAlgorithm_FindPath`, one of nine A* copies). It also corrected two earlier names: 0x0055C210 is `Path_NearestNodeNotCrossPlanes` (was `Sentient_GetOrigin` from `kisak-graph`; both `Sentient_GetOrigin` and `Path_NearestNode` are inlined and reserved), and `G_ParsePathnodeScriptField` moved to 0x005593D0 (0x00671D50 is Black Ops' client-side `CG_SpawnNode`, which shares one string with it). Private table `types/actors_pc_sp.tsv`, one line of evidence per name. | 573 / – (341 reach MP through `sp` and `sp-graph`; since names v46 also the path node loading and linking code) | (358 / 209 through `pc-graph`) |
| `xanim` (PC SP, since 2026-10-20) | The animation and model code (CoD4's `xanim*.cpp`, `dobj*.cpp`, `xmodel*.cpp` and the DObj pool in `dobj_management.cpp`), matched to KisakCOD's and KisakBlack's functions by their callees, strings and structure offsets and each checked by reading both. Zombies and hellhounds move by their animations (root motion: `Actor_GetAnimDeltas` -> `XAnimCalcDelta` -> `XAnimCalcRelDeltaParts`), so this is the code a port needs to reproduce their movement. Template instances get C-safe names (`XAnimCalcParts_uchar`). It also corrected three names: 0x00401010 is the C library's `floorf` (it had been `AngleNormalize360`, which World at War inlines around it), 0x0060FA10 is `DObjUpdateServerInfo` and 0x006137C0 is `XAnimCalc`. | 207 / – (184 reach MP through `sp`) | (70 / 83 through `pc-graph`) |
| `cgame` (PC SP, since 2026-10-20) | The client game code of the campaign program, which is also the co-op and zombies client (HUD, compass, scoreboard, hud elements, view, effects, server commands), matched to KisakCOD's single player and multiplayer `cgame` functions and KisakBlack's; anchored by the HUD's ownerdraw switch (`CG_OwnerDraw`), the server command letters and the entity type switch, and each checked by reading both. It also corrected three names that had come from one or a few shared strings (`CG_DrawClientScore`, `CG_DrawScoreboard_ListBanner`, `CG_CornerDebugPrintCaption`). | 490 / – (353 reach MP through `sp`) | (291 / 211 through `pc-graph`) |
| `game` (PC SP, since 2026-10-20) | The server game code that the earlier passes had not reached (CoD4's `g_active`, `g_client`, `g_cmds`, `g_combat`, `g_hudelem`, `g_items`, `g_main`, `g_missile`, `g_mover`, `g_save`, `g_scr_main`, `g_scr_vehicle`, `g_spawn`, `g_trigger`, `g_utils` ...), plus Black Ops' names for World at War code Black Ops kept (vehicles, destructibles), each checked by reading both bodies. The campaign program is also the co-op and zombies server, so this covers damage (`G_Damage`), last stand and revive, and spawning. World at War-only functions get a descriptive note instead of a name. It also corrected 0x00412600 to `VEH_ParseSpecificField` (it had a Black Ops pair's name that came from one string shared with the Tracy profiler in the KisakBlack tree) and 0x00559880 to `G_SpawnPathnodeDynamic`. | 473 / – (266 reach MP through `sp`) | (306 / 184 through `pc-graph`) |
| `net` (PC SP, since 2026-10-20) | The engine's server and client code: CoD4's `server/` and `client/` files (connections, snapshots, client input, server commands, saves and restarts), the message and net channel code (`msg`, `net_chan`) and the start of `g_client`, matched to KisakCOD's SP and MP files and to Black Ops' names, each checked by reading both bodies. The campaign program is the co-op server and client, so this covers how co-op players join and what each frame sends; World at War's lobby (party) code has no source names and gets descriptive notes, a few functions are named from their own log text. It also corrected `SV_MapRestart` (was `SV_MapRestart_f`), `SV_FindClientByAddress` (was `SV_ReadPackets`, in both PC programs), `SV_IsInternalSave` (was `init`, from a string KisakCOD's bundled physics library shares) and `Netchan_TransmitFragment`. | 320 / – (231 reach MP through `sp`) | (152 / 118 through `pc-graph`) |
| `bg` (PC SP, since 2026-10-20) | The code the server and the client share (CoD4's `bgame/` files: player animation scripts, movement helpers, bg_misc, perks), aim assist, the menu file preprocessor and the rest of the client game code in 0x00400000-0x00470000, matched to KisakCOD and to Black Ops' names (fire, wind, vehicle animation), each checked by reading both bodies. It also corrected `SetConfigString` (was `BG_StringCopy`) and `MemFile_ReadData` (was `MemFile_ReadByteInternal`). | 228 / – (202 reach MP through `sp`) | (95 / 75 through `pc-graph`) |
| `cm` (PC SP, since 2026-10-20) | The collision code and the common code around it: CoD4's `cm_*.cpp` files (traces, sight traces, brush and mesh tests, the collision map and BSP loaders, world sectors), `cmd.cpp`, `common.cpp`, the player profile and stats code and the dvar commands, matched to KisakCOD's bodies and, where World at War already has Black Ops' version (the trace with a primitive list, water and burnable cells, rope constraints), to KisakBlack's, each checked by reading both bodies. It also corrected `CM_AreaEntities_r` (was `CM_AreaEntities`, which every caller inlines) and `ResetCreateAClassNames` (was `LiveStorage_StatsInit`, which is its caller). | 226 / – (210 reach MP through `sp`) | (41 / 38 through `pc-graph`) |
| `db` (PC SP, since 2026-10-20) | The console, key and game pad bindings, the screen and the fastfile database: CoD4's `cl_console`, `cl_keys`, `cl_scrn`, `con_channels`, `screen_placement` and `database/` files (zone lookup and loading, the XFile reader, the asset registry, pools and handler tables, unloading) and Black Ops' `cl_gamepad`, each checked by reading both bodies. It also corrected 29 of the `zone` pass's loader names (`zone_loader.py` had shifted the XAnim trans/quat chain one level, and several `Mark_` names sat on functions that load), `DB_PostLoadXZone` (was `DB_SyncXAssets`, a public label; the real `DB_SyncXAssets` calls it), `I_strnicmp` (was `I_stricmp`, a public label: the PC builds inline `I_stricmp` and pass the count in a register; 58 call sites pass real counts), `SCR_UpdateScreen` and `Con_DrawOutputVersion`. | 211 / – (184 reach MP through `sp`) | (53 / 45 through `pc-graph`) |
| `fx` (PC SP, since 2026-10-20) | Dynamic entities and the effects system: CoD4's `DynEntity/` and `EffectsCore/` files (effect loading and conversion, spawning, looping, bolted effects, drawing, marks, beams, trails, sorting) with Black Ops' additions that World at War already has (unique effect handles, effects bolted to dynamic entities, burning dynamic entities), each checked by reading both bodies. World at War's own effect priority list (it evicts low-priority effects when the pools are full; Call of Duty 4 drops the new effect) gets descriptive notes. | 303 / – (275 reach MP through `sp`) | (103 / 92 through `pc-graph`) |
| `fs` (PC SP, since 2026-10-20) | The file system's search paths and iwd files (CoD4's `files.cpp`: game directories, iwd order, pure checks, mod list), dvar info strings, huffman, thread setup, ragdoll, the server's game interface (`sv_game.cpp`, `sv_world.cpp`) and localized strings, each checked by reading both bodies. It also corrected `FS_GetMapBaseName` (was `UI_GetGameTypesList_FastFile` from the `bo1` pass, on PC and Xbox 360), `SEH_DecodeLetter`, `strchr` (was `Info_Validate`) and `FS_FOpenFileReadForThread` (was `FS_FOpenFileByMode`, a public label; now `verified=no`). | 152 / – | (through `pc-graph`) |
| `core` (PC SP, since 2026-10-20) | The rest of the common code in 0x005D0000-0x005E8000: file handles and reads, vector math, hunk and temp memory, sound alias loading and picking (`com_sndalias.cpp`), string tables, the dvar system and the end of the UI code. It also corrected `BoxOnPlaneSide` (was `R_FilterDynEntIntoCells`), `FS_FOpenTextFileWrite`, `UI_GetGameTypeName` (was `CG_TeamColor`) and found an out-of-line `AngleNormalize360` (the version 44 reservation as inlined was lifted). | 206 / – | (through `pc-graph`) |
| `ui` (PC SP, since 2026-10-20) | The menu system: CoD4's `ui_expressions.cpp` (69 operator functions from the operator table), `ui_localvars.cpp`, `ui_main.cpp`, `ui_shared.cpp` and `ui_shared_obj.cpp`, with Black Ops' names for the playlist and mod list code, each checked by reading both bodies. It also corrected `Menu_Setup` (was `UI_Init`), `Item_Text_Paint`, `Menu_PaintAll`, `I_strcmp`, and six World at War-only script commands the `table` pass had named `ItemParse_*` (it ran past the end of the item keyword table into the script command table; now `Script_*` by hand in all four builds). | 298 / – | (through `pc-graph`) |
| `zone` (PC SP) | `zone_loader.py`: the fastfile loader's functions, followed from the asset type switch, named after the structure each one loads (`Load_GfxWorld`, `Mark_XModelPtr`; KisakCOD's name where it has one), and its var globals with their types. | 258 (+ 172 globals) / – (195 reach MP through `sp`) | – |
| `pc` | The PC function that is the only user of the same strings. A name several functions want goes to the one with the most of those strings (a helper inlined in one build shares a string or two with its caller: `bdConnectionStore::flushAll` on PC has `bdLinkedList::getHead` inside). | – | 897 / 794 |
| `selfname` | The function's own name in its messages: `"BG_ParseCommands: expected animation"`. | 54 / 64 | 6 / 6 |
| `cmd` | Console commands. On PC, `Cmd_AddCommand` is inlined: the handler and name are stored straight into the `cmd_function_s` that is linked to `cmd_functions`. On Xbox 360 it is a call. | 191 / 183 handlers | 100 / 85 |
| `dvar` | `g = Dvar_RegisterX("name", ...)`: the global becomes `dvar_<name>`. Only functions that reach the dvar system's core within a few calls count as registration functions: `Material_RegisterHandle` and a sound alias lookup are called the same way (a name string, the result stored in a global) and gave about 130 false `dvar_` names per build before this check. | 1,682 / 1,594 globals | 1,406 / 1,353 globals |
| `kisak` | KisakCOD (a reconstructed CoD4 source; WaW is built on that engine) uses the same distinctive strings in a function of that name. Names only. | 1,039 / 1,011 | 40 / 37 |
| `table` | `{ "name", handler }` arrays: script functions and methods (`GScr_`, `PlayerCmd_`, `ActorCmd_`, `HECmd_`, `ScriptEntCmd_`), menu commands and keywords, effect parsers; names from the KisakCOD array with the most of the same entries, then from its SP/MP twin, then from other arrays when a World at War table holds two of CoD4's; else that table's prefix. A table ends where the loop that reads it stops (World at War SP keeps the 20 hud element methods and the 114 player methods back to back, with no name repeated). On Xbox 360 a PC table with the same entries gives its names, so both builds name a builtin the same way. | 1,092 / 793 | 800 / 611 |
| `reader` | The function that looks a name up in a table, named after KisakCOD's function reading the same array (`Player_GetMethod` loops over the player methods), when it is the only function whose loop stops at the table's size. | 6 / 7 | (through `pc-graph`) |
| `sp` / `mp` | The other PC executable: functions with the same code (addresses masked out) or the only users of the same strings. Only the names that executable found itself: a name it took from this one would come back with no new evidence, and a wrong name kept itself alive that way from version to version (`Com_DPrintf` was `CM_AreaEntities_r` in both); version 16 dropped 28 such names in SP and 26 in MP. | 378 / 818 | – |
| `sp-graph` / `mp-graph`, `pc-graph` | Names of the other build through matched functions (`match_builds.py`, below). The PC's x86 compiler helpers (`__SEH_epilog4`, `__allmul`) and ambiguous library matches give no names to Xbox 360 functions. | 89 / 400 | 355 / 336 |
| `bo1` (since 2026-10-20) | Names Black Ops' Xbox 360 programs have (named by the chat "x360-family" from KisakBlack, a reconstruction of the Black Ops multiplayer program) for World at War functions that had no name, through the blind pairing of the two games (*Cross-check with Black Ops*, below); kept only when strings or data back the pair and strings back the Black Ops name (`BG_Wind_RegisterDvars`, `R_InitWaterSimulation`, `Phys_EffectsInit`, `Playlist_ParsePlaylists`, `Dog_RegisterDvars`). Private tables `types/bo1_<build>.tsv`, read with `--extra` after the hand tables. Version 48 dropped one pair (PC SP 0x005A2690 / Xbox 360 0x8223FFA0 is `FS_GetMapBaseName`, not Black Ops' `UI_GetGameTypesList_FastFile`). | 36 / 43 | 42 / 58 |
| `carry` (Xbox 360, since 2026-10-20) | Graph names an earlier version had that a new run dropped only because the matcher's low-vote pairs moved (new PC names change which pairs win in clusters such as the script compiler): carried when the address is still unnamed, the name unused, the earlier PC partner still has that name, and the two functions still share a named callee or caller. Private tables `types/x360_<build>_carry.tsv`. | – | 15 / 21 |
| `kisak-graph` | Call graph against KisakCOD: the one unnamed callee of a named function matches the one leftover callee of its KisakCOD counterpart, only when the two agree on all their other callees (and the candidate's own callees agree too). Library code (unaligned functions on PC; import thunks and register routines on Xbox 360) is skipped. Weakest pass; a looser first version gave wrong names and was tightened. The C library math helpers (`sqrtf`, `fabsf`, 15-byte functions on PC) count as neither named nor open: KisakCOD writes them as `sqrt`, `I_fabs` or `Vec3Length`, or inlines them. Until version 40 `sqrtf` was named `Jump_GetLandFactor` by this pass (World at War inlines that one). Where KisakCOD calls `I_stricmp`, a PC caller's `I_strnicmp` counts as the same callee (the PC builds inline the wrapper). | 53 / 8 | 30 / 24 |
| `method` | C++ methods that name themselves: Demonware's log lines start with the method (`bdCore::init`), and assertions carry the compiler's function signature (`bool __cdecl bdUnicastConnection::handleData(...)`). A name only one function has is taken early (after `selfname` on PC, before `pc` on Xbox 360: the 360's `bdKeyArchive::handleTask` has the message PC's `bdStats::handleTask` has); a name two functions share comes last (`name`, `name_2`); a name more share is an inlined helper's (`bdLinkedList::GetHead`) and names none. Named `Class__method` (`Class__ctor`, `Class__dtor`). | 265 / 260 | 97 / 101 |

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
   A wrapper one build inlines pairs with the inner function on the other side: the PC builds call
   `I_strnicmp` where the Xbox 360 builds call their `I_stricmp` wrapper, so the PC's `I_strnicmp`
   seeds with the Xbox 360 `I_stricmp`, which is what the same callers call there.
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

With all seeds: Xbox 360 SP pairs 5,474 functions with PC SP, Xbox 360 MP 4,569 with PC MP, and
PC SP 10,968 with PC MP. `crossref.py` joins the three tables: 13,527 functions, 3,693 of them
found in all four builds. In the Xbox 360 C, a paired function that has no name yet carries its PC
address as a note (`note: pc-graph (PC 005EEE20: the same place in the call graph (3))`), so the
PC function, which has recovered register parameters and types, can be read next to it.

## Script API (for the Rust script VM)

`script_api.py` lists every builtin the game's scripts can call, from the `{ "name", handler }` tables:
the table, the function that reads it (`BuiltIn_GetFunction`, `Player_GetMethod`, `CScr_GetFunction`...),
the script name, the handler's address and C name, and the developer-only flag. The singleplayer
executable has two script VMs with their own builtins: the server scripts (GSC) and the client scripts
(CSC); the table's reader tells them apart. `script_usage.py` then lists what a set of scripts calls,
most used first, with `--crossref` each handler's address in all four builds, and with
`--column NAME=FOLDER` a column of calls per folder (one per map). The zombies counts are in
[Zombies and custom maps](#zombies-and-custom-maps).

| Build | Table entries | Tables |
|---|---|---|
| PC SP | 1,385 | 18 (GSC functions and methods; player, actor, sentient, hud element, script entity and vehicle methods; CSC functions and methods; menu, effect and spawn keyword parsers) |
| PC MP | 1,122 | 16 |

The table pass needs six entries in a row, except for `{ "name", handler, developer flag }` arrays,
whose 0/1 flags are evidence enough from three: that finds the five sentient methods
(`setthreatbiasgroup`, `getthreatbiasgroup`, `getclosestenemysqdist`, `getenemysqdist`, `isnotarget`),
which every zombies map calls. `waittillmatch`, `prof_begin`, `prof_end` and `#using_animtree` are
statements and a directive of the script compiler, not builtins.
Two PC tables have no KisakCOD array that names them, so the pass gave their handlers the
script prefix `Scr_`. Since names v46 hand rows name them instead: the menu file preprocessor's
directives (`PC_Directive_ifdef` ...; KisakCOD writes that array with casts the reader missed) and
World at War's lobby (party) out-of-band commands, which have no source names and are labeled
`PartyCmd_<command>` after their table key.

The script language and bytecode are Call of Duty 4's: World at War's VM has the same 138 opcodes in
the same order (`OP_End` ... `OP_manualAndAssignmentBreakpoint`; the VM's switch handles 135 of
them, the three breakpoint opcodes are the debugger's) and its parser the same node kinds plus two
(`ENUM_duplicate_variable`, `ENUM_duplicate_expression`: a variable or expression the parse tree
uses twice, compiled while its constant strings' reference counts are kept). Lists compared:
KisakCOD's `scr_vm.h` and the T4SP plugin's headers. What World at War adds is the second VM
instance (the client scripts, with their own variables, strings and builtins), its own builtins and
its limits (see *Zombies and custom maps*). The T4SP plugin's `src/codsrc/clientscript/` is a source
reconstruction of World at War's script compiler and VM (about 20,000 lines); it is AGPL-3.0, so for
the Rust engine it is a reference for behavior to check against, not code to copy unless the engine
takes that license.

**How the engine runs the scripts** (read from the C and checked in places against the bytes; the
private `zone/script_scheduling.md`, `engine_callbacks.tsv` and `engine_notifies.tsv` have the
addresses):
- Map start, server: `G_InitGame` compiles in one pass `codescripts/delete`, `codescripts/struct`,
  the ten `CodeCallback_*` labels of `maps/_callbacksetup` (all required), the animscripts,
  `maps/<map>::main` and the AI type, dog, traverse and turret scripts (at most 128 labels the
  engine keeps). The map's entities spawn first; then the VM starts at time 0, the script structs
  are built, paths and spawners are set up, and `main` runs on `level` until its first wait. Three
  settle frames of 100 ms follow, then `CodeCallback_PlayerConnect` for clients already connected; a
  player gets `begin` with its first command.
- Map start, client: `clientscripts/<map>::main` runs after the server's `main` and the player's
  connect, before `begin`, then `localclientconnect`.
- Time: a server frame is 1000/`sv_fps` ms (20 by default, so 50 ms). `wait` is converted to ticks
  at 20 per second on the server and 60 per second on the client (one tick per rendered client
  frame), rounded to the nearest, and any wait above zero is at least one tick: `wait 0.01` is `wait
  0.05`. `waittillframeend` runs the thread again at the end of the current pass. There is no
  `waitrealtime`.
- Order: within a frame, threads due at the same time run newest first. `notify` never runs a
  waiting thread on the spot: it moves the waiters to the front of the current frame's list, and
  `endon` threads are killed at once. `G_RunFrame` (which the naming had called `DebugDumpAnims`: a
  debug dump is inlined at its end) runs the scripts in several passes: after the triggers, after
  each animation pass, and before deleted entities are freed and entities think.
- The engine runs 10 server callbacks and 15 client callbacks by name (`CodeCallback_PlayerDamage`
  gets 11 arguments, one more than in Call of Duty 4; `StartGameType` and `PlayerRevive` are looked
  up but never run in singleplayer) and sends 166 kinds of notify. The ones the zombies scripts wait
  on most: `death` (attacker, means of death, weapon), `disconnect`, `trigger` (the activator), the
  notify names scripts give to sounds, `movedone`, `rotatedone`, `bad_path` (the goal), `goal`,
  `damage` (amount, attacker, direction, point, means of death, model, tag, part) and `grenade_fire`
  (grenade, weapon).
- Errors: with `developer 0` a script error is silent and the thread goes on (the failed value is
  undefined; a `waittill` on an undefined entity does not wait). Even running out of script
  variables is such an error: World at War's `Scr_TerminalError` sets its terminal flag and then
  clears it again when it calls `Scr_Error` (Call of Duty 4's does stop the game). A loop that does
  not wait for 2,500 ms is killed in a release run and only warned about while the map loads.

**Player movement** (private `zone/pmove.md`, `pmove_dvars.tsv`): World at War's movement is Call of
Duty 4's, and in singleplayer it follows KisakCOD's multiplayer branch of the shared code, not its
singleplayer one. The core constants are CoD4's (`g_speed` 190, gravity 800, friction 5.5, stop
speed 100, jump height 39, sprint x1.5 for 4 s, step 18, walkable slope 0.7, the same acceleration,
slide/step and stance tables). World at War adds water (six levels that change friction and speed,
block prone, crouch and sprint, a separate swim move without gravity, and drowning), the last stand
player types (no movement input, a prone-height view), players that cannot stand on other players,
the bayonet charge, and different sprint rules (a second press does not end a sprint, aiming down
the sights blocks starting one, jumping out of a sprint costs meter). For CoD4-style movement built
from KisakCOD's singleplayer code, three things World at War shows differently: the ground trace
lets a player moving away from the surface leave the ground, the crouch check refuses prone on the
ground with an event, and client prediction uses the multiplayer design.

**Network protocol** (private `zone/network.md`, `netfields.tsv` with all 44 netfield tables of both
programs read from the bytes): World at War's messages are built on Call of Duty 4's multiplayer
code but a CoD4 parser will not read them. The server message opcodes moved (server command 8,
download 9, snapshot 10, end 12 in MP and 11 in SP); every netfield table changed (a larger entity
state with `targetname`, new field orders, wider fields, new actor tables, more player-state fields)
and there are new field codes for ranged floats, numbered differently in SP and MP; the gamestate
has 3,056 config strings and three more sections; network fragments are 1,164 bytes; user commands
carry separate forward and right moves and 27 button bits; and the connection is answered with a
Demonware license ID and carries a Demonware ticket instead of using an authorize server.
Singleplayer co-op (zombies included) is a protocol of its own on top of that: each client has a
16-frame ring, a snapshot carries the clients, up to 32 actors and their animation commands besides
the entities, entities can be delta-coded from other entities or a cache sent with the gamestate,
the local player can read the server's state directly, and there is no XOR encoding. A Rust server
that wants original clients to join must copy these tables exactly; a Rust-only game can use them as
the reference for what each snapshot holds.

## Zombies and custom maps

Zombies is a mode of the singleplayer program, not a program of its own. This is what the engine
does for it, and what loading a custom ("modded") map takes, read from the PC 1.7 SP code (static
reading only; the addresses and the per-map tables are in `dev/waw-decomp/zone/`, private).

**Where a map comes from.**
- Zones (fastfiles) at startup: `code_post_gfx` (with one more zone named at run time), then `mod`
  when `fs_game` is set, then `ui`, `localized_common`, `common` and `patch`. A level adds
  `<map>_load`, `localized_<map>`, `<map>_patch` and `<map>`.
- The zone path has three directory kinds (`DB_BuildOSPath_FromSource`, Black Ops' name; kind in EAX, the plugin's `FF_DIR`):
  the game's `zone\<language>\`, the mod folder `<fs_localAppData>\<fs_game>\`, and the user map
  folder `<fs_localAppData>\usermaps\<map>\` (named after the map without `_load`). `mod.ff` is looked
  for only in the mod folder. Any other zone: the game's copy first when it exists, then the mod
  folder, then the user map folder. A zone missing everywhere is a fatal error unless its name
  contains `_load`, `_patch`, `default` or `localized_` (a warning).
- A missing `<zone>_patch` is retried as `default`. A level loads `<map>_load` first, then (from
  `Com_LoadLevelFastFiles`) adds the `usermaps\<map>` iwd files when `fs_game` is set and the map's
  fastfile is there, then loads `localized_<map>`, `<map>_patch` and `<map>`. Only unsigned fastfiles
  load (`IWffu100`, version 387); `IWff0100` is refused as "file not supported".
- Asset pools (`g_poolSize`) cap what a map or mod may hold: 1,000 xmodels, 2,048 materials, 2,400
  images, 16,000 sound aliases, 4,096 xanims, 256 weapons, 400 effects, 640 menus, 1,024 raw files,
  50 string tables.
- Loose files and iwd files: the folder added last is searched first, so with `fs_game` set the order is
  `<fs_localAppData>\<fs_game>`, `<fs_localAppData>\usermaps`, `<fs_localAppData>\mods`, then the game's
  folders; World at War never searches `<install>\<fs_game>` (Call of Duty 4 does), so a mod must live
  under `fs_localAppData`. An iwd overrides loose files in its own folder and the last iwd in sort order
  wins. A user map's iwd files are added at the head of the list.
- Menus: with fastfiles (retail) the menu lists are database assets, so a mod's menus must be in its
  fastfile; 512 menus, 256 items per menu (extra items are dropped silently), 16 open menus.
- Sound aliases: with fastfiles they are database assets; the alias `probability` column is ignored
  (a random pick that avoids repeating the last one), at most 64 entries per alias name.
- `exec <name>`: any config but `config.cfg` is read from the loaded fastfiles' raw files first
  ("execing %s from fastfile"), then from disk, so a config inside a mod's fastfile wins.
- `fs_localAppData` is `SHGetFolderPath(CSIDL_LOCAL_APPDATA)` + `\Activision\CoDWaW`
  (`fs_personalDocuments` the same under the Documents folder); `fs_game` must be empty or under
  `mods/`. With `fs_game` set, `FS_Startup` adds `<fs_localAppData>\mods`, `\usermaps` and
  `\<fs_game>` to the file search paths last, so they are searched first: a mod's `.iwd` files (zip
  archives of images, sounds and other loose files) come before the game's. A Rust engine needs an
  equivalent folder setting; the owner's mods are in the game folder's `mods\`, where the mod tools
  build them. Each of the two community maps keeps everything in its mod folder: the map's four
  zones, `mod.ff`, `mod.arena` and its `.iwd` files (none in `usermaps`).
- Two zones with the same asset (`DB_LinkXAssetEntry`): the one from the zone with the higher or
  equal load flags (`g_zoneNames[i].flags`) is used and the other is kept behind it; `mod` has the
  highest flags (0x800), so its assets win. Only types that have a default asset name, plus raw files
  and map entities, may be overridden at all: physics presets and constraints, animations, models,
  materials, technique sets, images, sounds, light definitions, fonts, menus, localized strings,
  weapons, effects, impact effects and string tables. Any other type (the map's collision, world and
  game data, AI types, characters...) loaded twice is the fatal "Attempting to override asset" error.
- Every asset a zone defines takes a slot in its type's fixed pool (`g_poolSize`; "Exceeded limit of
  %d '%s' assets" is fatal): xanim 4,096, techset 4,096, image 2,400, material 2,048, xmodel 1,000,
  sound aliases 16,000, loaded sounds 1,600, localized strings 10,240, raw files 1,024, menus 640,
  fx 400, weapons 256, menu files 128, string tables 50, fonts 16 (32,767 asset entries in all). An
  entry whose name starts with `,` only refers to an asset another zone defines and takes no slot;
  if no loaded zone has it, a copy of the type's default asset is used (for a map's collision data
  the error is "Couldn't find the bsp for this map"). A Rust engine needs at least these sizes; the
  limits are read only where the pools are set up and by that error, so larger pools change nothing
  else.
- Besides the asset pools, a custom map and its scripts run into the engine's other fixed limits
  (all read from the code; the private `zone/engine_limits.tsv` has 85 with the check that enforces
  each and Call of Duty 4's value). The ones a zombies map is most likely to reach: 1,024 game
  entities, of which 1,018 are free for the map (CoD4 SP: 2,176); per script VM 65,533 variables
  and 24,573 objects (arrays, structs, threads; CoD4 32,767); 48 different hint strings (CoD4 32);
  1,022 localized strings, and `setText` takes a new one for every different text, never freed; 511
  models, 127 materials, 195 effects (CoD4 99) and 255 effect-on-tag pairs (never freed); 63
  sound aliases for looping and positioned sounds (CoD4 255); 32 actors and 36 sentients (a spawn
  past them just gives undefined); 31 hud elements sent to each player (more are not drawn); 32
  script menus; 31 models attached to one entity; and a script loop that does not wait for 2,500 ms
  (CoD4 5,000) is stopped as an infinite loop. Most of the index limits end the map with an error; a
  Rust engine that wants to run every map the original runs needs at least these sizes, and may
  raise them.
- The Mods menu (`LoadMods` in `UI_RunMenuScript`) lists the folders in
  `<fs_localAppData>\mods\` through `FS_GetModList`, each titled with the first 48 bytes of its
  `description.txt`; `RunMod` sets `fs_game` to `mods/<name>` and runs `vid_restart`, `ClearMods`
  resets `fs_game` the same way. So the game itself only uses mods from the local application data
  folder; a Rust launcher can set the folder and `fs_game` at start instead.
- The map lists: `coopmaps.arena` and a mod's `mod.arena` (a custom zombies map lists gametype `zom`);
  `UI_LoadArenas` also lists every fastfile in the zone folder that is not a shared, `_load` or
  `localized_` zone, with gametypes `cmp arc`.

**Zombie mode** is the dvar `zombiemode` (registered with `burnZombieTerrain` next to `arcademode`
and `onlinegame`). `SV_SetGametype` sets it when a map is set up: on when `ui_gametype` is `zom` or
`ui_mapname` contains `nazi_zombie_` (any case), so a custom map is a zombies map by its arena
gametype or by its name.

**Scripts the engine loads by name:** `maps/_callbackglobal`, `maps/_callbacksetup`,
`codescripts/delete`, `codescripts/struct`, `maps/<map>`, `clientscripts/<map>`,
`clientscripts/_callbacks` (the client callbacks `init`, `statechange`, `localclientconnect`,
`entityspawned`, `playFootstep`, `sound_notify`, `zombie_eye_callback`...), `clientscripts/_footsteps`,
`clientscripts/_zombie_mode`, and the AI type and character scripts of the map's spawners.
Following `#include` and `path::function` from those through the map's zones gives the scripts a map
can run: the four stock zombies maps call 689 different builtins (588 GSC, 101 CSC) of the 1,096
the two script VMs have, from 654 (Nacht der Untoten) to 688 (Der Riese). Two community zombies
maps the owner has call 687 and 685, **none outside the stock maps' set**: a VM that runs the stock
maps runs them. 688 of the 689 handlers are named; 663 are paired with Xbox 360 SP.
`script_signatures.py` reads each handler's arguments and result from the C: 572 of the 689 show them
(the rest pass everything on to other code), 420 carry their own usage or error message. 26 of them
(22 names, four of them in both VMs) do nothing in this program: their table entries, 40 in all, point at one shared one-byte `ret`
(the linker folds identical functions; the console command `assert` is the same function): file
output (`openfile`, `fprintln`, `closefile`), rumble, achievements, `bbprint`, `logstring`,
engine sound volume, and a few developer and console functions. A Rust VM can make them no-ops.
568 have a handler of the same name in KisakCOD (Call of Duty 4); for the other 121, World at War's
own, this C is the only reference: revive and last stand (`reviveplayer`, `startrevive`,
`visionsetlaststand`), player effects (`setelectrified`, `settransported`, `setburn`), score
multipliers, fake entities, the client-system state the client scripts sync with the server, and
persistent profile variables, among others. Each of the 121 was read from its handler, its callees
and its call sites in the stock scripts (and, where Ghidra hides an argument passed in a register,
from the disassembly), and written down with its arguments, its result and what reaches the client:
85 with high confidence, 36 medium. The 92 that Call of Duty 4 also has but that may not behave
the same were compared with KisakCOD's code the same way: the 63 client-script versions of server
builtins (Call of Duty 4 has no client-script VM; these take the local client number first and work on
the client's entities, sounds and effects) and the 29 server builtins whose arguments differ. 26 do the
same, 19 do the same on the client's data, 47 differ, and each difference is written down: for
example `doDamage` takes a destructible piece and a means-of-death name and passes no head hit in
zombie mode, `playLoopSound` and `stopLoopSound` take a fade time, `bulletTrace` and the damage traces
also hit foliage (one more content bit) and can skip water, `getWeaponModel` can return the view model,
`spawnVehicle` checks the origin and takes a destructible, and `setDvar` takes values up to 1,535
characters (1,023 in CoD4). The other 455 read
the same arguments as their KisakCOD counterparts. The private `zone/zombies_vm_spec.tsv` puts all
689 in one table for the Rust VM: the signature, "does nothing", World at War's own description, the
comparison, or the KisakCOD file and function to follow.

**What reaches the client.** World at War's own builtins use the same channels as Call of Duty 4:
server commands (a letter, then arguments, handled by `CG_DeployServerCommand`; for example 'W' burn,
'(' electrified, ')' transported, '4' double vision, '!' water sheeting, '|' fading blur, '@' client
exploders, '9' client-system state, 'N' a stat, 'e' "left the game"), config strings (the last stand
and berserker visions, the client-system names), entity events (blood impact, a launched dynamic
entity, burning, stopping a sound) and the snapshot itself (per-player visibility masks, player state
flags). `maps/_utility::wait_network_frame`, which the zombies scripts use to spread spawns over
frames, rests on two of them: the server runs `CodeCallback_LevelNotify("snapacknowledged")` for every
valid packet from a client, and `snapshotacknowledged` compares each client's acknowledged message
number with the one `getsnapshotindexarray` saved. As compiled (checked in the disassembly) it only
reports "not yet" once a client has acknowledged two or more messages past the saved one, so in
practice the wait is one client packet, or at most a second. A Rust server has to keep that timing,
not the intent, for scripts to pace the same way.

**What the C code does in zombie mode** (34 functions read `zombiemode` or `burnZombieTerrain`; each
has a `zombies:` note in the private C):
- Map start: no start-of-map save (as for outro, credits and intro_pac).
- AI: most of the actor code's zombie-mode branches are for hellhounds, not zombies: they test the
  actor's species (1 = dog), not its team (an earlier version of these notes read it as the axis
  team). In zombie mode a hellhound scores an enemy that already has `ai_maxAttackerCount` attackers
  10,000 lower, so the dogs spread over the players; hellhound melee traces with
  `ai_meleeRange`/`ai_meleeWidth`/`ai_meleeHeight`; the dogs' exposed-state think has its own trace
  and angle changes (a raised trace start, a limit of 90 instead of 60). Zombies themselves get
  their enemy from script (`favoriteenemy`) and melee with the normal 64-unit trace. A player's
  nearest path node is looked up again after 50 units of movement. How the engine runs an actor each
  frame (paths, movement, sight, melee, animscripts) is written up in the private
  `zone/ai_actors.md`.
- Damage: `doDamage` from script hits no particular body part (Call of Duty 4, and World at War
  outside zombie mode, count it as a head hit); a player hurt by another player gets
  `weapon_pvp_attack` (attacker, weapon, damage, means of death).
- Client: "is a hellhound" = zombie mode, an actor, species dog; hellhounds get special handling
  when drawn; hellhound `dogstep_` and `sound_` notetracks; weapon notetracks starting with `tesla_`
  go to the client scripts; no screen damage flash; the followed player's name while spectating;
  per-player overhead name colors; extra use-hint text. Zombie eyes: the server method `haseyes`
  sets an entity flag, and the client runs the client scripts' `zombie_eye_callback(localClientNum,
  on)` whenever it changes on an actor that is not a hellhound (and `entityspawned` the first time
  it sees one).
- Fire marks on the ground (flamethrower) only when `zombiemode` and `burnZombieTerrain` are both 0.
- Scoreboard: its own layout and `scorebar_zom_*` materials, the local player first, colors from
  `cg_ScoresColor_Zombie` and `cg_ScoresColor_TransparencyZombie`.

**Perks** (private `zone/engine_perks.tsv`): `setperk`, `unsetperk` and `hasperk` set, clear or test
one bit of the player state's `perks` (32 perks; an unknown name is a script error), which reaches
the client in the player state; spawning clears it. Of the zombies perks only two are in the engine:
Speed Cola's `specialty_fastreload` makes reload timers run at 1/`perk_weapReloadMultiplier` (0.5,
so twice as fast; the client plays the reload animation to match) and Double Tap's `specialty_rof`
makes firing and rechamber timers run at 1/`perk_weapRateMultiplier` (0.75). Juggernog
(`specialty_armorvest`) and Quick Revive (`specialty_quickrevive`) do nothing in the engine: the
zombies scripts set health to 160 and halve the revive time themselves. The Bowie knife's
`specialty_altmelee` replaces melee damage with `perk_altMeleeDamage` (200 by default; Der Riese
sets 1000) plus 0 to 4, and swaps the knife model and animations. Other perks the engine acts on:
weapon spread (0.65), held breath (+5 s), bullet penetration (x2), longer sprint (x2) and three
vehicle turret perks. Compared with Call of Duty 4 the perk indexes moved by one (World at War added
`specialty_reconnaissance` and dropped `specialty_parabolic`). Float dvar defaults like these are
passed in a register Ghidra does not show, so the decompiled `Dvar_RegisterFloat` calls do not show
them; they were read from the bytes.

The Xbox 360 disc executable (`default.xex`, no title update) has older zombies code: 11 of its
functions read `zombiemode` (PC 1.7: 35), and it has none of the engine support the later zombies
maps use (no `dogstep_` or `tesla_` notetracks, no `burnZombieTerrain`, no `zombie_eye_callback`).
So PC 1.7 is the reference for zombies and custom maps, and the PC's zombie-mode notes are not
copied to the paired Xbox 360 functions (21 of the 28 paired ones don't read `zombiemode` there).

**Reading the zones** (`zone_loader.py`): the game's zone loader is generated code, one function per
structure working through "var" globals; following it from the asset type switch names 258
functions (182 `Load_`, 71 `Mark_`, 5 `DB_`; 174 with KisakCOD's names), gives 252 of them their
structure, types 163 vars, and writes each structure's load program (private: it uses the plugin's
layouts). The stream rules it confirms,
for any fastfile including custom ones: a pointer 0 is null, -1 means the data follows inline, -2
inline and registered for later references, any other value v is block (v−1)>>29 at offset
(v−1)&0x1FFFFFFF; alignment applies to where the data lands in memory, the file stream has no
padding; the zone header is `XFile` (36 bytes), then `XAssetList` (16 bytes), then the script strings
(a pointer each, inline string when -1), then 8 bytes per asset. Format details belong to
[formats/waw.md](formats/waw.md).

## Asset file keys (weapons, vehicles, flame tables)

The PC programs can still read their asset files as text: a weapon file is
`WEAPONFILE\key\value\key\value...`, as the mod tools write them. For each kind of file the program
has a table of `{ "key", offset, type }` entries, and the loader looks every table key up in the text
(keys the table doesn't have are ignored) and stores its value at that offset of the structure. The
fastfiles hold the same structures, so the tables label each field of a ripped weapon with the name
modders use. `asset_fields.py` reads them (the output stays private, `dev/waw-decomp/zone/`).

- Tables in the PC 1.7 programs: weapons (565 entries, 562 keys), vehicles (134), flame tables (119),
  physics constraints (58), destructible pieces (45), vision sets (35), physics presets (11), plus
  map entity keys. The weapon table is the same in both PC programs.
- The Xbox 360 programs have no weapon table and none of its key strings (consoles load weapons
  only from fastfiles). Their vehicle, flame table and vision set tables are the PC's, offsets
  included, so those structures have the same layout on both.
- Types, from the generic parser (PC campaign 0x005F75D0): 0 string, 1/2/3 a char array of 1024 / 64
  / 256, 4 int, 5 bool (4 bytes), 6 float, 7 seconds in the file stored as int milliseconds (times
  1000.0), 8 effect, 9 model, 10 material, 11 sound alias. Types from 12 up belong to each kind's own
  parser; the weapon's (`BG_ParseWeaponDefSpecificFieldType`, 0x00423250) are 20 enumerations matched
  case-blind (an unknown name is a fatal error naming the weapon file), the player anim type, the
  bounce sound, up to 8 hide tags and up to 20 notetrack-to-sound pairs.
- Checked against the 422 weapon files of the mod tools pack (UGX, read inside its zip): they use
  558 of the 562 keys, every enumeration value is in the parser's lists, and 36 keys they carry are
  not in the table, so the game never reads them (`worldClipDropEffect`, `adsTransBlendTime`,
  `twoHanded`, `projectileDLight`, `horTurnSpeed`, `vertTurnSpeed`, the flamethrower's
  sound keys (the flame table has four of them, without the `Player` versions), and others).

## Types and parameters

- **Register parameters:** World at War PC was built with whole-program optimization, so many
  functions take arguments in registers. Ghidra's "Decompiler Parameter ID" analysis
  (`WawEnableParamId.java`) recovers them.
- **Types (private output only):** the T4SP server plugin's structure headers (AGPL-3.0, with offsets
  checked for this build) are turned into one C header by `t4sp_types.py` and parsed into Ghidra by
  `WawApplyTypes.java`: every `dvar_*` global becomes a `dvar_s *`, and the plugin's typed globals and
  prototypes are applied. The C then reads `dvar_cg_debugInfoCornerOffset->current.value`. Nothing from
  those headers goes into a published file.
- **Checking the header's layout** (`t4sp_types.py --check`, 2026-10-16): clang compiles the header
  under MSVC's 32-bit layout rules and compares it with all 7,681 member offsets and 875 structure
  sizes the plugin documents, once as written and once with `__declspec(align(N))` removed (Ghidra's C
  parser reads that and ignores it). Before, 25 offsets and one size were wrong, and the C showed it:
  `if (dvar_zombiemode->modified)` where the game reads the value. Three causes, all fixed:
  - `enum DvarFlags : unsigned __int16` was turned into a plain enum, 4 bytes instead of 2, which
    moved every `dvar_s` member after `flags` by 2 bytes (and the render command structures with
    1-byte enums). Sized enums now become a typedef of their type plus an `enum X_values` with the
    constants, and every enum is defined first with a typedef of its name.
  - `structs.hpp` includes the asset and script headers part way through, after the structures they
    hold by value; joining the files in a fixed order defined `corpseInfo_t` after `scr_data_t` used it.
    The headers are now copied in where they are included.
  - Ghidra ignores `__declspec(align(N))`: structures MSVC pads to 16, 64 or 128 bytes came out short
    (`BrushWrapper` 0x58 instead of 0x60). They get an explicit tail member up to the plugin's size;
    one structure without offset comments (`scrMemTreeGlob_t`, a single global) stays short in Ghidra.
  The checks also found that 73 aligned structures had no typedef of their name, so Ghidra did not
  know `animation_s` or `bgs_s` by their bare names; they now have one. Now the C reads
  `if ((dvar_zombiemode->current).enabled != false)`.
- **Typed game globals (private output only):** 26 of the arrays and structures the game code
  addresses directly get the header's types (PC SP), so the C reads `level.clients = g_clients;` and
  `if (cgsArray.processedSnapshotNum < cgArray.latestSnapshotNum)` instead of raw addresses. They were
  found three ways, and each one's evidence is in its table row (`dev/waw-decomp/types/game_globals.tsv`,
  named by `name_pc.py --extra`, typed by `WawApplyTypes.java`):
  - addresses published for this build (`g_entities`, `level`, `client`, `cm`, `cmd_args`, ...);
  - the code that fills `level`'s pointers: `G_InitGame` sets `level.clients`, `.sentients`, `.actors`
    and `.animCmds`, other setup functions `.vehicles` and `.turrets`; the lengths come from the same
    code (bytes cleared, loop ends): `g_entities[1024]`, `g_clients[4]`, `g_sentients[36]`,
    `g_actors[32]`, `s_vehicles[64]`, `turretInfoStore[96]`, `g_animCmds[1024]`;
  - `struct_globals.py vote`: every address the code references votes for "base = address − field
    offset"; the true base collects most of the structure's fields and clearly more than a base one
    field away, and the functions using the fields confirm it (`CG_ReadNextSnapshot` uses
    `cgsArray.processedSnapshotNum`, `SV_SpawnServer` `sv.state`, `SND_Init` `g_snd.playbackIdCounter`).
    `cgArray`'s `bgs` member votes for `cgArray + 0xBD168` on its own. Found this way: `cgsArray`,
    `cgMedia`, `cls`, `clientConnections`, `sv`, `g_snd`, `sharedUiInfo`, `cinematicGlob`.

  `struct_globals.py check` keeps the table consistent (no overlaps, no other name inside). Left out:
  the console (`con`): the header's `Console` starts with `con_outputWindowColor`, which is a dvar of
  its own, so the structure would take the dvar's name.
- **Parameter types from KisakCOD: tried, not used.** 992 of the 1,845 named functions with a KisakCOD
  prototype have as many parameters, all on the stack, but a read-only trial showed prototypes one
  place off: World at War added parameters (the script engine's `scriptInstance_t`: `EmitBreakStatement`'s
  first parameter is the script instance, not CoD4's `sourcePos`) and moved others into registers
  (`SV_ClientEnterWorld`'s client is in EAX; its one stack parameter is the `usercmd_s`). The call sites
  give too little to check them (18 argument positions with a typed argument), so only the plugin's
  prototypes, made for this build, type parameters. The same caution applies to carrying prototypes
  between CoD2, CoD3 and World at War.

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
- `types/` (top of the output folder): `game_globals.tsv` (the typed game globals and names found by
  hand, with evidence), the header and typed symbols `t4sp_types.py` makes from the plugin, and
  `zone_names.tsv` (`zone_loader.py`'s names and var types).
- `zone/` (top of the output folder): zombies and custom maps: the builtins each zombies map calls
  (`zombies_builtins.tsv`), the engine's zombie-mode functions with notes (`zombies_engine.tsv`, used
  by `name_pc.py --extra`), the zone loader's load programs (`schema.txt`) and a README with the
  addresses; `asset_fields_<build>.tsv` and `weapon_enums_pc_<sp|mp>.tsv`: every asset file key with
  its offset and type, and the names each weapon enumeration accepts (*Asset file keys*).
- `bo1_360/` (top of the output folder): World at War's Xbox 360 builds paired with Black Ops' (every
  pair with both games' names, whether they agree and the verdict of the check by hand), name
  candidates for the chat "x360-family", and `zone/zombies_builtins_bo1.tsv` (the zombies builtins in
  Black Ops); see *Cross-check with Black Ops*.
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

## Cross-check with Black Ops (2026-10-20)

Owner (2026-10-20): "you should also cross check waw with black ops 1". Black Ops is Treyarch's next
game on World at War's engine, and its Xbox 360 programs come from the same compiler as World at
War's, so the two pair up better than World at War's own PC and Xbox 360 builds. Black Ops' Xbox 360
programs were decompiled and named by the chat "x360-family" (names from KisakBlack, a
reconstruction of the Black Ops multiplayer program; `X360_FAMILY.md`); this chat's REA worker
imported them again on the owner's PC for their images (nothing compiled in the cloud).

- **Blind pairing.** `match_builds.py` on copies of the four exports with every function name taken
  out, so only strings, named dvar globals, the call graph and the pointer tables count, and the
  names can be compared afterwards without having steered the pairs: campaign 9,132 pairs (13,815
  World at War / 17,982 Black Ops functions), holdout 98.4%; multiplayer 8,271 (13,095 / 19,584),
  98.2%. x360-family's identical-code pairs agree with 761 of 769 and 642 of 651.
- **The names on the pairs** (version 32): campaign 938 the same, 258 the same in another style
  (table prefixes, `Cmd_` handlers), 96 different; multiplayer 886 / 146 / 75. The different ones,
  less the builtins Black Ops renamed (`spawn` became `codespawn`), the dvar registration placeholders,
  Demonware's renamed classes and console command style (42 + 33), were checked by hand against
  the strings, the PC counterpart, the call graph and both reconstructions.
- **Eight World at War names were wrong** (both PC builds; the Xbox 360 builds follow):

  | PC build | Address | Was | Now | Evidence |
  |---|---|---|---|---|
  | SP / MP | 0x00707A20 / 0x006D7DC0 | `CG_VisionSetUpdateTweaksFromFile_Film` | `R_RegisterDvars` | 537 renderer dvar descriptions; Black Ops' `R_RegisterDvars` shares 395 of them. The old name rested on 7 `r_filmTweak*` names that the vision file parser also uses. |
  | SP | 0x00629EC0 | `CG_NorthDirectionChanged` (t4-rtx) | `Flame_Init_DVars` | Registers the 28 flamethrower dvars Black Ops' `Flame_Init_DVars` registers. t4-rtx's comment (`CG_Init :: CG_NorthDirectionChanged call`) names its hooked call by CoD4's `CG_Init` order; the label is marked not taken in `public_symbols.tsv`. |
  | SP | 0x0059DC30 | `CG_Init` | `Com_Statmon` | Uses only `code_warning_file`, `_fps`, `_serverfps`: `Com_Statmon` in KisakCOD and KisakBlack (`CG_Init` registers the same three icons in CoD4). The real `CG_Init` is 0x00664570, which holds t4-rtx's hooked call and pairs with Black Ops' `CG_Init` (33 call-graph votes); on Xbox 360 it has `CG_LoadHudMenu` inlined and had taken that name. |
  | SP / MP | 0x004558E0 / 0x00461130 | `BG_AddPredictableEventToPlayerstate` | `CG_CheckPlayerstateEvents` | Calls `CG_EntityEvent` and prints the "processed predicted/server event" lines under `showevents`, as `CG_CheckPlayerstateEvents` does in KisakBlack; the old name rested on the one string `showevents`. |
  | MP | 0x0064FAE0 | `Scr_EvalMethod` | `VM_ExecuteInternal` | The opcode loop (15 KB, the 22 VM errors, called by `VM_Execute` and `VM_Resume`), like SP's `VM_ExecuteInternal` (T4SP). |
  | MP | 0x005617B0 | `LiveStorage_StatsInit` | `ResetCreateAClassNames` | Only sets the class name dvars (`customclass*`, `prestigeclass*`) from localized strings, as KisakBlack's `ResetCreateAClassNames` does; CoD4's `LiveStorage_StatsInit` also resets stats. |

  Most other differences are World at War right and Black Ops' name doubtful (for x360-family to
  look at: `CMD_VEH_AddVehicleToCompass` against `CScr_GetType`, `Dvar_InfoString`, a C runtime
  function named `Com_PrintMessage`), or Black Ops' code changed (`SV_RateMsec` became windowed,
  `ParseWeaponDefFiles` grew into `CG_SetupWeaponConfigString`). Seven stay open.
- **New names** from Black Ops for World at War functions without one (pass `bo1`, above): 43 + 58 on
  Xbox 360, 37 + 43 on PC. Names version 35: PC SP 4,157, MP 3,771, Xbox 360 SP 2,516, MP 2,197
  (version 32: 4,111 / 3,723 / 2,465 / 2,133). The call-graph pairs between the PC and Xbox 360 builds
  shift a little with the new names (campaign 5,476 to 5,463 pairs): two names that dropped out that way
  (`DB_SyncXAssets`, `R_ResetModelLighting`) are kept in the Xbox 360 hand table.
- **Zombies builtins in Black Ops** (both campaign programs' builtin tables read from their images,
  `zone/zombies_builtins_bo1.tsv`): of the 691 builtins World at War's zombies maps call, 506 keep
  their name on the paired handler, 131 keep the name on another handler, 4 were renamed
  (`spawnvehicle` / `spawnturret` became `codespawnvehicle` / `codespawnturret`, the client's
  `loadedfx` became `loadfx`, the client's `syncsystemstates` points at a stub) and 22 are gone
  (`getstat`, `setstat`, `setstatbit`, `stalingradspawn`, `dospawn`, the berserker and sound-bus
  builtins, `musicplay`/`musicstop`, `ambientplay`, `eqon`/`eqoff`). For porting scripts between
  the two games, not for World at War's own maps.

## Next

1. Zombies for the Rust port: the round, perk, power-up and box logic lives in the map scripts (GSC),
   so the engine side is the script VM with the 689 builtins above plus the zombie-mode code listed;
   next, per builtin, what it does (for the VM), starting with the ones every zombies map calls.
2. More types: type the big globals and function parameters (`gentity_s *`, `playerState_s *`) from
   the plugin's headers, the MP-only structures, and carry checked layouts to the Xbox 360 builds.
3. United Offensive: the same pairing against CoD2 PC and CoD 1.5 could name more of its
   multiplayer program (the weakest-named build of the Call of Duty 2 / UO decomp), if that chat
   wants it.
4. Per-system write-ups (weapons, the script VM) for the Rust port.
