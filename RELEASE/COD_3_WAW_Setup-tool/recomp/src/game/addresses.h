// cod3 - reverse-engineered addresses for Call of Duty 3 (Xbox 360)
//
// Target binary: default.xex, Title ID 415607E1, media ID 2E07093A,
// "Call of Duty 3 (USA, Europe)" redump, SHA-1 681724c40e250e584e6585dd903e05afa00a8634.
// Every address below was located by hand in that image; a different build
// (other region, title update) will not match.
//
// The engine is a Quake III derivative (CoD:UO lineage): cvars use the Q3
// cvar_t / vmCvar_t layouts and the Q3 command buffer is intact. Retail
// stripped the bodies of Com_Printf / Com_DPrintf (they are bare `blr`s) and
// the console UI, but every caller still passes its format arguments.

#pragma once

#include <cstdint>

#if defined(COD3_MP)  // [mp] codmp_xenonf.xex's values (tools/mp_aliases.py)
#include "game/mp/addresses_mp.h"
#else

namespace cod3::addr {

// ---------------------------------------------------------------------------
// Cvar system (qcommon/cvar.c)
// ---------------------------------------------------------------------------
// cvar_t* Cvar_Get(const char* name, const char* value, int flags)
inline constexpr uint32_t kCvarGet = 0x8252BBE0;
// cvar_t* Cvar_Set2(const char* name, const char* value, qboolean force)
inline constexpr uint32_t kCvarSet2 = 0x8252B850;
// cvar_t* Cvar_FindVar(const char* name)
inline constexpr uint32_t kCvarFindVar = 0x82529790;
// cvar_t* cvar_vars  (alphabetically sorted list, linked through cvar_t::next)
inline constexpr uint32_t kCvarVars = 0x829BAB14;
// cvar_t* sv_cheats
inline constexpr uint32_t kSvCheatsPtr = 0x829BB178;

// cvar_t layout (44 bytes)
namespace cvar {
inline constexpr uint32_t kName = 0;           // char*
inline constexpr uint32_t kString = 4;         // char*
inline constexpr uint32_t kResetString = 8;    // char*
inline constexpr uint32_t kLatchedString = 12; // char*
inline constexpr uint32_t kFlags = 16;         // int
inline constexpr uint32_t kModified = 20;      // qboolean
inline constexpr uint32_t kModCount = 24;      // int
inline constexpr uint32_t kValue = 28;         // float
inline constexpr uint32_t kInteger = 32;       // int
inline constexpr uint32_t kNext = 36;          // cvar_t*
inline constexpr uint32_t kHashNext = 40;      // cvar_t*
}  // namespace cvar

// Q3 cvar flags (verified against the checks in Cvar_Set2)
namespace cvar_flag {
inline constexpr uint32_t kArchive = 0x0001;
inline constexpr uint32_t kUserInfo = 0x0002;
inline constexpr uint32_t kServerInfo = 0x0004;
inline constexpr uint32_t kSystemInfo = 0x0008;
inline constexpr uint32_t kInit = 0x0010;       // "%s is write protected."
inline constexpr uint32_t kLatch = 0x0020;      // "%s will be changed upon restarting."
inline constexpr uint32_t kRom = 0x0040;        // "%s is read only."
inline constexpr uint32_t kUserCreated = 0x0080;
inline constexpr uint32_t kTemp = 0x0100;
inline constexpr uint32_t kCheat = 0x0200;      // "%s is cheat protected." unless sv_cheats
}  // namespace cvar_flag

// ---------------------------------------------------------------------------
// Command system (qcommon/cmd.c)
// ---------------------------------------------------------------------------
// void Cbuf_AddText(const char* text)        - appends, caller adds '\n'
inline constexpr uint32_t kCbufAddText = 0x824644D0;
// void Cbuf_InsertText(const char* text)
inline constexpr uint32_t kCbufInsertText = 0x82464408;
// void Cbuf_Execute(void)                    - run once per frame by Com_Frame
inline constexpr uint32_t kCbufExecute = 0x8247E380;
// void Cbuf_ExecuteText(int exec_when, const char* text)
inline constexpr uint32_t kCbufExecuteText = 0x8247E498;
// void Cmd_ExecuteString(const char* text)
inline constexpr uint32_t kCmdExecuteString = 0x8247E228;
// void Cmd_AddCommand(const char* name, xcommand_t function)
inline constexpr uint32_t kCmdAddCommand = 0x8246CBA8;
// cmd_function_t* cmd_functions  { next @0, name @4, type @8 (1=engine 2=script), func @12 }
inline constexpr uint32_t kCmdFunctions = 0x829EBA10;

// ---------------------------------------------------------------------------
// Printing / errors (qcommon/common.c)
// ---------------------------------------------------------------------------
// void Com_Printf(const char* fmt, ...)      - body compiled out in retail
inline constexpr uint32_t kComPrintf = 0x82523A58;
// void Com_DPrintf(const char* fmt, ...)     - body compiled out in retail
inline constexpr uint32_t kComDPrintf = 0x82523A60;
// void Com_Error(int code, const char* fmt, ...) - retail just hits DbgBreakPoint
inline constexpr uint32_t kComError = 0x825239A0;
// qboolean Cvar_Command(void): a dvar name typed alone. It answers through
// Com_DPrintf in this build (so only with developer 1) with these two formats:
// "\"%s\" is:\"%s^7\" default:\"%s^7\"\n" and, when a change waits for a
// restart, "latched: \"%s\"\n". With a value it calls Cvar_Set2(force 0).
inline constexpr uint32_t kCvarCommand = 0x8252DB38;
inline constexpr uint32_t kCvarCommandValueFormat = 0x82068B08;
inline constexpr uint32_t kCvarCommandLatchedFormat = 0x82068B28;

// ---------------------------------------------------------------------------
// cgame field of view
// ---------------------------------------------------------------------------
// float CG_GetViewFov(void): reads cg_fov, clamps to [1,160], applies ADS /
// turret / vehicle zoom, returns the horizontal FOV in f1. Every consumer
// (refdef, zoom sensitivity, viewmodel) goes through it.
inline constexpr uint32_t kCgGetViewFov = 0x8243A198;
// vmCvar_t cg_fov { handle @0, modificationCount @4, value @8, integer @12, string[256] @16 }
inline constexpr uint32_t kCgFovVmCvar = 0x82A11358;
inline constexpr uint32_t kVmCvarValue = 8;    // float
inline constexpr uint32_t kVmCvarInteger = 12; // int
// Who else changes the FOV (game/fov.h):
// - The interaction system (vehicles, turrets, melee, levers ...), one
//   Interaction per local client: its start (0x82501BA8, 0x824FC858) saves
//   cgame's cg_fov value at +176, melee states blend it (0x824F2738), and
//   its end (0x824FDE90 (interaction, restore_view)) writes the saved value
//   back. All three write the vmCvar directly, never the dvar.
// - Level scripts: the level interface's setcvar slots 0x258 (string),
//   0x25C (int) and 0x260 (float), each Cvar_Get(name, value, 0x1000) then
//   Cvar_Set2(force 1); they return the cvar_t*.
namespace interact {
inline constexpr uint32_t kPerClient = 0x82A59DA0;  // Interaction*[4], by kActiveLocalClient
inline constexpr uint32_t kState = 4;               // the running InteractState*, 0 = none
inline constexpr uint32_t kSavedFov = 176;          // cgame's cg_fov when it started
inline constexpr uint32_t kEnd = 0x824FDE90;        // hooked: sub_824FDE90
}  // namespace interact
namespace level_cvar {
inline constexpr uint32_t kSetString = 0x824AB398;  // hooked: sub_824AB398 (name, const char*)
inline constexpr uint32_t kSetInt = 0x824AB300;     // hooked: sub_824AB300 (name, int)
inline constexpr uint32_t kSetFloat = 0x824AB260;   // hooked: sub_824AB260 (name, f1)
}  // namespace level_cvar

// ---------------------------------------------------------------------------
// Client input / view angles (client/cl_input.c)
// ---------------------------------------------------------------------------
// void CL_AdjustAngles(void): turns the view from the right stick (or the
// +left/+right keys with cl_yawspeed / cl_pitchspeed). Called first thing by
// CL_CreateCmd (0x82510748), which saves the old pitch before and clamps the
// per-frame pitch change after - the right place to add mouse deltas.
inline constexpr uint32_t kClAdjustAngles = 0x8250B460;
// int clientActiveIndex: local client whose usercmd is being built.
inline constexpr uint32_t kActiveLocalClient = 0x82A2A1C8;
// clientActive_t cl[] (one per local splitscreen player).
inline constexpr uint32_t kClientActive = 0x82A57900;
inline constexpr uint32_t kClientActiveStride = 7280;
// float viewangles[3] (PITCH, YAW, ROLL) inside clientActive_t.
inline constexpr uint32_t kViewAnglesOffset = 5732;

// The player's control settings (Options > Controls, saved in the profile),
// as the stick turning (0x8250FF48) reads them. The Controls page copies its
// rows into this struct when it closes (0x824E5DF8).
namespace controls {
inline constexpr uint32_t kSettings = 0x82A8E610;
// Options > Gameplay rows (copied in by 0x824C3E48 when the page closes):
inline constexpr uint32_t kSubtitles = 56;           // u8
inline constexpr uint32_t kCrosshair = 57;           // u8: Gameplay > Crosshair on
inline constexpr uint32_t kFriendlyTags = 58;        // u8
inline constexpr uint32_t kStickyAim = 67;           // u8: the game's aim assist (read by 0x8248D138)
// void OptionsGameplayMenu::Apply(page): copies the page's four rows into the settings.
inline constexpr uint32_t kGameplayApply = 0x824C3E48;
// Reads the settings from the profile save (0x823FBC58).
inline constexpr uint32_t kProfileLoad = 0x823FBC58;
inline constexpr uint32_t kInvertAim = 65;           // u8: Invert aim (pitch sign +128 instead of -128)
inline constexpr uint32_t kHorizontalSens = 68;      // s32: 0..37, times kSensScale (0 counts as kSensZero)
inline constexpr uint32_t kVerticalSens = 72;        // s32: 0..50, times kSensScale
inline constexpr uint32_t kStickLayout = 80;         // s32: 2 and 3 swap the sticks
inline constexpr float kSensScale = 0.04f;           // 0x8207C648
inline constexpr float kSensZero = 0.1f;             // 0x82001930
}  // namespace controls

// Crosshair color. CG_CalcCrosshairColor (0x82435588) gives the infantry
// crosshair white, red (0x08000000 in ps+2860: aiming at an enemy), green
// 0.25/1/0.25 (0x02000000: a friendly) or black (the entity under it is
// dead), with alpha cg_crosshairAlpha times the movement fade. The mounted /
// vehicle crosshair (0x82439530) is always white at cg_crosshairAlpha.
namespace crosshair {
inline constexpr uint32_t kColorOnStack = 112;         // CG_DrawCrosshair: float rgba[4] at r1+112
inline constexpr uint32_t kMountedColorOnStack = 128;  // 0x82439530: float rgb at r1+128
inline constexpr uint32_t kAlphaVmCvar = 0x82A0E630;   // vmCvar cg_crosshairAlpha (value at +8)
inline constexpr uint32_t kVmCvarValue = 8;
}  // namespace crosshair

// Cheat commands (the client command table at 0x825F49F0: {name, handler}).
// Each cheat handler asks CheatsOk(ent) first: the cheats vmCvar on and the
// player alive. See game/cheat_watch.h.
namespace cheats {
inline constexpr uint32_t kCheatsOk = 0x825432C8;      // qboolean CheatsOk(gentity_t*)
inline constexpr uint32_t kCheatsVmCvar = 0x829B6110;  // vmCvar CheatsOk reads (integer at +12)
// The console game turns cheats on for every level: SV_Map_f calls
// Cvar_Set2("sv_cheats", "1", force 1) right after SV_SpawnServer (whatever
// the map command was). This is that call's return address (the generated
// code sets ctx.lr to it), and a "0" string in the game's data (sv_cheats's
// own default in its Cvar_Get).
inline constexpr uint32_t kSvMapSetsCheatsReturn = 0x82518DDC;
inline constexpr uint32_t kStringZero = 0x82065B94;
inline constexpr uint32_t kVmCvarInteger = 12;
inline constexpr uint32_t kGod = 0x825452E0;           // Cmd_God_f: ent->flags ^= 1
inline constexpr uint32_t kNoTarget = 0x82545248;      // Cmd_Notarget_f: ent->flags ^= 2
inline constexpr uint32_t kNoclip = 0x825451B0;        // Cmd_Noclip_f: client->noclip
inline constexpr uint32_t kUfo = 0x82545128;           // Cmd_Ufo_f: client->ufo
inline constexpr uint32_t kGive = 0x82560988;
inline constexpr uint32_t kSetViewPos = 0x82543048;    // checks the cheats itself
inline constexpr uint32_t kJumpToNode = 0x82542F00;    // checks the cheats itself
// ("invinc" at 0x8253FE98 is an empty function in this build.)
inline constexpr uint32_t kEntClient = 528;            // gentity_t::client
inline constexpr uint32_t kEntFlags = 632;             // gentity_t::flags
inline constexpr uint32_t kFlagGod = 0x1;
inline constexpr uint32_t kFlagNoTarget = 0x2;
inline constexpr uint32_t kClientNoclip = 1452;        // gclient_t::noclip
inline constexpr uint32_t kClientUfo = 1456;           // gclient_t::ufo
// Level start: SV_SpawnServer (prints "------ Server Initialization ------")
// and the map_restart / map_restart_internal commands.
inline constexpr uint32_t kSpawnServer = 0x82517A00;
inline constexpr uint32_t kMapRestart = 0x825148D0;
inline constexpr uint32_t kMapRestartInternal = 0x825149A8;
}  // namespace cheats

// ---------------------------------------------------------------------------
// Front end: Options > Gameplay page (panel SP_OP_gameplay.panel)
// ---------------------------------------------------------------------------
// The panel layout ships five rows (slot_01..slot_05) but the code only wires
// four options and hides row five. We bring row five back as "Field of View".
namespace fe {
// Page vtable 0x82071FAC; methods we hook:
inline constexpr uint32_t kGameplayCtor = 0x824DBBF0;     // (this, parent)
inline constexpr uint32_t kGameplayCtorCountInsn = 0x824DBC0C;  // li r5,4 (item count)
inline constexpr uint32_t kGameplayInit = 0x824E4D78;     // vtbl[0]  (this, panel)
inline constexpr uint32_t kGameplayUpdate = 0x824CD2D0;   // vtbl[16] (this, f1=dt)
inline constexpr uint32_t kGameplayRefresh = 0x824DB960;  // vtbl[21] (this)
inline constexpr uint32_t kGameplayMoveA = 0x824C3FE0;    // vtbl[25] (this) up/down + texts
inline constexpr uint32_t kGameplayMoveB = 0x824C3F08;    // vtbl[26] (this)
inline constexpr uint32_t kGameplayToggle = 0x824BD160;   // (this, dir) via vtbl[27]/[28]

// Page object fields
inline constexpr uint32_t kItems = 4;              // MenuItem** (count at +38)
inline constexpr uint32_t kSelected = 34;          // int16
inline constexpr uint32_t kItemCount = 38;         // int16
inline constexpr uint32_t kPanel = 56;             // Panel*
inline constexpr uint32_t kCurrentSelectedText = 72;  // text element
inline constexpr uint32_t kInstructionsText = 92;  // wrapped text element
inline constexpr uint32_t kArrowFlashTimer = 152;  // float

// Panel element lookup: Element* Panel_FindElement(Panel*, const char* name)
inline constexpr uint32_t kPanelFindElement = 0x824D07D0;  // Element* Panel::FindElement(panel, name): elements @12, count @20; 0 if missing
// The panel's pictures (arrows, row backgrounds) are a second list: quads @0,
// count @8, each named at +72 (string, chars at +12). A missing name gives a
// shared stand-in, so check the name of what comes back.
inline constexpr uint32_t kPanelFindQuad = 0x824DFC70;     // PanelQuad* (panel, name)
inline constexpr uint32_t kQuadName = 72;
inline constexpr uint32_t kQuadY = 24;                     // float (x at +20)
// What a quad draws (PanelQuad draw 0x824C80D0): +32 = array of parts, +40
// = their count; each part is drawn by 0x824BFDD8 from its four vertices
// {x, y, u, v, color} at +16, 20 bytes apart.
inline constexpr uint32_t kQuadParts = 32;
inline constexpr uint32_t kQuadPartCount = 40;
inline constexpr uint32_t kQuadPartVerts = 16;
inline constexpr uint32_t kQuadVertStride = 20;
// A text's transform, animated by the panel: +32 offset, +44 x, +48 y, +52 z,
// +56 / +68 / +80 scales (selected, ?, normal), all floats.
inline constexpr uint32_t kTextTransform = 32;
inline constexpr uint32_t kTextTransformEnd = 92;
inline constexpr uint32_t kTextX = 44;                     // float
inline constexpr uint32_t kTextY = 48;                     // float
inline constexpr uint32_t kElemOpacity = 4;                // float, animated by the panel's fades
// Copies of elements, the way the game makes them (0x824E1588): operator new,
// the class's constructor, then Assign(source) (vtable slot 18, a deep copy).
inline constexpr uint32_t kOperatorNew = 0x820CC1D0;       // (size)
inline constexpr uint32_t kTextCtor = 0x820ACA38;          // FEText(this), 112 bytes
inline constexpr uint32_t kTextSize = 112;
inline constexpr uint32_t kQuadCtor = 0x824C8498;          // PanelQuad(this), 80 bytes
inline constexpr uint32_t kQuadSize = 80;
inline constexpr uint32_t kElemDelete = 0;                 // (flags 1): destroy and free
inline constexpr uint32_t kElemAssign = 72;                // (source)
inline constexpr uint32_t kElemDraw = 4;                   // () as the panel draws (0x824BFA70)
// The gameplay page's draw (vtbl[14], 0x824CD398): its panel, then its items
// (FEMenu::DrawItems 0x824C58F0 draws each item's label, highlighted or not).
inline constexpr uint32_t kGameplayDraw = 0x824CD398;
inline constexpr uint32_t kOperatorDelete = 0x820CC468;     // (pointer)
// Page virtual slots used to move the highlight like FEMenu's own moves do.
inline constexpr uint32_t kPageSetSelection = 164;         // (index, 1)
inline constexpr uint32_t kPageSelectionChanged = 28;      // ()

// Pause menu > Options: AdvancedOptionsMenu (vtable 0x82071B24, panel
// advanced_options_menu). 11 items: 0 Invert Aim, 1 Vibration, 2/3
// Horizontal / Vertical Sensitivity, 4 Crosshair, 5 Friendly Tags, 6 Music
// Volume, 7 Subtitles, 8 Tank Controls, 9 Reset to Defaults, 10 Back
// (hidden). Rows are 20 apart from y 151 to 311; Reset sits at 361. The
// panel's unused "ADSToggle" row (ADSToggleOnOff, arrowL7/arrowR7) is parked
// at y -102.2. MoveUp/MoveDown wrap 0 <-> 9 (0x824BD560).
namespace adv {
inline constexpr uint32_t kVtable = 0x82071B24;
inline constexpr uint32_t kInit = 0x824E60E8;              // (this, panel); hooked
inline constexpr uint32_t kRefresh = 0x824E6588;           // vtbl[21]; hooked
inline constexpr uint32_t kUpdate = 0x824EE128;            // vtbl[16] (f1 = dt); hooked
inline constexpr uint32_t kDraw = 0x824CD530;              // vtbl[14]: panel, items, values; hooked
inline constexpr uint32_t kMoveUp = 0x824C4218;            // vtbl[25]; hooked
inline constexpr uint32_t kMoveDown = 0x824C41B8;          // vtbl[26]; hooked
inline constexpr uint32_t kToggle = 0x824DC1C0;            // (this, right): OnLeft / OnRight; hooked
inline constexpr uint32_t kSyncValueHighlight = 0x824BD3F8;  // (this): after a move
inline constexpr uint32_t kPanel = 56;
inline constexpr int kItemCount = 11;
inline constexpr int kTankItem = 8;
inline constexpr int kResetItem = 9;
// For the video rows (game/menu_video.h).
inline constexpr uint32_t kSelect = 0x824E6020;            // vfunc_19 (this, item): 9 Reset, 10 Back; hooked
inline constexpr uint32_t kBack = 0x824F07B0;              // vfunc_30 (this): B; hooked
inline constexpr uint32_t kTitleKey = 0x8206FEA4;          // "FEMENU_OPTIONS" on the "AdvancedOptions" text
inline constexpr uint32_t kValueNames = 0x82064658;        // 6 x char*: InvertAimOnOff ... TankControlOnOff
inline constexpr uint32_t kArrowNames = 0x82064670;        // 12 x char*: arrowL1, arrowR1 ... arrowR6
}  // namespace adv

// Front end Options page: ProfileEditMenu (vtable 0x820729A4, panel
// SP_OP_editprofile.panel). Four items GAMEPLAY, CONTROLS, SOUND, SCREEN on
// text_option_01..04; the panel has a fifth row (text_option_05, bkg_row_05,
// bkg_line_04) that Init hides. Its item handler opens page 3 (gameplay), 4,
// 5 or 8 with FEMenuSystem vfunc_4 (page, 0). Title / help text keys come
// from two four-entry tables indexed by the selection.
namespace options {
inline constexpr uint32_t kVtable = 0x820729A4;
inline constexpr uint32_t kInit = 0x824E2948;               // vtbl[0] (this, panel); hooked
inline constexpr uint32_t kSelect = 0x824BC070;             // vtbl[19] (this, item); hooked
inline constexpr uint32_t kEnter = 0x824ED298;              // vtbl[20] (this, how); hooked
inline constexpr uint32_t kMoveUp = 0x824C3248;             // vtbl[25]; hooked
inline constexpr uint32_t kMoveDown = 0x824C3190;           // vtbl[26]; hooked
inline constexpr uint32_t kPanel = 60;
inline constexpr uint32_t kSelectedText = 72;               // the big title of the selected item
inline constexpr uint32_t kInstructions = 80;               // wrapped help text
inline constexpr int kItemCount = 4;
inline constexpr int kVideoItem = 4;
inline constexpr uint32_t kGameplayPage = 3;                // FrontEndMenuSystem page index
}  // namespace options

// FEMenu / FEMenuSystem: the owning menu system and its "go to page" call.
inline constexpr uint32_t kPageSystem = 8;                  // FEMenu: FEMenuSystem*
inline constexpr uint32_t kSysPages = 4;                    // FEMenuSystem: FEMenu** pages
inline constexpr uint32_t kSysGoTo = 16;                    // vtbl: (page index, 0)

// Options > Gameplay page fields used by the video mode (game/menu_video.h).
inline constexpr uint32_t kGameplayTitleMain = 64;          // "OPTIONS / GAMEPLAY" text
inline constexpr uint32_t kGameplayValues = 100;            // 4 x value text (slot_0N_text_b)
inline constexpr uint32_t kGameplayArrows = 116;            // 4 x {left, right} arrow pictures
inline constexpr uint32_t kGameplayTitleKey = 0x8206FFFC;   // "FEMENU_OP_GAMEPLAY_TITLE"
inline constexpr uint32_t kGameplayLabelKeys = 0x82064744;  // 4 x char*: FEMENU_COP_SUBTITLES ...

// Element / text element virtual slots (byte offsets into the vtable)
inline constexpr uint32_t kElemSetVisible = 60;    // (bool)
inline constexpr uint32_t kElemSetTextKey = 132;   // (const char* key) -> Localize -> SetTextRaw
inline constexpr uint32_t kElemSetAlpha = 136;     // (f1 alpha)
inline constexpr uint32_t kElemSetTextRaw = 144;   // (const char* text)
inline constexpr uint32_t kWrapGetWidth = 304;     // instructions box: () -> int
inline constexpr uint32_t kWrapSetTextKey = 360;   // instructions box: (key, width, f1 scale)

// MenuItem virtual slots
inline constexpr uint32_t kItemSetVisible = 24;    // (bool)
inline constexpr uint32_t kItemSetDisabled = 32;   // (bool)
inline constexpr uint32_t kItemSetLabelKey = 48;   // (const char* key)

// Page virtual slots
inline constexpr uint32_t kPageAddItem = 16;       // (index, Element* label, int flags)

// Localization: const char* Localize(StringTable*, const char* key); returns
// the key itself when it is not in the table.
inline constexpr uint32_t kLocalize = 0x8252CB98;
// Language mode: 0 = English, otherwise subtitles are forced on and the
// gameplay page collapses row one.
inline constexpr uint32_t kLanguageMode = 0x82A2A150;

// float constants used by the page
inline constexpr uint32_t kArrowAlphaNormal = 0x82062720;  // 0.5
inline constexpr uint32_t kArrowAlphaFlash = 0x82064200;   // 1.0
inline constexpr uint32_t kArrowFlashTime = 0x82001930;    // 0.1
inline constexpr uint32_t kTextScaleDefault = 0x825D121C;  // -1.0
}  // namespace fe

// ---------------------------------------------------------------------------
// Render resolution (game/render_resolution.cpp)
// ---------------------------------------------------------------------------
// main() asks XGetVideoMode for the display and picks a mode from a table of
// 16-byte entries {width, height, flags, matching display mode}: 5 = 1040x624
// for a widescreen display (shown on 1280x720), 3 = 880x736 for 4:3, and 6 =
// 1280x720 when the `capture_movie` dvar is set. 7 = 1920x1080 is in the
// table but never picked. The chosen entry is copied to kVideoMode; the
// back buffer and every screen-sized render target are made from it.
namespace video {
inline constexpr uint32_t kModeTable = 0x825A2168;
inline constexpr uint32_t kSelectMode = 0x8212FDB8;  // (const u32* wanted, count)
inline constexpr uint32_t kVideoMode = 0x8290F2BC;   // {u32 width, u32 height, u8 .., u8 .., u8 widescreen}
inline constexpr uint32_t kModeId = 0x8290F29C;      // the chosen entry's index
// R_SetMultisample(type): 0 = none, 1 = 2x. Called with 0 when the device is
// made, then by main() with 1 (0 with capture_movie). Refuses MSAA at 1920
// wide (it wouldn't fit the 10 MB EDRAM).
inline constexpr uint32_t kSetMultisample = 0x82153F50;
}  // namespace video

// ---------------------------------------------------------------------------
// Movies (game/cutscene_skip.h). movies/<name>.wmv with <name>-<lang>.wma.
// ---------------------------------------------------------------------------
namespace movie {
inline constexpr uint32_t kStart = 0x824EED20;          // open movies/<name>.wmv and its audio
inline constexpr uint32_t kPlayBlocking = 0x824EF308;   // play until it ends or is skipped (logos, bonus)
inline constexpr uint32_t kFinishLevelMovie = 0x824EEBA0;  // the briefing, after loading (called by SV_SpawnServer)
inline constexpr uint32_t kSkipCheck = 0x824D0470;      // true when the player skips (any button)
inline constexpr uint32_t kStop = 0x824D0528;           // stop and free the movie (draws the loading screen)
inline constexpr uint32_t kHandle = 0x82A2A24C;         // the playing movie object, 0 = none
inline constexpr uint32_t kElapsedMs = 0x82A2A254;      // float: how long the movie has played
inline constexpr uint32_t kMinimumMs = 0x82A2A258;      // float: skippable from (legal 5000; briefing FLT_MAX)
inline constexpr uint32_t kSkipState = 0x82A8DF2C;      // r31 in the skip path: +54 = "skipped", +60 = button seen
// While a level loads, the loader (*kLoader) calls the function at
// +kLoaderScreen now and then: the plain loading screen, or the briefing's
// kLoadingMovie (draw the movie frame, present, advance) once it started.
inline constexpr uint32_t kLoader = 0x82A2AB6C;         // pointer to the zone loader
inline constexpr uint32_t kLoaderScreen = 848;          // loader: loading screen callback
inline constexpr uint32_t kLoadingMovie = 0x824EECF8;   // the briefing as the loading screen
inline constexpr uint32_t kLoadingScreen = 0x82526240;  // the plain loading screen
}  // namespace movie

// ---------------------------------------------------------------------------
// Job system (game/job_wake.cpp). Worker threads run 0x8212A520 and sleep on
// one auto-reset event; queuing a job (0x82129948) wakes them via 0x8212A100.
// ---------------------------------------------------------------------------
namespace jobs {
inline constexpr uint32_t kQueue = 0x8290E930;      // {job pool (56-byte jobs), ..., group table, ...}
inline constexpr uint32_t kWakeEvent = 0x8290E98C;  // HANDLE: the workers' auto-reset event
inline constexpr uint32_t kWake = 0x8212A100;       // PulseEvent(kWakeEvent); hooked
}  // namespace jobs

// ---------------------------------------------------------------------------
// Picture shape (game/ultrawide.cpp)
// ---------------------------------------------------------------------------
// cgame works in a 640x480 virtual screen (glconfig vidWidth/Height 640/480,
// so cg.refdef is 640x480). CG_CalcFov (0x82443A98) treats cg_fov as the 4:3
// horizontal angle, derives fov_y from the 640x480 refdef and, with
// cg_widescreen (vmCvar 0x82A0DED0, from the video mode's widescreen flag),
// recomputes fov_x for 16:9. The renderer projects with fov_y and an aspect
// ratio only (nglBuildMatrices 0x8212C700: yScale = 1/tan(fov_y/2), xScale =
// yScale/aspect, frustum planes from that), so the aspect alone widens the view.
namespace ultrawide {
// Scene setup (sub_82430210, called by RE_RenderScene): aspect = 1.0 for the
// game's square renders, else cg_widescreen ? 16/9 (0x8207CE40) : 4/3
// (0x8207C610), passed in f1 to nglSetAspect (0x8212AEA8 -> list+1036).
inline constexpr uint32_t kSceneSetAspectCall = 0x82430340;  // mid-asm hook
inline constexpr uint32_t kRenderScene = 0x82430710;       // RE_RenderScene(refdef), refexport
inline constexpr uint32_t kSetAspect = 0x8212AEA8;         // nglSetAspect(f1)
inline constexpr uint32_t kSetPerspective = 0x8212AEC8;    // nglSetPerspective(f1 fov_y, f2 near, f3 far)
// 2D: quads and text in virtual coordinates, added to the list being
// recorded (the render job converts them with the list's 2D matrix at
// list+560, built by 0x8212C700 / 0x8212BC10).
inline constexpr uint32_t kListAddQuad = 0x8212AC58;       // nglListAddQuad(quad*): copies 104 bytes
inline constexpr uint32_t kListAddString = 0x82134F70;     // (font, text, f1 x, f2 y, f3 depth, f4 x scale, f5 y scale, r8 color)
inline constexpr uint32_t kQuadVertexStride = 20;          // {x, y, u, v, color} x4: x at +0/+20/+40/+60
// The list being recorded and its target: the 640x480 virtual screen when
// the target's flag 0x2000 is set (the back buffer), else the target's pixels.
inline constexpr uint32_t kCurrentList = 0x8290E9A4;
inline constexpr uint32_t kListTarget = 808;               // texture record*
inline constexpr uint32_t kTargetFlags = 24;
inline constexpr uint32_t kTargetVirtual = 0x2000;
// The movie's full-screen quad (0,0)-(640,480), kept between frames.
inline constexpr uint32_t kMovieQuadPtr = 0x82A2A250;
}  // namespace ultrawide

// ---------------------------------------------------------------------------
// Multiplayer hand-off (game/multiplayer.h)
// ---------------------------------------------------------------------------
namespace mp {
// SV_LoadMpExe_f, the `load_mp_exe` command (name at 0x82069654, registered
// with Cmd_AddServerCommand at 0x82519150). Hooked.
inline constexpr uint32_t kLoadMpExe = 0x825145D8;
inline constexpr uint32_t kMainMenuSelect = 0x824C40B8;  // MainMenu::vfunc_18: item 1 runs load_mp_exe
inline constexpr uint32_t kLaunchNewImage = 0x82343928;  // XLaunchNewImage(path, flags) -> XamLoaderLaunchTitle
inline constexpr uint32_t kMpExePath = 0x8206950C;       // "d:\codmp_xenonf.xex"
}  // namespace mp

// ---------------------------------------------------------------------------
// The game's message box (game/multiplayer.cpp). Every message ("Quit
// without saving?", "Signed out", storage errors) goes through one
// DialogMenuSystem per local player; a message is shown with the steps the
// game's own one-button message (0x823FC150) takes:
//   Close(system); SetMessage(system, key);
//   page = system->pages[system->has_page_two[system->mode]];
//   AddButton(page, "MEM_DIALOG_OK", callback); page->selected = 0;
//   UpdateSelection(system->buttons); Open(page, 1, 0);
// A button's callback gets the player index and returns true to close the
// box; Back (B) calls page->back or, when that is 0, closes the box.
// ---------------------------------------------------------------------------
namespace msgbox {
inline constexpr uint32_t kFrontEnd = 0x82A8DF2C;        // front end state (movie::kSkipState too)
inline constexpr uint32_t kSystems = kFrontEnd + 200;    // DialogMenuSystem* per local player
inline constexpr uint32_t kPlayer = 0x82A2A1C8;          // the player index the game uses (kActiveLocalClient)
inline constexpr uint32_t kSystemVtable = 0x820719D4;    // DialogMenuSystem
inline constexpr uint32_t kPageVtable = 0x820718EC;      // DialogMenu
// DialogMenuSystem fields
inline constexpr uint32_t kSysPages = 4;                 // DialogMenu*[2]
inline constexpr uint32_t kSysHasPageTwo = 12;           // u32[]: which page, indexed by kSysMode
inline constexpr uint32_t kSysButtons = 52;              // the button list
inline constexpr uint32_t kSysMode = 60;                 // u32
// DialogMenu (page) fields
inline constexpr uint32_t kPageSelected = 34;            // u16: the highlighted button
inline constexpr uint32_t kPageBack = 76;                // Back (B) callback, 0 = close the box
// Functions
inline constexpr uint32_t kClose = 0x824C4608;           // (system): closes the box if it is up
inline constexpr uint32_t kSetMessage = 0x824D31E0;      // (system, key): localized; an unknown key shows as is
inline constexpr uint32_t kAddButton = 0x824C4428;       // (page, key, callback)
inline constexpr uint32_t kUpdateSelection = 0x824BDDE8; // (system->buttons)
inline constexpr uint32_t kOpen = 0x824CDB28;            // (page, 1, 0)
inline constexpr uint32_t kCloseCallback = 0x823F86D0;   // `return 1`: the button just closes the box
inline constexpr uint32_t kOkKey = 0x8206FDF8;           // "MEM_DIALOG_OK"
}  // namespace msgbox

// ---------------------------------------------------------------------------
// Changing the resolution while running (game/render_resolution.cpp)
// ---------------------------------------------------------------------------
// The game has no vid_restart, but R_SetMultisample (video::kSetMultisample)
// carries the whole rebuild: it stores the MSAA type in the present parameters
// and calls 0x82130530 = D3DDevice_Reset(device, present params) followed by
// kRebuildScreenTargets, which releases and remakes the two front buffers
// (from video::kVideoMode), the back buffer copy and the two quarter-size
// targets (from kPresentParams). With a new size written to both structs
// first, that is the switch. The quarter targets are remade without releasing
// the old ones (kQuarterTargetA/B + kRecordTexture), so the caller releases
// them. Three more targets are made elsewhere: the full-size depth texture
// (Sys_Main) and, while a level is loaded, the effects' half-size color and
// depth (CG_RegisterGraphics -> 0x824243C8; freed by SV_SpawnServer).
// Rendering runs on a job worker (nglListSend 0x8213DAA0 queues the list for
// kRenderJobGroup), so the switch waits for that job and the GPU first.
namespace live_video {
inline constexpr uint32_t kComFrame = 0x82536DD0;          // the frame boundary; hooked
inline constexpr uint32_t kPresentParams = 0x8290EBC0;     // {u32 width, u32 height, u32 format, .., +16 multisample}
inline constexpr uint32_t kDevice = 0x8290F24C;            // IDirect3DDevice9*
inline constexpr uint32_t kRenderJobGroup = 0x8290F8FC;    // job group the render job runs in
inline constexpr uint32_t kWaitJobGroup = 0x8212A110;      // (group): waits until its jobs are done
inline constexpr uint32_t kUnbindAndIdle = 0x82130CC8;     // unbind streams/textures, D3DDevice_BlockUntilIdle
inline constexpr uint32_t kCurrentList = 0x8290E9A4;       // list being recorded (ultrawide::kCurrentList)
inline constexpr uint32_t kListCursor = 0x8290E9BC;        // the frame's list memory: every command allocates here
// After each present (0x82525190 -> 0x82130980), this flips the double-
// buffered list memory, resets kListCursor and opens the next frame's list
// (0x8212DB18), so a list is open at Com_Frame's start; it is still empty
// when kCurrentList and kListCursor are what this left. Hooked.
inline constexpr uint32_t kBeginNextFrame = 0x82130808;
inline constexpr uint32_t kTargetStackCount = 0x82910B58;  // EDRAM render targets bound right now
inline constexpr uint32_t kRebuildScreenTargets = 0x8213F810;  // after a Reset; via R_SetMultisample
// ngl texture records (44 bytes): {width, height, .., +16 .., +20 IDirect3DTexture9*, +24 flags}
inline constexpr uint32_t kRecordTexture = 20;
inline constexpr uint32_t kFrontBufferA = 0x82911068;      // video mode size, swapped alternately
inline constexpr uint32_t kFrontBufferB = 0x8291103C;
inline constexpr uint32_t kBackBufferCopy = 0x8291100C;    // present params size
inline constexpr uint32_t kQuarterTargetA = 0x82910FB4;    // (width + 3) / 4 x (height + 3) / 4
inline constexpr uint32_t kQuarterTargetB = 0x82910F88;
inline constexpr uint32_t kResourceRelease = 0x822F1340;   // D3DResource_Release(resource)
// nglCreateTexture(flags, format, width, height, r7, 1) -> record; nglReleaseTexture(record)
inline constexpr uint32_t kCreateTexture = 0x8214DDB8;
inline constexpr uint32_t kReleaseTexture = 0x8213F8A0;
inline constexpr uint32_t kFullDepth = 0x82A2ABDC;         // record*: full-size depth texture
inline constexpr uint32_t kDepthInUse = 0x8290A97C;        // record*: the depth texture shaders read now
inline constexpr uint32_t kMakeFullDepth = 0x82419408;     // makes kFullDepth from kVideoMode when it is 0
inline constexpr uint32_t kEffectsColor = 0x82A2ACDC;      // record*: half-size color (0x51, 0x18280186)
inline constexpr uint32_t kEffectsDepth = 0x82A2ACE0;      // record*: half-size depth (0x20, 0x1A220197)
inline constexpr uint32_t kEffectsWidth = 0x82A2ACD0;      // s32: width / 2
inline constexpr uint32_t kEffectsHeight = 0x82A2ACD4;     // s32: height / 2
inline constexpr uint32_t kEffectsColorFlags = 0x51;
inline constexpr uint32_t kEffectsColorFormat = 0x18280186;
inline constexpr uint32_t kEffectsDepthFlags = 0x20;
inline constexpr uint32_t kDepthFormat = 0x1A220197;
inline constexpr uint32_t kGammaDirty = 0x8290F240;        // u8: the present reapplies the gamma ramp
// The level effects run in their own job group (made by CG_RegisterGraphics
// -> 0x824243C8, waited for by 0x82424320 before the effects pass).
inline constexpr uint32_t kEffectsJobGroup = 0x82A11C44;
}  // namespace live_video

// ---------------------------------------------------------------------------
// PC controls for the game's menus (game/pc_menu.h)
// ---------------------------------------------------------------------------
// Every menu is a page (FEMenu) of an FEMenuSystem: FrontEndMenuSystem (title,
// main menu, options, bonus ...), InGameMenuSystem (pause menu, its options,
// mission success/fail, objectives) and one DialogMenuSystem per player (the
// message boxes, msgbox above). Each frame the system's ProcessInput reads
// the pad and hands at most one button event to its HandleButton, which calls
// the page's handler for that button (MoveUp, MoveDown, OnLeft, OnRight,
// Accept -> the page's item switch, Back ...).
namespace pc_menu {
// FEMenuSystem::vfunc_12, ProcessInput(this); hooked.
inline constexpr uint32_t kProcessInput = 0x824C4DB8;
// The game's frame counter: Com_Frame (0x82536DD0) adds one every frame
// (0x82536FA4); the start of a timing struct read all over the game.
inline constexpr uint32_t kFrameCount = 0x825F4E08;
// FEMenuSystem fields and virtual slots (byte offsets into the vtable)
inline constexpr uint32_t kSysPages = 4;          // FEMenu** pages
inline constexpr uint32_t kSysCurrent = 12;       // s32[]: current page per slot, < 0 = none
inline constexpr uint32_t kSysButton = 68;        // HandleButton(this, code, player)
// Release(this, code, player): what ProcessInput sends when a held direction
// is let go; the page (vtbl+152) stops repeating it. HandleButton on a
// direction makes the page repeat it (vtbl+148 keeps the code at page+50,
// FEMenu's update repeats it) until then; a page's Refresh forgets it.
inline constexpr uint32_t kSysRelease = 72;
inline constexpr uint32_t kSysPlayer = 88;        // (this) -> the player the system reads
inline constexpr uint32_t kSysSlot = 92;          // (this) -> index into kSysCurrent
inline constexpr uint32_t kDialogSystemVtable = 0x820719D4;
// Button codes HandleButton takes (0x824BE118 maps them to pad buttons).
inline constexpr uint32_t kCodeBack = 1;
inline constexpr uint32_t kCodeStart = 2;
inline constexpr uint32_t kCodeUp = 4;
inline constexpr uint32_t kCodeDown = 8;
inline constexpr uint32_t kCodeLeft = 16;
inline constexpr uint32_t kCodeRight = 32;
inline constexpr uint32_t kCodeA = 64;
inline constexpr uint32_t kCodeB = 128;
inline constexpr uint32_t kCodeX = 256;
inline constexpr uint32_t kCodeY = 512;
// FEMenu (page) fields and virtual slots
inline constexpr uint32_t kPageItems = 4;         // FEMenuEntry**
inline constexpr uint32_t kPageScrollTop = 32;    // s16: first item shown (scrolling lists)
inline constexpr uint32_t kPageSelected = 34;     // s16
inline constexpr uint32_t kPageCount = 38;        // s16
inline constexpr uint32_t kPageScrollShown = 40;  // s16: items shown at once (scrolling lists)
inline constexpr uint32_t kPageFlags = 42;        // u16: 0x1 = scrolling list
inline constexpr uint32_t kPageMoveUp = 100;
inline constexpr uint32_t kPageMoveDown = 104;
inline constexpr uint32_t kPageOnLeft = 108;
inline constexpr uint32_t kFEMenuOnLeft = 0x820ADAC0;  // the default: the page has no left/right
inline constexpr uint32_t kNullsub = 0x82144CA8;       // an empty method (Options' OnLeft)
inline constexpr uint32_t kTitleMenuVtable = 0x82071724;  // "Press Start": Start, not A
// FEMenuEntry (menu item) fields and virtual slots
inline constexpr uint32_t kEntryLabel = 16;       // the text element showing the item
inline constexpr uint32_t kEntryDisabled = 22;    // u8: skipped by MoveUp / MoveDown
inline constexpr uint32_t kEntrySetVisible = 24;  // (bool): shows / hides the label
inline constexpr uint32_t kEntrySetDisabled = 32; // (bool): also greys the label
inline constexpr uint32_t kEntrySetLabelKey = 48; // (const char* key)
// Text elements (FEText, vtable 0x82070B54; FEMultiLineText derives from it).
// The game draws an item's label with nglListAddString at (x, y) moved by
// its alignment (0x824BF4A8): right (x -= width), left, else centered; bottom
// (y -= height), top, else centered.
inline constexpr uint32_t kTextVtable = 0x82070B54;
inline constexpr uint32_t kMultiLineTextVtable = 0x82070C84;
inline constexpr uint32_t kTextVisibility = 16;   // u8: 0x4 = visible
inline constexpr uint32_t kTextX = 44;            // float, 640x480 menu space
inline constexpr uint32_t kTextY = 48;
inline constexpr uint32_t kTextAlign = 108;       // u16: 16 left, 32 right, 64 top, 128 bottom
inline constexpr uint32_t kTextWidth = 240;       // (this, const float* scale or 0) -> f1
inline constexpr uint32_t kTextHeight = 244;
inline constexpr uint16_t kAlignLeft = 16, kAlignRight = 32, kAlignTop = 64, kAlignBottom = 128;
// FEMenu: nonzero while the highlight animates; the page ignores moves then.
inline constexpr uint32_t kPageLocked = 48;               // u8
inline constexpr uint32_t kPageSelectionChanged = 28;     // vtbl: () after SetSelection
inline constexpr uint32_t kPageSetSelection = 164;        // vtbl: (index, 1)
inline constexpr uint32_t kPageBack = 120;                // vtbl: () what B does on the page
// FEMenuEntry::Draw (vtbl +12) draws its label with this byte as "selected".
inline constexpr uint32_t kEntryDrawnSelected = 21;       // u8
// How FEText::Draw (0x824CFD58) places its text: font index (4 and 5 draw
// nothing), the String* shown, x / y scales for the selected (+56/+60) and
// the normal (+80/+84) state, name String* (FindElement). The fonts are
// g_FEManager +4 (prompts::kFonts).
inline constexpr uint32_t kTextFont = 24;                 // s32
inline constexpr uint32_t kTextString = 28;               // String*: chars at +12
inline constexpr uint32_t kTextScaleSelected = 56;        // float x, float y
inline constexpr uint32_t kTextScaleNormal = 80;          // float x, float y
inline constexpr uint32_t kTextName = 100;                // String*
inline constexpr uint32_t kStringChars = 12;
inline constexpr uint32_t kStringLength = 6;              // u16
// Font object: glyphs of 28 bytes {.., .., width, height, x offset, y offset,
// advance} (s32 each), for characters first .. first + count - 1 (others
// clamp to the ends); a space advances like 'a' (layout 0x821347A8, measure
// 0x82134B90: the width ends at the last glyph's right edge).
inline constexpr uint32_t kFontTexture = 4;               // 0: the font can't draw
inline constexpr uint32_t kFontGlyphs = 8;
inline constexpr uint32_t kFontLineHeight = 28;           // s32
inline constexpr uint32_t kFontFirstChar = 36;            // s32
inline constexpr uint32_t kFontGlyphCount = 40;           // s32
inline constexpr uint32_t kGlyphSize = 28;
// Message boxes: the page's items aren't what is drawn. The box draws the
// button list of its DialogMenuSystem (+52, msgbox::kSysButtons; drawn by
// 0x824C4738): four button texts in fixed slots at +24..+36, the highlighted
// slot at +60, the button count at +56. UpdateSelection (msgbox::
// kUpdateSelection) puts item i in slot 2 for one button, i + 1 for two or
// three (when slots 1 and 2 exist), i for four.
inline constexpr uint32_t kButtonSlots = 24;              // FEText*[4]
inline constexpr uint32_t kButtonCount = 56;              // u32
inline constexpr uint32_t kButtonSelectedSlot = 60;       // u32
// FEMultiLineText (footers, message texts): lines of 32 bytes at +144,
// count +136 (prompts: kLayoutLine). A line: +0 font index, +4 / +8 x / y,
// +16 width (float), +20 String* (its button tokens already replaced by the
// glyph or name), +24 s16 pairs {string index, x offset}, +28 their count:
// entry 2k starts button k, entry 2k + 1 the text after it. Drawn by
// prompts::kDrawLine (this, .., f2 x scale, f3 y scale, ...).
inline constexpr uint32_t kTextLines = 144;
inline constexpr uint32_t kTextLineCount = 136;
inline constexpr uint32_t kLineSize = 32;
inline constexpr uint32_t kLineX = 4;
inline constexpr uint32_t kLineY = 8;
inline constexpr uint32_t kLineWidth = 16;
inline constexpr uint32_t kLineString = 20;
inline constexpr uint32_t kLineSegments = 24;
inline constexpr uint32_t kLineSegmentCount = 28;
// Each menu system's draw (vtbl +56), which draws its page: the footers drawn
// inside belong to that system. Hooked.
inline constexpr uint32_t kFrontEndDraw = 0x824CE4D0;
inline constexpr uint32_t kInGameDraw = 0x824CD9E0;
inline constexpr uint32_t kDialogDraw = 0x824CE3E8;
}  // namespace pc_menu

// ---------------------------------------------------------------------------
// The front end's main menu (game/pc_menu.cpp): MainMenu (vtable 0x82071DDC,
// panel at +60). Init (this, panel) adds five items on mm_text_option_01..05
// (SINGLE PLAYER, MULTIPLAYER, OPTIONS, BONUS MATERIAL, XBOX LIVE
// MARKETPLACE; tables at 0x820646A0 / 0x820646B4) into the five-slot item
// array FEMenu's constructor made, and a footer FEMultiLineText (+56, made
// from mm_text_helpbar). Rows have bands mm_bkg_btn_back_01..05 and lines
// mm_bkg_btn_line_01..04 between them; "text_profile" under the rows says
// SIGNED IN. Update re-enables or greys item 4 (Xbox Live sign-in) every
// frame. The item handler (this, item) stores the item at +64 and does
// nothing for items above 4; it and Back return early while +60 is 0 or +72
// is set. Back (B) goes to the front end's page 0, the "Press Start" title.
// ---------------------------------------------------------------------------
namespace main_menu {
inline constexpr uint32_t kVtable = 0x82071DDC;
inline constexpr uint32_t kInit = 0x824E56B8;               // hooked
inline constexpr uint32_t kUpdate = 0x824DBEE8;             // vtbl[16]; hooked
inline constexpr uint32_t kDraw = 0x824EBF78;               // vtbl[14]: panel, items, footer; hooked
inline constexpr uint32_t kSelect = 0x824C40B8;             // vtbl[18] (this, item); hooked
inline constexpr uint32_t kBack = 0x824BD370;               // vtbl[30]: to the title
inline constexpr uint32_t kPanelDraw = 0x824BFA70;          // (panel): any panel's draw; hooked
inline constexpr uint32_t kPanel = 60;
inline constexpr uint32_t kFooter = 56;                     // FEMultiLineText*
inline constexpr uint32_t kBusy = 72;                       // u8: input ignored while set
inline constexpr uint32_t kLastItem = 64;                   // s32: highlighted when the page opens
inline constexpr int kItemCount = 5;
inline constexpr int kMarketplaceItem = 4;
// Message box buttons the pause menu's quit box uses.
inline constexpr uint32_t kYesKey = 0x8206FE30;             // "MEM_DIALOG_YES"
inline constexpr uint32_t kNoKey = 0x8206FE40;              // "MEM_DIALOG_NO"
inline constexpr uint32_t kNoCallback = 0x824BD750;         // `return 1`: closes the box
}  // namespace main_menu

// ---------------------------------------------------------------------------
// No storage device (game/pc_menu.cpp). The port saves into its userdata
// folder; the SDK's XamShowDeviceSelectorUI picks that "device" at once
// without any UI (the game asks from 0x823F9210, queued by 0x823FDE28).
// What is left is the game's own way to ask again:
// ---------------------------------------------------------------------------
namespace storage {
// Pause menu items (PauseMenu::Init 0x824E6640): 0 Resume, 1 Options,
// 2 Restart Level, 3 Restart Checkpoint, 4 Save / Sign In, 5 Select Storage
// Device (PauseMenu::vfunc_19 case 5 -> 0x823FDE28), 6 Quit. 0x824CD768 runs
// on every refresh and enables or disables items 4 and 5.
inline constexpr uint32_t kPauseInit = 0x824E6640;          // (this, panel); hooked
inline constexpr uint32_t kPauseItemStates = 0x824CD768;    // (this); hooked
inline constexpr int kPauseStorageItem = 5;
inline constexpr int kPauseQuitItem = 6;
inline constexpr uint32_t kPauseQuitKey = 0x8206FD68;       // "PAUSEMENU_QUIT"
// The item handler (PauseMenu::vfunc_19, this, item; a jump table): 1 opens
// Options (InGameMenuSystem vfunc_7 (1, 0)), 5 the device selector, 6 calls
// kPauseQuit. Hooked.
inline constexpr uint32_t kPauseSelect = 0x824F0B50;
// Up / down (vtbl[25] / vtbl[26]): FEMenu's move, then 0x824BD668 wraps
// 0 <-> 6. Hooked (the rows' order on screen differs from the items').
inline constexpr uint32_t kPauseMoveUp = 0x824C4390;
inline constexpr uint32_t kPauseMoveDown = 0x824C4338;
// Quit: the "MEM_QUIT_WARNING" message box with Yes / No; Yes runs
// kPauseQuitYes (back to the main menu). Neither is used anywhere else. The
// Yes callback is hooked.
inline constexpr uint32_t kPauseQuit = 0x824DC7F8;          // (this)
inline constexpr uint32_t kPauseQuitYes = 0x824DC798;       // () -> 1
// Message boxes add "Select (different) Storage Device" buttons with the
// dialog AddButton (msgbox::kAddButton; hooked).
inline constexpr uint32_t kSelectDeviceKey = 0x8207ADA4;    // "FEMENU_XENON_GAMEDESTROY_SELECT"
inline constexpr uint32_t kFailedSelectKey = 0x8207AEBC;    // "FEMENU_XENON_DEVICE_FAILED_SELECT"
// The boxes that have one (0x823FD400, 0x823FE188, ...) preselect it
// (page->selected = its index) before msgbox::kOpen; the Open hook moves the
// highlight onto the last button left. AddButton counts the buttons here:
inline constexpr uint32_t kDialogButtonCount = 56;          // DialogMenu: u32
}  // namespace storage

// ---------------------------------------------------------------------------
// Dynamic shadows (game/shadows.h). Five shadow maps, D24FS8 depth textures
// (ngl texture records), made once at start by kMakeMaps (called from the
// start-up 0x82400098 and the shadow-light set-up 0x8241B9C8; it makes them
// only while slot 4 has none): slot 0 1024x1024, slots 1-4 512x512, each
// nglCreateTexture(kMapFlags, live_video::kDepthFormat, size, size, 0, 1).
// At every frame's start 0x8241B9C8 (via the next-frame callback of
// 0x82130808) clears the slot lights and scenes. Then 0x82430210 (the 3D
// view) calls kCascades when r_testshadow is non-zero: slots 2, 3 and 4
// follow the sun around the camera, 384, 1024 and 2048 units wide
// (orthographic, out to 5000). kAimAtCasters (the frame's end) aims slots 0-4
// at the models that cast (0x824202C0 for slots 0-1, 200 and 330 units wide;
// 0x82429F90 for 2-4). 0x8241FE68 opens a slot's depth-only scene into its
// map (viewport and projection from the map's width and height at record +0
// and +4; the scene first clears depth and stencil, flags 48 at scene +952,
// to 1 minus the 1.0 at scene +956 = 0, the far end); kAddCaster adds a
// model to every slot that has a light this frame.
// The slot lights also carry the sun's light onto the world near the camera
// (with none set, the ground there goes dark). r_testshadow 0 stops only
// kCascades (and the casters of 0x8242E450).
// ---------------------------------------------------------------------------
namespace shadows {
inline constexpr uint32_t kMakeMaps = 0x82419098;     // hooked
inline constexpr uint32_t kFreeMaps = 0x82417230;     // releases all five
inline constexpr uint32_t kCascades = 0x8242BAA0;
inline constexpr uint32_t kAimAtCasters = 0x8242C170;
// (r3 ?, r4 object, ..., r9 flags*, r10 record**) -> 0; hooked. Returns at
// once unless bit kShadowView of the record's 64-bit mask is set, the byte at
// record + (view + 2) * 4 is 0 and kWhiteTexture exists; then clears the
// object's shadow node (+kShadowNode) and adds it to the lit slots.
inline constexpr uint32_t kAddCaster = 0x82425BD0;
inline constexpr uint32_t kShadowView = 0x8290F2B8;   // u32: the view index tested
inline constexpr uint32_t kWhiteTexture = 0x82A2AC6C; // record*: "white", the projected color
inline constexpr uint32_t kShadowNode = 144;
inline constexpr uint32_t kMaps = 0x82A2AC44;         // record*[5]: the maps by slot
inline constexpr uint32_t kSlotLights = 0x82A2AC30;   // light*[5] in use this frame
inline constexpr uint32_t kSlotScenes = 0x82A2AC58;   // the "ShadowMapLight" scenes [5]
inline constexpr int kSlots = 5;
inline constexpr uint32_t kMapFlags = 32;             // nglCreateTexture flags: render target
inline constexpr uint32_t kTestShadowCvar = 0x82A11C4C;  // cvar_t* r_testshadow (R_Register)
}  // namespace shadows

// ---------------------------------------------------------------------------
// Controller buttons in text (game/button_prompts.h). Menus, message boxes
// and in-game hints are FEMultiLineText elements (vtable 0x82070C84). Their
// text names buttons two ways:
//  * "[ACTION]" words ("[HOLD_USE]", "[VERB_AIM]"): set-text runs
//    0x824DE290, which asks 0x824DD9A8 for the pad button bound to the action
//    (the game's own binds, 0x8250AE00) and writes "~square", "Press and hold
//    ~square" (INGAME_PRESS*, table 0x825F51D8: key code, "~name", string).
//  * "~name" tokens, straight from the string tables ("~cross SELECT
//    ~menu_back BACK", "Press ~r1 to fire the mortar.").
// Laying out a line (kLayoutLine) or measuring a word (kMeasureWord), each
// "~name" goes through kTokenGlyph, which returns the token's length and
// stores a one-character string: that character of font 1,
// i_button_icons_xe_<lang> (a 256x128 atlas of 32x32 buttons, characters
// 32-59: '!' RT, '"' A, '#' X, '$' Y, '%' B, '&' LT, '\'' LB, '(' RB,
// ')' up, '*' right, '+' left, ',' down, '-' START, '.' left/right,
// '/' up/down, '0' D-pad, '1' BACK, '2' LS, '3' RS). The token becomes that
// character in the line's text; the line keeps a segment table (u16 start,
// x, end, x end per token) and kDrawLine draws the odd segments with font 1
// at the button scale (+116 of the element) raised by +128, in the button
// color, the rest with the line's font.
// The HUD's own "{BUTTON_1}" path (0x8242A1E0 / 0x82525438 / 0x82522B38:
// BUTTON_1 -> "XBOX_B01" texture names) has no such textures in the game's
// archives and isn't used by single player.
// ---------------------------------------------------------------------------
namespace prompts {
inline constexpr uint32_t kFEManager = 0x82A8DF2C;     // g_FEManager
inline constexpr uint32_t kFonts = kFEManager + 4;     // font*[4]: 0 garamond, 1 buttons,
                                                       // 2 i_helvetica_bold, 3 ..._italic
inline constexpr uint32_t kFontCount = 4;
inline constexpr uint32_t kButtonFont = 1;             // the font index of the buttons
// (text at '~', int* out_string) -> token length (0 = none), *out = the
// button's character as a string; hooked.
inline constexpr uint32_t kTokenGlyph = 0x824C68E0;
// (line, f1 text scale, f2 button scale): splits the line at its tokens and
// measures them with font 1 at f2; hooked.
inline constexpr uint32_t kLayoutLine = 0x824D7650;
// (const char* word, font index, f1 text scale, f2 button scale) -> f1
// width, for word wrap; hooked.
inline constexpr uint32_t kMeasureWord = 0x824D7B48;
// (line, f1 ?, f2 x scale, f3 y scale, f4 button scale, f5 button raise,
// r5 text color, r6 button color); hooked.
inline constexpr uint32_t kDrawLine = 0x824D7D88;
// Line, 32 bytes (array at element + 144, count + 136): +0 font index,
// +4/+8 x/y, +16 width, +20 String*, +24 segments, +28 segment count.
inline constexpr uint32_t kLineFont = 0;
// FEMultiLineText virtual slots (byte offsets), all hooked:
inline constexpr uint32_t kSetText = 144;          // vfunc_36 (this, const char*)
inline constexpr uint32_t kSetWrapped = 364;       // vfunc_91 (this, String**, width)
inline constexpr uint32_t kSetLines = 368;         // vfunc_92 (this, const char*)
inline constexpr uint32_t kSetWrappedRaw = 372;    // vfunc_93 (this, String**, width)
// The game's refcounted string: chars at +12. (String** out, const char*).
inline constexpr uint32_t kStringFromChars = 0x824A5978;
inline constexpr uint32_t kStringChars = 12;
// XInputGetState(user, X_INPUT_STATE*) wrapper the game reads its pads with;
// hooked (only watched).
inline constexpr uint32_t kGetPadState = 0x82344F98;
// Font object (as in the archive): +0x20 s32 the baseline below the top.
inline constexpr uint32_t kFontBaseline = 0x20;
// u32: the local player (pad) the prompts and binds are for; 0 in single
// player. 0x824DD9A8 reads the binds of this player.
inline constexpr uint32_t kClientActiveIndex = 0x82A2A1C8;
// u32 languageMode: 0 English, 1 German, 2 French, 3 Spanish, 4 Italian.
inline constexpr uint32_t kLanguageMode = 0x82A2A150;
// Localize(string tables*, key) -> text (key + "_XBOX360" first; the key
// itself when missing). The tables pointer is at kStringTables.
inline constexpr uint32_t kLocalize = 0x8252CB98;
inline constexpr uint32_t kStringTables = 0x82A2A14C;
inline constexpr uint32_t kPressKey = 0x8206F6C0;      // "INGAME_PRESS"
inline constexpr uint32_t kPullKey = 0x8206F6D0;       // "INGAME_PULL" (triggers)
inline constexpr uint32_t kClickKey = 0x8206F6DC;      // "INGAME_CLICK" (stick buttons)
// (const char* text) -> its "[ACTION]" words expanded, in a static 512-byte
// buffer; hooked.
inline constexpr uint32_t kExpandActions = 0x824DE290;
// ("[ACTION]") -> "~square", "Press and hold ~square", "Use the left
// thumbstick" ([USE_LEFT_STICK], INGAME_XBOX_USE_*_STICK) ...; hooked.
inline constexpr uint32_t kActionText = 0x824DD9A8;
// The title page: TitleMenu::Update (hooked) runs every frame; its "PRESS
// START" is an FEText at +60, set by TitleMenu::Init (0x824D33A8) with
// SetTextKey (virtual +132) to FEMENU_TITLE_PRESS_START_XBOX.
inline constexpr uint32_t kTitleUpdate = 0x824EFB18;
inline constexpr uint32_t kTitleHelpbar = 60;
inline constexpr uint32_t kTitlePressStartKey = 0x8206FAE0;
inline constexpr uint32_t kSetTextKey = 132;           // FEText (this, key)
inline constexpr uint32_t kTextString = 28;            // FEText: String* shown
}  // namespace prompts

// ---------------------------------------------------------------------------
// [crossref 2026-10-13] Cvars and frame timing, from cross-checking the
// CoD3Recomp project (github.com/FrankHUN88453/CoD3Recomp; it has no license,
// so only facts about the game are used here, no code). Every address below
// was checked in this build's code with tools/ppcx.py; see
// dev/agents/cod3recomp-crossref/NOTES.md. Nothing uses these yet.
// ---------------------------------------------------------------------------
namespace crossref {
// cvar_t* pointers the engine keeps (read ->integer at cvar::kInteger).
inline constexpr uint32_t kSvRunningPtr = 0x829BAB18;    // sv_running: 1 while a level runs (Com_Init stores it)
inline constexpr uint32_t kComMaxFpsPtr = 0x829C2514;    // com_maxfps (Com_Frame reads it every frame)
inline constexpr uint32_t kTimescalePtr = 0x829BAB0C;    // timescale (com_timescale)
inline constexpr uint32_t kRLodScalePtr = 0x82A11B5C;    // r_lodscale (R_Register; read by RE_RenderScene):
                                                         // 1 = the game's model LOD distances, 0 = finest model always
// Com_Frame's clock (Q3 com_frameTime): Com_EventLoop's time in ms, stored
// every frame while it waits for com_maxfps; and its static lastTime.
inline constexpr uint32_t kComFrameTime = 0x829C2518;
inline constexpr uint32_t kComFrameLastTime = 0x82BE3CA8;
// The console command buffer (Q3 cmd_text). Cbuf_AddText drops the whole
// text when cursize + length >= maxsize; maxsize is 0 until Cbuf_Init.
inline constexpr uint32_t kCmdText = 0x829F1BB8;
inline constexpr uint32_t kCmdTextData = 0;              // u8*
inline constexpr uint32_t kCmdTextMaxSize = 4;           // s32
inline constexpr uint32_t kCmdTextCurSize = 8;           // s32
// The cgame and game cvar tables (registered by CG_RegisterCvars 0x8243C7C8
// and G_RegisterCvars 0x82549B00).
inline constexpr uint32_t kCgCvarTable = 0x825CDBF0;     // 147 x {vmCvar_t*, name, default, flags}
inline constexpr uint32_t kCgCvarCount = 147;
inline constexpr uint32_t kCgCvarEntrySize = 16;
inline constexpr uint32_t kGameCvarTable = 0x825F3940;   // 85 x {vmCvar_t*, name, default, flags, modCount, trackChange}
inline constexpr uint32_t kGameCvarCount = 85;
inline constexpr uint32_t kGameCvarEntrySize = 24;
// cg_subtitles: cgame table entry 89 (0x825CE180), default "1", archive.
// No code reads its vmCvar in this build; the subtitles follow the profile's
// controls::kSubtitles byte (Options > Gameplay) and the language.
inline constexpr uint32_t kCgSubtitlesVmCvar = 0x82A0DF60;
// (World at War's aim_slowdown_enabled / aim_lockon_enabled /
// aim_autoaim_enabled don't exist in CoD3: setting them creates unused
// dvars. The game's aim assist is controls::kStickyAim.)

// Vertical blank and swap pacing, inside the game's Direct3D (the device is
// *(*0x82001144), VdGlobalDevice). The interrupt handler (0x822F1918) counts
// each vblank and counts the swap countdown down; at 0 it clears the swap
// flag (word +4 of the block at device + kD3DInterruptBlock), which the
// command stream waits on after every swap. The swap's own interrupt
// (0x822F4A10) sets the countdown from the presentation interval minus the
// vblanks since the last flip. So frames come at most once per vblank.
inline constexpr uint32_t kD3DInterruptBlock = 10772;    // device: u32 pointer; block +4 = swap flag
inline constexpr uint32_t kD3DVBlankCallback = 15120;    // device: set by D3DDevice_SetVerticalBlankCallback 0x822EC358
inline constexpr uint32_t kD3DVBlankCount = 15124;       // device: u32
inline constexpr uint32_t kD3DLastFlipVBlank = 15128;    // device: u32, vblank count at the last flip
inline constexpr uint32_t kD3DSwapCountdown = 15132;     // device: s32
inline constexpr uint32_t kD3DSwapCount = 15136;         // device: u32
// The game's vblank callback (0x82130100) counts vblanks here; the frame's
// start (live_video::kBeginNextFrame) copies the count. A render list given
// no time of its own (list +1052 = 0; RE_BeginFrame sets one through
// 0x820CF548 / 0x8212AE70 in a level) takes count x 16.67 ms (20 ms in 50 Hz
// modes) as its time (0x8214F2A0, list +1056), so that time follows the
// guest vblank rate.
inline constexpr uint32_t kVBlankCounter = 0x8290F290;   // u32
inline constexpr uint32_t kFrameStartVBlanks = 0x8290F294;  // u32
inline constexpr uint32_t kListTime = 1052;              // render list: float seconds, 0 = none set
inline constexpr uint32_t kListTimeUsed = 1056;          // render list: float seconds the shaders get
}  // namespace crossref

// ---------------------------------------------------------------------------
// [menus-brightness] The game's brightness: one float, 0.5 to 2.0, 1.0 by
// default. SCR_UpdateScreen (0x82511168, at 0x825111E0) calls 0x82419228,
// which rebuilds the gamma ramp whenever the value differs from the last one
// it applied (0x825CDAA8). Options > SCREEN (GammaScreenMenu, vtable
// 0x82072434, panel SP_OP_screen_gamma: gauge_fill +64, gauge_arrow_left
// +68, gauge_arrow_right +72) moves it in its draw (0x824CC278): by the left
// stick (stick x 1/16384 a frame) and by the D-pad (0.05 a press, pad
// buttons 0 / 2 through 0x820E9038); its OnLeft / OnRight only flash the
// arrows, so the PC menus' left / right (pc_menu.cpp) did nothing there. On
// leaving (vtbl[22] 0x824C3300) a value changed since the page opened goes
// into the profile (+400) and the profile is marked to be saved; signing in
// applies the profile's value (0x823F8990).
// ---------------------------------------------------------------------------
namespace brightness {
inline constexpr uint32_t kValue = 0x825CDAA4;        // float: the brightness in use
inline constexpr float kMin = 0.5f;                   // both clamps use 0x82062720 (0.5)
inline constexpr uint32_t kMax = 0x825CE628;          // float: the clamps' maximum (2.0)
inline constexpr uint32_t kStep = 0x826A13CC;         // float: one D-pad press (0.05)
inline constexpr uint32_t kProfile = 0x82A8E610;      // the player's options (664 bytes)
inline constexpr uint32_t kProfileValue = 400;        // float: the saved brightness
inline constexpr uint32_t kPageVtable = 0x82072434;   // GammaScreenMenu
inline constexpr uint32_t kPageOnLeft = 0x824BC2C0;   // vtbl[27] (this); hooked
inline constexpr uint32_t kPageOnRight = 0x824BC248;  // vtbl[28] (this); hooked
// ControlsPage_Apply (AdvancedOptionsMenu, pause > Options): swaps the page's
// values with the profile's. B (0x824F07B0) copies the profile, swaps,
// compares, swaps back; a difference opens "save changes?" (MEM_CONFIRM_SAVE,
// 0x824F0608), whose Yes (0x824F04D0) swaps and saves; hooked.
inline constexpr uint32_t kAdvSwapProfile = 0x824E5DF8;
// A gauge picture's fill, as the pages set it (0x824DC108): (part, f1 = 0..1,
// f3 = the picture's +52), from the part's left edge.
inline constexpr uint32_t kSetFill = 0x824C0588;
inline constexpr uint32_t kQuadFillScale = 52;        // float
}  // namespace brightness

// ---------------------------------------------------------------------------
// [keybinds] Keyboard and mouse keys that call the game's own actions
// (game/keybinds/binds.cpp, both programs; cod3mp's values come from
// mp/config/campaign_map.tsv). The pad poll, sub_82522138, runs once before
// every frame; it takes presses only while in_joystick is on and no menu
// blocks the player. The actions' handlers aren't here: binds.cpp finds them
// by name in the game's command list (kCmdFunctions).
// ---------------------------------------------------------------------------
namespace keybinds {
inline constexpr uint32_t kSysMilliseconds = 0x82468510;   // int Sys_Milliseconds(void)
inline constexpr uint32_t kInJoystick = 0x829C295C;        // dvar_t* in_joystick; int value at +32
inline constexpr uint32_t kPadPollOff = 0x82A2A17D;        // byte: the pad poll does nothing (campaign)
inline constexpr uint32_t kKeys = 0x82A59570;              // qkey_t keys[local client][256]: +4 char* binding
inline constexpr uint32_t kKeysStride = 2052;              // bytes per local client
inline constexpr uint32_t kInput = 0x8290A910;             // the input object*: +28 a pad pressed Start, +24 which
inline constexpr uint32_t kLocalClients = 0x82A98CD0;      // the local clients: +180 the client's controller
inline constexpr uint32_t kLocalClientPad = 180;
inline constexpr uint32_t kLocalClientState = 0;           // cod3mp: +184, 2 = in a match (else presses are menu keys)
inline constexpr uint32_t kLocalClientStride = 188;
inline constexpr uint32_t kKbuttons = 0x829C8E68;          // kbutton_t[] {down[2], downtime, msec, active, wasPressed}
inline constexpr uint32_t kActivateReloadButton = 456;     // +activatereload's kbutton (the frag throw-back)
inline constexpr uint32_t kSavedKbuttons = 0;              // cod3mp: the kbuttons of each local client (0 = one set)
inline constexpr uint32_t kSavedKbuttonsSize = 696;        // cod3mp: bytes per local client
inline constexpr uint32_t kPadSkip = 0;                    // cod3mp: byte per pad, set = the poll skips its client
inline constexpr uint32_t kFeBlock = 32;                   // campaign: *(g_FEManager + 32): +28 + cl blocks, +12 == 3 lets through
inline constexpr uint32_t kFeInputOn = 0;                  // cod3mp: byte g_FEManager + 50, 0 = no pad input
inline constexpr uint32_t kFePerClient = 0;                // cod3mp: object* at g_FEManager + 208 + cl * 4
inline constexpr uint32_t kFeBlockedVfunc = 0;             // cod3mp: that object's vfunc (this) -> u8, non-zero = blocked
}  // namespace keybinds

}  // namespace cod3::addr

#endif  // COD3_MP
