// cod3 - [waw-decomp/menus2] 2026-11-23 Native menus: the drawer of both looks (see cod3_menukit.h), drawn with the
// overlay's ImGui from what the game thread laid out (cod3_menukit_shown.h). menukit's items are in the menus'
// 640x480 space; here they land where the game's own menus land on screen (the picture, its 16:9 share on a wider
// screen with cod3_ultrawide_hud, as ui/hud_overlay.cpp CurrentHudArea and game/pc_menu.cpp ToMenuSpace), inside the
// player's own view in split screen.
//
// Modern (the owner's 2026-11-16 answer "a modern and left aligned menu", 14:17 "more sleek while still keeping the
// in-game menu's look"; f2-menus' mock-up colors): no solid blocks, a thin gold tick on the chosen page, a faint gold
// tint on the chosen row, hairlines, 2 px sliders with a small knob, CoD3's olive, khaki and gold.
// Original: the game's own layout (menukit's original metrics, measured from the game's Pause > Options), its dark
// panel, olive highlight band and step arrows, in the port's fonts for now (the game's own FONT pages and panel
// pictures, read from guest memory, are the next step: dev notes, native-menus P0 section 4).

#include <algorithm>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <mutex>
#include <string>
#include <vector>

#include <imgui.h>
#include <rex/cvar.h>

#include "game/ultrawide.h"
#include "ui/menus/cod3_menukit.h"
#include "ui/menus/cod3_menukit_shown.h"
#include "ui/theme.h"

REXCVAR_DECLARE(bool, cod3_native_menus);
REXCVAR_DECLARE(bool, cod3_ultrawide_hud);

namespace cod3::nativemenus {
namespace {

constexpr int64_t kShownForNs = 250'000'000;  // a layout older than this is a page the game no longer draws

std::mutex g_mutex;
std::vector<Shown> g_shown;  // g_mutex

int64_t NowNs() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

// The modern look's colors (f2-menus' mock-up, sleek version, 2026-11-16 14:20).
constexpr ImU32 kShade = IM_COL32(12, 13, 8, 196);
constexpr ImU32 kGold = IM_COL32(217, 178, 94, 255);
constexpr ImU32 kGoldLight = IM_COL32(240, 207, 134, 255);
constexpr ImU32 kText = IM_COL32(232, 228, 216, 255);
constexpr ImU32 kDim = IM_COL32(154, 149, 128, 255);
constexpr ImU32 kBand = IM_COL32(217, 178, 94, 23);
constexpr ImU32 kHair = IM_COL32(255, 255, 255, 16);
constexpr ImU32 kTrack = IM_COL32(255, 255, 255, 46);
// The original look's: the game's pause pages (a dark panel, khaki letters, an olive band on the chosen row).
constexpr ImU32 kPanel = IM_COL32(18, 20, 12, 206);
constexpr ImU32 kOliveBand = IM_COL32(104, 112, 66, 176);
constexpr ImU32 kKhaki = IM_COL32(214, 206, 170, 255);
constexpr ImU32 kChosen = IM_COL32(255, 240, 190, 255);
constexpr ImU32 kKhakiDim = IM_COL32(140, 136, 110, 255);
constexpr ImU32 kRuleLine = IM_COL32(214, 206, 170, 90);

// Where the menus' 640x480 space of one player's view lands on the screen.
struct Frame {
  float x = 0, y = 0, sx = 1, sy = 1;  // screen = x + menu_x * sx, y + menu_y * sy
  ImVec2 At(float mx, float my) const { return ImVec2(x + mx * sx, y + my * sy); }
};

Frame FrameFor(const ImGuiIO& io, const float part[4]) {
  float ax = 0.0f, ay = 0.0f, aw = io.DisplaySize.x, ah = io.DisplaySize.y;
  const float picture = std::max(cod3::DisplayAspect(), 0.1f);
  const cod3::PictureRect r =
      cod3::PictureInView(uint32_t(io.DisplaySize.x + 0.5f), uint32_t(io.DisplaySize.y + 0.5f), picture);
  if (r.width > 0 && r.height > 0) {
    ax = float(r.x), ay = float(r.y), aw = float(r.width), ah = float(r.height);
    if (picture > cod3::kSixteenByNine + cod3::kSameShape && REXCVAR_GET(cod3_ultrawide_hud)) {
      const float hud_width = std::floor(aw * cod3::kSixteenByNine / picture);  // the menus' 16:9 share
      ax += std::floor((aw - hud_width) * 0.5f);
      aw = hud_width;
    }
  }
  Frame f;
  f.x = ax + part[0] * aw;
  f.y = ay + part[1] * ah;
  f.sx = part[2] * aw / 640.0f;
  f.sy = part[3] * ah / 480.0f;
  return f;
}

void Text(ImDrawList* dl, float size, ImVec2 at, ImU32 col, const std::string& text, float wrap = 0.0f) {
  if (text.empty() || size < 4.0f) return;
  dl->AddText(cod3::ui::FontFor(size), size, at, col, text.c_str(), nullptr, wrap);
}

float Width(float size, const std::string& text) {
  return cod3::ui::FontFor(size)->CalcTextSizeA(size, FLT_MAX, 0.0f, text.c_str()).x;
}

// Text centered on a box's height, from its left edge (right edge when `right`).
void BoxText(ImDrawList* dl, const Frame& f, const menukit::Box& b, float size, ImU32 col, const std::string& text,
             bool right = false) {
  const ImVec2 a = f.At(b.x0, b.y0), z = f.At(b.x1, b.y1);
  const float y = (a.y + z.y - size) * 0.5f;
  const float x = right ? z.x - Width(size, text) : a.x;
  Text(dl, size, ImVec2(x, y), col, text);
}

// A filled triangle pointing `direction` (-1 left, 1 right, 2 up, 3 down) with its tip at `tip`.
void Triangle(ImDrawList* dl, ImVec2 tip, float size, int direction, ImU32 col) {
  const float h = size * 0.5f;
  const float dx = direction == -1 ? size : direction == 1 ? -size : 0.0f;
  const float dy = direction == 2 ? size : direction == 3 ? -size : 0.0f;
  const ImVec2 a = dx != 0.0f ? ImVec2(tip.x + dx, tip.y - h) : ImVec2(tip.x - h, tip.y + dy);
  const ImVec2 b = dx != 0.0f ? ImVec2(tip.x + dx, tip.y + h) : ImVec2(tip.x + h, tip.y + dy);
  dl->AddTriangleFilled(tip, a, b, col);
}

// A row's letters: the value of the chosen row stands out in the modern look, the whole chosen row in the original.
ImU32 RowColor(bool modern, bool focused, bool locked, bool value) {
  if (locked) return modern ? kDim : kKhakiDim;
  if (modern) return focused && value ? kGoldLight : kText;
  return focused ? kChosen : kKhaki;
}

void DrawPage(ImDrawList* dl, const Frame& f, const Shown& s) {
  const bool modern = s.look == menukit::Look::kModern;
  const float k = f.sy;  // pixels per menu unit, vertically (text sizes follow the height)
  const ImVec2 top_left = f.At(0, 0), bottom_right = f.At(640, 480);
  if (modern)
    dl->AddRectFilled(top_left, bottom_right, kShade);
  else
    dl->AddRectFilled(f.At(100, 76), f.At(548, 420), kPanel);
  for (const menukit::DrawItem& it : s.items) {
    const menukit::Box& b = it.box;
    const float h = b.y1 - b.y0;
    switch (it.what) {
      case menukit::What::kTitle:
        BoxText(dl, f, b, h * 0.78f * k, modern ? kGoldLight : kKhaki, it.text);
        break;
      case menukit::What::kRule:
        dl->AddLine(f.At(b.x0, b.y0), f.At(b.x1, b.y0), modern ? kHair : kRuleLine, std::max(1.0f, 0.5f * k));
        break;
      case menukit::What::kHighlight:
        if (modern) {
          dl->AddRectFilled(f.At(b.x0, b.y0), f.At(b.x1, b.y1), kBand);
          if (it.category) dl->AddRectFilled(f.At(b.x0, b.y0 + 4), f.At(b.x0 + 1.2f, b.y1 - 4), kGold);
        } else {
          dl->AddRectFilled(f.At(b.x0, b.y0), f.At(b.x1, b.y1), kOliveBand);
        }
        break;
      case menukit::What::kCategory:
        if (modern && it.focused) dl->AddRectFilled(f.At(b.x0 - 8, b.y0 + 5), f.At(b.x0 - 6.8f, b.y1 - 5), kGold);
        BoxText(dl, f, b, h * 0.62f * k, modern ? (it.focused ? kGold : kDim) : kKhaki,
                modern ? it.text : "< " + it.text + " >");
        break;
      case menukit::What::kLabel:
        BoxText(dl, f, b, h * 0.6f * k, RowColor(modern, it.focused, it.locked, false), it.text);
        break;
      case menukit::What::kValue:
        BoxText(dl, f, b, h * 0.6f * k, RowColor(modern, it.focused, it.locked, true), it.text, /*right=*/modern);
        break;
      case menukit::What::kGauge: {
        const ImVec2 a = f.At(b.x0, b.y0), z = f.At(b.x1, b.y1);
        const float cy = (a.y + z.y) * 0.5f, kx = a.x + it.fill * (z.x - a.x);
        if (modern) {
          dl->AddRectFilled(ImVec2(a.x, cy - 1.0f), ImVec2(z.x, cy + 1.0f), kTrack);
          dl->AddRectFilled(ImVec2(a.x, cy - 1.0f), ImVec2(kx, cy + 1.0f), kGold);
          dl->AddCircleFilled(ImVec2(kx, cy), std::max(3.0f, 2.6f * k), it.focused ? kGoldLight : kText);
        } else {
          dl->AddRect(a, z, kKhakiDim);
          dl->AddRectFilled(ImVec2(a.x + 1.0f, a.y + 1.0f), ImVec2(std::max(a.x + 1.0f, kx - 1.0f), z.y - 1.0f),
                            it.focused ? kChosen : kKhaki);
        }
        break;
      }
      case menukit::What::kArrows: {
        const ImVec2 a = f.At(b.x0, b.y0), z = f.At(b.x1, b.y1);
        const float cy = (a.y + z.y) * 0.5f, size = (z.y - a.y) * 0.45f;
        const ImU32 col = it.focused ? kChosen : kKhakiDim;
        Triangle(dl, ImVec2(a.x, cy), size, -1, col);
        Triangle(dl, ImVec2(z.x, cy), size, 1, col);
        break;
      }
      case menukit::What::kNote: {
        const ImVec2 a = f.At(b.x0, b.y0), z = f.At(b.x1, b.y1);
        Text(dl, 11.0f * k, a, modern ? kDim : kKhakiDim, it.text, z.x - a.x);
        break;
      }
      case menukit::What::kScroll: {
        const ImVec2 a = f.At(b.x0, b.y0), z = f.At(b.x1, b.y1);
        const bool down = it.fill > 0.5f;
        const ImVec2 tip((a.x + z.x) * 0.5f, down ? z.y : a.y);
        Triangle(dl, tip, 6.0f * k, down ? 3 : 2, modern ? kDim : kKhakiDim);
        break;
      }
    }
  }
  // The buttons (the game thread asked their words in the chosen language).
  Text(dl, 11.0f * k, f.At(modern ? 48.0f : 118.0f, modern ? 446.0f : 400.0f), modern ? kDim : kKhakiDim, s.prompts);
}

}  // namespace

void Publish(Shown shown) {
  std::lock_guard lock(g_mutex);
  for (Shown& s : g_shown) {
    if (s.system == shown.system) {
      s = std::move(shown);
      return;
    }
  }
  g_shown.push_back(std::move(shown));
}

void DrawOverlay() {
  std::vector<Shown> shown;
  {
    std::lock_guard lock(g_mutex);
    if (g_shown.empty()) return;
    const int64_t now = NowNs();
    g_shown.erase(std::remove_if(g_shown.begin(), g_shown.end(),
                                 [now](const Shown& s) { return now - s.ns > kShownForNs; }),
                  g_shown.end());
    if (!REXCVAR_GET(cod3_native_menus)) g_shown.clear();
    shown = g_shown;
  }
  if (shown.empty()) return;
  const ImGuiIO& io = ImGui::GetIO();
  if (io.DisplaySize.x < 64.0f || io.DisplaySize.y < 64.0f) return;
  ImDrawList* dl = ImGui::GetBackgroundDrawList();  // under the overlay's own windows (F2, the console)
  for (const Shown& s : shown) DrawPage(dl, FrameFor(io, s.part), s);
}

}  // namespace cod3::nativemenus
