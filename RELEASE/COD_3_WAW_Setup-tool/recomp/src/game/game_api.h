// cod3 - typed calls into the game's own functions.
//
// Port code that calls a game function goes through these wrappers instead
// of a raw address: each wrapper names the game function, takes typed
// arguments and takes the address from addresses.h, so the address lives in
// one place. docs/FUNCTIONS.md lists every known game function, with these
// wrappers (made by tools/catalog_functions.py, which reads the comments
// below: "<game function> 0x<address>: <guest signature>. <what it does>";
// a function without a name yet goes by its addresses.h constant).
//
// Rules for every wrapper:
//  - Game main thread only: from a hook, or from a task queued with
//    Engine::QueueGuestTask, with that hook's ctx and base.
//  - The call goes through the port's own hook of that function, if there is
//    one, exactly like the game's own calls do.
//  - Host strings are copied onto the guest stack for the call; a returned
//    guest pointer stays the game's.
//  - Each call runs on its own copy of the registers (guest::Call), so the
//    calling hook's registers are left as they were.

#pragma once

#include <cstdint>
#include <string>
#include <string_view>

struct PPCContext;

namespace cod3::game {

// ---------------------------------------------------------------------------
// Dvars (the game's Q3 cvars)
// ---------------------------------------------------------------------------
// Cvar_FindVar 0x82529790: cvar_t* (const char* name). The dvar, or 0.
uint32_t CvarFind(PPCContext& ctx, uint8_t* base, std::string_view name);
// Cvar_FindVar 0x82529790: cvar_t* (const char* name). The same, for a name
// already in guest memory (a hook's argument); nothing is copied.
uint32_t CvarFindGuest(PPCContext& ctx, uint8_t* base, uint32_t guest_name);
// Cvar_Get 0x8252BBE0: cvar_t* (const char* name, const char* value, int flags).
// Registers the dvar with this default (or returns the one that exists).
uint32_t CvarGet(PPCContext& ctx, uint8_t* base, std::string_view name, std::string_view value,
                 uint32_t flags);
// Cvar_Set2 0x8252B850: cvar_t* (const char* name, const char* value, qboolean force).
// The game's own write with its checks (force skips them). Port code writes
// dvars with Engine::CvarSet, which decides when force is allowed
// (game/cheat_guard.h) and then calls this.
void CvarSet2(PPCContext& ctx, uint8_t* base, std::string_view name, std::string_view value,
              bool force);

// Reading a dvar (a cvar_t* from the calls above); plain memory reads.
std::string CvarName(uint8_t* base, uint32_t cvar);
std::string CvarString(uint8_t* base, uint32_t cvar, size_t max_len = 256);
std::string CvarResetString(uint8_t* base, uint32_t cvar, size_t max_len = 256);
float CvarValue(uint8_t* base, uint32_t cvar);
int32_t CvarInteger(uint8_t* base, uint32_t cvar);
uint32_t CvarFlags(uint8_t* base, uint32_t cvar);

// ---------------------------------------------------------------------------
// Console text, printing, localized text
// ---------------------------------------------------------------------------
// Cbuf_AddText 0x824644D0: void (const char* text). Appends console text; it
// runs at the next Cbuf_Execute (once a frame). End each command with '\n'.
// Only for real console text: a dvar is set directly with Engine::CvarSet.
void CbufAddText(PPCContext& ctx, uint8_t* base, std::string_view text);
// Com_Printf 0x82523A58: void (const char* fmt, ...). Empty in the retail
// game; the port's hook (game/print_hooks.cpp) shows the line in the console
// and the log as a game line. The text is passed through "%s".
void ComPrintf(PPCContext& ctx, uint8_t* base, std::string_view text);
// Localize 0x8252CB98: const char* (StringTable* tables, const char* key).
// The text of a string-table key in the game's language; empty when the key
// isn't there (the game itself would show the key).
std::string Localize(PPCContext& ctx, uint8_t* base, std::string_view key, size_t max_len = 512);

// ---------------------------------------------------------------------------
// Menus (the front end's FEMenu pages and their panels)
// ---------------------------------------------------------------------------
// Panel::FindElement 0x824D07D0: Element* (Panel* panel, const char* name).
// A panel's element by name, or 0.
uint32_t PanelFindElement(PPCContext& ctx, uint8_t* base, uint32_t panel, std::string_view name);
// fe::kPanelFindQuad 0x824DFC70: PanelQuad* (Panel* panel, const char* name).
// A panel's picture by name, or 0 (the game hands back a shared stand-in for
// a missing name; that is checked here).
uint32_t PanelFindQuad(PPCContext& ctx, uint8_t* base, uint32_t panel, std::string_view name);
// operator_new 0x820CC1D0: void* (size_t size). Game heap memory.
uint32_t OperatorNew(PPCContext& ctx, uint8_t* base, uint32_t size);
// operator_delete 0x820CC468: void (void* memory).
void OperatorDelete(PPCContext& ctx, uint8_t* base, uint32_t memory);
// FEText::ctor 0x820ACA38 and PanelQuad::ctor 0x824C8498: a copy of a panel
// text or picture, made the way the game copies elements (0x824E1588):
// operator new, the class's constructor, then Assign(source) (vtable +72).
// 0 when the source or the memory isn't valid.
uint32_t CopyElement(PPCContext& ctx, uint8_t* base, uint32_t source, bool quad);
// Element virtual calls (FEText and the panel's other elements; slots in
// addresses.h fe::kElemSet*).
void ElementSetVisible(PPCContext& ctx, uint8_t* base, uint32_t element, bool visible);
void ElementSetAlpha(PPCContext& ctx, uint8_t* base, uint32_t element, float alpha);
void ElementSetText(PPCContext& ctx, uint8_t* base, uint32_t element, std::string_view text);
void ElementSetTextKey(PPCContext& ctx, uint8_t* base, uint32_t element, std::string_view key);
// FEMenuEntry (menu item) virtual calls (fe::kItemSet*).
void ItemSetVisible(PPCContext& ctx, uint8_t* base, uint32_t item, bool visible);
void ItemSetDisabled(PPCContext& ctx, uint8_t* base, uint32_t item, bool disabled);
void ItemSetLabelKey(PPCContext& ctx, uint8_t* base, uint32_t item, std::string_view key);

// The game's message box (addresses.h msgbox), one wrapper per step; a box
// is shown with the steps the game's own one-button message (0x823FC150)
// takes: Close, SetMessage, AddButton per button, UpdateSelection, Open.
// msgbox::kClose 0x824C4608: void (DialogMenuSystem* system). Closes the box if it is up.
void MessageBoxClose(PPCContext& ctx, uint8_t* base, uint32_t system);
// msgbox::kSetMessage 0x824D31E0: void (DialogMenuSystem* system, const char* key).
// Localized; an unknown key shows as it is. `text` is a guest string that stays valid.
void MessageBoxSetMessage(PPCContext& ctx, uint8_t* base, uint32_t system, uint32_t text);
// msgbox::kAddButton 0x824C4428: void (DialogMenu* page, const char* key, callback).
// The callback gets the player index and returns true to close the box.
void MessageBoxAddButton(PPCContext& ctx, uint8_t* base, uint32_t page, uint32_t key, uint32_t callback);
// msgbox::kUpdateSelection 0x824BDDE8: void (buttons, int selected). Puts the
// highlight on button `selected` (system->buttons, msgbox::kSysButtons).
void MessageBoxUpdateSelection(PPCContext& ctx, uint8_t* base, uint32_t buttons, uint32_t selected);
// msgbox::kOpen 0x824CDB28: void (DialogMenu* page, 1, 0). Shows the box.
void MessageBoxOpen(PPCContext& ctx, uint8_t* base, uint32_t page);

// ---------------------------------------------------------------------------
// Movies
// ---------------------------------------------------------------------------
// movie::kStop 0x824D0528: void (). Stops and frees the playing movie and its
// sound, and draws the loading screen. Reads no argument.
void MovieStop(PPCContext& ctx, uint8_t* base);

// ---------------------------------------------------------------------------
// Rendering: the calls the live resolution switch makes
// (game/render_resolution.cpp; addresses.h live_video)
// ---------------------------------------------------------------------------
// R_SetMultisample 0x82153F50: void (int type). 0 none, 1 2x. Resets the device
// with the present parameters and remakes the screen-sized targets.
void SetMultisample(PPCContext& ctx, uint8_t* base, uint32_t type);
// live_video::kWaitJobGroup 0x8212A110: void (group). Waits until the group's jobs are done.
void WaitJobGroup(PPCContext& ctx, uint8_t* base, uint32_t group);
// live_video::kUnbindAndIdle 0x82130CC8: void (). Unbinds streams and textures, then
// D3DDevice_BlockUntilIdle.
void UnbindAndIdle(PPCContext& ctx, uint8_t* base);
// D3DResource_Release 0x822F1340: void (IDirect3DResource9* resource).
void ResourceRelease(PPCContext& ctx, uint8_t* base, uint32_t resource);
// nglCreateTexture 0x8214DDB8: record* (flags, format, width, height, r7, 1).
// An ngl texture record (44 bytes; IDirect3DTexture9* at +20).
uint32_t CreateTexture(PPCContext& ctx, uint8_t* base, uint32_t flags, uint32_t format, uint32_t width,
                       uint32_t height, uint32_t r7);
// nglReleaseTexture 0x8213F8A0: void (record*).
void ReleaseTexture(PPCContext& ctx, uint8_t* base, uint32_t record);
// live_video::kMakeFullDepth 0x82419408: void (). Makes the full-size depth texture from
// the video mode when there is none.
void MakeFullDepth(PPCContext& ctx, uint8_t* base);

}  // namespace cod3::game
