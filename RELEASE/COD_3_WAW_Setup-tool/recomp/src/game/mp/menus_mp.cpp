// cod3mp: the multiplayer program's own menus (codmp_xenonf.xex addresses).
//
// The mouse and keyboard in the menus are game/pc_menu.cpp, the campaign's,
// compiled into this program too (its ProcessInput, draw and picture hooks
// bind to multiplayer's functions through mp_aliases.h). What is
// multiplayer's own is here:
//  - whether a menu page is on screen, so raw mouse aim lets the pointer go
//    in the pause, side selection and scoreboard pages (MenuActive): a page
//    was drawn in this game frame or the last one (the game's frame counter,
//    as game/pc_menu.cpp counts it), so it holds at any frame rate;
//  - the after-action report's draw (a fourth kind of menu system the
//    campaign doesn't have): what it draws belongs to it for the mouse;
//  - the main menu: XBOX LIVE does nothing (there is no Live service for the
//    PC version; A there stopped on a null pointer in the session code) and
//    says so; DOWNLOAD CONTENT becomes QUIT TO DESKTOP;
//  - the pause menu in a match and after a match: a seventh row, QUIT TO
//    DESKTOP, under QUIT;
//  - QUIT TO DESKTOP asks first in the game's own Yes / No message box, like
//    the campaign (No, B or Escape close only the box); Yes saves the
//    settings and ends the program like the window's close button (in a
//    match the other players see this player drop; see kQuitYesCallback);
//  - message boxes: no "Select Storage Device" button is added (pc_menu.cpp);
//    a box that would have highlighted it highlights its last button;
//  - the texts of Options > Gameplay's FIELD OF VIEW and FOV SCALE rows
//    (the rows are game/mp/options_mp.cpp);
//  - a warning when the game's voice and session system didn't start: it
//    starts with the sound system, and without an audio output device
//    creating or joining a match stops the game (a null read below MPPeer's
//    constructor, in 0x8229B420).

#include "game/mp/menus_mp.h"

#include <algorithm>
#include <cctype>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>

#include "game/addresses.h"
#include "game/guest.h"
#include "game/pc_menu.h"
#include "ui/announcement_feed.h"

REXCVAR_DECLARE(bool, cod3_pc_menus);

namespace {

using cod3::guest::Call;
using cod3::guest::ReadU16;
using cod3::guest::ReadU32;
using cod3::guest::ReadU8;
using cod3::guest::VirtualSlot;
using cod3::guest::WriteU32;
using Clock = std::chrono::steady_clock;

// FEMenuSystem (codmp_xenonf.xex; the campaign's keeps an array of pages
// per slot): pages at +4, the page taking the input at +28 and a page under
// an overlay at +20 (< 0: none), as its draw (FEMenuSystem__vfunc_19,
// 0x824D5ED0) reads them.
constexpr uint32_t kSysPages = 4;
constexpr uint32_t kSysUnder = 20;
constexpr uint32_t kSysCurrent = 28;

// MPMainMenuXBox's item handler (slot 36, 0x823D2978) switches on +244 plus
// +248 (its list box's top row and cursor): 0 XBOX LIVE, 1 SYSTEM LINK,
// 2 LOCAL SPLIT SCREEN, 3 OPTIONS, 4 DOWNLOAD CONTENT. Its labels come from
// the key table at 0x825D4738 and are localized when the page is built
// (sub_823C9A90 calls Localize).
constexpr uint32_t kMainMenuRow = 244;
constexpr uint32_t kMainMenuRowOffset = 248;
constexpr uint32_t kXboxLiveItem = 0;
constexpr uint32_t kQuitItem = 4;  // DOWNLOAD CONTENT on the console

// ---------------------------------------------------------------------------
// Message boxes (DialogMenuSystem, one per local player at FEManager +192):
// the steps of the game's own Yes / No boxes (the pause menu's QUIT,
// 0x823B14E0; the main menu's, 0x823D2E88):
//   Close(system); SetMessage(system, text, 0, 0, title, 1);
//   page = system->pages[system->vfunc96() != 0];   (+28, the box's page)
//   AddButton(page, YES, yes); AddButton(page, NO, no);
//   UpdateSelection(system, 1); Open(system, 1)
// A button's callback gets the player and returns true to close the box;
// the box calls it without a null check. B calls the page's Back (+96):
// SetMessage leaves it at 0, which in a match closed the pause menu under
// the box too, so ours gets the pause QUIT box's own Back (0x823952F8: close
// the player's message box only).
// ---------------------------------------------------------------------------
constexpr uint32_t kFEManager = 0x82C6B1FC;
constexpr uint32_t kDialogSystems = kFEManager + 192;
constexpr uint32_t kActiveClient = 0x82BCBE9C;     // the game's current local client
constexpr uint32_t kDialogSystemVtable = 0x82071614;
constexpr uint32_t kDialogPageVtable = 0x82071504;
constexpr uint32_t kSysPageIndex = 96;              // vtbl: () -> +28
constexpr uint32_t kDialogClose = 0x824DC448;       // (system)
constexpr uint32_t kDialogSetMessage = 0x8250C098;  // (system, text, 0, 0, title, 1)
constexpr uint32_t kDialogAddButton = 0x824DBC50;   // (page, key, callback)
constexpr uint32_t kDialogSelect = 0x824DC570;      // (system, button)
constexpr uint32_t kDialogOpen = 0x820B4B10;        // (system, 1); hooked below
constexpr uint32_t kDialogPageSelected = 34;        // s16 (DialogMenu)
constexpr uint32_t kDialogPageButtons = 76;         // u32
constexpr uint32_t kDialogPageBack = 96;            // callback(player), 0 = the default
constexpr uint32_t kDialogCloseBack = 0x823952F8;   // (player): Close(the player's box)
// Buttons: the front end's and the match's texts.
constexpr uint32_t kFrontEndYesKey = 0x820700CC;  // "MEM_DIALOG_YES"
constexpr uint32_t kFrontEndNoKey = 0x820700DC;   // "MEM_DIALOG_NO"
constexpr uint32_t kMatchYesKey = 0x8206E494;     // "INGAME_DIALOG_YES"
constexpr uint32_t kMatchNoKey = 0x8206E4A8;      // "INGAME_DIALOG_NO"
// The box's Yes: a `return 1` callback only the Live invitation box uses
// (sub_82415C00's No, 0x82415CA4; there is no Live here), hooked below; while
// the QUIT TO DESKTOP box is up it quits. No: the one-button messages' OK
// (also `return 1`), which just closes the box.
constexpr uint32_t kQuitYesCallback = 0x82412170;
constexpr uint32_t kQuitNoCallback = 0x82412288;
// QUIT TO DESKTOP in a match doesn't go through the pause menu's own QUIT
// (Yes 0x823A5440): that leave runs later in a script thread (0x823FBA08),
// which in a match started from the console (`map`, no session object at
// *0x82BCCAC0) reads through a null pointer (claude-setup run mpm4, the
// game's own QUIT with the pad). Quitting ends the program like the window's
// close button; the other players see this player drop.
// The after-match pause menu's player: the game's input object (+24 the
// controller that pressed), and its local client (-1 none), as its QUIT
// (0x823B1170) finds them.
constexpr uint32_t kInputObject = 0x820F9CA8;     // () -> input
constexpr uint32_t kInputActivePad = 24;
constexpr uint32_t kClientForPad = 0x82521BD0;    // (pad) -> local client or -1

// ---------------------------------------------------------------------------
// The pause menus' QUIT TO DESKTOP row. PauseMenu (in a match; one per local
// player, InGameMenuSystem page 0) has six items, 0 SELECT WEAPON ...
// 5 QUIT, in two sets: the full-screen panel's (+88) and the split-screen
// panel's (+96), counts +94 / +102 (s16; capacities +92 / +100); Refresh
// copies the set the screen uses into the items (+4, count +38) (0x82392D10).
// Init runs once per panel (0x8239D920 -> 0x8239D6B0 full screen, 0x8239D5C0
// split screen); each adds its six items on its rows and links item 0's up
// to 5 and 5's down to 0. Both panels have unused rows under the sixth
// (full screen slot_07_text_option at y 271, split screen text_option_07).
// The arrays only hold six: they become seven-slot arrays, then a seventh
// item goes on row seven of each set, its links wrap through it, and its A
// opens the QUIT TO DESKTOP box (the page's A, slot 36, 0x823B1EA0, does
// nothing above item 5). AARPauseMenu (after a match; a plain FEMenu: items
// +4, count +38) the same way on its one panel (Init 0x82397B90, A
// 0x823B1DE0, QUIT item 5).
// ---------------------------------------------------------------------------
constexpr uint32_t kPauseVtable = 0x82083934;
constexpr uint32_t kAarPauseVtable = 0x82083A3C;
constexpr uint32_t kPageItems = 4;
constexpr uint32_t kPageSelected = 34;            // s16
constexpr uint32_t kPageCount = 38;               // s16
constexpr uint32_t kPanelName = 29;              // char[]: the panel file's name
constexpr uint32_t kPagePlayer = 80;              // PauseMenu: its local client
constexpr uint32_t kSetFull = 88, kSetSplit = 96;  // {FEMenuEntry** items; s16 capacity; s16 count}
constexpr uint32_t kSetCapacity = 4, kSetCount = 6;
constexpr uint32_t kPageAddItem = 16;             // vtbl: (index, label, flags)
constexpr uint32_t kEntryUp = 8, kEntryDown = 10;  // s16 links
constexpr uint32_t kEntrySetVisible = 56;         // vtbl: (bool)
constexpr uint32_t kEntrySetDisabled = 64;        // vtbl: (bool)
constexpr uint32_t kOperatorNew = 0x820DE6D8;     // (size)
constexpr int kPauseItems = 6;
constexpr int kQuitToDesktopItem = 6;

// The voice / session singleton MPPeer's constructor reads (sub_8229B420:
// lwz r3,60 of it). It is made by an init the sound system runs (FMOD's
// init, 0x8238D990, after its own device check); without an audio device
// it stays null.
constexpr uint32_t kVoiceSystemPtr = 0x82A9ACB8;

// While the game thread is busy (a loading screen) the frame counter stands
// still: a page drawn longer ago than this no longer counts.
constexpr int64_t kLoadingLimitNs = 20'000'000'000;
constexpr int64_t kVoiceCheckNs = 15'000'000'000;  // after the first menu, then the check

std::atomic<int64_t> g_menu_ns{0};
std::atomic<uint32_t> g_menu_frame{0};  // the game frame of that draw
std::atomic<int64_t> g_first_menu_ns{0};
bool g_voice_checked = false;  // UI thread

// The QUIT TO DESKTOP box (game thread): on screen for this player, opened
// from a match or the main menu. Any other box opening clears it.
struct QuitBox {
  bool up = false;
  bool in_match = false;
  uint32_t player = 0;
};
QuitBox g_quit_box;
bool g_opening_quit_box = false;

bool Valid(uint32_t p) { return p >= 0x10000 && p < 0xC0000000u && (p & 3) == 0; }

int64_t NowNs() { return Clock::now().time_since_epoch().count(); }

uint8_t* GuestBase() {
  auto* ks = rex::system::kernel_state();
  return ks && ks->memory() ? ks->memory()->virtual_membase() : nullptr;
}

void WriteU16(uint8_t* base, uint32_t addr, uint16_t value) {  // big-endian like all guest data
  base[addr] = uint8_t(value >> 8);
  base[addr + 1] = uint8_t(value);
}

// The port's texts for the game's keys and its own, as guest strings made
// once. The main menu's changed items keep the game's keys.
struct PortText {
  const char* key;
  const char* text;
  std::atomic<uint32_t> guest{0};
};
PortText g_texts[] = {
    {"MPFRONTEND_DOWNLOAD_CONTENT", "QUIT TO DESKTOP"},
    {"MPFRONTEND_DOWNLOAD_CONTENT_DESCRIPTION", "Close Call of Duty 3 and return to the desktop."},
    {"MPFRONTEND_XBOXLIVE_DESCRIPTION",
     "Xbox Live isn't part of the PC version: play over System Link on your network."},
    {"COD3_QUIT_TO_DESKTOP", "QUIT TO DESKTOP"},
    {"COD3_QUIT_TO_DESKTOP_QUESTION", "Quit the game and go back to the desktop?"},
    {"COD3_QUIT_MATCH_TO_DESKTOP_QUESTION", "Leave the match and quit to the desktop?"},
    {"COD3_EMPTY", ""},
    // Options > Gameplay's FIELD OF VIEW and FOV SCALE rows (game/mp/options_mp.cpp),
    // the campaign's keys (game/menu_fov.cpp).
    {"FEMENU_COP_FOV", "FIELD OF VIEW"},
    {"FEMENU_COP_FOV_SCALE", "FOV SCALE"},
    {"FEMENU_GAMEPLAY_INST_FOV",
     "Adjust the field of view (cg_fov). 80 is the original console view. With the FOV scale it stays within "
     "65 to 165."},
    {"FEMENU_GAMEPLAY_INST_FOV_SCALE",
     "Multiply the field of view (cg_fovScale), World at War style: 1.20 turns 80 into 96. Aiming down the "
     "sights is scaled the same way."},
    // [claude-9b] The game's sensitivity rows are the controller's (owner 2026-10-26: "Horizontal and
    // vertical sensitivity sliders should only effect controller's sense"; game/mouse_look.cpp), the
    // campaign's keys (game/menu_aim.cpp kGameTexts). The mouse's is F2 > Controls.
    {"FEMENU_COP_HORIZONTALSENS", "HORIZ. SENS. (CONTROLLER)"},
    {"FEMENU_COP_HORIZ_SENS", "HORIZ. SENS. (CONTROLLER)"},
    {"FEMENU_COP_VERTICALSENS", "VERT. SENS. (CONTROLLER)"},
    {"FEMENU_COP_VERT_SENS", "VERT. SENS. (CONTROLLER)"},
    {"FEMENU_CONTROLS_INST_HORIZONTALSENS", "Controller only. The mouse has its own sensitivity in F2 > Controls."},
    {"FEMENU_CONTROLS_INST_VERTICALSENS", "Controller only. The mouse has its own sensitivity in F2 > Controls."},
};

uint32_t GuestString(uint8_t* base, const char* text) {
  auto* mem = rex::system::kernel_state()->memory();
  const uint32_t len = static_cast<uint32_t>(std::strlen(text)) + 1;
  const uint32_t addr = mem->SystemHeapAlloc(len);
  if (addr) std::memcpy(base + addr, text, len);
  return addr;
}

uint32_t PortTextFor(uint8_t* base, PortText& t) {
  uint32_t text = t.guest.load(std::memory_order_acquire);
  if (!text) {
    text = GuestString(base, t.text);
    uint32_t none = 0;
    if (!t.guest.compare_exchange_strong(none, text, std::memory_order_acq_rel)) text = none;
  }
  return text;
}

// The port's text of `key` (one of g_texts), 0 if none.
uint32_t TextOf(uint8_t* base, const char* key) {
  for (auto& t : g_texts)
    if (std::strcmp(key, t.key) == 0) return PortTextFor(base, t);
  return 0;
}

bool SameText(const std::string& a, const char* b) {
  const size_t n = std::strlen(b);
  if (a.size() != n) return false;
  for (size_t i = 0; i < n; ++i)
    if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) return false;
  return true;
}

uint64_t VCall(PPCContext& ctx, uint8_t* base, uint32_t obj, uint32_t slot, uint64_t a1 = 0, uint64_t a2 = 0,
               uint64_t a3 = 0) {
  Call c(ctx, base);
  return c.Invoke(VirtualSlot(base, obj, slot), {obj, a1, a2, a3});
}

// The QUIT TO DESKTOP box on `player`'s message box system. False when it
// can't be made (the caller quits without asking, as the campaign does).
bool OpenQuitBox(PPCContext& ctx, uint8_t* base, uint32_t player, bool in_match) {
  if (player >= 4) player = 0;
  const uint32_t system = ReadU32(base, kDialogSystems + 4 * player);
  if (!Valid(system) || ReadU32(base, system) != kDialogSystemVtable) return false;
  const uint32_t pages = ReadU32(base, system + kSysPages);
  const uint32_t text =
      TextOf(base, in_match ? "COD3_QUIT_MATCH_TO_DESKTOP_QUESTION" : "COD3_QUIT_TO_DESKTOP_QUESTION");
  const uint32_t title = TextOf(base, in_match ? "COD3_QUIT_TO_DESKTOP" : "COD3_EMPTY");
  if (!Valid(pages) || !text || !title) return false;
  Call c(ctx, base);
  c.Invoke(kDialogClose, {system});
  c.Invoke(kDialogSetMessage, {system, text, 0, 0, title, 1});
  const uint32_t index = static_cast<uint32_t>(VCall(ctx, base, system, kSysPageIndex));
  const uint32_t page = ReadU32(base, pages + (index != 0 ? 4 : 0));
  if (!Valid(page) || ReadU32(base, page) != kDialogPageVtable) {
    c.Invoke(kDialogClose, {system});
    return false;
  }
  g_opening_quit_box = true;
  c.Invoke(kDialogAddButton, {page, in_match ? kMatchYesKey : kFrontEndYesKey, kQuitYesCallback});
  c.Invoke(kDialogAddButton, {page, in_match ? kMatchNoKey : kFrontEndNoKey, kQuitNoCallback});
  WriteU32(base, page + kDialogPageBack, kDialogCloseBack);  // B: only the box closes
  c.Invoke(kDialogSelect, {system, 1});  // No
  c.Invoke(kDialogOpen, {system, 1});
  g_opening_quit_box = false;
  g_quit_box = QuitBox{true, in_match, player};
  return true;
}

void QuitToDesktop(PPCContext& ctx, uint8_t* base, uint32_t player, bool in_match, const char* from) {
  REXLOG_INFO("cod3mp: QUIT TO DESKTOP ({}): asking", from);
  if (OpenQuitBox(ctx, base, player, in_match)) return;
  REXLOG_WARN("cod3mp: QUIT TO DESKTOP ({}): no message box; quitting without asking", from);
  cod3::pcmenu::QuitToDesktopNow();
}

// ---------------------------------------------------------------------------
// The pause menus' seventh row (see above).
// ---------------------------------------------------------------------------
std::mutex g_pause_mutex;
struct WidePage {
  uint32_t page = 0, items = 0, full = 0, split = 0;  // the seven-slot arrays made for it
};
std::vector<WidePage> g_wide_pages;  // g_pause_mutex

// The page has the seven-slot arrays made for it (a page made again at the
// same address has the game's six-slot ones: it doesn't).
bool IsWide(uint8_t* base, uint32_t page) {
  std::lock_guard lock(g_pause_mutex);
  for (const WidePage& w : g_wide_pages)
    if (w.page == page)
      return ReadU32(base, page + kPageItems) == w.items &&
             (!w.full || (ReadU32(base, page + kSetFull) == w.full && ReadU32(base, page + kSetSplit) == w.split));
  return false;
}

// A seven-slot copy of a six-slot array of entries (0 if it can't be made).
uint32_t WiderArray(PPCContext& ctx, uint8_t* base, uint32_t old_items) {
  Call c(ctx, base);
  const uint32_t items = static_cast<uint32_t>(c.Invoke(kOperatorNew, {4u * (kPauseItems + 1)}));
  if (!Valid(items)) return 0;
  for (int i = 0; i <= kPauseItems; ++i)
    WriteU32(base, items + 4 * uint32_t(i),
             i < kPauseItems && Valid(old_items) ? ReadU32(base, old_items + 4 * uint32_t(i)) : 0);
  return items;
}

// Room for the seventh item, once per page: the page's items and (PauseMenu)
// both sets. The old arrays are left as they are (a few bytes per page;
// the pages live as long as the game).
bool MakeRoom(PPCContext& ctx, uint8_t* base, uint32_t page, bool sets) {
  if (IsWide(base, page)) return true;
  WidePage w;
  w.page = page;
  w.items = WiderArray(ctx, base, ReadU32(base, page + kPageItems));
  if (sets) {
    w.full = WiderArray(ctx, base, ReadU32(base, page + kSetFull));
    w.split = WiderArray(ctx, base, ReadU32(base, page + kSetSplit));
  }
  if (!w.items || (sets && (!w.full || !w.split))) return false;
  WriteU32(base, page + kPageItems, w.items);
  if (sets) {
    WriteU32(base, page + kSetFull, w.full);
    WriteU16(base, page + kSetFull + kSetCapacity, kPauseItems + 1);
    WriteU32(base, page + kSetSplit, w.split);
    WriteU16(base, page + kSetSplit + kSetCapacity, kPauseItems + 1);
  }
  std::lock_guard lock(g_pause_mutex);
  std::erase_if(g_wide_pages, [page](const WidePage& o) { return o.page == page; });
  g_wide_pages.push_back(w);
  return true;
}

// Adds QUIT TO DESKTOP as item 6 on the panel's row text `row`; `set` is the
// item set it goes in (PauseMenu's +88 / +96; 0 for a plain page, whose
// items and count are the page's own).
void AddQuitRow(PPCContext& ctx, uint8_t* base, uint32_t page, uint32_t panel, const char* row, uint32_t set,
                const char* what) {
  const uint32_t label = cod3::pcmenu::FindPanelText(ctx, base, panel, row);
  if (!label) return;  // not this panel's layout
  const uint32_t list = ReadU32(base, page + kPageItems);
  const uint32_t first = Valid(list) ? ReadU32(base, list) : 0;
  const uint32_t quit = Valid(list) ? ReadU32(base, list + 4 * (kPauseItems - 1)) : 0;
  if (!Valid(first) || !Valid(quit) || (set && ReadU16(base, page + set + kSetCount) < kPauseItems)) {
    REXLOG_WARN("cod3mp: {} not as expected; no QUIT TO DESKTOP row", what);
    return;
  }
  if (!MakeRoom(ctx, base, page, set != 0)) {
    REXLOG_WARN("cod3mp: {}: no memory for a seventh row", what);
    return;
  }
  // AddItem(6) puts the entry in the page's items and links it under item 5.
  VCall(ctx, base, page, kPageAddItem, kQuitToDesktopItem, label, 0);
  const uint32_t items = ReadU32(base, page + kPageItems);
  const uint32_t entry = ReadU32(base, items + 4 * kQuitToDesktopItem);
  if (!Valid(entry)) return;
  cod3::pcmenu::SetItemLabelKey(ctx, base, entry, "COD3_QUIT_TO_DESKTOP");
  VCall(ctx, base, entry, kEntrySetVisible, 1);
  VCall(ctx, base, entry, kEntrySetDisabled, 0);
  // Up from the first row wraps to it, down from it to the first.
  WriteU16(base, first + kEntryUp, uint16_t(kQuitToDesktopItem));
  WriteU16(base, entry + kEntryDown, 0);
  WriteU16(base, quit + kEntryDown, uint16_t(kQuitToDesktopItem));
  if (set) {
    WriteU32(base, ReadU32(base, page + set) + 4 * kQuitToDesktopItem, entry);
    WriteU16(base, page + set + kSetCount, kPauseItems + 1);
  } else {
    WriteU16(base, page + kPageCount, kPauseItems + 1);
  }
  REXLOG_INFO("cod3mp: {}: QUIT TO DESKTOP added under QUIT", what);
}

}  // namespace

namespace cod3::mpmenu {

bool MenuActive() {
  const int64_t when = g_menu_ns.load(std::memory_order_relaxed);
  if (when == 0 || NowNs() - when >= kLoadingLimitNs) return false;
  uint8_t* base = GuestBase();
  if (!base) return false;
  const uint32_t frame = ReadU32(base, cod3::addr::pc_menu::kFrameCount);
  return frame - g_menu_frame.load(std::memory_order_relaxed) <= 1;
}

bool KeyboardMenuUp() { return REXCVAR_GET(cod3_pc_menus) ? cod3::pcmenu::MenuActive() : MenuActive(); }

void Tick() {
  if (g_voice_checked) return;
  const int64_t first = g_first_menu_ns.load(std::memory_order_relaxed);
  if (first == 0 || NowNs() - first < kVoiceCheckNs) return;
  uint8_t* base = GuestBase();
  if (!base) return;
  g_voice_checked = true;
  const uint32_t voice = ReadU32(base, kVoiceSystemPtr);
  if (voice) {
    REXLOG_INFO("cod3mp: the game's voice and session system is up ({:08X})", voice);
    return;
  }
  REXLOG_WARN("cod3mp: the game's voice and session system didn't start (it starts with the sound "
              "system: is there an audio output device?); creating or joining a match will stop "
              "the game");
  cod3::hud::AnnouncementFeed::Push(
      "No sound device: matches can't start. Connect speakers or headphones and restart.");
}

}  // namespace cod3::mpmenu

// Every menu system's draw (front end, in game, message boxes) ends here.
REX_EXTERN(__imp__FEMenuSystem__vfunc_19);
REX_HOOK_RAW(FEMenuSystem__vfunc_19) {
  const uint32_t system = ctx.r3.u32;
  if (Valid(system) && Valid(ReadU32(base, system + kSysPages)) &&
      (static_cast<int32_t>(ReadU32(base, system + kSysCurrent)) >= 0 ||
       static_cast<int32_t>(ReadU32(base, system + kSysUnder)) >= 0)) {
    const int64_t now = NowNs();
    g_menu_frame.store(ReadU32(base, cod3::addr::pc_menu::kFrameCount), std::memory_order_relaxed);
    g_menu_ns.store(now, std::memory_order_relaxed);
    int64_t none = 0;
    g_first_menu_ns.compare_exchange_strong(none, now, std::memory_order_relaxed);
  }
  __imp__FEMenuSystem__vfunc_19(ctx, base);
}

// The after-action report's draw (AARMenuSystem slot 19): what it draws
// belongs to it (game/pc_menu.cpp hooks the other three systems' draws).
REX_EXTERN(__imp__AARMenuSystem__vfunc_19);
REX_HOOK_RAW(AARMenuSystem__vfunc_19) {
  const uint32_t outer = cod3::pcmenu::BeginSystemDraw(ctx.r3.u32);
  __imp__AARMenuSystem__vfunc_19(ctx, base);
  cod3::pcmenu::EndSystemDraw(outer);
}

// The main menu's A (this, player).
REX_EXTERN(__imp__MPMainMenuXBox__vfunc_36);
REX_HOOK_RAW(MPMainMenuXBox__vfunc_36) {
  const uint32_t page = ctx.r3.u32;
  if (Valid(page)) {
    const uint32_t item = ReadU32(base, page + kMainMenuRow) + ReadU32(base, page + kMainMenuRowOffset);
    if (item == kXboxLiveItem) {
      REXLOG_INFO("cod3mp: main menu: XBOX LIVE isn't part of the PC version; not opened");
      cod3::hud::AnnouncementFeed::Push("XBOX LIVE isn't part of the PC version: play over SYSTEM LINK");
      return;
    }
    if (item == kQuitItem) {
      QuitToDesktop(ctx, base, ReadU32(base, kActiveClient), /*in_match=*/false, "main menu");
      return;
    }
  }
  __imp__MPMainMenuXBox__vfunc_36(ctx, base);
}

// PauseMenu's Init (this, panel), once per panel: then the seventh row.
REX_EXTERN(__imp__sub_8239D920);
REX_HOOK_RAW(sub_8239D920) {
  const uint32_t page = ctx.r3.u32, panel = ctx.r4.u32;
  __imp__sub_8239D920(ctx, base);
  if (!Valid(page) || ReadU32(base, page) != kPauseVtable || !Valid(panel)) return;
  // The panel the game's Init laid out (its own test of the name).
  const std::string name = cod3::guest::ReadString(base, panel + kPanelName, 64);
  if (SameText(name, "MP_PM_mainmenu.PANEL"))
    AddQuitRow(ctx, base, page, panel, "slot_07_text_option", kSetFull, "pause menu");
  else if (SameText(name, "MP_SS_PM_options.PANEL"))
    AddQuitRow(ctx, base, page, panel, "text_option_07", kSetSplit, "split-screen pause menu");
}

// AARPauseMenu's Init (this, panel).
REX_EXTERN(__imp__sub_82397B90);
REX_HOOK_RAW(sub_82397B90) {
  const uint32_t page = ctx.r3.u32, panel = ctx.r4.u32;
  __imp__sub_82397B90(ctx, base);
  if (!Valid(page) || ReadU32(base, page) != kAarPauseVtable || !Valid(panel)) return;
  AddQuitRow(ctx, base, page, panel, "slot_07_text_option", 0, "after-match pause menu");
}

// PauseMenu's A (this, player): QUIT TO DESKTOP, the others as before.
REX_EXTERN(__imp__PauseMenu__vfunc_36);
REX_HOOK_RAW(PauseMenu__vfunc_36) {
  const uint32_t page = ctx.r3.u32;
  if (Valid(page) && IsWide(base, page) && static_cast<int16_t>(ReadU16(base, page + kPageSelected)) == kQuitToDesktopItem &&
      !ReadU8(base, page + 48)) {
    QuitToDesktop(ctx, base, ReadU32(base, page + kPagePlayer), /*in_match=*/true, "pause menu");
    return;
  }
  __imp__PauseMenu__vfunc_36(ctx, base);
}

// AARPauseMenu's A (this, player): its player as its QUIT finds it.
REX_EXTERN(__imp__AARPauseMenu__vfunc_36);
REX_HOOK_RAW(AARPauseMenu__vfunc_36) {
  const uint32_t page = ctx.r3.u32;
  if (Valid(page) && IsWide(base, page) && static_cast<int16_t>(ReadU16(base, page + kPageSelected)) == kQuitToDesktopItem &&
      !ReadU8(base, page + 48)) {
    Call c(ctx, base);
    const uint32_t input = static_cast<uint32_t>(c.Invoke(kInputObject, {}));
    const uint32_t pad = Valid(input) ? ReadU32(base, input + kInputActivePad) : 0;
    const int32_t client = static_cast<int32_t>(c.Invoke(kClientForPad, {pad}));
    QuitToDesktop(ctx, base, client >= 0 && client < 4 ? uint32_t(client) : 0, /*in_match=*/true,
                  "after-match pause menu");
    return;
  }
  __imp__AARPauseMenu__vfunc_36(ctx, base);
}

// The QUIT TO DESKTOP box's Yes (player) -> 1 (see kQuitYesCallback).
REX_EXTERN(__imp__sub_82412170);
REX_HOOK_RAW(sub_82412170) {
  if (g_quit_box.up) {
    const QuitBox box = g_quit_box;
    g_quit_box = QuitBox{};
    REXLOG_INFO("cod3mp: quit to desktop (confirmed in the game's message box{})",
                box.in_match ? ", in a match" : "");
    cod3::pcmenu::QuitToDesktopNow();
    ctx.r3.u64 = 1;
    return;
  }
  __imp__sub_82412170(ctx, base);
}

// A message box opens (system, 1): another box than ours clears the QUIT TO
// DESKTOP mark; one that had highlighted a button it doesn't have (the
// "Select Storage Device" buttons pc_menu.cpp leaves out) highlights its
// last one.
REX_EXTERN(__imp__sub_820B4B10);
REX_HOOK_RAW(sub_820B4B10) {
  const uint32_t system = ctx.r3.u32;
  if (!g_opening_quit_box) g_quit_box = QuitBox{};
  if (Valid(system) && ReadU32(base, system) == kDialogSystemVtable) {
    const uint32_t pages = ReadU32(base, system + kSysPages);
    const uint32_t index = static_cast<uint32_t>(VCall(ctx, base, system, kSysPageIndex));
    const uint32_t page = Valid(pages) ? ReadU32(base, pages + (index != 0 ? 4 : 0)) : 0;
    if (Valid(page) && ReadU32(base, page) == kDialogPageVtable) {
      const int32_t buttons = static_cast<int32_t>(ReadU32(base, page + kDialogPageButtons));
      const int16_t selected = static_cast<int16_t>(ReadU16(base, page + kDialogPageSelected));
      if (buttons > 0 && buttons <= 2 && selected >= buttons) {
        Call c(ctx, base);
        c.Invoke(kDialogSelect, {system, uint64_t(buttons - 1)});
      }
    }
  }
  __imp__sub_820B4B10(ctx, base);
}

// const char* Localize(StringTable*, const char* key): the port's texts for
// the main menu's changed items and its own keys. (The campaign's own hook of
// this function is in game/menu_fov.cpp, which isn't part of cod3mp.)
REX_EXTERN(__imp__Localize);
REX_HOOK_RAW(Localize) {
  const uint32_t key = ctx.r4.u32;
  if (key) {
    const char* k = reinterpret_cast<const char*>(base + key);
    for (auto& t : g_texts) {
      if (std::strcmp(k, t.key) != 0) continue;
      if (const uint32_t text = PortTextFor(base, t)) {
        ctx.r3.u64 = text;
        return;
      }
      break;
    }
  }
  __imp__Localize(ctx, base);
}
