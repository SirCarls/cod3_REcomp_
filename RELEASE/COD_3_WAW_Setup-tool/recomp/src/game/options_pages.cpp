// cod3 - the table of the Options category pages (see options_pages.h).

#include "game/options_pages.h"

#include <algorithm>
#include <cstring>
#include <iterator>

#include "game/menu_video.h"

namespace cod3::options_pages {
namespace {

namespace vm = cod3::videomenu;

constexpr Row Native(int entry) { return {Source::kNative, int8_t(entry)}; }
constexpr Row Video(vm::Row row) { return {Source::kVideo, int8_t(row)}; }

// --- cod3mp: Pause > OPTIONS ------------------------------------------------
// The page's own entries (its full-screen Init, 0x82500868): 0 STICK LAYOUT,
// 1 BUTTON LAYOUT, 2 HORIZONTAL SENSITIVITY, 3 VERTICAL SENSITIVITY (both
// gauges), 4 INVERT AIM, 5 TOGGLE ADS, 6 STICKY AIM, 7 ALTERNATE TANK
// CONTROLS, 8 VIBRATION, 9 RESTORE DEFAULTS. Multiplayer has no battle
// actions, and its movement is the match's rule (F2 > Game > Server), so it
// has no MOVEMENT page; its in-match page has no sound rows yet, so no AUDIO.
constexpr Row kMpGameplay[] = {{Source::kFov, 0}, {Source::kFov, 1}};
constexpr Row kMpControls[] = {Native(0), Native(1), Native(2), Native(3),
                               Native(4), Native(5), Native(7), Native(8)};
constexpr Row kMpAim[] = {Native(6)};
constexpr Row kMpVideo[] = {Video(vm::kResolution), Video(vm::kFrameRate),   Video(vm::kVsync),
                            Video(vm::kAntiAliasing), Video(vm::kShadows),  Video(vm::kHdTextures),
                            Video(vm::kDepthOfField), Video(vm::kMotionBlur)};

constexpr const char* kGameplayKey = "COD3_OPTIONS_GAMEPLAY";
constexpr const char* kControlsKey = "COD3_OPTIONS_CONTROLS";
constexpr const char* kAimKey = "COD3_OPTIONS_AIM";
constexpr const char* kVideoKey = "COD3_OPTIONS_VIDEO";
constexpr const char* kGameplayTitleKey = "COD3_OPTIONS_GAMEPLAY_TITLE";
constexpr const char* kControlsTitleKey = "COD3_OPTIONS_CONTROLS_TITLE";
constexpr const char* kAimTitleKey = "COD3_OPTIONS_AIM_TITLE";
constexpr const char* kVideoTitleKey = "COD3_OPTIONS_VIDEO_TITLE";

template <size_t N>
constexpr Page MakePage(Category c, const char* button, const char* title, const Row (&rows)[N]) {
  return {c, button, title, rows, int(N)};
}

constexpr Page kMpPause[] = {
    MakePage(Category::kGameplay, kGameplayKey, kGameplayTitleKey, kMpGameplay),
    MakePage(Category::kControls, kControlsKey, kControlsTitleKey, kMpControls),
    MakePage(Category::kAim, kAimKey, kAimTitleKey, kMpAim),
    MakePage(Category::kVideo, kVideoKey, kVideoTitleKey, kMpVideo),
};

struct Text {
  const char* key;
  const char* text;
};
constexpr Text kTexts[] = {
    {kGameplayKey, "GAMEPLAY"},
    {kControlsKey, "CONTROLS"},
    {kAimKey, "AIM"},
    {kVideoKey, "VIDEO"},
    {kGameplayTitleKey, "OPTIONS / GAMEPLAY"},
    {kControlsTitleKey, "OPTIONS / CONTROLS"},
    {kAimTitleKey, "OPTIONS / AIM"},
    {kVideoTitleKey, "OPTIONS / VIDEO"},
};

struct Pages {
  const Page* pages;
  int count;
};

Pages PagesOf(Menu menu) {
  switch (menu) {
    case Menu::kMultiplayerPause:
      return {kMpPause, int(std::size(kMpPause))};
  }
  return {nullptr, 0};
}

}  // namespace

int PageCount(Menu menu) { return PagesOf(menu).count; }

const Page* PageAt(Menu menu, int i) {
  const Pages p = PagesOf(menu);
  return i >= 0 && i < p.count ? &p.pages[i] : nullptr;
}

int MostRows(Menu menu) {
  const Pages p = PagesOf(menu);
  int most = 0;
  for (int i = 0; i < p.count; ++i) most = std::max(most, p.pages[i].count);
  return most;
}

const char* Text(const char* key) {
  if (!key) return nullptr;
  for (const auto& t : kTexts)
    if (std::strcmp(key, t.key) == 0) return t.text;
  return nullptr;
}

}  // namespace cod3::options_pages
