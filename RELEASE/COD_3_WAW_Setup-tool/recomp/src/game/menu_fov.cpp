// cod3 - native "FIELD OF VIEW" and "FOV SCALE" options in Options > Gameplay.
//
// The gameplay page (SP_OP_gameplay.panel) is laid out with five rows but the
// game only uses four (Subtitles, Crosshair, Friendly Tags, Sticky Aim) and
// explicitly hides row five. We turn row five into a left/right selector for
// cg_fov and add a sixth row, an exact copy of row five one row lower, for
// cg_fovScale; both look and behave like the stock options:
//
//   * a midasm hook bumps the page's item array from 4 to 6 entries
//     (config/default.toml, Cod3GameplayItemCount),
//   * Init adds item #4 bound to the slot_05 elements (text, value, arrow and
//     row pictures) and un-hides them, then copies those elements one row
//     down the way the game copies elements itself and adds item #5 on them,
//   * the page's draw draws the copies (the item label is drawn and
//     highlighted by the menu like every item),
//   * Toggle (left/right) on them steps cg_fov / cg_fovScale instead of
//     flipping a bool,
//   * Refresh / MoveUp / MoveDown fix the selection title and help text,
//   * Update resets the arrow flash just like the stock rows,
//   * Localize resolves our new string keys.
//
// In the non-English layout (language mode != 0) the game collapses row one
// and re-flows the rows; there the extra items are added but kept disabled
// and hidden so navigation stays correct. Both stay in F2 and the console.
//
// Video mode (game/menu_video.h): Options > VIDEO opens this same page with
// its title and six row labels re-keyed to the video settings; the values,
// help texts and left/right steps then come from menu_video.cpp, and the
// page's own options are left untouched (Back writes them back unchanged).
// GAMEPLAY puts the page's own labels back.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>

#include "game/addresses.h"
#include "game/engine.h"
#include "game/fov.h"
#include "game/guest.h"
#include "game/menu_video.h"
#include "game/movement/menu_movement.h"  // [movement]
#include "game/qte_actions.h"

namespace {

namespace fe = cod3::addr::fe;
using cod3::guest::Call;
using cod3::guest::ReadF32;
using cod3::guest::ReadU16;
using cod3::guest::ReadU32;
using cod3::guest::ReadU8;
using cod3::guest::VirtualSlot;

constexpr uint32_t kGameplayVtable = 0x82071FAC;
constexpr int kFovItem = 4;        // row five: cg_fov
constexpr int kScaleItem = 5;      // row six: cg_fovScale
constexpr int kItemCount = 6;
// Each row's range follows the other row's value, the same as F2 and the
// console (game/fov.h: cg_fov x cg_fovScale stays within 65-165).
constexpr float kFovStep = 5.0f;
constexpr float kScaleStep = 0.05f;

constexpr const char* kLabelKey = "FEMENU_COP_FOV";
constexpr const char* kLabelText = "FIELD OF VIEW";
constexpr const char* kInstKey = "FEMENU_GAMEPLAY_INST_FOV";
constexpr const char* kInstText =
    "Adjust the field of view (cg_fov). 65 is the original console view. With the FOV scale it "
    "stays within 65 to 165.";
constexpr const char* kScaleLabelKey = "FEMENU_COP_FOV_SCALE";
constexpr const char* kScaleLabelText = "FOV SCALE";
constexpr const char* kScaleInstKey = "FEMENU_GAMEPLAY_INST_FOV_SCALE";
constexpr const char* kScaleInstText =
    "Multiply the field of view (cg_fovScale), World at War style: 1.20 turns 65 into 78. "
    "Aiming down the sights is scaled the same way.";

struct FovRow {
  uint32_t text_a = 0;
  uint32_t text_b = 0;
  uint32_t arrow_left = 0;
  uint32_t arrow_right = 0;
  uint32_t bkg_row = 0;
  uint32_t bkg_line = 0;
  bool enabled = false;
};

// The two rows of one gameplay page: [0] = FIELD OF VIEW (the panel's row
// five), [1] = FOV SCALE (our copies of row five's elements, which the page
// owns: drawn by the draw hook, freed with the page).
struct FovRows {
  FovRow row[2];
  bool copies = false;
  float pitch = 0.0f;  // row six's distance below row five
  bool video = false;        // open as Options > Video (game/menu_video.h)
  bool video_shown = false;  // the labels are the video ones now
};

std::mutex g_mutex;
std::unordered_map<uint32_t, FovRows> g_rows;  // keyed by page object

bool IsGameplayPage(uint8_t* base, uint32_t self) {
  return self && ReadU32(base, self) == kGameplayVtable;
}

bool GetRows(uint8_t* base, uint32_t self, FovRows& out) {
  if (!IsGameplayPage(base, self)) return false;
  std::lock_guard lock(g_mutex);
  auto it = g_rows.find(self);
  if (it == g_rows.end()) return false;
  out = it->second;
  return true;
}

// Our row for a page item (kFovItem / kScaleItem), if it is shown.
const FovRow* RowFor(const FovRows& rows, int item) {
  if (item != kFovItem && item != kScaleItem) return nullptr;
  const FovRow& r = rows.row[item - kFovItem];
  return r.enabled ? &r : nullptr;
}

int16_t Selected(uint8_t* base, uint32_t self) {
  return static_cast<int16_t>(ReadU16(base, self + fe::kSelected));
}

// --- guest UI calls ----------------------------------------------------------
uint32_t FindElement(PPCContext& ctx, uint8_t* base, uint32_t panel, const char* name) {
  Call c(ctx, base);
  uint32_t s = c.PushString(name);
  return static_cast<uint32_t>(c.Invoke(fe::kPanelFindElement, {panel, s}));
}

// A picture of the panel by name, or 0 (the lookup's stand-in for a missing
// name is not that name).
uint32_t FindQuad(PPCContext& ctx, uint8_t* base, uint32_t panel, const char* name) {
  Call c(ctx, base);
  uint32_t s = c.PushString(name);
  const uint32_t quad = static_cast<uint32_t>(c.Invoke(fe::kPanelFindQuad, {panel, s}));
  if (quad < 0x10000 || (quad & 3)) return 0;
  const uint32_t str = ReadU32(base, quad + fe::kQuadName);
  if (!str || cod3::guest::ReadString(base, str + 12, 64) != name) return 0;
  return quad;
}

void VCall(PPCContext& ctx, uint8_t* base, uint32_t obj, uint32_t slot, uint64_t a1 = 0,
           uint64_t a2 = 0, uint64_t a3 = 0) {
  if (!obj) return;
  Call c(ctx, base);
  c.Invoke(VirtualSlot(base, obj, slot), {obj, a1, a2, a3});
}

// A copy of a panel element `dy` lower, made the way the game copies elements
// (0x824E1588): operator new, the class's constructor, Assign(source).
uint32_t CopyElement(PPCContext& ctx, uint8_t* base, uint32_t src, bool quad, float dy) {
  if (src < 0x10000) return 0;
  Call c(ctx, base);
  const uint32_t mem =
      static_cast<uint32_t>(c.Invoke(fe::kOperatorNew, {quad ? fe::kQuadSize : fe::kTextSize}));
  if (mem < 0x10000) return 0;
  c.Invoke(quad ? fe::kQuadCtor : fe::kTextCtor, {mem});
  c.Invoke(VirtualSlot(base, mem, fe::kElemAssign), {mem, src});
  const uint32_t y = mem + (quad ? fe::kQuadY : fe::kTextY);
  cod3::guest::WriteF32(base, y, ReadF32(base, y) + dy);
  return mem;
}

void SetVisible(PPCContext& ctx, uint8_t* base, uint32_t elem, bool visible) {
  VCall(ctx, base, elem, fe::kElemSetVisible, visible ? 1 : 0);
}

void SetAlpha(PPCContext& ctx, uint8_t* base, uint32_t elem, float alpha) {
  if (!elem) return;
  Call c(ctx, base);
  c.Invoke(VirtualSlot(base, elem, fe::kElemSetAlpha), {elem}, {alpha});
}

void SetTextRaw(PPCContext& ctx, uint8_t* base, uint32_t elem, const std::string& text) {
  if (!elem) return;
  Call c(ctx, base);
  uint32_t s = c.PushString(text);
  c.Invoke(VirtualSlot(base, elem, fe::kElemSetTextRaw), {elem, s});
}

void SetTextKey(PPCContext& ctx, uint8_t* base, uint32_t elem, const char* key) {
  if (!elem) return;
  Call c(ctx, base);
  uint32_t s = c.PushString(key);
  c.Invoke(VirtualSlot(base, elem, fe::kElemSetTextKey), {elem, s});
}

void SetInstructions(PPCContext& ctx, uint8_t* base, uint32_t box, const char* key) {
  if (!box) return;
  Call c(ctx, base);
  uint64_t width = c.Invoke(VirtualSlot(base, box, fe::kWrapGetWidth), {box});
  uint32_t s = c.PushString(key);
  float scale = ReadF32(base, fe::kTextScaleDefault);
  c.Invoke(VirtualSlot(base, box, fe::kWrapSetTextKey), {box, s, width}, {scale});
}

float CurrentFov() {
  float fov = cod3::Engine::Get().fov();
  return std::isfinite(fov) ? fov : 65.0f;
}

float CurrentScale() {
  float scale = cod3::Engine::Get().fov_scale();
  return std::isfinite(scale) && scale > 0.0f ? scale : 1.0f;
}

std::string FovText(float fov) {
  char buf[16];
  std::snprintf(buf, sizeof(buf), "%d", static_cast<int>(std::lround(fov)));
  return buf;
}

std::string ScaleText(float scale) {
  char buf[16];
  std::snprintf(buf, sizeof(buf), "%.2f", scale);
  return buf;
}

// The value shown on our row for item kFovItem / kScaleItem.
std::string RowValueText(int item) {
  return item == kScaleItem ? ScaleText(CurrentScale()) : FovText(CurrentFov());
}

// Title and help text while one of our rows is selected.
void ShowFovSelection(PPCContext& ctx, uint8_t* base, uint32_t self, int item) {
  const bool scale = item == kScaleItem;
  SetTextKey(ctx, base, ReadU32(base, self + fe::kCurrentSelectedText),
             scale ? kScaleLabelKey : kLabelKey);
  SetInstructions(ctx, base, ReadU32(base, self + fe::kInstructionsText),
                  scale ? kScaleInstKey : kInstKey);
}

// Guest copies of our localized strings, allocated once.
uint32_t g_label_str = 0;
uint32_t g_inst_str = 0;
uint32_t g_scale_label_str = 0;
uint32_t g_scale_inst_str = 0;

// The next multiple of `step` past `value` (a value set in F2 or the console
// can lie between two steps: 83 goes to 85 or 80), kept within `range`.
float NextStep(float value, float step, bool right, cod3::fov::Range range) {
  const float steps = value / step;
  const float next = right ? std::floor(steps + 1e-3f) + 1.0f : std::ceil(steps - 1e-3f) - 1.0f;
  return std::clamp(next * step, range.min, range.max);
}

// One step left or right on the FOV (`scale` false) or FOV scale row;
// returns the new value's text. Each row's range follows the other row.
std::string StepValue(PPCContext& ctx, uint8_t* base, bool scale, bool right) {
  if (scale) {
    const float v = NextStep(CurrentScale(), kScaleStep, right, cod3::fov::ScaleRange(CurrentFov()));
    cod3::Engine::Get().SetFovScale(v);  // applied on the next frame, saved like F2
    return ScaleText(CurrentScale());
  }
  const float v = NextStep(CurrentFov(), kFovStep, right, cod3::fov::FovRange(CurrentScale()));
  cod3::Engine::Get().ApplyFovFromGuest(ctx, base, v);
  return FovText(CurrentFov());
}

void SetItemCount(uint8_t* base, uint32_t self, int count) {  // big-endian s16
  base[self + fe::kItemCount] = uint8_t(count >> 8);
  base[self + fe::kItemCount + 1] = uint8_t(count);
}

uint32_t GuestString(uint8_t* base, const char* text) {
  auto* mem = rex::system::kernel_state()->memory();
  uint32_t len = static_cast<uint32_t>(std::strlen(text)) + 1;
  uint32_t addr = mem->SystemHeapAlloc(len);
  if (addr) std::memcpy(base + addr, text, len);
  return addr;
}

// --- video mode (game/menu_video.h) -----------------------------------------
namespace vm = cod3::videomenu;

// The values each video row showed last time it was read from its setting,
// per page: a change made in F2 or the console shows up on the next frame.
std::unordered_map<uint32_t, std::array<std::string, vm::kRowCount>> g_video_seen;  // g_mutex

// Items in video mode: the page's four rows, then FIELD OF VIEW and FOV SCALE
// (when they were added), one setting each.
int VideoItemCount(const FovRows& rows) {
  int n = 4;
  if (rows.row[0].enabled) ++n;
  if (rows.row[0].enabled && rows.row[1].enabled) ++n;
  return std::min(n, int(vm::kRowCount));
}

uint32_t VideoValueElem(uint8_t* base, uint32_t self, const FovRows& rows, int item) {
  if (item >= 0 && item < 4) return ReadU32(base, self + fe::kGameplayValues + 4 * item);
  if (const FovRow* r = RowFor(rows, item)) return r->text_b;
  return 0;
}

void VideoArrows(uint8_t* base, uint32_t self, const FovRows& rows, int item, uint32_t* left,
                 uint32_t* right) {
  *left = *right = 0;
  if (item >= 0 && item < 4) {
    *left = ReadU32(base, self + fe::kGameplayArrows + 8 * item);
    *right = ReadU32(base, self + fe::kGameplayArrows + 8 * item + 4);
  } else if (const FovRow* r = RowFor(rows, item)) {
    *left = r->arrow_left;
    *right = r->arrow_right;
  }
}

void SetItemLabelKey(PPCContext& ctx, uint8_t* base, uint32_t self, int index, const std::string& key) {
  const uint32_t list = ReadU32(base, self + fe::kItems);
  if (list < 0x10000) return;
  const uint32_t item = ReadU32(base, list + 4 * index);
  if (item < 0x10000 || (item & 3)) return;
  Call c(ctx, base);
  const uint32_t s = c.PushString(key);
  c.Invoke(VirtualSlot(base, item, fe::kItemSetLabelKey), {item, s});
}

// Title and row labels: the video settings', or the page's own.
void ApplyGameplayMode(PPCContext& ctx, uint8_t* base, uint32_t self, const FovRows& rows, bool video) {
  SetTextKey(ctx, base, ReadU32(base, self + fe::kGameplayTitleMain),
             video ? vm::TitleKey()
                   : cod3::guest::ReadString(base, fe::kGameplayTitleKey, 64).c_str());
  const int count = VideoItemCount(rows);
  for (int i = 0; i < count; ++i) {
    std::string key;
    if (video)
      key = vm::LabelKey(i);
    else if (i < 4)
      key = cod3::guest::ReadString(base, ReadU32(base, fe::kGameplayLabelKeys + 4 * i), 64);
    else
      key = i == kFovItem ? kLabelKey : kScaleLabelKey;
    if (!key.empty()) SetItemLabelKey(ctx, base, self, i, key);
  }
}

// The big title and the help text for video row `item`.
void ShowVideoSelection(PPCContext& ctx, uint8_t* base, uint32_t self, int item) {
  if (item < 0 || item >= vm::kRowCount) return;
  SetTextKey(ctx, base, ReadU32(base, self + fe::kCurrentSelectedText), vm::LabelKey(item));
  SetInstructions(ctx, base, ReadU32(base, self + fe::kInstructionsText), vm::InstructionsKey(item));
}

// Every row's value from its setting, the arrows at rest.
void ShowVideoValues(PPCContext& ctx, uint8_t* base, uint32_t self, const FovRows& rows) {
  std::array<std::string, vm::kRowCount> seen;
  const float normal = ReadF32(base, fe::kArrowAlphaNormal);
  for (int i = 0; i < VideoItemCount(rows); ++i) {
    seen[i] = vm::Value(i);
    SetTextRaw(ctx, base, VideoValueElem(base, self, rows, i), seen[i]);
    uint32_t left = 0, right = 0;
    VideoArrows(base, self, rows, i, &left, &right);
    SetAlpha(ctx, base, left, normal);
    SetAlpha(ctx, base, right, normal);
  }
  std::lock_guard lock(g_mutex);
  g_video_seen[self] = seen;
}

}  // namespace

namespace cod3::videomenu {
void SetGameplayPageMode(uint32_t page, bool video) {
  std::lock_guard lock(g_mutex);
  if (auto it = g_rows.find(page); it != g_rows.end()) it->second.video = video;
}
}  // namespace cod3::videomenu

// ---------------------------------------------------------------------------
// midasm hook @ 0x824DBC0C (`li r5,4` in the gameplay page constructor):
// size the item array for six entries instead of four.
// ---------------------------------------------------------------------------
void Cod3GameplayItemCount(PPCRegister& r5) {
  r5.u64 = kItemCount;
}

namespace {
// Frees our copied elements (they belong to the page, not to the panel).
void FreeCopies(PPCContext& ctx, uint8_t* base, const FovRows& rows) {
  if (!rows.copies) return;
  const FovRow& r = rows.row[1];
  for (uint32_t e : {r.text_a, r.text_b, r.arrow_left, r.arrow_right, r.bkg_row, r.bkg_line})
    if (e) VCall(ctx, base, e, fe::kElemDelete, 1);
}

// Adds item `index` on a row's label and shows the row (or, in the other
// language layout, keeps the item disabled and hidden). False if the page
// refused the item.
bool AddRow(PPCContext& ctx, uint8_t* base, uint32_t self, int index, FovRow& row,
            const char* label_key, bool english_layout) {
  // this->AddItem(index, label, 0); items[index]->SetLabelKey(...)
  VCall(ctx, base, self, fe::kPageAddItem, index, row.text_a, 0);
  const uint32_t item = ReadU32(base, ReadU32(base, self + fe::kItems) + index * 4);
  if (item < 0x10000 || (item & 3)) return false;
  {
    Call c(ctx, base);
    uint32_t s = c.PushString(label_key);
    c.Invoke(VirtualSlot(base, item, fe::kItemSetLabelKey), {item, s});
  }
  if (english_layout) {
    for (uint32_t e : {row.text_a, row.text_b, row.arrow_left, row.arrow_right, row.bkg_row, row.bkg_line})
      SetVisible(ctx, base, e, true);
    SetTextRaw(ctx, base, row.text_b, RowValueText(index));
    float normal = ReadF32(base, fe::kArrowAlphaNormal);
    SetAlpha(ctx, base, row.arrow_left, normal);
    SetAlpha(ctx, base, row.arrow_right, normal);
    row.enabled = true;
  } else {
    VCall(ctx, base, item, fe::kItemSetDisabled, 1);
    VCall(ctx, base, item, fe::kItemSetVisible, 0);
  }
  return true;
}
}  // namespace

// Gameplay page destructor body: forget the page, and free our copies once
// its items (which point at one of them) are gone.
REX_EXTERN(__imp__OptionsGameplayMenu__dtor);
REX_HOOK_RAW(OptionsGameplayMenu__dtor) {
  const uint32_t self = ctx.r3.u32;
  FovRows rows;
  bool known = false;
  {
    std::lock_guard lock(g_mutex);
    if (auto it = g_rows.find(self); it != g_rows.end()) {
      rows = it->second;
      known = true;
      g_rows.erase(it);
    }
    g_video_seen.erase(self);
  }
  __imp__OptionsGameplayMenu__dtor(ctx, base);
  if (known) FreeCopies(ctx, base, rows);
}

// vtbl[0] Init(this, panel)
REX_EXTERN(__imp__OptionsGameplayMenu__Init);
REX_HOOK_RAW(OptionsGameplayMenu__Init) {
  const uint32_t self = ctx.r3.u32;
  __imp__OptionsGameplayMenu__Init(ctx, base);

  if (!IsGameplayPage(base, self)) return;
  // Init stores the panel at this+56; that is what its own lookups use.
  const uint32_t panel = ReadU32(base, self + fe::kPanel);
  if (panel < 0x10000) return;
  // Only safe when the constructor hook grew the item array.
  const int room = static_cast<int16_t>(ReadU16(base, self + fe::kItemCount));
  if (room <= kFovItem) {
    REXLOG_WARN("cod3: gameplay page has no room for the FOV rows (codegen hook missing?)");
    return;
  }

  // Never hand the game a bad pointer: if the panel doesn't have the row we
  // expect (other language layout, different build), leave the page alone.
  auto valid = [](uint32_t p) { return p >= 0x10000 && (p & 3) == 0; };
  const bool english_layout = ReadU32(base, fe::kLanguageMode) == 0;
  FovRows rows;
  int used = kFovItem;  // items the page has now

  // Row five: the panel's own elements (text in the element list, the
  // arrows and the row's background pictures in the quad list).
  FovRow& fov = rows.row[0];
  fov.text_a = FindElement(ctx, base, panel, "slot_05_text_a");
  fov.text_b = FindElement(ctx, base, panel, "slot_05_text_b");
  fov.arrow_left = FindQuad(ctx, base, panel, "slot_05_arrow_left");
  fov.arrow_right = FindQuad(ctx, base, panel, "slot_05_arrow_right");
  fov.bkg_row = FindQuad(ctx, base, panel, "bkg_row_05");
  fov.bkg_line = FindQuad(ctx, base, panel, "bkg_line_04");
  if (!valid(fov.text_a) || !valid(fov.text_b)) {
    REXLOG_WARN("cod3: gameplay panel has no slot_05 row; FOV options not added");
  } else if (AddRow(ctx, base, self, kFovItem, fov, kLabelKey, english_layout)) {
    used = kFovItem + 1;
    // Row six: copies of row five's elements, one row lower.
    const uint32_t row4 = FindElement(ctx, base, panel, "slot_04_text_a");
    const float pitch = valid(row4) ? ReadF32(base, fov.text_a + fe::kTextY) -
                                          ReadF32(base, row4 + fe::kTextY)
                                    : 0.0f;
    if (room > kScaleItem && pitch > 5.0f && pitch < 60.0f) {
      FovRow& sc = rows.row[1];
      sc.text_a = CopyElement(ctx, base, fov.text_a, false, pitch);
      sc.text_b = CopyElement(ctx, base, fov.text_b, false, pitch);
      sc.arrow_left = CopyElement(ctx, base, fov.arrow_left, true, pitch);
      sc.arrow_right = CopyElement(ctx, base, fov.arrow_right, true, pitch);
      sc.bkg_row = CopyElement(ctx, base, fov.bkg_row, true, pitch);
      sc.bkg_line = CopyElement(ctx, base, fov.bkg_line, true, pitch);
      rows.copies = true;
      rows.pitch = pitch;
      if (valid(sc.text_a) && valid(sc.text_b) &&
          AddRow(ctx, base, self, kScaleItem, sc, kScaleLabelKey, english_layout))
        used = kScaleItem + 1;
    }
  }
  // The page walks every slot of its item array: never leave one empty.
  if (used < room) SetItemCount(base, self, used);

  std::lock_guard lock(g_mutex);
  g_rows[self] = rows;
}

namespace {
// The panel animates its elements (the page slides and fades in and out).
// Our moved and copied elements follow a live element of the page, `dy`
// lower: a text takes its transform (offset, position, scales) and opacity,
// a picture the vertex positions of what it draws.
void FollowText(uint8_t* base, uint32_t src, uint32_t dst, float dy) {
  using cod3::guest::WriteF32;
  if (!src || !dst) return;
  for (uint32_t at = fe::kTextTransform; at < fe::kTextTransformEnd; at += 4)
    WriteF32(base, dst + at, ReadF32(base, src + at));
  WriteF32(base, dst + fe::kTextY, ReadF32(base, src + fe::kTextY) + dy);
  WriteF32(base, dst + fe::kElemOpacity, ReadF32(base, src + fe::kElemOpacity));
}

void FollowQuad(uint8_t* base, uint32_t src, uint32_t dst, float dy) {
  using cod3::guest::WriteF32;
  if (!src || !dst) return;
  const uint32_t a = ReadU32(base, src + fe::kQuadParts);
  const uint32_t b = ReadU32(base, dst + fe::kQuadParts);
  const uint32_t n = ReadU32(base, src + fe::kQuadPartCount);
  if (a < 0x10000 || b < 0x10000 || n != ReadU32(base, dst + fe::kQuadPartCount) || n > 8) return;
  for (uint32_t part = 0; part < n; ++part) {
    const uint32_t qa = ReadU32(base, a + 4 * part);
    const uint32_t qb = ReadU32(base, b + 4 * part);
    if (qa < 0x10000 || qb < 0x10000) continue;
    for (uint32_t v = 0; v < 4; ++v) {
      const uint32_t at = fe::kQuadPartVerts + v * fe::kQuadVertStride;
      WriteF32(base, qb + at, ReadF32(base, qa + at));
      WriteF32(base, qb + at + 4, ReadF32(base, qa + at + 4) + dy);
    }
  }
  WriteF32(base, dst + fe::kElemOpacity, ReadF32(base, src + fe::kElemOpacity));
}

// Row six follows row five, one row lower.
void FollowRowFive(uint8_t* base, const FovRows& rows) {
  const FovRow& from = rows.row[0];
  const FovRow& to = rows.row[1];
  FollowText(base, from.text_a, to.text_a, rows.pitch);
  FollowText(base, from.text_b, to.text_b, rows.pitch);
  FollowQuad(base, from.arrow_left, to.arrow_left, rows.pitch);
  FollowQuad(base, from.arrow_right, to.arrow_right, rows.pitch);
  FollowQuad(base, from.bkg_row, to.bkg_row, rows.pitch);
  FollowQuad(base, from.bkg_line, to.bkg_line, rows.pitch);
}
}  // namespace

// vtbl[14] draw: the page's panel and its items (the copied label is item
// #5's and drawn, highlighted or not, with the items), then the rest of our
// row-six copies.
REX_EXTERN(__imp__OptionsGameplayMenu__vfunc_14);
REX_HOOK_RAW(OptionsGameplayMenu__vfunc_14) {
  const uint32_t self = ctx.r3.u32;
  FovRows rows;
  const bool ours = GetRows(base, self, rows) && rows.copies && rows.row[1].enabled;
  if (ours) FollowRowFive(base, rows);
  __imp__OptionsGameplayMenu__vfunc_14(ctx, base);
  if (!ours) return;
  const FovRow& r = rows.row[1];
  for (uint32_t e : {r.bkg_row, r.bkg_line, r.arrow_left, r.arrow_right, r.text_b})
    if (e) VCall(ctx, base, e, fe::kElemDraw);
}

// vtbl[21] Refresh(this): redraws title, help text and every value.
REX_EXTERN(__imp__OptionsGameplayMenu__Refresh);
REX_HOOK_RAW(OptionsGameplayMenu__Refresh) {
  const uint32_t self = ctx.r3.u32;
  __imp__OptionsGameplayMenu__Refresh(ctx, base);
  FovRows rows;
  if (!GetRows(base, self, rows)) return;
  // Options > Video or Options > Gameplay (the page opens with Refresh).
  if (rows.video != rows.video_shown) {
    ApplyGameplayMode(ctx, base, self, rows, rows.video);
    std::lock_guard lock(g_mutex);
    if (auto it = g_rows.find(self); it != g_rows.end()) it->second.video_shown = rows.video;
  }
  if (rows.video) {
    ShowVideoValues(ctx, base, self, rows);
    ShowVideoSelection(ctx, base, self, Selected(base, self));
    return;
  }
  const float normal = ReadF32(base, fe::kArrowAlphaNormal);
  for (int item : {kFovItem, kScaleItem}) {
    const FovRow* row = RowFor(rows, item);
    if (!row) continue;
    SetTextRaw(ctx, base, row->text_b, RowValueText(item));
    SetAlpha(ctx, base, row->arrow_left, normal);
    SetAlpha(ctx, base, row->arrow_right, normal);
    if (Selected(base, self) == item) ShowFovSelection(ctx, base, self, item);
  }
}

// vtbl[25] / vtbl[26]: move selection up / down, then update title + help.
REX_EXTERN(__imp__OptionsGameplayMenu__MoveUp);
REX_HOOK_RAW(OptionsGameplayMenu__MoveUp) {
  const uint32_t self = ctx.r3.u32;
  __imp__OptionsGameplayMenu__MoveUp(ctx, base);
  FovRows rows;
  const int sel = Selected(base, self);
  if (!GetRows(base, self, rows)) return;
  if (rows.video)
    ShowVideoSelection(ctx, base, self, sel);
  else if (RowFor(rows, sel))
    ShowFovSelection(ctx, base, self, sel);
}

REX_EXTERN(__imp__OptionsGameplayMenu__MoveDown);
REX_HOOK_RAW(OptionsGameplayMenu__MoveDown) {
  const uint32_t self = ctx.r3.u32;
  __imp__OptionsGameplayMenu__MoveDown(ctx, base);
  FovRows rows;
  const int sel = Selected(base, self);
  if (!GetRows(base, self, rows)) return;
  if (rows.video)
    ShowVideoSelection(ctx, base, self, sel);
  else if (RowFor(rows, sel))
    ShowFovSelection(ctx, base, self, sel);
}

// Toggle(this, dir): dir 0 = left, 1 = right. Stock rows flip a byte at
// this+148+sel; for rows five and six that byte would be the arrow-flash
// timer, so our rows never reach the original.
REX_EXTERN(__imp__OptionsGameplayMenu__Toggle);
REX_HOOK_RAW(OptionsGameplayMenu__Toggle) {
  const uint32_t self = ctx.r3.u32;
  const bool right = (ctx.r4.u32 & 0xFF) == 1;
  const int sel = Selected(base, self);
  FovRows rows;
  const bool known = GetRows(base, self, rows);
  if (known && rows.video) {
    // Options > Video: every row steps its setting; the page's own options
    // stay as they are.
    if (sel < 0 || sel >= VideoItemCount(rows)) return;
    const std::string text = vm::Step(ctx, base, sel, right);
    if (!text.empty()) {
      SetTextRaw(ctx, base, VideoValueElem(base, self, rows, sel), text);
      cod3::guest::WriteF32(base, self + fe::kArrowFlashTimer, ReadF32(base, fe::kArrowFlashTime));
      uint32_t left = 0, rgt = 0;
      VideoArrows(base, self, rows, sel, &left, &rgt);
      const float normal = ReadF32(base, fe::kArrowAlphaNormal);
      const float flash = ReadF32(base, fe::kArrowAlphaFlash);
      SetAlpha(ctx, base, left, right ? normal : flash);
      SetAlpha(ctx, base, rgt, right ? flash : normal);
    }
    ShowVideoSelection(ctx, base, self, sel);  // the help text may say more now
    return;
  }
  const FovRow* row = known ? RowFor(rows, sel) : nullptr;
  if (!row) {
    __imp__OptionsGameplayMenu__Toggle(ctx, base);
    return;
  }

  SetTextRaw(ctx, base, row->text_b, StepValue(ctx, base, sel == kScaleItem, right));
  cod3::guest::WriteF32(base, self + fe::kArrowFlashTimer, ReadF32(base, fe::kArrowFlashTime));
  float normal = ReadF32(base, fe::kArrowAlphaNormal);
  float flash = ReadF32(base, fe::kArrowAlphaFlash);
  SetAlpha(ctx, base, row->arrow_left, right ? normal : flash);
  SetAlpha(ctx, base, row->arrow_right, right ? flash : normal);
}

// vtbl[16] Update(this, f1 = dt): the stock code resets the four arrow pairs
// when the flash timer runs out; do the same for rows five and six.
REX_EXTERN(__imp__OptionsGameplayMenu__Update);
REX_HOOK_RAW(OptionsGameplayMenu__Update) {
  const uint32_t self = ctx.r3.u32;
  __imp__OptionsGameplayMenu__Update(ctx, base);
  FovRows rows;
  if (!GetRows(base, self, rows)) return;
  if (rows.video && rows.video_shown) {
    // A setting changed in F2 or the console shows up here at once.
    for (int i = 0; i < VideoItemCount(rows); ++i) {
      const std::string now = vm::Value(i);
      {
        std::lock_guard lock(g_mutex);
        std::string& seen = g_video_seen[self][i];
        if (seen == now) continue;
        seen = now;
      }
      SetTextRaw(ctx, base, VideoValueElem(base, self, rows, i), now);
    }
  }
  if (ReadF32(base, self + fe::kArrowFlashTimer) > 0.0f) return;
  float normal = ReadF32(base, fe::kArrowAlphaNormal);
  for (int item : {kFovItem, kScaleItem}) {
    if (const FovRow* row = RowFor(rows, item)) {
      SetAlpha(ctx, base, row->arrow_left, normal);
      SetAlpha(ctx, base, row->arrow_right, normal);
    }
  }
}

// ===========================================================================
// Pause menu > Options (AdvancedOptionsMenu): the same two rows in game.
// The panel keeps an unused "ADS toggle" row (label, value, arrows L7 / R7)
// parked off screen. It becomes FIELD OF VIEW, one row under Tank Controls;
// a copy of it one row lower is FOV SCALE; "Reset to Defaults" moves down a
// row. The page's item array grows from 11 (items 11 and 12; FEMenu
// allocates it with operator new, so a bigger one takes its place), and the
// highlight visits the new rows between Tank Controls and Reset.
//
// Video options (game/menu_video.h): under FOV SCALE a VIDEO OPTIONS row
// (item 13, a copy of Reset's label in Reset's place; Reset moves down one
// more row). A switches the page to the video rows: six more copies of the
// ADS row (items 14-19) in the places of the first six options, the title
// "OPTIONS / VIDEO" and a line under them saying when a change applies
// (there is no help text on this page). The options' items are disabled and
// hidden meanwhile and their values, arrows and gauges skipped while the
// page draws; B (or opening the page again from the pause menu) brings them
// back as they were.
//
// [menus-brightness] Under the video rows, BRIGHTNESS (item 21): the game's
// own brightness, the value the main menu's Options > SCREEN changes, with a
// gauge like Volume's (left / right a D-pad step, the mouse sets it where
// clicked or dragged). It is saved with the page's options: B's "save
// changes?" covers it.
//
// [movement] Movement options (game/movement/menu_movement.h): under VIDEO
// OPTIONS a MOVEMENT OPTIONS row (item 22, another copy of Reset's label;
// VIDEO OPTIONS and MOVEMENT OPTIONS share the room between FOV SCALE and
// Reset evenly, so Reset stays where it is with VIDEO OPTIONS alone, clear of
// the help bar's line). A shows the movement rows on the same
// copied rows as the video ones (items 14-20, re-labeled), the same way;
// BRIGHTNESS belongs to the video rows only. Both are "sub pages": the rows,
// values, notes and steps come from one SubPage each.
// ===========================================================================
namespace {
namespace adv = fe::adv;
namespace mm = cod3::movementmenu;  // [movement]
constexpr int kAdvFovItem = adv::kItemCount;           // 11
constexpr int kAdvScaleItem = adv::kItemCount + 1;     // 12
constexpr int kAdvVideoItem = adv::kItemCount + 2;     // 13: VIDEO OPTIONS
constexpr int kAdvVideoFirst = adv::kItemCount + 3;    // 14..20: the rows of the sub page shown
// [movement] The video and the movement rows share these copied rows.
constexpr int kAdvSubRows = int(vm::kRowCount) > mm::kRows ? int(vm::kRowCount) : mm::kRows;
constexpr int kAdvItemsMax = kAdvVideoFirst + kAdvSubRows;
// [menus-brightness] BRIGHTNESS (the game's brightness, menu_video.h): the
// last row of OPTIONS / VIDEO, under the video rows, with a gauge like
// Volume's. (The options list itself has no room: a row more pushes Reset
// to Defaults onto the help bar's line.)
constexpr int kAdvBrightItem = kAdvItemsMax;
// [movement] MOVEMENT OPTIONS (22): shown under VIDEO OPTIONS; its item comes
// after BRIGHTNESS's (the page's item array has no holes, so it is only added
// when BRIGHTNESS was).
constexpr int kAdvMovementItem = kAdvBrightItem + 1;
constexpr int kAdvItemsAll = kAdvMovementItem + 1;
namespace br = cod3::addr::brightness;

bool IsSubRow(int item) { return item >= kAdvVideoFirst && item < kAdvVideoFirst + kAdvSubRows; }

// [movement] The rows of one sub page of the in-game options.
struct SubPage {
  int rows;
  const char* (*label_key)(int);
  std::string (*value)(int);
  std::string (*note)(int);
  std::string (*step)(PPCContext&, uint8_t*, int, bool);
  const char* (*title_key)();
};
const SubPage kVideoPage = {int(vm::kRowCount), vm::LabelKey, vm::Value, vm::Note, vm::Step, vm::TitleKey};
const SubPage kMovementPage = {mm::kRows, mm::LabelKey, mm::Value, mm::Note, mm::Step, mm::TitleKey};
constexpr int kPageVideo = 0;
constexpr int kPageMovement = 1;

struct AdvRows {
  FovRow row[2];        // [0] the ADS row as FIELD OF VIEW, [1] our copy as FOV SCALE
  uint32_t tank[4] = {};  // Tank Controls' label, value, left and right arrow
  uint32_t reset = 0;     // Reset to Defaults' label
  float pitch = 0.0f;     // distance between two rows
  float reset_gap = 0.0f; // Reset's distance below Tank Controls
  // Video and movement options
  bool has_video = false;
  bool video = false;                // a sub page's rows are shown
  int page = kPageVideo;             // [movement] which one
  uint32_t video_label = 0;          // VIDEO OPTIONS (a copy of Reset's label)
  bool has_movement = false;         // [movement]
  uint32_t movement_label = 0;       // [movement] MOVEMENT OPTIONS (a copy of Reset's label)
  FovRow vrow[kAdvSubRows];          // the sub page's rows (copies of the ADS row)
  uint32_t note = 0;                 // the line under them (a copy of Reset's label)
  uint32_t invert[4] = {};           // Invert Aim's label, value and arrows: the first row's place
  uint32_t title = 0;                // the page title
  std::vector<uint32_t> hide;        // the options' values, arrows and gauges
  uint8_t saved_disabled[kAdvItemsAll] = {};  // the other items before a sub page showed
  bool saved_visible[kAdvItemsAll] = {};
  std::array<std::string, kAdvSubRows> seen;  // values as last read from the settings
  std::string note_text;
  bool reopened = false;  // the pause menu's Options item opened the page again
  // [menus-brightness] BRIGHTNESS: a copy of the ADS row's label and of
  // Volume's gauge (its frame and its fill), all owned by the page.
  bool has_bright = false;
  uint32_t bright_label = 0, bright_back = 0, bright_fill = 0;
  uint32_t volume_label = 0, volume_back = 0, volume_fill = 0;  // Volume's own
  float bright_page = 1.0f;  // the page's brightness while it is swapped (ControlsPage_Apply)
};
std::unordered_map<uint32_t, AdvRows> g_adv;  // keyed by page; guarded by g_mutex

bool GetAdv(uint8_t* base, uint32_t self, AdvRows& out) {
  if (!self || ReadU32(base, self) != adv::kVtable) return false;
  std::lock_guard lock(g_mutex);
  auto it = g_adv.find(self);
  if (it == g_adv.end()) return false;
  out = it->second;
  return true;
}

void PutAdv(uint32_t self, const AdvRows& r) {
  std::lock_guard lock(g_mutex);
  if (auto it = g_adv.find(self); it != g_adv.end()) it->second = r;
}

// [movement] The sub page shown (or last shown).
const SubPage& PageOf(const AdvRows& r) { return r.page == kPageMovement ? kMovementPage : kVideoPage; }
// [menus-brightness] BRIGHTNESS is a row of the video page only.
bool BrightShown(const AdvRows& r) { return r.has_bright && r.video && r.page == kPageVideo; }
// The last row of the sub page shown: BRIGHTNESS on the video page, else
// the page's last copied row.
int SubPageLast(const AdvRows& r) {
  return BrightShown(r) ? kAdvBrightItem : kAdvVideoFirst + PageOf(r).rows - 1;
}

// [menus-brightness] BRIGHTNESS one row under the video rows (in Subtitles'
// place), with a copy of Volume's gauge where the values are.
void FollowBright(uint8_t* base, const AdvRows& r) {
  if (!r.has_bright || !r.invert[0] || !r.volume_label) return;
  const float dy = r.pitch * float(vm::kRowCount);
  FollowText(base, r.invert[0], r.bright_label, dy);
  const float to_volume = ReadF32(base, r.invert[0] + fe::kTextY) + dy - ReadF32(base, r.volume_label + fe::kTextY);
  FollowQuad(base, r.volume_back, r.bright_back, to_volume);
  FollowQuad(base, r.volume_fill, r.bright_fill, to_volume);
}

// [menus-brightness] The first picture part of a quad element (0 if none).
uint32_t FirstPart(uint8_t* base, uint32_t quad) {
  if (!quad) return 0;
  const uint32_t parts = ReadU32(base, quad + fe::kQuadParts);
  if (parts < 0x10000 || ReadU32(base, quad + fe::kQuadPartCount) == 0) return 0;
  const uint32_t part = ReadU32(base, parts);
  return part >= 0x10000 && !(part & 3) ? part : 0;
}

// The leftmost and rightmost corner of a picture part.
void PartSpanX(uint8_t* base, uint32_t part, float& left, float& right) {
  left = right = ReadF32(base, part + fe::kQuadPartVerts);
  for (uint32_t v = 1; v < 4; ++v) {
    const float x = ReadF32(base, part + fe::kQuadPartVerts + v * fe::kQuadVertStride);
    left = std::min(left, x);
    right = std::max(right, x);
  }
}

// [menus-brightness] The BRIGHTNESS gauge's fill shows the brightness, set
// the way the page fills Volume's gauge (0x824C0588, after FollowBright, which
// copies Volume's frame and fill). That call makes the fill (picture width
// in texels) x scale x fraction wide; the copy's picture is wider than
// Volume's (measured: full at about 78 %, the fill running past the frame), so
// the scale is taken from the frame instead: a full gauge ends inside the
// frame's right edge by half the fill's inset on the left, as Volume's does.
void ShowBrightFill(PPCContext& ctx, uint8_t* base, const AdvRows& r) {
  if (!r.has_bright || !r.volume_fill) return;
  const uint32_t part = FirstPart(base, r.bright_fill);
  const uint32_t frame = FirstPart(base, r.bright_back);
  if (!part) return;
  float scale = ReadF32(base, r.volume_fill + br::kQuadFillScale);
  const int texels = int(int16_t(ReadU16(base, part + 6))) - int(int16_t(ReadU16(base, part + 0)));
  float fill_left = 0, fill_right = 0, frame_left = 0, frame_right = 0;
  PartSpanX(base, part, fill_left, fill_right);
  if (frame) PartSpanX(base, frame, frame_left, frame_right);
  const float inset = std::max(0.0f, fill_left - frame_left) * 0.5f;
  const float full = frame_right - inset - fill_left;
  if (frame && texels > 0 && std::isfinite(full) && full > 0.0f) scale = full / float(texels);
  static bool logged = false;
  if (!logged) {
    logged = true;
    REXLOG_INFO("cod3: BRIGHTNESS gauge: picture {} texels, frame {:.1f}-{:.1f}, fill from {:.1f}, "
                "scale {:.3f} (Volume's {:.3f})",
                texels, frame_left, frame_right, fill_left, scale,
                ReadF32(base, r.volume_fill + br::kQuadFillScale));
  }
  if (!std::isfinite(scale) || scale <= 0.0f) return;
  Call c(ctx, base);
  c.Invoke(br::kSetFill, {part}, {double(vm::BrightnessFraction(base)), 1.0, double(scale)});
}

// Our rows sit under Tank Controls and move with it (the panel animates);
// the video rows take the places of the first six options.
void FollowTank(uint8_t* base, const AdvRows& r) {
  for (int i = 0; i < 2; ++i) {
    const float dy = r.pitch * float(i + 1);
    FollowText(base, r.tank[0], r.row[i].text_a, dy);
    FollowText(base, r.tank[1], r.row[i].text_b, dy);
    FollowQuad(base, r.tank[2], r.row[i].arrow_left, dy);
    FollowQuad(base, r.tank[3], r.row[i].arrow_right, dy);
  }
  if (!r.reset || !r.tank[0]) return;
  const float below = ReadF32(base, r.tank[0] + fe::kTextY) + r.reset_gap + r.pitch;
  float reset_y = below + (r.has_video ? r.pitch : 0.0f);
  // VIDEO OPTIONS one row above Reset; [movement] with MOVEMENT OPTIONS too,
  // the two label rows share the room between FOV SCALE and Reset evenly
  // (5/6 of a row apart with the panel's own gap), so Reset stays where it
  // is with VIDEO OPTIONS alone: a row lower would put it on the help bar's
  // line (menus-brightness run lvl1).
  float label_step = r.pitch;
  if (r.has_movement) {
    const float scale_y = ReadF32(base, r.tank[0] + fe::kTextY) + 2.0f * r.pitch;  // FOV SCALE
    label_step = std::max((reset_y - scale_y) / 3.0f, 0.75f * r.pitch);
    reset_y = scale_y + 3.0f * label_step;
  }
  cod3::guest::WriteF32(base, r.reset + fe::kTextY, reset_y);
  if (!r.has_video) return;
  FollowText(base, r.reset, r.video_label, -label_step * (r.has_movement ? 2.0f : 1.0f));
  if (r.has_movement) FollowText(base, r.reset, r.movement_label, -label_step);  // [movement]
  for (int i = 0; i < kAdvSubRows; ++i) {
    const float dy = r.pitch * float(i);
    FollowText(base, r.invert[0], r.vrow[i].text_a, dy);
    FollowText(base, r.invert[1], r.vrow[i].text_b, dy);
    FollowQuad(base, r.invert[2], r.vrow[i].arrow_left, dy);
    FollowQuad(base, r.invert[3], r.vrow[i].arrow_right, dy);
  }
  FollowBright(base, r);  // [menus-brightness]
  // The note: one blank row under the sub page's rows (and BRIGHTNESS on the
  // video page), centered like Reset.
  const int rows = PageOf(r).rows + (r.has_bright && r.page == kPageVideo ? 1 : 0);
  const float note_y = ReadF32(base, r.invert[0] + fe::kTextY) + r.pitch * float(rows + 1);
  FollowText(base, r.reset, r.note, note_y - ReadF32(base, r.reset + fe::kTextY));
}

// Highlights `index` the way the page's own moves do (FEMenu::SetSelection,
// the selection sound, the value highlight).
void AdvSelect(PPCContext& ctx, uint8_t* base, uint32_t self, int index) {
  VCall(ctx, base, self, fe::kPageSetSelection, index, 1);
  VCall(ctx, base, self, fe::kPageSelectionChanged);
  Call c(ctx, base);
  c.Invoke(adv::kSyncValueHighlight, {self});
}

uint32_t AdvItem(uint8_t* base, uint32_t self, int index) {
  const uint32_t list = ReadU32(base, self + fe::kItems);
  if (list < 0x10000) return 0;
  const uint32_t item = ReadU32(base, list + 4 * index);
  return item >= 0x10000 && (item & 3) == 0 ? item : 0;
}

// Adds item `index` on `label` with a string key. False if the page refused it.
bool AddLabelItem(PPCContext& ctx, uint8_t* base, uint32_t self, int index, uint32_t label,
                  const char* key) {
  VCall(ctx, base, self, fe::kPageAddItem, index, label, 0);
  const uint32_t item = AdvItem(base, self, index);
  if (!item) return false;
  SetItemLabelKey(ctx, base, self, index, key);
  SetVisible(ctx, base, label, true);
  return true;
}

void SetItemShown(PPCContext& ctx, uint8_t* base, uint32_t item, bool shown) {
  if (!item) return;
  VCall(ctx, base, item, fe::kItemSetDisabled, shown ? 0 : 1);
  VCall(ctx, base, item, fe::kItemSetVisible, shown ? 1 : 0);
}

// The line under the video rows for the selected row.
void ShowAdvNote(PPCContext& ctx, uint8_t* base, uint32_t self, AdvRows& r) {
  const int sel = Selected(base, self) - kAdvVideoFirst;
  const std::string text = sel >= 0 && sel < PageOf(r).rows ? PageOf(r).note(sel) : std::string();
  if (text == r.note_text) return;
  r.note_text = text;
  SetTextRaw(ctx, base, r.note, text.empty() ? " " : text);
}

void ShowAdvVideoValues(PPCContext& ctx, uint8_t* base, AdvRows& r) {
  const float normal = ReadF32(base, fe::kArrowAlphaNormal);
  for (int i = 0; i < PageOf(r).rows; ++i) {
    r.seen[i] = PageOf(r).value(i);
    SetTextRaw(ctx, base, r.vrow[i].text_b, r.seen[i]);
    SetAlpha(ctx, base, r.vrow[i].arrow_left, normal);
    SetAlpha(ctx, base, r.vrow[i].arrow_right, normal);
  }
}

void EnterAdvVideo(PPCContext& ctx, uint8_t* base, uint32_t self, AdvRows& r, int page) {
  if (!r.has_video || r.video) return;
  if (page == kPageMovement && !r.has_movement) return;  // [movement]
  r.page = page;
  const int count = static_cast<int16_t>(ReadU16(base, self + fe::kItemCount));
  for (int i = 0; i < count && i < kAdvItemsAll; ++i) {
    if (IsSubRow(i) || i == kAdvBrightItem) continue;  // shown below, by page
    const uint32_t item = AdvItem(base, self, i);
    if (!item) continue;
    const uint32_t label = ReadU32(base, item + 16);
    r.saved_disabled[i] = ReadU8(base, item + 22);
    r.saved_visible[i] = label >= 0x10000 && (ReadU8(base, label + 16) & 0x4);
    SetItemShown(ctx, base, item, false);
  }
  for (int i = 0; i < kAdvSubRows; ++i) {
    const bool used = i < PageOf(r).rows;
    if (used) SetItemLabelKey(ctx, base, self, kAdvVideoFirst + i, PageOf(r).label_key(i));  // [movement]
    SetItemShown(ctx, base, AdvItem(base, self, kAdvVideoFirst + i), used);
  }
  if (r.has_bright)  // [menus-brightness] on the video page only [movement]
    SetItemShown(ctx, base, AdvItem(base, self, kAdvBrightItem), page == kPageVideo);
  SetTextKey(ctx, base, r.title, PageOf(r).title_key());
  r.video = true;
  r.note_text = "-";  // shown by ShowAdvNote below
  ShowAdvVideoValues(ctx, base, r);
  FollowTank(base, r);
  AdvSelect(ctx, base, self, kAdvVideoFirst);
  ShowAdvNote(ctx, base, self, r);
  REXLOG_INFO("cod3: pause menu: {} options", page == kPageMovement ? "movement" : "video");
}

void LeaveAdvVideo(PPCContext& ctx, uint8_t* base, uint32_t self, AdvRows& r, bool select_row) {
  if (!r.video) return;
  for (int i = 0; i < kAdvSubRows; ++i) SetItemShown(ctx, base, AdvItem(base, self, kAdvVideoFirst + i), false);
  if (r.has_bright) SetItemShown(ctx, base, AdvItem(base, self, kAdvBrightItem), false);  // [menus-brightness]
  const int count = static_cast<int16_t>(ReadU16(base, self + fe::kItemCount));
  for (int i = 0; i < count && i < kAdvItemsAll; ++i) {
    if (IsSubRow(i) || i == kAdvBrightItem) continue;
    const uint32_t item = AdvItem(base, self, i);
    if (!item) continue;
    VCall(ctx, base, item, fe::kItemSetDisabled, r.saved_disabled[i] ? 1 : 0);
    VCall(ctx, base, item, fe::kItemSetVisible, r.saved_visible[i] ? 1 : 0);
  }
  SetTextKey(ctx, base, r.title, cod3::guest::ReadString(base, adv::kTitleKey, 64).c_str());
  r.video = false;
  r.reopened = false;
  if (select_row) AdvSelect(ctx, base, self, r.page == kPageMovement ? kAdvMovementItem : kAdvVideoItem);
}

// Adds VIDEO OPTIONS and the video rows (Init, after the FOV rows). Returns
// the page's item count.
int AddAdvVideoRows(PPCContext& ctx, uint8_t* base, uint32_t self, uint32_t panel, AdvRows& r) {
  auto valid = [](uint32_t p) { return p >= 0x10000 && (p & 3) == 0; };
  r.invert[0] = FindElement(ctx, base, panel, "InvertAim");
  r.invert[1] = FindElement(ctx, base, panel, "InvertAimOnOff");
  r.invert[2] = FindQuad(ctx, base, panel, "arrowL1");
  r.invert[3] = FindQuad(ctx, base, panel, "arrowR1");
  r.title = FindElement(ctx, base, panel, "AdvancedOptions");
  for (uint32_t e : {r.invert[0], r.invert[1], r.invert[2], r.invert[3], r.title, r.reset}) {
    if (!valid(e)) {
      REXLOG_WARN("cod3: in-game options panel isn't the expected one; video options not added");
      return kAdvScaleItem + 1;
    }
  }
  // Copies: VIDEO OPTIONS and the note from Reset's label, the rows from the
  // ADS row (all owned by the page, freed with it).
  const FovRow& ads = r.row[0];
  r.video_label = CopyElement(ctx, base, r.reset, false, 0.0f);
  r.note = CopyElement(ctx, base, r.reset, false, 0.0f);
  bool ok = valid(r.video_label) && valid(r.note);
  for (FovRow& v : r.vrow) {
    v.text_a = CopyElement(ctx, base, ads.text_a, false, 0.0f);
    v.text_b = CopyElement(ctx, base, ads.text_b, false, 0.0f);
    v.arrow_left = CopyElement(ctx, base, ads.arrow_left, true, 0.0f);
    v.arrow_right = CopyElement(ctx, base, ads.arrow_right, true, 0.0f);
    ok = ok && valid(v.text_a) && valid(v.text_b);
    for (uint32_t e : {v.text_b, v.arrow_left, v.arrow_right}) SetVisible(ctx, base, e, true);
  }
  if (ok) {
    SetVisible(ctx, base, r.note, true);
    SetTextRaw(ctx, base, r.note, " ");
    r.has_video = true;
    FollowTank(base, r);
  }
  if (!ok || !AddLabelItem(ctx, base, self, kAdvVideoItem, r.video_label, vm::PauseItemKey())) {
    r.has_video = false;
    REXLOG_WARN("cod3: in-game options: VIDEO OPTIONS not added");
    return kAdvScaleItem + 1;  // the copies are freed with the page (dtor hook)
  }
  int used = kAdvVideoItem + 1;
  for (int i = 0; i < kAdvSubRows; ++i) {
    const char* key = i < kVideoPage.rows ? kVideoPage.label_key(i) : kMovementPage.label_key(i);
    if (!AddLabelItem(ctx, base, self, kAdvVideoFirst + i, r.vrow[i].text_a, key)) break;
    SetItemShown(ctx, base, AdvItem(base, self, kAdvVideoFirst + i), false);
    r.vrow[i].enabled = true;
    used = kAdvVideoFirst + i + 1;
  }
  if (used != kAdvItemsMax) {  // all rows or none
    SetItemShown(ctx, base, AdvItem(base, self, kAdvVideoItem), false);
    r.has_video = false;
    REXLOG_WARN("cod3: in-game options: video rows not added");
    return used;
  }
  // What the video rows replace on screen: the options' values, arrows and
  // gauges, and the FIELD OF VIEW row's (the ADS row's) own.
  for (int i = 0; i < 6; ++i) {
    const std::string name = cod3::guest::ReadString(base, ReadU32(base, adv::kValueNames + 4 * i), 32);
    if (uint32_t e = FindElement(ctx, base, panel, name.c_str()); valid(e)) r.hide.push_back(e);
  }
  for (int i = 0; i < 12; ++i) {
    const std::string name = cod3::guest::ReadString(base, ReadU32(base, adv::kArrowNames + 4 * i), 32);
    if (uint32_t e = FindQuad(ctx, base, panel, name.c_str())) r.hide.push_back(e);
  }
  for (const char* name : {"HSensProgbar1", "HSensProgbar2", "VSensProgBar1", "VSensProgbar2",
                           "MusicVolProgbar", "MusicVolumeProgbar2"}) {
    uint32_t e = FindQuad(ctx, base, panel, name);
    if (!e) e = FindElement(ctx, base, panel, name);
    if (valid(e)) r.hide.push_back(e);
  }
  for (uint32_t e : {ads.text_b, ads.arrow_left, ads.arrow_right})
    if (valid(e)) r.hide.push_back(e);
  return used;
}

// [menus-brightness] Adds BRIGHTNESS (Init, after the video rows): a copy of
// the ADS row's label and of Volume's gauge, shown with the video rows.
// Returns the page's item count.
int AddAdvBrightRow(PPCContext& ctx, uint8_t* base, uint32_t self, uint32_t panel, AdvRows& r) {
  auto valid = [](uint32_t p) { return p >= 0x10000 && (p & 3) == 0; };
  r.volume_label = FindElement(ctx, base, panel, "MusicVol");
  r.volume_back = FindQuad(ctx, base, panel, "MusicVolProgbar");
  r.volume_fill = FindQuad(ctx, base, panel, "MusicVolumeProgbar2");
  if (!valid(r.volume_label) || !r.volume_back || !r.volume_fill) {
    REXLOG_WARN("cod3: in-game options panel has no Volume gauge; BRIGHTNESS not added");
    r.volume_label = 0;
    return kAdvItemsMax;
  }
  // Owned by the page (freed with it, dtor hook), whatever happens below.
  r.bright_label = CopyElement(ctx, base, r.row[0].text_a, false, 0.0f);
  r.bright_back = CopyElement(ctx, base, r.volume_back, true, 0.0f);
  r.bright_fill = CopyElement(ctx, base, r.volume_fill, true, 0.0f);
  if (!valid(r.bright_label) || !valid(r.bright_back) || !valid(r.bright_fill)) {
    REXLOG_WARN("cod3: in-game options: BRIGHTNESS not added");
    return kAdvItemsMax;
  }
  for (uint32_t e : {r.bright_back, r.bright_fill}) SetVisible(ctx, base, e, true);
  r.has_bright = true;
  FollowTank(base, r);
  if (!AddLabelItem(ctx, base, self, kAdvBrightItem, r.bright_label, vm::BrightnessKey())) {
    r.has_bright = false;
    REXLOG_WARN("cod3: in-game options refused the BRIGHTNESS item");
    return kAdvItemsMax;
  }
  SetItemShown(ctx, base, AdvItem(base, self, kAdvBrightItem), false);  // shown with the video rows
  ShowBrightFill(ctx, base, r);
  REXLOG_INFO("cod3: in-game options: BRIGHTNESS added to VIDEO OPTIONS (gauge scale {:.3f}, the copy's {:.3f})",
              ReadF32(base, r.volume_fill + br::kQuadFillScale), ReadF32(base, r.bright_fill + br::kQuadFillScale));
  return kAdvMovementItem;
}

// [movement] Adds MOVEMENT OPTIONS (Init, after BRIGHTNESS): one more copy of
// Reset's label, shown under VIDEO OPTIONS. Returns the page's item count.
int AddAdvMovementRow(PPCContext& ctx, uint8_t* base, uint32_t self, AdvRows& r) {
  auto valid = [](uint32_t p) { return p >= 0x10000 && (p & 3) == 0; };
  r.movement_label = CopyElement(ctx, base, r.reset, false, 0.0f);  // owned by the page (dtor hook)
  if (!valid(r.movement_label) ||
      !AddLabelItem(ctx, base, self, kAdvMovementItem, r.movement_label, mm::PauseItemKey())) {
    REXLOG_WARN("cod3: in-game options: MOVEMENT OPTIONS not added");
    if (valid(r.movement_label)) SetVisible(ctx, base, r.movement_label, false);
    return kAdvMovementItem;
  }
  r.has_movement = true;
  FollowTank(base, r);
  REXLOG_INFO("cod3: in-game options: MOVEMENT OPTIONS added");
  return kAdvItemsAll;
}
}  // namespace

namespace cod3::videomenu {
void ResetPauseOptions() {
  // Marks the pages; the page's next Refresh / Update brings its options back.
  std::lock_guard lock(g_mutex);
  for (auto& [page, r] : g_adv)
    if (r.video) r.reopened = true;
}
}  // namespace cod3::videomenu

// AdvancedOptionsMenu::Init(this, panel)
REX_EXTERN(__imp__sub_824E60E8);
REX_HOOK_RAW(sub_824E60E8) {
  const uint32_t self = ctx.r3.u32;
  __imp__sub_824E60E8(ctx, base);
  vm::NoteStartValues();
  if (ReadU32(base, self) != adv::kVtable) return;
  auto valid = [](uint32_t p) { return p >= 0x10000 && (p & 3) == 0; };
  const uint32_t panel = ReadU32(base, self + adv::kPanel);
  if (!valid(panel) || static_cast<int16_t>(ReadU16(base, self + fe::kItemCount)) != adv::kItemCount)
    return;

  AdvRows r;
  r.tank[0] = FindElement(ctx, base, panel, "TankControl");
  r.tank[1] = FindElement(ctx, base, panel, "TankControlOnOff");
  r.tank[2] = FindQuad(ctx, base, panel, "arrowL6");
  r.tank[3] = FindQuad(ctx, base, panel, "arrowR6");
  const uint32_t subtitle = FindElement(ctx, base, panel, "Subtitle");
  r.reset = FindElement(ctx, base, panel, "Reset2Defaults");
  FovRow& fov = r.row[0];
  fov.text_a = FindElement(ctx, base, panel, "ADSToggle");
  fov.text_b = FindElement(ctx, base, panel, "ADSToggleOnOff");
  fov.arrow_left = FindQuad(ctx, base, panel, "arrowL7");
  fov.arrow_right = FindQuad(ctx, base, panel, "arrowR7");
  for (uint32_t e : {r.tank[0], r.tank[1], r.tank[2], r.tank[3], subtitle, fov.text_a, fov.text_b,
                     fov.arrow_left, fov.arrow_right}) {
    if (!valid(e)) {
      REXLOG_WARN("cod3: in-game options panel isn't the expected one; FOV options not added");
      return;
    }
  }
  r.pitch = ReadF32(base, r.tank[0] + fe::kTextY) - ReadF32(base, subtitle + fe::kTextY);
  if (!(r.pitch > 5.0f && r.pitch < 60.0f)) return;
  if (valid(r.reset))
    r.reset_gap = ReadF32(base, r.reset + fe::kTextY) - ReadF32(base, r.tank[0] + fe::kTextY);
  else
    r.reset = 0;

  // Room for our items: a bigger array in place of the page's.
  const uint32_t old_items = ReadU32(base, self + fe::kItems);
  uint32_t items = 0;
  {
    Call c(ctx, base);
    items = static_cast<uint32_t>(c.Invoke(fe::kOperatorNew, {4u * kAdvItemsAll}));  // [menus-brightness]
  }
  if (!valid(items) || !valid(old_items)) return;
  for (int i = 0; i < kAdvItemsAll; ++i)
    cod3::guest::WriteU32(base, items + 4 * i, i < adv::kItemCount ? ReadU32(base, old_items + 4 * i) : 0);
  cod3::guest::WriteU32(base, self + fe::kItems, items);
  {
    Call c(ctx, base);
    c.Invoke(fe::kOperatorDelete, {old_items});
  }

  // FOV SCALE: a copy of the (parked) ADS row; FollowTank places both.
  FovRow& sc = r.row[1];
  sc.text_a = CopyElement(ctx, base, fov.text_a, false, 0.0f);
  sc.text_b = CopyElement(ctx, base, fov.text_b, false, 0.0f);
  sc.arrow_left = CopyElement(ctx, base, fov.arrow_left, true, 0.0f);
  sc.arrow_right = CopyElement(ctx, base, fov.arrow_right, true, 0.0f);
  FollowTank(base, r);

  int used = adv::kItemCount;
  if (AddRow(ctx, base, self, kAdvFovItem, fov, kLabelKey, true)) {
    used = kAdvFovItem + 1;
    SetTextRaw(ctx, base, fov.text_b, FovText(CurrentFov()));
    if (valid(sc.text_a) && valid(sc.text_b) &&
        AddRow(ctx, base, self, kAdvScaleItem, sc, kScaleLabelKey, true)) {
      used = kAdvScaleItem + 1;
      SetTextRaw(ctx, base, sc.text_b, ScaleText(CurrentScale()));
      if (r.reset) used = AddAdvVideoRows(ctx, base, self, panel, r);
      if (used == kAdvItemsMax) used = AddAdvBrightRow(ctx, base, self, panel, r);  // [menus-brightness]
      if (used == kAdvMovementItem) used = AddAdvMovementRow(ctx, base, self, r);  // [movement]
    }
  }
  SetItemCount(base, self, used);
  FollowTank(base, r);

  std::lock_guard lock(g_mutex);
  g_adv[self] = r;
}

REX_EXTERN(__imp__AdvancedOptionsMenu__dtor);
REX_HOOK_RAW(AdvancedOptionsMenu__dtor) {
  const uint32_t self = ctx.r3.u32;
  AdvRows r;
  bool known = false;
  {
    std::lock_guard lock(g_mutex);
    if (auto it = g_adv.find(self); it != g_adv.end()) {
      r = it->second;
      known = true;
      g_adv.erase(it);
    }
  }
  __imp__AdvancedOptionsMenu__dtor(ctx, base);
  if (!known) return;
  std::vector<uint32_t> copies;
  const FovRow& sc = r.row[1];
  copies.insert(copies.end(), {sc.text_a, sc.text_b, sc.arrow_left, sc.arrow_right, r.video_label, r.note,
                               r.movement_label});  // [movement]
  for (const FovRow& v : r.vrow)
    copies.insert(copies.end(), {v.text_a, v.text_b, v.arrow_left, v.arrow_right});
  copies.insert(copies.end(), {r.bright_label, r.bright_back, r.bright_fill});  // [menus-brightness]
  for (uint32_t e : copies)
    if (e) VCall(ctx, base, e, fe::kElemDelete, 1);
}

// Refresh (the page opens): the values again (they may have changed in F2
// or the console), and the game's options rather than the video rows when
// the pause menu's Options item opened it.
REX_EXTERN(__imp__sub_824E6588);
REX_HOOK_RAW(sub_824E6588) {
  const uint32_t self = ctx.r3.u32;
  __imp__sub_824E6588(ctx, base);
  AdvRows r;
  if (!GetAdv(base, self, r)) return;
  SetTextRaw(ctx, base, r.row[0].text_b, FovText(CurrentFov()));
  SetTextRaw(ctx, base, r.row[1].text_b, ScaleText(CurrentScale()));
  if (r.video && r.reopened) {
    LeaveAdvVideo(ctx, base, self, r, /*select_row=*/false);
    AdvSelect(ctx, base, self, 0);
  } else if (r.video) {
    ShowAdvVideoValues(ctx, base, r);
    ShowAdvNote(ctx, base, self, r);
  }
  PutAdv(self, r);
}

// Update: keep our rows in place (before the menu reads the pointer, so the
// mouse finds them where they are drawn); with the video rows up, values
// changed in F2 or the console show at once.
REX_EXTERN(__imp__sub_824EE128);
REX_HOOK_RAW(sub_824EE128) {
  const uint32_t self = ctx.r3.u32;
  __imp__sub_824EE128(ctx, base);
  AdvRows r;
  if (!GetAdv(base, self, r)) return;
  FollowTank(base, r);
  ShowBrightFill(ctx, base, r);  // [menus-brightness]
  if (!r.video) return;
  if (r.reopened) {  // opened again from the pause menu
    LeaveAdvVideo(ctx, base, self, r, /*select_row=*/false);
    AdvSelect(ctx, base, self, 0);
    PutAdv(self, r);
    return;
  }
  bool changed = false;
  for (int i = 0; i < PageOf(r).rows; ++i) {
    const std::string now = PageOf(r).value(i);
    if (now == r.seen[i]) continue;
    r.seen[i] = now;
    SetTextRaw(ctx, base, r.vrow[i].text_b, now);
    changed = true;
  }
  const std::string before = r.note_text;
  ShowAdvNote(ctx, base, self, r);
  if (changed || before != r.note_text) PutAdv(self, r);
}

// Draw: panel, items (our labels too), values; then our copied values and
// arrows. With the video rows up, the options' values, arrows and gauges are
// skipped while the page draws (their visible flag cleared and put back).
REX_EXTERN(__imp__AdvancedOptionsMenu__vfunc_14);
REX_HOOK_RAW(AdvancedOptionsMenu__vfunc_14) {
  const uint32_t self = ctx.r3.u32;
  AdvRows r;
  const bool ours = GetAdv(base, self, r);
  if (ours) FollowTank(base, r);
  if (ours) ShowBrightFill(ctx, base, r);  // [menus-brightness]
  if (!ours || !r.video) {
    __imp__AdvancedOptionsMenu__vfunc_14(ctx, base);
    if (!ours) return;
    for (uint32_t e : {r.row[1].arrow_left, r.row[1].arrow_right, r.row[1].text_b})
      if (e) VCall(ctx, base, e, fe::kElemDraw);
    return;
  }
  std::vector<std::pair<uint32_t, uint8_t>> shown;
  for (uint32_t e : r.hide) {
    const uint8_t flags = ReadU8(base, e + 16);
    if (!(flags & 0x4)) continue;
    shown.emplace_back(e, flags);
    cod3::guest::WriteU8(base, e + 16, uint8_t(flags & ~0x4));
  }
  __imp__AdvancedOptionsMenu__vfunc_14(ctx, base);
  for (const auto& [e, flags] : shown) cod3::guest::WriteU8(base, e + 16, flags);
  for (int i = 0; i < PageOf(r).rows; ++i)
    for (uint32_t e : {r.vrow[i].arrow_left, r.vrow[i].arrow_right, r.vrow[i].text_b})
      if (e) VCall(ctx, base, e, fe::kElemDraw);
  if (BrightShown(r))  // [menus-brightness] BRIGHTNESS's gauge: frame, then fill
    for (uint32_t e : {r.bright_back, r.bright_fill})
      if (e) VCall(ctx, base, e, fe::kElemDraw);
  if (r.note) VCall(ctx, base, r.note, fe::kElemDraw);
}

// MoveDown / MoveUp: Tank Controls (8) -> FIELD OF VIEW (11) -> FOV SCALE
// (12) -> VIDEO OPTIONS (13) -> MOVEMENT OPTIONS (22) -> Reset (9), the order
// they are shown in; a sub page's rows go round among themselves.
REX_EXTERN(__imp__sub_824C41B8);
REX_HOOK_RAW(sub_824C41B8) {
  const uint32_t self = ctx.r3.u32;
  AdvRows r;
  const int sel = Selected(base, self);
  const int count = static_cast<int16_t>(ReadU16(base, self + fe::kItemCount));
  if (GetAdv(base, self, r) && count > kAdvFovItem) {
    if (r.video) {
      const int last = SubPageLast(r);
      if ((IsSubRow(sel) || sel == kAdvBrightItem) && sel <= last)
        AdvSelect(ctx, base, self, sel < last ? sel + 1 : kAdvVideoFirst);
      return;
    }
    const bool scale = count > kAdvScaleItem;
    if (sel == adv::kTankItem) return AdvSelect(ctx, base, self, kAdvFovItem);
    if (sel == kAdvFovItem) return AdvSelect(ctx, base, self, scale ? kAdvScaleItem : adv::kResetItem);
    if (sel == kAdvScaleItem) return AdvSelect(ctx, base, self, r.has_video ? kAdvVideoItem : adv::kResetItem);
    if (sel == kAdvVideoItem)
      return AdvSelect(ctx, base, self, r.has_movement ? kAdvMovementItem : adv::kResetItem);  // [movement]
    if (sel == kAdvMovementItem) return AdvSelect(ctx, base, self, adv::kResetItem);  // [movement]
  }
  __imp__sub_824C41B8(ctx, base);
}

REX_EXTERN(__imp__sub_824C4218);
REX_HOOK_RAW(sub_824C4218) {
  const uint32_t self = ctx.r3.u32;
  AdvRows r;
  const int sel = Selected(base, self);
  const int count = static_cast<int16_t>(ReadU16(base, self + fe::kItemCount));
  if (GetAdv(base, self, r) && count > kAdvFovItem) {
    if (r.video) {
      const int last = SubPageLast(r);
      if ((IsSubRow(sel) || sel == kAdvBrightItem) && sel <= last)
        AdvSelect(ctx, base, self, sel > kAdvVideoFirst ? sel - 1 : last);
      return;
    }
    const bool scale = count > kAdvScaleItem;
    if (sel == adv::kResetItem)
      return AdvSelect(ctx, base, self,
                       r.has_movement ? kAdvMovementItem  // [movement]
                       : r.has_video  ? kAdvVideoItem
                       : scale        ? kAdvScaleItem
                                      : kAdvFovItem);
    if (sel == kAdvMovementItem) return AdvSelect(ctx, base, self, kAdvVideoItem);  // [movement]
    if (sel == kAdvVideoItem) return AdvSelect(ctx, base, self, kAdvScaleItem);
    if (sel == kAdvScaleItem) return AdvSelect(ctx, base, self, kAdvFovItem);
    if (sel == kAdvFovItem) return AdvSelect(ctx, base, self, adv::kTankItem);
  }
  __imp__sub_824C4218(ctx, base);
}

// OnLeft / OnRight (this, right): our rows step cg_fov / cg_fovScale, the
// video rows their settings.
REX_EXTERN(__imp__sub_824DC1C0);
REX_HOOK_RAW(sub_824DC1C0) {
  const uint32_t self = ctx.r3.u32;
  const bool right = (ctx.r4.u32 & 0xFF) != 0;
  const int sel = Selected(base, self);
  AdvRows r;
  if ((sel == kAdvFovItem || sel == kAdvScaleItem) && GetAdv(base, self, r)) {
    const FovRow& row = r.row[sel - kAdvFovItem];
    SetTextRaw(ctx, base, row.text_b, StepValue(ctx, base, sel == kAdvScaleItem, right));
    return;
  }
  if (IsSubRow(sel) && GetAdv(base, self, r) && r.video && sel - kAdvVideoFirst < PageOf(r).rows) {
    const int i = sel - kAdvVideoFirst;
    const std::string text = PageOf(r).step(ctx, base, i, right);
    if (!text.empty()) SetTextRaw(ctx, base, r.vrow[i].text_b, text);
    ShowAdvNote(ctx, base, self, r);
    PutAdv(self, r);
    return;
  }
  // [menus-brightness] BRIGHTNESS: the game's brightness, one D-pad step.
  if (sel == kAdvBrightItem && GetAdv(base, self, r) && BrightShown(r)) {
    vm::StepBrightness(base, right, "pause menu");
    ShowBrightFill(ctx, base, r);
    return;
  }
  if (sel >= kAdvVideoItem) return;  // VIDEO / MOVEMENT OPTIONS: no value
  __imp__sub_824DC1C0(ctx, base);
}

// The item handler (this, item): VIDEO OPTIONS shows the video rows,
// MOVEMENT OPTIONS the movement rows [movement].
REX_EXTERN(__imp__AdvancedOptionsMenu__vfunc_19);
REX_HOOK_RAW(AdvancedOptionsMenu__vfunc_19) {
  const uint32_t self = ctx.r3.u32;
  const int item = static_cast<int32_t>(ctx.r4.u32);
  AdvRows r;
  if (item >= kAdvVideoItem && GetAdv(base, self, r)) {
    if (item == kAdvVideoItem && r.has_video && !r.video) {
      EnterAdvVideo(ctx, base, self, r, kPageVideo);
      PutAdv(self, r);
    } else if (item == kAdvMovementItem && r.has_movement && !r.video) {  // [movement]
      EnterAdvVideo(ctx, base, self, r, kPageMovement);
      PutAdv(self, r);
    }
    return;  // A on a sub page's row: nothing, like the game's option rows
  }
  __imp__AdvancedOptionsMenu__vfunc_19(ctx, base);
}

// B: from the video rows back to the options (the page stays); otherwise the
// game's own (save the options if changed, back to the pause menu).
REX_EXTERN(__imp__AdvancedOptionsMenu__vfunc_30);
REX_HOOK_RAW(AdvancedOptionsMenu__vfunc_30) {
  const uint32_t self = ctx.r3.u32;
  AdvRows r;
  if (GetAdv(base, self, r) && r.video) {
    LeaveAdvVideo(ctx, base, self, r, /*select_row=*/true);
    PutAdv(self, r);
    return;
  }
  if (GetAdv(base, self, r) && r.has_bright) {  // [menus-brightness] see ControlsPage_Apply
    r.bright_page = vm::Brightness(base);
    PutAdv(self, r);
  }
  __imp__AdvancedOptionsMenu__vfunc_30(ctx, base);
}

// [menus-brightness] ControlsPage_Apply (this): swaps the page's values with
// the player's options. B swaps, compares the options with a copy taken
// before ("save changes?" when they differ) and swaps back; that box's Yes
// swaps and saves. The brightness takes part the same way: the page's value
// is the one in use when B was pressed (bright_page).
REX_EXTERN(__imp__ControlsPage_Apply);
REX_HOOK_RAW(ControlsPage_Apply) {
  const uint32_t self = ctx.r3.u32;
  __imp__ControlsPage_Apply(ctx, base);
  AdvRows r;
  if (!GetAdv(base, self, r) || !r.has_bright) return;
  r.bright_page = vm::ExchangeProfileBrightness(base, r.bright_page);
  PutAdv(self, r);
}

namespace cod3::videomenu {
// [menus-brightness] See menu_video.h: the SCREEN page's own slider and the
// pause menu's BRIGHTNESS row set the game's brightness directly.
bool SetSlider(uint8_t* base, uint32_t page, int item, float fraction) {
  if (page < 0x10000) return false;
  const uint32_t vt = ReadU32(base, page);
  bool ours = vt == br::kPageVtable;
  if (!ours && vt == adv::kVtable && item == kAdvBrightItem) {
    std::lock_guard lock(g_mutex);
    auto it = g_adv.find(page);
    ours = it != g_adv.end() && BrightShown(it->second);
  }
  if (ours) SetBrightnessFraction(base, fraction);
  return ours;
}
}  // namespace cod3::videomenu

// const char* Localize(StringTable*, const char* key)
REX_EXTERN(__imp__Localize);
REX_HOOK_RAW(Localize) {
  const uint32_t key = ctx.r4.u32;
  if (key) {
    const char* k = reinterpret_cast<const char*>(base + key);
    struct Text {
      const char* key;
      const char* text;
      uint32_t* guest;
    };
    const Text texts[] = {
        {kLabelKey, kLabelText, &g_label_str},
        {kInstKey, kInstText, &g_inst_str},
        {kScaleLabelKey, kScaleLabelText, &g_scale_label_str},
        {kScaleInstKey, kScaleInstText, &g_scale_inst_str},
    };
    for (const auto& t : texts) {
      if (std::strcmp(k, t.key) != 0) continue;
      if (!*t.guest) *t.guest = GuestString(base, t.text);
      ctx.r3.u64 = *t.guest ? *t.guest : key;
      return;
    }
    // The video settings' texts (game/menu_video.h).
    if (const uint32_t text = cod3::videomenu::Localize(base, k)) {
      ctx.r3.u64 = text;
      return;
    }
    // [movement] The movement settings' texts (game/movement/menu_movement.h).
    if (const uint32_t text = cod3::movementmenu::Localize(base, k)) {
      ctx.r3.u64 = text;
      return;
    }
  }
  __imp__Localize(ctx, base);
  // Stick battle action hints for the mouse (game/qte_actions.h).
  if (const uint32_t text = cod3::qte::LocalizeHint(base, ctx.r3.u32)) ctx.r3.u64 = text;
}
