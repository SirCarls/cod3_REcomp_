// cod3mp: FIELD OF VIEW and FOV SCALE in the multiplayer program's own
// option pages, like the campaign's rows (game/menu_fov.cpp). [mp-menus]
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
// match, derives from it and shares what is used here). Its full-screen panel
// (MP_PM_controller, MP_AAR_PM_controller) has ten rows, slot_01 to slot_10,
// and none to spare:
//  * the page keeps its entries in three arrays: +4 (FEMenu's: what is drawn
//    and moved through, count +38), +88 (the full-screen layout's) and +96
//    (the split-screen layout's), each {entries; s16 capacity; s16 count};
//    the page's refresh copies one of the last two into +4 with its count.
//    After the constructor all three hold twelve (the counts stay ten);
//  * after the full-screen Init: FIELD OF VIEW and FOV SCALE are copies of
//    row nine (VIBRATION: its label, value and arrows) one and two rows lower,
//    with copies of the line above row ten, and RESTORE DEFAULTS (row ten)
//    sits two rows lower, inside the page's background and clear of the help
//    bar. They are entries 10 and 11 (FEComboBox, made as Init makes
//    VIBRATION) in the full-screen array, its count 12; the highlight goes
//    VIBRATION, FIELD OF VIEW, FOV SCALE, RESTORE DEFAULTS and on to the top;
//  * entries draw their own texts; the panel draws only its own pictures, so
//    the page's draw places the copies from row nine every frame (their
//    opacity follows its fades) and draws the copied arrows and lines;
//  * the split-screen layout (four rows that scroll through the entries in
//    order, RESTORE DEFAULTS fixed as entry nine) keeps its ten; F2 has both
//    settings there. A selection past the end when the layout changes goes
//    to RESTORE DEFAULTS;
//  * B and RESTORE DEFAULTS read and write entries 0-8 only;
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
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <mutex>
#include <string>
#include <unordered_map>

#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>

#include "game/engine.h"
#include "game/fov.h"
#include "game/guest.h"
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
constexpr int kMatchStock = 10;              // entries 0-9
constexpr int kMatchVibration = 8;           // row nine, the one copied
constexpr int kMatchReset = 9;               // RESTORE DEFAULTS, row ten
constexpr int kMatchFirst = 10;              // FIELD OF VIEW's entry; FOV SCALE's is the next
constexpr int kMatchAll = 12;

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

// Our rows of one page (game thread). Row 0 is FIELD OF VIEW, 1 FOV SCALE.
struct PageRows {
  bool match = false;                 // Pause > OPTIONS in a match
  int rows = 0;                       // 0, 1 or 2
  std::array<uint32_t, 2> entry{};    // their FEComboBox entries
  std::array<std::string, 2> shown;   // the value each shows
  // Pause > OPTIONS: its panel, row nine's and row eight's elements (where
  // ours are placed from), RESTORE DEFAULTS' label, and our copies.
  uint32_t panel = 0;
  uint32_t label9 = 0, value9 = 0, left9 = 0, right9 = 0, line9 = 0, label8 = 0, left8 = 0;
  uint32_t reset_label = 0;
  std::array<uint32_t, 2> label{}, value{}, left{}, right{}, line{};
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

// Where a page keeps the entry of our row `k` (the array it lives in).
uint32_t EntrySlot(uint8_t* base, uint32_t page, bool match, int k) {
  if (match) {
    const uint32_t list = ReadU32(base, page + kMatchFullSet);
    if (!Valid(list) || ReadS16(base, page + kMatchFullSet + kSetCount) <= kMatchFirst + k) return 0;
    return list + 4 * uint32_t(kMatchFirst + k);
  }
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
      if (rows.match ? !IsMatchPage(base, page) : !IsGameplayPage(base, page)) return -1;
      const uint32_t at = EntrySlot(base, page, rows.match, k);
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

// === Pause > OPTIONS (the controller page) ===================================

uint32_t GrowSet(Call& c, uint8_t* base, uint32_t old, int keep, int size) {
  if (!Valid(old)) return 0;
  const uint32_t list = static_cast<uint32_t>(c.Invoke(kOperatorNew, {uint64_t(size) * 4}));
  if (!Valid(list)) return 0;
  for (int i = 0; i < size; ++i)
    WriteU32(base, list + 4 * uint32_t(i), i < keep ? ReadU32(base, old + 4 * uint32_t(i)) : 0);
  return list;
}

// The constructor's three ten-entry arrays become twelve-entry ones (the
// counts stay ten).
void GrowMatchArrays(PPCContext& ctx, uint8_t* base, uint32_t page) {
  if (!Valid(page) || ReadU32(base, page) != kMatchVtable) return;
  const uint32_t old[3] = {ReadU32(base, page + kItems), ReadU32(base, page + kMatchFullSet),
                           ReadU32(base, page + kMatchSplitSet)};
  if (ReadS16(base, page + kItemCount) != kMatchStock ||
      ReadS16(base, page + kMatchFullSet + kSetCapacity) != kMatchStock ||
      ReadS16(base, page + kMatchSplitSet + kSetCapacity) != kMatchStock || !Valid(old[0]) || !Valid(old[1]) ||
      !Valid(old[2])) {
    REXLOG_WARN("cod3mp: Pause > OPTIONS isn't laid out as expected; no FIELD OF VIEW rows");
    return;
  }
  Call c(ctx, base);
  uint32_t list[3] = {};
  for (int i = 0; i < 3; ++i) list[i] = GrowSet(c, base, old[i], kMatchStock, kMatchAll);
  if (!list[0] || !list[1] || !list[2]) {
    for (uint32_t l : list)
      if (l) c.Invoke(kOperatorDelete, {l});
    REXLOG_WARN("cod3mp: Pause > OPTIONS: no memory for more rows; no FIELD OF VIEW rows");
    return;
  }
  WriteU32(base, page + kItems, list[0]);
  WriteU32(base, page + kMatchFullSet, list[1]);
  WriteU16(base, page + kMatchFullSet + kSetCapacity, uint16_t(kMatchAll));
  WriteU32(base, page + kMatchSplitSet, list[2]);
  WriteU16(base, page + kMatchSplitSet + kSetCapacity, uint16_t(kMatchAll));
  for (uint32_t o : old) c.Invoke(kOperatorDelete, {o});
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

// Our copies placed from row nine (the row pitch measured from rows eight
// and nine, as the panel has them now), and RESTORE DEFAULTS under them.
// False when the rows don't look as expected (then nothing is drawn).
bool PlaceMatchRows(uint8_t* base, const PageRows& rows) {
  const float pitch = TextYOf(base, rows.label9) - TextYOf(base, rows.label8);
  if (!(pitch > 5.0f && pitch < 60.0f)) return false;
  float pitch_pictures = pitch, y8 = 0.0f, y9 = 0.0f;
  if (PictureY(base, rows.left8, &y8) && PictureY(base, rows.left9, &y9) && y9 - y8 > 5.0f && y9 - y8 < 60.0f)
    pitch_pictures = y9 - y8;
  for (int k = 0; k < rows.rows; ++k) {
    const float n = float(k + 1);
    cod3::pcmenu::FollowPanelElement(base, rows.label9, rows.label[k], false, pitch * n);
    cod3::pcmenu::FollowPanelElement(base, rows.value9, rows.value[k], false, pitch * n);
    cod3::pcmenu::FollowPanelElement(base, rows.left9, rows.left[k], true, pitch_pictures * n);
    cod3::pcmenu::FollowPanelElement(base, rows.right9, rows.right[k], true, pitch_pictures * n);
    cod3::pcmenu::FollowPanelElement(base, rows.line9, rows.line[k], true, pitch_pictures * n);
  }
  WriteF32(base, rows.reset_label + kTextY, TextYOf(base, rows.label9) + pitch * float(rows.rows + 1));
  return true;
}

void FreeCopies(PPCContext& ctx, uint8_t* base, const PageRows& rows) {
  for (int k = 0; k < 2; ++k)
    for (uint32_t e : {rows.label[k], rows.value[k], rows.left[k], rows.right[k], rows.line[k]})
      if (Valid(e)) VCall(ctx, base, e, kElemDelete, {1});
}

// After the full-screen Init (both pages): FIELD OF VIEW and FOV SCALE as
// entries 10 and 11 on copies of row nine.
void AddMatchRows(PPCContext& ctx, uint8_t* base, uint32_t page) {
  if (!IsMatchPage(base, page)) return;
  const uint32_t panel = ReadU32(base, page + kMatchPanel);
  const uint32_t full = ReadU32(base, page + kMatchFullSet);
  if (!Valid(panel) || !Valid(full)) return;
  cod3::pcmenu::DumpMenuPanel(base, panel, "multiplayer Pause > OPTIONS");
  // A panel made again: the old copies go (the game deleted the entries).
  PageRows old;
  bool had = false;
  {
    std::lock_guard lock(g_mutex);
    if (auto it = g_pages.find(page); it != g_pages.end()) {
      old = it->second;
      had = true;
      g_pages.erase(it);
    }
  }
  if (had && old.match) FreeCopies(ctx, base, old);
  auto set_count = [&](int count) {
    WriteU16(base, page + kMatchFullSet + kSetCount, uint16_t(count));
    for (int i = count; i < kMatchAll; ++i) WriteU32(base, full + 4 * uint32_t(i), 0);
  };
  if (ReadS16(base, page + kMatchFullSet + kSetCapacity) < kMatchAll) {
    set_count(kMatchStock);
    return;  // the constructor's hook said why
  }
  PageRows rows;
  rows.match = true;
  rows.panel = panel;
  rows.label9 = cod3::pcmenu::FindPanelText(ctx, base, panel, "slot_09_text_option");
  rows.value9 = cod3::pcmenu::FindPanelText(ctx, base, panel, "slot_09_text_spec");
  rows.left9 = cod3::pcmenu::FindPanelPicture(ctx, base, panel, "slot_09_arrow_left");
  rows.right9 = cod3::pcmenu::FindPanelPicture(ctx, base, panel, "slot_09_arrow_right");
  rows.line9 = cod3::pcmenu::FindPanelPicture(ctx, base, panel, "bkg_line_09");
  rows.label8 = cod3::pcmenu::FindPanelText(ctx, base, panel, "slot_08_text_option");
  rows.left8 = cod3::pcmenu::FindPanelPicture(ctx, base, panel, "slot_08_arrow_left");
  rows.reset_label = cod3::pcmenu::FindPanelText(ctx, base, panel, "slot_10_text_option");
  const uint32_t vibration = ReadU32(base, full + 4 * uint32_t(kMatchVibration));
  const uint32_t reset = ReadU32(base, full + 4 * uint32_t(kMatchReset));
  if (!rows.label9 || !rows.value9 || !rows.label8 || !rows.reset_label || !Valid(vibration) || !Valid(reset)) {
    REXLOG_WARN("cod3mp: Pause > OPTIONS: rows nine and ten aren't as expected; no FIELD OF VIEW rows");
    set_count(kMatchStock);
    return;
  }
  for (int k = 0; k < 2; ++k) {
    rows.label[k] = cod3::pcmenu::CopyPanelElement(ctx, base, rows.label9, false);
    rows.value[k] = cod3::pcmenu::CopyPanelElement(ctx, base, rows.value9, false);
    rows.left[k] = cod3::pcmenu::CopyPanelElement(ctx, base, rows.left9, true);
    rows.right[k] = cod3::pcmenu::CopyPanelElement(ctx, base, rows.right9, true);
    rows.line[k] = cod3::pcmenu::CopyPanelElement(ctx, base, rows.line9, true);
    const int index = kMatchFirst + k;
    const uint32_t entry =
        rows.label[k] && rows.value[k]
            ? AddRowEntry(ctx, base, page, index, k, rows.label[k], rows.value[k], rows.left[k], rows.right[k])
            : 0;
    if (!entry) {
      REXLOG_WARN("cod3mp: Pause > OPTIONS: row {} not added", index + 1);
      for (uint32_t e : {rows.label[k], rows.value[k], rows.left[k], rows.right[k], rows.line[k]})
        if (Valid(e)) VCall(ctx, base, e, kElemDelete, {1});
      rows.label[k] = rows.value[k] = rows.left[k] = rows.right[k] = rows.line[k] = 0;
      break;
    }
    WriteU32(base, full + 4 * uint32_t(index), entry);
    rows.entry[k] = entry;
    rows.shown[k] = ValueText(k);
    rows.rows = k + 1;
  }
  set_count(kMatchStock + rows.rows);
  // VIBRATION -> FIELD OF VIEW -> FOV SCALE -> RESTORE DEFAULTS (whose down
  // stays Init's: the top row).
  if (rows.rows) {
    const int last = kMatchFirst + rows.rows - 1;
    WriteU16(base, vibration + kEntryDown, uint16_t(kMatchFirst));
    for (int k = 0; k < rows.rows; ++k) {
      const int index = kMatchFirst + k;
      WriteU16(base, rows.entry[k] + kEntryUp, uint16_t(k ? index - 1 : kMatchVibration));
      WriteU16(base, rows.entry[k] + kEntryDown, uint16_t(index == last ? kMatchReset : index + 1));
    }
    WriteU16(base, reset + kEntryUp, uint16_t(last));
    WriteU16(base, reset + kEntryDown, 0);
  }
  PlaceMatchRows(base, rows);
  {
    std::lock_guard lock(g_mutex);
    g_pages[page] = rows;
  }
  REXLOG_INFO("cod3mp: Pause > OPTIONS: {} added", rows.rows > 1   ? "FIELD OF VIEW and FOV SCALE"
                                                       : rows.rows ? "FIELD OF VIEW"
                                                                   : "no row");
}

// A selection past the entries the page has now (our rows, after the layout
// changed to the split-screen one) goes to RESTORE DEFAULTS.
void KeepSelection(PPCContext& ctx, uint8_t* base, uint32_t page) {
  if (!IsMatchPage(base, page)) return;
  const int count = ReadS16(base, page + kItemCount);
  const int sel = ReadS16(base, page + kSelected);
  if (count > kMatchReset && sel >= count) VCall(ctx, base, page, kPageSetSelection, {uint64_t(kMatchReset), 1});
}

// Forgets a page's rows and frees its copies (the game deleted the entries).
// When its panel went (the page lives on), the full-screen count goes back
// to ten until Init adds the rows again.
void DropMatchRows(PPCContext& ctx, uint8_t* base, uint32_t page, bool panel_gone) {
  PageRows rows;
  {
    std::lock_guard lock(g_mutex);
    auto it = g_pages.find(page);
    if (it == g_pages.end() || !it->second.match) return;
    if (panel_gone && ReadU32(base, page + kMatchPanel) == it->second.panel) return;
    rows = it->second;
    g_pages.erase(it);
  }
  if (panel_gone && IsMatchPage(base, page)) {
    const uint32_t full = ReadU32(base, page + kMatchFullSet);
    WriteU16(base, page + kMatchFullSet + kSetCount, uint16_t(kMatchStock));
    if (Valid(full))
      for (int i = kMatchStock; i < kMatchAll; ++i) WriteU32(base, full + 4 * uint32_t(i), 0);
  }
  FreeCopies(ctx, base, rows);
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

// === Pause > OPTIONS hooks ==================================================

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
  AddMatchRows(ctx, base, page);
}

// The page's draw (vtbl slot 20, both pages): our copies placed first (the
// entries draw their texts), then the copied arrows and lines drawn.
REX_EXTERN(__imp__InGameOptionsMenu__vfunc_20);
REX_HOOK_RAW(InGameOptionsMenu__vfunc_20) {
  const uint32_t page = ctx.r3.u32;
  PageRows rows;
  const bool ours = GetRows(page, rows) && rows.match && rows.rows && IsMatchPage(base, page) &&
                    ReadU32(base, page + kMatchSplit) == 0 && ReadU32(base, page + kMatchPanel) == rows.panel;
  const bool placed = ours && PlaceMatchRows(base, rows);
  __imp__InGameOptionsMenu__vfunc_20(ctx, base);
  if (!placed) return;
  for (int k = 0; k < rows.rows; ++k)
    for (uint32_t e : {rows.line[k], rows.left[k], rows.right[k]})
      if (Valid(e) && (ReadU8(base, e + kElemVisibility) & 0x4)) VCall(ctx, base, e, kElemDraw);
}

// Refresh (both pages): the layout's entries into FEMenu's array, the
// profile's values into the entries; then ours.
REX_EXTERN(__imp__sub_824EB138);
REX_HOOK_RAW(sub_824EB138) {
  const uint32_t page = ctx.r3.u32;
  __imp__sub_824EB138(ctx, base);
  KeepSelection(ctx, base, page);
  ShowValues(ctx, base, page);
}

// Slot 63 (both pages): the layout's entries into FEMenu's array.
REX_EXTERN(__imp__InGameOptionsMenu__vfunc_63);
REX_HOOK_RAW(InGameOptionsMenu__vfunc_63) {
  const uint32_t page = ctx.r3.u32;
  __imp__InGameOptionsMenu__vfunc_63(ctx, base);
  KeepSelection(ctx, base, page);
}

// Update (vtbl slot 23; the after-match page's calls it).
REX_EXTERN(__imp__InGameOptionsMenu__vfunc_23);
REX_HOOK_RAW(InGameOptionsMenu__vfunc_23) {
  const uint32_t page = ctx.r3.u32;
  __imp__InGameOptionsMenu__vfunc_23(ctx, base);
  ShowValues(ctx, base, page);
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
// Ours step the setting after the entry's own (arrow flash, sound).
REX_EXTERN(__imp__FEComboBox__vfunc_5);
REX_HOOK_RAW(FEComboBox__vfunc_5) {
  const uint32_t entry = ctx.r3.u32;
  uint32_t page = 0;
  const int row = RowOf(base, entry, &page);
  if (row < 0) {
    __imp__FEComboBox__vfunc_5(ctx, base);
    return;
  }
  if (HasValue(base, entry))
    __imp__FEComboBox__vfunc_5(ctx, base);
  else
    ctx.r3.u64 = ~uint64_t(0);  // what it returns
  Stepped(ctx, base, entry, page, row, false);
}

REX_EXTERN(__imp__FEComboBox__vfunc_6);
REX_HOOK_RAW(FEComboBox__vfunc_6) {
  const uint32_t entry = ctx.r3.u32;
  uint32_t page = 0;
  const int row = RowOf(base, entry, &page);
  if (row < 0) {
    __imp__FEComboBox__vfunc_6(ctx, base);
    return;
  }
  if (HasValue(base, entry))
    __imp__FEComboBox__vfunc_6(ctx, base);
  else
    ctx.r3.u64 = ~uint64_t(0);
  Stepped(ctx, base, entry, page, row, true);
}
