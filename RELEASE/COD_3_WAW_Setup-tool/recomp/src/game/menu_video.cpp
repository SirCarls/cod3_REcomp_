// cod3 - the port's video settings in the game's own Options menus (see
// menu_video.h). This file holds the campaign's front end Options page (its
// new VIDEO item), the texts and the brightness; the rows themselves (what
// each shows and changes) are menu_video_rows.cpp's, which the multiplayer
// program builds too [menus-categories]. The pages that show the rows
// (Options > Gameplay in video mode, the pause menu's options) are in
// menu_fov.cpp, next to the field of view rows on the same pages.

#include "game/menu_video.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>

#include "game/addresses.h"
#include "game/guest.h"

namespace {

namespace fe = cod3::addr::fe;
namespace opt = cod3::addr::fe::options;
using cod3::guest::Call;
using cod3::guest::ReadF32;
using cod3::guest::ReadU16;
using cod3::guest::ReadU32;
using cod3::guest::VirtualSlot;
using cod3::videomenu::Row;

// ---------------------------------------------------------------------------
// Texts
// ---------------------------------------------------------------------------
struct Text {
  const char* key;
  const char* text;  // nullptr: made when asked (HelpText)
};

constexpr const char* kTitleKey = "FEMENU_OP_VIDEO_TITLE";
constexpr const char* kItemKey = "FEMENU_OP_VIDEO";
constexpr const char* kItemInstKey = "FEMENU_OP_INST_VIDEO";
constexpr const char* kPauseItemKey = "FEMENU_COP_VIDEO_OPTIONS";
constexpr const char* kQuitToMenuKey = "FEMENU_PAUSE_QUIT_TO_MENU";
constexpr const char* kQuitToDesktopKey = "FEMENU_PAUSE_QUIT_TO_DESKTOP";
constexpr const char* kQuitToTitleKey = "FEMENU_MAIN_QUIT_TO_TITLE";
constexpr const char* kQuitQuestionKey = "FEMENU_QUIT_TO_DESKTOP_QUESTION";
constexpr const char* kBrightnessKey = "FEMENU_COP_BRIGHTNESS";  // [menus-brightness]

constexpr Text kTexts[] = {
    {kTitleKey, "OPTIONS / VIDEO"},
    {kItemKey, "VIDEO"},
    {kItemInstKey,
     "Resolution, frame rate limit, V-Sync, anti-aliasing, shadows and HD textures. The same "
     "settings are in F2."},
    {kPauseItemKey, "VIDEO OPTIONS"},
    {kQuitToMenuKey, "QUIT TO MENU"},
    {kQuitToDesktopKey, "QUIT TO DESKTOP"},
    {kQuitToTitleKey, "QUIT TO TITLE SCREEN"},
    {kQuitQuestionKey, "Quit the game and go back to the desktop?"},
    {kBrightnessKey, "BRIGHTNESS"},  // [menus-brightness]
};

// Guest copies of the texts: fixed ones once, help texts in a buffer of
// their own that is rewritten whenever the game asks for the key.
constexpr uint32_t kHelpBufferSize = 512;
std::mutex g_text_mutex;
std::unordered_map<std::string, uint32_t> g_guest_texts;

uint32_t GuestAlloc(uint32_t size) {
  auto* mem = rex::system::kernel_state()->memory();
  return mem->SystemHeapAlloc(size);
}

uint32_t FixedText(uint8_t* base, const char* key, const char* text) {
  std::lock_guard lock(g_text_mutex);
  uint32_t& addr = g_guest_texts[key];
  if (!addr) {
    const uint32_t len = uint32_t(std::strlen(text)) + 1;
    addr = GuestAlloc(len);
    if (addr) std::memcpy(base + addr, text, len);
  }
  return addr;
}

uint32_t HelpTextBuffer(uint8_t* base, const char* key, const std::string& text) {
  std::lock_guard lock(g_text_mutex);
  uint32_t& addr = g_guest_texts[key];
  if (!addr) addr = GuestAlloc(kHelpBufferSize);
  if (!addr) return 0;
  const size_t len = std::min<size_t>(text.size(), kHelpBufferSize - 1);
  std::memcpy(base + addr, text.data(), len);
  base[addr + len] = 0;
  return addr;
}

// ---------------------------------------------------------------------------
// Guest UI calls (the same as menu_fov.cpp's)
// ---------------------------------------------------------------------------
bool Valid(uint32_t p) { return p >= 0x10000 && p < 0xC0000000u && (p & 3) == 0; }

uint64_t VCall(PPCContext& ctx, uint8_t* base, uint32_t obj, uint32_t slot, uint64_t a1 = 0,
               uint64_t a2 = 0, uint64_t a3 = 0) {
  if (!obj) return 0;
  Call c(ctx, base);
  return c.Invoke(VirtualSlot(base, obj, slot), {obj, a1, a2, a3});
}

uint32_t FindElement(PPCContext& ctx, uint8_t* base, uint32_t panel, const char* name) {
  Call c(ctx, base);
  const uint32_t s = c.PushString(name);
  return static_cast<uint32_t>(c.Invoke(fe::kPanelFindElement, {panel, s}));
}

uint32_t FindQuad(PPCContext& ctx, uint8_t* base, uint32_t panel, const char* name) {
  Call c(ctx, base);
  const uint32_t s = c.PushString(name);
  const uint32_t quad = static_cast<uint32_t>(c.Invoke(fe::kPanelFindQuad, {panel, s}));
  if (!Valid(quad)) return 0;
  const uint32_t str = ReadU32(base, quad + fe::kQuadName);
  if (!str || cod3::guest::ReadString(base, str + 12, 64) != name) return 0;
  return quad;
}

void SetTextKey(PPCContext& ctx, uint8_t* base, uint32_t elem, const char* key) {
  if (!Valid(elem)) return;
  Call c(ctx, base);
  const uint32_t s = c.PushString(key);
  c.Invoke(VirtualSlot(base, elem, fe::kElemSetTextKey), {elem, s});
}

void SetInstructions(PPCContext& ctx, uint8_t* base, uint32_t box, const char* key) {
  if (!Valid(box)) return;
  Call c(ctx, base);
  const uint64_t width = c.Invoke(VirtualSlot(base, box, fe::kWrapGetWidth), {box});
  const uint32_t s = c.PushString(key);
  const float scale = ReadF32(base, fe::kTextScaleDefault);
  c.Invoke(VirtualSlot(base, box, fe::kWrapSetTextKey), {box, s, width}, {scale});
}

int16_t Selected(uint8_t* base, uint32_t page) {
  return static_cast<int16_t>(ReadU16(base, page + fe::kSelected));
}

void SetItemCount(uint8_t* base, uint32_t page, int count) {  // big-endian s16
  base[page + fe::kItemCount] = uint8_t(count >> 8);
  base[page + fe::kItemCount + 1] = uint8_t(count);
}

// ---------------------------------------------------------------------------
// Front end Options page: the VIDEO item
// ---------------------------------------------------------------------------
std::mutex g_pages_mutex;
std::unordered_set<uint32_t> g_options_pages;  // pages that have the VIDEO item
uint32_t g_back_from_video = 0;  // the Options page whose VIDEO page is open (g_pages_mutex)

bool HasVideoItem(uint8_t* base, uint32_t page) {
  if (!Valid(page) || ReadU32(base, page) != opt::kVtable) return false;
  std::lock_guard lock(g_pages_mutex);
  return g_options_pages.count(page) != 0;
}

// The big title and the help text for the VIDEO item.
void ShowVideoItem(PPCContext& ctx, uint8_t* base, uint32_t page) {
  SetTextKey(ctx, base, ReadU32(base, page + opt::kSelectedText), kItemKey);
  SetInstructions(ctx, base, ReadU32(base, page + opt::kInstructions), kItemInstKey);
}

// The Options > Gameplay page of the menu system that owns `page`.
uint32_t GameplayPage(uint8_t* base, uint32_t page) {
  const uint32_t system = ReadU32(base, page + fe::kPageSystem);
  if (!Valid(system)) return 0;
  const uint32_t pages = ReadU32(base, system + fe::kSysPages);
  if (!Valid(pages)) return 0;
  const uint32_t gameplay = ReadU32(base, pages + 4 * opt::kGameplayPage);
  return Valid(gameplay) ? gameplay : 0;
}

}  // namespace

// ===========================================================================
// Rows
// ===========================================================================
namespace cod3::videomenu {

const char* TitleKey() { return kTitleKey; }
const char* ItemKey() { return kItemKey; }
const char* ItemInstructionsKey() { return kItemInstKey; }
const char* PauseItemKey() { return kPauseItemKey; }
const char* QuitToMenuKey() { return kQuitToMenuKey; }
const char* QuitToDesktopKey() { return kQuitToDesktopKey; }
const char* QuitToTitleKey() { return kQuitToTitleKey; }
const char* QuitToDesktopQuestionKey() { return kQuitQuestionKey; }

// [menus-categories] The rows' own texts (labels, help texts) are
// game/menu_video_rows.cpp's, which both programs build.
uint32_t Localize(uint8_t* base, const char* key) {
  if (!key || std::strncmp(key, "FEMENU_", 7) != 0) return 0;
  for (const Text& t : kTexts)
    if (std::strcmp(key, t.key) == 0) return FixedText(base, t.key, t.text);
  for (int row = 0; row < kRowCount; ++row) {
    if (std::strcmp(key, LabelKey(row)) == 0) return FixedText(base, LabelKey(row), LabelText(row));
    if (std::strcmp(key, InstructionsKey(row)) == 0)
      return HelpTextBuffer(base, InstructionsKey(row), HelpText(row));
  }
  return 0;
}

// ---------------------------------------------------------------------------
// [menus-brightness] The game's brightness (addresses.h, brightness)
// ---------------------------------------------------------------------------
namespace {
namespace br = cod3::addr::brightness;

float BrightnessMax(uint8_t* base) {
  const float max = ReadF32(base, br::kMax);
  return std::isfinite(max) && max > br::kMin ? max : 2.0f;
}

void SetBrightnessValue(uint8_t* base, float value) {
  value = std::clamp(value, br::kMin, BrightnessMax(base));
  cod3::guest::WriteF32(base, br::kValue, value);
}
}  // namespace

float Brightness(uint8_t* base) {
  const float v = ReadF32(base, br::kValue);
  return std::isfinite(v) ? v : 1.0f;
}

float BrightnessFraction(uint8_t* base) {
  const float range = BrightnessMax(base) - br::kMin;
  return std::clamp((Brightness(base) - br::kMin) / range, 0.0f, 1.0f);
}

void SetBrightnessFraction(uint8_t* base, float fraction) {
  const float range = BrightnessMax(base) - br::kMin;
  SetBrightnessValue(base, br::kMin + std::clamp(fraction, 0.0f, 1.0f) * range);
}

void StepBrightness(uint8_t* base, bool right, const char* from) {
  float step = ReadF32(base, br::kStep);
  if (!(step > 0.0f && step < 1.0f)) step = 0.05f;
  SetBrightnessValue(base, Brightness(base) + (right ? step : -step));
  REXLOG_INFO("cod3: brightness {:.2f} ({})", Brightness(base), from);
}

float ExchangeProfileBrightness(uint8_t* base, float value) {
  const float old = ReadF32(base, br::kProfile + br::kProfileValue);
  cod3::guest::WriteF32(base, br::kProfile + br::kProfileValue, value);
  return old;
}

const char* BrightnessKey() { return kBrightnessKey; }

}  // namespace cod3::videomenu

// ===========================================================================
// Front end Options page (ProfileEditMenu): a fifth item, VIDEO
// ===========================================================================

// Init(this, panel): the game adds GAMEPLAY, CONTROLS, SOUND and SCREEN and
// hides the fifth row's band and line. VIDEO goes on that row: the page's
// item array gets a fifth slot (a bigger array in place of the page's, like
// the in-game options in menu_fov.cpp), the item is added on text_option_05
// and the row's band and line are shown again.
REX_EXTERN(__imp__sub_824E2948);
REX_HOOK_RAW(sub_824E2948) {
  const uint32_t self = ctx.r3.u32;
  __imp__sub_824E2948(ctx, base);
  cod3::videomenu::NoteStartValues();
  if (!Valid(self) || ReadU32(base, self) != opt::kVtable) return;
  // The other language layout moves the rows; there the settings stay in F2.
  if (ReadU32(base, fe::kLanguageMode) != 0) return;
  if (static_cast<int16_t>(ReadU16(base, self + fe::kItemCount)) != opt::kItemCount) return;
  const uint32_t panel = ReadU32(base, self + opt::kPanel);
  if (!Valid(panel)) return;
  const uint32_t label = FindElement(ctx, base, panel, "text_option_05");
  const uint32_t band = FindQuad(ctx, base, panel, "bkg_row_05");
  const uint32_t line = FindQuad(ctx, base, panel, "bkg_line_04");
  const uint32_t old_items = ReadU32(base, self + fe::kItems);
  if (!Valid(label) || !Valid(old_items)) {
    REXLOG_WARN("cod3: Options panel has no fifth row; the VIDEO item is not added");
    return;
  }

  uint32_t items = 0;
  {
    Call c(ctx, base);
    items = static_cast<uint32_t>(c.Invoke(fe::kOperatorNew, {4u * (opt::kItemCount + 1)}));
  }
  if (!Valid(items)) return;
  for (int i = 0; i < opt::kItemCount + 1; ++i)
    cod3::guest::WriteU32(base, items + 4 * i, i < opt::kItemCount ? ReadU32(base, old_items + 4 * i) : 0);
  cod3::guest::WriteU32(base, self + fe::kItems, items);
  {
    Call c(ctx, base);
    c.Invoke(fe::kOperatorDelete, {old_items});
  }

  // this->AddItem(4, text_option_05, 0); items[4]->SetLabelKey("VIDEO")
  VCall(ctx, base, self, fe::kPageAddItem, opt::kVideoItem, label, 0);
  const uint32_t item = ReadU32(base, items + 4 * opt::kVideoItem);
  if (!Valid(item)) {
    REXLOG_WARN("cod3: Options page refused the VIDEO item");
    return;  // the count stays 4: the page never reads the fifth slot
  }
  {
    Call c(ctx, base);
    const uint32_t s = c.PushString(kItemKey);
    c.Invoke(VirtualSlot(base, item, fe::kItemSetLabelKey), {item, s});
  }
  for (uint32_t e : {label, band, line})
    if (Valid(e)) VCall(ctx, base, e, fe::kElemSetVisible, 1);
  SetItemCount(base, self, opt::kItemCount + 1);

  std::lock_guard lock(g_pages_mutex);
  g_options_pages.insert(self);
}

REX_EXTERN(__imp__ProfileEditMenu__dtor);
REX_HOOK_RAW(ProfileEditMenu__dtor) {
  {
    std::lock_guard lock(g_pages_mutex);
    g_options_pages.erase(ctx.r3.u32);
  }
  __imp__ProfileEditMenu__dtor(ctx, base);
}

// The item handler (this, item): GAMEPLAY opens the gameplay page as itself,
// VIDEO opens it in video mode the same way (FEMenuSystem "go to page 3").
REX_EXTERN(__imp__ProfileEditMenu__vfunc_19);
REX_HOOK_RAW(ProfileEditMenu__vfunc_19) {
  const uint32_t self = ctx.r3.u32;
  const int item = static_cast<int32_t>(ctx.r4.u32);
  if (HasVideoItem(base, self) && (item == 0 || item == opt::kVideoItem)) {
    const uint32_t gameplay = GameplayPage(base, self);
    if (gameplay) cod3::videomenu::SetGameplayPageMode(gameplay, item == opt::kVideoItem);
    if (item == opt::kVideoItem) {
      if (!gameplay) return;
      {
        std::lock_guard lock(g_pages_mutex);
        g_back_from_video = self;
      }
      const uint32_t system = ReadU32(base, self + fe::kPageSystem);
      VCall(ctx, base, system, fe::kSysGoTo, opt::kGameplayPage, 0);
      return;
    }
  }
  __imp__ProfileEditMenu__vfunc_19(ctx, base);
}

// Opening the page and moving the highlight set the big title and the help
// text from two four-entry tables; for VIDEO they are ours. Coming back from
// the VIDEO page, VIDEO stays highlighted (the game starts at the top).
REX_EXTERN(__imp__ProfileEditMenu__vfunc_20);
REX_HOOK_RAW(ProfileEditMenu__vfunc_20) {
  const uint32_t self = ctx.r3.u32;
  __imp__ProfileEditMenu__vfunc_20(ctx, base);
  if (!HasVideoItem(base, self)) return;
  bool back_from_video = false;
  {
    std::lock_guard lock(g_pages_mutex);
    back_from_video = g_back_from_video == self;
    g_back_from_video = 0;
  }
  if (back_from_video && Selected(base, self) != opt::kVideoItem)
    VCall(ctx, base, self, fe::kPageSetSelection, opt::kVideoItem, 1);
  if (Selected(base, self) == opt::kVideoItem) ShowVideoItem(ctx, base, self);
}

REX_EXTERN(__imp__sub_824C3248);
REX_HOOK_RAW(sub_824C3248) {
  const uint32_t self = ctx.r3.u32;
  __imp__sub_824C3248(ctx, base);
  if (HasVideoItem(base, self) && Selected(base, self) == opt::kVideoItem) ShowVideoItem(ctx, base, self);
}

REX_EXTERN(__imp__sub_824C3190);
REX_HOOK_RAW(sub_824C3190) {
  const uint32_t self = ctx.r3.u32;
  __imp__sub_824C3190(ctx, base);
  if (HasVideoItem(base, self) && Selected(base, self) == opt::kVideoItem) ShowVideoItem(ctx, base, self);
}
