# Call of Duty 3's engine, as found in `default.xex`

A map of the game's code for reading the recompilation and for the Rust port. It covers what each part does, what it's called and where it starts. Every name below comes from the symbol map, `COD_3_WAW_Setup-tool/recomp/config/symbols.toml`, which also gives the address and the evidence for each one. The map is made by `tools/name_functions.py`.

The executable has 19,083 functions. 2,242 have names: 2,023 are certain and 219 are likely.

## How the code is laid out

| Part | Names (examples) | What it does |
|---|---|---|
| **Quake III core** | `Sys_Main` (the program's `main`), `Com_Init`, `Com_Frame` (→ `Com_EventLoop`, `Com_ModifyMsec`, `SV_Frame`, ...), `VM_Call`, `Com_Printf`, `Com_Error`, `Cbuf_Execute`, `Cmd_AddCommand`, `Cmd_ExecuteString`, `Cvar_Get`, `Cvar_Set2`, `Cvar_Register`, `FS_ReadFile`, `Info_ValueForKey`, `va` | Startup, the frame loop, console text, commands and dvars, files. CoD3 keeps Quake III's structure and many of its error strings. `Com_Printf` and `Com_DPrintf` are empty in the retail build; `recomp/src/game/print_hooks.cpp` brings their output back. |
| **Client** | `CL_Init`, `CL_CreateCmd`, `CL_AdjustAngles`, `CL_MouseMove`, `CL_ParseServerMessage`, 74 `IN_*Down/Up` button commands | Input into user commands and the connection to the local server. Despite its name, `CL_MouseMove` is the right-stick turning. |
| **Client game (cgame)** | `CG_DrawActiveFrame` (each frame: `CG_ProcessSnapshots`, `CG_PredictPlayerState`, `CG_CalcViewValues`, `CG_AddPacketEntities`, `CG_Draw2D`), `CG_RegisterCvars`, `CG_ConsoleCommand`, `CG_ProcessSnapshots`, `CG_DrawCrosshair`, `CG_CalcCrosshairColor`, `CG_GetViewFov` | View, HUD and effects. The 147 `cg_*` dvars are a table at 0x825CDBF0. |
| **Server** | `SV_Init`, `SV_SpawnServer`, `SV_SetConfigstring`, `SV_BuildClientSnapshot`, `SV_ExecuteClientMessage`, `SV_Devmap_f`, `SV_Difficulty*_f` | The local server that single player runs on. |
| **Game** | `vmMain` (the game module's entry: 0 `G_InitGame`, 1 `G_ShutdownGame`, 2 `ClientConnect`, 3 `ClientBegin` ...), `G_RunFrame`, `G_RunFrameForEntity`, `ClientSpawn`, `ClientThink`, `ClientCommand` (with `god`, `noclip`, `give`, ...), `CheatsOk`, `G_RegisterCvars`, `G_RunThink` | Entities and gameplay rules. The 85 `g_*` dvars are a table at 0x825F3940. |
| **Shared game code** | `BG_FindItemForWeapon`, `PM_GroundTrace` | Player movement and items, run by both client and server. |
| **Renderer** | `R_Init`, `R_SelectVideoMode`, `R_SetMultisample`, `RE_RenderScene`, `RE_LoadWorldMap`, `GetRefAPI` | Reached through Quake III-style import and export tables. The game's resolution comes from its own mode table; see `render_resolution.h`. |
| **Assets** | `AssetLoad_TEX` / `MAT` / `MESH` / `SKEL` / `FX` / `FONT` / `AEPS` (+ `AssetUnload_*`), `*AssetBankSet` classes | Callbacks for each asset type in the `.cod` archives. The formats are in [formats/cod3.md](formats/cod3.md). |
| **Front end (menus)** | `FEMenuSystem`, `FEMenu`, `FEMenuEntry`, `FEText`, `PanelQuad`, 30 `*Menu` classes (`TitleMenu`, `MainMenu`, `SPMainMenu`, `PauseMenu`, `OptionsGameplayMenu`, `OptionsControlsMenu`, `ChapterSelectMenu`, ...), the global `g_FEManager` | The whole menu system, as C++ classes. There's no separate UI module. `recomp/src/game/menu_fov.cpp` adds the FOV row through these. |
| **In-game overlay** | 22 `IGO*Widget` classes (compass, grenade indicator, weapon name, progress bar ...) | HUD widgets drawn by the front end. |
| **Interactions** | 28 `InteractState*` classes (mortar loading, throwing back a live grenade, vehicles, rowboat ...), `InteractInputRcvr*` (button mash, stick swirl ...) | CoD3's context actions. |
| **Effects and animation** | `AbstractEffect*` (sound, particle, shake and rumble), `MetaNalBaseAnim`, `ADSMetaAnim*`, `nal*` anim classes, `AnimBankManager`, `CODNoteTrack` | Effects and the animation system. |
| **Level scripts** | `G_InitLevelInterface` (fills the table the level DLLs call), 76 `Broc_*` built-ins (`Broc_Thread`, `Broc_Wait`, `Broc_Notify`, `Broc_Endon` ...), `ScriptThread_Run`, `ScriptThread_CallEntry`, `ScriptThread_Destroy`, `AeThread*State`, `WaitTil*`, `Task*` | Each level's script, the "Broc" library, lives in its own DLL and runs as stackful threads (`recomp/src/game/script_threads.cpp`). The DLLs call the engine through an interface table of about 1,000 functions. |
| **World** | `SceneManager`, `EntityManager`, `PathNodeMgr`, `LightGridMgr`, `GrassRenderNode`, `SpeedTree*Node` | Scene, entities, AI paths, lighting, vegetation. |
| **Libraries (mostly unnamed)** | `D3D*`, FMOD, SpeedTree, video and audio codecs, the C runtime | Code linked into the game, not written for it. Naming it needs library signatures. The game's own copies of C functions are named `memcpy_guest` and similar. |

## RTTI

The retail executable still has MSVC run-time type information: 238 type descriptors, and 220 vtables with their class hierarchies. That's where the 210 class names come from.

- **Constructors, destructors and deleting destructors** are recognized from their code.
- **Other virtual functions** are named `Class::vfunc_N` (slot N) until their purpose is found. For example, `OptionsGameplayMenu::Init`, `::Refresh`, `::MoveUp` and `::Apply` were worked out for the FOV and aim assist rows.

## Reading the generated code

The recompiler turns each PowerPC function into a C++ function, `name(ctx, base)`:

- `ctx` holds the guest registers (`ctx.r3` is the first argument and the return value; `ctx.f1` the first float).
- `base` is where guest memory starts.

With the names from `recomp/config/names.toml` (made by `tools/gen_codegen_names.py`; the codegen reads it through SDK patch 0011), calls read `Com_Printf(ctx, base)` instead of `sub_82523A58(ctx, base)`. Functions without a name keep `sub_<address>`.

Our own code replaces or wraps functions:

- `REX_HOOK_RAW(Name)` with `__imp__Name` for the original;
- `[[midasm_hook]]` entries in `recomp/config/default.toml` for single instructions.

Guest addresses and structure layouts are in `recomp/src/game/addresses.h`.

To call a game function from port code, use the typed wrappers in `recomp/src/game/game_api.h` (one per function, address from `addresses.h`), not a raw `guest::Call`. Every named function, with its address, arguments, which port file hooks or calls it and where its name comes from, is listed in [FUNCTIONS.md](FUNCTIONS.md), made by `tools/catalog_functions.py`; its Conflicts section lists names that disagree between the symbol map, `names.toml`, `addresses.h` and the notes.

## Toward the Rust port

The recomp is the reference: what the game does, frame by frame, at known addresses. A system moves to Rust in these steps:

1. Name and understand it here (symbol map, `addresses.h`).
2. Write down its data and behavior (`docs/formats/` for files; this page and the code comments for logic).
3. Re-implement it in `engine/`, checked against the recomp.

The file formats are the furthest along: the archives, textures, meshes and materials already rip in Rust. The game's own logic is next: server, game, cgame and the level scripts.

**Step 3 is now mechanical:** the Rust decomp ([DECOMP.md](DECOMP.md)) ports one function at a time into `engine/crates/cod3_game` and checks each against the original machine code, run in a CPU emulator, on thousands of inputs. Started 2026-10-13 with the shared string, info-string, hash, angle, vector and trig code and the level scripts' math built-ins (43 functions, all matching). Each system gets two layers: the console's exact behavior (the record) and an engine version with the same results but none of the Xbox 360's limits.
