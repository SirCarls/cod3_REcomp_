# Call of Duty 3 functions

Every named function of `default.xex` in one list, with what the port does with it. Made by `tools/catalog_functions.py` from symbol map, codegen function table, addresses.h, docs/ENGINE.md, 25 helper notes, `game_api.h` and the port's hooks; edit those and run the tool again (`python3 tools/catalog_functions.py`), not this file.

2303 named functions: 232 the port already relied on, 1796 certain and 219 likely (the symbol map's confidence; see [ENGINE.md](ENGINE.md)), plus 56 named only by the port (addresses.h, game_api.h, notes). 134 of them are hooked or called by the port.

Columns: **codegen** is the C++ name the generated code gives the function, the name a `REX_HOOK_RAW` must use; **port use** is a hook (`REX_HOOK_RAW`), a mid-function hook (`config/*.toml`), a typed call (`game_api.h`), another call (`guest::Call` / `REX_EXTERN`) or a plain reference; **sources**: `map` the symbol map (known / high / medium), `names.toml` the codegen names, `addresses.h`, `game_api.h`, `notes` the helpers' working notes, `comments` the port's code comments, `ENGINE.md`.

## Calling a game function from port code

- **Use `src/game/game_api.h`.** One typed wrapper per function the port calls, named after it, with the address from `addresses.h` (one place). It replaces raw `guest::Call(...).Invoke(addr, {...})` calls. Game main thread only (a hook, or a task queued with `Engine::QueueGuestTask`). A call goes through the port's own hook of that function, as the game's own calls do.
- **Dvars:** write them with `Engine::CvarSet` (the cheat guard decides when `force` is allowed, then `game::CvarSet2`), read them with `game::CvarFind` and `game::CvarString` / `CvarValue` / `CvarInteger`. A dvar is never set by sending console text.
- **Console text** (`game::CbufAddText`) is for real console lines only (the console, `cod3_autoexec`): they need the game's parser, run in order at the next `Cbuf_Execute` and pass the cheat guard first.
- **Hooks:** `REX_HOOK_RAW(<codegen>)` and `REX_EXTERN(__imp__<codegen>)` for the original. A function without a name in `names.toml` is `sub_<address>`; when it gets a name, its hooks must be renamed (the list under Conflicts).
- **One instruction:** a `[[midasm_hook]]` in `config/default.toml` (the table below).

| Wrapper (`cod3::game::`) | Game function | Address | Guest signature |
|---|---|---|---|
| `CopyElement` | `FEText::ctor` | `0x820ACA38` |  |
| `OperatorNew` | `operator_new` | `0x820CC1D0` | `void* (size_t size)` |
| `OperatorDelete` | `operator_delete` | `0x820CC468` | `void (void* memory)` |
| `WaitJobGroup` | `live_video::kWaitJobGroup` | `0x8212A110` | `void (group)` |
| `UnbindAndIdle` | `live_video::kUnbindAndIdle` | `0x82130CC8` | `void ()` |
| `ReleaseTexture` | `nglReleaseTexture` | `0x8213F8A0` | `void (record*)` |
| `CreateTexture` | `nglCreateTexture` | `0x8214DDB8` | `record* (flags, format, width, height, r7, 1)` |
| `SetMultisample` | `R_SetMultisample` | `0x82153F50` | `void (int type)` |
| `ResourceRelease` | `D3DResource_Release` | `0x822F1340` | `void (IDirect3DResource9* resource)` |
| `MakeFullDepth` | `live_video::kMakeFullDepth` | `0x82419408` | `void ()` |
| `CbufAddText` | `Cbuf_AddText` | `0x824644D0` | `void (const char* text)` |
| `MessageBoxUpdateSelection` | `UpdateSelection` | `0x824BDDE8` | `void (buttons, int selected)` |
| `MessageBoxAddButton` | `msgbox::kAddButton` | `0x824C4428` | `void (DialogMenu* page, const char* key, callback)` |
| `MessageBoxClose` | `msgbox::kClose` | `0x824C4608` | `void (DialogMenuSystem* system)` |
| `CopyElement` | `PanelQuad::ctor` | `0x824C8498` |  |
| `MessageBoxOpen` | `msgbox::kOpen` | `0x824CDB28` | `void (DialogMenu* page, 1, 0)` |
| `MovieStop` | `movie::kStop` | `0x824D0528` | `void ()` |
| `PanelFindElement` | `Panel::FindElement` | `0x824D07D0` | `Element* (Panel* panel, const char* name)` |
| `MessageBoxSetMessage` | `msgbox::kSetMessage` | `0x824D31E0` | `void (DialogMenuSystem* system, const char* key)` |
| `PanelFindQuad` | `fe::kPanelFindQuad` | `0x824DFC70` | `PanelQuad* (Panel* panel, const char* name)` |
| `ComPrintf` | `Com_Printf` | `0x82523A58` | `void (const char* fmt, ...)` |
| `CvarFind` | `Cvar_FindVar` | `0x82529790` | `cvar_t* (const char* name)` |
| `CvarFindGuest` | `Cvar_FindVar` | `0x82529790` | `cvar_t* (const char* name)` |
| `CvarSet2` | `Cvar_Set2` | `0x8252B850` | `cvar_t* (const char* name, const char* value, qboolean force)` |
| `CvarGet` | `Cvar_Get` | `0x8252BBE0` | `cvar_t* (const char* name, const char* value, int flags)` |
| `Localize` | `Localize` | `0x8252CB98` | `const char* (StringTable* tables, const char* key)` |

The element and menu item wrappers (`ElementSet*`, `ItemSet*`) are virtual calls; their slots are in `addresses.h` (`fe::kElemSet*`, `fe::kItemSet*`).

### Where a direct call replaces a slower path

- Settings (F2, the game's own menus, the `fov` console command) already set dvars with a direct `Cvar_Set2` call, not a console line; nothing in the port sends console text for a value it could set directly. Console lines and `cod3_autoexec` stay text on purpose (see above).
- `Engine` (engine.cpp) and the cheat guard (cheat_guard.cpp) call the game through `game_api.h`, with the same calls and reads as before; the Cvar_Get hook passes its own name argument straight to `Cvar_FindVar` (`game::CvarFindGuest`), without a copy, as it did.

Not changed, could be (nothing here was timed, so no gain is claimed; none changes game speed):

- `cg_fov` (engine.cpp, `OnGuestFrame`) and `introScreen` (cutscene_skip.cpp) are looked up by name with `Cvar_FindVar` every frame until the game registers them (all through the menus); `cvar_t` pointers never move, so a lookup every 30th frame, like `SyncWatchedCvars` does, would do.
- `Engine::SyncWatchedCvars` reads both watched dvars' strings every frame, though only `cg_drawFPS` is needed every frame (the HUD); `com_maxfps` is compared only every 30th frame.
- `CheatWatch::Update` (every 10th frame while a cheat is on) copies each watched dvar's value and default into two `std::string`s to compare them; comparing the two guest strings in place would need no copy.
- Menu code (menu_fov.cpp, menu_video.cpp, pc_menu.cpp, multiplayer.cpp, button_prompts.cpp, cutscene_skip.cpp, render_resolution.cpp) still calls the same functions with raw `guest::Call` + `addresses.h` constants. Most of those calls have a `game_api.h` wrapper with the same arguments, so moving them over is a plain substitution, file by file, by each file's owner; the few without one (`storage::kPauseQuit`, `prompts::kStringFromChars`, `fe::adv::kSyncValueHighlight`, the page virtual calls) get one when their file moves.

## Conflicts

### Hooks that name no function (0)

None.

### names.toml and the symbol map (4)

- `0x8212AC58` `nglListAddQuad`: only in names.toml (not in the symbol map); regenerating names.toml drops it, and src/game/ultrawide.cpp use it by that name. Fix: add it to KNOWN_FUNCTIONS in tools/name_functions.py.
- `0x82134F70` `nglListAddString`: only in names.toml (not in the symbol map); regenerating names.toml drops it, and src/game/ultrawide.cpp use it by that name. Fix: add it to KNOWN_FUNCTIONS in tools/name_functions.py.
- `0x82430710` `RE_RenderScene`: only in names.toml (the symbol map has it as `RE_RenderScene` (medium)); regenerating names.toml drops it, and src/game/ultrawide.cpp use it by that name. Fix: add it to KNOWN_FUNCTIONS in tools/name_functions.py.
- 5 names of the symbol map are not in names.toml, e.g. `VectorNormalizeFast`, `Vec2Normalize`, `Vec2DistanceSq`, `Vec2Distance`, `Distance`: names.toml is older than the symbol map.

### addresses.h and the symbol map (1)

- `0x82344F98`: `XInputGetState` (addresses.h); `XamInputGetState_wrapper` (symbol map)

### Two names for one address (0)

None.

### Two addresses for one name (2)

- `Cmd_ExecuteString`: `0x82468720` (src/game/multiplayer.h); `0x8247E228` (addresses.h, names.toml, symbol map)
- `Sys_Main`: `0x823FCCA8` (notes/security); `0x82400098` (names.toml, symbol map)

### Hooks by sub_ name that the symbol map names (rename the hook if the name is promoted to names.toml) (15)

- `sub_82344F98` (src/game/button_prompts.cpp): the symbol map calls it `XamInputGetState_wrapper` (medium)
- `sub_824C3190` (src/game/menu_video.cpp): the symbol map calls it `ProfileEditMenu::MoveDown` (medium)
- `sub_824C3248` (src/game/menu_video.cpp): the symbol map calls it `ProfileEditMenu::MoveUp` (medium)
- `sub_824C41B8` (src/game/menu_fov.cpp): the symbol map calls it `AdvancedOptionsMenu::MoveDown` (medium)
- `sub_824C4218` (src/game/menu_fov.cpp): the symbol map calls it `AdvancedOptionsMenu::MoveUp` (medium)
- `sub_824C4338` (src/game/pc_menu.cpp): the symbol map calls it `PauseMenu::MoveDown` (medium)
- `sub_824C4390` (src/game/pc_menu.cpp): the symbol map calls it `PauseMenu::MoveUp` (medium)
- `sub_824DBEE8` (src/game/pc_menu.cpp): the symbol map calls it `MainMenu::Update` (medium)
- `sub_824E2948` (src/game/menu_video.cpp): the symbol map calls it `ProfileEditMenu::Init` (medium)
- `sub_824E56B8` (src/game/pc_menu.cpp): the symbol map calls it `MainMenu::Init` (medium)
- `sub_824E60E8` (src/game/menu_fov.cpp): the symbol map calls it `AdvancedOptionsMenu::Init` (medium)
- `sub_824E6588` (src/game/menu_fov.cpp): the symbol map calls it `AdvancedOptionsMenu::Refresh` (medium)
- `sub_824E6640` (src/game/pc_menu.cpp): the symbol map calls it `PauseMenu::Init` (medium)
- `sub_824EE128` (src/game/menu_fov.cpp): the symbol map calls it `AdvancedOptionsMenu::Update` (medium)
- `sub_824EFB18` (src/game/button_prompts.cpp): the symbol map calls it `TitleMenu::Update` (medium)

### Placeholder names (vfunc_N) with a better name elsewhere (6)

- `0x824C48B8` `FEMenuSystem::vfunc_17`: also called `FEMenuSystem::HandleButton`
- `0x824C4DB8` `FEMenuSystem::vfunc_12`: also called `FEMenuSystem::ProcessInput`
- `0x824C58F0` `FEMenu::vfunc_14`: also called `FEMenu::DrawItems`
- `0x824CDD40` `DialogMenu::vfunc_30`: also called `DialogMenu::Back`
- `0x824CFA00` `FEMultiLineText::vfunc_19`: also called `FEMultiLineText::Draw`
- `0x824CFD58` `FEText::vfunc_19`: also called `FEText::Draw`

### One function under several addresses.h constants (fine, but worth knowing) (2)

- `0x824C40B8`: `main_menu::kSelect`, `mp::kMainMenuSelect`
- `0x8252CB98`: `fe::kLocalize`, `prompts::kLocalize`

## Functions the port uses (134)

| Address | Name | Codegen | Subsystem | What it does | Arguments | Port use | Sources |
|---|---|---|---|---|---|---|---|
| `0x824644D0` | `Cbuf_AddText` | `Cbuf_AddText` | core | Appends console text; it runs at the next Cbuf\_Execute (once a frame). End each command with '\\n'. Only for real console text: a dvar is ... | `void (const char* text)` | game\_api CbufAddText; typed call src/game/game\_api.cpp | map:known; names.toml; addresses.h; game\_api.h; comments |
| `0x8247E380` | `Cbuf_Execute` | `Cbuf_Execute` | core | void Cbuf\_Execute(void) - run once per frame by Com\_Frame | `void (void)` | hook src/game/engine.cpp | map:known; names.toml; addresses.h; ENGINE.md |
| `0x825239A0` | `Com_Error` | `Com_Error` | core | void Com\_Error(int code, const char\* fmt, ...) - retail just hits DbgBreakPoint | `void (int code, const char* fmt, ...)` | hook src/game/print\_hooks.cpp | map:known; names.toml; addresses.h; ENGINE.md |
| `0x82523A58` | `Com_Printf` | `Com_Printf` | core | Empty in the retail game; the port's hook (game/print\_hooks.cpp) shows the line in the console and the log as a game line. The text is ... | `void (const char* fmt, ...)` | game\_api ComPrintf; hook src/game/print\_hooks.cpp; typed call src/game/game\_api.cpp | map:known; names.toml; addresses.h; game\_api.h; comments; ENGINE.md |
| `0x82523A60` | `Com_DPrintf` | `Com_DPrintf` | core | void Com\_DPrintf(const char\* fmt, ...) - body compiled out in retail | `void (const char* fmt, ...)` | hook src/game/print\_hooks.cpp | map:known; names.toml; addresses.h; ENGINE.md |
| `0x82529790` | `Cvar_FindVar` | `Cvar_FindVar` | core | The dvar, or 0 | `cvar_t* (const char* name)` | game\_api CvarFind, CvarFindGuest; typed call src/game/game\_api.cpp | map:known; names.toml; addresses.h; game\_api.h; comments |
| `0x8252B850` | `Cvar_Set2` | `Cvar_Set2` | core | The game's own write with its checks (force skips them). Port code writes dvars with Engine::CvarSet, which decides when force is allowed ... | `cvar_t* (const char* name, const char* value, qboolean force)` | game\_api CvarSet2; hook src/game/cheat\_watch.cpp; typed call src/game/game\_api.cpp | map:known; names.toml; addresses.h; game\_api.h; comments; ENGINE.md |
| `0x8252BBE0` | `Cvar_Get` | `Cvar_Get` | core | Registers the dvar with this default (or returns the one that exists) | `cvar_t* (const char* name, const char* value, int flags)` | game\_api CvarGet; hook src/game/cheat\_guard.cpp; typed call src/game/game\_api.cpp | map:known; names.toml; addresses.h; game\_api.h; notes: multiplayer; comments; ENGINE.md |
| `0x82536DD0` | `Com_Frame` | `Com_Frame` | core | the frame boundary; hooked |  | hook src/game/render\_resolution.cpp; mid-asm Cod3FrameLimiterHook config/default.toml | map:known; names.toml; addresses.h; comments; ENGINE.md |
| `0x8250B460` | `CL_AdjustAngles` | `CL_AdjustAngles` | client | void CL\_AdjustAngles(void): turns the view from the right stick (or the +left/+right keys with cl\_yawspeed / cl\_pitchspeed). Called first thing by CL\_CreateCmd ... | `void (void)` | hook src/game/mouse\_look.cpp | map:known; names.toml; addresses.h; comments; ENGINE.md |
| `0x82439530` | `CG_DrawMountedCrosshair` | `CG_DrawMountedCrosshair` | cgame | addresses.h: the mounted gun / vehicle crosshair |  | mid-asm Cod3MountedCrosshairHook config/default.toml | map:known; names.toml |
| `0x8243A198` | `CG_GetViewFov` | `CG_GetViewFov` | cgame | float CG\_GetViewFov(void): reads cg\_fov, clamps to [1,160], applies ADS / turret / vehicle zoom, returns the horizontal FOV in f1. Every consumer (refdef, zoom ... | `float (void)` | hook src/game/fov.cpp | map:known; names.toml; addresses.h; comments; ENGINE.md |
| `0x824447C8` | `CG_DrawCrosshair` | `CG_DrawCrosshair` | cgame | addresses.h / config hooks |  | mid-asm Cod3CrosshairHook config/default.toml | map:known; names.toml; ENGINE.md |
| `0x825145D8` | `SV_LoadMpExe_f` | `SV_LoadMpExe_f` | server | SV\_LoadMpExe\_f, the load\_mp\_exe command (name at 0x82069654, registered with Cmd\_AddServerCommand at 0x82519150). Hooked |  | hook src/game/multiplayer.cpp | map:high; names.toml; addresses.h; notes: multiplayer-exit |
| `0x825148D0` | `SV_MapRestart_f` | `SV_MapRestart_f` | server | addresses.h kMapRestart ('map\_restart') |  | hook src/game/cheat\_watch.cpp | map:known; names.toml; addresses.h |
| `0x825149A8` | `SV_MapRestartInternal_f` | `SV_MapRestartInternal_f` | server | addresses.h kMapRestartInternal ('map\_restart\_internal') |  | hook src/game/cheat\_watch.cpp | map:known; names.toml; addresses.h |
| `0x82517A00` | `SV_SpawnServer` | `SV_SpawnServer` | server | Level start: SV\_SpawnServer (prints "------ Server Initialization ------") and the map\_restart / map\_restart\_internal commands |  | hook src/game/cheat\_watch.cpp | map:known; names.toml; addresses.h; notes: cutscenes; ENGINE.md |
| `0x82542F00` | `Cmd_JumpToNode_f` | `Cmd_JumpToNode_f` | game | checks the cheats itself |  | hook src/game/cheat\_watch.cpp | map:known; names.toml; addresses.h |
| `0x82543048` | `Cmd_SetViewpos_f` | `Cmd_SetViewpos_f` | game | checks the cheats itself |  | hook src/game/cheat\_watch.cpp | map:known; names.toml; addresses.h |
| `0x825432C8` | `CheatsOk` | `CheatsOk` | game | qboolean CheatsOk(gentity\_t\*) | `qboolean (gentity_t*)` | hook src/game/cheat\_watch.cpp | map:known; names.toml; addresses.h; ENGINE.md |
| `0x82545128` | `Cmd_Ufo_f` | `Cmd_Ufo_f` | game | Cmd\_Ufo\_f: client-&gt;ufo |  | hook src/game/cheat\_watch.cpp | map:known; names.toml; addresses.h |
| `0x825451B0` | `Cmd_Noclip_f` | `Cmd_Noclip_f` | game | Cmd\_Noclip\_f: client-&gt;noclip |  | hook src/game/cheat\_watch.cpp | map:known; names.toml; addresses.h |
| `0x82545248` | `Cmd_Notarget_f` | `Cmd_Notarget_f` | game | Cmd\_Notarget\_f: ent-&gt;flags ^= 2 |  | hook src/game/cheat\_watch.cpp | map:known; names.toml; addresses.h |
| `0x825452E0` | `Cmd_God_f` | `Cmd_God_f` | game | Cmd\_God\_f: ent-&gt;flags ^= 1 |  | hook src/game/cheat\_watch.cpp | map:known; names.toml; addresses.h |
| `0x82560988` | `Cmd_Give_f` | `Cmd_Give_f` | game | addresses.h kGive |  | hook src/game/cheat\_watch.cpp | map:known; names.toml; addresses.h |
| `0x8212A110` | `live_video::kWaitJobGroup` | `sub_8212A110` | renderer | Waits until the group's jobs are done | `void (group)` | game\_api WaitJobGroup; typed call src/game/game\_api.cpp; calls src/game/render\_resolution.cpp | addresses.h; game\_api.h |
| `0x8212AC58` | `nglListAddQuad` | `nglListAddQuad` | renderer | nglListAddQuad(quad\*): copies 104 bytes | `(quad*)` | hook src/game/ultrawide.cpp | names.toml; addresses.h; notes: ultrawide |
| `0x8212FDB8` | `R_SelectVideoMode` | `R_SelectVideoMode` | renderer | (const u32\* wanted, count) | `(const u32* wanted, count)` | hook src/game/render\_resolution.cpp | map:known; names.toml; addresses.h; ENGINE.md |
| `0x82130808` | `live_video::kBeginNextFrame` | `sub_82130808` | renderer | After each present (0x82525190 -&gt; 0x82130980), this flips the double- buffered list memory, resets kListCursor and opens the next frame's list (0x8212DB18), so ... |  | hook src/game/render\_resolution.cpp | addresses.h |
| `0x82130CC8` | `live_video::kUnbindAndIdle` | `sub_82130CC8` | renderer | Unbinds streams and textures, then D3DDevice\_BlockUntilIdle | `void ()` | game\_api UnbindAndIdle; typed call src/game/game\_api.cpp; calls src/game/render\_resolution.cpp | addresses.h; game\_api.h |
| `0x82134F70` | `nglListAddString` | `nglListAddString` | renderer | (font, text, f1 x, f2 y, f3 depth, f4 x scale, f5 y scale, r8 color) | `(font, text, f1 x, f2 y, f3 depth, f4 x scale, f5 y scale, r8 color)` | hook src/game/ultrawide.cpp | names.toml; addresses.h; notes: ultrawide |
| `0x8213F8A0` | `nglReleaseTexture` | `sub_8213F8A0` | renderer | void (record\*) | `void (record*)` | game\_api ReleaseTexture; typed call src/game/game\_api.cpp; calls src/game/render\_resolution.cpp | addresses.h; game\_api.h; comments |
| `0x8214DDB8` | `nglCreateTexture` | `sub_8214DDB8` | renderer | An ngl texture record (44 bytes; IDirect3DTexture9\* at +20) | `record* (flags, format, width, height, r7, 1)` | game\_api CreateTexture; typed call src/game/game\_api.cpp; calls src/game/render\_resolution.cpp | addresses.h; game\_api.h; notes: shadows; comments |
| `0x82153F50` | `R_SetMultisample` | `R_SetMultisample` | renderer | 0 none, 1 2x. Resets the device with the present parameters and remakes the screen-sized targets | `void (int type)` | game\_api SetMultisample; hook src/game/render\_resolution.cpp; typed call src/game/game\_api.cpp; calls src/game/render\_resolution.cpp | map:known; names.toml; addresses.h; game\_api.h; comments; ENGINE.md |
| `0x822F1340` | `D3DResource_Release` | `sub_822F1340` | renderer | D3DResource\_Release(resource) | `void (IDirect3DResource9* resource)` | game\_api ResourceRelease; typed call src/game/game\_api.cpp; calls src/game/render\_resolution.cpp | addresses.h; game\_api.h; comments |
| `0x82419098` | `shadows::kMakeMaps` | `sub_82419098` | renderer | hooked |  | hook src/game/shadows.cpp | addresses.h |
| `0x82419408` | `live_video::kMakeFullDepth` | `sub_82419408` | renderer | Makes the full-size depth texture from the video mode when there is none | `void ()` | game\_api MakeFullDepth; typed call src/game/game\_api.cpp; calls src/game/render\_resolution.cpp | addresses.h; game\_api.h |
| `0x8241A0A0` | `RE_BeginFrame` | `RE_BeginFrame` | renderer | refexport slot 24: SCR\_UpdateScreen calls it before SCR\_DrawScreenField (Q3 BeginFrame) |  | hook src/game/ultrawide.cpp | map:high; names.toml |
| `0x82425BD0` | `shadows::kAddCaster` | `sub_82425BD0` | renderer | (r3 ?, r4 object, ..., r9 flags\*, r10 record\*\*) -&gt; 0; hooked. Returns at once unless bit kShadowView of the record's 64-bit mask is set, the byte at record + ... | `(r3 ?, r4 object, ..., r9 flags*, r10 record**)` | hook src/game/shadows.cpp | addresses.h |
| `0x82430710` | `RE_RenderScene` | `RE_RenderScene` | renderer | RE\_RenderScene(refdef), refexport | `(refdef)` | hook src/game/ultrawide.cpp | map:medium; names.toml; addresses.h; notes: ultrawide; comments; ENGINE.md |
| `0x820ACA38` | `FEText::ctor` | `FEText__ctor` | frontend | FEText(this), 112 bytes | `(this)` | game\_api CopyElement; typed call src/game/game\_api.cpp; calls src/game/menu\_fov.cpp, src/game/pc\_menu.cpp | map:high; names.toml; addresses.h; game\_api.h; comments |
| `0x820AD1B8` | `FEMultiLineText::vfunc_0` | `FEMultiLineText__vfunc_0` | frontend | rtti FEMultiLineText vtable 0x82070C84 slot 0 |  | hook src/game/button\_prompts.cpp | map:high; names.toml |
| `0x820ADAC0` | `FEMenu::OnLeft` (likely) | `sub_820ADAC0` | frontend | the default: the page has no left/right |  | refers to src/game/pc\_menu.cpp | map:medium; addresses.h |
| `0x823F86D0` | `msgbox::kCloseCallback` | `sub_823F86D0` | frontend | return 1: the button just closes the box |  | refers to src/game/multiplayer.cpp | addresses.h |
| `0x823FBC58` | `Profile_LoadSettings` | `Profile_LoadSettings` | frontend | Reads the settings from the profile save (0x823FBC58) |  | hook src/game/aim\_assist.cpp | map:known; names.toml; addresses.h |
| `0x824A5978` | `prompts::kStringFromChars` | `sub_824A5978` | frontend | The game's refcounted string: chars at +12. (String\*\* out, const char\*) |  | calls src/game/button\_prompts.cpp | addresses.h |
| `0x824BC070` | `ProfileEditMenu::vfunc_19` | `ProfileEditMenu__vfunc_19` | frontend | vtbl[19] (this, item); hooked | `(this, item)` | hook src/game/menu\_video.cpp | map:high; names.toml; addresses.h |
| `0x824BD160` | `OptionsGameplayMenu::Toggle` | `OptionsGameplayMenu__Toggle` | frontend | (this, dir) via vtbl[27]/[28] | `(this, dir)` | hook src/game/menu\_fov.cpp | map:known; names.toml; addresses.h |
| `0x824BD3F8` | `fe::adv::kSyncValueHighlight` | `sub_824BD3F8` | frontend | (this): after a move | `(this)` | calls src/game/menu\_fov.cpp | addresses.h |
| `0x824BD750` | `main_menu::kNoCallback` | `sub_824BD750` | frontend | return 1: closes the box |  | refers to src/game/pc\_menu.cpp | addresses.h |
| `0x824BDDE8` | `UpdateSelection` (likely) | `sub_824BDDE8` | frontend | Puts the highlight on button selected (system-&gt;buttons, msgbox::kSysButtons) | `void (buttons, int selected)` | game\_api MessageBoxUpdateSelection; typed call src/game/game\_api.cpp; calls src/game/multiplayer.cpp, src/game/pc\_menu.cpp | addresses.h; game\_api.h; notes: menus-pass |
| `0x824BFA70` | `main_menu::kPanelDraw` | `sub_824BFA70` | frontend | (panel): any panel's draw; hooked | `(panel)` | hook src/game/pc\_menu.cpp | addresses.h |
| `0x824C3190` | `ProfileEditMenu::MoveDown` (likely) | `sub_824C3190` | frontend | vtbl[26]; hooked |  | hook src/game/menu\_video.cpp | map:medium; addresses.h |
| `0x824C3248` | `ProfileEditMenu::MoveUp` (likely) | `sub_824C3248` | frontend | vtbl[25]; hooked |  | hook src/game/menu\_video.cpp | map:medium; addresses.h |
| `0x824C3E48` | `OptionsGameplayMenu::Apply` | `OptionsGameplayMenu__Apply` | frontend | void OptionsGameplayMenu::Apply(page): copies the page's four rows into the settings | `void (page)` | hook src/game/aim\_assist.cpp | map:known; names.toml; addresses.h |
| `0x824C3F08` | `OptionsGameplayMenu::MoveDown` | `OptionsGameplayMenu__MoveDown` | frontend | vtbl[26] (this) | `(this)` | hook src/game/menu\_fov.cpp | map:known; names.toml; addresses.h |
| `0x824C3FE0` | `OptionsGameplayMenu::MoveUp` | `OptionsGameplayMenu__MoveUp` | frontend | vtbl[25] (this) up/down + texts | `(this)` | hook src/game/menu\_fov.cpp | map:known; names.toml; addresses.h |
| `0x824C40B8` | `MainMenu::vfunc_18` | `MainMenu__vfunc_18` | frontend | MainMenu::vfunc\_18: item 1 runs load\_mp\_exe | `(this, item)` | hook src/game/pc\_menu.cpp | map:high; names.toml; addresses.h |
| `0x824C41B8` | `AdvancedOptionsMenu::MoveDown` (likely) | `sub_824C41B8` | frontend | vtbl[26]; hooked |  | hook src/game/menu\_fov.cpp | map:medium; addresses.h |
| `0x824C4218` | `AdvancedOptionsMenu::MoveUp` (likely) | `sub_824C4218` | frontend | vtbl[25]; hooked |  | hook src/game/menu\_fov.cpp | map:medium; addresses.h |
| `0x824C4338` | `PauseMenu::MoveDown` (likely) | `sub_824C4338` | frontend | vtable slot 26; meaning from Options &gt; Gameplay vtbl[26] move selection down |  | hook src/game/pc\_menu.cpp | map:medium; addresses.h |
| `0x824C4390` | `PauseMenu::MoveUp` (likely) | `sub_824C4390` | frontend | Up / down (vtbl[25] / vtbl[26]): FEMenu's move, then 0x824BD668 wraps 0 &lt;-&gt; 6. Hooked (the rows' order on screen differs from the items') |  | hook src/game/pc\_menu.cpp | map:medium; addresses.h |
| `0x824C4428` | `msgbox::kAddButton` | `sub_824C4428` | frontend | The callback gets the player index and returns true to close the box | `void (DialogMenu* page, const char* key, callback)` | game\_api MessageBoxAddButton; hook src/game/pc\_menu.cpp; typed call src/game/game\_api.cpp; calls src/game/multiplayer.cpp, src/game/pc\_menu.cpp | addresses.h; game\_api.h |
| `0x824C4608` | `msgbox::kClose` | `sub_824C4608` | frontend | Closes the box if it is up | `void (DialogMenuSystem* system)` | game\_api MessageBoxClose; typed call src/game/game\_api.cpp; calls src/game/multiplayer.cpp, src/game/pc\_menu.cpp | addresses.h; game\_api.h |
| `0x824C48B8` | `FEMenuSystem::vfunc_17` | `FEMenuSystem__vfunc_17` | frontend | rtti FEMenuSystem vtable 0x82071884 slot 17 (also in 1 derived vtables) |  | hook src/game/pc\_menu.cpp | map:high; names.toml; comments |
| `0x824C4DB8` | `FEMenuSystem::vfunc_12` | `FEMenuSystem__vfunc_12` | frontend | FEMenuSystem::vfunc\_12, ProcessInput(this); hooked |  | hook src/game/pc\_menu.cpp | map:high; names.toml; addresses.h; comments |
| `0x824C68E0` | `prompts::kTokenGlyph` | `sub_824C68E0` | frontend | (text at '~', int\* out\_string) -&gt; token length (0 = none), \*out = the button's character as a string; hooked |  | hook src/game/button\_prompts.cpp | addresses.h |
| `0x824C8498` | `PanelQuad::ctor` | `PanelQuad__ctor` | frontend | PanelQuad(this), 80 bytes | `(this)` | game\_api CopyElement; typed call src/game/game\_api.cpp; calls src/game/menu\_fov.cpp, src/game/pc\_menu.cpp | map:high; names.toml; addresses.h; game\_api.h; comments |
| `0x824CD2D0` | `OptionsGameplayMenu::Update` | `OptionsGameplayMenu__Update` | frontend | vtbl[16] (this, f1=dt) | `(this, f1=dt)` | hook src/game/menu\_fov.cpp | map:known; names.toml; addresses.h |
| `0x824CD398` | `OptionsGameplayMenu::vfunc_14` | `OptionsGameplayMenu__vfunc_14` | frontend | The gameplay page's draw (vtbl[14], 0x824CD398): its panel, then its items (FEMenu::DrawItems 0x824C58F0 draws each item's label, highlighted or not) |  | hook src/game/menu\_fov.cpp | map:high; names.toml; addresses.h |
| `0x824CD530` | `AdvancedOptionsMenu::vfunc_14` | `AdvancedOptionsMenu__vfunc_14` | frontend | vtbl[14]: panel, items, values; hooked |  | hook src/game/menu\_fov.cpp | map:high; names.toml; addresses.h |
| `0x824CD768` | `storage::kPauseItemStates` | `sub_824CD768` | frontend | (this); hooked | `(this)` | hook src/game/pc\_menu.cpp | addresses.h |
| `0x824CD9E0` | `InGameMenuSystem::vfunc_14` | `InGameMenuSystem__vfunc_14` | frontend | rtti InGameMenuSystem vtable 0x82071884 slot 14 |  | hook src/game/pc\_menu.cpp | map:high; names.toml; addresses.h |
| `0x824CDB28` | `msgbox::kOpen` | `sub_824CDB28` | frontend | Shows the box | `void (DialogMenu* page, 1, 0)` | game\_api MessageBoxOpen; hook src/game/pc\_menu.cpp; typed call src/game/game\_api.cpp; calls src/game/multiplayer.cpp, src/game/pc\_menu.cpp | addresses.h; game\_api.h |
| `0x824CE3E8` | `DialogMenuSystem::vfunc_14` | `DialogMenuSystem__vfunc_14` | frontend | rtti DialogMenuSystem vtable 0x820719D4 slot 14 |  | hook src/game/pc\_menu.cpp | map:high; names.toml; addresses.h |
| `0x824CE4D0` | `FrontEndMenuSystem::vfunc_14` | `FrontEndMenuSystem__vfunc_14` | frontend | Each menu system's draw (vtbl +56), which draws its page: the footers drawn inside belong to that system. Hooked |  | hook src/game/pc\_menu.cpp | map:high; names.toml; addresses.h |
| `0x824CFA00` | `FEMultiLineText::vfunc_19` | `FEMultiLineText__vfunc_19` | frontend | rtti FEMultiLineText vtable 0x82070C84 slot 19 |  | hook src/game/button\_prompts.cpp | map:high; names.toml; comments |
| `0x824D07D0` | `Panel::FindElement` | `Panel__FindElement` | frontend | A panel's element by name, or 0 | `Element* (Panel* panel, const char* name)` | game\_api PanelFindElement; typed call src/game/game\_api.cpp; calls src/game/menu\_fov.cpp, src/game/menu\_video.cpp, src/game/pc\_menu.cpp | map:known; names.toml; addresses.h; game\_api.h; comments |
| `0x824D31E0` | `msgbox::kSetMessage` | `sub_824D31E0` | frontend | Localized; an unknown key shows as it is. text is a guest string that stays valid | `void (DialogMenuSystem* system, const char* key)` | game\_api MessageBoxSetMessage; typed call src/game/game\_api.cpp; calls src/game/multiplayer.cpp, src/game/pc\_menu.cpp | addresses.h; game\_api.h |
| `0x824D7650` | `prompts::kLayoutLine` | `sub_824D7650` | frontend | (line, f1 text scale, f2 button scale): splits the line at its tokens and measures them with font 1 at f2; hooked | `(line, f1 text scale, f2 button scale)` | hook src/game/button\_prompts.cpp | addresses.h |
| `0x824D7B48` | `prompts::kMeasureWord` | `sub_824D7B48` | frontend | (const char\* word, font index, f1 text scale, f2 button scale) -&gt; f1 width, for word wrap; hooked | `(const char* word, font index, f1 text scale, f2 button scale)` | hook src/game/button\_prompts.cpp | addresses.h |
| `0x824D7D88` | `prompts::kDrawLine` | `sub_824D7D88` | frontend | (line, f1 ?, f2 x scale, f3 y scale, f4 button scale, f5 button raise, r5 text color, r6 button color); hooked | `(line, f1 ?, f2 x scale, f3 y scale, f4 button scale, f5 button raise, r5 text color, r6 button color)` | hook src/game/button\_prompts.cpp | addresses.h |
| `0x824DA818` | `ProfileEditMenu::dtor` | `ProfileEditMenu__dtor` | frontend | rtti: stores the ProfileEditMenu vtable, then its base's (FEMenu) |  | hook src/game/menu\_video.cpp | map:high; names.toml |
| `0x824DB960` | `OptionsGameplayMenu::Refresh` | `OptionsGameplayMenu__Refresh` | frontend | vtbl[21] (this) | `(this)` | hook src/game/menu\_fov.cpp | map:known; names.toml; addresses.h |
| `0x824DBB38` | `OptionsGameplayMenu::dtor` | `OptionsGameplayMenu__dtor` | frontend | menu\_fov.cpp (destructor body) |  | hook src/game/menu\_fov.cpp | map:known; names.toml |
| `0x824DBBF0` | `OptionsGameplayMenu::ctor` | `OptionsGameplayMenu__ctor` | frontend | (this, parent) | `(this, parent)` | mid-asm Cod3GameplayItemCount config/default.toml | map:known; names.toml; addresses.h |
| `0x824DBEE8` | `MainMenu::Update` (likely) | `sub_824DBEE8` | frontend | vtbl[16]; hooked | `(this, dt)` | hook src/game/pc\_menu.cpp | map:medium; addresses.h; comments |
| `0x824DBFE8` | `MainMenu::dtor` | `MainMenu__dtor` | frontend | rtti: stores the MainMenu vtable, then its base's (FEMenu) |  | hook src/game/pc\_menu.cpp | map:high; names.toml |
| `0x824DC1C0` | `fe::adv::kToggle` | `sub_824DC1C0` | frontend | (this, right): OnLeft / OnRight; hooked | `(this, right)` | hook src/game/menu\_fov.cpp | addresses.h |
| `0x824DC640` | `AdvancedOptionsMenu::dtor` | `AdvancedOptionsMenu__dtor` | frontend | rtti: stores the AdvancedOptionsMenu vtable, then its base's (FEMenu) |  | hook src/game/menu\_fov.cpp | map:high; names.toml |
| `0x824DC798` | `storage::kPauseQuitYes` | `sub_824DC798` | frontend | () -&gt; 1 | `()` | hook src/game/pc\_menu.cpp; refers to src/game/pc\_menu.cpp | addresses.h |
| `0x824DC7F8` | `storage::kPauseQuit` | `sub_824DC7F8` | frontend | (this) | `(this)` | calls src/game/pc\_menu.cpp | addresses.h |
| `0x824DD630` | `FEMultiLineText::vfunc_93` | `FEMultiLineText__vfunc_93` | frontend | rtti FEMultiLineText vtable 0x82070C84 slot 93 |  | hook src/game/button\_prompts.cpp | map:high; names.toml |
| `0x824DD790` | `FEMultiLineText::vfunc_92` | `FEMultiLineText__vfunc_92` | frontend | rtti FEMultiLineText vtable 0x82070C84 slot 92 |  | hook src/game/button\_prompts.cpp | map:high; names.toml |
| `0x824DD9A8` | `prompts::kActionText` | `sub_824DD9A8` | frontend | ("[ACTION]") -&gt; "~square", "Press and hold ~square", "Use the left thumbstick" ([USE\_LEFT\_STICK], INGAME\_XBOX\_USE\_\*\_STICK) ...; hooked |  | hook src/game/button\_prompts.cpp | addresses.h |
| `0x824DE290` | `prompts::kExpandActions` | `sub_824DE290` | frontend | (const char\* text) -&gt; its "[ACTION]" words expanded, in a static 512-byte buffer; hooked | `(const char* text)` | hook src/game/button\_prompts.cpp | addresses.h |
| `0x824DEAD0` | `FEMultiLineText::ctor` | `FEMultiLineText__ctor` | frontend | rtti: stores the FEMultiLineText vtable at this+0; called on the result of operator new |  | hook src/game/button\_prompts.cpp | map:high; names.toml |
| `0x824DFC70` | `fe::kPanelFindQuad` | `sub_824DFC70` | frontend | A panel's picture by name, or 0 (the game hands back a shared stand-in for a missing name; that is checked here) | `PanelQuad* (Panel* panel, const char* name)` | game\_api PanelFindQuad; typed call src/game/game\_api.cpp; calls src/game/menu\_fov.cpp, src/game/menu\_video.cpp, src/game/pc\_menu.cpp | addresses.h; game\_api.h |
| `0x824E2948` | `ProfileEditMenu::Init` (likely) | `sub_824E2948` | frontend | vtbl[0] (this, panel); hooked | `(this, panel)` | hook src/game/menu\_video.cpp | map:medium; addresses.h |
| `0x824E4D78` | `OptionsGameplayMenu::Init` | `OptionsGameplayMenu__Init` | frontend | vtbl[0] (this, panel) | `(this, panel)` | hook src/game/menu\_fov.cpp | map:known; names.toml; addresses.h; ENGINE.md |
| `0x824E56B8` | `MainMenu::Init` (likely) | `sub_824E56B8` | frontend | hooked | `(this, panel)` | hook src/game/pc\_menu.cpp | map:medium; addresses.h; comments |
| `0x824E6020` | `AdvancedOptionsMenu::vfunc_19` | `AdvancedOptionsMenu__vfunc_19` | frontend | vfunc\_19 (this, item): 9 Reset, 10 Back; hooked | `(this, item)` | hook src/game/menu\_fov.cpp | map:high; names.toml; addresses.h |
| `0x824E60E8` | `AdvancedOptionsMenu::Init` (likely) | `sub_824E60E8` | frontend | (this, panel); hooked | `(this, panel)` | hook src/game/menu\_fov.cpp | map:medium; addresses.h; comments |
| `0x824E6588` | `AdvancedOptionsMenu::Refresh` (likely) | `sub_824E6588` | frontend | vtbl[21]; hooked | `(this)` | hook src/game/menu\_fov.cpp | map:medium; addresses.h |
| `0x824E6640` | `PauseMenu::Init` (likely) | `sub_824E6640` | frontend | (this, panel); hooked | `(this, panel)` | hook src/game/pc\_menu.cpp | map:medium; addresses.h; notes: pc-menu; comments |
| `0x824E8098` | `FEMultiLineText::vfunc_91` | `FEMultiLineText__vfunc_91` | frontend | rtti FEMultiLineText vtable 0x82070C84 slot 91 |  | hook src/game/button\_prompts.cpp | map:high; names.toml |
| `0x824E8180` | `FEMultiLineText::vfunc_36` | `FEMultiLineText__vfunc_36` | frontend | rtti FEMultiLineText vtable 0x82070C84 slot 36 |  | hook src/game/button\_prompts.cpp | map:high; names.toml |
| `0x824EBF78` | `MainMenu::vfunc_14` | `MainMenu__vfunc_14` | frontend | vtbl[14]: panel, items, footer; hooked |  | hook src/game/pc\_menu.cpp | map:high; names.toml; addresses.h |
| `0x824ED298` | `ProfileEditMenu::vfunc_20` | `ProfileEditMenu__vfunc_20` | frontend | vtbl[20] (this, how); hooked | `(this, how)` | hook src/game/menu\_video.cpp | map:high; names.toml; addresses.h |
| `0x824EE128` | `AdvancedOptionsMenu::Update` (likely) | `sub_824EE128` | frontend | vtbl[16] (f1 = dt); hooked | `(f1 = dt)` | hook src/game/menu\_fov.cpp | map:medium; addresses.h |
| `0x824EFB18` | `TitleMenu::Update` (likely) | `sub_824EFB18` | frontend | The title page: TitleMenu::Update (hooked) runs every frame; its "PRESS START" is an FEText at +60, set by TitleMenu::Init (0x824D33A8) with SetTextKey ... | `(this, dt)` | hook src/game/button\_prompts.cpp | map:medium; addresses.h; notes: button-prompts, cutscenes; comments |
| `0x824F07B0` | `AdvancedOptionsMenu::vfunc_30` | `AdvancedOptionsMenu__vfunc_30` | frontend | vfunc\_30 (this): B; hooked | `(this)` | hook src/game/menu\_fov.cpp | map:high; names.toml; addresses.h |
| `0x824F0B50` | `PauseMenu::vfunc_19` | `PauseMenu__vfunc_19` | frontend | The item handler (PauseMenu::vfunc\_19, this, item; a jump table): 1 opens Options (InGameMenuSystem vfunc\_7 (1, 0)), 5 the device selector, 6 calls kPauseQuit. ... |  | hook src/game/pc\_menu.cpp | map:high; names.toml; addresses.h |
| `0x8252CB98` | `Localize` | `Localize` | frontend | The text of a string-table key in the game's language; empty when the key isn't there (the game itself would show the key) | `const char* (StringTable* tables, const char* key)` | game\_api Localize; hook src/game/menu\_fov.cpp; typed call src/game/game\_api.cpp; calls src/game/button\_prompts.cpp | map:known; names.toml; addresses.h; game\_api.h |
| `0x824D0470` | `movie::kSkipCheck` | `sub_824D0470` | movies | true when the player skips (any button) |  | mid-asm Cod3MovieSkipHook config/default.toml; mid-asm Cod3MovieSkipped config/default.toml | addresses.h |
| `0x824D0528` | `movie::kStop` | `sub_824D0528` | movies | Stops and frees the playing movie and its sound, and draws the loading screen. Reads no argument | `void ()` | game\_api MovieStop; typed call src/game/game\_api.cpp; calls src/game/cutscene\_skip.cpp | addresses.h; game\_api.h |
| `0x824EEBA0` | `movie::kFinishLevelMovie` | `sub_824EEBA0` | movies | the briefing, after loading (called by SV\_SpawnServer) |  | mid-asm Cod3BriefingLoopBegin config/default.toml; mid-asm Cod3MovieLoopEnd config/default.toml | addresses.h |
| `0x824EECF8` | `movie::kLoadingMovie` | `sub_824EECF8` | movies | the briefing as the loading screen |  | mid-asm Cod3LoadingMovieHook config/default.toml; refers to src/game/cutscene\_skip.cpp | addresses.h |
| `0x824EF308` | `movie::kPlayBlocking` | `sub_824EF308` | movies | play until it ends or is skipped (logos, bonus) |  | mid-asm Cod3MovieLoopBegin config/default.toml; mid-asm Cod3MovieLoopEnd config/default.toml | addresses.h |
| `0x82526240` | `movie::kLoadingScreen` | `sub_82526240` | movies | the plain loading screen |  | refers to src/game/cutscene\_skip.cpp | addresses.h |
| `0x823C0B98` | `ScriptThread_CallEntry` | `ScriptThread_CallEntry` | scripts | script\_threads.cpp: calls a new thread's entry functor |  | hook src/game/script\_threads.cpp | map:known; names.toml; comments; ENGINE.md |
| `0x824A3040` | `ScriptThread_Destroy` | `ScriptThread_Destroy` | scripts | script\_threads.cpp: the thread object's destructor |  | hook src/game/script\_threads.cpp | map:known; names.toml; comments; ENGINE.md |
| `0x824A6150` | `ScriptThread_Run` | `ScriptThread_Run` | scripts | script\_threads.cpp: runs (starts or resumes) one script thread under setjmp |  | mid-asm cod3\_script\_resume config/default.toml; mid-asm cod3\_script\_resume\_prepare config/default.toml | map:known; names.toml; notes: multiplayer, proton-stall; comments; ENGINE.md |
| `0x820CC1D0` | `operator_new` | `operator_new` | system | Game heap memory | `void* (size_t size)` | game\_api OperatorNew; typed call src/game/game\_api.cpp; calls src/game/menu\_fov.cpp, src/game/menu\_video.cpp, src/game/pc\_menu.cpp | map:high; names.toml; addresses.h; game\_api.h; comments |
| `0x820CC468` | `operator_delete` | `operator_delete` | system | (pointer) | `void (void* memory)` | game\_api OperatorDelete; typed call src/game/game\_api.cpp; calls src/game/menu\_fov.cpp, src/game/menu\_video.cpp, src/game/pc\_menu.cpp | map:high; names.toml; addresses.h; game\_api.h; comments |
| `0x8212A100` | `jobs::kWake` | `sub_8212A100` | system | PulseEvent(kWakeEvent); hooked |  | hook src/game/job\_wake.cpp | addresses.h |
| `0x82144CA8` | `nullsub` | `nullsub` | system | an empty method (Options' OnLeft) |  | refers to src/game/pc\_menu.cpp | map:high; names.toml; addresses.h |
| `0x82344F98` | `XamInputGetState_wrapper` (likely) | `sub_82344F98` | system | XInputGetState(user, X\_INPUT\_STATE\*) wrapper the game reads its pads with; hooked (only watched) | `(user, X_INPUT_STATE*)` | hook src/game/button\_prompts.cpp | map:medium; addresses.h |
| `0x82345000` | `SetEvent` (likely) | `sub_82345000` | system | XAPI wrapper: only calls NtSetEvent and BaseSetLastNTError |  | calls src/game/job\_wake.cpp | map:medium |
| `0x8234EEC0` | `longjmp` | `longjmp_guest` | system | config/default.toml / script\_threads.cpp |  | hook src/game/script\_threads.cpp | map:known; names.toml; comments |
| `0x82351CC0` | `setjmp` | `setjmp_guest` | system | config/default.toml setjmp\_address |  | setjmp\_address config/default.toml | map:known; names.toml |
| `0x82430210` | `sub_82430210` | `sub_82430210` | other |  |  | mid-asm Cod3SceneAspectHook config/default.toml |  |
| `0x82462FB0` | `sub_82462FB0` | `sub_82462FB0` | other |  |  | hook src/game/watchdog.cpp |  |
| `0x82525190` | `sub_82525190` | `sub_82525190` | other |  |  | hook src/game/watchdog.cpp |  |

## Mid-function hooks (44)

| Address | Module | In function | Hook | Handler | Config |
|---|---|---|---|---|---|
| `0x82430340` | default.xex | `sub_82430210` (+0x130) | `Cod3SceneAspectHook` | src/game/ultrawide.cpp | config/default.toml |
| `0x824396D4` | default.xex | `CG_DrawMountedCrosshair` (+0x1A4) | `Cod3MountedCrosshairHook` | src/ui/hud\_overlay.cpp | config/default.toml |
| `0x82444BB8` | default.xex | `CG_DrawCrosshair` (+0x3F0) | `Cod3CrosshairHook` | src/ui/hud\_overlay.cpp | config/default.toml |
| `0x824A63AC` | default.xex | `ScriptThread_Run` (+0x25C) | `cod3_script_resume_prepare` | src/game/script\_threads.cpp | config/default.toml |
| `0x824A6480` | default.xex | `ScriptThread_Run` (+0x330) | `cod3_script_resume` | src/game/script\_threads.cpp | config/default.toml |
| `0x824D04E4` | default.xex | `movie::kSkipCheck` (+0x74) | `Cod3MovieSkipped` | src/game/cutscene\_skip.cpp | config/default.toml |
| `0x824D050C` | default.xex | `movie::kSkipCheck` (+0x9C) | `Cod3MovieSkipHook` | src/game/cutscene\_skip.cpp | config/default.toml |
| `0x824DBC0C` | default.xex | `OptionsGameplayMenu::ctor` (+0x1C) | `Cod3GameplayItemCount` | src/game/menu\_fov.cpp | config/default.toml |
| `0x824EEC08` | default.xex | `movie::kFinishLevelMovie` (+0x68) | `Cod3BriefingLoopBegin` | src/game/cutscene\_skip.cpp | config/default.toml |
| `0x824EECD0` | default.xex | `movie::kFinishLevelMovie` (+0x130) | `Cod3MovieLoopEnd` | src/game/cutscene\_skip.cpp | config/default.toml |
| `0x824EECF8` | default.xex | `movie::kLoadingMovie` (+0x0) | `Cod3LoadingMovieHook` | src/game/cutscene\_skip.cpp | config/default.toml |
| `0x824EF438` | default.xex | `movie::kPlayBlocking` (+0x130) | `Cod3MovieLoopBegin` | src/game/cutscene\_skip.cpp | config/default.toml |
| `0x824EF518` | default.xex | `movie::kPlayBlocking` (+0x210) | `Cod3MovieLoopEnd` | src/game/cutscene\_skip.cpp | config/default.toml |
| `0x82536EE0` | default.xex | `Com_Frame` (+0x110) | `Cod3FrameLimiterHook` | src/game/frame\_rate.cpp | config/default.toml |

- `cod3_script_yield_returned` (src/game/level\_hooks.cpp): 30 sites in 15 level DLLs (`config/<level>.toml`): blkbrn, chambois, credits, crssrds, falaise, forest, fuelplnt, hostage, island, laison, mace2, mayenne, nightd, saint_lo, stbert.

## All named functions by subsystem

[Core: dvars, commands, files, printing (qcommon)](#core-dvars-commands-files-printing-qcommon-172) (172) · [Client: input, connection, screen](#client-input-connection-screen-138) (138) · [Client game (cgame): view and HUD](#client-game-cgame-view-and-hud-64) (64) · [Server](#server-55) (55) · [Game: entities, rules, cheats](#game-entities-rules-cheats-70) (70) · [Shared game code and math (bg, pmove)](#shared-game-code-and-math-bg-pmove-32) (32) · [Renderer and ngl](#renderer-and-ngl-42) (42) · [Assets and archives](#assets-and-archives-51) (51) · [Front end: menus and message boxes](#front-end-menus-and-message-boxes-558) (558) · [Movies and loading screens](#movies-and-loading-screens-7) (7) · [In-game overlay widgets (IGO)](#in-game-overlay-widgets-igo-112) (112) · [Interactions](#interactions-135) (135) · [Effects and animation](#effects-and-animation-152) (152) · [Level scripts (Broc, script threads)](#level-scripts-broc-script-threads-98) (98) · [World: scene, entities, AI paths, vegetation](#world-scene-entities-ai-paths-vegetation-34) (34) · [System: kernel and XAM imports, C runtime, jobs](#system-kernel-and-xam-imports-c-runtime-jobs-279) (279) · [Other classes (from RTTI)](#other-classes-from-rtti-56) (56) · [Other](#other-9) (9)

### Core: dvars, commands, files, printing (qcommon) (172)

| Address | Name | What it does | Arguments | Port use | Sources |
|---|---|---|---|---|---|
| `0x820A0118` | `NameHash_Init` (likely) | fills a 32-byte {djb2 hash of the lower-cased text, text[28]} record from a C string |  |  | medium |
| `0x820CDE10` | `Q_stricmpn` | (s1, s2, n) called with n = 99999 by CG\_ConsoleCommand (Q3 Q\_stricmp = Q\_stricmpn(s1, s2, 99999)) | `(s1, s2, 99999)` |  | high; names.toml |
| `0x820CEBB8` | `NameHash` (likely) | NameHash\_Init on the stack and returns the hash; 706 callers pass names like 'tag\_flash', 'end' |  |  | medium |
| `0x82127800` | `Sys_PerformanceCounter` (likely) | returns QueryPerformanceCounter's 64-bit value; Sys\_Milliseconds and the per-subsystem frame timers ... |  |  | medium |
| `0x823FACA0` | `Sys_GetEvent` | returns the next queued event (64-entry ring, 24-byte events) or an empty one stamped with ... |  |  | high; names.toml |
| `0x823FAD70` | `Sys_QueEvent` (likely) | writes the next slot of Sys\_GetEvent's 64-entry ring (frees the overwritten evPtr, Q3) with ... |  |  | medium |
| `0x823FBA30` | `Sys_DirectoryHasContents` (likely) | '%s\\\*' search skipping '.', '..' and 'CVS'; FS\_AddGameDirectory checks a folder with it |  |  | medium |
| `0x823FBE78` | `Sys_ListFilteredFiles` | '%s\\%s\\\*' / '%s\\\*' searches, skips '.', '..' and 'CVS', recurses into subdirectories and filters ... |  |  | high; names.toml |
| `0x823FC6E8` | `Sys_ListFiles` | '%s\\\*%s' search with the extension, CopyString of each name into a Z\_Malloc'd list (Q3 win\_main.c) |  |  | high; names.toml |
| `0x823FD210` | `Sys_Init` | Cvar\_Set 'arch' 'ps2', Cvar\_SetValue 'sys\_cpuid', then IN\_Init (Q3 order; Com\_Init has an inlined ... |  |  | high; names.toml |
| `0x82400098` | `Sys_Main` | the image's main(): picks the video mode (R\_SelectVideoMode, R\_SetMultisample), calls Com\_Init once ... |  |  | high; names.toml; ENGINE.md |
| `0x82451568` | `Com_sprintf` | (dest, size, fmt, ...): Cvar\_SetValue formats '%i' / '%f' into a 32-byte buffer with it (Q3) |  |  | high; names.toml |
| `0x824515D8` | `Q_strlwr` | tolower over the string in place, returns it (Q3) |  |  | high; names.toml |
| `0x82451900` | `COM_DefaultExtension` | Cmd\_Exec\_f adds '.cfg' with it; Com\_sprintf '%s%s' when the name has no extension (Q3) |  |  | high; names.toml |
| `0x82451C68` | `NET_SendLoopPacket` | copies a packet into the next loopback slot (3076-byte messages, send counter +12308) (Q3) |  |  | high; names.toml |
| `0x82451D00` | `NET_GetLoopPacket` | loopbacks[sock] (12312 bytes: 4 x 3076-byte messages), get/send counters, copies one message and ... |  |  | high; names.toml |
| `0x82451DB0` | `NET_AdrToString` | 'loopback', '%i.%i.%i.%i:%i' and the IPX format (Q3); Netchan\_Process and CL\_PacketEvent print with ... |  |  | high; names.toml |
| `0x82451F48` | `MSG_ReadString` | reads a NUL-terminated string at msg-&gt;readcount into a static buffer (empty on overflow) ... |  |  | high; names.toml |
| `0x824531B8` | `Info_RemoveKey_Big` | string 'Info\_RemoveKey\_Big: oversize infostring' (only user) |  |  | high; names.toml |
| `0x82453338` | `Info_RemoveKey` | string 'Info\_RemoveKey: oversize infostring' (only user) |  |  | high; names.toml |
| `0x824534B0` | `va` | string 'overrun string in call to va()'; formats into a static buffer |  |  | high; names.toml; ENGINE.md |
| `0x82453580` | `Q_stricmp` | Q\_stricmpn with n = 99999 inlined: case-folding compare (Q3) |  |  | high; names.toml |
| `0x824535F8` | `Q_strcat` | string 'Q\_strcat: already overflowed' (only user) |  |  | high; names.toml |
| `0x824536D8` | `Swap_Init` | short {1,0} byte-order test choosing the Big/Little Short/Long/Float function pointers (Q3 ... |  |  | high; names.toml |
| `0x824537D8` | `Com_ParseExt` | string 'Com\_ParseExt: NULL data\_p' (only user) |  |  | high; names.toml |
| `0x82453CD8` | `Com_EndParseSession` | 'Com\_EndParseSession: session underflow' (the other user inlines it) |  |  | high; names.toml |
| `0x82453D40` | `Com_BeginParseSession` | string 'Com\_BeginParseSession: session overflow' (only user) |  |  | high; names.toml |
| `0x82453DC8` | `NET_SendPacket` | 'send packet %4i' under showpackets, then NET\_SendLoopPacket (Q3 net\_chan.c) |  |  | high; names.toml |
| `0x82453E28` | `NET_CompareAdrSigned` | string 'NET\_CompareAdrSigned: bad address type' (only user) |  |  | high; names.toml |
| `0x82453F28` | `Netchan_Process` | Q3 Netchan\_Process: 3 strings in common, e.g. '%s:Out of order packet %i at %i' |  |  | high; names.toml |
| `0x824540B0` | `Netchan_Transmit` | string 'Netchan\_Transmit: length = %i' (only user) |  |  | high; names.toml |
| `0x82454280` | `MSG_WriteString` | string 'MSG\_WriteString: MAX\_STRING\_CHARS' (only user) |  |  | high; names.toml |
| `0x824571F8` | `Info_SetValueForKey_Big` | calls Info\_RemoveKey\_Big (Q3's \_Big variant shares the 'Info\_SetValueForKey: oversize infostring' ... |  |  | high; names.toml |
| `0x82457408` | `Info_SetValueForKey` | the backslash / semicolon / quote key checks, Info\_RemoveKey, Com\_sprintf '\\%s\\%s' (Q3) ... |  |  | high; names.toml |
| `0x82457570` | `Info_ValueForKey` | string 'Info\_ValueForKey: oversize infostring' (only user) |  |  | high; names.toml; ENGINE.md |
| `0x824577C0` | `NET_OutOfBandPrint` | vsprintf after the -1 header, 'send packet' under showpackets, NET\_SendLoopPacket; SV\_PacketEvent ... |  |  | high; names.toml |
| `0x82463F30` | `Cmd_List_f` | cmd 'cmdlist' (Cmd\_AddCommand from 0x82488370); Q3 handler name |  |  | high; names.toml |
| `0x82463F38` | `Cmd_TokenizeString` | fills cmd\_argc / cmd\_argv; Cmd\_ExecuteString, Com\_StartupVariable and Com\_SafeMode tokenize with it ... |  |  | high; names.toml |
| `0x82464150` | `Cmd_Args` | joins cmd\_argv[1..cmd\_argc) with ' ' into a static buffer; CL\_ForwardToServer\_f sends it (Q3) |  |  | high; names.toml |
| `0x82464208` | `Cmd_Argv` | returns cmd\_argv[arg] or "" when arg &gt;= cmd\_argc (Q3) |  |  | high; names.toml |
| `0x82464238` | `Cmd_Argc` | returns cmd\_argc (Q3) |  |  | high; names.toml |
| `0x82464408` | `Cbuf_InsertText` | void Cbuf\_InsertText(const char\* text) | `void (const char* text)` |  | known; names.toml; addresses.h |
| `0x824644D0` | `Cbuf_AddText` | Appends console text; it runs at the next Cbuf\_Execute (once a frame). End each command with '\\n'. ... | `void (const char* text)` | game\_api; typed call game\_api.cpp | known; names.toml; addresses.h; game\_api.h; comments |
| `0x824645B0` | `Cmd_Wait_f` | cmd 'wait' (Cmd\_AddCommand from 0x82488370); Q3 handler name |  |  | high; names.toml |
| `0x824679A8` | `Cmd_Clearallaliases_f` | cmd 'clearallaliases' (Cmd\_AddCommand from 0x82471FD8) |  |  | high; names.toml |
| `0x82467A00` | `Cmd_Stickalias_f` | cmd 'stickalias' (Cmd\_AddCommand from 0x82471FD8) |  |  | high; names.toml |
| `0x82467BB8` | `Cmd_Buttonalias_f` | cmd 'buttonalias' (Cmd\_AddCommand from 0x82471FD8) |  |  | high; names.toml |
| `0x82468510` | `Sys_Milliseconds` | CL\_ScaledMilliseconds = this \* com\_timescale-&gt;value (Q3) |  |  | high; names.toml |
| `0x82468630` | `Cmd_ArgvBuffer` | (arg, buffer, length): strncpy of cmd\_argv[arg]; CG\_ConsoleCommand and ClientCommand fetch argv 0 ... |  |  | high; names.toml |
| `0x824686A8` | `Cmd_Echo_f` | cmd 'echo' (Cmd\_AddCommand from 0x82488370); Q3 handler name |  |  | high; names.toml |
| `0x8246CAA0` | `Cmd_AddButtonCommand` (likely) | Cmd\_AddCommand's body storing type 2; used only for the +/- button commands |  |  | medium |
| `0x8246CBA8` | `Cmd_AddCommand` | void Cmd\_AddCommand(const char\* name, xcommand\_t function) | `void (const char* name, xcommand_t function)` |  | known; names.toml; addresses.h; ENGINE.md |
| `0x8246CCB0` | `Cmd_AddServerCommand` | string 'Cmd\_AddServerCommand: %s already defined' (only user) |  |  | high; names.toml |
| `0x824799C0` | `Cmd_RemoveCommand` | unlinks and frees the named entry of cmd\_functions; CL\_Shutdown and CL\_ShutdownInput remove their ... |  |  | high; names.toml |
| `0x82479A88` | `Cmd_Vstr_f` | cmd 'vstr' (Cmd\_AddCommand from 0x82488370); Q3 handler name; also Q3 Cmd\_Vstr\_f: 1 strings in ... |  |  | high; names.toml |
| `0x8247E228` | `Cmd_ExecuteString` | void Cmd\_ExecuteString(const char\* text) | `void (const char* text)` |  | known; names.toml; addresses.h; ENGINE.md |
| `0x8247E380` | `Cbuf_Execute` | void Cbuf\_Execute(void) - run once per frame by Com\_Frame | `void (void)` | hook engine.cpp | known; names.toml; addresses.h; ENGINE.md |
| `0x8247E498` | `Cbuf_ExecuteText` | void Cbuf\_ExecuteText(int exec\_when, const char\* text) | `void (int exec_when, const char* text)` |  | known; names.toml; addresses.h |
| `0x82485708` | `Cmd_Exec_f` | Q3 Cmd\_Exec\_f: 3 strings in common, e.g. 'exec &lt;filename&gt; : execute a script file' |  |  | high; names.toml |
| `0x82488370` | `Cmd_Init` | adds cmdlist/exec/vstr/echo/wait, exactly Q3 Cmd\_Init |  |  | high; names.toml |
| `0x82497450` | `VM_Call` | (vm, callnum, ...): saves currentVM (0x82A2A2BC), copies 16 varargs and calls vm-&gt;entryPoint (+136) ... |  |  | high; names.toml; ENGINE.md |
| `0x82497540` | `VM_DllSyscall` | VM\_Create stores it as the game's and cgame's syscall pointer; forwards &arg to ... |  |  | high; names.toml |
| `0x824BB4D8` | `VM_Create` | string 'VM\_Create: bad parms' (only user) |  |  | high; names.toml |
| `0x825091C0` | `Cmd_Unbindallaxis_f` | cmd 'unbindallaxis' (Cmd\_AddCommand from 0x8250C0B0) |  |  | high; names.toml |
| `0x8250A6E0` | `Cmd_Bindaxis_f` | cmd 'bindaxis' (Cmd\_AddCommand from 0x8250C0B0) |  |  | high; names.toml |
| `0x825235D0` | `Cvar_ValidateString` | strchr for backslash, quote and semicolon; Cvar\_Get and Cvar\_Set2 reject names with them (Q3) |  |  | high; names.toml |
| `0x82523808` | `Com_Writedefaults_f` | cmd 'writedefaults' (Cmd\_AddCommand from 0x825372E8) |  |  | high; names.toml |
| `0x82523810` | `Com_WriteConfig_f` | cmd 'writeconfig' (Cmd\_AddCommand from 0x825372E8); Q3 handler name |  |  | high; names.toml |
| `0x82523828` | `Com_Crash_f` | cmd 'crash' (Cmd\_AddCommand from 0x825372E8); Q3 handler name |  |  | high; names.toml |
| `0x82523880` | `Com_AddStartupCommands` | Cbuf\_AddText of each startup line and '\\n', skipping Q\_stricmpn(line, 'set', 3) ones (Q3) |  |  | high; names.toml |
| `0x82523920` | `Com_ParseCommandLine` | Com\_Init: stores the command line and splits it at '+' / newline into com\_consoleLines (max 32) ... |  |  | high; names.toml |
| `0x825239A0` | `Com_Error` | void Com\_Error(int code, const char\* fmt, ...) - retail just hits DbgBreakPoint | `void (int code, const char* fmt, ...)` | hook print\_hooks.cpp | known; names.toml; addresses.h; ENGINE.md |
| `0x82523A58` | `Com_Printf` | Empty in the retail game; the port's hook (game/print\_hooks.cpp) shows the line in the console and ... | `void (const char* fmt, ...)` | game\_api; hook print\_hooks.cpp; typed call game\_api.cpp | known; names.toml; addresses.h; game\_api.h; comments; ENGINE.md |
| `0x82523A60` | `Com_DPrintf` | void Com\_DPrintf(const char\* fmt, ...) - body compiled out in retail | `void (const char* fmt, ...)` | hook print\_hooks.cpp | known; names.toml; addresses.h; ENGINE.md |
| `0x82523AC8` | `Z_Malloc` (likely) | operator new + memset 0, fatal 'out of memory' on failure; CopyString's allocator |  |  | medium |
| `0x82523B38` | `Com_StringContains` | substring search with optional case folding by toupper; Com\_Filter's helper (Q3) |  |  | high; names.toml |
| `0x82524E50` | `COM_GetExtension` (likely) | returns the text after the last '.' of the last path component, or ""; FS\_FOpenFileRead's extension ... |  |  | medium |
| `0x82524EB0` | `FS_FilenameCompare` | case-folding compare treating backslash and ':' as '/', -1 on a difference (Q3) |  |  | high; names.toml |
| `0x82524F98` | `FS_HandleForFile` | string 'FS\_HandleForFile: none free' (only user) |  |  | high; names.toml |
| `0x82525038` | `FS_HashFileName` | tolower, stops at '.', backslash as '/', hash += c \* (i + 119), folded with &gt;&gt; 10 / &gt;&gt; 20 and ... |  |  | high; names.toml |
| `0x82526040` | `Cvar_Update` | string 'Cvar\_Update: handle out of range' (only user) |  |  | high; names.toml |
| `0x82526148` | `generateHashValue` | string 'null name in generateHashValue'; tolower hash used by Cvar\_FindVar and Cvar\_Get (Q3 cvar.c) |  |  | high; names.toml |
| `0x825262C8` | `Com_Error_f` | cmd 'error' (Cmd\_AddCommand from 0x825372E8); Q3 handler name |  |  | high; names.toml |
| `0x82526328` | `Com_PushEvent` | Com\_Milliseconds pushes each non-empty event through it (Q3) |  |  | high; names.toml |
| `0x825263F0` | `Com_SafeMode` | scans the console lines for 'safe' and 'cvar\_restart' (Q3); Com\_Init execs safemode.cfg when true |  |  | high; names.toml |
| `0x82526570` | `CopyString` | allocates and copies the name in Cvar\_Get, Cvar\_Set2 and Cmd\_AddCommand (Q3 CopyString) |  |  | high; names.toml |
| `0x825265E0` | `Com_Filter` | wildcard match ('\*', '?', '[...]') through Com\_StringContains and toupper (Q3) |  |  | high; names.toml |
| `0x82527118` | `FS_FreeFileList` | frees each name then the list; refimport slot after FS\_ListFiles (Q3) |  |  | high; names.toml |
| `0x82527180` | `FS_AddFileToList` | Q\_stricmp against the names already listed, else CopyString into the list (Q3) |  |  | high; names.toml |
| `0x82527228` | `FS_FreeFile` | string 'FS\_FreeFile( NULL )' (only user) | `(NULL)` |  | high; names.toml |
| `0x82527280` | `FS_Write` | string 'FS\_Write: -1 bytes written' (only user) |  |  | high; names.toml |
| `0x82527348` | `FS_Read` | string 'FS\_Read: -1 bytes read' (only user) |  |  | high; names.toml |
| `0x825274D8` | `FS_FCloseFile` | SV\_SetExpectedHunkUsage closes hunkusage.dat with it after FS\_Read; FS\_ReadFile calls it twice (Q3) |  |  | high; names.toml |
| `0x82527560` | `FS_CreatePath` (likely) | Q3 FS\_CreatePath: 1 strings in common, e.g. 'WARNING: refusing to create relative path "%s"' |  |  | medium |
| `0x82527610` | `FS_BuildOSPath` | string 'FS\_BuildOSPath: os path length exceeded' (only user) |  |  | high; names.toml |
| `0x82527780` | `FS_filelength` | ftell, fseek to the end, ftell, fseek back (or the zip entry's length); FS\_FOpenFileRead's result ... |  |  | high; names.toml |
| `0x82529690` | `Cvar_InfoString_Big` | Info\_SetValueForKey\_Big of every cvar with the flag bit; SV\_SetConfigstring(1 ... | `(8)` |  | high; names.toml |
| `0x82529710` | `Cvar_InfoString` | Info\_SetValueForKey of every cvar with the flag bit; SV\_SetConfigstring(0, Cvar\_InfoString(4)) (Q3) | `(4)` |  | high; names.toml |
| `0x82529790` | `Cvar_FindVar` | The dvar, or 0 | `cvar_t* (const char* name)` | game\_api; typed call game\_api.cpp | known; names.toml; addresses.h; game\_api.h; comments |
| `0x82529820` | `Com_Shutdown` | Com\_Quit\_f's step after SV\_Shutdown / CL\_Shutdown: closes the log and journal files with ... |  |  | high; names.toml |
| `0x825298D8` | `Com_ModifyMsec` | Com\_Frame passes the frame msec: fixedtime override, scaled by com\_timescale, at least 1 when ... |  |  | high; names.toml; ENGINE.md |
| `0x82529988` | `Com_GetRealEvent` | Com\_Milliseconds' event source (Q3) |  |  | high; names.toml |
| `0x82529D18` | `Com_FilterPath` | Sys\_ListFilteredFiles' filter: path separators folded, then Com\_Filter (Q3) |  |  | high; names.toml |
| `0x8252A370` | `FS_CopyFile` | Q3 FS\_CopyFile: 2 strings in common, e.g. 'Short write in FS\_Copyfiles()' |  |  | high; names.toml |
| `0x8252B718` | `Cvar_VariableStringBuffer` | Cvar\_FindVar then strncpy of -&gt;string into the buffer (Q3) |  |  | high; names.toml |
| `0x8252B780` | `Cvar_VariableString` | Cvar\_FindVar, returns -&gt;string (+4) or "" (Q3) |  |  | high; names.toml |
| `0x8252B7C8` | `Cvar_VariableIntegerValue` | Cvar\_FindVar, returns -&gt;integer (+32) or 0 (Q3) |  |  | high; names.toml |
| `0x8252B808` | `Cvar_VariableValue` | Cvar\_FindVar, returns -&gt;value (+28) or 0.0 (Q3) |  |  | high; names.toml |
| `0x8252B850` | `Cvar_Set2` | The game's own write with its checks (force skips them). Port code writes dvars with ... | `cvar_t* (const char* name, const char* value, qboolean force)` | game\_api; hook cheat\_watch.cpp; typed call game\_api.cpp | known; names.toml; addresses.h; game\_api.h; comments; ENGINE.md |
| `0x8252BBE0` | `Cvar_Get` | Registers the dvar with this default (or returns the one that exists) | `cvar_t* (const char* name, const char* value, int flags)` | game\_api; hook cheat\_guard.cpp; typed call game\_api.cpp | known; names.toml; addresses.h; game\_api.h; notes: multiplayer; comments; ENGINE.md |
| `0x8252BF88` | `Com_Milliseconds` | pulls 24-byte events with Com\_GetRealEvent and Com\_PushEvent until an empty one, returns its time ... |  |  | high; names.toml |
| `0x8252BFF8` | `Com_GetEvent` | Com\_EventLoop's event source; falls back to Com\_GetRealEvent when nothing is pushed (Q3) |  |  | high; names.toml |
| `0x8252C0B8` | `FS_AddGameDirectory` | adds a search path: 'localized' / 'non-localized', 'WARNING: game folder %s/%s added as both ... |  |  | high; names.toml |
| `0x8252C330` | `FS_FOpenFileRead` | string 'FS\_FOpenFileRead: %s (found in '%s/%s')' (only user) |  |  | high; names.toml |
| `0x8252C850` | `FS_FOpenFileAppend` | string 'FS\_FOpenFileAppend: %s' (only user) |  |  | high; names.toml |
| `0x8252C940` | `FS_FOpenFileWrite` | string 'FS\_FOpenFileWrite: %s' (only user) |  |  | high; names.toml |
| `0x8252CA38` | `FS_FileExists` | FS\_BuildOSPath(fs\_homepath, ...) then fopen 'rb' / fclose (Q3) |  |  | high; names.toml |
| `0x8252D790` | `Cvar_Setfromcvar_f` | cmd 'setfromcvar' (Cmd\_AddCommand from 0x825326E8) |  |  | high; names.toml |
| `0x8252D828` | `Cvar_Set_f` | cmd 'set' (Cmd\_AddCommand from 0x825326E8); Q3 handler name; also Q3 Cvar\_Set\_f: 1 strings in ... |  |  | high; names.toml |
| `0x8252D998` | `Cvar_Toggle_f` | cmd 'toggle' (Cmd\_AddCommand from 0x825326E8); Q3 handler name |  |  | high; names.toml |
| `0x8252DB38` | `Cvar_Command` (likely) | Q3 Cvar\_Command: 1 strings in common, e.g. 'latched: "%s"' |  |  | medium |
| `0x8252DBF8` | `Cvar_Register` | Cvar\_Get then vmCvar handle and Cvar\_Update (Q3) |  |  | high; names.toml; ENGINE.md |
| `0x8252DC70` | `Cvar_Reset` | Cvar\_Set2(name, NULL, 0) (Q3) |  |  | high; names.toml |
| `0x8252DC80` | `Cvar_Set` | Cvar\_Set2(name, value, 1); the refimport's Cvar\_Set slot after Cvar\_FindVar in CL\_InitRef (Q3) |  |  | high; names.toml |
| `0x8252DC88` | `Com_LoadCvarsFromBuffer` (likely) | parses 'name value' pairs with Com\_ParseExt and Cvar\_Set2, "WARNING: unknown cvar '%s'" and the ... |  |  | medium |
| `0x8252E128` | `Com_Freeze_f` | cmd 'freeze' (Cmd\_AddCommand from 0x825372E8); Q3 handler name |  |  | high; names.toml |
| `0x8252E1C0` | `Com_RunAndTimeServerPacket` | SV\_PacketEvent bracketed by com\_speeds timing, prints 'SV\_PacketEvent time: %i' at com\_speeds 3 (Q3) |  |  | high; names.toml |
| `0x8252E248` | `Com_StartupVariable` | Com\_Init calls it with NULL and 'developer': tokenizes each console line, 'set' -&gt; Cvar\_Set2 / ... |  |  | high; names.toml |
| `0x8252EC20` | `FS_Shutdown` | removes 'path', 'fullpath', 'dir', 'fdir', 'touchFile' and frees the search paths; Com\_Quit\_f ... |  |  | high; names.toml |
| `0x8252ED38` | `FS_AddLocalizedGameDirectory` (likely) | FS\_Startup's per-directory call: FS\_AddGameDirectory for the localized and the plain folder |  |  | medium |
| `0x8252ED90` | `FS_ListFilteredFiles` | FS\_BuildOSPath per search path, Sys\_ListFiles, FS\_AddFileToList into a Z\_Malloc'd list (Q3) |  |  | high; names.toml |
| `0x82530948` | `Cvar_Restart_f` | cmd 'cvar\_restart' (Cmd\_AddCommand from 0x825326E8); Q3 handler name |  |  | high; names.toml |
| `0x82530A08` | `Cvar_Reset_f` | cmd 'reset' (Cmd\_AddCommand from 0x825326E8); Q3 handler name; also Q3 Cvar\_Reset\_f: 1 strings in ... |  |  | high; names.toml |
| `0x82530A68` | `Cvar_SetA_f` | cmd 'seta' (Cmd\_AddCommand from 0x825326E8); Q3 handler name; also Q3 Cvar\_SetA\_f: 1 strings in ... |  |  | high; names.toml |
| `0x82530AF8` | `Cvar_SetS_f` | cmd 'sets' (Cmd\_AddCommand from 0x825326E8); Q3 handler name; also Q3 Cvar\_SetS\_f: 1 strings in ... |  |  | high; names.toml |
| `0x82530B88` | `Cvar_SetCheatState` | every cvar with the cheat flag (0x200) whose string differs from its reset string is set back with ... |  |  | high; names.toml |
| `0x82530C20` | `Cvar_SetValue` | Com\_sprintf '%i' or '%f' then Cvar\_Set2(name, buf, 1) (Q3) |  |  | high; names.toml |
| `0x82530D00` | `Com_InitJournaling` | 'journal' cvar, journal.dat / journaldata.dat, 'Journaling events' (Q3) |  |  | high; names.toml |
| `0x82531E10` | `FS_FOpenFileByMode` (likely) | Q3 FS\_FOpenFileByMode: 1 strings in common, e.g. 'FSH\_FOpenFile: bad mode' |  |  | medium |
| `0x82531F80` | `FS_Startup` (likely) | Q3 FS\_Startup: 1 strings in common, e.g. '----- FS\_Startup -----' |  |  | medium |
| `0x82532340` | `FS_ListFiles` | FS\_ListFilteredFiles(path, extension, NULL, numfiles); refimport slot between FS\_FreeFile and ... |  |  | high; names.toml |
| `0x82532350` | `FS_ReadFile` | Q3 FS\_ReadFile: 5 strings in common, e.g. 'Writing zero for %s to journal file.' |  |  | high; names.toml; ENGINE.md |
| `0x825326E8` | `Cvar_Init` (likely) | Q3 Cvar\_Init: 4 cvar/key names in common, e.g. 'toggle' |  |  | medium |
| `0x82532BB8` | `FS_Restart` (likely) | Q3 FS\_Restart: 1 strings in common, e.g. 'Invalid game folder' |  |  | medium |
| `0x82532D28` | `FS_InitFilesystem` | Com\_StartupVariable for fs\_cdpath, fs\_basepath, fs\_homepath, fs\_game..., FS\_Startup, then ... |  |  | high; names.toml |
| `0x82535E80` | `Com_EventLoop` | frame\_rate notes (Com\_Frame's wait loop) |  |  | known; names.toml; ENGINE.md |
| `0x82536610` | `Com_Quit_f` | cmd 'quit' (Cmd\_AddCommand from 0x825372E8); Q3 handler name |  |  | high; names.toml |
| `0x82536DD0` | `Com_Frame` | the frame boundary; hooked |  | hook render\_resolution.cpp; mid-asm Cod3FrameLimiterHook default.toml | known; names.toml; addresses.h; comments; ENGINE.md |
| `0x825372E8` | `Com_Init` | '--- Common Initialization Complete ---'; registers developer and com\_maxfps, adds ... |  |  | high; names.toml; ENGINE.md |
| `0x82539730` | `Cmd_Thread_f` | cmd 'thread' (command table 0x825F49F0) |  |  | high; names.toml |
| `0x82539738` | `Cmd_Texturetiling_f` | cmd 'texturetiling' (command table 0x825F49F0) |  |  | high; names.toml |
| `0x82539770` | `Cmd_Texturesize_f` | cmd 'texturesize' (command table 0x825F49F0) |  |  | high; names.toml |
| `0x825397A8` | `Cmd_Solidcolor_f` | cmd 'solidcolor' (command table 0x825F49F0) |  |  | high; names.toml |
| `0x825397E0` | `Cmd_Fullbright_f` | cmd 'fullbright' (command table 0x825F49F0) |  |  | high; names.toml |
| `0x82539818` | `Cmd_Wireframe_f` | cmd 'wireframe' (command table 0x825F49F0) |  |  | high; names.toml |
| `0x82539850` | `Cmd_Nglfpsdisplay_f` | cmd 'nglfpsdisplay' (command table 0x825F49F0) |  |  | high; names.toml |
| `0x82539878` | `Cmd_Profilenodes_f` | cmd 'profilenodes' (command table 0x825F49F0) |  |  | high; names.toml |
| `0x82539880` | `Cmd_Profileshaders_f` | cmd 'profileshaders' (command table 0x825F49F0) |  |  | high; names.toml |
| `0x82539888` | `Cmd_Nglstatdisplay_f` | cmd 'nglstatdisplay' (command table 0x825F49F0) |  |  | high; names.toml |
| `0x825398B0` | `Cmd_Nglstats_f` | cmd 'nglstats' (command table 0x825F49F0) |  |  | high; names.toml |
| `0x8253CBF0` | `Cmd_EntityStats_f` | cmd 'entity\_stats' (command table 0x825F49F0) |  |  | high; names.toml |
| `0x8253CC78` | `Cmd_Toggleshader_f` | cmd 'toggleshader' (command table 0x825F49F0) |  |  | high; names.toml |
| `0x8253CCE0` | `Cmd_Texturemip_f` | cmd 'texturemip' (command table 0x825F49F0) |  |  | high; names.toml |
| `0x8253CD28` | `Cmd_Setspawnpt_f` | cmd 'setspawnpt' (command table 0x825F49F0) |  |  | high; names.toml |
| `0x8253FE98` | `Cmd_Invinc_f` | addresses.h ('invinc', an empty function) |  |  | known; names.toml |
| `0x82542E20` | `Cmd_Mr_f` | cmd 'mr' (command table 0x825F49F0) |  |  | high; names.toml |
| `0x82543158` | `Cmd_Where_f` | cmd 'where' (command table 0x825F49F0); Q3 handler name |  |  | high; names.toml |
| `0x82545378` | `Cmd_Take_f` | cmd 'take' (command table 0x825F49F0) |  |  | high; names.toml |
| `0x825457B0` | `Cmd_Fogswitch_f` | cmd 'fogswitch' (command table 0x825F49F0) |  |  | high; names.toml |
| `0x8254E080` | `Cmd_Killsound_f` | cmd 'killsound' (command table 0x825F49F0) |  |  | high; names.toml |
| `0x8255F410` | `Cmd_Dropweapon_f` | cmd 'dropweapon' (command table 0x825F49F0) |  |  | high; names.toml |
| `0x8255F488` | `Cmd_Kill_f` | cmd 'kill' (command table 0x825F49F0); Q3 handler name |  |  | high; names.toml |

### Client: input, connection, screen (138)

| Address | Name | What it does | Arguments | Port use | Sources |
|---|---|---|---|---|---|
| `0x82509558` | `CL_ParseCommandString` | CL\_ParseServerMessage's svc\_serverCommand (5) case: MSG\_ReadString into the server command ring (Q3) |  |  | high; names.toml |
| `0x82509680` | `CL_ShutdownRef` | calls re.Shutdown through the refexport then zeroes re (Q3) |  |  | high; names.toml |
| `0x825097B0` | `CL_Disconnect_f` | cmd 'disconnect' (Cmd\_AddServerCommand from 0x82511398); Q3 handler name |  |  | high; names.toml |
| `0x825097B8` | `CL_Setenv_f` | cmd 'setenv' (Cmd\_AddCommand from 0x82511398); Q3 handler name |  |  | high; names.toml |
| `0x825097C0` | `CL_AddReliableCommand` | copies the text into the next reliable-command slot; CL\_ForwardToServer\_f / ... |  |  | high; names.toml |
| `0x82509A48` | `CL_EnableAsserts_f` | cmd 'EnableAsserts' (Cmd\_AddCommand from 0x8250EDB8) |  |  | high; names.toml |
| `0x82509A90` | `CL_FinishMove` | cmd serverTime / weapon and ANGLE2SHORT of cl.viewangles (x 65536/360 + 0.5); CL\_CreateCmd's last ... |  |  | high; names.toml |
| `0x82509BB8` | `CL_CmdButtons` | sets cmd-&gt;buttons bit i for each in\_buttons[i] active or wasPressed and clears wasPressed, then the ... |  |  | high; names.toml |
| `0x82509EA8` | `IN_AnalogstickleanUp` | cmd '-analogsticklean' (Cmd\_AddCommand from 0x8250EDB8) |  |  | high; names.toml |
| `0x82509EC0` | `IN_AnalogstickleanDown` | cmd '+analogsticklean' (Cmd\_AddCommand from 0x8250EDB8) |  |  | high; names.toml |
| `0x82509ED8` | `IN_StanceUp` | cmd '-stance' (Cmd\_AddCommand from 0x8250EDB8) |  |  | high; names.toml |
| `0x82509F48` | `IN_StanceDown` | cmd '+stance' (Cmd\_AddCommand from 0x8250EDB8) |  |  | high; names.toml |
| `0x82509FC8` | `IN_CenterView` | cmd 'centerview' (Cmd\_AddCommand from 0x8250EDB8); Q3 handler name |  |  | high; names.toml |
| `0x8250A028` | `IN_SprintUp` | cmd '-sprint' (Cmd\_AddButtonCommand from 0x8250EDB8) |  |  | high; names.toml |
| `0x8250A030` | `IN_SprintDown` | cmd '+sprint' (Cmd\_AddButtonCommand from 0x8250EDB8) |  |  | high; names.toml |
| `0x8250A038` | `IN_SprintbreathUp` | cmd '-sprintbreath' (Cmd\_AddButtonCommand from 0x8250EDB8) |  |  | high; names.toml |
| `0x8250A040` | `IN_SprintbreathDown` | cmd '+sprintbreath' (Cmd\_AddButtonCommand from 0x8250EDB8) |  |  | high; names.toml |
| `0x8250A048` | `CL_ToggleClRun_f` | cmd 'toggle cl\_run' (Cmd\_AddButtonCommand from 0x8250EDB8) |  |  | high; names.toml |
| `0x8250A070` | `CL_KeyState` | kbutton\_t fraction of the frame held; CL\_AdjustAngles calls it 4 times and CL\_KeyMove 9 times (Q3) |  |  | high; names.toml |
| `0x8250A110` | `IN_KeyUp` | called with a kbutton\_t by the '-' button handlers (IN\_ProneUp, IN\_DownUp...) that did not inline ... |  |  | high; names.toml |
| `0x8250A1B0` | `IN_MLookUp` | cmd '-mlook' (Cmd\_AddCommand from 0x8250EDB8); Q3 handler name |  |  | high; names.toml |
| `0x8250A1E0` | `IN_MLookDown` | cmd '+mlook' (Cmd\_AddCommand from 0x8250EDB8); Q3 handler name |  |  | high; names.toml |
| `0x8250A318` | `CL_GameCommand` | when cgvm is set: VM\_Call(cgvm, 2 = CG\_CONSOLE\_COMMAND), the cgame case that runs CG\_ConsoleCommand ... |  |  | high; names.toml |
| `0x8250A660` | `CL_GamepadEvent` | string 'CL\_GamepadEvent: bad axis %i' (only user) |  |  | high; names.toml |
| `0x8250AA58` | `CL_ShellExecute_f` | cmd 'shellExecute' (Cmd\_AddCommand from 0x82511398) |  |  | high; names.toml |
| `0x8250AB10` | `CL_ScaledMilliseconds` | Sys\_Milliseconds() \* com\_timescale-&gt;value; refimport Milliseconds slot (Q3) |  |  | high; names.toml |
| `0x8250AB68` | `CL_RefPrintf` | refimport Printf slot: vsprintf into a buffer (Q3) |  |  | high; names.toml |
| `0x8250ABB8` | `CL_ForwardToServer_f` | cmd 'cmd' (Cmd\_AddCommand from 0x82511398); Q3 handler name |  |  | high; names.toml |
| `0x8250AC20` | `CL_ForwardCommandToServer` (likely) | Q3 CL\_ForwardCommandToServer: 1 strings in common, e.g. 'Unknown command "%s"' |  |  | medium |
| `0x8250AE88` | `CL_WritePacket` | prints '%i ' under cl\_showSend, MSG\_WriteString of the reliable commands, Netchan\_Transmit (Q3) |  |  | high; names.toml |
| `0x8250B108` | `CL_KeyMove` | CL\_CreateCmd's movement step after CL\_CmdButtons: CL\_KeyState of the forward / back / side / up ... |  |  | high; names.toml |
| `0x8250B460` | `CL_AdjustAngles` | void CL\_AdjustAngles(void): turns the view from the right stick (or the +left/+right keys with ... | `void (void)` | hook mouse\_look.cpp | known; names.toml; addresses.h; comments; ENGINE.md |
| `0x8250B908` | `IN_GostandUp` | cmd '-gostand' (Cmd\_AddButtonCommand from 0x8250EDB8) |  |  | high; names.toml |
| `0x8250B948` | `IN_LeanrightUp` | cmd '-leanright' (Cmd\_AddButtonCommand from 0x8250EDB8) |  |  | high; names.toml |
| `0x8250B960` | `IN_LeanleftUp` | cmd '-leanleft' (Cmd\_AddButtonCommand from 0x8250EDB8) |  |  | high; names.toml |
| `0x8250B978` | `IN_ReloadUp` | cmd '-reload' (Cmd\_AddButtonCommand from 0x8250EDB8) |  |  | high; names.toml |
| `0x8250B9A8` | `IN_ActivatereloadUp` | cmd '-activatereload' (Cmd\_AddButtonCommand from 0x8250EDB8) |  |  | high; names.toml |
| `0x8250B9C0` | `IN_ActivateUp` | cmd '-activate' (Cmd\_AddButtonCommand from 0x8250EDB8) |  |  | high; names.toml |
| `0x8250B9D8` | `IN_MeleeUp` | cmd '-melee' (Cmd\_AddButtonCommand from 0x8250EDB8) |  |  | high; names.toml |
| `0x8250BA08` | `IN_Button0Up` | cmd '-attack' (Cmd\_AddButtonCommand from 0x8250EDB8); Q3 handler name |  |  | high; names.toml |
| `0x8250BA20` | `IN_StrafeUp` | cmd '-strafe' (Cmd\_AddButtonCommand from 0x8250EDB8); Q3 handler name |  |  | high; names.toml |
| `0x8250BA38` | `IN_SpeedUp` | cmd '-speed' (Cmd\_AddButtonCommand from 0x8250EDB8); Q3 handler name |  |  | high; names.toml |
| `0x8250BA50` | `IN_MoverightUp` | cmd '-moveright' (Cmd\_AddButtonCommand from 0x8250EDB8); Q3 handler name |  |  | high; names.toml |
| `0x8250BA68` | `IN_MoveleftUp` | cmd '-moveleft' (Cmd\_AddButtonCommand from 0x8250EDB8); Q3 handler name |  |  | high; names.toml |
| `0x8250BA80` | `IN_LookdownUp` | cmd '-lookdown' (Cmd\_AddButtonCommand from 0x8250EDB8); Q3 handler name |  |  | high; names.toml |
| `0x8250BA98` | `IN_LookupUp` | cmd '-lookup' (Cmd\_AddButtonCommand from 0x8250EDB8); Q3 handler name |  |  | high; names.toml |
| `0x8250BAB0` | `IN_BackUp` | cmd '-back' (Cmd\_AddButtonCommand from 0x8250EDB8); Q3 handler name |  |  | high; names.toml |
| `0x8250BAC8` | `IN_ForwardUp` | cmd '-forward' (Cmd\_AddButtonCommand from 0x8250EDB8); Q3 handler name |  |  | high; names.toml |
| `0x8250BAE0` | `IN_RightUp` | cmd '-right' (Cmd\_AddButtonCommand from 0x8250EDB8); Q3 handler name |  |  | high; names.toml |
| `0x8250BAF8` | `IN_LeftUp` | cmd '-left' (Cmd\_AddButtonCommand from 0x8250EDB8); Q3 handler name |  |  | high; names.toml |
| `0x8250BB10` | `IN_UpUp` | cmd '-moveup' (Cmd\_AddButtonCommand from 0x8250EDB8); Q3 handler name |  |  | high; names.toml |
| `0x8250BB50` | `IN_KeyDown` (likely) | Q3 IN\_KeyDown: 1 strings in common, e.g. 'Three keys down for a button!' |  |  | medium |
| `0x8250BBD8` | `CL_CgameSystemCalls` (likely) | Q3 CL\_CgameSystemCalls: 1 strings in common, e.g. 'Bad cgame system trap: %i' |  |  | medium |
| `0x8250BF60` | `CL_ShutdownCGame` | VM\_Call(cgvm, 1 = CG\_SHUTDOWN) then clears the vm (Q3); CL\_Shutdown and SV\_SpawnServer call it |  |  | high; names.toml |
| `0x8250BFD0` | `CL_GetUserCmd` | string 'CL\_GetUserCmd: %i &gt;= %i' (only user) |  |  | high; names.toml |
| `0x8250C0F8` | `CL_CubemapShot_f` | cmd 'cubemapShot' (Cmd\_AddCommand from 0x82511398) |  |  | high; names.toml |
| `0x8250C828` | `IN_ActivatemeleeUp` | cmd '-activatemelee' (Cmd\_AddButtonCommand from 0x8250EDB8) |  |  | high; names.toml |
| `0x8250C868` | `IN_ActivatemoveupUp` | cmd '-activatemoveup' (Cmd\_AddButtonCommand from 0x8250EDB8) |  |  | high; names.toml |
| `0x8250C8B0` | `IN_GrenadeattackUp` | cmd '-grenadeattack' (Cmd\_AddButtonCommand from 0x8250EDB8) |  |  | high; names.toml |
| `0x8250C8F0` | `IN_ClassUp` | cmd '-class' (Cmd\_AddButtonCommand from 0x8250EDB8) |  |  | high; names.toml |
| `0x8250C908` | `IN_SmokegrenadeUp` | cmd '-smokegrenade' (Cmd\_AddButtonCommand from 0x8250EDB8) |  |  | high; names.toml |
| `0x8250C920` | `CL_Togglepaused_f` | cmd 'togglepaused' (Cmd\_AddCommand from 0x8250EDB8) |  |  | high; names.toml |
| `0x8250C948` | `IN_LeanleftswitchnextUp` | cmd '-leanleftswitchnext' (Cmd\_AddButtonCommand from 0x8250EDB8) |  |  | high; names.toml |
| `0x8250C960` | `IN_LeanrightswitchnextUp` | cmd '-leanrightswitchnext' (Cmd\_AddButtonCommand from 0x8250EDB8) |  |  | high; names.toml |
| `0x8250C978` | `IN_GostandDown` | cmd '+gostand' (Cmd\_AddButtonCommand from 0x8250EDB8) |  |  | high; names.toml |
| `0x8250CA20` | `IN_LeanrightDown` | cmd '+leanright' (Cmd\_AddButtonCommand from 0x8250EDB8) |  |  | high; names.toml |
| `0x8250CA38` | `IN_LeanleftDown` | cmd '+leanleft' (Cmd\_AddButtonCommand from 0x8250EDB8) |  |  | high; names.toml |
| `0x8250CA50` | `IN_ReloadDown` | cmd '+reload' (Cmd\_AddButtonCommand from 0x8250EDB8) |  |  | high; names.toml |
| `0x8250CA80` | `IN_ActivatereloadDown` | cmd '+activatereload' (Cmd\_AddButtonCommand from 0x8250EDB8) |  |  | high; names.toml |
| `0x8250CAD0` | `IN_ActivateDown` | cmd '+activate' (Cmd\_AddButtonCommand from 0x8250EDB8) |  |  | high; names.toml |
| `0x8250CAE8` | `IN_MeleeDown` | cmd '+melee' (Cmd\_AddButtonCommand from 0x8250EDB8) |  |  | high; names.toml |
| `0x8250CB18` | `IN_Button0Down` | cmd '+attack' (Cmd\_AddButtonCommand from 0x8250EDB8); Q3 handler name |  |  | high; names.toml |
| `0x8250CB50` | `IN_StrafeDown` | cmd '+strafe' (Cmd\_AddButtonCommand from 0x8250EDB8); Q3 handler name |  |  | high; names.toml |
| `0x8250CB68` | `IN_SpeedDown` | cmd '+speed' (Cmd\_AddButtonCommand from 0x8250EDB8); Q3 handler name |  |  | high; names.toml |
| `0x8250CB80` | `IN_MoverightDown` | cmd '+moveright' (Cmd\_AddButtonCommand from 0x8250EDB8); Q3 handler name |  |  | high; names.toml |
| `0x8250CB98` | `IN_MoveleftDown` | cmd '+moveleft' (Cmd\_AddButtonCommand from 0x8250EDB8); Q3 handler name |  |  | high; names.toml |
| `0x8250CBB0` | `IN_LookdownDown` | cmd '+lookdown' (Cmd\_AddButtonCommand from 0x8250EDB8); Q3 handler name |  |  | high; names.toml |
| `0x8250CBC8` | `IN_LookupDown` | cmd '+lookup' (Cmd\_AddButtonCommand from 0x8250EDB8); Q3 handler name |  |  | high; names.toml |
| `0x8250CBE0` | `IN_BackDown` | cmd '+back' (Cmd\_AddButtonCommand from 0x8250EDB8); Q3 handler name |  |  | high; names.toml |
| `0x8250CBF8` | `IN_ForwardDown` | cmd '+forward' (Cmd\_AddButtonCommand from 0x8250EDB8); Q3 handler name |  |  | high; names.toml |
| `0x8250CC10` | `IN_RightDown` | cmd '+right' (Cmd\_AddButtonCommand from 0x8250EDB8); Q3 handler name |  |  | high; names.toml |
| `0x8250CC28` | `IN_LeftDown` | cmd '+left' (Cmd\_AddButtonCommand from 0x8250EDB8); Q3 handler name |  |  | high; names.toml |
| `0x8250CCF8` | `IN_HoldbreathUp` | cmd '-holdbreath' (Cmd\_AddButtonCommand from 0x8250EDB8) |  |  | high; names.toml |
| `0x8250CD30` | `IN_HoldbreathDown` | cmd '+holdbreath' (Cmd\_AddButtonCommand from 0x8250EDB8) |  |  | high; names.toml |
| `0x8250CD68` | `IN_BinocularsUp` | cmd '-binoculars' (Cmd\_AddButtonCommand from 0x8250EDB8) |  |  | high; names.toml |
| `0x8250CDB0` | `IN_BinocularsDown` | cmd '+binoculars' (Cmd\_AddButtonCommand from 0x8250EDB8) |  |  | high; names.toml |
| `0x8250CE40` | `CL_GetSnapshot` | string 'CL\_GetSnapshot: snapshotNumber &gt; cl[currCl].snapshot.mess...' (only user) |  |  | high; names.toml |
| `0x8250CF10` | `CL_ParseSnapshot` | CL\_ParseServerMessage's svc\_snapshot (7) case (Q3 svc\_ops order: nop 1, serverCommand 5, snapshot ... |  |  | high; names.toml |
| `0x8250D590` | `CL_ShutdownInput` | removes every command CL\_InitInput added ('centerview', '+moveup' / '-moveup'...) with ... |  |  | high; names.toml |
| `0x8250D898` | `IN_ActivatemeleeDown` | cmd '+activatemelee' (Cmd\_AddButtonCommand from 0x8250EDB8) |  |  | high; names.toml |
| `0x8250D978` | `IN_GrenadeattackDown` | cmd '+grenadeattack' (Cmd\_AddButtonCommand from 0x8250EDB8) |  |  | high; names.toml |
| `0x8250DA30` | `IN_ClassDown` | cmd '+class' (Cmd\_AddButtonCommand from 0x8250EDB8) |  |  | high; names.toml |
| `0x8250DA90` | `IN_SmokegrenadeDown` | cmd '+smokegrenade' (Cmd\_AddButtonCommand from 0x8250EDB8) |  |  | high; names.toml |
| `0x8250DB80` | `CL_ParseServerMessage` | string 'CL\_ParseServerMessage: Illegible server message %d' (only user) |  |  | high; names.toml; ENGINE.md |
| `0x8250DD80` | `CL_PacketEvent` (likely) | Q3 CL\_PacketEvent: 1 strings in common, e.g. '%s:sequenced packet without connection' |  |  | medium |
| `0x8250DE88` | `CL_CharEvent` | Com\_EventLoop's SE\_CHAR (type 2) case, between CL\_KeyEvent (1) and the empty mouse case (Q3) |  |  | high; names.toml |
| `0x8250DEE0` | `CL_InitKeyCommands` (likely) | Q3 CL\_InitKeyCommands: 4 cvar/key names in common, e.g. 'unbindall' |  |  | medium |
| `0x8250DF50` | `CL_Gocrouch_f` | cmd 'gocrouch' (Cmd\_AddCommand from 0x8250EDB8) |  |  | high; names.toml |
| `0x8250DFC0` | `CL_Goprone_f` | cmd 'goprone' (Cmd\_AddCommand from 0x8250EDB8) |  |  | high; names.toml |
| `0x8250E030` | `CL_Toggleprone_f` | cmd 'toggleprone' (Cmd\_AddCommand from 0x8250EDB8) |  |  | high; names.toml |
| `0x8250E0D0` | `CL_Togglecrouch_f` | cmd 'togglecrouch' (Cmd\_AddCommand from 0x8250EDB8) |  |  | high; names.toml |
| `0x8250E170` | `CL_Raisestance_f` | cmd 'raisestance' (Cmd\_AddCommand from 0x8250EDB8) |  |  | high; names.toml |
| `0x8250E238` | `CL_Lowerstance_f` | cmd 'lowerstance' (Cmd\_AddCommand from 0x8250EDB8) |  |  | high; names.toml |
| `0x8250E300` | `IN_UpDown` | cmd '+moveup' (Cmd\_AddButtonCommand from 0x8250EDB8); Q3 handler name |  |  | high; names.toml |
| `0x8250E518` | `CL_SystemInfoChanged` | Info\_ValueForKey of 'sv\_serverid', 'sv\_cheats' (Cvar\_SetCheatState when 0), 'sv\_paks' ... |  |  | high; names.toml |
| `0x8250E5E8` | `IN_DecAnimWeightDown` | cmd '+decAnimWeight' (Cmd\_AddCommand from 0x82511398) |  |  | high; names.toml |
| `0x8250E6A0` | `IN_IncAnimWeightDown` | cmd '+incAnimWeight' (Cmd\_AddCommand from 0x82511398) |  |  | high; names.toml |
| `0x8250E768` | `IN_ActivatemoveupDown` | cmd '+activatemoveup' (Cmd\_AddButtonCommand from 0x8250EDB8) |  |  | high; names.toml |
| `0x8250E7C0` | `IN_LeanleftswitchnextDown` | cmd '+leanleftswitchnext' (Cmd\_AddButtonCommand from 0x8250EDB8) |  |  | high; names.toml |
| `0x8250E808` | `IN_LeanrightswitchnextDown` | cmd '+leanrightswitchnext' (Cmd\_AddButtonCommand from 0x8250EDB8) |  |  | high; names.toml |
| `0x8250E850` | `IN_ProneUp` | cmd '-prone' (Cmd\_AddButtonCommand from 0x8250EDB8) |  |  | high; names.toml |
| `0x8250E8C0` | `IN_ProneDown` | cmd '+prone' (Cmd\_AddButtonCommand from 0x8250EDB8) |  |  | high; names.toml |
| `0x8250E930` | `IN_DownUp` | cmd '-movedown' (Cmd\_AddButtonCommand from 0x8250EDB8); Q3 handler name |  |  | high; names.toml |
| `0x8250E9A0` | `IN_DownDown` | cmd '+movedown' (Cmd\_AddButtonCommand from 0x8250EDB8); Q3 handler name |  |  | high; names.toml |
| `0x8250EA10` | `CL_ConfigstringModified` | atoi(Cmd\_Argv(1)) with 'configstring &gt; MAX\_CONFIGSTRINGS'; CL\_GetServerCommand's 'cs' case (Q3) |  |  | high; names.toml |
| `0x8250EC08` | `CL_ClearState` (likely) | zeroes the client state (memset of cl); CL\_Disconnect and CL\_MapLoading call it (Q3) |  |  | medium |
| `0x8250EDB8` | `CL_InitInput` | adds centerview and the +/- button commands, cl\_nodelta, cl\_debugMove (Q3) |  |  | high; names.toml |
| `0x8250F4A8` | `CL_GetServerCommand` (likely) | Q3 CL\_GetServerCommand: 1 strings in common, e.g. 'wait ; wait ; wait ; wait ; screenshot levelshot' |  |  | medium |
| `0x8250F5F0` | `CL_Disconnect` | under cl\_running: Cvar\_Set 'r\_uiFullScreen' '1' (Q3's first step); CL\_Shutdown and SV\_Shutdown call ... |  |  | high; names.toml |
| `0x8250F8B0` | `CL_KeyEvent` | Q3 CL\_KeyEvent: 5 strings in common, e.g. '%s is unbound, use controls menu to set.' |  |  | high; names.toml |
| `0x8250FDF8` | `CL_AddKeyUpCommands` (likely) | Cbuf\_AddText of '-%s %i %i' for the released key's '+' binding (Q3) |  |  | medium |
| `0x8250FF48` | `CL_MouseMove` | prints '%f : %f' under cl\_showmouserate, reads m\_pitch, m\_yaw, cl\_mouseAccel; called by ... |  |  | high; names.toml; ENGINE.md |
| `0x82510530` | `CL_InitRef` | Q3 CL\_InitRef: 2 strings in common, e.g. '----- Initializing Renderer ----' |  |  | high; names.toml |
| `0x82510748` | `CL_CreateCmd` | addresses.h (calls CL\_AdjustAngles) |  |  | known; names.toml; comments; ENGINE.md |
| `0x82510980` | `CL_CreateNewCommands` | stores CL\_CreateCmd's result in the next cl.cmds slot (Q3); called by CL\_Frame and CL\_SendCmd |  |  | high; names.toml |
| `0x82510B20` | `CL_ShutdownUI` (likely) | the front-end teardown called by CL\_Shutdown and CL\_ShutdownAll (Q3 order) |  |  | medium |
| `0x82510C50` | `CL_Shutdown` | Q3 CL\_Shutdown: 2 strings in common, e.g. '----- CL\_Shutdown -----' |  |  | high; names.toml |
| `0x82510DF8` | `CL_StartHunkUsers` | under cl\_running: CL\_InitRenderer, then the front end's 'FrontEnd' load (Q3); Com\_Init / ... |  |  | high; names.toml |
| `0x82510E98` | `CL_ShutdownAll` | CL\_ShutdownCGame, the UI shutdown and the renderer's shutdown (Q3) |  |  | high; names.toml |
| `0x82510EF0` | `CL_SendCmd` | when cls.state is connected: CL\_CreateNewCommands then CL\_WritePacket (Q3) |  |  | high; names.toml |
| `0x82510F60` | `SCR_DrawScreenField` | string 'SCR\_DrawScreenField: bad cls.state' (only user) |  |  | high; names.toml |
| `0x82511168` | `SCR_UpdateScreen` | CL\_Updatescreen\_f's call; draws through SCR\_DrawScreenField between the renderer's begin / end (Q3) |  |  | high; names.toml |
| `0x82511370` | `CL_Updatescreen_f` | cmd 'updatescreen' (Cmd\_AddCommand from 0x82511398) |  |  | high; names.toml |
| `0x82511398` | `CL_Init` | Q3 CL\_Init: 2 strings in common, e.g. '----- Client Initialization Complete -----' |  |  | high; names.toml; ENGINE.md |
| `0x82511B28` | `CL_Frame` (likely) | Q3 CL\_Frame: 1 strings in common, e.g. 'screenshot silent' |  |  | medium |
| `0x82511FE0` | `CL_MapLoading` | SV\_SpawnServer's client step: Cvar\_Set 'nextmap', CL\_ClearState, servername 'localhost' ... |  |  | high; names.toml |
| `0x82512118` | `CL_InitCGame` | string 'CL\_InitCGame: %5.2f seconds' (only user) |  |  | high; names.toml |
| `0x8251E8E0` | `IN_Init` (likely) | Q3 IN\_Init: 5 cvar/key names in common, e.g. 'in\_debugjoystick' |  |  | medium |

### Client game (cgame): view and HUD (64)

| Address | Name | What it does | Arguments | Port use | Sources |
|---|---|---|---|---|---|
| `0x82431FC0` | `CG_ScoresUp_f` | cmd '-scores' (command table 0x820637F0); Q3 handler name |  |  | high; names.toml |
| `0x82432458` | `CG_SaveViewModelAnimTrees` | string 'CG\_SaveViewModelAnimTrees: viewmodel has no dobj' (only user) |  |  | high; names.toml |
| `0x82433460` | `CG_Error` | vsprintf then Com\_Error; cg\_vmMain reports 'cg\_vmMain: unknown command %i' with it (Q3) |  |  | high; names.toml |
| `0x824334C0` | `CG_Printf` | vsprintf into a 1 KB buffer then the (empty) print; CG\_Viewpos\_f's '(%i %i %i) : %i' and the ... |  |  | high; names.toml |
| `0x82433520` | `CG_FreeLocalEntity` | string 'CG\_FreeLocalEntity: not active' (CG\_AddLocalEntities has an inlined copy) (Q3) |  |  | high; names.toml |
| `0x82433810` | `CG_FillRect` | re.SetColor(color), screen-scaled re.StretchPic with the fixed white shader, re.SetColor(NULL); the ... |  |  | high; names.toml |
| `0x824340E0` | `CG_Viewpos_f` | cmd 'viewpos' (command table 0x820637F0); Q3 handler name; also Q3 CG\_Viewpos\_f: 1 strings in ... |  |  | high; names.toml |
| `0x82434EE0` | `CG_UpdateCvars` | Cvar\_Update on every entry of the cgame cvarTable (Q3) |  |  | high; names.toml |
| `0x82434FC8` | `CG_AllocLocalEntity` | frees the oldest active local entity with CG\_FreeLocalEntity when none is free, then zeroes the new ... |  |  | high; names.toml |
| `0x82435588` | `CG_CalcCrosshairColor` | addresses.h crosshair notes |  |  | known; names.toml; comments; ENGINE.md |
| `0x82435680` | `CG_ConsoleCommand` | called by cg\_vmMain; Cmd\_ArgvBuffer(0) compared with the cgame command table 0x820637F0 (Q3) |  |  | high; names.toml; ENGINE.md |
| `0x82435728` | `CG_Fade_f` | cmd 'fade' (command table 0x820637F0) |  |  | high; names.toml |
| `0x82436C38` | `CG_Weapon_f` | cmd 'weapon' (command table 0x820637F0); Q3 handler name |  |  | high; names.toml |
| `0x82439250` | `CG_ShouldDrawCrosshair` | project notes: decides whether a crosshair is shown |  |  | known; names.toml |
| `0x82439530` | `CG_DrawMountedCrosshair` | addresses.h: the mounted gun / vehicle crosshair |  | mid-asm Cod3MountedCrosshairHook default.toml | known; names.toml |
| `0x824398F8` | `CG_InitConsoleCommands` | called by CG\_Init; adds the cgame command names and the server's 'levelshot', 'stats' without ... |  |  | high; names.toml |
| `0x82439C90` | `CG_Weaponslot_f` | cmd 'weaponslot' (command table 0x820637F0) |  |  | high; names.toml |
| `0x8243A198` | `CG_GetViewFov` | float CG\_GetViewFov(void): reads cg\_fov, clamps to [1,160], applies ADS / turret / vehicle zoom ... | `float (void)` | hook fov.cpp | known; names.toml; addresses.h; comments; ENGINE.md |
| `0x8243A410` | `CG_ReadNextSnapshot` | string 'WARNING: CG\_ReadNextSnapshot: way out of range, %i &gt; %i' (only user) |  |  | high; names.toml |
| `0x8243A638` | `CG_ParseServerinfo` | Info\_ValueForKey 'mapname' into 'maps/%s.bsp' (Q3) |  |  | high; names.toml |
| `0x8243A9D0` | `CG_Shutdown` | cgame vmMain case 1 (CG\_SHUTDOWN), reached by CL\_ShutdownCGame's VM\_Call(cgvm, 1) (Q3) |  |  | high; names.toml |
| `0x8243AA38` | `CG_NewClientInfo` (likely) | Info\_ValueForKey 'n' (name) and 't' (atoi team) of the client configstring (Q3) |  |  | medium |
| `0x8243B1D8` | `CG_ScoresDown_f` | cmd '+scores' (command table 0x820637F0); Q3 handler name |  |  | high; names.toml |
| `0x8243B820` | `CG_DrawSkyBoxPortal` | string 'CG\_DrawSkyBoxPortal: error parsing skybox configstring' (only user) |  |  | high; names.toml |
| `0x8243C390` | `CG_TransitionPlayerState` | called by CG\_TransitionSnapshot and at the end of CG\_PredictPlayerState (Q3) |  |  | high; names.toml |
| `0x8243C7C8` | `CG_RegisterCvars` | loops over the cgame cvarTable (147 x {vmCvar\*, name, default, flags}): Cvar\_Get, handle ... |  |  | high; names.toml; ENGINE.md |
| `0x8243C850` | `CG_AddLocalEntities` (likely) | Q3 CG\_AddLocalEntities: 1 strings in common, e.g. 'Bad leType: %i' |  |  | medium |
| `0x8243CD70` | `CG_CalcEntityLerpPositions` | BG\_EvaluateTrajectory of pos and apos at cg.time (or interpolation), called right before ... |  |  | high; names.toml |
| `0x8243D528` | `CG_DrawCenterString` | reads cg\_centertime, splits the text at newlines (Q3) |  |  | high; names.toml |
| `0x8243D940` | `CG_SizeDown_f` | cmd 'sizedown' (command table 0x820637F0); Q3 handler name |  |  | high; names.toml |
| `0x8243D990` | `CG_SizeUp_f` | cmd 'sizeup' (command table 0x820637F0); Q3 handler name |  |  | high; names.toml |
| `0x8243DFB0` | `CG_PrevWeapon_f` | cmd 'weapprev' (command table 0x820637F0); Q3 handler name |  |  | high; names.toml |
| `0x8243E050` | `CG_NextWeapon_f` | cmd 'weapnext' (command table 0x820637F0); Q3 handler name |  |  | high; names.toml |
| `0x8243EDB8` | `CG_OffsetFirstPersonView` (likely) | view height, bob (needs speed |  |  | notes: camera-drift |
| `0x8243F280` | `CG_CalcVrect` | clamps cg\_viewsize to 30..100 with Cvar\_Set (Q3), cg\_letterbox |  |  | high; names.toml |
| `0x8243F490` | `CG_SetInitialSnapshot` (likely) | CG\_ProcessSnapshots' first-snapshot setup: 'fade' command, cg\_norender back to 0 |  |  | medium |
| `0x824408F0` | `CG_ShellshockSave_f` | cmd 'cg\_shellshock\_save' (command table 0x820637F0) |  |  | high; names.toml |
| `0x82440950` | `CG_ShellshockLoad_f` | cmd 'cg\_shellshock\_load' (command table 0x820637F0) |  |  | high; names.toml |
| `0x824409B0` | `CG_Shellshock_f` | cmd 'cg\_shellshock' (command table 0x820637F0) |  |  | high; names.toml |
| `0x82442F18` | `CG_Weapalt_f` | cmd 'weapalt' (command table 0x820637F0) |  |  | high; names.toml |
| `0x82443338` | `CG_WeaponRunXModelAnims` | string 'CG\_WeaponRunXModelAnims: Unknown weapon animation %i' (only user) |  |  | high; names.toml |
| `0x82443A98` | `CG_CalcFov` | reads cg\_fov through CG\_GetViewFov, the widescreen and zoom adjustments (Q3) |  |  | high; names.toml; comments |
| `0x824447C8` | `CG_DrawCrosshair` | addresses.h / config hooks |  | mid-asm Cod3CrosshairHook default.toml | known; names.toml; ENGINE.md |
| `0x82444CD0` | `CG_CenterPrint` (likely) | 'Center Print' text into the center-print buffer |  |  | medium |
| `0x82444E88` | `CG_CalcMuzzlePoint` | string 'No %s in CG\_CalcMuzzlePoint on ...' (tag\_gunner\_flash), the bg\_viewheight cvars (Q3) |  |  | high; names.toml |
| `0x82446238` | `CG_Draw2D` | profile label 'CG\_Draw2D'; reads cg\_draw2D and cg\_drawStatus (Q3) |  |  | high; names.toml; ENGINE.md |
| `0x824483C8` | `CG_OffsetThirdPersonView` | reads cg\_thirdPersonAngle, cg\_thirdPersonRange, cg\_thirdPersonLock (Q3) |  |  | high; names.toml |
| `0x824487F8` | `CG_DrawActive` | CG\_DrawActiveFrame's last call: renders the scene through the refexport (cg\_skybox) then CG\_Draw2D ... |  |  | high; names.toml |
| `0x82448C78` | `CG_RegisterWeapon` | string 'CG\_RegisterWeapon: No hand model specified for [%s]' (only user) |  |  | high; names.toml |
| `0x82449B68` | `CG_LoadViewModelAnimTrees` | string 'CG\_LoadViewModelAnimTrees: viewmodel has no dobj' (only user) |  |  | high; names.toml |
| `0x82449F48` | `CG_Item` (likely) | Q3 CG\_Item: 1 strings in common, e.g. 'Bad item index %i on entity' |  |  | medium |
| `0x8244AAE0` | `CG_PredictPlayerState` | reads cg\_nopredict, cg\_errordecay, pmove\_fixed, pmove\_msec; CL\_GetUserCmd and Pmove per command (Q3) |  |  | high; names.toml; ENGINE.md |
| `0x8244BF90` | `CG_Init` (likely) | Q3 CG\_Init: 1 strings in common, e.g. 'Client/Server game mismatch: %s/%s' |  |  | medium |
| `0x8244CFB0` | `CG_AddCEntity` | 'Bad entity type: %i' default case of the entity type switch, CG\_Item for items (Q3) |  |  | high; names.toml |
| `0x8244DF60` | `CG_ServerCommand` | Cmd\_ArgvBuffer(0) dispatch of the server commands ('server print', 'game message'...) (Q3) |  |  | high; names.toml |
| `0x8244E678` | `CG_AddPacketEntities` | loops over the snapshot entities: CG\_CalcEntityLerpPositions then CG\_AddCEntity for each (Q3) |  |  | high; names.toml; ENGINE.md |
| `0x8244E9F8` | `CG_ExecuteNewServerCommands` | CL\_GetServerCommand for each new sequence, then CG\_ServerCommand (Q3) |  |  | high; names.toml |
| `0x8244EB48` | `CG_SetNextSnap` | CG\_ProcessSnapshots stores the newly read snapshot with it before the 'Server time went backwards' ... |  |  | high; names.toml |
| `0x8244ECF8` | `CG_TransitionSnapshot` | CG\_ProcessSnapshots' loop step when cg.time passed the next snapshot; cg\_nopredict test before ... |  |  | high; names.toml |
| `0x8244F080` | `CG_ProcessSnapshots` | string 'CG\_ProcessSnapshots: n &lt; cg[currCl].latestSnapshotNum' (only user) |  |  | high; names.toml; ENGINE.md |
| `0x8244FEA8` | `CG_AddViewWeapon` | cg\_drawGun check, CG\_RegisterWeapon and CG\_WeaponRunXModelAnims for the view weapon (Q3) |  |  | high; names.toml |
| `0x82450C10` | `CG_CalcViewValues` | view offsets, then CG\_CalcVrect and CG\_CalcFov (Q3 order ends with return CG\_CalcFov()) |  |  | high; names.toml; notes: camera-drift; ENGINE.md |
| `0x82450C90` | `CG_DrawActiveFrame` | cgame vmMain case 3 (CG\_DRAW\_ACTIVE\_FRAME); CG\_UpdateCvars then CG\_ProcessSnapshots, the 'Cg\_view' ... |  |  | high; names.toml; ENGINE.md |
| `0x82451320` | `cg_vmMain` | string 'cg\_vmMain: unknown command %i' (only user) |  |  | high; names.toml |

### Server (55)

| Address | Name | What it does | Arguments | Port use | Sources |
|---|---|---|---|---|---|
| `0x82512290` | `SV_WriteSnapshotToClient` | writes svc\_snapshot (7), the server time from VM\_Call(gvm, 19) and the snapshot flags (Q3) |  |  | high; names.toml |
| `0x82512338` | `SV_ClientThink` | VM\_Call(gvm, 6, the active client's last usercmd), the game case that runs ClientThink (Q3 ... |  |  | high; names.toml |
| `0x82512660` | `SV_GameCommand` | Cmd\_ExecuteString's fallback: VM\_Call(gvm, 12) whose vmMain case is the 'entitylist' console ... |  |  | high; names.toml |
| `0x825126D0` | `SV_Invert_f` | cmd 'invert' (Cmd\_AddServerCommand from 0x82519090) |  |  | high; names.toml |
| `0x825126E8` | `SV_QuickStart_f` | cmd 'quick\_start' (Cmd\_AddServerCommand from 0x82519090) |  |  | high; names.toml |
| `0x825126F8` | `SV_Frontend_f` | cmd 'frontend' (Cmd\_AddServerCommand from 0x82519090) |  |  | high; names.toml |
| `0x82512740` | `SV_StringUsage_f` | cmd 'stringUsage' (Cmd\_AddServerCommand from 0x82519090) |  |  | high; names.toml |
| `0x825127F0` | `SV_UpdateServerCommandsToClient` | writes svc\_serverCommand (5), the sequence and MSG\_WriteString for each unsent reliable command ... |  |  | high; names.toml |
| `0x82512908` | `SV_AddServerCommand` | Q3 SV\_AddServerCommand: 2 strings in common, e.g. '===== pending server commands =====' |  |  | high; names.toml |
| `0x82512C40` | `SV_ShutdownGameProgs` | VM\_Call(gvm, 1 = GAME\_SHUTDOWN) then clears the vm (Q3); SV\_Shutdown and SV\_SpawnServer call it |  |  | high; names.toml |
| `0x82512CA0` | `SV_GameSystemCalls` (likely) | Q3 SV\_GameSystemCalls: 1 strings in common, e.g. 'Bad game system trap: %i' |  |  | medium |
| `0x82512D78` | `SV_GetUsercmd` | string 'SV\_GetUsercmd: bad clientNum:%i' (only user) |  |  | high; names.toml |
| `0x82512E08` | `SV_ExecuteClientCommand` | VM\_Call(gvm, 5) whose vmMain case is ClientCommand (Q3) |  |  | high; names.toml |
| `0x82512F28` | `SV_ClearServer` | frees the configstrings and zeroes sv; SV\_Shutdown and SV\_SpawnServer (Q3) |  |  | high; names.toml |
| `0x82513020` | `SV_GetConfigstring` | string 'SV\_GetConfigstring: bad index %i' (only user) |  |  | high; names.toml |
| `0x825130C0` | `SV_ClientCommand` | MSG\_ReadString of the command, 'clientCommand: %i : %s' under sv\_showCommands, then ... |  |  | high; names.toml |
| `0x82513198` | `SV_LinkEntity` | the game's trap\_LinkEntity: last call of FinishSpawningItem and of Cmd\_SetViewpos\_f's teleport ... |  |  | high; names.toml |
| `0x82513628` | `SV_SendServerCommand` | (client, fmt, ...): vsprintf then SV\_AddServerCommand; SV\_SetConfigstring broadcasts 'cs' with it ... |  |  | high; names.toml |
| `0x825136F8` | `SV_GameSendServerCommand` | the game's trap: SV\_SendServerCommand of '%s' to one client or all; CheatsOk / Cmd\_Where\_f print ... |  |  | high; names.toml |
| `0x825137A8` | `SV_ExecuteClientMessage` (likely) | Q3 SV\_ExecuteClientMessage: 1 strings in common, e.g. 'WARNING: bad command byte for client %i' |  |  | medium; ENGINE.md |
| `0x82513B80` | `SV_PacketEvent` | Com\_RunAndTimeServerPacket's call; Netchan\_Process then SV\_ExecuteClientMessage, 'disconnect' (Q3) |  |  | high; names.toml |
| `0x82513C98` | `SV_SetConfigstring` | string 'SV\_SetConfigstring: bad index %i' (only user) |  |  | high; names.toml; ENGINE.md |
| `0x82513E48` | `SV_InitGameVM` | VM\_Call(gvm, 0 = GAME\_INIT, ..., Com\_Milliseconds()) (Q3); SV\_InitGameProgs' last call |  |  | high; names.toml |
| `0x82513EF0` | `SV_SetBrushModel` | the inline model number from the '\*N' name (atoi), its bounds, then SV\_LinkEntity; InitTrigger's ... |  |  | high; names.toml |
| `0x825140D0` | `SV_BuildClientSnapshot` | string 'SV\_BuildClientSnapshot: bad gEnt' (only user) |  |  | high; names.toml; ENGINE.md |
| `0x82514198` | `SV_RunGameFrame` | SV\_Frame's game step: refreshes CS\_SERVERINFO / CS\_SYSTEMINFO from Cvar\_InfoString(4) / ... |  |  | high; names.toml |
| `0x82514240` | `SV_Startup` | string 'SV\_Startup: svs.initialized' (only user) |  |  | high; names.toml |
| `0x82514440` | `SV_DirectConnect` (likely) | Q3 SV\_DirectConnect: 1 strings in common, e.g. 'SVC\_DirectConnect ()' |  |  | medium |
| `0x82514530` | `SV_MapRestartCheck_f` | cmd 'map\_restart\_check' (Cmd\_AddServerCommand from 0x82519090) |  |  | high; names.toml |
| `0x825145D8` | `SV_LoadMpExe_f` | SV\_LoadMpExe\_f, the load\_mp\_exe command (name at 0x82069654, registered with Cmd\_AddServerCommand ... |  | hook multiplayer.cpp | high; names.toml; addresses.h; notes: multiplayer-exit |
| `0x82514710` | `SV_Reboot_f` | cmd 'reboot' (Cmd\_AddServerCommand from 0x82519090) |  |  | high; names.toml; comments |
| `0x825148D0` | `SV_MapRestart_f` | addresses.h kMapRestart ('map\_restart') |  | hook cheat\_watch.cpp | known; names.toml; addresses.h |
| `0x825149A8` | `SV_MapRestartInternal_f` | addresses.h kMapRestartInternal ('map\_restart\_internal') |  | hook cheat\_watch.cpp | known; names.toml; addresses.h |
| `0x82514A60` | `SV_MapRestartComp_f` | cmd 'map\_restart\_comp' (Cmd\_AddServerCommand from 0x82519090) |  |  | high; names.toml |
| `0x82514A68` | `SV_DifficultyHard_f` | cmd 'difficultyHard' (Cmd\_AddServerCommand from 0x82519090) |  |  | high; names.toml |
| `0x82514B18` | `SV_DifficultyMedium_f` | cmd 'difficultyMedium' (Cmd\_AddServerCommand from 0x82519090) |  |  | high; names.toml |
| `0x82514BC8` | `SV_DifficultyEasy_f` | cmd 'difficultyEasy' (Cmd\_AddServerCommand from 0x82519090) |  |  | high; names.toml |
| `0x82514DD8` | `SV_SendClientSnapshot` | SV\_BuildClientSnapshot, message init, SV\_UpdateServerCommandsToClient, SV\_WriteSnapshotToClient ... |  |  | high; names.toml |
| `0x82514ED0` | `SV_SetExpectedHunkUsage` | reads hunkusage.dat, finds the map with Com\_ParseExt and sets com\_expectedhunkusage, else '-1' ... |  |  | high; names.toml |
| `0x82515030` | `SV_LoadgameContinue_f` | cmd 'loadgame\_continue' (Cmd\_AddServerCommand from 0x82519090) |  |  | high; names.toml |
| `0x82515038` | `SV_LoadGameRestart_f` | string 'SV\_LoadGameRestart\_f: '%s'. Invalid character (%c) in fi...' (only user) |  |  | high; names.toml |
| `0x825161D0` | `SV_SendClientMessages` | SV\_Frame's last step: VM\_Call(gvm, 21) then SV\_SendClientSnapshot for the active client (Q3) |  |  | high; names.toml |
| `0x82516350` | `SV_Shutdown` | Q3 SV\_Shutdown: 2 strings in common, e.g. '---------------------------' |  |  | high; names.toml |
| `0x82516E28` | `SV_Trace` | the game's trap\_Trace (64 callers): FinishSpawningItem's drop trace and G\_RunItem's move trace go ... |  |  | high; names.toml |
| `0x825177D8` | `SV_InitGameProgs` (likely) | Q3 SV\_InitGameProgs: 1 strings in common, e.g. 'VM\_Create on game failed' |  |  | medium |
| `0x82517A00` | `SV_SpawnServer` | Level start: SV\_SpawnServer (prints "------ Server Initialization ------") and the map\_restart / ... |  | hook cheat\_watch.cpp | known; names.toml; addresses.h; notes: cutscenes; ENGINE.md |
| `0x82518518` | `SV_LoadgameRestart_f` | cmd 'loadgame\_restart' (Cmd\_AddServerCommand from 0x82519090) |  |  | high; names.toml |
| `0x82518580` | `SV_Loadgame_f` | cmd 'loadgame' (Cmd\_AddServerCommand from 0x82519090) |  |  | high; names.toml |
| `0x82518810` | `SV_Map_f` | cmd 'spmap' (Cmd\_AddServerCommand from 0x82519090), also 'map'; Q3 handler name |  |  | high; names.toml |
| `0x82518E38` | `SV_Devmap_f` | cmd 'devmap' (Cmd\_AddServerCommand from 0x82519090) |  |  | high; names.toml; ENGINE.md |
| `0x82518E40` | `SV_Frame` | Com\_Frame calls it with msec before the client half; sv\_killserver -&gt; shutdown and Cvar\_Set ... |  |  | high; names.toml; ENGINE.md |
| `0x82519090` | `SV_AddOperatorCommands` | adds map, devmap, spmap, map\_restart... (Q3) |  |  | high; names.toml |
| `0x82519240` | `SV_Init` | calls SV\_AddOperatorCommands, registers sv\_serverid, sv\_paks, sv\_pakNames, sv\_referencedPaks ... |  |  | high; names.toml; ENGINE.md |
| `0x8253DC18` | `Svcmd_EntityList_f` | ConsoleCommand's 'entitylist' handler (Q3) |  |  | high; names.toml |
| `0x8253EB70` | `Svcmd_ListVehicles_f` | ConsoleCommand's 'listvehicles' handler |  |  | high; names.toml |

### Game: entities, rules, cheats (70)

| Address | Name | What it does | Arguments | Port use | Sources |
|---|---|---|---|---|---|
| `0x823D14B8` | `Actor_UpdateEyeInformation` | string 'Actor\_UpdateEyeInformation: Actor dobj doesn't have BIP01...' (only user) |  |  | high; names.toml |
| `0x824B27A8` | `MagicGrenade` (likely) | string 'MagicGrenade: None of the sScrMethods worked (probably di...' (only user) |  |  | medium |
| `0x824BB028` | `G_InitLevelInterface` | fills the level interface table (\*0x82A2A2A0) from the (offset, function) lists at 0x825F5638 / ... |  |  | high; names.toml; ENGINE.md |
| `0x824BB360` | `G_LoadLevelDll` | sprintf 'game:\\%s.dll', loads the level DLL, calls its first export with the interface pointer ... |  |  | high; names.toml |
| `0x82538B08` | `G_SpawnStringNoDefault` (likely) | (key hash, out): the same spawnVars lookup without a default; SP\_trigger\_multiple's 'wait' / ... |  |  | medium |
| `0x82538B60` | `G_SpawnString` | (key hash, default, out): looks the key up in level.spawnVars (count at +176) or returns the ... |  |  | high; names.toml |
| `0x82539518` | `G_Error` | vsprintf then the error call; G\_RunThink's 'NULL ent-&gt;think' and SelectRandomFurthestSpawnPoint's ... |  |  | high; names.toml |
| `0x82539570` | `G_LogPrintf` | empty (compiled-out) log print; G\_ShutdownGame logs 'ShutdownGame:' and the dashed line with it (Q3) |  |  | high; names.toml |
| `0x82539578` | `G_Printf` (likely) | empty (compiled-out) print called with the Q3 G\_Printf texts: FinishSpawningItem 'startsolid' ... |  |  | medium |
| `0x8253B0F8` | `G_SpawnVector` | spawn var parsed with sscanf '%f %f %f' (Q3) |  |  | high; names.toml |
| `0x8253B170` | `G_SpawnInt` | spawn var through G\_SpawnStringNoDefault then atoi, else the default (Q3) |  |  | high; names.toml |
| `0x8253B1D0` | `G_SpawnFloat` | spawn var through G\_SpawnStringNoDefault then atof, else the default (Q3) |  |  | high; names.toml |
| `0x8253C960` | `G_RunThink` (likely) | Q3 G\_RunThink: 1 strings in common, e.g. 'NULL ent-&gt;think' |  |  | medium; ENGINE.md |
| `0x8253CD70` | `ConcatArgs` | joins cmd\_argv[start..] into a static buffer; Cmd\_Give\_f / Cmd\_Take\_f read the item name with it ... |  |  | high; names.toml |
| `0x8253D1A0` | `G_FreeEntity` | unlinks the entity and frees its memory; FinishSpawningItem (startsolid) and ClientDisconnect call ... |  |  | high; names.toml |
| `0x8253F510` | `G_UpdateCvars` | Cvar\_Update on every entry of the game cvarTable, modificationCount check (Q3) |  |  | high; names.toml |
| `0x8253FDC8` | `G_ParseHitLocDmgTable` (likely) | 'Error parsing hitloc damage table' |  |  | medium |
| `0x82540168` | `SetClientViewAngle` (likely) | Cmd\_SetViewpos\_f and Cmd\_JumpToNode\_f set the view angles with it after moving the player (Q3 ... |  |  | medium |
| `0x82541150` | `ConsoleCommand` | game vmMain case 12: Cmd\_ArgvBuffer(0) compared with 'entitylist' and 'listvehicles' (Q3 g\_svcmds.c) |  |  | high; names.toml |
| `0x82542B88` | `RespawnItem` (likely) | string 'RespawnItem: bad teammaster' (only user) |  |  | medium |
| `0x82542CB0` | `ClientCommand` | (clientNum): Cmd\_ArgvBuffer(0) dispatched through the client command tables 0x825F49F0 / 0x825F4A70 ... |  |  | high; names.toml; ENGINE.md |
| `0x82542F00` | `Cmd_JumpToNode_f` | checks the cheats itself |  | hook cheat\_watch.cpp | known; names.toml; addresses.h |
| `0x82543048` | `Cmd_SetViewpos_f` | checks the cheats itself |  | hook cheat\_watch.cpp | known; names.toml; addresses.h |
| `0x825432C8` | `CheatsOk` | qboolean CheatsOk(gentity\_t\*) | `qboolean (gentity_t*)` | hook cheat\_watch.cpp | known; names.toml; addresses.h; ENGINE.md |
| `0x82543568` | `G_Spawn` | allocates a 1040-byte game entity (pool or heap), constructs it with its number (-1 = next free) ... |  |  | high; names.toml |
| `0x82543C70` | `G_FindConfigstringIndex` | string '&gt;&gt;&gt; G\_FindConfigstringIndex...' overflow report listing the sound alias / shellshock ... |  |  | high; names.toml |
| `0x82545128` | `Cmd_Ufo_f` | Cmd\_Ufo\_f: client-&gt;ufo |  | hook cheat\_watch.cpp | known; names.toml; addresses.h |
| `0x825451B0` | `Cmd_Noclip_f` | Cmd\_Noclip\_f: client-&gt;noclip |  | hook cheat\_watch.cpp | known; names.toml; addresses.h |
| `0x82545248` | `Cmd_Notarget_f` | Cmd\_Notarget\_f: ent-&gt;flags ^= 2 |  | hook cheat\_watch.cpp | known; names.toml; addresses.h |
| `0x825452E0` | `Cmd_God_f` | Cmd\_God\_f: ent-&gt;flags ^= 1 |  | hook cheat\_watch.cpp | known; names.toml; addresses.h |
| `0x82545820` | `SpotWouldTelefrag` (likely) | CM\_AreaEntities over the spot's bounds; used by the spawn point selection (Q3) |  |  | medium |
| `0x82547140` | `InitTrigger` | called by SP\_trigger\_multiple and the other trigger spawners: movedir and SV\_SetBrushModel (Q3) |  |  | high; names.toml |
| `0x82549450` | `InitMover` | reads 'noise' (default '100'), 'light' and 'color' spawn vars; called by the func\_bobbing / ... |  |  | high; names.toml |
| `0x82549B00` | `G_RegisterCvars` | loops over the game cvarTable (85 x 24 bytes) with Cvar\_Get (Q3) |  |  | high; names.toml; ENGINE.md |
| `0x8254A060` | `ClientConnect` | game vmMain case 2 (GAME\_CLIENT\_CONNECT, returns a value); 'No sentient for player.' ... |  |  | high; names.toml; ENGINE.md |
| `0x8254AC60` | `SP_trigger_damage` | 'wait' / 'random' / 'accumulate' / 'threshold' spawn vars; 'trigger\_damage has random &gt;= wait' |  |  | high; names.toml |
| `0x8254B128` | `SP_trigger_friendlychain` | 'trigger\_friendlychain must target...' error, InitTrigger, SV\_LinkEntity |  |  | high; names.toml |
| `0x8254B210` | `SP_trigger_multiple` (likely) | Q3 SP\_trigger\_multiple: 1 strings in common, e.g. 'trigger\_multiple has random &gt;= wait' |  |  | medium |
| `0x8254C088` | `SP_func_pendulum` | 'speed', 'dmg', 'phase' spawn vars and g\_gravity for the swing frequency, then InitMover (Q3) |  |  | high; names.toml |
| `0x8254C2A0` | `SP_func_bobbing` | 'speed', 'height', 'phase', 'dmg' spawn vars, then InitMover (Q3) |  |  | high; names.toml |
| `0x8254D070` | `SelectRandomFurthestSpawnPoint` (likely) | Q3 SelectRandomFurthestSpawnPoint: 1 strings in common, e.g. 'Couldn't find a spawn point' |  |  | medium |
| `0x8254DFB8` | `G_ShutdownGame` (likely) | Q3 G\_ShutdownGame: 1 strings in common, e.g. 'ShutdownGame:' |  |  | medium; ENGINE.md |
| `0x8254E090` | `SelectSpawnPoint` | ClientBegin's spawn choice; calls SelectRandomFurthestSpawnPoint (Q3) |  |  | high; names.toml |
| `0x82550A10` | `Reached_BinaryMover` | string 'Reached\_BinaryMover: bad moverState' (only user) |  |  | high; names.toml |
| `0x82551568` | `FinishSpawningItem` (likely) | string 'FinishSpawningItem: %s startsolid at %s' (only user) |  |  | medium |
| `0x82557B10` | `G_BounceItem` (likely) | G\_RunItem's impact step: reflects the trajectory (g\_gravity evaluation), relinks the item (Q3) |  |  | medium |
| `0x82557F68` | `G_SpawnItem` | G\_CallSpawn's item path: 'random' / 'wait' spawn vars, then links the item (Q3) |  |  | high; names.toml |
| `0x82558308` | `LaunchItem` | Drop\_Item's call: spawns the item entity and links it with SV\_LinkEntity (Q3) |  |  | high; names.toml |
| `0x82558FB0` | `G_CallSpawnEntity` | string 'G\_CallSpawnEntity: NULL classname' (only user) |  |  | high; names.toml |
| `0x82559F10` | `G_RunItem` | G\_RunFrameForEntity's eType 2 (ET\_ITEM) / physicsObject case: trace, SV\_LinkEntity, G\_RunThink ... |  |  | high; names.toml |
| `0x8255A158` | `Drop_Item` | random spread (rand) then LaunchItem; TossClientItems and the drop-weapon paths (Q3) |  |  | high; names.toml |
| `0x8255A270` | `TossClientItems` | BG\_FindItemForWeapon of the held weapon then Drop\_Item; called by player\_die and ClientDisconnect ... |  |  | high; names.toml |
| `0x8255A370` | `ClientDisconnect` (likely) | game vmMain case 4: TossClientItems then frees the entity (Q3) |  |  | medium |
| `0x8255EF30` | `player_die` | the death handler: TossClientItems, 'INGAME\_PLAYER\_DIED', cg\_redFlashTime; reached from Cmd\_Kill\_f ... |  |  | high; names.toml |
| `0x8255F760` | `G_CallSpawn` (likely) | Q3 G\_CallSpawn: 1 strings in common, e.g. '%s doesn't have a spawn function' |  |  | medium |
| `0x82560490` | `Touch_Item` (likely) | Q3 Touch\_Item: 1 strings in common, e.g. 'Item: %i %s' |  |  | medium |
| `0x82560988` | `Cmd_Give_f` | addresses.h kGive |  | hook cheat\_watch.cpp | known; names.toml; addresses.h |
| `0x82565400` | `Bullet_Fire_Extended` | string 'Bullet\_Fire\_Extended: Too many resursions, bullet aborted' (only user) |  |  | high; names.toml |
| `0x8256A460` | `G_RunMover` | G\_RunFrameForEntity's eType 4 (ET\_MOVER) case: moves the team then G\_RunThink (Q3) |  |  | high; names.toml |
| `0x8256A660` | `G_MissileImpact` (likely) | G\_RunMissile's impact step: 'Warning Missle Weapon == 0', the explosion / damage calls (Q3) |  |  | medium |
| `0x8256B7E8` | `G_RunMissile` | G\_RunFrameForEntity's eType 3 (ET\_MISSILE) case: trajectory trace, impact handling, G\_RunThink (Q3) |  |  | high; names.toml |
| `0x8256C010` | `G_RunFrameForEntity` | G\_RunFrame's per-entity step: eType 3 (ET\_MISSILE) -&gt; G\_RunMissile, eType 2 (ET\_ITEM) or ... |  |  | high; names.toml; ENGINE.md |
| `0x8256C260` | `ClientThink_real` | clamps pmove\_msec to 8..33 with Cvar\_Set (the Q3 strings), reads g\_gravity, g\_speed, pmove\_fixed ... |  |  | high; names.toml; notes: camera-drift |
| `0x8256CFB0` | `ClientEndFrame` | called by G\_RunFrame after the entity loop and at the end of ClientSpawn (Q3) |  |  | high; names.toml |
| `0x8256D2B8` | `ClientThink` | game vmMain case 6: copies the user command and runs ClientThink\_real (Q3) |  |  | high; names.toml; ENGINE.md |
| `0x8256D3E0` | `G_RunFrame` | the game's frame: G\_UpdateCvars first, then the entity loop reaching G\_RunThink; SV\_SpawnServer ... |  |  | high; names.toml; ENGINE.md |
| `0x8256D978` | `G_InitGame` | game vmMain case 0 (GAME\_INIT, 4 arguments); sets up the level: timescale, checkpoints, the ... |  |  | high; names.toml; ENGINE.md |
| `0x8256E370` | `ClientSpawn` | ClientBegin's call: SV\_GetUsercmd, SetClientViewAngle, the bg\_viewheight cvars, ClientThink then ... |  |  | high; names.toml; ENGINE.md |
| `0x8256E878` | `ClientBegin` | game vmMain case 3 (GAME\_CLIENT\_BEGIN): SelectSpawnPoint then ClientSpawn (Q3) |  |  | high; names.toml; ENGINE.md |
| `0x8256E920` | `vmMain` | game entry point VM\_Create stores for 'game': switch over 22 GAME\_\* commands (GAME\_SHUTDOWN 1 -&gt; ... |  |  | high; names.toml; ENGINE.md |

### Shared game code and math (bg, pmove) (32)

| Address | Name | What it does | Arguments | Port use | Sources |
|---|---|---|---|---|---|
| `0x820A0520` | `AnglesToForward` (likely) | level slot 0x1AC (anglestoforward) tail-calls it with (angles, out): the forward vector only |  |  | medium |
| `0x8246B000` | `PM_Friction` (likely) | below 1 u/s it zeroes all three velocity |  |  | notes: camera-drift |
| `0x8246B1B0` | `PM_AddEvent` | pm-&gt;ps-&gt;events[eventSequence & 3] = event, eventParms 0, eventSequence++ ... |  |  | high; names.toml |
| `0x8246B398` | `BG_PlayerStateToEntityState` | copies origin, viewangles, movementDir, eFlags and the pending events from the playerState to the ... |  |  | high; names.toml |
| `0x8246B500` | `BG_EvaluateTrajectoryDelta` | the velocity variant: cos for TR\_SINE, g\_gravity for the gravity types; G\_BounceItem (Q3) |  |  | high; names.toml |
| `0x8246B848` | `BG_CanItemBeGrabbed` | string 'BG\_CanItemBeGrabbed: IT\_BAD' (only user) |  |  | high; names.toml |
| `0x8246BB08` | `BG_FindItemForWeapon` | string 'BG\_FindItemForWeapon: weapon out of range %i' (only user) |  |  | high; names.toml; ENGINE.md |
| `0x824714A8` | `PM_CrashLand` | 'landing vel: %.1f fall height', 'falling damage: %i', the bg\_fallDamage cvars; called from ... |  |  | high; names.toml |
| `0x82471C78` | `PM_AddTouchEnt` | appends an entity number to pm-&gt;touchents (max 32) unless already there; PM\_GroundTrace (Q3) |  |  | high; names.toml |
| `0x82476008` | `CM_AreaEntities` | string 'CM\_AreaEntities: MAXCOUNT' (only user) |  |  | high; names.toml |
| `0x82478430` | `BG_EvaluateTrajectory` | switch over 11 trajectory types with sin for TR\_SINE and the 0.001 time scale; Reached\_BinaryMover ... |  |  | high; names.toml |
| `0x8247CB28` | `CM_LoadMap` | string 'CM\_LoadMap: NULL name' (only user) |  |  | high; names.toml |
| `0x82486908` | `BG_SetupWeaponInfo` | prints 'Game: BG\_SetupWeaponInfo', 'Could not allocate WeaponInfo'; loads the weapon files (CG\_Init ... |  |  | high; names.toml |
| `0x8248DF18` | `PM_UpdateViewAngles` (likely) | angles, pitch clamp, ladder / |  |  | notes: camera-drift |
| `0x8248EFF8` | `PM_GroundTrace` | Q3 PM\_GroundTrace: 3 strings in common, e.g. '%i:kickoff' |  |  | high; names.toml; ENGINE.md |
| `0x82492038` | `PM_WalkMove` (likely) | moves only with velocity. Collision |  |  | notes: camera-drift |
| `0x824960D0` | `PmoveSingle` (likely) | pml at 0x82A922B0 (frametime +36 |  |  | notes: camera-drift |
| `0x82526B88` | `RadiusFromBounds` | VectorLength of the larger \|mins[i]\| / \|maxs[i]\| per axis; SV\_LinkEntity uses it (Q3) |  |  | high; names.toml |
| `0x82526BF8` | `AngleNormalize180` | (360/65536) \* ((int)(a \* 65536/360) & 65535), minus 360 above 180 (Q3) |  |  | high; names.toml |
| `0x82526C58` | `AngleSubtract` | fmod(a1 - a2, 360) folded into -180..180 (Q3) |  |  | high; names.toml |
| `0x82526E08` | `VectorNormalize2` | (v, out): out = v / \|v\| or cleared when the length is 0; returns the length (Q3) |  |  | high; names.toml |
| `0x82526E78` | `VectorNormalizeFast` | Q3's fast inverse square root on a vec3 in place: 0x5F3759DF - (bits &gt;&gt; 1), one Newton step with ... |  |  | high |
| `0x82526EF0` | `Vec2Normalize` | VectorNormalize on x and y only: returns the 2D length and scales by 1/length when non-zero (Rust ... |  |  | high |
| `0x82526F38` | `VectorNormalize` | length = sqrt(x\*x + y\*y + z\*z); scales the vector by 1/length when non-zero and returns the length ... |  |  | high; names.toml |
| `0x82526F90` | `Vec2DistanceSq` | (p1, p2): (p2.x - p1.x)^2 + (p2.y - p1.y)^2 (Rust port checked) |  |  | high |
| `0x82526FB8` | `Vec2Distance` | fsqrts of the same sum as Vec2DistanceSq (Rust port checked) |  |  | high |
| `0x82526FE0` | `Distance` | (p1, p2): \|p2 - p1\| in 3D (Q3 Distance; Rust port checked) |  |  | high |
| `0x8252A008` | `AngleDelta` | AngleNormalize180(a1 - a2) inlined (Q3) |  |  | high; names.toml |
| `0x8252A068` | `AnglesSubtract` | AngleSubtract on each of the three components into v3 (Q3) |  |  | high; names.toml |
| `0x8252E838` | `AngleVectors` | level slot 0x1B0 (anglevectors) is a lone branch to it: forward / right / up from angles ... |  |  | high; names.toml |
| `0x82531240` | `vectoangles` | level slot 0x1A0 (vectortoangles) swaps its arguments and tail-calls it: direction vector -&gt; pitch ... |  |  | high; names.toml |
| `0x82539D58` | `vtos` | '(%i %i %i)' into one of 8 rotating 32-byte buffers; Cmd\_Where\_f and FinishSpawningItem's ... |  |  | high; names.toml |

### Renderer and ngl (42)

| Address | Name | What it does | Arguments | Port use | Sources |
|---|---|---|---|---|---|
| `0x820C1B48` | `cdAepsShader::vfunc_1` | rtti cdAepsShader vtable 0x8207A458 slot 1 |  |  | high; names.toml |
| `0x820C3358` | `cdAepsShader::vfunc_2` | rtti cdAepsShader vtable 0x8207A458 slot 2 |  |  | high; names.toml |
| `0x821276D0` | `tlMemFree` (likely) | frees under tlMemAlloc's lock (0x8290E6A8) |  |  | medium |
| `0x82127938` | `tlFatalError` (likely) | prints 'TL Fatal Error: ' and the message, or calls the installed handler |  |  | medium |
| `0x821279C0` | `tlFatalErrorf` (likely) | vsprintf then tlFatalError; 'out of memory' callers |  |  | medium |
| `0x82127A20` | `tlMemAlloc` (likely) | (size, align, flags) under a lock; 'Memory allocation failed. %d bytes, %d align' |  |  | medium |
| `0x8212A110` | `live_video::kWaitJobGroup` | Waits until the group's jobs are done | `void (group)` | game\_api; typed call game\_api.cpp; calls render\_resolution.cpp | addresses.h; game\_api.h |
| `0x8212AC58` | `nglListAddQuad` | nglListAddQuad(quad\*): copies 104 bytes | `(quad*)` | hook ultrawide.cpp | names.toml; addresses.h; notes: ultrawide |
| `0x8212AEA8` | `nglSetAspect` | nglSetAspect(f1) | `(f1)` |  | addresses.h; comments |
| `0x8212AEC8` | `nglSetPerspective` | nglSetPerspective(f1 fov\_y, f2 near, f3 far) | `(f1 fov_y, f2 near, f3 far)` |  | addresses.h |
| `0x8212C700` | `nglBuildMatrices` (likely) | yScale = 1/tan(fov\_y/2), xScale = |  |  | notes: ultrawide; comments |
| `0x8212FDB8` | `R_SelectVideoMode` | (const u32\* wanted, count) | `(const u32* wanted, count)` | hook render\_resolution.cpp | known; names.toml; addresses.h; ENGINE.md |
| `0x82130530` | `D3DDevice_Reset` (likely) | (device, present params) followed by |  |  | comments |
| `0x82130808` | `live_video::kBeginNextFrame` | After each present (0x82525190 -&gt; 0x82130980), this flips the double- buffered list memory, resets ... |  | hook render\_resolution.cpp | addresses.h |
| `0x82130CC8` | `live_video::kUnbindAndIdle` | Unbinds streams and textures, then D3DDevice\_BlockUntilIdle | `void ()` | game\_api; typed call game\_api.cpp; calls render\_resolution.cpp | addresses.h; game\_api.h |
| `0x82134F70` | `nglListAddString` | (font, text, f1 x, f2 y, f3 depth, f4 x scale, f5 y scale, r8 color) | `(font, text, f1 x, f2 y, f3 depth, f4 x scale, f5 y scale, r8 color)` | hook ultrawide.cpp | names.toml; addresses.h; notes: ultrawide |
| `0x8213DAA0` | `nglListSend` (likely) | queues the list for |  |  | notes: cpu-cores; comments |
| `0x8213F810` | `live_video::kRebuildScreenTargets` | after a Reset; via R\_SetMultisample |  |  | addresses.h |
| `0x8213F8A0` | `nglReleaseTexture` | void (record\*) | `void (record*)` | game\_api; typed call game\_api.cpp; calls render\_resolution.cpp | addresses.h; game\_api.h; comments |
| `0x8214DDB8` | `nglCreateTexture` | An ngl texture record (44 bytes; IDirect3DTexture9\* at +20) | `record* (flags, format, width, height, r7, 1)` | game\_api; typed call game\_api.cpp; calls render\_resolution.cpp | addresses.h; game\_api.h; notes: shadows; comments |
| `0x82153F50` | `R_SetMultisample` | 0 none, 1 2x. Resets the device with the present parameters and remakes the screen-sized targets | `void (int type)` | game\_api; hook render\_resolution.cpp; typed call game\_api.cpp; calls render\_resolution.cpp | known; names.toml; addresses.h; game\_api.h; comments; ENGINE.md |
| `0x822F1340` | `D3DResource_Release` | D3DResource\_Release(resource) | `void (IDirect3DResource9* resource)` | game\_api; typed call game\_api.cpp; calls render\_resolution.cpp | addresses.h; game\_api.h; comments |
| `0x82417230` | `shadows::kFreeMaps` | releases all five |  |  | addresses.h |
| `0x824178A0` | `RE_EndFrame` | refexport slot 25: SCR\_UpdateScreen calls it after drawing with &time\_frontend (0x829BB210) (Q3 ... |  |  | high; names.toml |
| `0x824179A0` | `RE_StretchPic` | refexport slot 18: CG\_FillRect's draw call with x, y, w, h, s1, t1, s2, t2 and a shader (Q3) |  |  | high; names.toml |
| `0x82417A58` | `RE_SetColor` | refexport slot 17: CG\_FillRect calls it with the color and again with NULL around the stretch pic ... |  |  | high; names.toml |
| `0x82419098` | `shadows::kMakeMaps` | hooked |  | hook shadows.cpp | addresses.h |
| `0x82419408` | `live_video::kMakeFullDepth` | Makes the full-size depth texture from the video mode when there is none | `void ()` | game\_api; typed call game\_api.cpp; calls render\_resolution.cpp | addresses.h; game\_api.h |
| `0x82419700` | `RE_Shutdown` | string 'RE\_Shutdown( %i )' (only user) |  |  | high; names.toml |
| `0x8241A0A0` | `RE_BeginFrame` | refexport slot 24: SCR\_UpdateScreen calls it before SCR\_DrawScreenField (Q3 BeginFrame) |  | hook ultrawide.cpp | high; names.toml |
| `0x8241B748` | `RE_AddRefEntityToScene` | string 'RE\_AddRefEntityToScene: bad reType %i' (only user) |  |  | high; names.toml |
| `0x8241BCF0` | `AssertCvarRange` | Q3 AssertCvarRange: 3 strings in common, e.g. 'WARNING: cvar '%s' out of range (%f &gt; %f)' |  |  | high; names.toml |
| `0x8241C190` | `R_CellForCamera` | string 'R\_CellForCamera: bad model' (only user) |  |  | high; names.toml |
| `0x8241C878` | `RE_LoadWorldMap` (likely) | Q3 RE\_LoadWorldMap: 1 strings in common, e.g. 'ERROR: attempted to redundantly load world map' |  |  | medium; ENGINE.md |
| `0x82425BD0` | `shadows::kAddCaster` | (r3 ?, r4 object, ..., r9 flags\*, r10 record\*\*) -&gt; 0; hooked. Returns at once unless bit ... | `(r3 ?, r4 object, ..., r9 flags*, r10 record**)` | hook shadows.cpp | addresses.h |
| `0x82426A28` | `R_Register` (likely) | Q3 R\_Register: 22 cvar/key names in common, e.g. 'r\_offsetfactor' |  |  | medium; notes: shadows |
| `0x8242A138` | `R_Init` | Q3 R\_Init: 2 strings in common, e.g. '----- finished R\_Init -----' |  |  | high; names.toml; ENGINE.md |
| `0x8242BA20` | `RE_BeginRegistration` | refexport slot 1: calls R\_Init and copies the glconfig out (Q3) |  |  | high; names.toml |
| `0x8242BAA0` | `shadows::kCascades` |  |  |  | addresses.h |
| `0x8242C170` | `shadows::kAimAtCasters` |  |  |  | addresses.h |
| `0x82430710` | `RE_RenderScene` | RE\_RenderScene(refdef), refexport | `(refdef)` | hook ultrawide.cpp | medium; names.toml; addresses.h; notes: ultrawide; comments; ENGINE.md |
| `0x824309F0` | `GetRefAPI` | CL\_InitRef copies its result (a 160-byte refexport\_t, slot 0 RE\_Shutdown) into re ... |  |  | high; names.toml; ENGINE.md |

### Assets and archives (51)

| Address | Name | What it does | Arguments | Port use | Sources |
|---|---|---|---|---|---|
| `0x820A5190` | `InplaceAssetBankSet_DestructibleBank::deleting_dtor` | vtable slot 0: (this, flags) with the flags & 1 delete test | `(this, flags)` |  | high; names.toml |
| `0x820A5A50` | `InplaceAssetBankSet_ConfigStringBank::deleting_dtor` | vtable slot 0: (this, flags) with the flags & 1 delete test | `(this, flags)` |  | high; names.toml |
| `0x820A5AA0` | `InplaceAssetBankSet_StringTableBank::deleting_dtor` | vtable slot 0: (this, flags) with the flags & 1 delete test | `(this, flags)` |  | high; names.toml |
| `0x820A5E18` | `InplaceAssetBankSet_PhysDataBank::deleting_dtor` | vtable slot 0: (this, flags) with the flags & 1 delete test | `(this, flags)` |  | high; names.toml |
| `0x820A5E68` | `InplaceAssetBankSet_DbTablesetBank::deleting_dtor` | vtable slot 0: (this, flags) with the flags & 1 delete test | `(this, flags)` |  | high; names.toml |
| `0x820BEF20` | `AudioBankMgr::vfunc_0` | rtti AudioBankMgr vtable 0x82077564 slot 0 |  |  | high; names.toml |
| `0x820BEF70` | `InplaceAssetBankSet_GdbFileBank::deleting_dtor` | vtable slot 0: (this, flags) with the flags & 1 delete test | `(this, flags)` |  | high; names.toml |
| `0x820C0F08` | `InplaceAssetBankSet_ZoneBoundaryBank::deleting_dtor` | vtable slot 0: (this, flags) with the flags & 1 delete test | `(this, flags)` |  | high; names.toml |
| `0x820C27D8` | `InplaceAssetBankSet_XModelPartsBank::deleting_dtor` | vtable slot 0: (this, flags) with the flags & 1 delete test | `(this, flags)` |  | high; names.toml |
| `0x820C3390` | `DCGBankManager::vfunc_0` | rtti DCGBankManager vtable 0x8207A3CC slot 0 |  |  | high; names.toml |
| `0x820C34A0` | `CGBankManager::deleting_dtor` | vtable slot 0: (this, flags) with the flags & 1 delete test | `(this, flags)` |  | high; names.toml |
| `0x820C3670` | `InplaceAssetBankSet_XModelBank::deleting_dtor` | vtable slot 0: (this, flags) with the flags & 1 delete test | `(this, flags)` |  | high; names.toml |
| `0x820CA528` | `InplaceAssetBankSet_AITypeBank::deleting_dtor` | vtable slot 0: (this, flags) with the flags & 1 delete test | `(this, flags)` |  | high; names.toml |
| `0x820CA578` | `AssetBankSet::deleting_dtor` | vtable slot 0: (this, flags) with the flags & 1 delete test | `(this, flags)` |  | high; names.toml |
| `0x82128500` | `AssetType_Register` | (fourcc, version, load, unload, 0) for TEX, MESH, MAT, SKEL, FONT, AEPS, FX; links a 24-byte record ... |  |  | high; names.toml |
| `0x82128CF8` | `AssetChunk_WalkDirectory` | steps through a chunk's type records and items (project notes: the directory walker) |  |  | high; names.toml |
| `0x821344D0` | `AssetLoad_TEX_2` | load callback of asset type TEX v3 (AssetType\_Register from 0x82134608) |  |  | high; names.toml |
| `0x82134550` | `AssetUnload_TEX_2` | unload callback of asset type TEX v3 (AssetType\_Register from 0x82134608) |  |  | high; names.toml |
| `0x821351C8` | `AssetLoad_FONT_2` | load callback of asset type FONT v1 (AssetType\_Register from 0x82135380) |  |  | high; names.toml |
| `0x82135308` | `AssetUnload_FONT_2` | unload callback of asset type FONT v1 (AssetType\_Register from 0x82135380) |  |  | high; names.toml |
| `0x821445F8` | `AssetLoad_FX_2` | load callback of asset type FX v1 (AssetType\_Register from 0x82144C18) |  |  | high; names.toml |
| `0x82144AE8` | `AssetUnload_FX_2` | unload callback of asset type FX v1 (AssetType\_Register from 0x82144C18) |  |  | high; names.toml |
| `0x8214C8E0` | `AssetLoad_MESH_2` | load callback of asset type MESH v2 (AssetType\_Register from 0x8214CB48) |  |  | high; names.toml |
| `0x8214C970` | `AssetUnload_MESH_2` | unload callback of asset type MESH v2 (AssetType\_Register from 0x8214CB48) |  |  | high; names.toml |
| `0x8214CA38` | `AssetLoad_MAT_2` | load callback of asset type MAT v2 (AssetType\_Register from 0x8214CB48) |  |  | high; names.toml |
| `0x8214CAD0` | `AssetUnload_MAT_2` | unload callback of asset type MAT v2 (AssetType\_Register from 0x8214CB48) |  |  | high; names.toml |
| `0x8232DB00` | `inflate` | Q3 inflate: 5 strings in common, e.g. 'unknown compression method' |  |  | high; names.toml |
| `0x8233EE90` | `inflate_blocks` | Q3 inflate\_blocks: 4 strings in common, e.g. 'too many length or distance symbols' |  |  | high; names.toml |
| `0x82342BD8` | `inflate_trees_bits` | Q3 inflate\_trees\_bits: 2 strings in common, e.g. 'oversubscribed dynamic bit lengths tree' |  |  | high; names.toml |
| `0x82342CB8` | `inflate_trees_dynamic` | Q3 inflate\_trees\_dynamic: 5 strings in common, e.g. 'oversubscribed literal/length tree' |  |  | high; names.toml |
| `0x82416A18` | `DCGBankManager::vfunc_1` | rtti DCGBankManager vtable 0x8207A3CC slot 1 |  |  | high; names.toml |
| `0x8241A908` | `CGBankManager::ctor` | rtti: stores the CGBankManager vtable at this+0; called on the result of operator new |  |  | high; names.toml |
| `0x8241E668` | `DCGBankManager::ctor` | rtti: stores the DCGBankManager vtable at this+0; called on the result of operator new |  |  | high; names.toml |
| `0x82422A00` | `CGBankManager::vfunc_1` | rtti CGBankManager vtable 0x8207A3D8 slot 1 |  |  | high; names.toml |
| `0x82452228` | `AssetUnload_MAT` | unload callback of asset type MAT v2 (AssetType\_Register from 0x8245F3D8) |  |  | high; names.toml |
| `0x82452230` | `AssetUnload_SKEL` | unload callback of asset type SKEL v2 (AssetType\_Register from 0x8245F3D8) |  |  | high; names.toml |
| `0x82452250` | `AssetUnload_AEPS` | unload callback of asset type AEPS v1 (AssetType\_Register from 0x8245F3D8) |  |  | high; names.toml |
| `0x82455D88` | `AssetBankSet::dtor` | rtti: called first by InplaceAssetBankSet\_ConfigStringBank::deleting\_dtor (0x820A5A50) |  |  | high; names.toml |
| `0x824578D8` | `AssetLoad_MAT` | load callback of asset type MAT v2 (AssetType\_Register from 0x8245F3D8) |  |  | high; names.toml |
| `0x82457940` | `AssetUnload_FX` | unload callback of asset type FX v1 (AssetType\_Register from 0x8245F3D8) |  |  | high; names.toml |
| `0x824579B0` | `AssetLoad_SKEL` | load callback of asset type SKEL v2 (AssetType\_Register from 0x8245F3D8) |  |  | high; names.toml |
| `0x82457A80` | `AssetUnload_FONT` | unload callback of asset type FONT v1 (AssetType\_Register from 0x8245F3D8) |  |  | high; names.toml |
| `0x82457AE8` | `AssetUnload_MESH` | unload callback of asset type MESH v2 (AssetType\_Register from 0x8245F3D8) |  |  | high; names.toml |
| `0x82457B90` | `AssetUnload_TEX` | unload callback of asset type TEX v3 (AssetType\_Register from 0x8245F3D8) |  |  | high; names.toml |
| `0x8245BBB8` | `AssetLoad_FX` | load callback of asset type FX v1 (AssetType\_Register from 0x8245F3D8) |  |  | high; names.toml |
| `0x8245BD30` | `AssetLoad_FONT` | load callback of asset type FONT v1 (AssetType\_Register from 0x8245F3D8) |  |  | high; names.toml |
| `0x8245BEF0` | `AssetLoad_AEPS` | load callback of asset type AEPS v1 (AssetType\_Register from 0x8245F3D8) |  |  | high; names.toml |
| `0x8245BF68` | `AssetLoad_MESH` | load callback of asset type MESH v2 (AssetType\_Register from 0x8245F3D8) |  |  | high; names.toml |
| `0x8245C018` | `AssetLoad_TEX` | load callback of asset type TEX v3 (AssetType\_Register from 0x8245F3D8) |  |  | high; names.toml; ENGINE.md |
| `0x82478CE8` | `AudioBankMgr::ctor` | rtti: stores the AudioBankMgr vtable at this+0; called on the result of operator new |  |  | high; names.toml |
| `0x824950A8` | `AudioBankMgr::vfunc_1` | rtti AudioBankMgr vtable 0x82077564 slot 1 |  |  | high; names.toml |

### Front end: menus and message boxes (558)

| Address | Name | What it does | Arguments | Port use | Sources |
|---|---|---|---|---|---|
| `0x820A5730` | `DialogueManager::deleting_dtor` | vtable slot 0: (this, flags) with the flags & 1 delete test | `(this, flags)` |  | high; names.toml |
| `0x820ACA38` | `FEText::ctor` | FEText(this), 112 bytes | `(this)` | game\_api; typed call game\_api.cpp; calls menu\_fov.cpp, pc\_menu.cpp | high; names.toml; addresses.h; game\_api.h; comments |
| `0x820ACB28` | `FEText::vfunc_1` | rtti FEText vtable 0x82070C84 slot 1 (also in 1 derived vtables) |  |  | high; names.toml |
| `0x820ACB40` | `FEText::vfunc_20` | rtti FEText vtable 0x820714A4 slot 20 (also in 2 derived vtables) |  |  | high; names.toml |
| `0x820ACB58` | `FEText::vfunc_21` | rtti FEText vtable 0x820714A4 slot 21 (also in 2 derived vtables) |  |  | high; names.toml |
| `0x820ACB70` | `FEText::vfunc_26` | rtti FEText vtable 0x820714A4 slot 26 (also in 2 derived vtables) |  |  | high; names.toml |
| `0x820ACBA0` | `FEText::vfunc_28` | rtti FEText vtable 0x820714A4 slot 28 (also in 1 derived vtables) |  |  | high; names.toml |
| `0x820ACC00` | `FEText::vfunc_27` | rtti FEText vtable 0x820714A4 slot 27 (also in 1 derived vtables) |  |  | high; names.toml |
| `0x820ACC60` | `FEText::vfunc_29` | rtti FEText vtable 0x820714A4 slot 29 (also in 2 derived vtables) |  |  | high; names.toml |
| `0x820ACCC0` | `FEText::vfunc_30` | rtti FEText vtable 0x820714A4 slot 30 (also in 2 derived vtables) |  |  | high; names.toml |
| `0x820ACCF8` | `FEText::vfunc_31` | rtti FEText vtable 0x820714A4 slot 31 (also in 2 derived vtables) |  |  | high; names.toml |
| `0x820ACD00` | `FEText::vfunc_34` | rtti FEText vtable 0x820714A4 slot 34 (also in 1 derived vtables) |  |  | high; names.toml |
| `0x820ACD90` | `FEText::vfunc_35` | rtti FEText vtable 0x820714A4 slot 35 (also in 2 derived vtables) |  |  | high; names.toml |
| `0x820ACDD0` | `FEText::vfunc_36` | rtti FEText vtable 0x820714A4 slot 36 (also in 1 derived vtables) |  |  | high; names.toml |
| `0x820ACDD8` | `FEText::vfunc_37` | rtti FEText vtable 0x820714A4 slot 37 (also in 1 derived vtables) |  |  | high; names.toml |
| `0x820ACE10` | `FEText::vfunc_38` | rtti FEText vtable 0x820714A4 slot 38 (also in 2 derived vtables) |  |  | high; names.toml |
| `0x820ACE18` | `FEText::vfunc_39` | rtti FEText vtable 0x820714A4 slot 39 (also in 2 derived vtables) |  |  | high; names.toml |
| `0x820ACE20` | `FEText::vfunc_40` | rtti FEText vtable 0x820714A4 slot 40 (also in 2 derived vtables) |  |  | high; names.toml |
| `0x820ACE48` | `FEText::vfunc_41` | rtti FEText vtable 0x820714A4 slot 41 (also in 2 derived vtables) |  |  | high; names.toml |
| `0x820ACE50` | `FEText::vfunc_16` | rtti FEText vtable 0x820714A4 slot 16 (also in 2 derived vtables) |  |  | high; names.toml |
| `0x820ACE60` | `FEText::vfunc_44` | rtti FEText vtable 0x820714A4 slot 44 (also in 2 derived vtables) |  |  | high; names.toml |
| `0x820ACE88` | `FEText::vfunc_46` | rtti FEText vtable 0x820714A4 slot 46 (also in 2 derived vtables) |  |  | high; names.toml |
| `0x820ACEC0` | `FEText::vfunc_47` | rtti FEText vtable 0x820714A4 slot 47 (also in 2 derived vtables) |  |  | high; names.toml |
| `0x820ACEF8` | `FEText::vfunc_48` | rtti FEText vtable 0x820714A4 slot 48 (also in 2 derived vtables) |  |  | high; names.toml |
| `0x820ACF00` | `FEText::vfunc_49` | rtti FEText vtable 0x820714A4 slot 49 (also in 2 derived vtables) |  |  | high; names.toml |
| `0x820ACF08` | `FEText::vfunc_51` | rtti FEText vtable 0x820714A4 slot 51 (also in 2 derived vtables) |  |  | high; names.toml |
| `0x820ACF10` | `FEText::vfunc_52` | rtti FEText vtable 0x820714A4 slot 52 (also in 2 derived vtables) |  |  | high; names.toml |
| `0x820ACF20` | `FEText::vfunc_53` | rtti FEText vtable 0x820714A4 slot 53 (also in 2 derived vtables) |  |  | high; names.toml |
| `0x820ACF28` | `FEText::vfunc_17` | rtti FEText vtable 0x820714A4 slot 17 (also in 2 derived vtables) |  |  | high; names.toml |
| `0x820ACF38` | `FEText::vfunc_54` | rtti FEText vtable 0x820714A4 slot 54 (also in 2 derived vtables) |  |  | high; names.toml |
| `0x820ACF48` | `FEText::vfunc_55` | rtti FEText vtable 0x820714A4 slot 55 (also in 2 derived vtables) |  |  | high; names.toml |
| `0x820ACF58` | `FEText::vfunc_56` | rtti FEText vtable 0x820714A4 slot 56 (also in 2 derived vtables) |  |  | high; names.toml |
| `0x820ACF68` | `FEText::vfunc_57` | rtti FEText vtable 0x820714A4 slot 57 (also in 2 derived vtables) |  |  | high; names.toml |
| `0x820ACF78` | `FEText::vfunc_59` | rtti FEText vtable 0x820714A4 slot 59 (also in 2 derived vtables) |  |  | high; names.toml |
| `0x820ACF88` | `FEText::vfunc_62` | rtti FEText vtable 0x820714A4 slot 62 (also in 2 derived vtables) |  |  | high; names.toml |
| `0x820ACFA8` | `FEText::vfunc_63` | rtti FEText vtable 0x820714A4 slot 63 (also in 2 derived vtables) |  |  | high; names.toml |
| `0x820ACFF0` | `FEMultiLineText::vfunc_58` | rtti FEMultiLineText vtable 0x82070C84 slot 58 |  |  | high; names.toml |
| `0x820ACFF8` | `FEMultiLineText::vfunc_75` | rtti FEMultiLineText vtable 0x82070C84 slot 75 |  |  | high; names.toml |
| `0x820AD000` | `FEMultiLineText::vfunc_76` | rtti FEMultiLineText vtable 0x82070C84 slot 76 |  |  | high; names.toml |
| `0x820AD008` | `FEMultiLineText::vfunc_66` | rtti FEMultiLineText vtable 0x82070C84 slot 66 |  |  | high; names.toml |
| `0x820AD020` | `FEMultiLineText::vfunc_23` | rtti FEMultiLineText vtable 0x82070C84 slot 23 |  |  | high; names.toml |
| `0x820AD058` | `FEMultiLineText::vfunc_24` | rtti FEMultiLineText vtable 0x82070C84 slot 24 |  |  | high; names.toml |
| `0x820AD090` | `FEMultiLineText::vfunc_28` | rtti FEMultiLineText vtable 0x82070C84 slot 28 |  |  | high; names.toml |
| `0x820AD108` | `FEMultiLineText::vfunc_27` | rtti FEMultiLineText vtable 0x82070C84 slot 27 |  |  | high; names.toml |
| `0x820AD180` | `FEMultiLineText::vfunc_80` | rtti FEMultiLineText vtable 0x82070C84 slot 80 |  |  | high; names.toml |
| `0x820AD188` | `FEMultiLineText::vfunc_81` | rtti FEMultiLineText vtable 0x82070C84 slot 81 |  |  | high; names.toml |
| `0x820AD190` | `FEMultiLineText::vfunc_83` | rtti FEMultiLineText vtable 0x82070C84 slot 83 |  |  | high; names.toml |
| `0x820AD198` | `FEMultiLineText::vfunc_84` | rtti FEMultiLineText vtable 0x82070C84 slot 84 |  |  | high; names.toml |
| `0x820AD1B0` | `FEMultiLineText::vfunc_88` | rtti FEMultiLineText vtable 0x82070C84 slot 88 |  |  | high; names.toml |
| `0x820AD1B8` | `FEMultiLineText::vfunc_0` | rtti FEMultiLineText vtable 0x82070C84 slot 0 |  | hook button\_prompts.cpp | high; names.toml |
| `0x820AD238` | `PanelQuad::vfunc_23` | rtti PanelQuad vtable 0x82070E04 slot 23 |  |  | high; names.toml |
| `0x820AD258` | `PanelQuad::vfunc_27` | rtti PanelQuad vtable 0x82070E04 slot 27 |  |  | high; names.toml |
| `0x820AD280` | `PanelQuad::vfunc_26` | rtti PanelQuad vtable 0x82070E04 slot 26 |  |  | high; names.toml |
| `0x820AD2A8` | `PanelQuad::vfunc_25` | rtti PanelQuad vtable 0x82070E04 slot 25 |  |  | high; names.toml |
| `0x820AD2C0` | `PanelQuad::vfunc_29` | rtti PanelQuad vtable 0x82070E04 slot 29 |  |  | high; names.toml |
| `0x820AD2D8` | `PanelQuad::vfunc_33` | rtti PanelQuad vtable 0x82070E04 slot 33 |  |  | high; names.toml |
| `0x820AD340` | `PanelQuad::vfunc_36` | rtti PanelQuad vtable 0x82070E04 slot 36 |  |  | high; names.toml |
| `0x820AD390` | `PanelQuad::vfunc_39` | rtti PanelQuad vtable 0x82070E04 slot 39 |  |  | high; names.toml |
| `0x820AD3B0` | `PanelQuad::vfunc_42` | rtti PanelQuad vtable 0x82070E04 slot 42 |  |  | high; names.toml |
| `0x820AD3C8` | `PanelQuad::vfunc_43` | rtti PanelQuad vtable 0x82070E04 slot 43 |  |  | high; names.toml |
| `0x820AD3D8` | `PanelQuad::vfunc_45` | rtti PanelQuad vtable 0x82070E04 slot 45 |  |  | high; names.toml |
| `0x820AD3E8` | `PanelQuad::vfunc_17` | rtti PanelQuad vtable 0x82070E04 slot 17 |  |  | high; names.toml |
| `0x820AD418` | `PanelQuad::vfunc_47` | rtti PanelQuad vtable 0x82070E04 slot 47 |  |  | high; names.toml |
| `0x820AD460` | `PanelQuad::vfunc_48` | rtti PanelQuad vtable 0x82070E04 slot 48 |  |  | high; names.toml |
| `0x820AD4A8` | `PanelQuad::vfunc_49` | rtti PanelQuad vtable 0x82070E04 slot 49 |  |  | high; names.toml |
| `0x820AD5E0` | `FEMenuEntry::vfunc_3` | rtti FEMenuEntry vtable 0x82071034 slot 3 |  |  | high; names.toml |
| `0x820AD610` | `FEMenuEntry::vfunc_4` | rtti FEMenuEntry vtable 0x82071034 slot 4 |  |  | high; names.toml |
| `0x820AD638` | `FEMenuEntry::vfunc_5` | rtti FEMenuEntry vtable 0x82071034 slot 5 |  |  | high; names.toml |
| `0x820AD650` | `FEMenuEntry::vfunc_6` | rtti FEMenuEntry vtable 0x82071034 slot 6 |  |  | high; names.toml |
| `0x820AD668` | `FEMenuEntry::vfunc_9` | rtti FEMenuEntry vtable 0x82071034 slot 9 |  |  | high; names.toml |
| `0x820AD670` | `FEMenuEntry::vfunc_10` | rtti FEMenuEntry vtable 0x82071034 slot 10 |  |  | high; names.toml |
| `0x820AD688` | `FEMenuEntry::vfunc_11` | rtti FEMenuEntry vtable 0x82071034 slot 11 |  |  | high; names.toml |
| `0x820AD6A0` | `FEMenuEntry::vfunc_12` | rtti FEMenuEntry vtable 0x82071034 slot 12 |  |  | high; names.toml |
| `0x820AD6B8` | `FEMenuEntry::vfunc_14` | rtti FEMenuEntry vtable 0x82071034 slot 14 |  |  | high; names.toml |
| `0x820AD718` | `FEMenuEntry::vfunc_13` | rtti FEMenuEntry vtable 0x82071034 slot 13 |  |  | high; names.toml |
| `0x820AD730` | `FEMenuEntry::vfunc_15` | rtti FEMenuEntry vtable 0x82071034 slot 15 |  |  | high; names.toml |
| `0x820AD770` | `FEMenuEntry::vfunc_16` | rtti FEMenuEntry vtable 0x82071034 slot 16 |  |  | high; names.toml |
| `0x820AD788` | `FEMenuEntry::vfunc_17` | rtti FEMenuEntry vtable 0x82071034 slot 17 |  |  | high; names.toml |
| `0x820AD7A0` | `FEMenuEntry::vfunc_18` | rtti FEMenuEntry vtable 0x82071034 slot 18 |  |  | high; names.toml |
| `0x820AD7B8` | `FEMenuEntry::vfunc_19` | rtti FEMenuEntry vtable 0x82071034 slot 19 |  |  | high; names.toml |
| `0x820AD7D0` | `FEMenuEntry::vfunc_20` | rtti FEMenuEntry vtable 0x82071034 slot 20 |  |  | high; names.toml |
| `0x820AD7E8` | `FEMenuEntry::vfunc_21` | rtti FEMenuEntry vtable 0x82071034 slot 21 |  |  | high; names.toml |
| `0x820AD800` | `FEMenuEntry::vfunc_22` | rtti FEMenuEntry vtable 0x82071034 slot 22 |  |  | high; names.toml |
| `0x820AD818` | `FEMenuEntry::vfunc_24` | rtti FEMenuEntry vtable 0x82071034 slot 24 |  |  | high; names.toml |
| `0x820AD830` | `FEMenuEntry::vfunc_23` | rtti FEMenuEntry vtable 0x82071034 slot 23 |  |  | high; names.toml |
| `0x820AD848` | `FEMenuEntry::vfunc_25` | rtti FEMenuEntry vtable 0x82071034 slot 25 |  |  | high; names.toml |
| `0x820AD860` | `FEMenuEntry::vfunc_26` | rtti FEMenuEntry vtable 0x82071034 slot 26 |  |  | high; names.toml |
| `0x820AD8A0` | `FEMenuEntry::vfunc_27` | rtti FEMenuEntry vtable 0x82071034 slot 27 |  |  | high; names.toml |
| `0x820AD8B8` | `FEMenuEntry::vfunc_28` | rtti FEMenuEntry vtable 0x82071034 slot 28 |  |  | high; names.toml |
| `0x820AD8D0` | `FEMenuEntry::vfunc_29` | rtti FEMenuEntry vtable 0x82071034 slot 29 |  |  | high; names.toml |
| `0x820AD8E8` | `FEMenuEntry::vfunc_30` | rtti FEMenuEntry vtable 0x82071034 slot 30 |  |  | high; names.toml |
| `0x820AD900` | `FEMenuEntry::vfunc_31` | rtti FEMenuEntry vtable 0x82071034 slot 31 |  |  | high; names.toml |
| `0x820AD918` | `FEMenuEntry::vfunc_32` | rtti FEMenuEntry vtable 0x82071034 slot 32 |  |  | high; names.toml |
| `0x820AD930` | `FEMenuEntry::vfunc_33` | rtti FEMenuEntry vtable 0x82071034 slot 33 |  |  | high; names.toml |
| `0x820AD938` | `FEMenuEntry::vfunc_34` | rtti FEMenuEntry vtable 0x82071034 slot 34 |  |  | high; names.toml |
| `0x820AD978` | `FEMenuEntry::vfunc_35` | rtti FEMenuEntry vtable 0x82071034 slot 35 |  |  | high; names.toml |
| `0x820AD990` | `FEMenuEntry::vfunc_36` | rtti FEMenuEntry vtable 0x82071034 slot 36 |  |  | high; names.toml |
| `0x820AD9A8` | `FEMenuEntry::vfunc_0` | rtti FEMenuEntry vtable 0x82071034 slot 0 |  |  | high; names.toml |
| `0x820ADA30` | `FEMenu::vfunc_6` | rtti FEMenu vtable 0x82072A8C slot 6 (also in 23 derived vtables) |  |  | high; names.toml |
| `0x820ADA38` | `FEMenu::vfunc_12` | rtti FEMenu vtable 0x82072A8C slot 12 (also in 23 derived vtables) |  |  | high; names.toml |
| `0x820ADA50` | `FEMenu::vfunc_18` | rtti FEMenu vtable 0x82072A8C slot 18 (also in 22 derived vtables) |  |  | high; names.toml |
| `0x820ADA60` | `FEMenu::vfunc_20` | rtti FEMenu vtable 0x82072434 slot 20 (also in 13 derived vtables) |  |  | high; names.toml |
| `0x820ADA70` | `FEMenu::MoveUp` (likely) | vtable slot 25; meaning from Options &gt; Gameplay vtbl[25] move selection up |  |  | medium |
| `0x820ADA80` | `FEMenu::MoveDown` (likely) | vtable slot 26; meaning from Options &gt; Gameplay vtbl[26] move selection down |  |  | medium |
| `0x820ADA90` | `FEMenu::vfunc_43` | rtti FEMenu vtable 0x82072A8C slot 43 (also in 23 derived vtables) |  |  | high; names.toml |
| `0x820ADA98` | `FEMenu::vfunc_47` | rtti FEMenu vtable 0x82072A8C slot 47 (also in 23 derived vtables) |  |  | high; names.toml |
| `0x820ADAA0` | `FEMenu::vfunc_48` | rtti FEMenu vtable 0x82072A8C slot 48 (also in 23 derived vtables) |  |  | high; names.toml |
| `0x820ADAA8` | `FEMenu::vfunc_49` | rtti FEMenu vtable 0x82072A8C slot 49 (also in 23 derived vtables) |  |  | high; names.toml |
| `0x820ADAB0` | `FEMenu::vfunc_50` | rtti FEMenu vtable 0x82072A8C slot 50 (also in 23 derived vtables) |  |  | high; names.toml |
| `0x820ADAC0` | `FEMenu::OnLeft` (likely) | the default: the page has no left/right |  | refers to pc\_menu.cpp | medium; addresses.h |
| `0x820ADAD0` | `FEMenu::OnRight` (likely) | vtable slot 28; meaning from Options &gt; Gameplay vtbl[28] = Toggle(this, 1) |  |  | medium |
| `0x820ADAE8` | `FEMenuSystem::vfunc_16` | rtti FEMenuSystem vtable 0x820719D4 slot 16 (also in 3 derived vtables) |  |  | high; names.toml |
| `0x820ADAF0` | `FEMenuSystem::vfunc_0` | rtti FEMenuSystem vtable 0x82070FC4 slot 0 |  |  | high; names.toml |
| `0x820AE060` | `ControllerDisconnectedMenu::vfunc_3` | rtti ControllerDisconnectedMenu vtable 0x8207163C slot 3 |  |  | high; names.toml |
| `0x820AE178` | `TitleMenu::vfunc_24` | rtti TitleMenu vtable 0x82071724 slot 24 |  |  | high; names.toml |
| `0x820AE180` | `TitleMenu::vfunc_3` | rtti TitleMenu vtable 0x82071724 slot 3 |  |  | high; names.toml |
| `0x820AE200` | `InGameMenuSystem::vfunc_0` | rtti InGameMenuSystem vtable 0x82071884 slot 0 |  |  | high; names.toml |
| `0x820AE260` | `DialogMenu::vfunc_3` | rtti DialogMenu vtable 0x820718EC slot 3 |  |  | high; names.toml |
| `0x820AE2E0` | `AdvancedOptionsMenu::OnLeft` (likely) | vtable slot 27; meaning from Options &gt; Gameplay vtbl[27] = Toggle(this, 0) |  |  | medium |
| `0x820AE2E8` | `AdvancedOptionsMenu::OnRight` (likely) | vtable slot 28; meaning from Options &gt; Gameplay vtbl[28] = Toggle(this, 1) |  |  | medium |
| `0x820AE568` | `FEText::vfunc_0` | rtti FEText vtable 0x820714A4 slot 0 (also in 1 derived vtables) |  |  | high; names.toml |
| `0x820AE5B8` | `PanelQuad::vfunc_0` | rtti PanelQuad vtable 0x82070E04 slot 0 |  |  | high; names.toml |
| `0x820AE830` | `OptionsGameplayMenu::OnLeft` (likely) | vtable slot 27; meaning from Options &gt; Gameplay vtbl[27] = Toggle(this, 0) |  |  | medium |
| `0x820AE838` | `OptionsGameplayMenu::OnRight` (likely) | vtable slot 28; meaning from Options &gt; Gameplay vtbl[28] = Toggle(this, 1) |  |  | medium |
| `0x820AE840` | `OptionsControlsMenu::OnLeft` (likely) | vtable slot 27; meaning from Options &gt; Gameplay vtbl[27] = Toggle(this, 0) |  |  | medium |
| `0x820AE848` | `OptionsControlsMenu::OnRight` (likely) | vtable slot 28; meaning from Options &gt; Gameplay vtbl[28] = Toggle(this, 1) |  |  | medium |
| `0x820AE860` | `OptionsStickMenu::OnLeft` (likely) | vtable slot 27; meaning from Options &gt; Gameplay vtbl[27] = Toggle(this, 0) |  |  | medium |
| `0x820AE868` | `OptionsStickMenu::OnRight` (likely) | vtable slot 28; meaning from Options &gt; Gameplay vtbl[28] = Toggle(this, 1) |  |  | medium |
| `0x820AE870` | `OptionsButtonMenu::OnLeft` (likely) | vtable slot 27; meaning from Options &gt; Gameplay vtbl[27] = Toggle(this, 0) |  |  | medium |
| `0x820AE878` | `OptionsButtonMenu::OnRight` (likely) | vtable slot 28; meaning from Options &gt; Gameplay vtbl[28] = Toggle(this, 1) |  |  | medium |
| `0x820AE880` | `BonusContentMenu::vfunc_51` | rtti BonusContentMenu vtable 0x82072604 slot 51 |  |  | high; names.toml |
| `0x820AE898` | `BonusContentMenu::vfunc_52` | rtti BonusContentMenu vtable 0x82072604 slot 52 |  |  | high; names.toml |
| `0x820AE8B0` | `BonusSelectMenu::vfunc_31` | rtti BonusSelectMenu vtable 0x820727D4 slot 31 |  |  | high; names.toml |
| `0x820AE8C8` | `DifficultyMenu::MoveUp` (likely) | vtable slot 25; meaning from Options &gt; Gameplay vtbl[25] move selection up |  |  | medium |
| `0x820AE908` | `DifficultyMenu::MoveDown` (likely) | vtable slot 26; meaning from Options &gt; Gameplay vtbl[26] move selection down |  |  | medium |
| `0x820AE978` | `FEManager::vfunc_0` | rtti FEManager vtable 0x82071028 slot 0 |  |  | high; names.toml |
| `0x820AE9C8` | `FEMenu::vfunc_3` | rtti FEMenu vtable 0x820710EC slot 3 (also in 1 derived vtables) |  |  | high; names.toml |
| `0x820AEA28` | `FrontEndMenuSystem::vfunc_0` | rtti FrontEndMenuSystem vtable 0x8207180C slot 0 |  |  | high; names.toml |
| `0x820AEA78` | `DialogMenuSystem::vfunc_0` | rtti DialogMenuSystem vtable 0x820719D4 slot 0 |  |  | high; names.toml |
| `0x820AEB28` | `PauseMenu::vfunc_3` | rtti PauseMenu vtable 0x82071A3C slot 3 |  |  | high; names.toml |
| `0x820AEB78` | `AdvancedOptionsMenu::vfunc_3` | rtti AdvancedOptionsMenu vtable 0x82071B24 slot 3 |  |  | high; names.toml |
| `0x820AEBC8` | `MainMenu::vfunc_3` | rtti MainMenu vtable 0x82071DDC slot 3 |  |  | high; names.toml |
| `0x820AEC18` | `SPMainMenu::vfunc_3` | rtti SPMainMenu vtable 0x82071EC4 slot 3 |  |  | high; names.toml |
| `0x820AEC68` | `OptionsGameplayMenu::vfunc_3` | rtti OptionsGameplayMenu vtable 0x82071FAC slot 3 |  |  | high; names.toml |
| `0x820AECB8` | `OptionsControlsMenu::vfunc_3` | rtti OptionsControlsMenu vtable 0x82072094 slot 3 |  |  | high; names.toml |
| `0x820AED58` | `OptionsStickMenu::vfunc_3` | rtti OptionsStickMenu vtable 0x82072264 slot 3 |  |  | high; names.toml |
| `0x820AEDA8` | `OptionsButtonMenu::vfunc_3` | rtti OptionsButtonMenu vtable 0x8207234C slot 3 |  |  | high; names.toml |
| `0x820AEDF8` | `BonusMainMenu::vfunc_3` | rtti BonusMainMenu vtable 0x8207251C slot 3 |  |  | high; names.toml |
| `0x820AEE48` | `BonusContentMenu::vfunc_3` | rtti BonusContentMenu vtable 0x82072604 slot 3 |  |  | high; names.toml |
| `0x820AEE98` | `BonusConceptArtMenu::vfunc_3` | rtti BonusConceptArtMenu vtable 0x820726EC slot 3 |  |  | high; names.toml |
| `0x820AEEE8` | `BonusSelectMenu::vfunc_3` | rtti BonusSelectMenu vtable 0x820727D4 slot 3 |  |  | high; names.toml |
| `0x820AEF38` | `GammaScreenMenu::vfunc_3` | rtti GammaScreenMenu vtable 0x82072434 slot 3 |  |  | high; names.toml |
| `0x820AEF88` | `DifficultyMenu::vfunc_3` | rtti DifficultyMenu vtable 0x820728BC slot 3 |  |  | high; names.toml |
| `0x820AEFD8` | `ProfileEditMenu::vfunc_3` | rtti ProfileEditMenu vtable 0x820729A4 slot 3 |  |  | high; names.toml |
| `0x820AF028` | `SuccessFailMenu::vfunc_3` | rtti SuccessFailMenu vtable 0x82071CF4 slot 3 |  |  | high; names.toml |
| `0x820AF078` | `ChapterSelectMenu::vfunc_3` | rtti ChapterSelectMenu vtable 0x82072A8C slot 3 |  |  | high; names.toml |
| `0x820AF348` | `ObjectiveMenu::vfunc_3` | rtti ObjectiveMenu vtable 0x82071C0C slot 3 |  |  | high; names.toml |
| `0x821CCC28` | `FEText::vfunc_50` | rtti FEText vtable 0x820714A4 slot 50 (also in 2 derived vtables) |  |  | high; names.toml |
| `0x823F86D0` | `msgbox::kCloseCallback` | return 1: the button just closes the box |  | refers to multiplayer.cpp | addresses.h |
| `0x823FBC58` | `Profile_LoadSettings` | Reads the settings from the profile save (0x823FBC58) |  | hook aim\_assist.cpp | known; names.toml; addresses.h |
| `0x824A5978` | `prompts::kStringFromChars` | The game's refcounted string: chars at +12. (String\*\* out, const char\*) |  | calls button\_prompts.cpp | addresses.h |
| `0x824BBA80` | `ObjectiveMenu::vfunc_38` | rtti ObjectiveMenu vtable 0x82071C0C slot 38 |  |  | high; names.toml |
| `0x824BBB00` | `ObjectiveMenu::vfunc_11` | rtti ObjectiveMenu vtable 0x82071C0C slot 11 |  |  | high; names.toml |
| `0x824BBE98` | `ChapterSelectMenu::vfunc_30` | rtti ChapterSelectMenu vtable 0x82072A8C slot 30 |  |  | high; names.toml |
| `0x824BC028` | `ProfileEditMenu::vfunc_55` | rtti ProfileEditMenu vtable 0x820729A4 slot 55 |  |  | high; names.toml |
| `0x824BC070` | `ProfileEditMenu::vfunc_19` | vtbl[19] (this, item); hooked | `(this, item)` | hook menu\_video.cpp | high; names.toml; addresses.h |
| `0x824BC180` | `DifficultyMenu::vfunc_30` | rtti DifficultyMenu vtable 0x820728BC slot 30 |  |  | high; names.toml |
| `0x824BC1A0` | `DifficultyMenu::vfunc_55` | rtti DifficultyMenu vtable 0x820728BC slot 55 |  |  | high; names.toml |
| `0x824BC200` | `GammaScreenMenu::vfunc_55` | rtti GammaScreenMenu vtable 0x82072434 slot 55 |  |  | high; names.toml |
| `0x824BC248` | `GammaScreenMenu::OnRight` (likely) | vtable slot 28; meaning from Options &gt; Gameplay vtbl[28] = Toggle(this, 1) |  |  | medium |
| `0x824BC2C0` | `GammaScreenMenu::OnLeft` (likely) | vtable slot 27; meaning from Options &gt; Gameplay vtbl[27] = Toggle(this, 0) |  |  | medium |
| `0x824BC338` | `GammaScreenMenu::vfunc_30` | rtti GammaScreenMenu vtable 0x82072434 slot 30 |  |  | high; names.toml |
| `0x824BC500` | `BonusSelectMenu::OnRight` (likely) | vtable slot 28; meaning from Options &gt; Gameplay vtbl[28] = Toggle(this, 1) |  |  | medium |
| `0x824BC5C8` | `BonusSelectMenu::OnLeft` (likely) | vtable slot 27; meaning from Options &gt; Gameplay vtbl[27] = Toggle(this, 0) |  |  | medium |
| `0x824BC690` | `BonusSelectMenu::vfunc_30` | rtti BonusSelectMenu vtable 0x820727D4 slot 30 |  |  | high; names.toml |
| `0x824BC6F0` | `BonusConceptArtMenu::vfunc_30` | rtti BonusConceptArtMenu vtable 0x820726EC slot 30 |  |  | high; names.toml |
| `0x824BC9C0` | `BonusMainMenu::vfunc_55` | rtti BonusMainMenu vtable 0x8207251C slot 55 |  |  | high; names.toml |
| `0x824BCA20` | `OptionsButtonMenu::vfunc_55` | rtti OptionsButtonMenu vtable 0x8207234C slot 55 |  |  | high; names.toml |
| `0x824BCCD0` | `OptionsStickMenu::vfunc_55` | rtti OptionsStickMenu vtable 0x82072264 slot 55 |  |  | high; names.toml |
| `0x824BCE68` | `OptionsStickMenu::Refresh` (likely) | vtable slot 21; meaning from Options &gt; Gameplay vtbl[21] Refresh(this) | `(this)` |  | medium |
| `0x824BD028` | `OptionsGameplayMenu::vfunc_55` | rtti OptionsGameplayMenu vtable 0x82071FAC slot 55 |  |  | high; names.toml |
| `0x824BD160` | `OptionsGameplayMenu::Toggle` | (this, dir) via vtbl[27]/[28] | `(this, dir)` | hook menu\_fov.cpp | known; names.toml; addresses.h |
| `0x824BD2C0` | `SPMainMenu::vfunc_55` | rtti SPMainMenu vtable 0x82071EC4 slot 55 |  |  | high; names.toml |
| `0x824BD308` | `SPMainMenu::vfunc_30` | rtti SPMainMenu vtable 0x82071EC4 slot 30 |  |  | high; names.toml |
| `0x824BD370` | `MainMenu::vfunc_30` | vtbl[30]: to the title |  |  | high; names.toml; addresses.h |
| `0x824BD3B0` | `MainMenu::vfunc_55` | rtti MainMenu vtable 0x82071DDC slot 55 |  |  | high; names.toml |
| `0x824BD3F8` | `fe::adv::kSyncValueHighlight` | (this): after a move | `(this)` | calls menu\_fov.cpp | addresses.h |
| `0x824BD660` | `AdvancedOptionsMenu::vfunc_24` | rtti AdvancedOptionsMenu vtable 0x82071B24 slot 24 |  |  | high; names.toml |
| `0x824BD6F0` | `PauseMenu::vfunc_55` | rtti PauseMenu vtable 0x82071A3C slot 55 |  |  | high; names.toml |
| `0x824BD750` | `main_menu::kNoCallback` | return 1: closes the box |  | refers to pc\_menu.cpp | addresses.h |
| `0x824BD770` | `InGameMenuSystem::vfunc_20` | rtti InGameMenuSystem vtable 0x82071884 slot 20 |  |  | high; names.toml |
| `0x824BD7D0` | `InGameMenuSystem::vfunc_19` | rtti InGameMenuSystem vtable 0x82071884 slot 19 |  |  | high; names.toml |
| `0x824BD9B8` | `DialogMenu::vfunc_55` | rtti DialogMenu vtable 0x820718EC slot 55 |  |  | high; names.toml |
| `0x824BDA00` | `DialogMenu::vfunc_10` | rtti DialogMenu vtable 0x820718EC slot 10 |  |  | high; names.toml |
| `0x824BDA08` | `DialogMenu::vfunc_14` | rtti DialogMenu vtable 0x820718EC slot 14 |  |  | high; names.toml |
| `0x824BDDE8` | `UpdateSelection` (likely) | Puts the highlight on button selected (system-&gt;buttons, msgbox::kSysButtons) | `void (buttons, int selected)` | game\_api; typed call game\_api.cpp; calls multiplayer.cpp, pc\_menu.cpp | addresses.h; game\_api.h; notes: menus-pass |
| `0x824BE2F0` | `FEMenuSystem::vfunc_20` | rtti FEMenuSystem vtable 0x8207180C slot 20 (also in 1 derived vtables) |  |  | high; names.toml |
| `0x824BE338` | `FEMenuSystem::vfunc_23` | rtti FEMenuSystem vtable 0x82071884 slot 23 (also in 2 derived vtables) |  |  | high; names.toml |
| `0x824BE348` | `FEMenuSystem::vfunc_22` | rtti FEMenuSystem vtable 0x82071884 slot 22 (also in 2 derived vtables) |  |  | high; names.toml |
| `0x824BE358` | `FEMenuSystem::vfunc_21` | rtti FEMenuSystem vtable 0x82071884 slot 21 (also in 2 derived vtables) |  |  | high; names.toml |
| `0x824BE360` | `FEMenuSystem::vfunc_19` | rtti FEMenuSystem vtable 0x8207180C slot 19 (also in 1 derived vtables) |  |  | high; names.toml |
| `0x824BE550` | `FEMenuSystem::vfunc_10` | rtti FEMenuSystem vtable 0x82071884 slot 10 (also in 2 derived vtables) |  |  | high; names.toml |
| `0x824BE5A8` | `FEMenuSystem::vfunc_4` | rtti FEMenuSystem vtable 0x82071884 slot 4 (also in 2 derived vtables) |  |  | high; names.toml |
| `0x824BE5C0` | `FEMenuSystem::vfunc_1` | rtti FEMenuSystem vtable 0x820719D4 slot 1 (also in 3 derived vtables) |  |  | high; names.toml |
| `0x824BE628` | `FEMenuSystem::vfunc_2` | rtti FEMenuSystem vtable 0x820719D4 slot 2 (also in 3 derived vtables) |  |  | high; names.toml |
| `0x824BE648` | `FEMenu::vfunc_29` | rtti FEMenu vtable 0x820729A4 slot 29 (also in 18 derived vtables) |  |  | high; names.toml |
| `0x824BE6D8` | `FEMenu::vfunc_17` | rtti FEMenu vtable 0x82072A8C slot 17 (also in 23 derived vtables) |  |  | high; names.toml |
| `0x824BE770` | `FEMenu::vfunc_46` | rtti FEMenu vtable 0x82072A8C slot 46 (also in 23 derived vtables) |  |  | high; names.toml |
| `0x824BE7E8` | `FEMenu::vfunc_45` | rtti FEMenu vtable 0x82072A8C slot 45 (also in 23 derived vtables) |  |  | high; names.toml |
| `0x824BE858` | `FEMenu::vfunc_54` | rtti FEMenu vtable 0x820729A4 slot 54 (also in 18 derived vtables) |  |  | high; names.toml |
| `0x824BE860` | `FEMenu::vfunc_53` | rtti FEMenu vtable 0x820729A4 slot 53 (also in 18 derived vtables) |  |  | high; names.toml |
| `0x824BE868` | `FEMenuEntry::vfunc_8` | rtti FEMenuEntry vtable 0x82071034 slot 8 |  |  | high; names.toml |
| `0x824BE8C8` | `FEMenuEntry::vfunc_7` | rtti FEMenuEntry vtable 0x82071034 slot 7 |  |  | high; names.toml |
| `0x824BEAF0` | `FEMultiLineText::vfunc_87` | rtti FEMultiLineText vtable 0x82070C84 slot 87 |  |  | high; names.toml |
| `0x824BEB90` | `FEMultiLineText::vfunc_86` | rtti FEMultiLineText vtable 0x82070C84 slot 86 |  |  | high; names.toml |
| `0x824BEC58` | `FEMultiLineText::vfunc_85` | rtti FEMultiLineText vtable 0x82070C84 slot 85 |  |  | high; names.toml |
| `0x824BEE00` | `FEMultiLineText::vfunc_25` | rtti FEMultiLineText vtable 0x82070C84 slot 25 |  |  | high; names.toml |
| `0x824BEE40` | `FEMultiLineText::vfunc_82` | rtti FEMultiLineText vtable 0x82070C84 slot 82 |  |  | high; names.toml |
| `0x824BEE50` | `FEMultiLineText::vfunc_65` | rtti FEMultiLineText vtable 0x82070C84 slot 65 |  |  | high; names.toml |
| `0x824BF4A8` | `FEText::vfunc_73` | rtti FEText vtable 0x820714A4 slot 73 (also in 2 derived vtables) |  |  | high; names.toml |
| `0x824BF5E0` | `FEText::vfunc_45` | rtti FEText vtable 0x820714A4 slot 45 (also in 2 derived vtables) |  |  | high; names.toml |
| `0x824BF5F8` | `FEText::vfunc_24` | rtti FEText vtable 0x820714A4 slot 24 (also in 1 derived vtables) |  |  | high; names.toml |
| `0x824BF670` | `FEText::vfunc_23` | rtti FEText vtable 0x820714A4 slot 23 (also in 1 derived vtables) |  |  | high; names.toml |
| `0x824BFA70` | `main_menu::kPanelDraw` | (panel): any panel's draw; hooked | `(panel)` | hook pc\_menu.cpp | addresses.h |
| `0x824BFC20` | `PanelQuad::vfunc_37` | rtti PanelQuad vtable 0x82070E04 slot 37 |  |  | high; names.toml |
| `0x824BFC78` | `PanelQuad::vfunc_31` | rtti PanelQuad vtable 0x82070E04 slot 31 |  |  | high; names.toml |
| `0x824C0930` | `FETypewriterText::vfunc_33` | rtti FETypewriterText vtable 0x820714A4 slot 33 |  |  | high; names.toml |
| `0x824C2C78` | `ChapterSelectMenu::vfunc_14` | rtti ChapterSelectMenu vtable 0x82072A8C slot 14 |  |  | high; names.toml |
| `0x824C30C8` | `SuccessFailMenu::vfunc_14` | rtti SuccessFailMenu vtable 0x82071CF4 slot 14 |  |  | high; names.toml |
| `0x824C3190` | `ProfileEditMenu::MoveDown` (likely) | vtbl[26]; hooked |  | hook menu\_video.cpp | medium; addresses.h |
| `0x824C3248` | `ProfileEditMenu::MoveUp` (likely) | vtbl[25]; hooked |  | hook menu\_video.cpp | medium; addresses.h |
| `0x824C3300` | `GammaScreenMenu::vfunc_22` | rtti GammaScreenMenu vtable 0x82072434 slot 22 |  |  | high; names.toml |
| `0x824C33B0` | `BonusSelectMenu::vfunc_14` | rtti BonusSelectMenu vtable 0x820727D4 slot 14 |  |  | high; names.toml |
| `0x824C33F8` | `BonusConceptArtMenu::vfunc_14` | rtti BonusConceptArtMenu vtable 0x820726EC slot 14 |  |  | high; names.toml |
| `0x824C3460` | `BonusContentMenu::vfunc_30` | rtti BonusContentMenu vtable 0x82072604 slot 30 |  |  | high; names.toml |
| `0x824C34D8` | `BonusMainMenu::MoveDown` (likely) | vtable slot 26; meaning from Options &gt; Gameplay vtbl[26] move selection down |  |  | medium |
| `0x824C3518` | `BonusMainMenu::MoveUp` (likely) | vtable slot 25; meaning from Options &gt; Gameplay vtbl[25] move selection up |  |  | medium |
| `0x824C3728` | `OptionsButtonMenu::vfunc_30` | rtti OptionsButtonMenu vtable 0x8207234C slot 30 |  |  | high; names.toml |
| `0x824C3790` | `OptionsStickMenu::vfunc_30` | rtti OptionsStickMenu vtable 0x82072264 slot 30 |  |  | high; names.toml |
| `0x824C3A80` | `OptionsControlsMenu::MoveDown` (likely) | vtable slot 26; meaning from Options &gt; Gameplay vtbl[26] move selection down |  |  | medium |
| `0x824C3C08` | `OptionsControlsMenu::MoveUp` (likely) | vtable slot 25; meaning from Options &gt; Gameplay vtbl[25] move selection up |  |  | medium |
| `0x824C3D90` | `OptionsControlsMenu::vfunc_55` | rtti OptionsControlsMenu vtable 0x82072094 slot 55 |  |  | high; names.toml |
| `0x824C3E48` | `OptionsGameplayMenu::Apply` | void OptionsGameplayMenu::Apply(page): copies the page's four rows into the settings | `void (page)` | hook aim\_assist.cpp | known; names.toml; addresses.h |
| `0x824C3F08` | `OptionsGameplayMenu::MoveDown` | vtbl[26] (this) | `(this)` | hook menu\_fov.cpp | known; names.toml; addresses.h |
| `0x824C3FE0` | `OptionsGameplayMenu::MoveUp` | vtbl[25] (this) up/down + texts | `(this)` | hook menu\_fov.cpp | known; names.toml; addresses.h |
| `0x824C40B8` | `MainMenu::vfunc_18` | MainMenu::vfunc\_18: item 1 runs load\_mp\_exe | `(this, item)` | hook pc\_menu.cpp | high; names.toml; addresses.h |
| `0x824C41B8` | `AdvancedOptionsMenu::MoveDown` (likely) | vtbl[26]; hooked |  | hook menu\_fov.cpp | medium; addresses.h |
| `0x824C4218` | `AdvancedOptionsMenu::MoveUp` (likely) | vtbl[25]; hooked |  | hook menu\_fov.cpp | medium; addresses.h |
| `0x824C4278` | `AdvancedOptionsMenu::vfunc_55` | rtti AdvancedOptionsMenu vtable 0x82071B24 slot 55 |  |  | high; names.toml |
| `0x824C4338` | `PauseMenu::MoveDown` (likely) | vtable slot 26; meaning from Options &gt; Gameplay vtbl[26] move selection down |  | hook pc\_menu.cpp | medium; addresses.h |
| `0x824C4390` | `PauseMenu::MoveUp` (likely) | Up / down (vtbl[25] / vtbl[26]): FEMenu's move, then 0x824BD668 wraps 0 &lt;-&gt; 6. Hooked (the rows' ... |  | hook pc\_menu.cpp | medium; addresses.h |
| `0x824C4428` | `msgbox::kAddButton` | The callback gets the player index and returns true to close the box | `void (DialogMenu* page, const char* key, callback)` | game\_api; hook pc\_menu.cpp; typed call game\_api.cpp; calls multiplayer.cpp, pc\_menu.cpp | addresses.h; game\_api.h |
| `0x824C44E0` | `DialogMenu::MoveDown` (likely) | vtable slot 26; meaning from Options &gt; Gameplay vtbl[26] move selection down |  |  | medium |
| `0x824C4548` | `DialogMenu::MoveUp` (likely) | vtable slot 25; meaning from Options &gt; Gameplay vtbl[25] move selection up |  |  | medium |
| `0x824C4608` | `msgbox::kClose` | Closes the box if it is up | `void (DialogMenuSystem* system)` | game\_api; typed call game\_api.cpp; calls multiplayer.cpp, pc\_menu.cpp | addresses.h; game\_api.h |
| `0x824C46A0` | `DialogMenuSystem::vfunc_4` | rtti DialogMenuSystem vtable 0x820719D4 slot 4 |  |  | high; names.toml |
| `0x824C47D0` | `TitleMenu::vfunc_14` | rtti TitleMenu vtable 0x82071724 slot 14 |  |  | high; names.toml |
| `0x824C4860` | `FEMenuSystem::vfunc_18` | rtti FEMenuSystem vtable 0x82071884 slot 18 (also in 2 derived vtables) |  |  | high; names.toml |
| `0x824C48B8` | `FEMenuSystem::vfunc_17` | rtti FEMenuSystem vtable 0x82071884 slot 17 (also in 1 derived vtables) |  | hook pc\_menu.cpp | high; names.toml; comments |
| `0x824C4CF0` | `FEMenuSystem::vfunc_14` | rtti FEMenuSystem vtable 0x82070FC4 slot 14 |  |  | high; names.toml |
| `0x824C4D20` | `FEMenuSystem::vfunc_13` | rtti FEMenuSystem vtable 0x820719D4 slot 13 (also in 3 derived vtables) |  |  | high; names.toml |
| `0x824C4DB8` | `FEMenuSystem::vfunc_12` | FEMenuSystem::vfunc\_12, ProcessInput(this); hooked |  | hook pc\_menu.cpp | high; names.toml; addresses.h; comments |
| `0x824C4FD0` | `FEMenuSystem::vfunc_9` | rtti FEMenuSystem vtable 0x820719D4 slot 9 (also in 3 derived vtables) |  |  | high; names.toml |
| `0x824C4FF0` | `FEMenuSystem::vfunc_8` | rtti FEMenuSystem vtable 0x820719D4 slot 8 (also in 3 derived vtables) |  |  | high; names.toml |
| `0x824C5150` | `FEMultiMenu::vfunc_55` | rtti FEMultiMenu vtable 0x82072A8C slot 55 (also in 3 derived vtables) |  |  | high; names.toml |
| `0x824C5248` | `FEMultiMenu::vfunc_54` | rtti FEMultiMenu vtable 0x820710EC slot 54 |  |  | high; names.toml |
| `0x824C5318` | `FEMultiMenu::vfunc_53` | rtti FEMultiMenu vtable 0x820710EC slot 53 |  |  | high; names.toml |
| `0x824C53E8` | `FEMultiMenu::vfunc_52` | rtti FEMultiMenu vtable 0x820710EC slot 52 |  |  | high; names.toml |
| `0x824C54B8` | `FEMultiMenu::vfunc_51` | rtti FEMultiMenu vtable 0x820710EC slot 51 |  |  | high; names.toml |
| `0x824C5600` | `FEMenu::vfunc_55` | rtti FEMenu vtable 0x82072604 slot 55 (also in 5 derived vtables) |  |  | high; names.toml |
| `0x824C5650` | `FEMenu::vfunc_38` | rtti FEMenu vtable 0x82072A8C slot 38 (also in 22 derived vtables) |  |  | high; names.toml |
| `0x824C56A0` | `FEMenu::vfunc_37` | rtti FEMenu vtable 0x820729A4 slot 37 (also in 19 derived vtables) |  |  | high; names.toml |
| `0x824C56E8` | `FEMenu::Refresh` (likely) | vtable slot 21; meaning from Options &gt; Gameplay vtbl[21] Refresh(this) | `(this)` |  | medium |
| `0x824C5800` | `FEMenu::Update` (likely) | vtable slot 16; meaning from Options &gt; Gameplay vtbl[16] Update(this, dt) | `(this, dt)` |  | medium |
| `0x824C58F0` | `FEMenu::vfunc_14` | rtti FEMenu vtable 0x820710EC slot 14 (also in 1 derived vtables) |  |  | high; names.toml; notes: pc-menu; comments |
| `0x824C5A60` | `FEMenu::vfunc_11` | rtti FEMenu vtable 0x820718EC slot 11 (also in 1 derived vtables) |  |  | high; names.toml |
| `0x824C5CB8` | `FEMenu::vfunc_44` | rtti FEMenu vtable 0x82072A8C slot 44 (also in 23 derived vtables) |  |  | high; names.toml |
| `0x824C5D00` | `FEMenu::vfunc_42` | rtti FEMenu vtable 0x820729A4 slot 42 (also in 19 derived vtables) |  |  | high; names.toml |
| `0x824C5E50` | `FEMenu::vfunc_51` | rtti FEMenu vtable 0x820729A4 slot 51 (also in 17 derived vtables) |  |  | high; names.toml |
| `0x824C60D0` | `FEMenu::vfunc_52` | rtti FEMenu vtable 0x820729A4 slot 52 (also in 17 derived vtables) |  |  | high; names.toml |
| `0x824C6360` | `FEMenu::vfunc_4` | rtti FEMenu vtable 0x82072A8C slot 4 (also in 23 derived vtables) |  |  | high; names.toml |
| `0x824C63E0` | `FEMenuEntry::vfunc_37` | rtti FEMenuEntry vtable 0x82071034 slot 37 |  |  | high; names.toml |
| `0x824C67C0` | `FEMultiLineText::vfunc_77` | rtti FEMultiLineText vtable 0x82070C84 slot 77 |  |  | high; names.toml |
| `0x824C68E0` | `prompts::kTokenGlyph` | (text at '~', int\* out\_string) -&gt; token length (0 = none), \*out = the button's character as a ... |  | hook button\_prompts.cpp | addresses.h |
| `0x824C6DD8` | `FEText::vfunc_6` | rtti FEText vtable 0x820714A4 slot 6 (also in 1 derived vtables) |  |  | high; names.toml |
| `0x824C6E80` | `FEText::vfunc_42` | rtti FEText vtable 0x820714A4 slot 42 (also in 2 derived vtables) |  |  | high; names.toml |
| `0x824C6EA0` | `FEText::vfunc_71` | rtti FEText vtable 0x820714A4 slot 71 (also in 1 derived vtables) |  |  | high; names.toml |
| `0x824C7038` | `FEText::vfunc_18` | rtti FEText vtable 0x820714A4 slot 18 (also in 2 derived vtables) |  |  | high; names.toml |
| `0x824C71C8` | `FEText::dtor` | rtti: stores the FEText vtable, then its base's (PanelAnimObject) |  |  | high; names.toml |
| `0x824C7B88` | `PanelQuad::vfunc_52` | rtti PanelQuad vtable 0x82070E04 slot 52 |  |  | high; names.toml |
| `0x824C7C60` | `PanelQuad::vfunc_51` | rtti PanelQuad vtable 0x82070E04 slot 51 |  |  | high; names.toml |
| `0x824C7CC0` | `PanelQuad::vfunc_50` | rtti PanelQuad vtable 0x82070E04 slot 50 |  |  | high; names.toml |
| `0x824C7D98` | `PanelQuad::vfunc_41` | rtti PanelQuad vtable 0x82070E04 slot 41 |  |  | high; names.toml |
| `0x824C7DE8` | `PanelQuad::vfunc_10` | rtti PanelQuad vtable 0x82070E04 slot 10 |  |  | high; names.toml |
| `0x824C7E28` | `PanelQuad::vfunc_30` | rtti PanelQuad vtable 0x82070E04 slot 30 |  |  | high; names.toml |
| `0x824C7E68` | `PanelQuad::vfunc_28` | rtti PanelQuad vtable 0x82070E04 slot 28 |  |  | high; names.toml |
| `0x824C7F38` | `PanelQuad::vfunc_24` | rtti PanelQuad vtable 0x82070E04 slot 24 |  |  | high; names.toml |
| `0x824C80D0` | `PanelQuad::vfunc_1` | rtti PanelQuad vtable 0x82070E04 slot 1 |  |  | high; names.toml |
| `0x824C81E8` | `PanelQuad::dtor` | rtti: stores the PanelQuad vtable, then its base's (PanelAnimObject) |  |  | high; names.toml |
| `0x824C8348` | `PanelQuad::vfunc_18` | rtti PanelQuad vtable 0x82070E04 slot 18 |  |  | high; names.toml |
| `0x824C8498` | `PanelQuad::ctor` | PanelQuad(this), 80 bytes | `(this)` | game\_api; typed call game\_api.cpp; calls menu\_fov.cpp, pc\_menu.cpp | high; names.toml; addresses.h; game\_api.h; comments |
| `0x824C8C10` | `FETypewriterText::vfunc_1` | rtti FETypewriterText vtable 0x820714A4 slot 1 |  |  | high; names.toml |
| `0x824CB5E0` | `ObjectiveMenu::Update` (likely) | vtable slot 16; meaning from Options &gt; Gameplay vtbl[16] Update(this, dt) | `(this, dt)` |  | medium |
| `0x824CB688` | `ObjectiveMenu::vfunc_14` | rtti ObjectiveMenu vtable 0x82071C0C slot 14 |  |  | high; names.toml |
| `0x824CBD60` | `ChapterSelectMenu::Update` (likely) | vtable slot 16; meaning from Options &gt; Gameplay vtbl[16] Update(this, dt) | `(this, dt)` |  | medium |
| `0x824CBE88` | `AutoSaveMenu::vfunc_3` | rtti AutoSaveMenu vtable 0x8207147C slot 3 |  |  | high; names.toml |
| `0x824CBF00` | `AutoSaveMenu::vfunc_6` | rtti AutoSaveMenu vtable 0x8207147C slot 6 |  |  | high; names.toml |
| `0x824CC010` | `ProfileEditMenu::Update` (likely) | vtable slot 16; meaning from Options &gt; Gameplay vtbl[16] Update(this, dt) | `(this, dt)` |  | medium |
| `0x824CC050` | `ProfileEditMenu::vfunc_14` | rtti ProfileEditMenu vtable 0x820729A4 slot 14 |  |  | high; names.toml |
| `0x824CC0A0` | `ControllerDisconnectedMenu::Refresh` (likely) | vtable slot 21; meaning from Options &gt; Gameplay vtbl[21] Refresh(this) | `(this)` |  | medium |
| `0x824CC0A8` | `DifficultyMenu::Update` (likely) | vtable slot 16; meaning from Options &gt; Gameplay vtbl[16] Update(this, dt) | `(this, dt)` |  | medium |
| `0x824CC0F0` | `DifficultyMenu::vfunc_14` | rtti DifficultyMenu vtable 0x820728BC slot 14 |  |  | high; names.toml |
| `0x824CC1D0` | `GammaScreenMenu::Update` (likely) | vtable slot 16; meaning from Options &gt; Gameplay vtbl[16] Update(this, dt) | `(this, dt)` |  | medium |
| `0x824CC278` | `GammaScreenMenu::vfunc_14` | rtti GammaScreenMenu vtable 0x82072434 slot 14 |  |  | high; names.toml |
| `0x824CC3E8` | `BonusSelectMenu::Update` (likely) | vtable slot 16; meaning from Options &gt; Gameplay vtbl[16] Update(this, dt) | `(this, dt)` |  | medium |
| `0x824CC4D0` | `BonusSelectMenu::vfunc_20` | rtti BonusSelectMenu vtable 0x820727D4 slot 20 |  |  | high; names.toml |
| `0x824CC520` | `BonusConceptArtMenu::Update` (likely) | vtable slot 16; meaning from Options &gt; Gameplay vtbl[16] Update(this, dt) | `(this, dt)` |  | medium |
| `0x824CC608` | `BonusContentMenu::Update` (likely) | vtable slot 16; meaning from Options &gt; Gameplay vtbl[16] Update(this, dt) | `(this, dt)` |  | medium |
| `0x824CC6F0` | `BonusContentMenu::vfunc_14` | rtti BonusContentMenu vtable 0x82072604 slot 14 |  |  | high; names.toml |
| `0x824CC740` | `BonusMainMenu::Update` (likely) | vtable slot 16; meaning from Options &gt; Gameplay vtbl[16] Update(this, dt) | `(this, dt)` |  | medium |
| `0x824CC748` | `BonusMainMenu::vfunc_14` | rtti BonusMainMenu vtable 0x8207251C slot 14 |  |  | high; names.toml |
| `0x824CC798` | `BonusMainMenu::vfunc_20` | rtti BonusMainMenu vtable 0x8207251C slot 20 |  |  | high; names.toml |
| `0x824CC928` | `OptionsButtonMenu::Update` (likely) | vtable slot 16; meaning from Options &gt; Gameplay vtbl[16] Update(this, dt) | `(this, dt)` |  | medium |
| `0x824CC9D0` | `OptionsButtonMenu::vfunc_14` | rtti OptionsButtonMenu vtable 0x8207234C slot 14 |  |  | high; names.toml |
| `0x824CCA20` | `OptionsStickMenu::Update` (likely) | vtable slot 16; meaning from Options &gt; Gameplay vtbl[16] Update(this, dt) | `(this, dt)` |  | medium |
| `0x824CCAC8` | `OptionsStickMenu::vfunc_14` | rtti OptionsStickMenu vtable 0x82072264 slot 14 |  |  | high; names.toml |
| `0x824CCC10` | `OptionsControlsMenu::vfunc_19` | rtti OptionsControlsMenu vtable 0x82072094 slot 19 |  |  | high; names.toml |
| `0x824CCC68` | `OptionsControlsMenu::vfunc_30` | rtti OptionsControlsMenu vtable 0x82072094 slot 30 |  |  | high; names.toml |
| `0x824CD1A0` | `OptionsControlsMenu::Update` (likely) | vtable slot 16; meaning from Options &gt; Gameplay vtbl[16] Update(this, dt) | `(this, dt)` |  | medium |
| `0x824CD268` | `OptionsControlsMenu::vfunc_14` | rtti OptionsControlsMenu vtable 0x82072094 slot 14 |  |  | high; names.toml |
| `0x824CD2D0` | `OptionsGameplayMenu::Update` | vtbl[16] (this, f1=dt) | `(this, f1=dt)` | hook menu\_fov.cpp | known; names.toml; addresses.h |
| `0x824CD398` | `OptionsGameplayMenu::vfunc_14` | The gameplay page's draw (vtbl[14], 0x824CD398): its panel, then its items (FEMenu::DrawItems ... |  | hook menu\_fov.cpp | high; names.toml; addresses.h |
| `0x824CD400` | `SPMainMenu::vfunc_37` | rtti SPMainMenu vtable 0x82071EC4 slot 37 |  |  | high; names.toml |
| `0x824CD458` | `SPMainMenu::vfunc_14` | rtti SPMainMenu vtable 0x82071EC4 slot 14 |  |  | high; names.toml |
| `0x824CD4D0` | `MainMenu::vfunc_37` | rtti MainMenu vtable 0x82071DDC slot 37 |  |  | high; names.toml |
| `0x824CD530` | `AdvancedOptionsMenu::vfunc_14` | vtbl[14]: panel, items, values; hooked |  | hook menu\_fov.cpp | high; names.toml; addresses.h |
| `0x824CD5A8` | `PauseMenu::vfunc_30` | rtti PauseMenu vtable 0x82071A3C slot 30 |  |  | high; names.toml |
| `0x824CD658` | `PauseMenu::vfunc_24` | rtti PauseMenu vtable 0x82071A3C slot 24 |  |  | high; names.toml |
| `0x824CD6D0` | `PauseMenu::vfunc_14` | rtti PauseMenu vtable 0x82071A3C slot 14 |  |  | high; names.toml |
| `0x824CD768` | `storage::kPauseItemStates` | (this); hooked | `(this)` | hook pc\_menu.cpp | addresses.h |
| `0x824CD8D8` | `PauseMenu::Refresh` (likely) | vtable slot 21; meaning from Options &gt; Gameplay vtbl[21] Refresh(this) | `(this)` |  | medium |
| `0x824CD9E0` | `InGameMenuSystem::vfunc_14` | rtti InGameMenuSystem vtable 0x82071884 slot 14 |  | hook pc\_menu.cpp | high; names.toml; addresses.h |
| `0x824CDB28` | `msgbox::kOpen` | Shows the box | `void (DialogMenu* page, 1, 0)` | game\_api; hook pc\_menu.cpp; typed call game\_api.cpp; calls multiplayer.cpp, pc\_menu.cpp | addresses.h; game\_api.h |
| `0x824CDD40` | `DialogMenu::vfunc_30` | rtti DialogMenu vtable 0x820718EC slot 30 |  |  | high; names.toml; notes: multiplayer-exit |
| `0x824CDD78` | `DialogMenu::vfunc_19` | rtti DialogMenu vtable 0x820718EC slot 19 |  |  | high; names.toml; notes: multiplayer-exit |
| `0x824CDDF8` | `DialogMenu::Update` (likely) | vtable slot 16; meaning from Options &gt; Gameplay vtbl[16] Update(this, dt) | `(this, dt)` |  | medium |
| `0x824CDE80` | `DialogMenu::Refresh` (likely) | vtable slot 21; meaning from Options &gt; Gameplay vtbl[21] Refresh(this) | `(this)` |  | medium |
| `0x824CDE88` | `DialogMenuSystem::vfunc_20` | rtti DialogMenuSystem vtable 0x820719D4 slot 20 |  |  | high; names.toml |
| `0x824CDF30` | `DialogMenuSystem::vfunc_19` | rtti DialogMenuSystem vtable 0x820719D4 slot 19 |  |  | high; names.toml |
| `0x824CE158` | `DialogMenuSystem::vfunc_22` | rtti DialogMenuSystem vtable 0x820719D4 slot 22 |  |  | high; names.toml |
| `0x824CE1A8` | `DialogMenuSystem::vfunc_21` | rtti DialogMenuSystem vtable 0x820719D4 slot 21 |  |  | high; names.toml |
| `0x824CE1E8` | `DialogMenuSystem::vfunc_23` | rtti DialogMenuSystem vtable 0x820719D4 slot 23 |  |  | high; names.toml |
| `0x824CE388` | `DialogMenuSystem::vfunc_18` | rtti DialogMenuSystem vtable 0x820719D4 slot 18 |  |  | high; names.toml |
| `0x824CE3E0` | `DialogMenuSystem::vfunc_17` | rtti DialogMenuSystem vtable 0x820719D4 slot 17 |  |  | high; names.toml |
| `0x824CE3E8` | `DialogMenuSystem::vfunc_14` | rtti DialogMenuSystem vtable 0x820719D4 slot 14 |  | hook pc\_menu.cpp | high; names.toml; addresses.h |
| `0x824CE4C8` | `FrontEndMenuSystem::vfunc_17` | rtti FrontEndMenuSystem vtable 0x8207180C slot 17 |  |  | high; names.toml |
| `0x824CE4D0` | `FrontEndMenuSystem::vfunc_14` | Each menu system's draw (vtbl +56), which draws its page: the footers drawn inside belong to that ... |  | hook pc\_menu.cpp | high; names.toml; addresses.h |
| `0x824CE500` | `TitleMenu::vfunc_37` | rtti TitleMenu vtable 0x82071724 slot 37 |  |  | high; names.toml |
| `0x824CE558` | `FEMenu::vfunc_41` | rtti FEMenu vtable 0x82072A8C slot 41 (also in 23 derived vtables) |  |  | high; names.toml |
| `0x824CE638` | `FEMenu::ctor` | rtti: stores the FEMenu vtable at this+0; called on the result of operator new |  |  | high; names.toml |
| `0x824CE798` | `FEMenuEntry::vfunc_38` | rtti FEMenuEntry vtable 0x82071034 slot 38 |  |  | high; names.toml |
| `0x824CF4E8` | `FEMultiLineText::vfunc_6` | rtti FEMultiLineText vtable 0x82070C84 slot 6 |  |  | high; names.toml |
| `0x824CF6F8` | `FEMultiLineText::vfunc_71` | rtti FEMultiLineText vtable 0x82070C84 slot 71 |  |  | high; names.toml |
| `0x824CF888` | `FEMultiLineText::vfunc_34` | rtti FEMultiLineText vtable 0x82070C84 slot 34 |  |  | high; names.toml |
| `0x824CF8E0` | `FEMultiLineText::vfunc_37` | rtti FEMultiLineText vtable 0x82070C84 slot 37 |  |  | high; names.toml |
| `0x824CF930` | `FEMultiLineText::vfunc_79` | rtti FEMultiLineText vtable 0x82070C84 slot 79 |  |  | high; names.toml |
| `0x824CFA00` | `FEMultiLineText::vfunc_19` | rtti FEMultiLineText vtable 0x82070C84 slot 19 |  | hook button\_prompts.cpp | high; names.toml; comments |
| `0x824CFB48` | `FEText::vfunc_61` | rtti FEText vtable 0x820714A4 slot 61 (also in 2 derived vtables) |  |  | high; names.toml |
| `0x824CFBE8` | `FEText::vfunc_60` | rtti FEText vtable 0x820714A4 slot 60 (also in 2 derived vtables) |  |  | high; names.toml |
| `0x824CFC88` | `FEText::vfunc_43` | rtti FEText vtable 0x820714A4 slot 43 (also in 2 derived vtables) |  |  | high; names.toml |
| `0x824CFD58` | `FEText::vfunc_19` | rtti FEText vtable 0x820714A4 slot 19 (also in 1 derived vtables) |  |  | high; names.toml; comments |
| `0x824CFF70` | `FEText::vfunc_2` | rtti FEText vtable 0x82070C84 slot 2 (also in 1 derived vtables) |  |  | high; names.toml |
| `0x824CFFC8` | `FEText::vfunc_72` | rtti FEText vtable 0x820714A4 slot 72 (also in 1 derived vtables) |  |  | high; names.toml |
| `0x824D07D0` | `Panel::FindElement` | A panel's element by name, or 0 | `Element* (Panel* panel, const char* name)` | game\_api; typed call game\_api.cpp; calls menu\_fov.cpp, menu\_video.cpp, pc\_menu.cpp | known; names.toml; addresses.h; game\_api.h; comments |
| `0x824D0A60` | `PanelQuad::vfunc_16` | rtti PanelQuad vtable 0x82070E04 slot 16 |  |  | high; names.toml |
| `0x824D0AE8` | `PanelQuad::vfunc_22` | rtti PanelQuad vtable 0x82070E04 slot 22 |  |  | high; names.toml |
| `0x824D0B88` | `PanelQuad::vfunc_19` | rtti PanelQuad vtable 0x82070E04 slot 19 |  |  | high; names.toml |
| `0x824D2118` | `ObjectiveMenu::vfunc_2` | rtti ObjectiveMenu vtable 0x82071C0C slot 2 |  |  | high; names.toml |
| `0x824D2318` | `ChapterSelectMenu::vfunc_2` | rtti ChapterSelectMenu vtable 0x82072A8C slot 2 |  |  | high; names.toml |
| `0x824D24A8` | `SuccessFailMenu::vfunc_2` | rtti SuccessFailMenu vtable 0x82071CF4 slot 2 |  |  | high; names.toml |
| `0x824D2550` | `ProfileEditMenu::vfunc_2` | rtti ProfileEditMenu vtable 0x820729A4 slot 2 |  |  | high; names.toml |
| `0x824D25C8` | `DifficultyMenu::vfunc_2` | rtti DifficultyMenu vtable 0x820728BC slot 2 |  |  | high; names.toml |
| `0x824D26A0` | `GammaScreenMenu::vfunc_2` | rtti GammaScreenMenu vtable 0x82072434 slot 2 |  |  | high; names.toml |
| `0x824D2730` | `GammaScreenMenu::Refresh` (likely) | vtable slot 21; meaning from Options &gt; Gameplay vtbl[21] Refresh(this) | `(this)` |  | medium |
| `0x824D27F0` | `BonusSelectMenu::vfunc_2` | rtti BonusSelectMenu vtable 0x820727D4 slot 2 |  |  | high; names.toml |
| `0x824D2870` | `BonusConceptArtMenu::vfunc_2` | rtti BonusConceptArtMenu vtable 0x820726EC slot 2 |  |  | high; names.toml |
| `0x824D2968` | `BonusContentMenu::vfunc_2` | rtti BonusContentMenu vtable 0x82072604 slot 2 |  |  | high; names.toml |
| `0x824D29C8` | `BonusMainMenu::vfunc_2` | rtti BonusMainMenu vtable 0x8207251C slot 2 |  |  | high; names.toml |
| `0x824D2A40` | `BonusMainMenu::vfunc_30` | rtti BonusMainMenu vtable 0x8207251C slot 30 |  |  | high; names.toml |
| `0x824D2CB0` | `OptionsButtonMenu::vfunc_2` | rtti OptionsButtonMenu vtable 0x8207234C slot 2 |  |  | high; names.toml |
| `0x824D2D28` | `OptionsStickMenu::vfunc_2` | rtti OptionsStickMenu vtable 0x82072264 slot 2 |  |  | high; names.toml |
| `0x824D2E50` | `OptionsControlsMenu::vfunc_2` | rtti OptionsControlsMenu vtable 0x82072094 slot 2 |  |  | high; names.toml |
| `0x824D2F10` | `OptionsGameplayMenu::vfunc_2` | rtti OptionsGameplayMenu vtable 0x82071FAC slot 2 |  |  | high; names.toml |
| `0x824D2F88` | `SPMainMenu::vfunc_2` | rtti SPMainMenu vtable 0x82071EC4 slot 2 |  |  | high; names.toml |
| `0x824D3000` | `MainMenu::vfunc_2` | rtti MainMenu vtable 0x82071DDC slot 2 |  |  | high; names.toml |
| `0x824D3078` | `AdvancedOptionsMenu::vfunc_2` | rtti AdvancedOptionsMenu vtable 0x82071B24 slot 2 |  |  | high; names.toml |
| `0x824D3130` | `PauseMenu::vfunc_2` | rtti PauseMenu vtable 0x82071A3C slot 2 |  |  | high; names.toml |
| `0x824D31E0` | `msgbox::kSetMessage` | Localized; an unknown key shows as it is. text is a guest string that stays valid | `void (DialogMenuSystem* system, const char* key)` | game\_api; typed call game\_api.cpp; calls multiplayer.cpp, pc\_menu.cpp | addresses.h; game\_api.h |
| `0x824D33A8` | `TitleMenu::Init` (likely) | vtable slot 0; meaning from Options &gt; Gameplay vtbl[0] Init(this, panel) | `(this, panel)` |  | medium; comments |
| `0x824D3418` | `TitleMenu::vfunc_2` | rtti TitleMenu vtable 0x82071724 slot 2 |  |  | high; names.toml |
| `0x824D3428` | `FEMultiLineText::vfunc_78` | rtti FEMultiLineText vtable 0x82070C84 slot 78 |  |  | high; names.toml |
| `0x824D3810` | `PanelQuad::vfunc_40` | rtti PanelQuad vtable 0x82070E04 slot 40 |  |  | high; names.toml |
| `0x824D3900` | `PanelQuad::vfunc_38` | rtti PanelQuad vtable 0x82070E04 slot 38 |  |  | high; names.toml |
| `0x824D3BE0` | `PanelQuad::vfunc_34` | rtti PanelQuad vtable 0x82070E04 slot 34 |  |  | high; names.toml |
| `0x824D3CD8` | `PanelQuad::vfunc_32` | rtti PanelQuad vtable 0x82070E04 slot 32 |  |  | high; names.toml |
| `0x824D3DD8` | `PanelQuad::vfunc_21` | rtti PanelQuad vtable 0x82070E04 slot 21 |  |  | high; names.toml |
| `0x824D6238` | `ChapterSelectMenu::OnRight` (likely) | vtable slot 28; meaning from Options &gt; Gameplay vtbl[28] = Toggle(this, 1) |  |  | medium |
| `0x824D6340` | `ChapterSelectMenu::OnLeft` (likely) | vtable slot 27; meaning from Options &gt; Gameplay vtbl[27] = Toggle(this, 0) |  |  | medium |
| `0x824D6440` | `ChapterSelectMenu::vfunc_20` | rtti ChapterSelectMenu vtable 0x82072A8C slot 20 |  |  | high; names.toml |
| `0x824D6AB8` | `BonusConceptArtMenu::OnRight` (likely) | vtable slot 28; meaning from Options &gt; Gameplay vtbl[28] = Toggle(this, 1) |  |  | medium |
| `0x824D6AC0` | `BonusConceptArtMenu::OnLeft` (likely) | vtable slot 27; meaning from Options &gt; Gameplay vtbl[27] = Toggle(this, 0) |  |  | medium |
| `0x824D6AC8` | `BonusConceptArtMenu::vfunc_20` | rtti BonusConceptArtMenu vtable 0x820726EC slot 20 |  |  | high; names.toml |
| `0x824D7038` | `BonusContentMenu::MoveDown` (likely) | vtable slot 26; meaning from Options &gt; Gameplay vtbl[26] move selection down |  |  | medium |
| `0x824D7040` | `BonusContentMenu::MoveUp` (likely) | vtable slot 25; meaning from Options &gt; Gameplay vtbl[25] move selection up |  |  | medium |
| `0x824D7048` | `BonusContentMenu::vfunc_20` | rtti BonusContentMenu vtable 0x82072604 slot 20 |  |  | high; names.toml |
| `0x824D72D8` | `DialogMenu::vfunc_2` | rtti DialogMenu vtable 0x820718EC slot 2 |  |  | high; names.toml |
| `0x824D75F8` | `FEMultiLineText::vfunc_32` | rtti FEMultiLineText vtable 0x82070C84 slot 32 |  |  | high; names.toml |
| `0x824D7650` | `prompts::kLayoutLine` | (line, f1 text scale, f2 button scale): splits the line at its tokens and measures them with font 1 ... | `(line, f1 text scale, f2 button scale)` | hook button\_prompts.cpp | addresses.h |
| `0x824D7B48` | `prompts::kMeasureWord` | (const char\* word, font index, f1 text scale, f2 button scale) -&gt; f1 width, for word wrap; hooked | `(const char* word, font index, f1 text scale, f2 button scale)` | hook button\_prompts.cpp | addresses.h |
| `0x824D7D88` | `prompts::kDrawLine` | (line, f1 ?, f2 x scale, f3 y scale, f4 button scale, f5 button raise, r5 text color, r6 button ... | `(line, f1 ?, f2 x scale, f3 y scale, f4 button scale, f5 button raise, r5 text color, r6 button color)` | hook button\_prompts.cpp | addresses.h |
| `0x824D8380` | `FEText::vfunc_32` | rtti FEText vtable 0x820714A4 slot 32 (also in 1 derived vtables) |  |  | high; names.toml |
| `0x824D83D8` | `FEText::ctor_2` | rtti: stores the FEText vtable at this+0; called on the result of operator new |  |  | high; names.toml |
| `0x824D86B0` | `PanelQuad::vfunc_6` | rtti PanelQuad vtable 0x82070E04 slot 6 |  |  | high; names.toml |
| `0x824D8750` | `PanelQuad::vfunc_35` | rtti PanelQuad vtable 0x82070E04 slot 35 |  |  | high; names.toml |
| `0x824D8848` | `PanelQuad::ctor_2` | rtti: stores the PanelQuad vtable at this+0; called on the result of operator new |  |  | high; names.toml |
| `0x824D9E88` | `ObjectiveMenu::dtor` | rtti: stores the ObjectiveMenu vtable, then its base's (FEMenu) |  |  | high; names.toml |
| `0x824D9F38` | `ObjectiveMenu::ctor` | rtti: stores the ObjectiveMenu vtable at this+0; called on the result of operator new |  |  | high; names.toml |
| `0x824DA228` | `ChapterSelectMenu::vfunc_37` | rtti ChapterSelectMenu vtable 0x82072A8C slot 37 |  |  | high; names.toml |
| `0x824DA568` | `ChapterSelectMenu::vfunc_29` | rtti ChapterSelectMenu vtable 0x82072A8C slot 29 |  |  | high; names.toml |
| `0x824DA5F0` | `ChapterSelectMenu::dtor` | rtti: stores the ChapterSelectMenu vtable, then its base's (FEMenu) |  |  | high; names.toml |
| `0x824DA680` | `SuccessFailMenu::dtor` | rtti: stores the SuccessFailMenu vtable, then its base's (FEMenu) |  |  | high; names.toml |
| `0x824DA750` | `SuccessFailMenu::ctor` | rtti: stores the SuccessFailMenu vtable at this+0; called on the result of operator new |  |  | high; names.toml |
| `0x824DA818` | `ProfileEditMenu::dtor` | rtti: stores the ProfileEditMenu vtable, then its base's (FEMenu) |  | hook menu\_video.cpp | high; names.toml |
| `0x824DA8A8` | `ProfileEditMenu::ctor` | rtti: stores the ProfileEditMenu vtable at this+0; called on the result of operator new |  |  | high; names.toml |
| `0x824DA948` | `ControllerDisconnectedMenu::vfunc_14` | rtti ControllerDisconnectedMenu vtable 0x8207163C slot 14 |  |  | high; names.toml |
| `0x824DAAB0` | `DifficultyMenu::dtor` | rtti: stores the DifficultyMenu vtable, then its base's (FEMenu) |  |  | high; names.toml |
| `0x824DAB40` | `DifficultyMenu::ctor` | rtti: stores the DifficultyMenu vtable at this+0; called on the result of operator new |  |  | high; names.toml |
| `0x824DABB8` | `GammaScreenMenu::dtor` | rtti: stores the GammaScreenMenu vtable, then its base's (FEMenu) |  |  | high; names.toml |
| `0x824DAC48` | `GammaScreenMenu::ctor` | rtti: stores the GammaScreenMenu vtable at this+0; called on the result of operator new |  |  | high; names.toml |
| `0x824DACE0` | `BonusSelectMenu::dtor` | rtti: stores the BonusSelectMenu vtable, then its base's (FEMenu) |  |  | high; names.toml |
| `0x824DAD70` | `BonusConceptArtMenu::dtor` | rtti: stores the BonusConceptArtMenu vtable, then its base's (FEMenu) |  |  | high; names.toml |
| `0x824DAE40` | `BonusContentMenu::dtor` | rtti: stores the BonusContentMenu vtable, then its base's (FEMenu) |  |  | high; names.toml |
| `0x824DAED0` | `BonusContentMenu::ctor` | rtti: stores the BonusContentMenu vtable at this+0; called on the result of operator new |  |  | high; names.toml |
| `0x824DAF80` | `BonusMainMenu::dtor` | rtti: stores the BonusMainMenu vtable, then its base's (FEMenu) |  |  | high; names.toml |
| `0x824DB020` | `BonusMainMenu::ctor` | rtti: stores the BonusMainMenu vtable at this+0; called on the result of operator new |  |  | high; names.toml |
| `0x824DB0F0` | `OptionsButtonMenu::dtor` | rtti: stores the OptionsButtonMenu vtable, then its base's (FEMenu) |  |  | high; names.toml |
| `0x824DB180` | `OptionsButtonMenu::ctor` | rtti: stores the OptionsButtonMenu vtable at this+0; called on the result of operator new |  |  | high; names.toml |
| `0x824DB208` | `OptionsStickMenu::dtor` | rtti: stores the OptionsStickMenu vtable, then its base's (FEMenu) |  |  | high; names.toml |
| `0x824DB298` | `OptionsStickMenu::ctor` | rtti: stores the OptionsStickMenu vtable at this+0; called on the result of operator new |  |  | high; names.toml |
| `0x824DB638` | `OptionsControlsMenu::vfunc_20` | rtti OptionsControlsMenu vtable 0x82072094 slot 20 |  |  | high; names.toml |
| `0x824DB7C8` | `OptionsControlsMenu::dtor` | rtti: stores the OptionsControlsMenu vtable, then its base's (FEMenu) |  |  | high; names.toml |
| `0x824DB880` | `OptionsControlsMenu::ctor` | rtti: stores the OptionsControlsMenu vtable at this+0; called on the result of operator new |  |  | high; names.toml |
| `0x824DB960` | `OptionsGameplayMenu::Refresh` | vtbl[21] (this) | `(this)` | hook menu\_fov.cpp | known; names.toml; addresses.h |
| `0x824DBB38` | `OptionsGameplayMenu::dtor` | menu\_fov.cpp (destructor body) |  | hook menu\_fov.cpp | known; names.toml |
| `0x824DBBF0` | `OptionsGameplayMenu::ctor` | (this, parent) | `(this, parent)` | mid-asm Cod3GameplayItemCount default.toml | known; names.toml; addresses.h |
| `0x824DBDC0` | `SPMainMenu::dtor` | rtti: stores the SPMainMenu vtable, then its base's (FEMenu) |  |  | high; names.toml |
| `0x824DBE50` | `SPMainMenu::ctor` | rtti: stores the SPMainMenu vtable at this+0; called on the result of operator new |  |  | high; names.toml |
| `0x824DBEE8` | `MainMenu::Update` (likely) | vtbl[16]; hooked | `(this, dt)` | hook pc\_menu.cpp | medium; addresses.h; comments |
| `0x824DBFE8` | `MainMenu::dtor` | rtti: stores the MainMenu vtable, then its base's (FEMenu) |  | hook pc\_menu.cpp | high; names.toml |
| `0x824DC078` | `MainMenu::ctor` | rtti: stores the MainMenu vtable at this+0; called on the result of operator new |  |  | high; names.toml |
| `0x824DC1C0` | `fe::adv::kToggle` | (this, right): OnLeft / OnRight; hooked | `(this, right)` | hook menu\_fov.cpp | addresses.h |
| `0x824DC640` | `AdvancedOptionsMenu::dtor` | rtti: stores the AdvancedOptionsMenu vtable, then its base's (FEMenu) |  | hook menu\_fov.cpp | high; names.toml |
| `0x824DC6C0` | `AdvancedOptionsMenu::ctor` | rtti: stores the AdvancedOptionsMenu vtable at this+0; called on the result of operator new |  |  | high; names.toml |
| `0x824DC798` | `storage::kPauseQuitYes` | () -&gt; 1 | `()` | hook pc\_menu.cpp; refers to pc\_menu.cpp | addresses.h |
| `0x824DC7F8` | `storage::kPauseQuit` | (this) | `(this)` | calls pc\_menu.cpp | addresses.h |
| `0x824DC978` | `PauseMenu::dtor` | rtti: stores the PauseMenu vtable, then its base's (FEMenu) |  |  | high; names.toml |
| `0x824DC9E0` | `PauseMenu::ctor` | rtti: stores the PauseMenu vtable at this+0; called on the result of operator new |  |  | high; names.toml |
| `0x824DCA60` | `InGameMenuSystem::ctor` | rtti: stores the InGameMenuSystem vtable at this+0; called on the result of operator new |  |  | high; names.toml |
| `0x824DCBB0` | `DialogMenuSystem::vfunc_10` | rtti DialogMenuSystem vtable 0x820719D4 slot 10 |  |  | high; names.toml |
| `0x824DCC78` | `TitleMenu::ctor` | rtti: stores the TitleMenu vtable at this+0; called on the result of operator new |  |  | high; names.toml |
| `0x824DD630` | `FEMultiLineText::vfunc_93` | rtti FEMultiLineText vtable 0x82070C84 slot 93 |  | hook button\_prompts.cpp | high; names.toml |
| `0x824DD790` | `FEMultiLineText::vfunc_92` | rtti FEMultiLineText vtable 0x82070C84 slot 92 |  | hook button\_prompts.cpp | high; names.toml |
| `0x824DD928` | `FEMultiLineText::vfunc_90` | rtti FEMultiLineText vtable 0x82070C84 slot 90 |  |  | high; names.toml |
| `0x824DD9A8` | `prompts::kActionText` | ("[ACTION]") -&gt; "~square", "Press and hold ~square", "Use the left thumbstick" ([USE\_LEFT\_STICK] ... |  | hook button\_prompts.cpp | addresses.h |
| `0x824DE290` | `prompts::kExpandActions` | (const char\* text) -&gt; its "[ACTION]" words expanded, in a static 512-byte buffer; hooked | `(const char* text)` | hook button\_prompts.cpp | addresses.h |
| `0x824DE5F0` | `FEMultiLineText::vfunc_33` | rtti FEMultiLineText vtable 0x82070C84 slot 33 |  |  | high; names.toml |
| `0x824DE658` | `FEMultiLineText::vfunc_89` | rtti FEMultiLineText vtable 0x82070C84 slot 89 |  |  | high; names.toml |
| `0x824DE710` | `FEMultiLineText::vfunc_74` | rtti FEMultiLineText vtable 0x82070C84 slot 74 |  |  | high; names.toml |
| `0x824DEA58` | `FEMultiLineText::vfunc_72` | rtti FEMultiLineText vtable 0x82070C84 slot 72 |  |  | high; names.toml |
| `0x824DEAD0` | `FEMultiLineText::ctor` | rtti: stores the FEMultiLineText vtable at this+0; called on the result of operator new |  | hook button\_prompts.cpp | high; names.toml |
| `0x824DEBA8` | `FEText::vfunc_33` | rtti FEText vtable 0x82070B54 slot 33 |  |  | high; names.toml |
| `0x824DFC70` | `fe::kPanelFindQuad` | A panel's picture by name, or 0 (the game hands back a shared stand-in for a missing name; that is ... | `PanelQuad* (Panel* panel, const char* name)` | game\_api; typed call game\_api.cpp; calls menu\_fov.cpp, menu\_video.cpp, pc\_menu.cpp | addresses.h; game\_api.h |
| `0x824E2080` | `ChapterSelectMenu::Init` (likely) | vtable slot 0; meaning from Options &gt; Gameplay vtbl[0] Init(this, panel) | `(this, panel)` |  | medium |
| `0x824E22E0` | `ChapterSelectMenu::ctor` | rtti: stores the ChapterSelectMenu vtable at this+0; called on the result of operator new |  |  | high; names.toml |
| `0x824E2360` | `AutoSaveMenu::vfunc_1` | rtti AutoSaveMenu vtable 0x8207147C slot 1 |  |  | high; names.toml |
| `0x824E2818` | `SuccessFailMenu::Init` (likely) | vtable slot 0; meaning from Options &gt; Gameplay vtbl[0] Init(this, panel) | `(this, panel)` |  | medium |
| `0x824E2948` | `ProfileEditMenu::Init` (likely) | vtbl[0] (this, panel); hooked | `(this, panel)` | hook menu\_video.cpp | medium; addresses.h |
| `0x824E2BB0` | `ControllerDisconnectedMenu::Init` (likely) | vtable slot 0; meaning from Options &gt; Gameplay vtbl[0] Init(this, panel) | `(this, panel)` |  | medium |
| `0x824E2D40` | `DifficultyMenu::Init` (likely) | vtable slot 0; meaning from Options &gt; Gameplay vtbl[0] Init(this, panel) | `(this, panel)` |  | medium |
| `0x824E2F20` | `DifficultyMenu::vfunc_20` | rtti DifficultyMenu vtable 0x820728BC slot 20 |  |  | high; names.toml |
| `0x824E3038` | `GammaScreenMenu::vfunc_31` | rtti GammaScreenMenu vtable 0x82072434 slot 31 |  |  | high; names.toml |
| `0x824E30F8` | `GammaScreenMenu::Init` (likely) | vtable slot 0; meaning from Options &gt; Gameplay vtbl[0] Init(this, panel) | `(this, panel)` |  | medium |
| `0x824E3338` | `BonusSelectMenu::Init` (likely) | vtable slot 0; meaning from Options &gt; Gameplay vtbl[0] Init(this, panel) | `(this, panel)` |  | medium |
| `0x824E3708` | `BonusSelectMenu::ctor` | rtti: stores the BonusSelectMenu vtable at this+0; called on the result of operator new |  |  | high; names.toml |
| `0x824E3790` | `BonusConceptArtMenu::Init` (likely) | vtable slot 0; meaning from Options &gt; Gameplay vtbl[0] Init(this, panel) | `(this, panel)` |  | medium |
| `0x824E3AE8` | `BonusConceptArtMenu::vfunc_29` | rtti BonusConceptArtMenu vtable 0x820726EC slot 29 |  |  | high; names.toml |
| `0x824E3BB0` | `BonusConceptArtMenu::ctor` | rtti: stores the BonusConceptArtMenu vtable at this+0; called on the result of operator new |  |  | high; names.toml |
| `0x824E3C58` | `BonusContentMenu::Init` (likely) | vtable slot 0; meaning from Options &gt; Gameplay vtbl[0] Init(this, panel) | `(this, panel)` |  | medium |
| `0x824E40A8` | `BonusMainMenu::Init` (likely) | vtable slot 0; meaning from Options &gt; Gameplay vtbl[0] Init(this, panel) | `(this, panel)` |  | medium |
| `0x824E43B8` | `OptionsButtonMenu::Init` (likely) | vtable slot 0; meaning from Options &gt; Gameplay vtbl[0] Init(this, panel) | `(this, panel)` |  | medium |
| `0x824E45B0` | `OptionsButtonMenu::Refresh` (likely) | vtable slot 21; meaning from Options &gt; Gameplay vtbl[21] Refresh(this) | `(this)` |  | medium |
| `0x824E4650` | `OptionsStickMenu::Init` (likely) | vtable slot 0; meaning from Options &gt; Gameplay vtbl[0] Init(this, panel) | `(this, panel)` |  | medium |
| `0x824E4AB8` | `OptionsControlsMenu::Init` (likely) | vtable slot 0; meaning from Options &gt; Gameplay vtbl[0] Init(this, panel) | `(this, panel)` |  | medium |
| `0x824E4D78` | `OptionsGameplayMenu::Init` | vtbl[0] (this, panel) | `(this, panel)` | hook menu\_fov.cpp | known; names.toml; addresses.h; ENGINE.md |
| `0x824E53F8` | `SPMainMenu::MoveDown` (likely) | vtable slot 26; meaning from Options &gt; Gameplay vtbl[26] move selection down |  |  | medium |
| `0x824E5438` | `SPMainMenu::MoveUp` (likely) | vtable slot 25; meaning from Options &gt; Gameplay vtbl[25] move selection up |  |  | medium |
| `0x824E5478` | `SPMainMenu::Init` (likely) | vtable slot 0; meaning from Options &gt; Gameplay vtbl[0] Init(this, panel) | `(this, panel)` |  | medium |
| `0x824E56B8` | `MainMenu::Init` (likely) | hooked | `(this, panel)` | hook pc\_menu.cpp | medium; addresses.h; comments |
| `0x824E58C0` | `MainMenu::vfunc_20` | rtti MainMenu vtable 0x82071DDC slot 20 |  |  | high; names.toml |
| `0x824E5DF8` | `ControlsPage_Apply` | addresses.h: the Controls page copies its rows into the settings |  |  | known; names.toml |
| `0x824E6020` | `AdvancedOptionsMenu::vfunc_19` | vfunc\_19 (this, item): 9 Reset, 10 Back; hooked | `(this, item)` | hook menu\_fov.cpp | high; names.toml; addresses.h |
| `0x824E60E8` | `AdvancedOptionsMenu::Init` (likely) | (this, panel); hooked | `(this, panel)` | hook menu\_fov.cpp | medium; addresses.h; comments |
| `0x824E6588` | `AdvancedOptionsMenu::Refresh` (likely) | vtbl[21]; hooked | `(this)` | hook menu\_fov.cpp | medium; addresses.h |
| `0x824E6640` | `PauseMenu::Init` (likely) | (this, panel); hooked | `(this, panel)` | hook pc\_menu.cpp | medium; addresses.h; notes: pc-menu; comments |
| `0x824E6EB0` | `FrontEndMenuSystem::ctor` | rtti: stores the FrontEndMenuSystem vtable at this+0; called on the result of operator new |  |  | high; names.toml |
| `0x824E73B8` | `TitleMenu::Refresh` (likely) | vtable slot 21; meaning from Options &gt; Gameplay vtbl[21] Refresh(this) | `(this)` |  | medium |
| `0x824E7468` | `FEMenuSystem::vfunc_5` | rtti FEMenuSystem vtable 0x820719D4 slot 5 (also in 3 derived vtables) |  |  | high; names.toml |
| `0x824E75B0` | `FEMenuSystem::vfunc_6` | rtti FEMenuSystem vtable 0x820719D4 slot 6 (also in 3 derived vtables) |  |  | high; names.toml |
| `0x824E7618` | `FEMenuSystem::vfunc_7` | rtti FEMenuSystem vtable 0x820719D4 slot 7 (also in 3 derived vtables) |  |  | high; names.toml |
| `0x824E7680` | `FEMenuSystem::vfunc_3` | rtti FEMenuSystem vtable 0x820719D4 slot 3 (also in 3 derived vtables) |  |  | high; names.toml |
| `0x824E76F8` | `FEMenu::vfunc_8` | rtti FEMenu vtable 0x82072A8C slot 8 (also in 23 derived vtables) |  |  | high; names.toml |
| `0x824E7810` | `FEMenu::vfunc_7` | rtti FEMenu vtable 0x82072A8C slot 7 (also in 23 derived vtables) |  |  | high; names.toml |
| `0x824E78A8` | `FEMenuEntry::ctor` | rtti: stores the FEMenuEntry vtable at this+0; called on the result of operator new |  |  | high; names.toml |
| `0x824E8098` | `FEMultiLineText::vfunc_91` | rtti FEMultiLineText vtable 0x82070C84 slot 91 |  | hook button\_prompts.cpp | high; names.toml |
| `0x824E8180` | `FEMultiLineText::vfunc_36` | rtti FEMultiLineText vtable 0x82070C84 slot 36 |  | hook button\_prompts.cpp | high; names.toml |
| `0x824E8428` | `PanelQuad::vfunc_20` | rtti PanelQuad vtable 0x82070E04 slot 20 |  |  | high; names.toml |
| `0x824E8848` | `FETypewriterText::vfunc_2` | rtti FETypewriterText vtable 0x820714A4 slot 2 |  |  | high; names.toml |
| `0x824EB0D8` | `ObjectiveMenu::Init` (likely) | vtable slot 0; meaning from Options &gt; Gameplay vtbl[0] Init(this, panel) | `(this, panel)` |  | medium |
| `0x824EB920` | `DifficultyMenu::vfunc_19` | rtti DifficultyMenu vtable 0x820728BC slot 19 |  |  | high; names.toml |
| `0x824EBA28` | `BonusSelectMenu::vfunc_29` | rtti BonusSelectMenu vtable 0x820727D4 slot 29 |  |  | high; names.toml |
| `0x824EBB18` | `BonusMainMenu::vfunc_19` | rtti BonusMainMenu vtable 0x8207251C slot 19 |  |  | high; names.toml |
| `0x824EBF28` | `SPMainMenu::vfunc_19` | rtti SPMainMenu vtable 0x82071EC4 slot 19 |  |  | high; names.toml |
| `0x824EBF78` | `MainMenu::vfunc_14` | vtbl[14]: panel, items, footer; hooked |  | hook pc\_menu.cpp | high; names.toml; addresses.h |
| `0x824EC2F0` | `FEMenu::vfunc_5` | rtti FEMenu vtable 0x82072A8C slot 5 (also in 23 derived vtables) |  |  | high; names.toml |
| `0x824ECDB8` | `DialogMenu::ctor` | rtti: stores the DialogMenu vtable at this+0; called on the result of operator new |  |  | high; names.toml |
| `0x824ECEE0` | `DialogMenuSystem::ctor` | rtti: stores the DialogMenuSystem vtable at this+0; called on the result of operator new |  |  | high; names.toml |
| `0x824ED130` | `ObjectiveMenu::Refresh` (likely) | vtable slot 21; meaning from Options &gt; Gameplay vtbl[21] Refresh(this) | `(this)` |  | medium |
| `0x824ED298` | `ProfileEditMenu::vfunc_20` | vtbl[20] (this, how); hooked | `(this, how)` | hook menu\_video.cpp | high; names.toml; addresses.h |
| `0x824ED460` | `SPMainMenu::vfunc_20` | rtti SPMainMenu vtable 0x82071EC4 slot 20 |  |  | high; names.toml |
| `0x824EE0B0` | `AutoSaveMenu::vfunc_2` | rtti AutoSaveMenu vtable 0x8207147C slot 2 |  |  | high; names.toml |
| `0x824EE128` | `AdvancedOptionsMenu::Update` (likely) | vtbl[16] (f1 = dt); hooked | `(f1 = dt)` | hook menu\_fov.cpp | medium; addresses.h |
| `0x824EE250` | `PauseMenu::Update` (likely) | vtable slot 16; meaning from Options &gt; Gameplay vtbl[16] Update(this, dt) | `(this, dt)` |  | medium |
| `0x824EE2E0` | `FEMenuSystem::vfunc_11` | rtti FEMenuSystem vtable 0x82070FC4 slot 11 |  |  | high; names.toml |
| `0x824EE4B8` | `SuccessFailMenu::Update` (likely) | vtable slot 16; meaning from Options &gt; Gameplay vtbl[16] Update(this, dt) | `(this, dt)` |  | medium |
| `0x824EE5C0` | `SuccessFailMenu::vfunc_29` | rtti SuccessFailMenu vtable 0x82071CF4 slot 29 |  |  | high; names.toml |
| `0x824EE850` | `InGameMenuSystem::vfunc_11` | rtti InGameMenuSystem vtable 0x82071884 slot 11 |  |  | high; names.toml |
| `0x824EE858` | `DialogMenuSystem::vfunc_11` | rtti DialogMenuSystem vtable 0x820719D4 slot 11 |  |  | high; names.toml |
| `0x824EF540` | `BonusContentMenu::vfunc_29` | rtti BonusContentMenu vtable 0x82072604 slot 29 |  |  | high; names.toml; notes: cutscenes |
| `0x824EF610` | `SPMainMenu::Update` (likely) | vtable slot 16; meaning from Options &gt; Gameplay vtbl[16] Update(this, dt) | `(this, dt)` |  | medium; notes: cutscenes |
| `0x824EF788` | `FrontEndMenuSystem::vfunc_11` | rtti FrontEndMenuSystem vtable 0x8207180C slot 11 |  |  | high; names.toml; notes: cutscenes |
| `0x824EFB18` | `TitleMenu::Update` (likely) | The title page: TitleMenu::Update (hooked) runs every frame; its "PRESS START" is an FEText at +60 ... | `(this, dt)` | hook button\_prompts.cpp | medium; addresses.h; notes: button-prompts, cutscenes; comments |
| `0x824EFE10` | `SuccessFailMenu::Refresh` (likely) | vtable slot 21; meaning from Options &gt; Gameplay vtbl[21] Refresh(this) | `(this)` |  | medium; notes: cutscenes |
| `0x824F0438` | `ProfileEditMenu::vfunc_30` | rtti ProfileEditMenu vtable 0x820729A4 slot 30 |  |  | high; names.toml |
| `0x824F07B0` | `AdvancedOptionsMenu::vfunc_30` | vfunc\_30 (this): B; hooked | `(this)` | hook menu\_fov.cpp | high; names.toml; addresses.h |
| `0x824F0B50` | `PauseMenu::vfunc_19` | The item handler (PauseMenu::vfunc\_19, this, item; a jump table): 1 opens Options (InGameMenuSystem ... |  | hook pc\_menu.cpp | high; names.toml; addresses.h |
| `0x82523558` | `DialogueManager::vfunc_1` | rtti DialogueManager vtable 0x82068EAC slot 1 |  |  | high; names.toml |
| `0x82525EF8` | `DialogueManager::ctor` | rtti: stores the DialogueManager vtable at this+0; called on the result of operator new |  |  | high; names.toml |
| `0x8252CB98` | `Localize` | The text of a string-table key in the game's language; empty when the key isn't there (the game ... | `const char* (StringTable* tables, const char* key)` | game\_api; hook menu\_fov.cpp; typed call game\_api.cpp; calls button\_prompts.cpp | known; names.toml; addresses.h; game\_api.h |

### Movies and loading screens (7)

| Address | Name | What it does | Arguments | Port use | Sources |
|---|---|---|---|---|---|
| `0x824D0470` | `movie::kSkipCheck` | true when the player skips (any button) |  | mid-asm Cod3MovieSkipHook default.toml; mid-asm Cod3MovieSkipped default.toml | addresses.h |
| `0x824D0528` | `movie::kStop` | Stops and frees the playing movie and its sound, and draws the loading screen. Reads no argument | `void ()` | game\_api; typed call game\_api.cpp; calls cutscene\_skip.cpp | addresses.h; game\_api.h |
| `0x824EEBA0` | `movie::kFinishLevelMovie` | the briefing, after loading (called by SV\_SpawnServer) |  | mid-asm Cod3BriefingLoopBegin default.toml; mid-asm Cod3MovieLoopEnd default.toml | addresses.h |
| `0x824EECF8` | `movie::kLoadingMovie` | the briefing as the loading screen |  | mid-asm Cod3LoadingMovieHook default.toml; refers to cutscene\_skip.cpp | addresses.h |
| `0x824EED20` | `movie::kStart` | open movies/&lt;name&gt;.wmv and its audio |  |  | addresses.h |
| `0x824EF308` | `movie::kPlayBlocking` | play until it ends or is skipped (logos, bonus) |  | mid-asm Cod3MovieLoopBegin default.toml; mid-asm Cod3MovieLoopEnd default.toml | addresses.h |
| `0x82526240` | `movie::kLoadingScreen` | the plain loading screen |  | refers to cutscene\_skip.cpp | addresses.h |

### In-game overlay widgets (IGO) (112)

| Address | Name | What it does | Arguments | Port use | Sources |
|---|---|---|---|---|---|
| `0x820ADB40` | `IGOWidget::vfunc_4` | rtti IGOWidget vtable 0x820715F4 slot 4 (also in 21 derived vtables) |  |  | high; names.toml |
| `0x820ADB48` | `IGOWidget::vfunc_5` | rtti IGOWidget vtable 0x820715F4 slot 5 (also in 21 derived vtables) |  |  | high; names.toml |
| `0x820ADB50` | `IGOWidget::vfunc_7` | rtti IGOWidget vtable 0x820715F4 slot 7 (also in 21 derived vtables) |  |  | high; names.toml |
| `0x820ADB60` | `IGOHealthWidget::vfunc_0` | rtti IGOHealthWidget vtable 0x82071218 slot 0 |  |  | high; names.toml |
| `0x820ADBE8` | `IGOAmmoWidget::vfunc_0` | rtti IGOAmmoWidget vtable 0x820712A8 slot 0 |  |  | high; names.toml |
| `0x820ADC90` | `IGOGrenadeWidget::vfunc_0` | rtti IGOGrenadeWidget vtable 0x820712CC slot 0 |  |  | high; names.toml |
| `0x820ADD58` | `IGOWeaponNameWidget::vfunc_0` | rtti IGOWeaponNameWidget vtable 0x82071314 slot 0 |  |  | high; names.toml |
| `0x820ADE00` | `IGOHintWidget::vfunc_0` | rtti IGOHintWidget vtable 0x82071434 slot 0 |  |  | high; names.toml |
| `0x820ADEF8` | `IGOConsoleTextWidget::dtor` | rtti: stores the IGOConsoleTextWidget vtable, then its base's (IGOWidget) |  |  | high; names.toml |
| `0x820AE010` | `IGOConsoleTextWidget::vfunc_0` | rtti IGOConsoleTextWidget vtable 0x820715D0 slot 0 |  |  | high; names.toml |
| `0x820AE2F0` | `IGOFrontEnd::vfunc_3` | rtti IGOFrontEnd vtable 0x82071618 slot 3 |  |  | high; names.toml |
| `0x820AE340` | `IGOProgressBarWidget::vfunc_0` | rtti IGOProgressBarWidget vtable 0x8207123C slot 0 |  |  | high; names.toml |
| `0x820AE390` | `IGORaiseFlagWidget::vfunc_0` | rtti IGORaiseFlagWidget vtable 0x82071260 slot 0 |  |  | high; names.toml |
| `0x820AE3E0` | `IGOInGameScoreWidget::vfunc_0` | rtti IGOInGameScoreWidget vtable 0x82071284 slot 0 |  |  | high; names.toml |
| `0x820AE430` | `IGOGrenadeCookWidget::vfunc_0` | rtti IGOGrenadeCookWidget vtable 0x820712F0 slot 0 |  |  | high; names.toml |
| `0x820AE480` | `IGOWidget::deleting_dtor` | vtable slot 0: (this, flags) with the flags & 1 delete test | `(this, flags)` |  | high; names.toml |
| `0x820AE4C8` | `IGOGrenadeIndicator::vfunc_0` | rtti IGOGrenadeIndicator vtable 0x82071458 slot 0 |  |  | high; names.toml |
| `0x820AE518` | `IGOStickArrowWidget::vfunc_0` | rtti IGOStickArrowWidget vtable 0x820715F4 slot 0 |  |  | high; names.toml |
| `0x824C08A8` | `IGOConsoleTextWidget::vfunc_3` | rtti IGOConsoleTextWidget vtable 0x820715D0 slot 3 |  |  | high; names.toml |
| `0x824C0D78` | `IGOStickArrowWidget::vfunc_3` | rtti IGOStickArrowWidget vtable 0x820715F4 slot 3 |  |  | high; names.toml |
| `0x824C1430` | `IGOHintWidget::vfunc_3` | rtti IGOHintWidget vtable 0x82071434 slot 3 |  |  | high; names.toml |
| `0x824C14D0` | `IGOJeepMapWidget::vfunc_3` | rtti IGOJeepMapWidget vtable 0x82071410 slot 3 |  |  | high; names.toml |
| `0x824C1AE0` | `IGOFriendlyWidget::vfunc_6` | rtti IGOFriendlyWidget vtable 0x820713C8 slot 6 |  |  | high; names.toml |
| `0x824C1B40` | `IGOFriendlyWidget::vfunc_3` | rtti IGOFriendlyWidget vtable 0x820713C8 slot 3 |  |  | high; names.toml |
| `0x824C1BB0` | `IGOFriendlyWidget::vfunc_2` | rtti IGOFriendlyWidget vtable 0x820713C8 slot 2 |  |  | high; names.toml |
| `0x824C1BB8` | `IGOTankIconWidget::vfunc_3` | rtti IGOTankIconWidget vtable 0x820713A4 slot 3 |  |  | high; names.toml |
| `0x824C1C18` | `IGOTankHealthWidget::vfunc_3` | rtti IGOTankHealthWidget vtable 0x82071338 slot 3 |  |  | high; names.toml |
| `0x824C1C98` | `IGOWeaponNameWidget::vfunc_3` | rtti IGOWeaponNameWidget vtable 0x82071314 slot 3 |  |  | high; names.toml |
| `0x824C1D10` | `IGOGrenadeCookWidget::vfunc_3` | rtti IGOGrenadeCookWidget vtable 0x820712F0 slot 3 |  |  | high; names.toml |
| `0x824C1D90` | `IGOGrenadeWidget::vfunc_3` | rtti IGOGrenadeWidget vtable 0x820712CC slot 3 |  |  | high; names.toml |
| `0x824C1E38` | `IGOAmmoWidget::vfunc_3` | rtti IGOAmmoWidget vtable 0x820712A8 slot 3 |  |  | high; names.toml |
| `0x824C1EB8` | `IGOInGameScoreWidget::vfunc_3` | rtti IGOInGameScoreWidget vtable 0x82071284 slot 3 |  |  | high; names.toml |
| `0x824C1F60` | `IGOInGameScoreWidget::vfunc_2` | rtti IGOInGameScoreWidget vtable 0x82071284 slot 2 |  |  | high; names.toml |
| `0x824C1F68` | `IGORaiseFlagWidget::vfunc_3` | rtti IGORaiseFlagWidget vtable 0x82071260 slot 3 |  |  | high; names.toml |
| `0x824C1FC8` | `IGORaiseFlagWidget::vfunc_2` | rtti IGORaiseFlagWidget vtable 0x82071260 slot 2 |  |  | high; names.toml |
| `0x824C1FD0` | `IGOProgressBarWidget::vfunc_3` | rtti IGOProgressBarWidget vtable 0x8207123C slot 3 |  |  | high; names.toml |
| `0x824C1FD8` | `IGOProgressBarWidget::vfunc_2` | rtti IGOProgressBarWidget vtable 0x8207123C slot 2 |  |  | high; names.toml |
| `0x824C1FE0` | `IGOHealthWidget::vfunc_3` | rtti IGOHealthWidget vtable 0x82071218 slot 3 |  |  | high; names.toml |
| `0x824C2088` | `IGOStanceWidget::vfunc_3` | rtti IGOStanceWidget vtable 0x820711F4 slot 3 |  |  | high; names.toml |
| `0x824C2140` | `IGOFrontEnd::vfunc_6` | rtti IGOFrontEnd vtable 0x82071618 slot 6 |  |  | high; names.toml |
| `0x824C2588` | `IGOFrontEnd::vfunc_7` | rtti IGOFrontEnd vtable 0x82071618 slot 7 |  |  | high; names.toml |
| `0x824C25D8` | `IGOFrontEnd::vfunc_5` | rtti IGOFrontEnd vtable 0x82071618 slot 5 |  |  | high; names.toml |
| `0x824C25E0` | `IGOFrontEnd::vfunc_1` | rtti IGOFrontEnd vtable 0x82071618 slot 1 |  |  | high; names.toml |
| `0x824C8F38` | `IGOStickArrowWidget::vfunc_6` | rtti IGOStickArrowWidget vtable 0x820715F4 slot 6 |  |  | high; names.toml |
| `0x824C8FA8` | `IGOStickArrowWidget::vfunc_2` | rtti IGOStickArrowWidget vtable 0x820715F4 slot 2 |  |  | high; names.toml |
| `0x824C90E0` | `IGOStickArrowWidget::dtor` | rtti: stores the IGOStickArrowWidget vtable, then its base's (IGOWidget) |  |  | high; names.toml |
| `0x824C9180` | `IGOGrenadeIndicator::vfunc_6` | rtti IGOGrenadeIndicator vtable 0x82071458 slot 6 |  |  | high; names.toml |
| `0x824C9288` | `IGOGrenadeIndicator::dtor` | rtti: stores the IGOGrenadeIndicator vtable, then its base's (IGOWidget) |  |  | high; names.toml |
| `0x824C9398` | `IGOJeepMapWidget::vfunc_6` | rtti IGOJeepMapWidget vtable 0x82071410 slot 6 |  |  | high; names.toml |
| `0x824C9BC8` | `IGOCompassWidget::vfunc_6` | rtti IGOCompassWidget vtable 0x820713EC slot 6 |  |  | high; names.toml |
| `0x824C9D28` | `IGOCompassWidget::vfunc_3` | rtti IGOCompassWidget vtable 0x820713EC slot 3 |  |  | high; names.toml |
| `0x824C9E60` | `IGOCompassWidget::ctor` | rtti: stores the IGOCompassWidget vtable at this+0; called on the result of operator new |  |  | high; names.toml |
| `0x824CA0F8` | `IGOTankIconWidget::vfunc_6` | rtti IGOTankIconWidget vtable 0x820713A4 slot 6 |  |  | high; names.toml |
| `0x824CA240` | `IGOTankReticleWidget::vfunc_6` | rtti IGOTankReticleWidget vtable 0x82071380 slot 6 |  |  | high; names.toml |
| `0x824CA2B8` | `IGOTankLoadingWidget::vfunc_6` | rtti IGOTankLoadingWidget vtable 0x8207135C slot 6 |  |  | high; names.toml |
| `0x824CA318` | `IGOTankHealthWidget::vfunc_6` | rtti IGOTankHealthWidget vtable 0x82071338 slot 6 |  |  | high; names.toml |
| `0x824CA3A8` | `IGOWeaponNameWidget::vfunc_6` | rtti IGOWeaponNameWidget vtable 0x82071314 slot 6 |  |  | high; names.toml |
| `0x824CA428` | `IGOWeaponNameWidget::vfunc_1` | rtti IGOWeaponNameWidget vtable 0x82071314 slot 1 |  |  | high; names.toml |
| `0x824CA4F8` | `IGOGrenadeCookWidget::vfunc_6` | rtti IGOGrenadeCookWidget vtable 0x820712F0 slot 6 |  |  | high; names.toml |
| `0x824CA540` | `IGOGrenadeCookWidget::dtor` | rtti: stores the IGOGrenadeCookWidget vtable, then its base's (IGOWidget) |  |  | high; names.toml |
| `0x824CA628` | `IGOGrenadeWidget::vfunc_6` | rtti IGOGrenadeWidget vtable 0x820712CC slot 6 |  |  | high; names.toml |
| `0x824CA7B0` | `IGOAmmoWidget::vfunc_6` | rtti IGOAmmoWidget vtable 0x820712A8 slot 6 |  |  | high; names.toml |
| `0x824CA988` | `IGOInGameScoreWidget::vfunc_6` | rtti IGOInGameScoreWidget vtable 0x82071284 slot 6 |  |  | high; names.toml |
| `0x824CA9B8` | `IGOInGameScoreWidget::dtor` | rtti: stores the IGOInGameScoreWidget vtable, then its base's (IGOWidget) |  |  | high; names.toml |
| `0x824CAA98` | `IGORaiseFlagWidget::vfunc_6` | rtti IGORaiseFlagWidget vtable 0x82071260 slot 6 |  |  | high; names.toml |
| `0x824CAAA0` | `IGORaiseFlagWidget::dtor` | rtti: stores the IGORaiseFlagWidget vtable, then its base's (IGOWidget) |  |  | high; names.toml |
| `0x824CAB20` | `IGOProgressBarWidget::vfunc_6` | rtti IGOProgressBarWidget vtable 0x8207123C slot 6 |  |  | high; names.toml |
| `0x824CAB88` | `IGOProgressBarWidget::dtor` | rtti: stores the IGOProgressBarWidget vtable, then its base's (IGOWidget) |  |  | high; names.toml |
| `0x824CAC68` | `IGOHealthWidget::vfunc_6` | rtti IGOHealthWidget vtable 0x82071218 slot 6 |  |  | high; names.toml |
| `0x824CACD8` | `IGOHealthWidget::vfunc_2` | rtti IGOHealthWidget vtable 0x82071218 slot 2 |  |  | high; names.toml |
| `0x824CAF58` | `IGOStanceWidget::vfunc_6` | rtti IGOStanceWidget vtable 0x820711F4 slot 6 |  |  | high; names.toml |
| `0x824D0EF0` | `IGOGrenadeIndicator::vfunc_2` | rtti IGOGrenadeIndicator vtable 0x82071458 slot 2 |  |  | high; names.toml |
| `0x824D10D8` | `IGOHintWidget::vfunc_6` | rtti IGOHintWidget vtable 0x82071434 slot 6 |  |  | high; names.toml |
| `0x824D1180` | `IGOJeepMapWidget::vfunc_2` | rtti IGOJeepMapWidget vtable 0x82071410 slot 2 |  |  | high; names.toml |
| `0x824D1960` | `IGOFriendlyWidget::vfunc_1` | rtti IGOFriendlyWidget vtable 0x820713C8 slot 1 |  |  | high; names.toml |
| `0x824D1A38` | `IGOTankLoadingWidget::vfunc_3` | rtti IGOTankLoadingWidget vtable 0x8207135C slot 3 |  |  | high; names.toml |
| `0x824D1CE8` | `IGOFrontEnd::vfunc_2` | rtti IGOFrontEnd vtable 0x82071618 slot 2 |  |  | high; names.toml |
| `0x824D47B8` | `IGOTankReticleWidget::vfunc_3` | rtti IGOTankReticleWidget vtable 0x82071380 slot 3 |  |  | high; names.toml |
| `0x824D4900` | `IGOWeaponNameWidget::vfunc_2` | rtti IGOWeaponNameWidget vtable 0x82071314 slot 2 |  |  | high; names.toml |
| `0x824D4B90` | `IGOGrenadeCookWidget::vfunc_2` | rtti IGOGrenadeCookWidget vtable 0x820712F0 slot 2 |  |  | high; names.toml |
| `0x824D4DB8` | `IGOGrenadeWidget::vfunc_2` | rtti IGOGrenadeWidget vtable 0x820712CC slot 2 |  |  | high; names.toml |
| `0x824D5240` | `IGOAmmoWidget::vfunc_2` | rtti IGOAmmoWidget vtable 0x820712A8 slot 2 |  |  | high; names.toml |
| `0x824D5780` | `IGOStanceWidget::vfunc_2` | rtti IGOStanceWidget vtable 0x820711F4 slot 2 |  |  | high; names.toml |
| `0x824D9010` | `IGOTankIconWidget::vfunc_2` | rtti IGOTankIconWidget vtable 0x820713A4 slot 2 |  |  | high; names.toml |
| `0x824D91F0` | `IGOTankReticleWidget::vfunc_2` | rtti IGOTankReticleWidget vtable 0x82071380 slot 2 |  |  | high; names.toml |
| `0x824D93E8` | `IGOTankLoadingWidget::vfunc_2` | rtti IGOTankLoadingWidget vtable 0x8207135C slot 2 |  |  | high; names.toml |
| `0x824D9468` | `IGOTankHealthWidget::vfunc_2` | rtti IGOTankHealthWidget vtable 0x82071338 slot 2 |  |  | high; names.toml |
| `0x824D9558` | `IGOWeaponNameWidget::ctor` | rtti: stores the IGOWeaponNameWidget vtable at this+0; called on the result of operator new |  |  | high; names.toml |
| `0x824D9758` | `IGOFrontEnd::vfunc_4` | rtti IGOFrontEnd vtable 0x82071618 slot 4 |  |  | high; names.toml |
| `0x824E0038` | `IGOStickArrowWidget::vfunc_1` | rtti IGOStickArrowWidget vtable 0x820715F4 slot 1 |  |  | high; names.toml |
| `0x824E01E8` | `IGOStickArrowWidget::ctor` | rtti: stores the IGOStickArrowWidget vtable at this+0; called on the result of operator new |  |  | high; names.toml |
| `0x824E02D0` | `IGOGrenadeIndicator::vfunc_1` | rtti IGOGrenadeIndicator vtable 0x82071458 slot 1 |  |  | high; names.toml |
| `0x824E05F0` | `IGOHintWidget::vfunc_1` | rtti IGOHintWidget vtable 0x82071434 slot 1 |  |  | high; names.toml |
| `0x824E0790` | `IGOJeepMapWidget::vfunc_1` | rtti IGOJeepMapWidget vtable 0x82071410 slot 1 |  |  | high; names.toml |
| `0x824E0B18` | `IGOTankIconWidget::vfunc_1` | rtti IGOTankIconWidget vtable 0x820713A4 slot 1 |  |  | high; names.toml |
| `0x824E0B78` | `IGOTankReticleWidget::vfunc_1` | rtti IGOTankReticleWidget vtable 0x82071380 slot 1 |  |  | high; names.toml |
| `0x824E0DB0` | `IGOTankLoadingWidget::vfunc_1` | rtti IGOTankLoadingWidget vtable 0x8207135C slot 1 |  |  | high; names.toml |
| `0x824E0E10` | `IGOTankHealthWidget::vfunc_1` | rtti IGOTankHealthWidget vtable 0x82071338 slot 1 |  |  | high; names.toml |
| `0x824E0EA8` | `IGOGrenadeCookWidget::vfunc_1` | rtti IGOGrenadeCookWidget vtable 0x820712F0 slot 1 |  |  | high; names.toml |
| `0x824E1098` | `IGOGrenadeWidget::vfunc_1` | rtti IGOGrenadeWidget vtable 0x820712CC slot 1 |  |  | high; names.toml |
| `0x824E1148` | `IGOAmmoWidget::vfunc_1` | rtti IGOAmmoWidget vtable 0x820712A8 slot 1 |  |  | high; names.toml |
| `0x824E1240` | `IGOInGameScoreWidget::vfunc_1` | rtti IGOInGameScoreWidget vtable 0x82071284 slot 1 |  |  | high; names.toml |
| `0x824E1368` | `IGORaiseFlagWidget::vfunc_1` | rtti IGORaiseFlagWidget vtable 0x82071260 slot 1 |  |  | high; names.toml |
| `0x824E1400` | `IGOProgressBarWidget::vfunc_1` | rtti IGOProgressBarWidget vtable 0x8207123C slot 1 |  |  | high; names.toml |
| `0x824E1528` | `IGOHealthWidget::vfunc_1` | rtti IGOHealthWidget vtable 0x82071218 slot 1 |  |  | high; names.toml |
| `0x824E1610` | `IGOStanceWidget::vfunc_1` | rtti IGOStanceWidget vtable 0x820711F4 slot 1 |  |  | high; names.toml |
| `0x824E1708` | `IGOFrontEnd::vfunc_0` | rtti IGOFrontEnd vtable 0x82071618 slot 0 |  |  | high; names.toml |
| `0x824E87D0` | `IGOConsoleTextWidget::ctor` | rtti: stores the IGOConsoleTextWidget vtable at this+0; called on the result of operator new |  |  | high; names.toml |
| `0x824E8A18` | `IGOGrenadeIndicator::vfunc_3` | rtti IGOGrenadeIndicator vtable 0x82071458 slot 3 |  |  | high; names.toml |
| `0x824EA870` | `IGOCompassWidget::vfunc_2` | rtti IGOCompassWidget vtable 0x820713EC slot 2 |  |  | high; names.toml |
| `0x824EC8C0` | `IGOConsoleTextWidget::vfunc_2` | rtti IGOConsoleTextWidget vtable 0x820715D0 slot 2 |  |  | high; names.toml |
| `0x824ED5A8` | `IGOHintWidget::vfunc_2` | rtti IGOHintWidget vtable 0x82071434 slot 2 |  |  | high; names.toml |

### Interactions (135)

| Address | Name | What it does | Arguments | Port use | Sources |
|---|---|---|---|---|---|
| `0x820A9DA0` | `InteractMetaAnimInstance::dtor` | rtti: stores the InteractMetaAnimInstance vtable, then its base's ... |  |  | high; names.toml |
| `0x820A9E10` | `InteractMetaAnimInstance::vfunc_1` | rtti InteractMetaAnimInstance vtable 0x8206CBB0 slot 1 |  |  | high; names.toml |
| `0x820A9E80` | `InteractMetaAnimInstance::vfunc_0` | rtti InteractMetaAnimInstance vtable 0x8206CBB0 slot 0 |  |  | high; names.toml |
| `0x8223FF98` | `InteractStateVehicleBase::vfunc_4` | rtti InteractStateVehicleBase vtable 0x8206CF7C slot 4 (also in 3 derived vtables) |  |  | high; names.toml |
| `0x824F0CE8` | `InteractStatePickLiveGrenade::vfunc_6` | rtti InteractStatePickLiveGrenade vtable 0x8206CE50 slot 6 |  |  | high; names.toml |
| `0x824F0D10` | `InteractStatePickLiveGrenade::vfunc_5` | rtti InteractStatePickLiveGrenade vtable 0x8206CE50 slot 5 |  |  | high; names.toml |
| `0x824F1448` | `InteractStateVehicleBase::vfunc_7` | rtti InteractStateVehicleBase vtable 0x8206CF7C slot 7 (also in 3 derived vtables) |  |  | high; names.toml |
| `0x824F14F0` | `InteractStateMortarLoad::vfunc_4` | rtti InteractStateMortarLoad vtable 0x8206CE20 slot 4 |  |  | high; names.toml |
| `0x824F28B8` | `InteractState::vfunc_6` | rtti InteractState vtable 0x8206CEB0 slot 6 (also in 21 derived vtables) |  |  | high; names.toml |
| `0x824F2920` | `InteractState::vfunc_5` | rtti InteractState vtable 0x8206CEB0 slot 5 (also in 21 derived vtables) |  |  | high; names.toml |
| `0x824F2988` | `InteractState::vfunc_4` | rtti InteractState vtable 0x8206CEB0 slot 4 (also in 22 derived vtables) |  |  | high; names.toml |
| `0x824F29F8` | `InteractMetaAnimData::vfunc_4` | rtti InteractMetaAnimData vtable 0x8206CBBC slot 4 |  |  | high; names.toml |
| `0x824F2A18` | `InteractMetaAnimData::vfunc_3` | rtti InteractMetaAnimData vtable 0x8206CBBC slot 3 |  |  | high; names.toml |
| `0x824F2A38` | `InteractMetaAnimData::vfunc_2` | rtti InteractMetaAnimData vtable 0x8206CBBC slot 2 |  |  | high; names.toml |
| `0x824F2A60` | `InteractMetaAnimData::vfunc_1` | rtti InteractMetaAnimData vtable 0x8206CBBC slot 1 |  |  | high; names.toml |
| `0x824F2A80` | `InteractInputRcvrRowboat::vfunc_1` | rtti InteractInputRcvrRowboat vtable 0x8206CA44 slot 1 |  |  | high; names.toml |
| `0x824F2A98` | `InteractInputRcvrStickSwirl::vfunc_1` | rtti InteractInputRcvrStickSwirl vtable 0x8206C9F0 slot 1 |  |  | high; names.toml |
| `0x824F2AB0` | `InteractInputRcvrButtonPress::vfunc_3` | rtti InteractInputRcvrButtonPress vtable 0x8206C9D4 slot 3 |  |  | high; names.toml |
| `0x824F2B48` | `InteractInputRcvrButtonMash::vfunc_3` | rtti InteractInputRcvrButtonMash vtable 0x8206C9B8 slot 3 |  |  | high; names.toml |
| `0x824F5380` | `InteractStateVehicleRelease::vfunc_11` | rtti InteractStateVehicleRelease vtable 0x8206CF7C slot 11 |  |  | high; names.toml |
| `0x824F5580` | `InteractStateVehicleBase::vfunc_11` | rtti InteractStateVehicleBase vtable 0x8206CF48 slot 11 (also in 2 derived vtables) |  |  | high; names.toml |
| `0x824F58F8` | `InteractStateMortarLoad::vfunc_6` | rtti InteractStateMortarLoad vtable 0x8206CE20 slot 6 |  |  | high; names.toml |
| `0x824F5980` | `InteractStateMortarLoad::vfunc_5` | rtti InteractStateMortarLoad vtable 0x8206CE20 slot 5 |  |  | high; names.toml |
| `0x824F63A0` | `InteractMetaAnimData::vfunc_7` | rtti InteractMetaAnimData vtable 0x8206CBBC slot 7 |  |  | high; names.toml |
| `0x824F6438` | `InteractInputRcvrRowboat::vfunc_0` | rtti InteractInputRcvrRowboat vtable 0x8206CA44 slot 0 |  |  | high; names.toml |
| `0x824F6590` | `InteractInputRcvrStickSwirl::vfunc_0` | rtti InteractInputRcvrStickSwirl vtable 0x8206C9F0 slot 0 |  |  | high; names.toml |
| `0x824F6790` | `InteractInputRcvrButtonMash::vfunc_0` | rtti InteractInputRcvrButtonMash vtable 0x8206C9B8 slot 0 |  |  | high; names.toml |
| `0x824F87A0` | `InteractMetaAnimData::vfunc_5` | rtti InteractMetaAnimData vtable 0x8206CBBC slot 5 |  |  | high; names.toml |
| `0x824F9F28` | `InteractState::ctor` | rtti: stores the InteractState vtable at this+0; called on the result of operator new |  |  | high; names.toml |
| `0x824FA1F8` | `InteractInputRcvrButtonPress::vfunc_0` | rtti InteractInputRcvrButtonPress vtable 0x8206C9D4 slot 0 |  |  | high; names.toml |
| `0x824FA6E0` | `InteractStatePickLiveGrenade::ctor` | rtti: stores the InteractStatePickLiveGrenade vtable at this+0; called on the result of operator new |  |  | high; names.toml |
| `0x824FB510` | `InteractStateRowboat::ctor` | rtti: stores the InteractStateRowboat vtable at this+0; called on the result of operator new |  |  | high; names.toml |
| `0x824FB830` | `InteractStateMelee::ctor` | rtti: stores the InteractStateMelee vtable at this+0; called on the result of operator new |  |  | high; names.toml |
| `0x824FBBD8` | `InteractStateLeverPush::ctor` | rtti: stores the InteractStateLeverPush vtable at this+0; called on the result of operator new |  |  | high; names.toml |
| `0x824FBCD0` | `InteractState::vfunc_10` | rtti InteractState vtable 0x8206CF7C slot 10 (also in 25 derived vtables) |  |  | high; names.toml |
| `0x824FBFA8` | `InteractState::vfunc_8` | rtti InteractState vtable 0x8206CF7C slot 8 (also in 27 derived vtables) |  |  | high; names.toml |
| `0x824FC110` | `InteractState::vfunc_1` | rtti InteractState vtable 0x8206C97C slot 1 |  |  | high; names.toml |
| `0x824FC450` | `InteractInputRcvrRowboat::vfunc_5` | rtti InteractInputRcvrRowboat vtable 0x8206CA44 slot 5 |  |  | high; names.toml |
| `0x824FC6B0` | `InteractInputRcvrStickToggleHoriz::vfunc_1` | rtti InteractInputRcvrStickToggleHoriz vtable 0x8206CA28 slot 1 |  |  | high; names.toml |
| `0x824FC6E8` | `InteractInputRcvrStickToggleHoriz::vfunc_0` | rtti InteractInputRcvrStickToggleHoriz vtable 0x8206CA28 slot 0 |  |  | high; names.toml |
| `0x824FC730` | `InteractInputRcvrStickToggleVert::vfunc_1` | rtti InteractInputRcvrStickToggleVert vtable 0x8206CA0C slot 1 |  |  | high; names.toml |
| `0x824FC768` | `InteractInputRcvrStickToggleVert::vfunc_0` | rtti InteractInputRcvrStickToggleVert vtable 0x8206CA0C slot 0 |  |  | high; names.toml |
| `0x824FC7B0` | `InteractInputRcvrButtonPress::vfunc_1` | rtti InteractInputRcvrButtonPress vtable 0x8206C9D4 slot 1 |  |  | high; names.toml |
| `0x824FCB18` | `InteractStatePickLiveGrenade::vfunc_1` | rtti InteractStatePickLiveGrenade vtable 0x8206CE50 slot 1 |  |  | high; names.toml |
| `0x824FD548` | `InteractStateVehicleBase::vfunc_1` | rtti InteractStateVehicleBase vtable 0x8206CEE0 slot 1 |  |  | high; names.toml |
| `0x824FD5C8` | `InteractStateRowboat::vfunc_1` | rtti InteractStateRowboat vtable 0x8206CEB0 slot 1 |  |  | high; names.toml |
| `0x824FD718` | `InteractStateMortarLoad::ctor` | rtti: stores the InteractStateMortarLoad vtable at this+0; called on the result of operator new |  |  | high; names.toml |
| `0x824FD778` | `InteractStateMortarIdle::ctor` | rtti: stores the InteractStateMortarIdle vtable at this+0; called on the result of operator new |  |  | high; names.toml |
| `0x824FD820` | `InteractStateMeleeStagedSuccess::vfunc_1` | rtti InteractStateMeleeStagedSuccess vtable 0x8206CD60 slot 1 |  |  | high; names.toml |
| `0x824FD8D0` | `InteractStateMeleeStagedInitiate::vfunc_1` | rtti InteractStateMeleeStagedInitiate vtable 0x8206CD30 slot 1 |  |  | high; names.toml |
| `0x824FD968` | `InteractStateMeleeFailure::vfunc_10` | rtti InteractStateMeleeFailure vtable 0x8206CD00 slot 10 |  |  | high; names.toml |
| `0x824FD9C0` | `InteractStateMeleeSuccess::vfunc_10` | rtti InteractStateMeleeSuccess vtable 0x8206CCD0 slot 10 |  |  | high; names.toml |
| `0x824FDA18` | `InteractStateMeleeSuccessSetup::vfunc_1` | rtti InteractStateMeleeSuccessSetup vtable 0x8206CCA0 slot 1 |  |  | high; names.toml |
| `0x824FDA20` | `InteractStateMeleeStart::vfunc_1` | rtti InteractStateMeleeStart vtable 0x8206CC70 slot 1 |  |  | high; names.toml |
| `0x824FDAA8` | `InteractStateMeleeInitiate::vfunc_1` | rtti InteractStateMeleeInitiate vtable 0x8206CC40 slot 1 |  |  | high; names.toml |
| `0x824FDB40` | `InteractStateMelee::vfunc_1` | rtti InteractStateMelee vtable 0x8206CC10 slot 1 |  |  | high; names.toml |
| `0x824FDD50` | `InteractStatePlayPlayerAnim::vfunc_1` | rtti InteractStatePlayPlayerAnim vtable 0x8206CB20 slot 1 (also in 1 derived vtables) |  |  | high; names.toml |
| `0x824FDD58` | `InteractStatePlayAnims::vfunc_1` | rtti InteractStatePlayAnims vtable 0x8206CA60 slot 1 |  |  | high; names.toml |
| `0x824FF3B0` | `InteractStateVehicleRelease::vfunc_1` | rtti InteractStateVehicleRelease vtable 0x8206CF7C slot 1 |  |  | high; names.toml |
| `0x824FF3B8` | `InteractStateVehicleTurn::vfunc_1` | rtti InteractStateVehicleTurn vtable 0x8206CF48 slot 1 |  |  | high; names.toml |
| `0x824FF428` | `InteractStateVehicleIdle::vfunc_1` | rtti InteractStateVehicleIdle vtable 0x8206CF14 slot 1 |  |  | high; names.toml |
| `0x824FF430` | `InteractStateRowboatInit::vfunc_1` | rtti InteractStateRowboatInit vtable 0x8206CE80 slot 1 |  |  | high; names.toml |
| `0x824FF438` | `InteractStateMortarLoad::vfunc_1` | rtti InteractStateMortarLoad vtable 0x8206CE20 slot 1 |  |  | high; names.toml |
| `0x824FF480` | `InteractStateMortarStart::vfunc_1` | rtti InteractStateMortarStart vtable 0x8206CDF0 slot 1 |  |  | high; names.toml |
| `0x824FF4C8` | `InteractStateMortarStart::ctor` | rtti: stores the InteractStateMortarStart vtable at this+0; called on the result of operator new |  |  | high; names.toml |
| `0x824FF528` | `InteractStateMortarIdle::vfunc_1` | rtti InteractStateMortarIdle vtable 0x8206CDC0 slot 1 |  |  | high; names.toml |
| `0x824FF570` | `InteractStateMeleeSuccess::vfunc_1` | rtti InteractStateMeleeSuccess vtable 0x8206CCD0 slot 1 |  |  | high; names.toml |
| `0x824FF668` | `InteractStatePush::vfunc_1` | rtti InteractStatePush vtable 0x8206CAC0 slot 1 |  |  | high; names.toml |
| `0x82502448` | `InteractState::vfunc_2` | rtti InteractState vtable 0x8206C97C slot 2 |  |  | high; names.toml |
| `0x82502740` | `InteractState::vfunc_0` | rtti InteractState vtable 0x8206C97C slot 0 |  |  | high; names.toml |
| `0x82502D40` | `InteractStatePickLiveGrenade::vfunc_2` | rtti InteractStatePickLiveGrenade vtable 0x8206CE50 slot 2 |  |  | high; names.toml |
| `0x82502F00` | `InteractStatePickLiveGrenade::vfunc_0` | rtti InteractStatePickLiveGrenade vtable 0x8206CE50 slot 0 |  |  | high; names.toml |
| `0x82503188` | `InteractStateVehicleBase::vfunc_2` | rtti InteractStateVehicleBase vtable 0x8206CEE0 slot 2 |  |  | high; names.toml |
| `0x825031E0` | `InteractStateVehicleBase::vfunc_0` | rtti InteractStateVehicleBase vtable 0x8206CEE0 slot 0 |  |  | high; names.toml |
| `0x82503368` | `InteractStateRowboat::vfunc_0` | rtti InteractStateRowboat vtable 0x8206CEB0 slot 0 |  |  | high; names.toml |
| `0x825035A8` | `InteractStateMeleeStagedSuccess::vfunc_2` | rtti InteractStateMeleeStagedSuccess vtable 0x8206CD60 slot 2 |  |  | high; names.toml |
| `0x825036C0` | `InteractStateMeleeStagedSuccess::vfunc_0` | rtti InteractStateMeleeStagedSuccess vtable 0x8206CD60 slot 0 |  |  | high; names.toml |
| `0x82503770` | `InteractStateMeleeStagedInitiate::vfunc_2` | rtti InteractStateMeleeStagedInitiate vtable 0x8206CD30 slot 2 |  |  | high; names.toml |
| `0x825037D8` | `InteractStateMeleeStagedInitiate::vfunc_0` | rtti InteractStateMeleeStagedInitiate vtable 0x8206CD30 slot 0 |  |  | high; names.toml |
| `0x825038C8` | `InteractStateMeleeSuccessSetup::vfunc_2` | rtti InteractStateMeleeSuccessSetup vtable 0x8206CCA0 slot 2 |  |  | high; names.toml |
| `0x82503A38` | `InteractStateMeleeSuccessSetup::vfunc_0` | rtti InteractStateMeleeSuccessSetup vtable 0x8206CCA0 slot 0 |  |  | high; names.toml |
| `0x82503AB8` | `InteractStateMeleeStart::vfunc_2` | rtti InteractStateMeleeStart vtable 0x8206CC70 slot 2 |  |  | high; names.toml |
| `0x82503B10` | `InteractStateMeleeInitiate::vfunc_2` | rtti InteractStateMeleeInitiate vtable 0x8206CC40 slot 2 |  |  | high; names.toml |
| `0x82503B78` | `InteractStateMeleeInitiate::vfunc_0` | rtti InteractStateMeleeInitiate vtable 0x8206CC40 slot 0 |  |  | high; names.toml |
| `0x825042E0` | `InteractStateMelee::vfunc_2` | rtti InteractStateMelee vtable 0x8206CC10 slot 2 |  |  | high; names.toml |
| `0x82504850` | `InteractStateMelee::vfunc_0` | rtti InteractStateMelee vtable 0x8206CC10 slot 0 |  |  | high; names.toml |
| `0x825049C0` | `InteractStateLeverPush::vfunc_0` | rtti InteractStateLeverPush vtable 0x8206CBE0 slot 0 |  |  | high; names.toml |
| `0x82504A50` | `InteractStatePlayPlayerAnim::vfunc_2` | rtti InteractStatePlayPlayerAnim vtable 0x8206CD90 slot 2 (also in 1 derived vtables) |  |  | high; names.toml |
| `0x82504BE0` | `InteractStatePlayPlayerAnim::vfunc_0` | rtti InteractStatePlayPlayerAnim vtable 0x8206CD90 slot 0 (also in 1 derived vtables) |  |  | high; names.toml |
| `0x82504C78` | `InteractStatePlayAnims::vfunc_2` | rtti InteractStatePlayAnims vtable 0x8206CA60 slot 2 |  |  | high; names.toml |
| `0x82504DA0` | `InteractStatePlayAnims::vfunc_0` | rtti InteractStatePlayAnims vtable 0x8206CA60 slot 0 |  |  | high; names.toml |
| `0x82504E48` | `InteractState::vfunc_9` | rtti InteractState vtable 0x8206CF7C slot 9 (also in 26 derived vtables) |  |  | high; names.toml |
| `0x82505390` | `InteractStateVehicleRelease::vfunc_2` | rtti InteractStateVehicleRelease vtable 0x8206CF7C slot 2 |  |  | high; names.toml |
| `0x82505450` | `InteractStateVehicleRelease::vfunc_0` | rtti InteractStateVehicleRelease vtable 0x8206CF7C slot 0 |  |  | high; names.toml |
| `0x82505538` | `InteractStateVehicleIdle::vfunc_2` | rtti InteractStateVehicleIdle vtable 0x8206CF14 slot 2 |  |  | high; names.toml |
| `0x82505AD0` | `InteractStateRowboat::vfunc_2` | rtti InteractStateRowboat vtable 0x8206CEB0 slot 2 |  |  | high; names.toml |
| `0x82505C08` | `InteractStateRowboatInit::vfunc_2` | rtti InteractStateRowboatInit vtable 0x8206CE80 slot 2 |  |  | high; names.toml |
| `0x82505C58` | `InteractStateRowboatInit::vfunc_0` | rtti InteractStateRowboatInit vtable 0x8206CE80 slot 0 |  |  | high; names.toml |
| `0x82505CF0` | `InteractStateMortarLoad::vfunc_0` | rtti InteractStateMortarLoad vtable 0x8206CE20 slot 0 |  |  | high; names.toml |
| `0x82505D68` | `InteractStateMortarIdle::vfunc_2` | rtti InteractStateMortarIdle vtable 0x8206CDC0 slot 2 |  |  | high; names.toml |
| `0x82505D70` | `InteractStateMortarIdle::vfunc_0` | rtti InteractStateMortarIdle vtable 0x8206CDC0 slot 0 |  |  | high; names.toml |
| `0x82505E68` | `InteractStateMeleeStagedInitiate::vfunc_9` | rtti InteractStateMeleeStagedInitiate vtable 0x8206CD30 slot 9 |  |  | high; names.toml |
| `0x82505EC0` | `InteractStateMeleeFailure::vfunc_2` | rtti InteractStateMeleeFailure vtable 0x8206CD00 slot 2 |  |  | high; names.toml |
| `0x82505EC8` | `InteractStateMeleeFailure::vfunc_0` | rtti InteractStateMeleeFailure vtable 0x8206CD00 slot 0 |  |  | high; names.toml |
| `0x82505F10` | `InteractStateMeleeSuccess::vfunc_0` | rtti InteractStateMeleeSuccess vtable 0x8206CCD0 slot 0 |  |  | high; names.toml |
| `0x82505F78` | `InteractStateMeleeStart::vfunc_0` | rtti InteractStateMeleeStart vtable 0x8206CC70 slot 0 |  |  | high; names.toml |
| `0x82506550` | `InteractStateLeverPush::vfunc_2` | rtti InteractStateLeverPush vtable 0x8206CBE0 slot 2 |  |  | high; names.toml |
| `0x825066C8` | `InteractStateLeverPush::vfunc_1` | rtti InteractStateLeverPush vtable 0x8206CBE0 slot 1 |  |  | high; names.toml |
| `0x825067B0` | `InteractStateScaleAnimSpeed::vfunc_2` | rtti InteractStateScaleAnimSpeed vtable 0x8206CB50 slot 2 |  |  | high; names.toml |
| `0x825069F8` | `InteractStateScaleAnimSpeed::vfunc_1` | rtti InteractStateScaleAnimSpeed vtable 0x8206CB50 slot 1 |  |  | high; names.toml |
| `0x82506AA0` | `InteractStateScaleAnimSpeed::vfunc_0` | rtti InteractStateScaleAnimSpeed vtable 0x8206CB50 slot 0 |  |  | high; names.toml |
| `0x82506AF0` | `InteractStatePlaceItem::vfunc_2` | rtti InteractStatePlaceItem vtable 0x8206CAF0 slot 2 |  |  | high; names.toml |
| `0x82506AF8` | `InteractStatePlaceItem::vfunc_1` | rtti InteractStatePlaceItem vtable 0x8206CAF0 slot 1 |  |  | high; names.toml |
| `0x82506E10` | `InteractStatePlaceItem::vfunc_0` | rtti InteractStatePlaceItem vtable 0x8206CAF0 slot 0 |  |  | high; names.toml |
| `0x82506E18` | `InteractStatePush::vfunc_2` | rtti InteractStatePush vtable 0x8206CAC0 slot 2 |  |  | high; names.toml |
| `0x82507060` | `InteractStatePush::vfunc_0` | rtti InteractStatePush vtable 0x8206CAC0 slot 0 |  |  | high; names.toml |
| `0x825072D0` | `InteractState::vfunc_3` | rtti InteractState vtable 0x8206CF7C slot 3 (also in 27 derived vtables) |  |  | high; names.toml |
| `0x82507980` | `InteractStateVehicleTurn::vfunc_2` | rtti InteractStateVehicleTurn vtable 0x8206CF48 slot 2 |  |  | high; names.toml |
| `0x82507BA0` | `InteractStateVehicleTurn::vfunc_0` | rtti InteractStateVehicleTurn vtable 0x8206CF48 slot 0 |  |  | high; names.toml |
| `0x82507C88` | `InteractStateVehicleIdle::vfunc_0` | rtti InteractStateVehicleIdle vtable 0x8206CF14 slot 0 |  |  | high; names.toml |
| `0x82507E38` | `InteractStateMeleeDropWeapon::vfunc_1` | rtti InteractStateMeleeDropWeapon vtable 0x8206CD90 slot 1 |  |  | high; names.toml |
| `0x82507F78` | `InteractStateStrengthTest::vfunc_2` | rtti InteractStateStrengthTest vtable 0x8206CB80 slot 2 |  |  | high; names.toml |
| `0x82508078` | `InteractStateStrengthTest::vfunc_1` | rtti InteractStateStrengthTest vtable 0x8206CB80 slot 1 |  |  | high; names.toml |
| `0x825080B8` | `InteractStateStrengthTest::vfunc_0` | rtti InteractStateStrengthTest vtable 0x8206CB80 slot 0 |  |  | high; names.toml |
| `0x82508198` | `InteractStateVehicleLink::vfunc_2` | rtti InteractStateVehicleLink vtable 0x8206CB20 slot 2 |  |  | high; names.toml |
| `0x825081F0` | `InteractStateVehicleLink::vfunc_0` | rtti InteractStateVehicleLink vtable 0x8206CB20 slot 0 |  |  | high; names.toml |
| `0x82508238` | `InteractInputRcvrStickSwirl::vfunc_5` | rtti InteractInputRcvrStickSwirl vtable 0x8206C9F0 slot 5 |  |  | high; names.toml |
| `0x825083C0` | `InteractInputRcvrButtonPress::vfunc_5` | rtti InteractInputRcvrButtonPress vtable 0x8206C9D4 slot 5 |  |  | high; names.toml |
| `0x82508558` | `InteractInputRcvrButtonMash::vfunc_5` | rtti InteractInputRcvrButtonMash vtable 0x8206C9B8 slot 5 |  |  | high; names.toml |
| `0x82508618` | `InteractInputRcvrButtonMash::vfunc_1` | rtti InteractInputRcvrButtonMash vtable 0x8206C9B8 slot 1 |  |  | high; names.toml |
| `0x82508770` | `InteractStateMortarLoad::vfunc_2` | rtti InteractStateMortarLoad vtable 0x8206CE20 slot 2 |  |  | high; names.toml |
| `0x82508860` | `InteractStateMortarStart::vfunc_2` | rtti InteractStateMortarStart vtable 0x8206CDF0 slot 2 |  |  | high; names.toml |
| `0x82508910` | `InteractStateMortarStart::vfunc_0` | rtti InteractStateMortarStart vtable 0x8206CDF0 slot 0 |  |  | high; names.toml |
| `0x82508DB8` | `InteractStateMeleeSuccess::vfunc_2` | rtti InteractStateMeleeSuccess vtable 0x8206CCD0 slot 2 |  |  | high; names.toml |
| `0x825090D8` | `InteractStateMeleeFailure::vfunc_1` | rtti InteractStateMeleeFailure vtable 0x8206CD00 slot 1 |  |  | high; names.toml |

### Effects and animation (152)

| Address | Name | What it does | Arguments | Port use | Sources |
|---|---|---|---|---|---|
| `0x820A4B58` | `nalHeap::deleting_dtor` | vtable slot 0: (this, flags) with the flags & 1 delete test | `(this, flags)` |  | high; names.toml |
| `0x820A5718` | `AbstractEffectParticle::vfunc_3` | rtti AbstractEffectParticle vtable 0x82068EF8 slot 3 |  |  | high; names.toml |
| `0x820A5800` | `AbstractEffect::deleting_dtor` | vtable slot 0: (this, flags) with the flags & 1 delete test | `(this, flags)` |  | high; names.toml |
| `0x820A5860` | `AbstractEffectSound::vfunc_0` | rtti AbstractEffectSound vtable 0x82068F28 slot 0 |  |  | high; names.toml |
| `0x820A58B8` | `AbstractEffectParticle::vfunc_0` | rtti AbstractEffectParticle vtable 0x82068EF8 slot 0 |  |  | high; names.toml |
| `0x820A5910` | `AbstractEffectLight::vfunc_0` | rtti AbstractEffectLight vtable 0x82068F58 slot 0 |  |  | high; names.toml |
| `0x820A5968` | `AbstractEffectShakeAndRumble::vfunc_0` | rtti AbstractEffectShakeAndRumble vtable 0x82068F88 slot 0 |  |  | high; names.toml |
| `0x820A59E0` | `AnimHeap::vfunc_0` | rtti AnimHeap vtable 0x82068FB8 slot 0 |  |  | high; names.toml |
| `0x820A69C8` | `nalVirtual::deleting_dtor` | vtable slot 1: (this, flags) with the flags & 1 delete test | `(this, flags)` |  | high; names.toml |
| `0x820A7608` | `AnimationPlayer_nalPlayMethod::vfunc_1` | rtti AnimationPlayer::nalPlayMethod vtable 0x8206941C slot 1 (also in 1 derived vtables) |  |  | high; names.toml |
| `0x820A7CE0` | `apsRenderSort_SortedParticleIterator::vfunc_1` | rtti apsRenderSort::SortedParticleIterator vtable 0x820693E8 slot 1 |  |  | high; names.toml |
| `0x820A7D18` | `apsRenderSort_UnsortedParticleIterator::vfunc_1` | rtti apsRenderSort::UnsortedParticleIterator vtable 0x820693F4 slot 1 |  |  | high; names.toml |
| `0x820A7D48` | `apsRenderSort_ParticleIterator::deleting_dtor` | vtable slot 0: (this, flags) with the flags & 1 delete test | `(this, flags)` |  | high; names.toml |
| `0x820A8340` | `MetaAnimPlayMethod::vfunc_2` | rtti MetaAnimPlayMethod vtable 0x8206941C slot 2 |  |  | high; names.toml |
| `0x820A9748` | `nalBasePoseBlender::deleting_dtor` | vtable slot 0: (this, flags) with the flags & 1 delete test | `(this, flags)` |  | high; names.toml |
| `0x820A9790` | `nalAnimClass_nalAnyPose_nalInstanceClass::deleting_dtor` | vtable slot 0: (this, flags) with the flags & 1 delete test | `(this, flags)` |  | high; names.toml |
| `0x820A97E8` | `nalGeneric_nalGenericPoseBlender::vfunc_1` | rtti nalGeneric::nalGenericPoseBlender vtable 0x8206C93C slot 1 |  |  | high; names.toml |
| `0x820A97F0` | `nalGeneric_nalGenericPoseBlender::vfunc_0` | rtti nalGeneric::nalGenericPoseBlender vtable 0x8206C93C slot 0 |  |  | high; names.toml |
| `0x820A9860` | `nalInitList::deleting_dtor` | vtable slot 0: (this, flags) with the flags & 1 delete test | `(this, flags)` |  | high; names.toml |
| `0x820A98A8` | `nalComponentBase::vfunc_4` | rtti nalComponentBase vtable 0x8206CFB4 slot 4 (also in 1 derived vtables) |  |  | high; names.toml |
| `0x820A98B8` | `AnimationPlayer_nalAnimCallback::vfunc_1` | rtti AnimationPlayer::nalAnimCallback vtable 0x8206C96C slot 1 |  |  | high; names.toml |
| `0x820A98C0` | `AnimationPlayer_nalAnimCallback::vfunc_2` | rtti AnimationPlayer::nalAnimCallback vtable 0x8206C96C slot 2 |  |  | high; names.toml |
| `0x820A9F38` | `CODNoteTrack::vfunc_1` | rtti CODNoteTrack vtable 0x8206CFB4 slot 1 |  |  | high; names.toml |
| `0x820A9F48` | `CODNoteTrack::vfunc_26` | rtti CODNoteTrack vtable 0x8206CFB4 slot 26 |  |  | high; names.toml |
| `0x820A9F60` | `nalComponentBase::deleting_dtor` | vtable slot 0: (this, flags) with the flags & 1 delete test | `(this, flags)` |  | high; names.toml |
| `0x820AA0A8` | `SceneAnimClient::ctor` | rtti: stores the SceneAnimClient vtable at this+0; called on the result of operator new |  |  | high; names.toml |
| `0x820AA3A0` | `SceneAnimClient::vfunc_0` | rtti SceneAnimClient vtable 0x8206D028 slot 0 |  |  | high; names.toml |
| `0x820AA830` | `SceneAnimClient::vfunc_1` | rtti SceneAnimClient vtable 0x8206D028 slot 1 |  |  | high; names.toml |
| `0x820AB660` | `SceneAnimClient::vfunc_3` | rtti SceneAnimClient vtable 0x8206D028 slot 3 |  |  | high; names.toml |
| `0x820AB738` | `nalClientSceneAnim::deleting_dtor` | vtable slot 4: (this, flags) with the flags & 1 delete test | `(this, flags)` |  | high; names.toml |
| `0x820AB780` | `AnimBankManager::deleting_dtor` | vtable slot 0: (this, flags) with the flags & 1 delete test | `(this, flags)` |  | high; names.toml |
| `0x820AB968` | `CODNoteTrack::vfunc_8` | rtti CODNoteTrack vtable 0x8206CFB4 slot 8 |  |  | high; names.toml |
| `0x820ABAE0` | `CODNoteTrack::vfunc_11` | rtti CODNoteTrack vtable 0x8206CFB4 slot 11 |  |  | high; names.toml |
| `0x820ABBC0` | `CODNoteTrack::vfunc_12` | rtti CODNoteTrack vtable 0x8206CFB4 slot 12 |  |  | high; names.toml |
| `0x820ABCE8` | `CODNoteTrack::vfunc_16` | rtti CODNoteTrack vtable 0x8206CFB4 slot 16 |  |  | high; names.toml |
| `0x820ABD10` | `CODNoteTrack::vfunc_17` | rtti CODNoteTrack vtable 0x8206CFB4 slot 17 |  |  | high; names.toml |
| `0x820ABD28` | `CODNoteTrack::vfunc_18` | rtti CODNoteTrack vtable 0x8206CFB4 slot 18 |  |  | high; names.toml |
| `0x820AC928` | `PanelAnimObject::vfunc_4` | rtti PanelAnimObject vtable 0x820714A4 slot 4 (also in 4 derived vtables) |  |  | high; names.toml |
| `0x820AC958` | `PanelAnimObject::vfunc_5` | rtti PanelAnimObject vtable 0x820714A4 slot 5 (also in 4 derived vtables) |  |  | high; names.toml |
| `0x820AC970` | `PanelAnimObject::vfunc_10` | rtti PanelAnimObject vtable 0x820714A4 slot 10 (also in 3 derived vtables) |  |  | high; names.toml |
| `0x820AC978` | `PanelAnimObject::vfunc_11` | rtti PanelAnimObject vtable 0x820714A4 slot 11 (also in 4 derived vtables) |  |  | high; names.toml |
| `0x820AC980` | `PanelAnimObject::vfunc_13` | rtti PanelAnimObject vtable 0x820714A4 slot 13 (also in 4 derived vtables) |  |  | high; names.toml |
| `0x820AC9B0` | `PanelAnimObject::vfunc_14` | rtti PanelAnimObject vtable 0x820714A4 slot 14 (also in 4 derived vtables) |  |  | high; names.toml |
| `0x820AC9C0` | `PanelAnimObject::vfunc_15` | rtti PanelAnimObject vtable 0x820714A4 slot 15 (also in 4 derived vtables) |  |  | high; names.toml |
| `0x820AC9F0` | `PanelAnimObject::deleting_dtor` | vtable slot 0: (this, flags) with the flags & 1 delete test | `(this, flags)` |  | high; names.toml |
| `0x820AE850` | `OptionsSoundMenu::OnLeft` (likely) | vtable slot 27; meaning from Options &gt; Gameplay vtbl[27] = Toggle(this, 0) |  |  | medium |
| `0x820AE858` | `OptionsSoundMenu::OnRight` (likely) | vtable slot 28; meaning from Options &gt; Gameplay vtbl[28] = Toggle(this, 1) |  |  | medium |
| `0x820AED08` | `OptionsSoundMenu::vfunc_3` | rtti OptionsSoundMenu vtable 0x8207217C slot 3 |  |  | high; names.toml |
| `0x820C0228` | `cdResourceDirectory_nalAnimClass_nalAnyPose::vfunc_1` | rtti cdResourceDirectory&lt;nalAnimClass&lt;nalAnyPose&gt;&gt; vtable 0x820783B4 slot 1 |  |  | high; names.toml |
| `0x820C0238` | `cdResourceDirectory_nalSceneAnim::vfunc_1` | rtti cdResourceDirectory&lt;nalSceneAnim&gt; vtable 0x820783DC slot 1 |  |  | high; names.toml |
| `0x820C0248` | `cdResourceDirectory_nalAnimFile::vfunc_1` | rtti cdResourceDirectory&lt;nalAnimFile&gt; vtable 0x82078404 slot 1 |  |  | high; names.toml |
| `0x820C0BC8` | `cdResourceDirectory_nalAnimClass_nalAnyPose::vfunc_8` | rtti cdResourceDirectory&lt;nalAnimClass&lt;nalAnyPose&gt;&gt; vtable 0x820783B4 slot 8 |  |  | high; names.toml |
| `0x820C0BF8` | `tlResourceDirectory_nalAnimClass_nalAnyPose::vfunc_7` | rtti tlResourceDirectory&lt;nalAnimClass&lt;nalAnyPose&gt;&gt; vtable 0x820783B4 slot 7 (also in 1 derived ... |  |  | high; names.toml |
| `0x820C0C00` | `tlResourceDirectory_nalAnimClass_nalAnyPose::vfunc_8` | rtti tlResourceDirectory&lt;nalAnimClass&lt;nalAnyPose&gt;&gt; vtable 0x82078310 slot 8 |  |  | high; names.toml |
| `0x820C0C08` | `cdResourceDirectory_nalSceneAnim::vfunc_8` | rtti cdResourceDirectory&lt;nalSceneAnim&gt; vtable 0x820783DC slot 8 |  |  | high; names.toml |
| `0x820C0C38` | `tlResourceDirectory_nalSceneAnim::vfunc_7` | rtti tlResourceDirectory&lt;nalSceneAnim&gt; vtable 0x820783DC slot 7 (also in 1 derived vtables) |  |  | high; names.toml |
| `0x820C0C40` | `tlResourceDirectory_nalSceneAnim::vfunc_8` | rtti tlResourceDirectory&lt;nalSceneAnim&gt; vtable 0x82078360 slot 8 |  |  | high; names.toml |
| `0x820C0C48` | `cdResourceDirectory_nalAnimFile::vfunc_8` | rtti cdResourceDirectory&lt;nalAnimFile&gt; vtable 0x82078404 slot 8 |  |  | high; names.toml |
| `0x820C0E18` | `tlResourceDirectory_nalAnimFile::vfunc_7` | rtti tlResourceDirectory&lt;nalAnimFile&gt; vtable 0x82078404 slot 7 (also in 1 derived vtables) |  |  | high; names.toml |
| `0x820C0E20` | `tlResourceDirectory_nalAnimFile::vfunc_8` | rtti tlResourceDirectory&lt;nalAnimFile&gt; vtable 0x82078338 slot 8 |  |  | high; names.toml |
| `0x820C0F58` | `cdResourceDirectory_nalAnimClass_nalAnyPose::deleting_dtor` | vtable slot 0: (this, flags) with the flags & 1 delete test | `(this, flags)` |  | high; names.toml |
| `0x820C0FB0` | `tlResourceDirectory_nalAnimClass_nalAnyPose::deleting_dtor` | vtable slot 0: (this, flags) with the flags & 1 delete test | `(this, flags)` |  | high; names.toml |
| `0x820C0FF8` | `cdResourceDirectory_nalSceneAnim::deleting_dtor` | vtable slot 0: (this, flags) with the flags & 1 delete test | `(this, flags)` |  | high; names.toml |
| `0x820C1050` | `tlResourceDirectory_nalSceneAnim::deleting_dtor` | vtable slot 0: (this, flags) with the flags & 1 delete test | `(this, flags)` |  | high; names.toml |
| `0x820C1098` | `cdResourceDirectory_nalAnimFile::deleting_dtor` | vtable slot 0: (this, flags) with the flags & 1 delete test | `(this, flags)` |  | high; names.toml |
| `0x820C10F0` | `tlResourceDirectory_nalAnimFile::deleting_dtor` | vtable slot 0: (this, flags) with the flags & 1 delete test | `(this, flags)` |  | high; names.toml |
| `0x820C1500` | `GlobalEffect::vfunc_2` | rtti GlobalEffect vtable 0x82079A28 slot 2 |  |  | high; names.toml |
| `0x820C16B0` | `ADSMetaAnimInstance::ctor` | rtti: stores the ADSMetaAnimInstance vtable at this+0; called on the result of operator new |  |  | high; names.toml |
| `0x820C1788` | `ADSMetaAnimInstance::dtor` | rtti: stores the ADSMetaAnimInstance vtable, then its base's ... |  |  | high; names.toml |
| `0x820C1818` | `ADSMetaAnimInstance::vfunc_1` | rtti ADSMetaAnimInstance vtable 0x82079A64 slot 1 |  |  | high; names.toml |
| `0x820C1890` | `ADSMetaAnimInstance::vfunc_0` | rtti ADSMetaAnimInstance vtable 0x82079A64 slot 0 |  |  | high; names.toml |
| `0x820C3310` | `apsClient::deleting_dtor` | vtable slot 0: (this, flags) with the flags & 1 delete test | `(this, flags)` |  | high; names.toml |
| `0x82160DF0` | `MetaNalBaseAnim::deleting_dtor` | vtable slot 1: (this, flags) with the flags & 1 delete test | `(this, flags)` |  | high; names.toml |
| `0x8216A3B8` | `CODNoteTrack::vfunc_25` | rtti CODNoteTrack vtable 0x8206CFB4 slot 25 |  |  | high; names.toml |
| `0x821721D0` | `nalAnimClass_nalAnyPose_nalInstanceClass::ctor` | rtti: stores the nalAnimClass&lt;nalAnyPose&gt;::nalInstanceClass vtable at this+0; called on the result ... |  |  | high; names.toml |
| `0x82174C88` | `nalComponentInitList::vfunc_1` | rtti nalComponentInitList vtable 0x8206C948 slot 1 |  |  | high; names.toml |
| `0x82423F08` | `FX_PlayFX` | string 'FX\_PlayFX:-- %x failed to create ParticleEffect' (only user) |  |  | high; names.toml |
| `0x824293E8` | `FX_RegisterEffect` | string 'FX\_RegisterEffect:-- %s FAILED' (only user) |  |  | high; names.toml |
| `0x824325B0` | `ADSMetaAnimData::vfunc_4` | rtti ADSMetaAnimData vtable 0x82079A40 slot 4 |  |  | high; names.toml |
| `0x824325D0` | `ADSMetaAnimData::vfunc_3` | rtti ADSMetaAnimData vtable 0x82079A40 slot 3 |  |  | high; names.toml |
| `0x824325F0` | `ADSMetaAnimData::vfunc_2` | rtti ADSMetaAnimData vtable 0x82079A40 slot 2 |  |  | high; names.toml |
| `0x82432618` | `ADSMetaAnimData::vfunc_1` | rtti ADSMetaAnimData vtable 0x82079A40 slot 1 |  |  | high; names.toml |
| `0x824343F0` | `ADSMetaAnimData::vfunc_7` | rtti ADSMetaAnimData vtable 0x82079A40 slot 7 |  |  | high; names.toml |
| `0x82438110` | `ADSMetaAnimData::vfunc_5` | rtti ADSMetaAnimData vtable 0x82079A40 slot 5 |  |  | high; names.toml |
| `0x824589F0` | `cdResourceDirectory_nalAnimClass_nalAnyPose::ctor` | rtti: stores the cdResourceDirectory&lt;nalAnimClass&lt;nalAnyPose&gt;&gt; vtable at this+0; called on the ... |  |  | high; names.toml |
| `0x8245A430` | `cdResourceDirectory_nalAnimFile::vfunc_3` | rtti cdResourceDirectory&lt;nalAnimFile&gt; vtable 0x82078404 slot 3 |  |  | high; names.toml |
| `0x8245A4C8` | `cdResourceDirectory_nalAnimFile::vfunc_2` | rtti cdResourceDirectory&lt;nalAnimFile&gt; vtable 0x82078404 slot 2 |  |  | high; names.toml |
| `0x8245A530` | `cdResourceDirectory_nalSceneAnim::vfunc_3` | rtti cdResourceDirectory&lt;nalSceneAnim&gt; vtable 0x820783DC slot 3 |  |  | high; names.toml |
| `0x8245A5C8` | `cdResourceDirectory_nalSceneAnim::vfunc_2` | rtti cdResourceDirectory&lt;nalSceneAnim&gt; vtable 0x820783DC slot 2 |  |  | high; names.toml |
| `0x8245A630` | `cdResourceDirectory_nalAnimClass_nalAnyPose::vfunc_3` | rtti cdResourceDirectory&lt;nalAnimClass&lt;nalAnyPose&gt;&gt; vtable 0x820783B4 slot 3 |  |  | high; names.toml |
| `0x8245A6C8` | `cdResourceDirectory_nalAnimClass_nalAnyPose::vfunc_2` | rtti cdResourceDirectory&lt;nalAnimClass&lt;nalAnyPose&gt;&gt; vtable 0x820783B4 slot 2 |  |  | high; names.toml |
| `0x82471F80` | `AnimNotifyTask::ctor` (likely) | rtti: stores the AnimNotifyTask vtable at this+0; stores base then own vtables and returns this |  |  | medium |
| `0x82493018` | `AnimNotifyTask::vfunc_1` | rtti AnimNotifyTask vtable 0x82077558 slot 1 |  |  | high; names.toml |
| `0x824B3270` | `EffectEventPlay` (likely) | string 'EffectEventPlay: skipping effect %s, entity already has a...' (only user) |  |  | medium |
| `0x824BCF08` | `OptionsSoundMenu::MoveDown` (likely) | vtable slot 26; meaning from Options &gt; Gameplay vtbl[26] move selection down |  |  | medium |
| `0x824BCF10` | `OptionsSoundMenu::MoveUp` (likely) | vtable slot 25; meaning from Options &gt; Gameplay vtbl[25] move selection up |  |  | medium |
| `0x824BF6F8` | `PanelAnimObject::vfunc_8` | rtti PanelAnimObject vtable 0x820714A4 slot 8 (also in 4 derived vtables) |  |  | high; names.toml |
| `0x824BF770` | `PanelAnimObject::vfunc_7` | rtti PanelAnimObject vtable 0x820714A4 slot 7 (also in 4 derived vtables) |  |  | high; names.toml |
| `0x824BF7B0` | `PanelAnimObject::vfunc_3` | rtti PanelAnimObject vtable 0x820714A4 slot 3 (also in 4 derived vtables) |  |  | high; names.toml |
| `0x824C3870` | `OptionsSoundMenu::vfunc_30` | rtti OptionsSoundMenu vtable 0x8207217C slot 30 |  |  | high; names.toml |
| `0x824C38D8` | `OptionsSoundMenu::vfunc_55` | rtti OptionsSoundMenu vtable 0x8207217C slot 55 |  |  | high; names.toml |
| `0x824C7440` | `PanelAnimObject::vfunc_9` | rtti PanelAnimObject vtable 0x820714A4 slot 9 (also in 4 derived vtables) |  |  | high; names.toml |
| `0x824C74E0` | `PanelAnimObject::vfunc_2` | rtti PanelAnimObject vtable 0x82070E04 slot 2 (also in 1 derived vtables) |  |  | high; names.toml |
| `0x824CCBB8` | `OptionsSoundMenu::Update` (likely) | vtable slot 16; meaning from Options &gt; Gameplay vtbl[16] Update(this, dt) | `(this, dt)` |  | medium |
| `0x824CCBC0` | `OptionsSoundMenu::vfunc_14` | rtti OptionsSoundMenu vtable 0x8207217C slot 14 |  |  | high; names.toml |
| `0x824D2DA0` | `OptionsSoundMenu::vfunc_2` | rtti OptionsSoundMenu vtable 0x8207217C slot 2 |  |  | high; names.toml |
| `0x824DB400` | `OptionsSoundMenu::Refresh` (likely) | vtable slot 21; meaning from Options &gt; Gameplay vtbl[21] Refresh(this) | `(this)` |  | medium |
| `0x824DB530` | `OptionsSoundMenu::dtor` | rtti: stores the OptionsSoundMenu vtable, then its base's (FEMenu) |  |  | high; names.toml |
| `0x824DB5C0` | `OptionsSoundMenu::ctor` | rtti: stores the OptionsSoundMenu vtable at this+0; called on the result of operator new |  |  | high; names.toml |
| `0x824E4880` | `OptionsSoundMenu::Init` (likely) | vtable slot 0; meaning from Options &gt; Gameplay vtbl[0] Init(this, panel) | `(this, panel)` |  | medium |
| `0x824F0D40` | `AnimBankManager::vfunc_1` | rtti AnimBankManager vtable 0x8206C9AC slot 1 |  |  | high; names.toml |
| `0x824F1AD8` | `AnimBankManager::ctor` | rtti: stores the AnimBankManager vtable at this+0; called on the result of operator new |  |  | high; names.toml |
| `0x824F3B70` | `AnimationUpdateTask::ctor` | rtti: stores the AnimationUpdateTask vtable at this+0; called on the result of operator new |  |  | high; names.toml |
| `0x824F3C08` | `XAnimUpdateTask::ctor` | rtti: stores the XAnimUpdateTask vtable at this+0; called on the result of operator new |  |  | high; names.toml |
| `0x82501608` | `AnimationUpdateTask::vfunc_1` | rtti AnimationUpdateTask vtable 0x8206C960 slot 1 |  |  | high; names.toml |
| `0x82519840` | `AnimationPlayer_nalAnimCallback::vfunc_0` | rtti AnimationPlayer::nalAnimCallback vtable 0x8206C96C slot 0 |  |  | high; names.toml |
| `0x8251E9F8` | `AnimationPlayer_nalPlayMethod::vfunc_2` | rtti AnimationPlayer::nalPlayMethod vtable 0x820693B8 slot 2 |  |  | high; names.toml |
| `0x82521C40` | `AnimationPlayer_nalPlayMethod::vfunc_0` | rtti AnimationPlayer::nalPlayMethod vtable 0x8206941C slot 0 (also in 1 derived vtables) |  |  | high; names.toml |
| `0x82522CB0` | `AnimHeap::vfunc_2` | rtti AnimHeap vtable 0x82068FB8 slot 2 |  |  | high; names.toml |
| `0x82522CB8` | `AnimHeap::vfunc_1` | rtti AnimHeap vtable 0x82068FB8 slot 1 |  |  | high; names.toml |
| `0x82522EF0` | `AbstractEffectParticle::vfunc_5` | rtti AbstractEffectParticle vtable 0x82068EF8 slot 5 |  |  | high; names.toml |
| `0x82522F10` | `AbstractEffectParticle::vfunc_2` | rtti AbstractEffectParticle vtable 0x82068EF8 slot 2 |  |  | high; names.toml |
| `0x82522F98` | `AbstractEffectParticle::vfunc_8` | rtti AbstractEffectParticle vtable 0x82068EF8 slot 8 |  |  | high; names.toml |
| `0x82522FA0` | `AbstractEffectParticle::vfunc_7` | rtti AbstractEffectParticle vtable 0x82068EF8 slot 7 |  |  | high; names.toml |
| `0x82522FE0` | `AbstractEffectSound::vfunc_7` | rtti AbstractEffectSound vtable 0x82068F28 slot 7 |  |  | high; names.toml |
| `0x82525750` | `AnimHeap::dtor` | rtti: stores the AnimHeap vtable, then its base's (nalHeap) |  |  | high; names.toml |
| `0x825257F8` | `AbstractEffectParticle::vfunc_9` | rtti AbstractEffectParticle vtable 0x82068EF8 slot 9 |  |  | high; names.toml |
| `0x82525810` | `AbstractEffectParticle::dtor` | rtti: stores the AbstractEffectParticle vtable, then its base's (AbstractEffect) |  |  | high; names.toml |
| `0x825258C0` | `AbstractEffectSound::vfunc_8` | rtti AbstractEffectSound vtable 0x82068F28 slot 8 |  |  | high; names.toml |
| `0x82525980` | `AbstractEffectSound::vfunc_10` | rtti AbstractEffectSound vtable 0x82068F28 slot 10 |  |  | high; names.toml |
| `0x8252A788` | `AbstractEffectShakeAndRumble::vfunc_4` | rtti AbstractEffectShakeAndRumble vtable 0x82068F88 slot 4 |  |  | high; names.toml |
| `0x8252A800` | `AbstractEffectShakeAndRumble::vfunc_2` | rtti AbstractEffectShakeAndRumble vtable 0x82068F88 slot 2 |  |  | high; names.toml |
| `0x8252A838` | `AbstractEffectShakeAndRumble::vfunc_6` | rtti AbstractEffectShakeAndRumble vtable 0x82068F88 slot 6 |  |  | high; names.toml |
| `0x8252A928` | `AbstractEffectShakeAndRumble::dtor` | rtti: stores the AbstractEffectShakeAndRumble vtable, then its base's (AbstractEffect) |  |  | high; names.toml |
| `0x8252A978` | `AbstractEffectSound::vfunc_4` | rtti AbstractEffectSound vtable 0x82068F28 slot 4 |  |  | high; names.toml |
| `0x8252A9F0` | `AbstractEffect::vfunc_6` | rtti AbstractEffect vtable 0x82068E3C slot 6 |  |  | high; names.toml |
| `0x8252CD90` | `AbstractEffectLight::vfunc_6` | rtti AbstractEffectLight vtable 0x82068F58 slot 6 |  |  | high; names.toml |
| `0x8252CED0` | `AbstractEffectLight::dtor` | rtti: stores the AbstractEffectLight vtable, then its base's (AbstractEffect) |  |  | high; names.toml |
| `0x8252CF90` | `AbstractEffectParticle::vfunc_6` | rtti AbstractEffectParticle vtable 0x82068EF8 slot 6 |  |  | high; names.toml |
| `0x8252D078` | `AbstractEffectSound::vfunc_2` | rtti AbstractEffectSound vtable 0x82068F28 slot 2 |  |  | high; names.toml |
| `0x8252D2A8` | `AbstractEffectSound::vfunc_6` | rtti AbstractEffectSound vtable 0x82068F28 slot 6 |  |  | high; names.toml |
| `0x8252D3C8` | `AbstractEffectSound::vfunc_9` | rtti AbstractEffectSound vtable 0x82068F28 slot 9 |  |  | high; names.toml |
| `0x8252D450` | `AbstractEffectSound::dtor` | rtti: stores the AbstractEffectSound vtable, then its base's (AbstractEffect) |  |  | high; names.toml |
| `0x8252EFE8` | `AbstractEffectShakeAndRumble::vfunc_1` | rtti AbstractEffectShakeAndRumble vtable 0x82068F88 slot 1 |  |  | high; names.toml |
| `0x8252F1F8` | `AbstractEffectShakeAndRumble::ctor` | rtti: stores the AbstractEffectShakeAndRumble vtable at this+0; called on the result of operator new |  |  | high; names.toml |
| `0x8252F3C8` | `AbstractEffectSound::vfunc_3` | rtti AbstractEffectSound vtable 0x82068F28 slot 3 |  |  | high; names.toml |
| `0x8252F4D8` | `AbstractEffectSound::vfunc_1` | rtti AbstractEffectSound vtable 0x82068F28 slot 1 |  |  | high; names.toml |
| `0x825305C0` | `AbstractEffectSound::ctor` | rtti: stores the AbstractEffectSound vtable at this+0; called on the result of operator new |  |  | high; names.toml |
| `0x82532E58` | `AbstractEffectLight::vfunc_1` | rtti AbstractEffectLight vtable 0x82068F58 slot 1 |  |  | high; names.toml |
| `0x82532F90` | `AbstractEffectLight::ctor` | rtti: stores the AbstractEffectLight vtable at this+0; called on the result of operator new |  |  | high; names.toml |
| `0x82533208` | `AbstractEffectParticle::vfunc_1` | rtti AbstractEffectParticle vtable 0x82068EF8 slot 1 |  |  | high; names.toml |
| `0x825338C8` | `AbstractEffectParticle::ctor` | rtti: stores the AbstractEffectParticle vtable at this+0; called on the result of operator new |  |  | high; names.toml |

### Level scripts (Broc, script threads) (98)

| Address | Name | What it does | Arguments | Port use | Sources |
|---|---|---|---|---|---|
| `0x820A0098` | `Broc_Abs` | level slot 0x174: fabs |  |  | high; names.toml |
| `0x820A00A0` | `Broc_Cos` | level slot 0x158: float wrapper of cos |  |  | high; names.toml |
| `0x820A00C8` | `Broc_Sin` | level slot 0x154: float wrapper of sin |  |  | high; names.toml |
| `0x820A05F8` | `Task::deleting_dtor` | vtable slot 0: (this, flags) with the flags & 1 delete test | `(this, flags)` |  | high; names.toml |
| `0x820A06B0` | `WaitTilOutput::deleting_dtor` | vtable slot 0: (this, flags) with the flags & 1 delete test | `(this, flags)` |  | high; names.toml |
| `0x820A41F0` | `WaitTilOutputInst2_int_Broc_entity::vfunc_1` | rtti WaitTilOutputInst2&lt;int,Broc::entity&gt; vtable 0x820678BC slot 1 |  |  | high; names.toml |
| `0x820A4330` | `TaskFunctor::deleting_dtor` | vtable slot 0: (this, flags) with the flags & 1 delete test | `(this, flags)` |  | high; names.toml |
| `0x820A47C0` | `WaitTilOutputInst1_Broc_string::vfunc_1` | rtti WaitTilOutputInst1&lt;Broc::string&gt; vtable 0x820678E0 slot 1 |  |  | high; names.toml |
| `0x820A47D0` | `WaitTilOutputInst2_Broc_string_Broc_string::vfunc_1` | rtti WaitTilOutputInst2&lt;Broc::string,Broc::string&gt; vtable 0x820678F0 slot 1 |  |  | high; names.toml |
| `0x820A4860` | `WaitTilOutputInst1_Broc_string::vfunc_0` | rtti WaitTilOutputInst1&lt;Broc::string&gt; vtable 0x820678E0 slot 0 |  |  | high; names.toml |
| `0x820A48C8` | `WaitTilOutputInst2_Broc_string_Broc_string::vfunc_0` | rtti WaitTilOutputInst2&lt;Broc::string,Broc::string&gt; vtable 0x820678F0 slot 0 |  |  | high; names.toml |
| `0x820B2150` | `Broc_Length` | level slot 0x18C: sqrt(x\*x + y\*y + z\*z) |  |  | high; names.toml |
| `0x820B2170` | `Broc_LengthSquared` | level slot 0x190: x\*x + y\*y + z\*z |  |  | high; names.toml |
| `0x820B2190` | `Broc_VectorDot` | level slot 0x198: dot product of two vectors |  |  | high; names.toml |
| `0x820B21B8` | `Broc_Distance` | level slot 0x184: sqrt of the squared difference of two vectors |  |  | high; names.toml |
| `0x820B21F0` | `Broc_DistanceSquared` | level slot 0x188: squared difference of two vectors |  |  | high; names.toml |
| `0x820B2228` | `Broc_Closer` | level slot 0x194: compares the squared distances from a to b and to c |  |  | high; names.toml |
| `0x820B2290` | `Broc_VectorNormalize` | level slot 0x19C: copies the vector and scales it by 1/length when non-zero |  |  | high; names.toml |
| `0x820B2458` | `AeThreadEntityNotifyState::vfunc_0` | rtti AeThreadEntityNotifyState vtable 0x82074124 slot 0 |  |  | high; names.toml |
| `0x820B24B0` | `AeThreadState::deleting_dtor` | vtable slot 0: (this, flags) with the flags & 1 delete test | `(this, flags)` |  | high; names.toml |
| `0x820B2508` | `Broc_RandomInt` | level slot 0x140: own LCG (x 214013 + 2531011) scaled to [0, n) |  |  | high; names.toml |
| `0x820B2538` | `Broc_RandomFloat` | level slot 0x144: the same LCG scaled to a float range |  |  | high; names.toml |
| `0x820B2588` | `Broc_RandomIntRange` | level slot 0x148: the LCG scaled to [min, max) |  |  | high; names.toml |
| `0x820B25E0` | `Broc_RandomFloatRange` | level slot 0x14C: the LCG scaled to a float [min, max) |  |  | high; names.toml |
| `0x820B29D8` | `BrocDtor_Broc_string::vfunc_0` | rtti BrocDtor&lt;Broc::string&gt; vtable 0x82074164 slot 0 |  |  | high; names.toml |
| `0x820B3150` | `WaitTilOutputInst2_float_uint::vfunc_1` | rtti WaitTilOutputInst2&lt;float,unsigned int&gt; vtable 0x8207416C slot 1 |  |  | high; names.toml |
| `0x823C0B98` | `ScriptThread_CallEntry` | script\_threads.cpp: calls a new thread's entry functor |  | hook script\_threads.cpp | known; names.toml; comments; ENGINE.md |
| `0x82497E48` | `Broc_GetTime` | level slot 0x42C: returns level.time (0x82A4E790 + 156) |  |  | high; names.toml |
| `0x824981F8` | `AeThreadWaitFramesState::vfunc_1` | rtti AeThreadWaitFramesState vtable 0x82074118 slot 1 |  |  | high; names.toml |
| `0x82498D50` | `BroFunc` (likely) | string 'BroFunc: %s, NOT FOUND' (only user) |  |  | medium |
| `0x82499120` | `Broc_MoveOverTime` | level slot 0xAB4: 'move time %g &lt;= 0' / '&gt; 60' |  |  | high; names.toml |
| `0x824991F8` | `Broc_ScaleOverTime` | level slot 0xAB0: 'scale time %g &lt;= 0' / '&gt; 60' |  |  | high; names.toml |
| `0x824995C0` | `Broc_Sqrt` | level slot 0x178: fsqrts |  |  | high; names.toml |
| `0x824999C0` | `AeThreadWaitState::vfunc_1` | rtti AeThreadWaitState vtable 0x8207410C slot 1 |  |  | high; names.toml |
| `0x82499A38` | `AeThreadPakNotifyState::vfunc_1` | rtti AeThreadPakNotifyState vtable 0x8207413C slot 1 |  |  | high; names.toml |
| `0x8249ABC8` | `Broc_StartPath` | level slot 0x9F0: "Can't start path on a vehicle that hasn't been attached" |  |  | high; names.toml |
| `0x8249E508` | `Broc_SetTeam` | level slot 0x750: "unknown team '%s', should be axis, allies, or neutral" |  |  | high; names.toml |
| `0x8249E8D0` | `Broc_ViewKick` | level slot 0x71C: 'viewkick: damage %g &lt; 0' |  |  | high; names.toml |
| `0x8249EE80` | `Broc_SetCursorHint` | level slot 0x660: 'HINT\_INHERIT', 'List of valid hint type strings' |  |  | high; names.toml |
| `0x8249F470` | `Broc_SetTurretTeam` | level slot 0x644: 'axis' / 'allies', "entity type '%s' is not a turret" |  |  | high; names.toml |
| `0x8249FCB0` | `Broc_Hide` | level slot 0x60C: sets flag 0x400 of the entity's +632 |  |  | high; names.toml |
| `0x8249FD00` | `Broc_Show` | level slot 0x608: clears flag 0x400 of the entity's +632 |  |  | high; names.toml |
| `0x8249FDE8` | `Broc_SetNormalHealth` | level slot 0x5F8: 'setNormalHealth must be greater than 0' |  |  | high; names.toml |
| `0x824A06E0` | `Broc_IsTurret` | level slot 0x12C: entity reference -&gt; turret pointer (+540) set |  |  | high; names.toml |
| `0x824A0738` | `Broc_IsPlayer` | level slot 0x128: entity reference -&gt; client pointer (+536) set |  |  | high; names.toml |
| `0x824A0790` | `Broc_IsAI` | level slot 0x124: entity reference -&gt; actor pointer (+532) set |  |  | high; names.toml |
| `0x824A07E8` | `Broc_IsVehicle` | level slot 0x120: entity reference -&gt; vehicle pointer (+528) set |  |  | high; names.toml |
| `0x824A08B0` | `Broc_IsAlive` | level slot 0x118: entity reference -&gt; actor (+532) alive flag (+784) or health (+776) &gt; 0 |  |  | high; names.toml |
| `0x824A1670` | `Broc_FadeOverTime` | level slot 0xAAC: 'fade time %g &lt;= 0' / '&gt; 60' |  |  | high; names.toml |
| `0x824A2A28` | `AeThreadEntityNotifyState::dtor` | rtti: stores the AeThreadEntityNotifyState vtable, then its base's (AeThreadState) |  |  | high; names.toml |
| `0x824A3040` | `ScriptThread_Destroy` | script\_threads.cpp: the thread object's destructor |  | hook script\_threads.cpp | known; names.toml; comments; ENGINE.md |
| `0x824A3FB8` | `Broc_IsStanceAllowed` | level slot 0x840: 'invalid stance in isStanceAllowed()' |  |  | high; names.toml |
| `0x824A4DB8` | `Broc_SetTenthsTimerUp` | level slot 0xA9C: 'setTenthsTimerUp' |  |  | high; names.toml |
| `0x824A4DD0` | `Broc_SetTenthsTimer` | level slot 0xA98: 'setTenthsTimer' |  |  | high; names.toml |
| `0x824A4DE8` | `Broc_SetTimerUp` | level slot 0xA94: 'setTimerUp' |  |  | high; names.toml |
| `0x824A4E00` | `Broc_SetTimer` | level slot 0xA90: 'setTimer' |  |  | high; names.toml |
| `0x824A4F38` | `Broc_MissionSuccess` | level slot 0x4A4: next level name, 'creditsdone' / 'gamefinished', g\_changelevel\_time |  |  | high; names.toml |
| `0x824A5268` | `Broc_Notify` | level slot 0x74 (7,081 level calls): (entity, event hash) queues a 20-byte event for waiting ... |  |  | high; names.toml; ENGINE.md |
| `0x824A5500` | `Broc_Thread` | level slot 0x3C (12,523 level calls): allocates a script thread object for a level function and ... |  |  | high; names.toml; ENGINE.md |
| `0x824A55E8` | `Broc_Wait` | level slot 0x4C: an AeThreadWaitState of f1 seconds then yields with longjmp |  |  | high; names.toml; ENGINE.md |
| `0x824A56B8` | `Broc_WaitFrames` | level slot 0x50: an AeThreadWaitFramesState then yields with longjmp |  |  | high; names.toml |
| `0x824A5A18` | `AeThreadEntityNotifyMatchState::vfunc_1` | rtti AeThreadEntityNotifyMatchState vtable 0x82074148 slot 1 |  |  | high; names.toml |
| `0x824A5B30` | `AeThreadEntityNotifyTimeoutState::vfunc_1` | rtti AeThreadEntityNotifyTimeoutState vtable 0x82074130 slot 1 |  |  | high; names.toml |
| `0x824A5C28` | `AeThreadEntityNotifyState::vfunc_1` | rtti AeThreadEntityNotifyState vtable 0x82074124 slot 1 |  |  | high; names.toml |
| `0x824A6150` | `ScriptThread_Run` | script\_threads.cpp: runs (starts or resumes) one script thread under setjmp |  | mid-asm cod3\_script\_resume default.toml; mid-asm cod3\_script\_resume\_prepare default.toml | known; names.toml; notes: multiplayer, proton-stall; comments; ENGINE.md |
| `0x824A66B8` | `Broc_Reverb` | level slot 0x9C0: client command 'reverb "%s" %g %g' |  |  | high; names.toml |
| `0x824A6718` | `Broc_Noclip` | level slot 0x9BC: toggles noclip, prints GAME\_NOCLIPON / GAME\_NOCLIPOFF |  |  | high; names.toml |
| `0x824A70C0` | `Broc_ConnectPaths` | level slot 0x618: "entity of type '%s' cannot connect paths" |  |  | high; names.toml |
| `0x824A77E8` | `Broc_GetEnt` | level slot 0x94: entity lookup by value and key 'classname' / 'targetname' / 'target' / 'groupname' |  |  | high; names.toml |
| `0x824A7C28` | `Broc_Earthquake` | level slot 0x4B4: (scale, duration, origin, radius) through the cgame camera shake (cg\_camerashake) |  |  | high; names.toml |
| `0x824A84E8` | `Broc_GetDvarFloat` | level slot 0x254: Cvar\_FindVar(name)-&gt;value (+28) |  |  | high; names.toml |
| `0x824A8530` | `Broc_GetDvarInt` | level slot 0x250: Cvar\_FindVar(name)-&gt;integer (+32) |  |  | high; names.toml |
| `0x824A9030` | `AeThreadEntityNotifyState::ctor` | rtti: stores the AeThreadEntityNotifyState vtable at this+0; called on the result of operator new |  |  | high; names.toml |
| `0x824A92A8` | `Broc_GetWheelSurface` | level slot 0xA14: 'Vehicle type [%s] has no wheels' |  |  | high; names.toml |
| `0x824A95F0` | `Broc_ObjectiveRing` | level slot 0x208: toggles the objective's 'ring' key |  |  | high; names.toml |
| `0x824A9760` | `Broc_ObjectivePosition` | level slot 0x1FC: Info\_SetValueForKey 'org' as '%i %i %i' |  |  | high; names.toml |
| `0x824AA2F0` | `Broc_SetHintString` | level slot 0x668: 'Too many different hintstring values' |  |  | high; names.toml |
| `0x824AA9C0` | `Broc_SetShader` | level slot 0xA8C: HUD element 'width %i &lt; 0' / 'height %i &lt; 0' |  |  | high; names.toml |
| `0x824AAD00` | `Broc_MissionFailed` | level slot 0x4A8: 'INGAME\_PLAYER\_DIED' message through Localize, g\_reloading |  |  | high; names.toml |
| `0x824AAE48` | `Broc_GetSpawnerTeamArray` | level slot 0x444: 'getspawnerteamarray' |  |  | high; names.toml |
| `0x824AB430` | `Broc_GetDvar` | level slot 0x24C: Cvar\_VariableStringBuffer into a script string |  |  | high; names.toml |
| `0x824AB478` | `Broc_Endon` | level slot 0x58: (entity, event hash) adds an AeThreadEntityNotifyState of type 3 to the running ... |  |  | high; names.toml; ENGINE.md |
| `0x824ABA90` | `Broc_ObjectiveDelete` | level slot 0x1E8: 'index %i is an illegal objective index', clears the objective configstring |  |  | high; names.toml |
| `0x824ABB60` | `Broc_PlayLocalSound` | level slot 0x9C8: "unknown sound alias '%s'", client command 'ls %i' |  |  | high; names.toml |
| `0x824AC338` | `Broc_ShellShock` | level slot 0x714: shock types 'death' / 'pain' / 'default' registered with G\_FindConfigstringIndex |  |  | high; names.toml |
| `0x824ACC20` | `Broc_SetClockUp` | level slot 0xAA4: 'setClockUp' |  |  | high; names.toml |
| `0x824ACC58` | `Broc_SetClock` | level slot 0xAA0: 'setClock' |  |  | high; names.toml |
| `0x824AD700` | `Broc_DisconnectPaths` | level slot 0x614: "entity of type '%s' cannot disconnect paths" |  |  | high; names.toml |
| `0x824AFFC8` | `Broc_BadPlace_Arc` | level slot 0x50C: 'badplace\_arc' |  |  | high; names.toml |
| `0x824B0148` | `Broc_BadPlace_Cylinder` | level slot 0x508: 'badplace\_cylinder' |  |  | high; names.toml |
| `0x824B29E8` | `Broc_UseAnimTree` | level slot 0x6A0: 'cannot change the animtree of classname' |  |  | high; names.toml |
| `0x824B4510` | `Broc_Spawn` | level slot 0x264: 'unable to spawn "%s" entity' |  |  | high; names.toml |
| `0x824B4A60` | `Broc_DropWeapon` | level slot 0x81C: "unknown weapon '%s' in dropWeapon", tag\_weapon\_left / right |  |  | high; names.toml |
| `0x824B53F8` | `Broc_ObjectiveCurrent` | level slot 0x204: reads every objective's 'state' and makes one current |  |  | high; names.toml |
| `0x824B5910` | `Broc_ObjectiveState` | level slot 0x1EC: Info\_SetValueForKey 'state' in the objective configstring |  |  | high; names.toml |
| `0x824B7C70` | `Broc_Teleport` | level slot 0x824: 'Teleport failed because player...', 'Player can see goal pos' |  |  | high; names.toml |
| `0x824B8948` | `Broc_FireWeapon` | level slot 0xA68: 'No weapon specified for [%s]', 'Vehicles only support bullet and projectile' |  |  | high; names.toml |
| `0x824B9428` | `Broc_MagicBullet` | level slot 0x4D4: 'MagicBullet called with unknown weapon name' |  |  | high; names.toml |

### World: scene, entities, AI paths, vegetation (34)

| Address | Name | What it does | Arguments | Port use | Sources |
|---|---|---|---|---|---|
| `0x820A7460` | `SplineMgr::deleting_dtor` | vtable slot 0: (this, flags) with the flags & 1 delete test | `(this, flags)` |  | high; names.toml |
| `0x820BEFC0` | `EntityManager::deleting_dtor` | vtable slot 0: (this, flags) with the flags & 1 delete test | `(this, flags)` |  | high; names.toml |
| `0x820C03C0` | `SceneManager::deleting_dtor` | vtable slot 0: (this, flags) with the flags & 1 delete test | `(this, flags)` |  | high; names.toml |
| `0x820C8350` | `SpeedTreeBranchesNode::vfunc_3` | rtti SpeedTreeBranchesNode vtable 0x8207B7E0 slot 3 |  |  | high; names.toml |
| `0x820C8368` | `SpeedTreeFrondsNode::vfunc_3` | rtti SpeedTreeFrondsNode vtable 0x8207B7F8 slot 3 |  |  | high; names.toml |
| `0x820C8380` | `SpeedTreeLeafsNode::vfunc_3` | rtti SpeedTreeLeafsNode vtable 0x8207B810 slot 3 |  |  | high; names.toml |
| `0x820C8398` | `CSpeedTreeAllocator::deleting_dtor` | vtable slot 0: (this, flags) with the flags & 1 delete test | `(this, flags)` |  | high; names.toml |
| `0x820C83E0` | `CMySpeedTreeAllocator::vfunc_1` | rtti CMySpeedTreeAllocator vtable 0x8207B828 slot 1 |  |  | high; names.toml |
| `0x820C8438` | `CMySpeedTreeAllocator::vfunc_2` | rtti CMySpeedTreeAllocator vtable 0x8207B828 slot 2 |  |  | high; names.toml |
| `0x820C8478` | `SpeedTreeWrapper::vfunc_0` | rtti SpeedTreeWrapper vtable 0x8207B7D8 slot 0 |  |  | high; names.toml |
| `0x820C84F0` | `SpeedTreeWrapperNode::vfunc_2` | rtti SpeedTreeWrapperNode vtable 0x8207B838 slot 2 |  |  | high; names.toml |
| `0x820C8508` | `SpeedTreeWrapperNode::vfunc_3` | rtti SpeedTreeWrapperNode vtable 0x8207B838 slot 3 |  |  | high; names.toml |
| `0x820DD048` | `nglRenderNode_Vtbl::deleting_dtor` | vtable slot 0: (this, flags) with the flags & 1 delete test | `(this, flags)` |  | high; names.toml |
| `0x823C5CB0` | `PathNodeMgr::vfunc_1` | rtti PathNodeMgr vtable 0x8207C600 slot 1 |  |  | high; names.toml |
| `0x823C5CD0` | `PathNodeMgr::ctor` | rtti: stores the PathNodeMgr vtable at this+0; called on the result of operator new |  |  | high; names.toml |
| `0x823EE250` | `GrassRenderNode::vfunc_3` | rtti GrassRenderNode vtable 0x8207B798 slot 3 |  |  | high; names.toml |
| `0x823F2320` | `SpeedTreeWrapper::ctor` | rtti: stores the SpeedTreeWrapper vtable at this+0; called on the result of operator new |  |  | high; names.toml |
| `0x823F6478` | `SpeedTreeLeafsNode::vfunc_2` | rtti SpeedTreeLeafsNode vtable 0x8207B810 slot 2 |  |  | high; names.toml |
| `0x823F6480` | `SpeedTreeFrondsNode::vfunc_2` | rtti SpeedTreeFrondsNode vtable 0x8207B7F8 slot 2 |  |  | high; names.toml |
| `0x823F6488` | `SpeedTreeBranchesNode::vfunc_2` | rtti SpeedTreeBranchesNode vtable 0x8207B7E0 slot 2 |  |  | high; names.toml |
| `0x823F79A0` | `GrassVisQueryNode::vfunc_2` | rtti GrassVisQueryNode vtable 0x8207B7B0 slot 2 |  |  | high; names.toml |
| `0x823F79B0` | `GrassRenderNode::vfunc_2` | rtti GrassRenderNode vtable 0x8207B798 slot 2 |  |  | high; names.toml |
| `0x82417DF0` | `LightGridMgr::vfunc_1` | rtti LightGridMgr vtable 0x8207A438 slot 1 |  |  | high; names.toml |
| `0x8241E608` | `LightGridMgr::ctor` | rtti: stores the LightGridMgr vtable at this+0; called on the result of operator new |  |  | high; names.toml |
| `0x82457DD0` | `SceneManager::ctor` | rtti: stores the SceneManager vtable at this+0; called on the result of operator new |  |  | high; names.toml |
| `0x8245E1A0` | `SceneManager::vfunc_1` | rtti SceneManager vtable 0x82078454 slot 1 |  |  | high; names.toml |
| `0x82467DF8` | `EntityManager::ctor` (likely) | rtti: stores the EntityManager vtable at this+0; stores base then own vtables and returns this |  |  | medium |
| `0x824720F8` | `EntityManager::vfunc_1` | rtti EntityManager vtable 0x82077570 slot 1 |  |  | high; names.toml |
| `0x8251A208` | `DroneSplineTask::ctor` (likely) | rtti: stores the DroneSplineTask vtable at this+0; stores base then own vtables and returns this |  |  | medium |
| `0x8251C118` | `SplineMgr::ctor` | rtti: stores the SplineMgr vtable at this+0; called on the result of operator new |  |  | high; names.toml |
| `0x8251E888` | `SplineMgr::vfunc_1` | rtti SplineMgr vtable 0x820693AC slot 1 |  |  | high; names.toml |
| `0x8251EC50` | `DroneSplineTask::vfunc_1` | rtti DroneSplineTask vtable 0x820693D0 slot 1 |  |  | high; names.toml |
| `0x82539D08` | `EntityDeathTask::ctor` (likely) | rtti: stores the EntityDeathTask vtable at this+0; stores base then own vtables and returns this |  |  | medium |
| `0x82540AE0` | `EntityDeathTask::vfunc_1` | rtti EntityDeathTask vtable 0x820678CC slot 1 |  |  | high; names.toml |

### System: kernel and XAM imports, C runtime, jobs (279)

| Address | Name | What it does | Arguments | Port use | Sources |
|---|---|---|---|---|---|
| `0x820A4948` | `strstr_thunk` (likely) | a lone 'b strstr' |  |  | medium |
| `0x820A4978` | `operator_new_thunk` (likely) | a lone 'b operator\_new' |  |  | medium |
| `0x820CC1D0` | `operator_new` | Game heap memory | `void* (size_t size)` | game\_api; typed call game\_api.cpp; calls menu\_fov.cpp, menu\_video.cpp, pc\_menu.cpp | high; names.toml; addresses.h; game\_api.h; comments |
| `0x820CC468` | `operator_delete` | (pointer) | `void (void* memory)` | game\_api; typed call game\_api.cpp; calls menu\_fov.cpp, menu\_video.cpp, pc\_menu.cpp | high; names.toml; addresses.h; game\_api.h; comments |
| `0x820CDD60` | `fopen_thunk` (likely) | a lone 'b fopen' |  |  | medium |
| `0x8212A100` | `jobs::kWake` | PulseEvent(kWakeEvent); hooked |  | hook job\_wake.cpp | addresses.h |
| `0x82144CA8` | `nullsub` | an empty method (Options' OnLeft) |  | refers to pc\_menu.cpp | high; names.toml; addresses.h |
| `0x82160648` | `CloseHandle_thunk` (likely) | a lone 'b CloseHandle' |  |  | medium |
| `0x82196498` | `NetDll_WSACleanup_wrapper` (likely) | 2-instruction wrapper around the import NetDll\_WSACleanup |  |  | medium |
| `0x823438D8` | `CloseHandle` (likely) | XAPI wrapper: only calls NtClose and BaseSetLastNTError |  |  | medium |
| `0x82343910` | `DbgBreakPoint_thunk` (likely) | a lone 'b \_\_imp\_\_DbgBreakPoint' |  |  | medium; comments |
| `0x82343928` | `XLaunchNewImage` | XLaunchNewImage(path, flags) -&gt; XamLoaderLaunchTitle | `(path, flags)` |  | addresses.h; notes: multiplayer-exit; comments |
| `0x82343A70` | `XamShowSigninUI_thunk` (likely) | a lone 'b \_\_imp\_\_XamShowSigninUI' |  |  | medium |
| `0x82343A78` | `XamShowMarketplaceUI_wrapper` (likely) | 4-instruction wrapper around the import XamShowMarketplaceUI |  |  | medium |
| `0x82343A88` | `XamShowDeviceSelectorUI_thunk` (likely) | a lone 'b \_\_imp\_\_XamShowDeviceSelectorUI' |  |  | medium |
| `0x82343A90` | `XamShowMessageBoxUI_wrapper` (likely) | 10-instruction wrapper around the import XamShowMessageBoxUI |  |  | medium |
| `0x82343BF0` | `XUserSetContext` (likely) |  |  |  | notes: proton-stall |
| `0x82343BF8` | `XamUserGetName_thunk` (likely) | a lone 'b \_\_imp\_\_XamUserGetName' |  |  | medium |
| `0x82343C00` | `XamUserGetSigninState_thunk` (likely) | a lone 'b \_\_imp\_\_XamUserGetSigninState' |  |  | medium |
| `0x82343D10` | `XamWriteGamerTile_wrapper` (likely) | 6-instruction wrapper around the import XamWriteGamerTile |  |  | medium |
| `0x82344340` | `XamContentDelete_thunk` (likely) | a lone 'b \_\_imp\_\_XamContentDelete' |  |  | medium |
| `0x82344348` | `XamContentClose_thunk` (likely) | a lone 'b \_\_imp\_\_XamContentClose' |  |  | medium |
| `0x82344350` | `XamContentGetCreator_thunk` (likely) | a lone 'b \_\_imp\_\_XamContentGetCreator' |  |  | medium |
| `0x82344358` | `XamContentCreateEnumerator_thunk` (likely) | a lone 'b \_\_imp\_\_XamContentCreateEnumerator' |  |  | medium |
| `0x82344950` | `XamEnumerate_wrapper` (likely) | 6-instruction wrapper around the import XamEnumerate |  |  | medium |
| `0x823449C0` | `XamNotifyCreateListener_wrapper` (likely) | 2-instruction wrapper around the import XamNotifyCreateListener |  |  | medium |
| `0x82344F70` | `MmQueryAllocationSize_thunk` (likely) | a lone 'b \_\_imp\_\_MmQueryAllocationSize' |  |  | medium |
| `0x82344F78` | `MmFreePhysicalMemory_wrapper` (likely) | 3-instruction wrapper around the import MmFreePhysicalMemory |  |  | medium |
| `0x82344F88` | `MmQueryAddressProtect_thunk` (likely) | a lone 'b \_\_imp\_\_MmQueryAddressProtect' |  |  | medium |
| `0x82344F90` | `XamInputGetCapabilities_thunk` (likely) | a lone 'b \_\_imp\_\_XamInputGetCapabilities' |  |  | medium |
| `0x82344F98` | `XamInputGetState_wrapper` (likely) | XInputGetState(user, X\_INPUT\_STATE\*) wrapper the game reads its pads with; hooked (only watched) | `(user, X_INPUT_STATE*)` | hook button\_prompts.cpp | medium; addresses.h |
| `0x82344FA8` | `XamInputSetState_wrapper` (likely) | 6-instruction wrapper around the import XamInputSetState |  |  | medium |
| `0x82344FC0` | `ReleaseMutex` (likely) | XAPI wrapper: only calls NtReleaseMutant and BaseSetLastNTError |  |  | medium |
| `0x82345000` | `SetEvent` (likely) | XAPI wrapper: only calls NtSetEvent and BaseSetLastNTError |  | calls job\_wake.cpp | medium |
| `0x82345708` | `QueryPerformanceFrequency` (likely) | XAPI wrapper: only calls KeQueryPerformanceFrequency and BaseSetLastNTError |  |  | medium |
| `0x82345740` | `QueryPerformanceCounter` [QueryPerformanceCounter\_guest] | XAPI: reads the time base (mftb) into \*arg and returns TRUE |  |  | high; names.toml |
| `0x82345820` | `ResumeThread` (likely) | XAPI wrapper: only calls NtResumeThread and BaseSetLastNTError |  |  | medium |
| `0x82345860` | `ResetEvent` (likely) | XAPI wrapper: only calls NtClearEvent and BaseSetLastNTError |  |  | medium |
| `0x82345A58` | `BaseSetLastNTError` [BaseSetLastNTError\_guest] | only calls RtlNtStatusToDosError and stores the result as the thread's last error; called by the ... |  |  | high; names.toml |
| `0x823497D8` | `BaseSetLastNTError_thunk` (likely) | a lone 'b BaseSetLastNTError' |  |  | medium |
| `0x8234B180` | `RtlInitializeCriticalSection_thunk` (likely) | a lone 'b \_\_imp\_\_RtlInitializeCriticalSection' |  |  | medium |
| `0x8234B188` | `RtlEnterCriticalSection_thunk` (likely) | a lone 'b \_\_imp\_\_RtlEnterCriticalSection' |  |  | medium |
| `0x8234B190` | `RtlLeaveCriticalSection_thunk` (likely) | a lone 'b \_\_imp\_\_RtlLeaveCriticalSection' |  |  | medium |
| `0x8234B250` | `sprintf` [sprintf\_guest] | (buf, fmt, ...) with '%d' / '%f' formats; CRT formatter underneath |  |  | high; names.toml |
| `0x8234B400` | `vsprintf` (likely) | (buf, fmt, args); used by tlFatalErrorf and CL\_RefPrintf |  |  | medium |
| `0x8234B4D8` | `sscanf` [sscanf\_guest] | G\_SpawnVector's '%f %f %f' parse; CRT input formatter underneath |  |  | high; names.toml |
| `0x8234B538` | `sin` [sin\_guest] | range reduction by pi (constants pi/2, 1/pi, pi split) with an odd sign |  |  | high; names.toml |
| `0x8234B610` | `cos` [cos\_guest] | the same reduction applied to \|x\| + pi/2 |  |  | high; names.toml |
| `0x8234B6F0` | `strncmp` [strncmp\_guest] | counted byte compare (ctr = n) stopping at a difference or NUL; G\_CallSpawn compares class names ... |  |  | high; names.toml |
| `0x8234BB48` | `strstr` [strstr\_guest] | substring search loop; FS\_CreatePath checks '..' and '::' with it (Q3) |  |  | high; names.toml |
| `0x8234BBC8` | `atoi` [atoi\_guest] | strtol(s, NULL, 10) as a tail call |  |  | high; names.toml |
| `0x8234BFC0` | `tolower` [tolower\_guest] | c - 'A' &lt;= 25 ? c + 32 : c |  |  | high; names.toml |
| `0x8234BFD8` | `isalpha` [isalpha\_guest] | ctype table lookup masked with 0x103 (\_ALPHA) |  |  | high; names.toml |
| `0x8234C078` | `srand` [srand\_guest] | stores the seed where rand reads it; SV\_SpawnServer seeds it with the time (Q3 ... |  |  | high; names.toml |
| `0x8234C0A8` | `rand` [rand\_guest] | per-thread seed \* 214013 + 2531011, returns (seed &gt;&gt; 16) & 0x7FFF (MSVC CRT) |  |  | high; names.toml |
| `0x8234C240` | `atof` (likely) | tail call of the string-to-double converter with a NULL second argument |  |  | medium |
| `0x8234C2A0` | `strchr` [strchr\_guest] | byte search loop returning NULL at the terminator; Info\_\* key checks (Q3) |  |  | high; names.toml |
| `0x8234C5D8` | `fflush` [fflush\_guest] | FS\_Write flushes after fwrite (Q3 handleSync); uses \_\_iob\_func |  |  | high; names.toml |
| `0x8234C7F0` | `strncpy` [strncpy\_guest] | copies up to n bytes and pads with zeros |  |  | high; names.toml |
| `0x8234C850` | `fmod` [fmod\_guest] | double remainder with the NaN / infinity checks; AngleSubtract calls it with 360.0 |  |  | high; names.toml |
| `0x8234C9E0` | `_fsopen` [\_fsopen\_guest] | fopen's tail call with the share flag |  |  | high; names.toml |
| `0x8234CB70` | `fopen` [fopen\_guest] | li r5,0x40 (\_SH\_DENYNO) then b \_fsopen; FS\_CopyFile's call order fopen, fseek, ftell, fseek ... |  |  | high; names.toml |
| `0x8234CD80` | `fread_s` [fread\_s\_guest] | fread's tail call target with buffer size -1 |  |  | high; names.toml |
| `0x8234CF00` | `fread` [fread\_guest] | fread\_s(buf, SIZE\_MAX, size, count, stream) tail call; FS\_Read / FS\_CopyFile (Q3 fread) |  |  | high; names.toml |
| `0x8234D2F0` | `fwrite` [fwrite\_guest] | locked wrapper; FS\_Write and FS\_CopyFile write with it (Q3) |  |  | high; names.toml |
| `0x8234D410` | `toupper` [toupper\_guest] | c - 'a' &lt;= 25 ? c - 32 : c |  |  | high; names.toml |
| `0x8234D660` | `ftell` [ftell\_guest] | locked wrapper; FS\_CopyFile's length query between the two fseeks (Q3) |  |  | high; names.toml |
| `0x8234D838` | `fseek` [fseek\_guest] | locked wrapper; FS\_CopyFile seeks to the end and back around ftell (Q3) |  |  | high; names.toml |
| `0x8234DA08` | `fclose` [fclose\_guest] | locked wrapper; FS\_CopyFile closes both files with it (Q3) |  |  | high; names.toml |
| `0x8234E220` | `memmove` [memmove\_guest] | Cbuf\_Execute shifts the remaining command text down with it (Q3) |  |  | high; names.toml |
| `0x8234E2E8` | `printf` [printf\_guest] | stdout (\_\_iob\_func) formatted output; CL\_Shutdown's 'recursive shutdown' print (Q3) |  |  | high; names.toml |
| `0x8234EB30` | `strcpy` [strcpy\_guest] | byte copy loop up to and including the NUL; level interface slot 0x20 |  |  | high; names.toml |
| `0x8234EBA0` | `memset` [memset\_guest] | byte/word fill loop (dst, c, n); 724 callers |  |  | high; names.toml |
| `0x8234EEC0` | `longjmp` [longjmp\_guest] | config/default.toml / script\_threads.cpp |  | hook script\_threads.cpp | known; names.toml; comments |
| `0x8234F0D0` | `_vsnprintf` (likely) | va formats with (buf, 2048, fmt, args) |  |  | medium |
| `0x8234F460` | `memcpy` [memcpy\_guest] | (dst, src, n): copies CL\_InitRef's refimport\_t and the refexport\_t |  |  | high; names.toml; ENGINE.md |
| `0x82350390` | `__iob_func` (likely) | returns the stdio FILE array; +64 (stderr) is printed to |  |  | medium |
| `0x82350EA0` | `strtol` (likely) | atoi's target (s, NULL, 10) |  |  | medium |
| `0x82351858` | `abs` [abs\_guest] | cmpwi r3,0; bgelr; neg r3,r3 |  |  | high; names.toml |
| `0x82351CC0` | `setjmp` [setjmp\_guest] | config/default.toml setjmp\_address |  | setjmp\_address default.toml | known; names.toml |
| `0x823523E8` | `KeBugCheck_wrapper` (likely) | 2-instruction wrapper around the import KeBugCheck |  |  | medium |
| `0x82352688` | `KeTlsAlloc_thunk` (likely) | a lone 'b \_\_imp\_\_KeTlsAlloc' |  |  | medium |
| `0x823536D0` | `_stricmp` [\_stricmp\_guest] | byte compare folding A-Z to lower case |  |  | high; names.toml |
| `0x823582B0` | `RtlUnwind_wrapper` (likely) | 10-instruction wrapper around the import RtlUnwind |  |  | medium |
| `0x8235B540` | `RtlInitializeCriticalSection_wrapper` (likely) | 9-instruction wrapper around the import RtlInitializeCriticalSection |  |  | medium |
| `0x823C0280` | `KeQuerySystemTime_thunk` (likely) | a lone 'b \_\_imp\_\_KeQuerySystemTime' |  |  | medium |
| `0x82432AB0` | `Z_Malloc_thunk` (likely) | a lone 'b Z\_Malloc' |  |  | medium |
| `0x8244BF88` | `CG_PredictPlayerState_thunk` (likely) | a lone 'b CG\_PredictPlayerState' |  |  | medium |
| `0x82451690` | `Q_stricmpn_thunk` (likely) | a lone 'b Q\_stricmpn' |  |  | medium |
| `0x824BB4D0` | `G_LoadLevelDll_thunk` (likely) | a lone 'b G\_LoadLevelDll' |  |  | medium |
| `0x825098B0` | `Key_KeynumToString` | Q3 Key\_KeynumToString: 2 strings in common, e.g. '&lt;KEY NOT FOUND&gt;' |  |  | high; names.toml |
| `0x8250A960` | `Netchan_Process_thunk` (likely) | a lone 'b Netchan\_Process' |  |  | medium |
| `0x8250ACB8` | `Key_StringToKeynum` | Key\_Bind\_f and Key\_Unbind\_f look up argv 1 with it, '"%s" isn't a valid key' on failure (Q3) |  |  | high; names.toml |
| `0x8250C4F8` | `Key_Bindlist_f` | cmd 'bindlist' (Cmd\_AddCommand from 0x8250DEE0); Q3 handler name |  |  | high; names.toml |
| `0x8250CC80` | `Cmd_AddCommand_thunk` (likely) | a lone 'b Cmd\_AddCommand' |  |  | medium |
| `0x8250CC88` | `Key_SetBinding` | the last call of Key\_Bind\_f and Key\_Unbind\_f; frees the old binding and CopyString's the new one ... |  |  | high; names.toml |
| `0x8250D068` | `Key_Bind_f` | cmd 'bind' (Cmd\_AddCommand from 0x8250DEE0); Q3 handler name |  |  | high; names.toml |
| `0x8250D288` | `Key_Unbindall_f` | cmd 'unbindall' (Cmd\_AddCommand from 0x8250DEE0); Q3 handler name |  |  | high; names.toml |
| `0x8250D350` | `Key_Unbind_f` | cmd 'unbind' (Cmd\_AddCommand from 0x8250DEE0); Q3 handler name; also Q3 Key\_Unbind\_f: 1 strings in ... |  |  | high; names.toml |
| `0x82541148` | `SV_LinkEntity_thunk` (likely) | a lone 'b SV\_LinkEntity' |  |  | medium |
| `0x8257835C` | `__imp__RtlEnterCriticalSection` | import thunk xboxkrnl.exe!RtlEnterCriticalSection (codegen's name) |  |  | known |
| `0x8257836C` | `__imp__RtlLeaveCriticalSection` | import thunk xboxkrnl.exe!RtlLeaveCriticalSection (codegen's name) |  |  | known |
| `0x8257837C` | `__imp__RtlInitializeCriticalSection` | import thunk xboxkrnl.exe!RtlInitializeCriticalSection (codegen's name) |  |  | known |
| `0x8257838C` | `__imp__ObDereferenceObject` | import thunk xboxkrnl.exe!ObDereferenceObject (codegen's name) |  |  | known |
| `0x8257839C` | `__imp__DbgPrint` | import thunk xboxkrnl.exe!DbgPrint (codegen's name) |  |  | known |
| `0x825783AC` | `__imp__XexGetProcedureAddress` | import thunk xboxkrnl.exe!XexGetProcedureAddress (codegen's name) |  |  | known |
| `0x825783BC` | `__imp__XexGetModuleHandle` | import thunk xboxkrnl.exe!XexGetModuleHandle (codegen's name) |  |  | known |
| `0x825783CC` | `__imp__KeGetCurrentProcessType` | import thunk xboxkrnl.exe!KeGetCurrentProcessType (codegen's name) |  |  | known |
| `0x825783DC` | `__imp__XexCheckExecutablePrivilege` | import thunk xboxkrnl.exe!XexCheckExecutablePrivilege (codegen's name) |  |  | known |
| `0x825783EC` | `__imp____C_specific_handler` | import thunk xboxkrnl.exe!\_\_C\_specific\_handler (codegen's name) |  |  | known |
| `0x825783FC` | `__imp__RtlTryEnterCriticalSection` | import thunk xboxkrnl.exe!RtlTryEnterCriticalSection (codegen's name) |  |  | known |
| `0x8257840C` | `__imp__KeWaitForSingleObject` | import thunk xboxkrnl.exe!KeWaitForSingleObject (codegen's name) |  |  | known |
| `0x8257841C` | `__imp__KeSetEvent` | import thunk xboxkrnl.exe!KeSetEvent (codegen's name) |  |  | known |
| `0x8257842C` | `__imp__KeResetEvent` | import thunk xboxkrnl.exe!KeResetEvent (codegen's name) |  |  | known |
| `0x8257843C` | `__imp__RtlNtStatusToDosError` | import thunk xboxkrnl.exe!RtlNtStatusToDosError (codegen's name) |  |  | known |
| `0x8257844C` | `__imp__ExRegisterTitleTerminateNotification` | import thunk xboxkrnl.exe!ExRegisterTitleTerminateNotification (codegen's name) |  |  | known |
| `0x8257845C` | `__imp__KeAcquireSpinLockAtRaisedIrql` | import thunk xboxkrnl.exe!KeAcquireSpinLockAtRaisedIrql (codegen's name) |  |  | known |
| `0x8257846C` | `__imp__KeRaiseIrqlToDpcLevel` | import thunk xboxkrnl.exe!KeRaiseIrqlToDpcLevel (codegen's name) |  |  | known |
| `0x8257847C` | `__imp__KfLowerIrql` | import thunk xboxkrnl.exe!KfLowerIrql (codegen's name) |  |  | known |
| `0x8257848C` | `__imp__KeReleaseSpinLockFromRaisedIrql` | import thunk xboxkrnl.exe!KeReleaseSpinLockFromRaisedIrql (codegen's name) |  |  | known |
| `0x8257849C` | `__imp__KeReleaseSemaphore` | import thunk xboxkrnl.exe!KeReleaseSemaphore (codegen's name) |  |  | known |
| `0x825784AC` | `__imp__XAudioGetVoiceCategoryVolume` | import thunk xboxkrnl.exe!XAudioGetVoiceCategoryVolume (codegen's name) |  |  | known |
| `0x825784BC` | `__imp__XAudioGetVoiceCategoryVolumeChangeMask` | import thunk xboxkrnl.exe!XAudioGetVoiceCategoryVolumeChangeMask (codegen's name) |  |  | known |
| `0x825784CC` | `__imp__KeWaitForMultipleObjects` | import thunk xboxkrnl.exe!KeWaitForMultipleObjects (codegen's name) |  |  | known |
| `0x825784DC` | `__imp__KeResumeThread` | import thunk xboxkrnl.exe!KeResumeThread (codegen's name) |  |  | known |
| `0x825784EC` | `__imp__KeSetBasePriorityThread` | import thunk xboxkrnl.exe!KeSetBasePriorityThread (codegen's name) |  |  | known |
| `0x825784FC` | `__imp__ObReferenceObjectByHandle` | import thunk xboxkrnl.exe!ObReferenceObjectByHandle (codegen's name) |  |  | known |
| `0x8257850C` | `__imp__ExCreateThread` | import thunk xboxkrnl.exe!ExCreateThread (codegen's name) |  |  | known |
| `0x8257851C` | `__imp__KeInitializeSemaphore` | import thunk xboxkrnl.exe!KeInitializeSemaphore (codegen's name) |  |  | known |
| `0x8257852C` | `__imp__MmGetPhysicalAddress` | import thunk xboxkrnl.exe!MmGetPhysicalAddress (codegen's name) |  |  | known |
| `0x8257853C` | `__imp__XMAReleaseContext` | import thunk xboxkrnl.exe!XMAReleaseContext (codegen's name) |  |  | known |
| `0x8257854C` | `__imp__XMACreateContext` | import thunk xboxkrnl.exe!XMACreateContext (codegen's name) |  |  | known |
| `0x8257855C` | `__imp__KeQueryPerformanceFrequency` | import thunk xboxkrnl.exe!KeQueryPerformanceFrequency (codegen's name) |  |  | known |
| `0x8257856C` | `__imp__MmFreePhysicalMemory` | import thunk xboxkrnl.exe!MmFreePhysicalMemory (codegen's name) |  |  | known |
| `0x8257857C` | `__imp__XAudioRegisterRenderDriverClient` | import thunk xboxkrnl.exe!XAudioRegisterRenderDriverClient (codegen's name) |  |  | known |
| `0x8257858C` | `__imp__XAudioUnregisterRenderDriverClient` | import thunk xboxkrnl.exe!XAudioUnregisterRenderDriverClient (codegen's name) |  |  | known |
| `0x8257859C` | `__imp__XAudioSubmitRenderDriverFrame` | import thunk xboxkrnl.exe!XAudioSubmitRenderDriverFrame (codegen's name) |  |  | known |
| `0x825785AC` | `__imp__VdPersistDisplay` | import thunk xboxkrnl.exe!VdPersistDisplay (codegen's name) |  |  | known |
| `0x825785BC` | `__imp__MmAllocatePhysicalMemoryEx` | import thunk xboxkrnl.exe!MmAllocatePhysicalMemoryEx (codegen's name) |  |  | known |
| `0x825785CC` | `__imp__VdEnableDisableClockGating` | import thunk xboxkrnl.exe!VdEnableDisableClockGating (codegen's name) |  |  | known |
| `0x825785DC` | `__imp__KfReleaseSpinLock` | import thunk xboxkrnl.exe!KfReleaseSpinLock (codegen's name) |  |  | known |
| `0x825785EC` | `__imp__KfAcquireSpinLock` | import thunk xboxkrnl.exe!KfAcquireSpinLock (codegen's name) |  |  | known |
| `0x825785FC` | `__imp__KiApcNormalRoutineNop` | import thunk xboxkrnl.exe!KiApcNormalRoutineNop (codegen's name) |  |  | known |
| `0x8257860C` | `__imp__VdEnableRingBufferRPtrWriteBack` | import thunk xboxkrnl.exe!VdEnableRingBufferRPtrWriteBack (codegen's name) |  |  | known |
| `0x8257861C` | `__imp__VdInitializeRingBuffer` | import thunk xboxkrnl.exe!VdInitializeRingBuffer (codegen's name) |  |  | known |
| `0x8257862C` | `__imp__VdSetSystemCommandBufferGpuIdentifierAddress` | import thunk xboxkrnl.exe!VdSetSystemCommandBufferGpuIdentifierAddress (codegen's name) |  |  | known |
| `0x8257863C` | `__imp__VdSwap` | import thunk xboxkrnl.exe!VdSwap (codegen's name) |  |  | known |
| `0x8257864C` | `__imp__VdGetSystemCommandBuffer` | import thunk xboxkrnl.exe!VdGetSystemCommandBuffer (codegen's name) |  |  | known |
| `0x8257865C` | `__imp__sprintf` | import thunk xboxkrnl.exe!sprintf (codegen's name) |  |  | known |
| `0x8257866C` | `__imp__VdGetCurrentDisplayGamma` | import thunk xboxkrnl.exe!VdGetCurrentDisplayGamma (codegen's name) |  |  | known |
| `0x8257867C` | `__imp___vsnprintf` | import thunk xboxkrnl.exe!\_vsnprintf (codegen's name) |  |  | known |
| `0x8257868C` | `__imp__VdShutdownEngines` | import thunk xboxkrnl.exe!VdShutdownEngines (codegen's name) |  |  | known |
| `0x8257869C` | `__imp__ExGetXConfigSetting` | import thunk xboxkrnl.exe!ExGetXConfigSetting (codegen's name) |  |  | known |
| `0x825786AC` | `__imp__VdQueryVideoMode` | import thunk xboxkrnl.exe!VdQueryVideoMode (codegen's name) |  |  | known |
| `0x825786BC` | `__imp__VdGetCurrentDisplayInformation` | import thunk xboxkrnl.exe!VdGetCurrentDisplayInformation (codegen's name) |  |  | known |
| `0x825786CC` | `__imp__VdSetDisplayMode` | import thunk xboxkrnl.exe!VdSetDisplayMode (codegen's name) |  |  | known |
| `0x825786DC` | `__imp__VdSetGraphicsInterruptCallback` | import thunk xboxkrnl.exe!VdSetGraphicsInterruptCallback (codegen's name) |  |  | known |
| `0x825786EC` | `__imp__VdInitializeEngines` | import thunk xboxkrnl.exe!VdInitializeEngines (codegen's name) |  |  | known |
| `0x825786FC` | `__imp__VdIsHSIOTrainingSucceeded` | import thunk xboxkrnl.exe!VdIsHSIOTrainingSucceeded (codegen's name) |  |  | known |
| `0x8257870C` | `__imp__KeLeaveCriticalRegion` | import thunk xboxkrnl.exe!KeLeaveCriticalRegion (codegen's name) |  |  | known |
| `0x8257871C` | `__imp__VdRetrainEDRAM` | import thunk xboxkrnl.exe!VdRetrainEDRAM (codegen's name) |  |  | known |
| `0x8257872C` | `__imp__VdRetrainEDRAMWorker` | import thunk xboxkrnl.exe!VdRetrainEDRAMWorker (codegen's name) |  |  | known |
| `0x8257873C` | `__imp__KeEnterCriticalRegion` | import thunk xboxkrnl.exe!KeEnterCriticalRegion (codegen's name) |  |  | known |
| `0x8257874C` | `__imp__VdQueryVideoFlags` | import thunk xboxkrnl.exe!VdQueryVideoFlags (codegen's name) |  |  | known |
| `0x8257875C` | `__imp__VdCallGraphicsNotificationRoutines` | import thunk xboxkrnl.exe!VdCallGraphicsNotificationRoutines (codegen's name) |  |  | known |
| `0x8257876C` | `__imp__VdInitializeScalerCommandBuffer` | import thunk xboxkrnl.exe!VdInitializeScalerCommandBuffer (codegen's name) |  |  | known |
| `0x8257877C` | `__imp__RtlFillMemoryUlong` | import thunk xboxkrnl.exe!RtlFillMemoryUlong (codegen's name) |  |  | known |
| `0x8257878C` | `__imp__ObDeleteSymbolicLink` | import thunk xboxkrnl.exe!ObDeleteSymbolicLink (codegen's name) |  |  | known |
| `0x8257879C` | `__imp__ObCreateSymbolicLink` | import thunk xboxkrnl.exe!ObCreateSymbolicLink (codegen's name) |  |  | known |
| `0x825787AC` | `__imp__RtlInitAnsiString` | import thunk xboxkrnl.exe!RtlInitAnsiString (codegen's name) |  |  | known |
| `0x825787BC` | `__imp__KeLockL2` | import thunk xboxkrnl.exe!KeLockL2 (codegen's name) |  |  | known |
| `0x825787CC` | `__imp__KeUnlockL2` | import thunk xboxkrnl.exe!KeUnlockL2 (codegen's name) |  |  | known |
| `0x825787DC` | `__imp__XamLoaderSetLaunchData` | import thunk xam.xex!XamLoaderSetLaunchData (codegen's name) |  |  | known |
| `0x825787EC` | `__imp__XamLoaderGetLaunchDataSize` | import thunk xam.xex!XamLoaderGetLaunchDataSize (codegen's name) |  |  | known |
| `0x825787FC` | `__imp__XNotifyGetNext` | import thunk xam.xex!XNotifyGetNext (codegen's name) |  |  | known |
| `0x8257880C` | `__imp__XGetVideoMode` | import thunk xam.xex!XGetVideoMode (codegen's name) |  |  | known |
| `0x8257881C` | `__imp__XamLoaderGetLaunchData` | import thunk xam.xex!XamLoaderGetLaunchData (codegen's name) |  |  | known |
| `0x8257882C` | `__imp__XMsgStartIORequestEx` | import thunk xam.xex!XMsgStartIORequestEx (codegen's name) |  |  | known |
| `0x8257883C` | `__imp__XMsgStartIORequest` | import thunk xam.xex!XMsgStartIORequest (codegen's name) |  |  | known |
| `0x8257884C` | `__imp__XamGetSystemVersion` | import thunk xam.xex!XamGetSystemVersion (codegen's name) |  |  | known |
| `0x8257885C` | `__imp__XMsgInProcessCall` | import thunk xam.xex!XMsgInProcessCall (codegen's name) |  |  | known |
| `0x8257886C` | `__imp__XamUserReadProfileSettings` | import thunk xam.xex!XamUserReadProfileSettings (codegen's name) |  |  | known |
| `0x8257887C` | `__imp__XamUserWriteProfileSettings` | import thunk xam.xex!XamUserWriteProfileSettings (codegen's name) |  |  | known |
| `0x8257888C` | `__imp__XamUserGetXUID` | import thunk xam.xex!XamUserGetXUID (codegen's name) |  |  | known |
| `0x8257889C` | `__imp__XamFree` | import thunk xam.xex!XamFree (codegen's name) |  |  | known |
| `0x825788AC` | `__imp__XamAlloc` | import thunk xam.xex!XamAlloc (codegen's name) |  |  | known |
| `0x825788BC` | `__imp__NetDll_WSAStartup` | import thunk xam.xex!NetDll\_WSAStartup (codegen's name) |  |  | known |
| `0x825788CC` | `__imp__NetDll_WSACleanup` | import thunk xam.xex!NetDll\_WSACleanup (codegen's name) |  |  | known |
| `0x825788DC` | `__imp__XamLoaderLaunchTitle` | import thunk xam.xex!XamLoaderLaunchTitle (codegen's name) |  |  | known |
| `0x825788EC` | `__imp__XamShowSigninUI` | import thunk xam.xex!XamShowSigninUI (codegen's name) |  |  | known |
| `0x825788FC` | `__imp__XamShowMarketplaceUI` | import thunk xam.xex!XamShowMarketplaceUI (codegen's name) |  |  | known |
| `0x8257890C` | `__imp__XamShowDeviceSelectorUI` | import thunk xam.xex!XamShowDeviceSelectorUI (codegen's name) |  |  | known |
| `0x8257891C` | `__imp__XamShowMessageBoxUI` | import thunk xam.xex!XamShowMessageBoxUI (codegen's name) |  |  | known |
| `0x8257892C` | `__imp__XamShowDirtyDiscErrorUI` | import thunk xam.xex!XamShowDirtyDiscErrorUI (codegen's name) |  |  | known |
| `0x8257893C` | `__imp__XamUserGetName` | import thunk xam.xex!XamUserGetName (codegen's name) |  |  | known |
| `0x8257894C` | `__imp__XamUserGetSigninState` | import thunk xam.xex!XamUserGetSigninState (codegen's name) |  |  | known |
| `0x8257895C` | `__imp__XamReadTileToTexture` | import thunk xam.xex!XamReadTileToTexture (codegen's name) |  |  | known |
| `0x8257896C` | `__imp__XamParseGamerTileKey` | import thunk xam.xex!XamParseGamerTileKey (codegen's name) |  |  | known |
| `0x8257897C` | `__imp__XamWriteGamerTile` | import thunk xam.xex!XamWriteGamerTile (codegen's name) |  |  | known |
| `0x8257898C` | `__imp__XGetGameRegion` | import thunk xam.xex!XGetGameRegion (codegen's name) |  |  | known |
| `0x8257899C` | `__imp__XamContentCreateEx` | import thunk xam.xex!XamContentCreateEx (codegen's name) |  |  | known |
| `0x825789AC` | `__imp__XamContentDelete` | import thunk xam.xex!XamContentDelete (codegen's name) |  |  | known |
| `0x825789BC` | `__imp__XamContentClose` | import thunk xam.xex!XamContentClose (codegen's name) |  |  | known |
| `0x825789CC` | `__imp__XamContentGetCreator` | import thunk xam.xex!XamContentGetCreator (codegen's name) |  |  | known |
| `0x825789DC` | `__imp__XamContentCreateEnumerator` | import thunk xam.xex!XamContentCreateEnumerator (codegen's name) |  |  | known |
| `0x825789EC` | `__imp__XamEnumerate` | import thunk xam.xex!XamEnumerate (codegen's name) |  |  | known |
| `0x825789FC` | `__imp__XamNotifyCreateListener` | import thunk xam.xex!XamNotifyCreateListener (codegen's name) |  |  | known |
| `0x82578A0C` | `__imp__XamShowMessageBoxUIEx` | import thunk xam.xex!XamShowMessageBoxUIEx (codegen's name) |  |  | known |
| `0x82578A1C` | `__imp__XGetLanguage` | import thunk xam.xex!XGetLanguage (codegen's name) |  |  | known |
| `0x82578A2C` | `__imp__XGetAVPack` | import thunk xam.xex!XGetAVPack (codegen's name) |  |  | known |
| `0x82578A3C` | `__imp__XamLoaderTerminateTitle` | import thunk xam.xex!XamLoaderTerminateTitle (codegen's name) |  |  | known |
| `0x82578A4C` | `__imp__XamInputGetCapabilities` | import thunk xam.xex!XamInputGetCapabilities (codegen's name) |  |  | known |
| `0x82578A5C` | `__imp__XamInputGetState` | import thunk xam.xex!XamInputGetState (codegen's name) |  |  | known |
| `0x82578A6C` | `__imp__XamInputSetState` | import thunk xam.xex!XamInputSetState (codegen's name) |  |  | known |
| `0x82578A7C` | `__imp__XamGetExecutionId` | import thunk xam.xex!XamGetExecutionId (codegen's name) |  |  | known |
| `0x82578A8C` | `__imp__NtClose` | import thunk xboxkrnl.exe!NtClose (codegen's name) |  |  | known |
| `0x82578A9C` | `__imp__DbgBreakPoint` | import thunk xboxkrnl.exe!DbgBreakPoint (codegen's name) |  |  | known |
| `0x82578AAC` | `__imp__XexUnloadImage` | import thunk xboxkrnl.exe!XexUnloadImage (codegen's name) |  |  | known |
| `0x82578ABC` | `__imp__XexLoadImage` | import thunk xboxkrnl.exe!XexLoadImage (codegen's name) |  |  | known |
| `0x82578ACC` | `__imp__NtCreateFile` | import thunk xboxkrnl.exe!NtCreateFile (codegen's name) |  |  | known |
| `0x82578ADC` | `__imp__NtSuspendThread` | import thunk xboxkrnl.exe!NtSuspendThread (codegen's name) |  |  | known |
| `0x82578AEC` | `__imp__KeSetAffinityThread` | import thunk xboxkrnl.exe!KeSetAffinityThread (codegen's name) |  |  | known |
| `0x82578AFC` | `__imp__NtOpenFile` | import thunk xboxkrnl.exe!NtOpenFile (codegen's name) |  |  | known |
| `0x82578B0C` | `__imp__NtWaitForSingleObjectEx` | import thunk xboxkrnl.exe!NtWaitForSingleObjectEx (codegen's name) |  |  | known |
| `0x82578B1C` | `__imp__NtWriteFile` | import thunk xboxkrnl.exe!NtWriteFile (codegen's name) |  |  | known |
| `0x82578B2C` | `__imp__NtReadFile` | import thunk xboxkrnl.exe!NtReadFile (codegen's name) |  |  | known |
| `0x82578B3C` | `__imp__RtlMultiByteToUnicodeN` | import thunk xboxkrnl.exe!RtlMultiByteToUnicodeN (codegen's name) |  |  | known |
| `0x82578B4C` | `__imp__NtCreateEvent` | import thunk xboxkrnl.exe!NtCreateEvent (codegen's name) |  |  | known |
| `0x82578B5C` | `__imp__MmQueryAllocationSize` | import thunk xboxkrnl.exe!MmQueryAllocationSize (codegen's name) |  |  | known |
| `0x82578B6C` | `__imp__MmQueryAddressProtect` | import thunk xboxkrnl.exe!MmQueryAddressProtect (codegen's name) |  |  | known |
| `0x82578B7C` | `__imp__NtReleaseMutant` | import thunk xboxkrnl.exe!NtReleaseMutant (codegen's name) |  |  | known |
| `0x82578B8C` | `__imp__NtSetEvent` | import thunk xboxkrnl.exe!NtSetEvent (codegen's name) |  |  | known |
| `0x82578B9C` | `__imp__NtYieldExecution` | import thunk xboxkrnl.exe!NtYieldExecution (codegen's name) |  |  | known |
| `0x82578BAC` | `__imp__NtCreateMutant` | import thunk xboxkrnl.exe!NtCreateMutant (codegen's name) |  |  | known |
| `0x82578BBC` | `__imp__NtSetInformationFile` | import thunk xboxkrnl.exe!NtSetInformationFile (codegen's name) |  |  | known |
| `0x82578BCC` | `__imp__NtQueryInformationFile` | import thunk xboxkrnl.exe!NtQueryInformationFile (codegen's name) |  |  | known |
| `0x82578BDC` | `__imp__NtQueryVolumeInformationFile` | import thunk xboxkrnl.exe!NtQueryVolumeInformationFile (codegen's name) |  |  | known |
| `0x82578BEC` | `__imp__NtQueryVirtualMemory` | import thunk xboxkrnl.exe!NtQueryVirtualMemory (codegen's name) |  |  | known |
| `0x82578BFC` | `__imp__MmQueryStatistics` | import thunk xboxkrnl.exe!MmQueryStatistics (codegen's name) |  |  | known |
| `0x82578C0C` | `__imp__NtResumeThread` | import thunk xboxkrnl.exe!NtResumeThread (codegen's name) |  |  | known |
| `0x82578C1C` | `__imp__NtClearEvent` | import thunk xboxkrnl.exe!NtClearEvent (codegen's name) |  |  | known |
| `0x82578C2C` | `__imp__NtPulseEvent` | import thunk xboxkrnl.exe!NtPulseEvent (codegen's name) |  |  | known |
| `0x82578C3C` | `__imp__NtCreateSemaphore` | import thunk xboxkrnl.exe!NtCreateSemaphore (codegen's name) |  |  | known |
| `0x82578C4C` | `__imp__NtReleaseSemaphore` | import thunk xboxkrnl.exe!NtReleaseSemaphore (codegen's name) |  |  | known |
| `0x82578C5C` | `__imp__NtAllocateVirtualMemory` | import thunk xboxkrnl.exe!NtAllocateVirtualMemory (codegen's name) |  |  | known |
| `0x82578C6C` | `__imp__KeBugCheckEx` | import thunk xboxkrnl.exe!KeBugCheckEx (codegen's name) |  |  | known |
| `0x82578C7C` | `__imp__NtFreeVirtualMemory` | import thunk xboxkrnl.exe!NtFreeVirtualMemory (codegen's name) |  |  | known |
| `0x82578C8C` | `__imp__RtlCompareMemoryUlong` | import thunk xboxkrnl.exe!RtlCompareMemoryUlong (codegen's name) |  |  | known |
| `0x82578C9C` | `__imp__RtlRaiseException` | import thunk xboxkrnl.exe!RtlRaiseException (codegen's name) |  |  | known |
| `0x82578CAC` | `__imp__NtQueryDirectoryFile` | import thunk xboxkrnl.exe!NtQueryDirectoryFile (codegen's name) |  |  | known |
| `0x82578CBC` | `__imp__KeDelayExecutionThread` | import thunk xboxkrnl.exe!KeDelayExecutionThread (codegen's name) |  |  | known |
| `0x82578CCC` | `__imp__RtlImageXexHeaderField` | import thunk xboxkrnl.exe!RtlImageXexHeaderField (codegen's name) |  |  | known |
| `0x82578CDC` | `__imp__HalReturnToFirmware` | import thunk xboxkrnl.exe!HalReturnToFirmware (codegen's name) |  |  | known |
| `0x82578CEC` | `__imp__ExTerminateThread` | import thunk xboxkrnl.exe!ExTerminateThread (codegen's name) |  |  | known |
| `0x82578CFC` | `__imp__RtlUnwind` | import thunk xboxkrnl.exe!RtlUnwind (codegen's name) |  |  | known |
| `0x82578D0C` | `__imp__KeBugCheck` | import thunk xboxkrnl.exe!KeBugCheck (codegen's name) |  |  | known |
| `0x82578D1C` | `__imp__KeTlsAlloc` | import thunk xboxkrnl.exe!KeTlsAlloc (codegen's name) |  |  | known |
| `0x82578D2C` | `__imp__KeTlsGetValue` | import thunk xboxkrnl.exe!KeTlsGetValue (codegen's name) |  |  | known |
| `0x82578D3C` | `__imp__KeTlsSetValue` | import thunk xboxkrnl.exe!KeTlsSetValue (codegen's name) |  |  | known |
| `0x82578D4C` | `__imp__KeTlsFree` | import thunk xboxkrnl.exe!KeTlsFree (codegen's name) |  |  | known |
| `0x82578D5C` | `__imp__RtlCaptureContext` | import thunk xboxkrnl.exe!RtlCaptureContext (codegen's name) |  |  | known |
| `0x82578D6C` | `__imp__XMAEnableContext` | import thunk xboxkrnl.exe!XMAEnableContext (codegen's name) |  |  | known |
| `0x82578D7C` | `__imp__XMASetOutputBufferValid` | import thunk xboxkrnl.exe!XMASetOutputBufferValid (codegen's name) |  |  | known |
| `0x82578D8C` | `__imp__XMAInitializeContext` | import thunk xboxkrnl.exe!XMAInitializeContext (codegen's name) |  |  | known |
| `0x82578D9C` | `__imp__XMADisableContext` | import thunk xboxkrnl.exe!XMADisableContext (codegen's name) |  |  | known |
| `0x82578DAC` | `__imp__XMASetInputBufferReadOffset` | import thunk xboxkrnl.exe!XMASetInputBufferReadOffset (codegen's name) |  |  | known |
| `0x82578DBC` | `__imp__XMASetOutputBufferReadOffset` | import thunk xboxkrnl.exe!XMASetOutputBufferReadOffset (codegen's name) |  |  | known |
| `0x82578DCC` | `__imp__XMAIsOutputBufferValid` | import thunk xboxkrnl.exe!XMAIsOutputBufferValid (codegen's name) |  |  | known |
| `0x82578DDC` | `__imp__XMAGetOutputBufferWriteOffset` | import thunk xboxkrnl.exe!XMAGetOutputBufferWriteOffset (codegen's name) |  |  | known |
| `0x82578DEC` | `__imp__XMAGetOutputBufferReadOffset` | import thunk xboxkrnl.exe!XMAGetOutputBufferReadOffset (codegen's name) |  |  | known |
| `0x82578DFC` | `__imp__XMAGetInputBufferReadOffset` | import thunk xboxkrnl.exe!XMAGetInputBufferReadOffset (codegen's name) |  |  | known |
| `0x82578E0C` | `__imp__XMASetInputBuffer1Valid` | import thunk xboxkrnl.exe!XMASetInputBuffer1Valid (codegen's name) |  |  | known |
| `0x82578E1C` | `__imp__XMAIsInputBuffer1Valid` | import thunk xboxkrnl.exe!XMAIsInputBuffer1Valid (codegen's name) |  |  | known |
| `0x82578E2C` | `__imp__XMASetInputBuffer0Valid` | import thunk xboxkrnl.exe!XMASetInputBuffer0Valid (codegen's name) |  |  | known |
| `0x82578E3C` | `__imp__XMAIsInputBuffer0Valid` | import thunk xboxkrnl.exe!XMAIsInputBuffer0Valid (codegen's name) |  |  | known |
| `0x82578E4C` | `__imp__KeQuerySystemTime` | import thunk xboxkrnl.exe!KeQuerySystemTime (codegen's name) |  |  | known |
| `0x82578E5C` | `__imp__RtlTimeToTimeFields` | import thunk xboxkrnl.exe!RtlTimeToTimeFields (codegen's name) |  |  | known |
| `0x82578E6C` | `__imp__NtFlushBuffersFile` | import thunk xboxkrnl.exe!NtFlushBuffersFile (codegen's name) |  |  | known |
| `0x82578E7C` | `__imp__RtlTimeFieldsToTime` | import thunk xboxkrnl.exe!RtlTimeFieldsToTime (codegen's name) |  |  | known |

### Other classes (from RTTI) (56)

| Address | Name | What it does | Arguments | Port use | Sources |
|---|---|---|---|---|---|
| `0x820A4EB8` | `DbFieldSet::vfunc_0` | rtti DbFieldSet vtable 0x82068E08 slot 0 |  |  | high; names.toml |
| `0x820A5000` | `DbQuery::vfunc_0` | rtti DbQuery vtable 0x82068E10 slot 0 |  |  | high; names.toml |
| `0x820A5078` | `ae_heap_base::vfunc_0` | rtti ae\_heap\_base vtable 0x82068DE0 slot 0 |  |  | high; names.toml |
| `0x820AC528` | `std_exception::deleting_dtor` | vtable slot 0: (this, flags) with the flags & 1 delete test | `(this, flags)` |  | high; names.toml |
| `0x820AC628` | `std_logic_error::dtor` | rtti: stores the std::logic\_error vtable, then its base's (std::exception) |  |  | high; names.toml |
| `0x820AC6A0` | `std_logic_error::vfunc_1` | rtti std::logic\_error vtable 0x82070AF4 slot 1 (also in 2 derived vtables) |  |  | high; names.toml |
| `0x820AC6C0` | `std_logic_error::vfunc_0` | rtti std::logic\_error vtable 0x82070AD4 slot 0 |  |  | high; names.toml |
| `0x820AC760` | `std_length_error::vfunc_0` | rtti std::length\_error vtable 0x82070AE4 slot 0 |  |  | high; names.toml |
| `0x820AC810` | `std_out_of_range::vfunc_0` | rtti std::out\_of\_range vtable 0x82070AF4 slot 0 |  |  | high; names.toml |
| `0x820C0258` | `cdResourceDirectory_nalBaseSkeleton::vfunc_1` | rtti cdResourceDirectory&lt;nalBaseSkeleton&gt; vtable 0x8207842C slot 1 |  |  | high; names.toml |
| `0x820C0850` | `tlResourceDirectory_nalBaseSkeleton::vfunc_7` | rtti tlResourceDirectory&lt;nalBaseSkeleton&gt; vtable 0x8207842C slot 7 (also in 1 derived vtables) |  |  | high; names.toml |
| `0x820C0858` | `tlResourceDirectory_nalBaseSkeleton::vfunc_8` | rtti tlResourceDirectory&lt;nalBaseSkeleton&gt; vtable 0x820782E8 slot 8 |  |  | high; names.toml |
| `0x820C0E28` | `cdResourceDirectory_nalBaseSkeleton::vfunc_8` | rtti cdResourceDirectory&lt;nalBaseSkeleton&gt; vtable 0x8207842C slot 8 |  |  | high; names.toml |
| `0x820C0EC0` | `tlResourceDirectory_nalBaseSkeleton::deleting_dtor` | vtable slot 0: (this, flags) with the flags & 1 delete test | `(this, flags)` |  | high; names.toml |
| `0x820C1138` | `cdResourceDirectory_nalBaseSkeleton::deleting_dtor` | vtable slot 0: (this, flags) with the flags & 1 delete test | `(this, flags)` |  | high; names.toml |
| `0x820C4638` | `phys_gjk_geom_aabb::vfunc_0` | rtti phys\_gjk\_geom\_aabb vtable 0x8207A5F0 slot 0 |  |  | high; names.toml |
| `0x820C4688` | `phys_gjk_geom_aabb::vfunc_1` | rtti phys\_gjk\_geom\_aabb vtable 0x8207A5F0 slot 1 |  |  | high; names.toml |
| `0x820C4698` | `phys_gjk_geom_aabb::vfunc_2` | rtti phys\_gjk\_geom\_aabb vtable 0x8207A5F0 slot 2 |  |  | high; names.toml |
| `0x820C4700` | `phys_gjk_geom_vert_list::vfunc_0` | rtti phys\_gjk\_geom\_vert\_list vtable 0x8207A608 slot 0 |  |  | high; names.toml |
| `0x820C4778` | `phys_gjk_geom_vert_list::vfunc_2` | rtti phys\_gjk\_geom\_vert\_list vtable 0x8207A608 slot 2 |  |  | high; names.toml |
| `0x820C47C0` | `phys_gjk_geom_ragdoll_1::vfunc_0` | rtti phys\_gjk\_geom\_ragdoll\_1 vtable 0x8207A620 slot 0 |  |  | high; names.toml |
| `0x820C4890` | `phys_gjk_geom_ragdoll_1::vfunc_1` | rtti phys\_gjk\_geom\_ragdoll\_1 vtable 0x8207A620 slot 1 |  |  | high; names.toml |
| `0x820C48A0` | `phys_gjk_geom_ragdoll_1::vfunc_2` | rtti phys\_gjk\_geom\_ragdoll\_1 vtable 0x8207A620 slot 2 |  |  | high; names.toml |
| `0x820C49E8` | `phys_gjk_geom_aabb::vfunc_4` | rtti phys\_gjk\_geom\_aabb vtable 0x8207A5F0 slot 4 |  |  | high; names.toml |
| `0x820C4B38` | `phys_gjk_geom_vert_list::vfunc_4` | rtti phys\_gjk\_geom\_vert\_list vtable 0x8207A608 slot 4 |  |  | high; names.toml |
| `0x820C4D20` | `physics_colgeom_visitor::vfunc_0` | rtti physics\_colgeom\_visitor vtable 0x8207A638 slot 0 |  |  | high; names.toml |
| `0x820C5090` | `collide_entities_callback::vfunc_0` | rtti collide\_entities\_callback vtable 0x8207A640 slot 0 |  |  | high; names.toml |
| `0x820C8450` | `std_bad_alloc::ctor` (likely) | rtti: stores the std::bad\_alloc vtable at this+0; stores base then own vtables and returns this |  |  | medium |
| `0x820E0FF8` | `tlInitList::deleting_dtor` | vtable slot 0: (this, flags) with the flags & 1 delete test | `(this, flags)` |  | high; names.toml |
| `0x820EC028` | `phys_gjk_geom_ragdoll_1::vfunc_3` | rtti phys\_gjk\_geom\_ragdoll\_1 vtable 0x8207A620 slot 3 |  |  | high; names.toml |
| `0x821C4EA0` | `std_logic_error::ctor` (likely) | rtti: stores the std::logic\_error vtable at this+0; stores base then own vtables and returns this |  |  | medium |
| `0x821D0598` | `CCalChannel::SetMediaInfo` | string 'CCalChannel::SetMediaInfo: Sub-titles' (only user) |  |  | high; names.toml |
| `0x822C0128` | `std_exception::vfunc_1` | rtti std::exception vtable 0x82070AC4 slot 1 (also in 1 derived vtables) |  |  | high; names.toml |
| `0x823C6330` | `AITypeManager::ctor` | rtti: stores the AITypeManager vtable at this+0; called on the result of operator new |  |  | high; names.toml |
| `0x823F10A8` | `grassmanager::FrustumCullGPatch` | string 'grassmanager::FrustumCullGPatch(), heap isn't full, but r...' (only user) |  |  | high; names.toml |
| `0x82404358` | `phys_gjk_geom_ragdoll_1::vfunc_4` | rtti phys\_gjk\_geom\_ragdoll\_1 vtable 0x8207A620 slot 4 |  |  | high; names.toml |
| `0x82416E90` | `ApsGameClient::vfunc_3` | rtti ApsGameClient vtable 0x8207A444 slot 3 |  |  | high; names.toml |
| `0x82417CF0` | `XModelPartsManager::vfunc_2` | rtti XModelPartsManager vtable 0x8207A418 slot 2 |  |  | high; names.toml |
| `0x8241DAF0` | `XModelPartsManager::ctor` | rtti: stores the XModelPartsManager vtable at this+0; called on the result of operator new |  |  | high; names.toml |
| `0x8241DB60` | `XModelManager::ctor` | rtti: stores the XModelManager vtable at this+0; called on the result of operator new |  |  | high; names.toml |
| `0x824244A8` | `ApsGameClient::vfunc_2` | rtti ApsGameClient vtable 0x8207A444 slot 2 |  |  | high; names.toml |
| `0x824245D8` | `ApsGameClient::vfunc_1` | rtti ApsGameClient vtable 0x8207A444 slot 1 |  |  | high; names.toml |
| `0x824525A0` | `StreamZoneManager::vfunc_1` | rtti StreamZoneManager vtable 0x82078394 slot 1 |  |  | high; names.toml |
| `0x82457FE0` | `StreamZoneManager::ctor` | rtti: stores the StreamZoneManager vtable at this+0; called on the result of operator new |  |  | high; names.toml |
| `0x8245A330` | `cdResourceDirectory_nalBaseSkeleton::vfunc_3` | rtti cdResourceDirectory&lt;nalBaseSkeleton&gt; vtable 0x8207842C slot 3 |  |  | high; names.toml |
| `0x8245A3C8` | `cdResourceDirectory_nalBaseSkeleton::vfunc_2` | rtti cdResourceDirectory&lt;nalBaseSkeleton&gt; vtable 0x8207842C slot 2 |  |  | high; names.toml |
| `0x82463E90` | `CurveManager::vfunc_1` | rtti CurveManager vtable 0x8207759C slot 1 |  |  | high; names.toml |
| `0x82463E98` | `CurveManager::vfunc_0` | rtti CurveManager vtable 0x8207759C slot 0 |  |  | high; names.toml |
| `0x82464650` | `rtree_visitor_t::vfunc_0` | rtti rtree\_visitor\_t vtable 0x82074154 slot 0 |  |  | high; names.toml |
| `0x82467FA0` | `GdbFileManager::ctor` | rtti: stores the GdbFileManager vtable at this+0; called on the result of operator new |  |  | high; names.toml |
| `0x8246A848` | `ai_collision_context_t::vfunc_0` | rtti ai\_collision\_context\_t vtable 0x8207415C slot 0 |  |  | high; names.toml |
| `0x8246A870` | `player_collision_context_t::vfunc_0` | rtti player\_collision\_context\_t vtable 0x820678D8 slot 0 |  |  | high; names.toml |
| `0x82479470` | `CurveManager::ctor` | rtti: stores the CurveManager vtable at this+0; called on the result of operator new |  |  | high; names.toml |
| `0x82525638` | `STBManager::ctor` | rtti: stores the STBManager vtable at this+0; called on the result of operator new |  |  | high; names.toml |
| `0x825256F0` | `ConfigStringManager::ctor` | rtti: stores the ConfigStringManager vtable at this+0; called on the result of operator new |  |  | high; names.toml |
| `0x8252B418` | `DbQuery::vfunc_1` | rtti DbQuery vtable 0x82068E10 slot 1 |  |  | high; names.toml |

### Other (9)

| Address | Name | What it does | Arguments | Port use | Sources |
|---|---|---|---|---|---|
| `0x8219B308` | `DeleteTransientData` | string 'DeleteTransientData() called with no intact transient data' (only user) |  |  | high; names.toml |
| `0x8219B358` | `SetTreeSize` | string 'SetTreeSize() is only valid for size values greater than 0.0' (only user) |  |  | high; names.toml |
| `0x8219B488` | `SetCamera` | string 'SetCamera() requires non-NULL position and direction values' (only user) |  |  | high; names.toml |
| `0x8219B6A0` | `SetNumLeafRockingGroups` | string 'SetNumLeafRockingGroups() has no effect after Compute() h...' (only user) |  |  | high; names.toml |
| `0x8219B6E0` | `SetLeafWindMethod` | string 'SetLeafWindMethod() has no effect after Compute() has bee...' (only user) |  |  | high; names.toml |
| `0x8219B728` | `SetBranchWindMethod` | string 'SetBranchWindMethod() has no effect after Compute() has b...' (only user) |  |  | high; names.toml |
| `0x8219B7E0` | `SetFrondWindMethod` | string 'SetFrondWindMethod() has no effect after Compute() has be...' (only user) |  |  | high; names.toml |
| `0x8219B898` | `SetWindStrength` | string 'SetWindStrength() expects new wind strength &gt;= 0.0' (only user) |  |  | high; names.toml |
| `0x8219B998` | `GetBoundingBox` | string 'GetBoundingBox() expects a non-NULL parameter' (only user) |  |  | high; names.toml |

### Compiler helpers (239)

Register save and restore routines the compiler calls in prologues and epilogues; the codegen names them itself: `__savegprlr_14`, `__savegprlr_15`, `__savegprlr_16`, `__savegprlr_17`, `__savegprlr_18`, `__savegprlr_19` ...
