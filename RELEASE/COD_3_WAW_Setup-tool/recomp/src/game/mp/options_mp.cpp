// cod3mp: FIELD OF VIEW and FOV SCALE in the multiplayer program's own
// option pages, like the campaign's rows (game/menu_fov.cpp) [mp-menus], and
// Pause > OPTIONS in a match as category pages [menus-categories].
//
// Main menu > Options > Gameplay: MPOptionsGameplayMenu (vtable 0x8207B674,
// the front end's page 20) has two rows, STICKY AIM and CROSSHAIR. Both are
// FEComboBox entries: a list of values the entry steps through itself on
// left / right, with its own arrow flash and sound. Its panel has five rows;
// Init hides rows three to five. Rows three and four become FIELD OF VIEW and
// FOV SCALE, made the way Init makes its own two:
//  * the constructor sizes the page's entry array for two: after it, the
//    array is replaced by one for four (the game's operator new; the FEMenu
//    destructor frees it as its own, and the old one is freed here);
//  * after Init, an FEComboBox entry goes on each row (the page's own
//    AddComboBox, slot 10, on the row's texts and arrows, with one value),
//    the row's arrows, background and line are shown, and up / down wrap over
//    the four rows. A panel without the rows keeps its two (the entry count
//    goes back to 2, which the page's draw and moves read);
//  * the page's moves and refresh read the title and help text keys from two
//    two-entry tables by the selection: on rows three and four ours are set
//    after them.
//
// In a match: Pause > OPTIONS, InGameOptionsMenu (vtable 0x820716DC, the
// in-game menu system's page 7; AARInGameOptionsMenu, 0x820717E4, after a
// match, derives from it and shares what is used here). [menus-categories]
// Its full-screen layout is the category pages of game/options_pages.h (owner
// 2026-10-20 20:51: "maybe a menu re-design is needed. We have a lot of
// settings.", category pages in both programs): a list, GAMEPLAY, CONTROLS,
// AIM, VIDEO and RESTORE DEFAULTS; A on a category shows its page (title
// "OPTIONS / CONTROLS" ...), B goes back to the list, and B on the list is the
// page's own (it asks to apply changed options, then closes). The panel
// (MP_PM_controller, MP_AAR_PM_controller) has ten rows, slot_01 to slot_10:
//  * the page keeps its entries in three arrays: +4 (FEMenu's: what is drawn
//    and moved through, count +38), +88 (the full-screen layout's) and +96
//    (the split-screen layout's), each {entries; s16 capacity; s16 count};
//    the page's refresh copies one of the last two into +4 with its count.
//    After the constructor all three have room for ours (the counts stay
//    ten);
//  * the page's own entries 0-8 (STICK LAYOUT ... VIBRATION) stay where they
//    are, so B, RESTORE DEFAULTS and the refresh, which read them by index,
//    work as before. They are skipped (disabled) and their texts, arrows and
//    gauges hidden; a page's row that stands for one of them shows its label
//    and value and takes its own left / right (sound and all), so the
//    profile keeps what is chosen and "apply?" covers it;
//  * ours, after Init: the list's items (plain items like RESTORE DEFAULTS,
//    on copies of its label), a page's rows (combo boxes on copies of row
//    nine: label, value, arrows; up to ten, one per panel row) and a note
//    line under them (what the highlighted video row needs to know, like
//    "APPLIES AFTER A RESTART"). The page's draw places them on the panel's
//    rows every frame (their opacity follows its fades) and draws the
//    copied arrows; RESTORE DEFAULTS sits under the list's last item;
//  * up / down go through what is shown and wrap; A on a row steps right;
//  * the split-screen layout (four rows that scroll through the entries in
//    order, RESTORE DEFAULTS fixed as entry nine) keeps its ten; F2 has the
//    port's settings there;
//  * the game deletes our entries with its own; our copies are freed when the
//    page lets its panel go (FESplitScreenMenu slot 1) or is destroyed.
//
// On both pages, left / right on our rows (the entry's slots 5 / 6: the arrow
// flash and the sound stay the game's, then here) step cg_fov by 5 or
// cg_fovScale by 0.05 within what the other allows (game/fov.h: the field of
// view stays within 65 to 165), applied at once and saved like F2, and the
// row shows the new value; the pages' refresh and update show a value changed
// in F2 or the ~ console. The texts are the program's Localize hook's
// (game/mp/menus_mp.cpp). F2 > Game > View has both settings too, and the
// console has cg_fov and cg_fovScale.

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>

#include "game/engine.h"
#include "game/fov.h"
#include "game/guest.h"
#include "game/menu_video.h"     // [menus-categories] the VIDEO page's rows
#include "game/options_pages.h"  // [menus-categories]
#include "game/pc_menu.h"

namespace {

using cod3::guest::Call;
using cod3::guest::ReadF32;
using cod3::guest::ReadU16;
using cod3::guest::ReadU32;
using cod3::guest::ReadU8;
using cod3::guest::VirtualSlot;
using cod3::guest::WriteF32;
using cod3::guest::WriteU16;
using cod3::guest::WriteU32;

// FEMenu fields: the entries at +4 (an array the size of +38), the selected
// one at +34. FEMenuEntry: s16 links to the entry above (+8) and below (+10).
constexpr uint32_t kItems = 4;
constexpr uint32_t kSelected = 34;           // s16
constexpr uint32_t kItemCount = 38;          // s16
constexpr uint32_t kEntryUp = 8, kEntryDown = 10;
constexpr uint32_t kPageAddComboBox = 40;    // vtbl: (index, values, value text, label text, left, right)
constexpr uint32_t kPageSetSelection = 192;  // vtbl: (index, 1)

// MPOptionsGameplayMenu (codmp_xenonf.xex).
constexpr uint32_t kGameplayVtable = 0x8207B674;
constexpr uint32_t kPanel = 72;              // Init: stw r3,72(r31)
constexpr uint32_t kSelectedText = 84;       // text_current_selected: the selected row's name
constexpr uint32_t kInstructions = 92;       // FEMultiLineText: the selected row's help
constexpr uint32_t kNarrowHelp = 96;         // u8: the help text is 390 wide, else 520
constexpr int kStockRows = 2;
constexpr int kFirstRow = 2;                 // FIELD OF VIEW's entry; FOV SCALE's is the next
constexpr int kAllRows = 4;

// InGameOptionsMenu / AARInGameOptionsMenu.
constexpr uint32_t kMatchVtable = 0x820716DC;
constexpr uint32_t kAarMatchVtable = 0x820717E4;
constexpr uint32_t kMatchPanel = 72;         // the full-screen panel
constexpr uint32_t kMatchSplit = 76;         // u32: the split-screen layout is in use
constexpr uint32_t kMatchFullSet = 88;       // {entries; s16 capacity +4; s16 count +6}
constexpr uint32_t kMatchSplitSet = 96;
constexpr uint32_t kSetCapacity = 4, kSetCount = 6;

// FEComboBox (vtable 0x820709EC, ctor 0x824E8FC8, 96 bytes).
constexpr uint32_t kComboVtable = 0x820709EC;
constexpr uint32_t kComboValueText = 16;     // the text its value is shown in
constexpr uint32_t kComboValueCount = 28;    // s16
constexpr uint32_t kComboAddValue = 0x824FFA58;  // (entry, String*): localized, kept; the String released
constexpr uint32_t kStringCtor = 0x821E0E48;     // (String*, const char*)
constexpr uint32_t kOperatorNew = 0x820DE6D8;    // (size)
constexpr uint32_t kOperatorDelete = 0x820DE980; // (pointer)

// Panel elements: virtual slots (byte offsets) and fields.
constexpr uint32_t kElemDelete = 0;          // (1): destroy and free
constexpr uint32_t kElemDraw = 4;            // () as the panel draws its pictures
constexpr uint32_t kElemSetVisible = 60;     // (bool)
constexpr uint32_t kElemSetTextKey = 132;    // (key): Localize, then the text
constexpr uint32_t kElemSetTextRaw = 144;    // (text)
constexpr uint32_t kWrapSetTextKey = 384;    // FEMultiLineText: (key, width, f1 scale)
constexpr uint32_t kElemVisibility = 16;     // u8: 0x4 = shown
constexpr uint32_t kTextY = 48;              // float
constexpr uint32_t kQuadParts = 32;          // what a picture draws: parts, each 4 vertices
constexpr uint32_t kQuadPartVerts = 16;      // the first vertex {x, y, ...} of a part
constexpr uint32_t kTextScaleDefault = 0x825DA49C;  // -1.0, as the page passes it
constexpr uint64_t kHelpWidthNarrow = 390, kHelpWidth = 520;

constexpr float kFovStep = 5.0f;
constexpr float kScaleStep = 0.05f;

constexpr const char* kLabelKeys[2] = {"FEMENU_COP_FOV", "FEMENU_COP_FOV_SCALE"};
constexpr const char* kHelpKeys[2] = {"FEMENU_GAMEPLAY_INST_FOV", "FEMENU_GAMEPLAY_INST_FOV_SCALE"};

// The gameplay page's rows three and four (names as Init's table 0x82061BF0
// has them: a row's line is the one above it).
struct RowSpec {
  const char* label;
  const char* value;
  const char* arrow_left;
  const char* arrow_right;
  const char* background;
  const char* line;
};
constexpr RowSpec kRows[2] = {
    {"slot_03_text_a", "slot_03_text_b", "slot_03_arrow_left", "slot_03_arrow_right", "bkg_row_03",
     "bkg_line_02"},
    {"slot_04_text_a", "slot_04_text_b", "slot_04_arrow_left", "slot_04_arrow_right", "bkg_row_04",
     "bkg_line_03"},
};

// Our rows of Options > Gameplay (game thread). Row 0 is FIELD OF VIEW, 1 FOV
// SCALE. (Pause > OPTIONS has its own, MatchPage below.)
struct PageRows {
  int rows = 0;                       // 0, 1 or 2
  std::array<uint32_t, 2> entry{};    // their FEComboBox entries
  std::array<std::string, 2> shown;   // the value each shows
};
std::mutex g_mutex;
std::unordered_map<uint32_t, PageRows> g_pages;  // keyed by page

bool Valid(uint32_t p) { return p >= 0x10000 && p < 0xC0000000u && (p & 3) == 0; }

int16_t ReadS16(uint8_t* base, uint32_t addr) { return static_cast<int16_t>(ReadU16(base, addr)); }

bool IsGameplayPage(uint8_t* base, uint32_t page) {
  return Valid(page) && ReadU32(base, page) == kGameplayVtable;
}

bool IsMatchPage(uint8_t* base, uint32_t page) {
  if (!Valid(page)) return false;
  const uint32_t vt = ReadU32(base, page);
  return vt == kMatchVtable || vt == kAarMatchVtable;
}

// A virtual call with up to seven arguments after `obj` (unused ones are 0).
uint64_t VCall(PPCContext& ctx, uint8_t* base, uint32_t obj, uint32_t slot,
               std::initializer_list<uint64_t> args = {}) {
  if (!Valid(obj)) return 0;
  uint64_t a[7] = {};
  size_t n = 0;
  for (uint64_t v : args)
    if (n < 7) a[n++] = v;
  Call c(ctx, base);
  return c.Invoke(VirtualSlot(base, obj, slot), {obj, a[0], a[1], a[2], a[3], a[4], a[5], a[6]});
}

void SetVisible(PPCContext& ctx, uint8_t* base, uint32_t elem) {
  if (Valid(elem)) VCall(ctx, base, elem, kElemSetVisible, {1});
}

void SetTextKey(PPCContext& ctx, uint8_t* base, uint32_t elem, const char* key) {
  if (!Valid(elem)) return;
  Call c(ctx, base);
  const uint32_t s = c.PushString(key);
  c.Invoke(VirtualSlot(base, elem, kElemSetTextKey), {elem, s});
}

void SetTextRaw(PPCContext& ctx, uint8_t* base, uint32_t elem, const std::string& text) {
  if (!Valid(elem)) return;
  Call c(ctx, base);
  const uint32_t s = c.PushString(text);
  c.Invoke(VirtualSlot(base, elem, kElemSetTextRaw), {elem, s});
}

// The gameplay page's help text under the rows, as the page sets its own.
void SetHelp(PPCContext& ctx, uint8_t* base, uint32_t page, const char* key) {
  const uint32_t box = ReadU32(base, page + kInstructions);
  if (!Valid(box)) return;
  Call c(ctx, base);
  const uint32_t s = c.PushString(key);
  const uint64_t width = ReadU8(base, page + kNarrowHelp) ? kHelpWidthNarrow : kHelpWidth;
  c.Invoke(VirtualSlot(base, box, kWrapSetTextKey), {box, s, width}, {ReadF32(base, kTextScaleDefault)});
}

// A guest String's slot for AddValue (4 bytes, game thread only), made once.
uint32_t StringSlot() {
  static std::atomic<uint32_t> slot{0};
  uint32_t s = slot.load(std::memory_order_acquire);
  if (!s) {
    auto* ks = rex::system::kernel_state();
    if (!ks || !ks->memory()) return 0;
    s = ks->memory()->SystemHeapAlloc(16);
    slot.store(s, std::memory_order_release);
  }
  return s;
}

// One value in a combo box entry (so its own left / right have one to show).
void AddValue(PPCContext& ctx, uint8_t* base, uint32_t entry, const std::string& text) {
  const uint32_t slot = StringSlot();
  if (!Valid(slot)) return;
  WriteU32(base, slot, 0);
  Call c(ctx, base);
  const uint32_t s = c.PushString(text);
  c.Invoke(kStringCtor, {slot, s});
  c.Invoke(kComboAddValue, {entry, slot});
}

// --- the settings ------------------------------------------------------------
float CurrentFov() {
  const float fov = cod3::Engine::Get().fov();
  return std::isfinite(fov) ? fov : cod3::fov::kDefaultFov;
}

float CurrentScale() {
  const float scale = cod3::Engine::Get().fov_scale();
  return std::isfinite(scale) && scale > 0.0f ? scale : cod3::fov::kDefaultScale;
}

std::string ValueText(int row) {
  char buf[16];
  if (row == 1)
    std::snprintf(buf, sizeof(buf), "%.2f", CurrentScale());
  else
    std::snprintf(buf, sizeof(buf), "%d", static_cast<int>(std::lround(CurrentFov())));
  return buf;
}

// The next multiple of `step` past `value` (a value set in F2 or the console
// can lie between two steps: 83 goes to 85 or 80), kept within `range`.
float NextStep(float value, float step, bool right, cod3::fov::Range range) {
  const float steps = value / step;
  const float next = right ? std::floor(steps + 1e-3f) + 1.0f : std::ceil(steps - 1e-3f) - 1.0f;
  return std::clamp(next * step, range.min, range.max);
}

// One step on FIELD OF VIEW (row 0) or FOV SCALE (row 1); each row's range
// follows the other row's value. Returns the new value's text.
std::string StepValue(PPCContext& ctx, uint8_t* base, int row, bool right) {
  if (row == 1) {
    const float v = NextStep(CurrentScale(), kScaleStep, right, cod3::fov::ScaleRange(CurrentFov()));
    cod3::Engine::Get().SetFovScale(v);  // applied on the next frame, saved like F2
  } else {
    const float v = NextStep(CurrentFov(), kFovStep, right, cod3::fov::FovRange(CurrentScale()));
    cod3::Engine::Get().ApplyFovFromGuest(ctx, base, v);
  }
  return ValueText(row);
}

// --- our rows ------------------------------------------------------------------
bool GetRows(uint32_t page, PageRows& out) {
  std::lock_guard lock(g_mutex);
  auto it = g_pages.find(page);
  if (it == g_pages.end()) return false;
  out = it->second;
  return true;
}

// Where Options > Gameplay keeps the entry of our row `k`.
uint32_t EntrySlot(uint8_t* base, uint32_t page, int k) {
  const uint32_t list = ReadU32(base, page + kItems);
  if (!Valid(list) || ReadS16(base, page + kItemCount) <= kFirstRow + k) return 0;
  return list + 4 * uint32_t(kFirstRow + k);
}

// Which of our rows `entry` is (0 FIELD OF VIEW, 1 FOV SCALE), -1 none. Only
// while it is still that page's entry (a page made again gets new ones).
int RowOf(uint8_t* base, uint32_t entry, uint32_t* page_out) {
  if (!Valid(entry) || ReadU32(base, entry) != kComboVtable) return -1;
  std::lock_guard lock(g_mutex);
  for (const auto& [page, rows] : g_pages) {
    for (int k = 0; k < rows.rows; ++k) {
      if (rows.entry[k] != entry) continue;
      if (!IsGameplayPage(base, page)) return -1;
      const uint32_t at = EntrySlot(base, page, k);
      if (!at || ReadU32(base, at) != entry) return -1;
      if (page_out) *page_out = page;
      return k;
    }
  }
  return -1;
}

void Remember(uint32_t page, int row, const std::string& text) {
  std::lock_guard lock(g_mutex);
  if (auto it = g_pages.find(page); it != g_pages.end() && row >= 0 && row < 2) it->second.shown[row] = text;
}

// Each row shows its setting (changed in F2 or the console meanwhile).
void ShowValues(PPCContext& ctx, uint8_t* base, uint32_t page) {
  PageRows rows;
  if (!GetRows(page, rows)) return;
  for (int k = 0; k < rows.rows; ++k) {
    const std::string text = ValueText(k);
    if (text == rows.shown[k] || RowOf(base, rows.entry[k], nullptr) != k) continue;
    SetTextRaw(ctx, base, ReadU32(base, rows.entry[k] + kComboValueText), text);
    Remember(page, k, text);
  }
}

// Left / right on one of our rows, after (or instead of) the entry's own.
void Stepped(PPCContext& ctx, uint8_t* base, uint32_t entry, uint32_t page, int row, bool right) {
  const std::string text = StepValue(ctx, base, row, right);
  SetTextRaw(ctx, base, ReadU32(base, entry + kComboValueText), text);
  Remember(page, row, text);
}

bool HasValue(uint8_t* base, uint32_t entry) { return ReadS16(base, entry + kComboValueCount) >= 1; }

// A combo box entry on the page at `index` with one value, its label keyed;
// 0 if the page didn't take it.
uint32_t AddRowEntry(PPCContext& ctx, uint8_t* base, uint32_t page, int index, int row, uint32_t label,
                     uint32_t value, uint32_t left, uint32_t right) {
  SetTextKey(ctx, base, label, kLabelKeys[row]);
  const uint32_t entry = static_cast<uint32_t>(
      VCall(ctx, base, page, kPageAddComboBox, {uint64_t(index), 1, value, label, left, right}));
  const uint32_t list = ReadU32(base, page + kItems);
  if (!Valid(entry) || !Valid(list) || ReadU32(base, list + 4 * uint32_t(index)) != entry ||
      ReadU32(base, entry) != kComboVtable)
    return 0;
  const std::string text = ValueText(row);
  AddValue(ctx, base, entry, text);
  SetTextRaw(ctx, base, value, text);
  return entry;
}

// === Main menu > Options > Gameplay ==========================================

// The constructor's two-entry array becomes a four-entry one.
void GrowEntryArray(PPCContext& ctx, uint8_t* base, uint32_t page) {
  if (!IsGameplayPage(base, page)) return;
  const int n = ReadS16(base, page + kItemCount);
  const uint32_t old = ReadU32(base, page + kItems);
  if (n != kStockRows || !Valid(old)) {
    REXLOG_WARN("cod3mp: Options > Gameplay has {} rows, not {}; no FIELD OF VIEW rows", n, kStockRows);
    return;
  }
  Call c(ctx, base);
  const uint32_t list = static_cast<uint32_t>(c.Invoke(kOperatorNew, {uint64_t(kAllRows) * 4}));
  if (!Valid(list)) {
    REXLOG_WARN("cod3mp: Options > Gameplay: no memory for more rows; no FIELD OF VIEW rows");
    return;
  }
  for (int i = 0; i < kAllRows; ++i)
    WriteU32(base, list + 4 * uint32_t(i), i < kStockRows ? ReadU32(base, old + 4 * uint32_t(i)) : 0);
  WriteU32(base, page + kItems, list);
  WriteU16(base, page + kItemCount, uint16_t(kAllRows));
  c.Invoke(kOperatorDelete, {old});
}

// After Init: FIELD OF VIEW and FOV SCALE on rows three and four.
void AddRows(PPCContext& ctx, uint8_t* base, uint32_t page) {
  if (!IsGameplayPage(base, page)) return;
  const uint32_t panel = ReadU32(base, page + kPanel);
  const uint32_t list = ReadU32(base, page + kItems);
  const int room = ReadS16(base, page + kItemCount);
  if (!Valid(panel) || !Valid(list)) return;
  cod3::pcmenu::DumpMenuPanel(base, panel, "multiplayer Options > Gameplay");
  if (room < kAllRows) {
    if (room > kStockRows) WriteU16(base, page + kItemCount, uint16_t(kStockRows));
    return;  // the constructor's hook said why
  }
  PageRows rows;
  for (int k = 0; k < 2; ++k) {
    const RowSpec& r = kRows[k];
    const uint32_t label = cod3::pcmenu::FindPanelText(ctx, base, panel, r.label);
    const uint32_t value = cod3::pcmenu::FindPanelText(ctx, base, panel, r.value);
    if (!label || !value) {
      REXLOG_WARN("cod3mp: Options > Gameplay: the panel has no {}; {} not added", label ? r.value : r.label,
                  k ? "FOV SCALE" : "FIELD OF VIEW");
      break;
    }
    const uint32_t left = cod3::pcmenu::FindPanelPicture(ctx, base, panel, r.arrow_left);
    const uint32_t right = cod3::pcmenu::FindPanelPicture(ctx, base, panel, r.arrow_right);
    const uint32_t entry = AddRowEntry(ctx, base, page, kFirstRow + k, k, label, value, left, right);
    if (!entry) {
      REXLOG_WARN("cod3mp: Options > Gameplay: the page didn't take row {}", kFirstRow + k + 1);
      break;
    }
    for (uint32_t e : {label, value, left, right, cod3::pcmenu::FindPanelPicture(ctx, base, panel, r.background),
                       cod3::pcmenu::FindPanelPicture(ctx, base, panel, r.line)})
      SetVisible(ctx, base, e);
    rows.entry[k] = entry;
    rows.shown[k] = ValueText(k);
    rows.rows = k + 1;
  }
  // Every entry the count covers is a row; up from the first and down from
  // the last wrap, as Init links its two.
  const int count = kStockRows + rows.rows;
  WriteU16(base, page + kItemCount, uint16_t(count));
  const uint32_t first = ReadU32(base, list);
  const uint32_t last = ReadU32(base, list + 4 * uint32_t(count - 1));
  if (Valid(first) && Valid(last)) {
    WriteU16(base, first + kEntryUp, uint16_t(count - 1));
    WriteU16(base, last + kEntryDown, 0);
  }
  {
    std::lock_guard lock(g_mutex);
    g_pages[page] = rows;
  }
  if (rows.rows)
    REXLOG_INFO("cod3mp: Options > Gameplay: {} added (cg_fov {}, cg_fovScale {})",
                rows.rows > 1 ? "FIELD OF VIEW and FOV SCALE" : "FIELD OF VIEW", ValueText(0), ValueText(1));
}

// The title and help text while one of our rows is selected.
void ShowSelection(PPCContext& ctx, uint8_t* base, uint32_t page) {
  PageRows rows;
  if (!IsGameplayPage(base, page) || !GetRows(page, rows)) return;
  const int k = ReadS16(base, page + kSelected) - kFirstRow;
  if (k < 0 || k >= rows.rows) return;
  SetTextKey(ctx, base, ReadU32(base, page + kSelectedText), kLabelKeys[k]);
  SetHelp(ctx, base, page, kHelpKeys[k]);
}

// === Pause > OPTIONS: the category pages [menus-categories] =================

namespace op = cod3::options_pages;
namespace vm = cod3::videomenu;

constexpr op::Menu kMenu = op::Menu::kMultiplayerPause;

// The page's own entries (its full-screen Init, 0x82500868): 0-8 its
// options, 9 RESTORE DEFAULTS. Ours follow: the list's items, the rows a
// page shows (one per panel row), and the note line under them.
constexpr int kMatchStock = 10;
constexpr int kMatchOptions = 9;
constexpr int kMatchReset = 9;
constexpr int kMaxCategories = 6;
constexpr int kSlots = 10;  // the panel's rows, slot_01 to slot_10
constexpr int kItemFirst = kMatchStock;                // 10..15: GAMEPLAY, CONTROLS ...
constexpr int kRowFirst = kItemFirst + kMaxCategories;  // 16..25: a page's rows
constexpr int kNoteEntry = kRowFirst + kSlots;          // 26: the note
constexpr int kMatchAll = kNoteEntry + 1;

constexpr uint32_t kPageAddItem = 16;      // vtbl: (index, label, 0), as Init adds RESTORE DEFAULTS
constexpr uint32_t kEntryLabel = 16;       // a plain item's label; a combo box's value text (kComboValueText)
constexpr uint32_t kEntryDisabled = 22;    // u8: the moves, the mouse and left / right skip it
constexpr uint32_t kEntryLeft = 20;        // vtbl slot 5: one step left (returns -1: no move)
constexpr uint32_t kEntryRight = 24;       // vtbl slot 6
constexpr uint32_t kEntryGetValue = 200;   // vtbl: a combo box's choice, a gauge's step
constexpr uint32_t kGaugeMax = 0x20;       // s32: a gauge entry's top step (Init: 37 and 50)
constexpr uint32_t kTextString = 28;       // FEText: String*, its characters at +12
constexpr const char* kOwnTitleKey = "MPGAME_CONTROLLER";  // what Init gives text_title ("OPTIONS")

// What each of the page's own rows shows on the panel (hidden while ours do).
constexpr const char* kOwnRowParts[] = {"text_option", "text_spec",    "arrow_left",  "arrow_right",
                                        "gauge_use",   "gauge_back_a", "gauge_back_b"};
constexpr int kPartCount = int(sizeof(kOwnRowParts) / sizeof(kOwnRowParts[0]));

uint32_t GrowSet(Call& c, uint8_t* base, uint32_t old, int keep, int size) {
  if (!Valid(old)) return 0;
  const uint32_t list = static_cast<uint32_t>(c.Invoke(kOperatorNew, {uint64_t(size) * 4}));
  if (!Valid(list)) return 0;
  for (int i = 0; i < size; ++i)
    WriteU32(base, list + 4 * uint32_t(i), i < keep ? ReadU32(base, old + 4 * uint32_t(i)) : 0);
  return list;
}

float TextYOf(uint8_t* base, uint32_t text) { return ReadF32(base, text + kTextY); }

// The y of a picture's first vertex (pictures are placed by their vertices).
bool PictureY(uint8_t* base, uint32_t quad, float* y) {
  if (!Valid(quad)) return false;
  const uint32_t parts = ReadU32(base, quad + kQuadParts);
  if (!Valid(parts)) return false;
  const uint32_t part = ReadU32(base, parts);
  if (!Valid(part)) return false;
  *y = ReadF32(base, part + kQuadPartVerts + 4);
  return std::isfinite(*y);
}

// One row of a page: a combo box entry on copies of row nine's label, value
// and arrows.
struct RowCopy {
  uint32_t entry = 0;
  uint32_t label = 0, value = 0, left = 0, right = 0;
};

struct MatchPage {
  uint32_t panel = 0;
  int categories = 0;                                  // items on the list
  std::array<uint32_t, kMaxCategories> item{};         // their entries
  std::array<uint32_t, kMaxCategories> item_label{};   // copies of RESTORE DEFAULTS' label
  std::array<RowCopy, kSlots> row{};
  int rows_made = 0;
  uint32_t note_entry = 0, note_label = 0;
  // The panel's own elements: each option row's parts, RESTORE DEFAULTS'
  // label, row nine's and row eight's (ours are placed from them), the lines
  // between the rows and the title.
  std::array<std::array<uint32_t, kPartCount>, kMatchOptions> own{};
  std::array<uint32_t, kMatchOptions> own_entry{};
  uint32_t reset_entry = 0, reset_label = 0;
  uint32_t label9 = 0, value9 = 0, left9 = 0, right9 = 0, label8 = 0, left8 = 0;
  std::array<uint32_t, kSlots - 1> line{};  // bkg_line_01..09: under rows one to nine
  uint32_t title = 0;
  // What shows now.
  int page = -1;        // -1: the list; else the page shown (op::PageAt)
  int list_item = 0;    // the list item to highlight on the list
  int rows_shown = 0;   // rows of the page shown
  std::array<std::string, kSlots> label_text, value_text;
  std::string note_text, title_text;
  std::chrono::steady_clock::time_point last_drawn{};
};
std::unordered_map<uint32_t, MatchPage> g_match;  // keyed by page; g_mutex

bool GetMatch(uint32_t page, MatchPage& out) {
  std::lock_guard lock(g_mutex);
  auto it = g_match.find(page);
  if (it == g_match.end()) return false;
  out = it->second;
  return true;
}

void PutMatch(uint32_t page, const MatchPage& m) {
  std::lock_guard lock(g_mutex);
  if (auto it = g_match.find(page); it != g_match.end()) it->second = m;
}

// The full-screen layout with our rows is the one shown.
bool OursShown(uint8_t* base, uint32_t page, const MatchPage& m) {
  return IsMatchPage(base, page) && ReadU32(base, page + kMatchSplit) == 0 &&
         ReadU32(base, page + kMatchPanel) == m.panel;
}

std::string TextOf(uint8_t* base, uint32_t text) {
  if (!Valid(text)) return {};
  const uint32_t str = ReadU32(base, text + kTextString);
  return Valid(str) ? cod3::guest::ReadString(base, str + 12, 128) : std::string();
}

void SetShown(uint8_t* base, uint32_t elem, bool shown) {
  if (!Valid(elem)) return;
  const uint8_t v = ReadU8(base, elem + kElemVisibility);
  base[elem + kElemVisibility] = uint8_t(shown ? (v | 0x4) : (v & ~0x4));
}

void SetEntryOn(uint8_t* base, uint32_t entry, bool on) {
  if (Valid(entry)) base[entry + kEntryDisabled] = on ? 0 : 1;
}

// Up and down go through `order` (entry indices) and wrap.
void LinkEntries(uint8_t* base, uint32_t full, const std::vector<int>& order) {
  const int n = int(order.size());
  for (int i = 0; i < n; ++i) {
    const uint32_t entry = ReadU32(base, full + 4 * uint32_t(order[i]));
    if (!Valid(entry)) continue;
    WriteU16(base, entry + kEntryUp, uint16_t(order[(i + n - 1) % n]));
    WriteU16(base, entry + kEntryDown, uint16_t(order[(i + 1) % n]));
  }
}

// --- a row's label, value, step and note -----------------------------------

std::string RowLabel(uint8_t* base, const MatchPage& m, const op::Row& row) {
  switch (row.source) {
    case op::Source::kNative:
      return row.index >= 0 && row.index < kMatchOptions ? TextOf(base, m.own[row.index][0]) : std::string();
    case op::Source::kFov:
      return row.index ? "FOV SCALE" : "FIELD OF VIEW";
    case op::Source::kVideo:
      return vm::LabelText(row.index);
    default:
      return {};  // the campaign's rows: not on this page
  }
}

std::string RowValue(PPCContext& ctx, uint8_t* base, const MatchPage& m, const op::Row& row) {
  switch (row.source) {
    case op::Source::kNative: {
      if (row.index < 0 || row.index >= kMatchOptions) return {};
      const uint32_t entry = m.own_entry[row.index];
      if (!Valid(entry)) return {};
      if (ReadU32(base, entry) == kComboVtable) return TextOf(base, ReadU32(base, entry + kComboValueText));
      // A gauge (the sensitivities): its step, of its top one.
      const int value = int32_t(VCall(ctx, base, entry, kEntryGetValue));
      const int top = int32_t(ReadU32(base, entry + kGaugeMax));
      return top > 0 ? std::to_string(value) + " / " + std::to_string(top) : std::to_string(value);
    }
    case op::Source::kFov:
      return ValueText(row.index);
    case op::Source::kVideo:
      return vm::Value(row.index);
    default:
      return {};
  }
}

// One step left or right; the page's own options take their own step (sound
// and all), ours change the setting at once and save it like F2.
void StepRow(PPCContext& ctx, uint8_t* base, const MatchPage& m, const op::Row& row, bool right) {
  switch (row.source) {
    case op::Source::kNative:
      if (row.index >= 0 && row.index < kMatchOptions)
        VCall(ctx, base, m.own_entry[row.index], right ? kEntryRight : kEntryLeft);
      break;
    case op::Source::kFov:
      StepValue(ctx, base, row.index, right);
      break;
    case op::Source::kVideo:
      vm::Step(ctx, base, row.index, right);
      break;
    default:
      break;
  }
}

std::string RowNote(const op::Row& row) {
  return row.source == op::Source::kVideo ? vm::Note(row.index) : std::string();
}

const op::Row* RowOfPage(const MatchPage& m, int k) {
  const op::Page* p = op::PageAt(kMenu, m.page);
  return p && k >= 0 && k < p->count && k < kSlots ? &p->rows[k] : nullptr;
}

// --- the list and the pages ---------------------------------------------------

void SetText(PPCContext& ctx, uint8_t* base, uint32_t elem, std::string& shown, const std::string& text) {
  if (!Valid(elem)) return;
  if (shown == text && TextOf(base, elem) == text) return;  // the game may have set it again
  SetTextRaw(ctx, base, elem, text);
  shown = text;
}

void ShowTitle(PPCContext& ctx, uint8_t* base, MatchPage& m) {
  const op::Page* p = m.page >= 0 ? op::PageAt(kMenu, m.page) : nullptr;
  const char* want = p ? op::Text(p->title_key) : nullptr;
  const std::string key = want ? want : kOwnTitleKey;
  if (key == m.title_text) return;
  if (want)
    SetTextRaw(ctx, base, m.title, want);
  else
    SetTextKey(ctx, base, m.title, kOwnTitleKey);
  m.title_text = key;
}

// The values and the note as they are now (F2 or the console may have changed
// a setting meanwhile).
void ShowRowValues(PPCContext& ctx, uint8_t* base, uint32_t page, MatchPage& m) {
  for (int k = 0; k < m.rows_shown; ++k) {
    const op::Row* row = RowOfPage(m, k);
    if (!row) continue;
    SetText(ctx, base, m.row[k].label, m.label_text[k], RowLabel(base, m, *row));
    SetText(ctx, base, m.row[k].value, m.value_text[k], RowValue(ctx, base, m, *row));
  }
  std::string note;
  if (m.page >= 0) {
    const int k = ReadS16(base, page + kSelected) - kRowFirst;
    if (const op::Row* row = k >= 0 && k < m.rows_shown ? RowOfPage(m, k) : nullptr) note = RowNote(*row);
  }
  if (note.empty()) note = " ";
  if (Valid(m.note_label) && (note != m.note_text || TextOf(base, m.note_label) != note)) {
    SetTextRaw(ctx, base, m.note_label, note);
    m.note_text = note;
  }
}

// The list: the categories and RESTORE DEFAULTS under them.
void ShowList(PPCContext& ctx, uint8_t* base, uint32_t page, MatchPage& m) {
  const uint32_t full = ReadU32(base, page + kMatchFullSet);
  m.page = -1;
  m.rows_shown = 0;
  std::vector<int> order;
  for (int i = 0; i < m.categories; ++i) {
    SetEntryOn(base, m.item[i], true);
    order.push_back(kItemFirst + i);
  }
  for (const RowCopy& r : m.row) SetEntryOn(base, r.entry, false);
  SetEntryOn(base, m.reset_entry, true);
  order.push_back(kMatchReset);
  if (Valid(full)) LinkEntries(base, full, order);
  ShowTitle(ctx, base, m);
  ShowRowValues(ctx, base, page, m);
  const int sel = m.list_item >= 0 && m.list_item < m.categories ? kItemFirst + m.list_item : kItemFirst;
  VCall(ctx, base, page, kPageSetSelection, {uint64_t(sel), 1});
}

// Page `index` of the list (A on its item).
void ShowCategory(PPCContext& ctx, uint8_t* base, uint32_t page, MatchPage& m, int index) {
  const op::Page* p = op::PageAt(kMenu, index);
  const uint32_t full = ReadU32(base, page + kMatchFullSet);
  if (!p || !Valid(full)) return;
  m.page = index;
  m.list_item = index;
  m.rows_shown = std::min({p->count, m.rows_made, kSlots});
  std::vector<int> order;
  for (int i = 0; i < m.categories; ++i) SetEntryOn(base, m.item[i], false);
  SetEntryOn(base, m.reset_entry, false);
  for (int k = 0; k < kSlots; ++k) {
    const bool used = k < m.rows_shown;
    SetEntryOn(base, m.row[k].entry, used);
    if (used) order.push_back(kRowFirst + k);
    m.label_text[k].clear();
    m.value_text[k].clear();
  }
  if (!order.empty()) LinkEntries(base, full, order);
  ShowTitle(ctx, base, m);
  ShowRowValues(ctx, base, page, m);
  if (!order.empty()) VCall(ctx, base, page, kPageSetSelection, {uint64_t(order.front()), 1});
  REXLOG_INFO("cod3mp: Pause > OPTIONS: {} ({} rows)", op::Text(p->button_key) ? op::Text(p->button_key) : "?",
              m.rows_shown);
}

// --- placing and drawing --------------------------------------------------------

// Our copies placed on the panel's rows (from row nine, the row pitch measured
// from rows eight and nine as the panel has them now), the page's own rows
// hidden, RESTORE DEFAULTS under the list. False when the rows don't look as
// expected (then nothing of ours is drawn).
bool PlaceMatch(uint8_t* base, const MatchPage& m) {
  const float pitch = TextYOf(base, m.label9) - TextYOf(base, m.label8);
  if (!(pitch > 5.0f && pitch < 60.0f)) return false;
  float pitch_pictures = pitch, y8 = 0.0f, y9 = 0.0f;
  if (PictureY(base, m.left8, &y8) && PictureY(base, m.left9, &y9) && y9 - y8 > 5.0f && y9 - y8 < 60.0f)
    pitch_pictures = y9 - y8;
  const bool list = m.page < 0;
  // Row s of the panel (0 = slot_01) is (s - 8) rows from row nine.
  auto rows_from_nine = [](int s) { return float(s - 8); };
  for (int i = 0; i < m.categories; ++i) {
    cod3::pcmenu::FollowPanelElement(base, m.label9, m.item_label[i], false, pitch * rows_from_nine(i));
    SetShown(base, m.item_label[i], list);
  }
  for (int k = 0; k < m.rows_made; ++k) {
    const RowCopy& r = m.row[k];
    const bool shown = !list && k < m.rows_shown;
    cod3::pcmenu::FollowPanelElement(base, m.label9, r.label, false, pitch * rows_from_nine(k));
    cod3::pcmenu::FollowPanelElement(base, m.value9, r.value, false, pitch * rows_from_nine(k));
    cod3::pcmenu::FollowPanelElement(base, m.left9, r.left, true, pitch_pictures * rows_from_nine(k));
    cod3::pcmenu::FollowPanelElement(base, m.right9, r.right, true, pitch_pictures * rows_from_nine(k));
    for (uint32_t e : {r.label, r.value, r.left, r.right}) SetShown(base, e, shown);
  }
  // The note: a row under the page's rows (none when the page fills the panel).
  const bool note = !list && m.rows_shown < kSlots && m.note_text != " " && !m.note_text.empty();
  cod3::pcmenu::FollowPanelElement(base, m.label9, m.note_label, false, pitch * rows_from_nine(m.rows_shown));
  SetShown(base, m.note_label, note);
  // The page's own rows stay hidden (their entries are skipped); RESTORE
  // DEFAULTS is the list's last item.
  for (const auto& parts : m.own)
    for (uint32_t e : parts) SetShown(base, e, false);
  WriteF32(base, m.reset_label + kTextY, TextYOf(base, m.label9) + pitch * rows_from_nine(m.categories));
  SetShown(base, m.reset_label, list);
  // The lines between the rows in use.
  const int used = list ? m.categories + 1 : m.rows_shown;
  for (int k = 0; k < kSlots - 1; ++k) SetShown(base, m.line[k], k + 1 < used);
  // Which entries the moves and the mouse may land on (again each frame: the
  // page's refresh may have set them).
  for (uint32_t e : m.own_entry) SetEntryOn(base, e, false);
  SetEntryOn(base, m.reset_entry, list);
  for (int i = 0; i < m.categories; ++i) SetEntryOn(base, m.item[i], list);
  for (int k = 0; k < m.rows_made; ++k) SetEntryOn(base, m.row[k].entry, !list && k < m.rows_shown);
  SetEntryOn(base, m.note_entry, false);
  return true;
}

void FreeMatch(PPCContext& ctx, uint8_t* base, const MatchPage& m) {
  auto drop = [&](uint32_t e) {
    if (Valid(e)) VCall(ctx, base, e, kElemDelete, {1});
  };
  for (uint32_t e : m.item_label) drop(e);
  for (const RowCopy& r : m.row)
    for (uint32_t e : {r.label, r.value, r.left, r.right}) drop(e);
  drop(m.note_label);
}

// --- made after the full-screen Init ---------------------------------------------

// The constructor's three ten-entry arrays get room for ours (the counts
// stay ten until Init has run).
void GrowMatchArrays(PPCContext& ctx, uint8_t* base, uint32_t page) {
  if (!Valid(page) || ReadU32(base, page) != kMatchVtable) return;
  const uint32_t old[3] = {ReadU32(base, page + kItems), ReadU32(base, page + kMatchFullSet),
                           ReadU32(base, page + kMatchSplitSet)};
  if (ReadS16(base, page + kItemCount) != kMatchStock ||
      ReadS16(base, page + kMatchFullSet + kSetCapacity) != kMatchStock ||
      ReadS16(base, page + kMatchSplitSet + kSetCapacity) != kMatchStock || !Valid(old[0]) || !Valid(old[1]) ||
      !Valid(old[2])) {
    REXLOG_WARN("cod3mp: Pause > OPTIONS isn't laid out as expected; it keeps its own rows");
    return;
  }
  Call c(ctx, base);
  uint32_t list[3] = {};
  for (int i = 0; i < 3; ++i) list[i] = GrowSet(c, base, old[i], kMatchStock, kMatchAll);
  if (!list[0] || !list[1] || !list[2]) {
    for (uint32_t l : list)
      if (l) c.Invoke(kOperatorDelete, {l});
    REXLOG_WARN("cod3mp: Pause > OPTIONS: no memory for more rows; it keeps its own rows");
    return;
  }
  WriteU32(base, page + kItems, list[0]);
  WriteU32(base, page + kMatchFullSet, list[1]);
  WriteU16(base, page + kMatchFullSet + kSetCapacity, uint16_t(kMatchAll));
  WriteU32(base, page + kMatchSplitSet, list[2]);
  WriteU16(base, page + kMatchSplitSet + kSetCapacity, uint16_t(kMatchAll));
  for (uint32_t o : old) c.Invoke(kOperatorDelete, {o});
}

// A plain item on a copy of `like` (a label), at `index`; 0 if the page
// didn't take it. Its copy goes to `copy_out` either way (freed with ours).
uint32_t AddPlainItem(PPCContext& ctx, uint8_t* base, uint32_t page, int index, uint32_t like,
                      const std::string& text, uint32_t* copy_out) {
  const uint32_t label = cod3::pcmenu::CopyPanelElement(ctx, base, like, false);
  *copy_out = label;
  if (!label) return 0;
  SetTextRaw(ctx, base, label, text);
  VCall(ctx, base, page, kPageAddItem, {uint64_t(index), label, 0});
  const uint32_t list = ReadU32(base, page + kItems);
  const uint32_t entry = Valid(list) ? ReadU32(base, list + 4 * uint32_t(index)) : 0;
  return Valid(entry) && ReadU32(base, entry + kEntryLabel) == label ? entry : 0;
}

void MakeMatchRows(PPCContext& ctx, uint8_t* base, uint32_t page) {
  if (!IsMatchPage(base, page)) return;
  vm::NoteStartValues();  // which video rows wait for a restart
  const uint32_t panel = ReadU32(base, page + kMatchPanel);
  const uint32_t full = ReadU32(base, page + kMatchFullSet);
  if (!Valid(panel) || !Valid(full)) return;
  cod3::pcmenu::DumpMenuPanel(base, panel, "multiplayer Pause > OPTIONS");
  // A panel made again: the old copies go (the game deleted the entries).
  MatchPage old;
  bool had = false;
  {
    std::lock_guard lock(g_mutex);
    if (auto it = g_match.find(page); it != g_match.end()) {
      old = it->second;
      had = true;
      g_match.erase(it);
    }
  }
  if (had) FreeMatch(ctx, base, old);
  auto set_count = [&](int count) {
    WriteU16(base, page + kMatchFullSet + kSetCount, uint16_t(count));
    for (int i = count; i < kMatchAll; ++i) WriteU32(base, full + 4 * uint32_t(i), 0);
  };
  if (ReadS16(base, page + kMatchFullSet + kSetCapacity) < kMatchAll) {
    set_count(kMatchStock);
    return;  // the constructor's hook said why
  }
  auto find_text = [&](const std::string& name) { return cod3::pcmenu::FindPanelText(ctx, base, panel, name.c_str()); };
  auto find_picture = [&](const std::string& name) {
    return cod3::pcmenu::FindPanelPicture(ctx, base, panel, name.c_str());
  };
  MatchPage m;
  m.panel = panel;
  m.title = find_text("text_title");
  m.label9 = find_text("slot_09_text_option");
  m.value9 = find_text("slot_09_text_spec");
  m.left9 = find_picture("slot_09_arrow_left");
  m.right9 = find_picture("slot_09_arrow_right");
  m.label8 = find_text("slot_08_text_option");
  m.left8 = find_picture("slot_08_arrow_left");
  m.reset_label = find_text("slot_10_text_option");
  m.reset_entry = ReadU32(base, full + 4 * uint32_t(kMatchReset));
  char name[32];
  for (int r = 0; r < kMatchOptions; ++r) {
    m.own_entry[r] = ReadU32(base, full + 4 * uint32_t(r));
    for (int p = 0; p < kPartCount; ++p) {
      std::snprintf(name, sizeof(name), "slot_%02d_%s", r + 1, kOwnRowParts[p]);
      m.own[r][p] = p < 2 ? find_text(name) : find_picture(name);
    }
  }
  for (int k = 0; k < kSlots - 1; ++k) {
    std::snprintf(name, sizeof(name), "bkg_line_%02d", k + 1);
    m.line[k] = find_picture(name);
  }
  bool ok = m.title && m.label9 && m.value9 && m.label8 && m.reset_label && Valid(m.reset_entry);
  for (int r = 0; r < kMatchOptions && ok; ++r) ok = Valid(m.own_entry[r]) && m.own[r][0];
  if (!ok) {
    REXLOG_WARN("cod3mp: Pause > OPTIONS: the panel isn't as expected; it keeps its own rows");
    set_count(kMatchStock);
    return;
  }
  // The list's items, copies of RESTORE DEFAULTS' label.
  const int categories = std::min(op::PageCount(kMenu), kMaxCategories);
  for (int i = 0; i < categories; ++i) {
    const op::Page* p = op::PageAt(kMenu, i);
    const char* text = p ? op::Text(p->button_key) : nullptr;
    const uint32_t entry =
        AddPlainItem(ctx, base, page, kItemFirst + i, m.reset_label, text ? text : "?", &m.item_label[i]);
    if (!entry) break;
    WriteU32(base, full + 4 * uint32_t(kItemFirst + i), entry);
    m.item[i] = entry;
    m.categories = i + 1;
  }
  // A page's rows: combo boxes on copies of row nine.
  const int rows = std::min(op::MostRows(kMenu), kSlots);
  for (int k = 0; k < rows && m.categories == categories; ++k) {
    RowCopy& r = m.row[k];
    r.label = cod3::pcmenu::CopyPanelElement(ctx, base, m.label9, false);
    r.value = cod3::pcmenu::CopyPanelElement(ctx, base, m.value9, false);
    r.left = cod3::pcmenu::CopyPanelElement(ctx, base, m.left9, true);
    r.right = cod3::pcmenu::CopyPanelElement(ctx, base, m.right9, true);
    const int index = kRowFirst + k;
    r.entry = r.label && r.value ? AddRowEntry(ctx, base, page, index, 0, r.label, r.value, r.left, r.right) : 0;
    if (!r.entry) break;
    WriteU32(base, full + 4 * uint32_t(index), r.entry);
    m.rows_made = k + 1;
  }
  // The note line, a plain item that is never on.
  m.note_entry = m.rows_made == rows ? AddPlainItem(ctx, base, page, kNoteEntry, m.label9, " ", &m.note_label) : 0;
  if (m.categories < categories || m.rows_made < rows || !m.note_entry) {
    REXLOG_WARN("cod3mp: Pause > OPTIONS: the page didn't take our rows ({} of {} items, {} of {} rows); "
                "it keeps its own rows",
                m.categories, categories, m.rows_made, rows);
    FreeMatch(ctx, base, m);
    set_count(kMatchStock);
    return;
  }
  WriteU32(base, full + 4 * uint32_t(kNoteEntry), m.note_entry);
  set_count(kMatchAll);
  SetEntryOn(base, m.note_entry, false);
  for (uint32_t e : m.own_entry) SetEntryOn(base, e, false);
  ShowList(ctx, base, page, m);
  PlaceMatch(base, m);
  {
    std::lock_guard lock(g_mutex);
    g_match[page] = m;
  }
  REXLOG_INFO("cod3mp: Pause > OPTIONS: category pages ({} categories, up to {} rows)", m.categories, m.rows_made);
}

// The highlight on one of the page's own entries that ours stand in for (the
// page's refresh puts back the one it remembers): to the list's item, or the
// page's first row.
void KeepSelection(PPCContext& ctx, uint8_t* base, uint32_t page) {
  MatchPage m;
  if (!GetMatch(page, m) || !OursShown(base, page, m)) return;
  const int sel = ReadS16(base, page + kSelected);
  const bool on_list = sel >= kItemFirst && sel < kItemFirst + m.categories;
  const bool on_rows = sel >= kRowFirst && sel < kRowFirst + m.rows_shown;
  if (m.page < 0 ? (on_list || sel == kMatchReset) : on_rows) return;
  const int to = m.page < 0 ? kItemFirst + std::clamp(m.list_item, 0, std::max(0, m.categories - 1)) : kRowFirst;
  VCall(ctx, base, page, kPageSetSelection, {uint64_t(to), 1});
}

// Forgets a page's rows and frees our copies (the game deleted the entries).
// When its panel went (the page lives on), the full-screen count goes back to
// ten until Init adds ours again.
void DropMatchRows(PPCContext& ctx, uint8_t* base, uint32_t page, bool panel_gone) {
  MatchPage m;
  {
    std::lock_guard lock(g_mutex);
    auto it = g_match.find(page);
    if (it == g_match.end()) return;
    if (panel_gone && ReadU32(base, page + kMatchPanel) == it->second.panel) return;
    m = it->second;
    g_match.erase(it);
  }
  if (panel_gone && IsMatchPage(base, page)) {
    const uint32_t full = ReadU32(base, page + kMatchFullSet);
    WriteU16(base, page + kMatchFullSet + kSetCount, uint16_t(kMatchStock));
    if (Valid(full))
      for (int i = kMatchStock; i < kMatchAll; ++i) WriteU32(base, full + 4 * uint32_t(i), 0);
    // (The page's own entries went with the panel; Init makes new ones.)
  }
  FreeMatch(ctx, base, m);
}

// --- the buttons ----------------------------------------------------------------------

// A: on the list's item, its page; on a page's row, one step right (Enter
// on the keyboard does the same). False: the page's own (RESTORE DEFAULTS).
bool MatchAccept(PPCContext& ctx, uint8_t* base, uint32_t page) {
  MatchPage m;
  if (!GetMatch(page, m) || !OursShown(base, page, m)) return false;
  const int sel = ReadS16(base, page + kSelected);
  if (m.page < 0) {
    if (sel < kItemFirst || sel >= kItemFirst + m.categories) return false;
    ShowCategory(ctx, base, page, m, sel - kItemFirst);
    PutMatch(page, m);
    return true;
  }
  if (const op::Row* row = RowOfPage(m, sel - kRowFirst)) {
    StepRow(ctx, base, m, *row, true);
    ShowRowValues(ctx, base, page, m);
    PutMatch(page, m);
  }
  return true;
}

// B: on a page, back to the list. False on the list: the page's own (it
// asks to apply the changed options, then closes).
bool MatchBack(PPCContext& ctx, uint8_t* base, uint32_t page) {
  MatchPage m;
  if (!GetMatch(page, m) || !OursShown(base, page, m) || m.page < 0) return false;
  ShowList(ctx, base, page, m);
  PutMatch(page, m);
  REXLOG_INFO("cod3mp: Pause > OPTIONS: back to the list");
  return true;
}

// The help line: on a list item, the one with SELECT that RESTORE DEFAULTS
// has (the page's update shows its BACK / MOVE one on every other row).
constexpr uint32_t kHelpBack = 60;    // FEMultiLineText*: "BACK, MOVE"
constexpr uint32_t kHelpSelect = 64;  // FEMultiLineText*: "SELECT, BACK, MOVE"

void ShowHelpLine(PPCContext& ctx, uint8_t* base, uint32_t page, const MatchPage& m) {
  const int sel = ReadS16(base, page + kSelected);
  if (m.page >= 0 || sel < kItemFirst || sel >= kItemFirst + m.categories) return;
  const uint32_t back = ReadU32(base, page + kHelpBack), select = ReadU32(base, page + kHelpSelect);
  if (!Valid(back) || !Valid(select)) return;
  VCall(ctx, base, back, kElemSetVisible, {0});
  VCall(ctx, base, select, kElemSetVisible, {1});
}

// What a combo box entry is to Pause > OPTIONS.
enum RowKind : int {
  kNotOurs,      // not one of our rows
  kOurSetting,   // a row of a port setting (or a row not in use)
  kOurNative,    // a row standing for one of the page's own options
};

RowKind MatchRowKind(uint8_t* base, uint32_t entry) {
  if (!Valid(entry) || ReadU32(base, entry) != kComboVtable) return kNotOurs;
  std::lock_guard lock(g_mutex);
  for (const auto& [page, m] : g_match) {
    for (int k = 0; k < m.rows_made; ++k) {
      if (m.row[k].entry != entry) continue;
      const op::Row* row = m.page >= 0 && k < m.rows_shown ? RowOfPage(m, k) : nullptr;
      return row && row->source == op::Source::kNative ? kOurNative : kOurSetting;
    }
  }
  return kNotOurs;
}

// Left / right on one of our rows (`entry`): true if it was ours.
bool MatchStep(PPCContext& ctx, uint8_t* base, uint32_t entry, bool right) {
  if (!Valid(entry) || ReadU32(base, entry) != kComboVtable) return false;
  uint32_t page = 0;
  MatchPage m;
  int k = -1;
  {
    std::lock_guard lock(g_mutex);
    for (const auto& [p, mp] : g_match) {
      for (int i = 0; i < mp.rows_made; ++i)
        if (mp.row[i].entry == entry) {
          page = p;
          m = mp;
          k = i;
        }
    }
  }
  if (k < 0 || !IsMatchPage(base, page)) return false;
  if (const op::Row* row = m.page >= 0 && k < m.rows_shown ? RowOfPage(m, k) : nullptr) {
    StepRow(ctx, base, m, *row, right);
    ShowRowValues(ctx, base, page, m);
    PutMatch(page, m);
  }
  return true;
}

}  // namespace

// === Main menu > Options > Gameplay hooks ======================================

// MPOptionsGameplayMenu's constructor (this, system).
REX_EXTERN(__imp__MPOptionsGameplayMenu__ctor);
REX_HOOK_RAW(MPOptionsGameplayMenu__ctor) {
  const uint32_t page = ctx.r3.u32;
  __imp__MPOptionsGameplayMenu__ctor(ctx, base);
  GrowEntryArray(ctx, base, page);
}

// Init (this, panel): the page's own two rows, then ours.
REX_EXTERN(__imp__sub_823F11A8);
REX_HOOK_RAW(sub_823F11A8) {
  const uint32_t page = ctx.r3.u32;
  __imp__sub_823F11A8(ctx, base);
  AddRows(ctx, base, page);
}

// Refresh (vtbl slot 28): its title and help text by the selection, then the
// profile's values into its two rows.
REX_EXTERN(__imp__sub_823EE650);
REX_HOOK_RAW(sub_823EE650) {
  const uint32_t page = ctx.r3.u32;
  __imp__sub_823EE650(ctx, base);
  ShowSelection(ctx, base, page);
  ShowValues(ctx, base, page);
}

// The page's two moves (vtbl slots 32 / 33): FEMenu's, then the title and help
// text by the selection.
REX_EXTERN(__imp__MPOptionsGameplayMenu__vfunc_32);
REX_HOOK_RAW(MPOptionsGameplayMenu__vfunc_32) {
  const uint32_t page = ctx.r3.u32;
  __imp__MPOptionsGameplayMenu__vfunc_32(ctx, base);
  ShowSelection(ctx, base, page);
}

REX_EXTERN(__imp__MPOptionsGameplayMenu__vfunc_33);
REX_HOOK_RAW(MPOptionsGameplayMenu__vfunc_33) {
  const uint32_t page = ctx.r3.u32;
  __imp__MPOptionsGameplayMenu__vfunc_33(ctx, base);
  ShowSelection(ctx, base, page);
}

// Update (vtbl slot 23, this, f1 = dt).
REX_EXTERN(__imp__MPOptionsGameplayMenu__vfunc_23);
REX_HOOK_RAW(MPOptionsGameplayMenu__vfunc_23) {
  const uint32_t page = ctx.r3.u32;
  __imp__MPOptionsGameplayMenu__vfunc_23(ctx, base);
  ShowValues(ctx, base, page);
}

// === Pause > OPTIONS hooks [menus-categories] =================================

// InGameOptionsMenu's constructor (this, ...); AARInGameOptionsMenu's calls it.
REX_EXTERN(__imp__InGameOptionsMenu__ctor);
REX_HOOK_RAW(InGameOptionsMenu__ctor) {
  const uint32_t page = ctx.r3.u32;
  __imp__InGameOptionsMenu__ctor(ctx, base);
  GrowMatchArrays(ctx, base, page);
}

// The full-screen layout's Init (this, panel), both pages'.
REX_EXTERN(__imp__sub_82500868);
REX_HOOK_RAW(sub_82500868) {
  const uint32_t page = ctx.r3.u32;
  __imp__sub_82500868(ctx, base);
  MakeMatchRows(ctx, base, page);
}

// The page's draw (vtbl slot 20, both pages): ours placed and the page's own
// rows hidden first (the entries draw their texts), then our arrows drawn.
// Opened again after a while (the page lives on between visits): the list.
REX_EXTERN(__imp__InGameOptionsMenu__vfunc_20);
REX_HOOK_RAW(InGameOptionsMenu__vfunc_20) {
  const uint32_t page = ctx.r3.u32;
  MatchPage m;
  bool placed = false;
  if (GetMatch(page, m) && OursShown(base, page, m)) {
    const auto now = std::chrono::steady_clock::now();
    if (m.page >= 0 && now - m.last_drawn > std::chrono::milliseconds(300)) {
      m.list_item = std::max(0, m.page);
      ShowList(ctx, base, page, m);
    }
    m.last_drawn = now;
    placed = PlaceMatch(base, m);
    PutMatch(page, m);
  }
  __imp__InGameOptionsMenu__vfunc_20(ctx, base);
  if (!placed) return;
  for (int k = 0; k < m.rows_made; ++k)
    for (uint32_t e : {m.row[k].left, m.row[k].right})
      if (Valid(e) && (ReadU8(base, e + kElemVisibility) & 0x4)) VCall(ctx, base, e, kElemDraw);
}

// Refresh (both pages): the layout's entries into FEMenu's array, the
// profile's values into the entries; then ours.
REX_EXTERN(__imp__sub_824EB138);
REX_HOOK_RAW(sub_824EB138) {
  const uint32_t page = ctx.r3.u32;
  __imp__sub_824EB138(ctx, base);
  KeepSelection(ctx, base, page);
  MatchPage m;
  if (GetMatch(page, m) && OursShown(base, page, m)) {
    ShowRowValues(ctx, base, page, m);
    PutMatch(page, m);
  }
}

// Slot 63 (both pages): the layout's entries into FEMenu's array.
REX_EXTERN(__imp__InGameOptionsMenu__vfunc_63);
REX_HOOK_RAW(InGameOptionsMenu__vfunc_63) {
  const uint32_t page = ctx.r3.u32;
  __imp__InGameOptionsMenu__vfunc_63(ctx, base);
  KeepSelection(ctx, base, page);
}

// Update (vtbl slot 23; the after-match page's calls it): the values as they
// are now, and the note of the row highlighted.
REX_EXTERN(__imp__InGameOptionsMenu__vfunc_23);
REX_HOOK_RAW(InGameOptionsMenu__vfunc_23) {
  const uint32_t page = ctx.r3.u32;
  __imp__InGameOptionsMenu__vfunc_23(ctx, base);
  MatchPage m;
  if (GetMatch(page, m) && OursShown(base, page, m)) {
    ShowRowValues(ctx, base, page, m);
    ShowHelpLine(ctx, base, page, m);
    PutMatch(page, m);
  }
}

// A (vtbl slot 36, both pages): a list item opens its page; RESTORE DEFAULTS
// is the page's own.
REX_EXTERN(__imp__InGameOptionsMenu__vfunc_36);
REX_HOOK_RAW(InGameOptionsMenu__vfunc_36) {
  if (MatchAccept(ctx, base, ctx.r3.u32)) return;
  __imp__InGameOptionsMenu__vfunc_36(ctx, base);
}

// B (vtbl slot 37): a page goes back to the list; on the list, the page's own
// (apply the changed options, close).
REX_EXTERN(__imp__InGameOptionsMenu__vfunc_37);
REX_HOOK_RAW(InGameOptionsMenu__vfunc_37) {
  if (MatchBack(ctx, base, ctx.r3.u32)) return;
  __imp__InGameOptionsMenu__vfunc_37(ctx, base);
}

REX_EXTERN(__imp__AARInGameOptionsMenu__vfunc_37);
REX_HOOK_RAW(AARInGameOptionsMenu__vfunc_37) {
  if (MatchBack(ctx, base, ctx.r3.u32)) return;
  __imp__AARInGameOptionsMenu__vfunc_37(ctx, base);
}

// FESplitScreenMenu slot 1 (every split-screen-capable page's): lets the
// panels go and deletes the entries; then our copies of a page whose panel
// went.
REX_EXTERN(__imp__FESplitScreenMenu__vfunc_1);
REX_HOOK_RAW(FESplitScreenMenu__vfunc_1) {
  const uint32_t page = ctx.r3.u32;
  __imp__FESplitScreenMenu__vfunc_1(ctx, base);
  DropMatchRows(ctx, base, page, true);
}

REX_EXTERN(__imp__InGameOptionsMenu__dtor);
REX_HOOK_RAW(InGameOptionsMenu__dtor) {
  const uint32_t page = ctx.r3.u32;
  __imp__InGameOptionsMenu__dtor(ctx, base);
  DropMatchRows(ctx, base, page, false);
}

// === Both: FEComboBox left (slot 5) and right (slot 6), every combo box's =====
// Ours step the setting after the entry's own (arrow flash, sound); a row
// that stands for one of the page's own options takes that option's step
// instead (its own sound).
REX_EXTERN(__imp__FEComboBox__vfunc_5);
REX_HOOK_RAW(FEComboBox__vfunc_5) {
  const uint32_t entry = ctx.r3.u32;
  uint32_t page = 0;
  const int row = RowOf(base, entry, &page);
  if (row >= 0) {
    if (HasValue(base, entry))
      __imp__FEComboBox__vfunc_5(ctx, base);
    else
      ctx.r3.u64 = ~uint64_t(0);  // what it returns
    Stepped(ctx, base, entry, page, row, false);
    return;
  }
  const int kind = MatchRowKind(base, entry);
  if (kind == kNotOurs) {
    __imp__FEComboBox__vfunc_5(ctx, base);
    return;
  }
  if (kind == kOurSetting && HasValue(base, entry))
    __imp__FEComboBox__vfunc_5(ctx, base);
  else
    ctx.r3.u64 = ~uint64_t(0);
  const uint64_t ret = ctx.r3.u64;
  MatchStep(ctx, base, entry, false);
  ctx.r3.u64 = ret;
}

REX_EXTERN(__imp__FEComboBox__vfunc_6);
REX_HOOK_RAW(FEComboBox__vfunc_6) {
  const uint32_t entry = ctx.r3.u32;
  uint32_t page = 0;
  const int row = RowOf(base, entry, &page);
  if (row >= 0) {
    if (HasValue(base, entry))
      __imp__FEComboBox__vfunc_6(ctx, base);
    else
      ctx.r3.u64 = ~uint64_t(0);
    Stepped(ctx, base, entry, page, row, true);
    return;
  }
  const int kind = MatchRowKind(base, entry);
  if (kind == kNotOurs) {
    __imp__FEComboBox__vfunc_6(ctx, base);
    return;
  }
  if (kind == kOurSetting && HasValue(base, entry))
    __imp__FEComboBox__vfunc_6(ctx, base);
  else
    ctx.r3.u64 = ~uint64_t(0);
  const uint64_t ret = ctx.r3.u64;
  MatchStep(ctx, base, entry, true);
  ctx.r3.u64 = ret;
}
