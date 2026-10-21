// cod3 - number boxes for the slider rows of the game's menus (see
// menu_slider.h).

#include "game/menu_slider.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <mutex>
#include <vector>

#include <fmt/format.h>

#include <rex/logging.h>
#include <rex/ui/virtual_key.h>

namespace cod3::menuslider {
namespace {

using Clock = std::chrono::steady_clock;
using rex::ui::VirtualKey;

// A row offered longer ago than this is no longer on screen (the page closed
// or a loading screen came): keys go back to the menu.
constexpr auto kOfferFresh = std::chrono::milliseconds(500);
// A click on a box highlights its row first; typing waits this long for it.
constexpr auto kSelectWait = std::chrono::milliseconds(1500);
// The caret blinks: shown, then not, this long each.
constexpr auto kCaretBlink = std::chrono::milliseconds(500);
// Digits a box takes (with a sign and a period).
constexpr size_t kMaxChars = 7;

struct Row {
  uint32_t page = 0;
  int item = -1;
};
bool Same(const Row& a, const Row& b) { return a.item >= 0 && a.page == b.page && a.item == b.item; }

struct Box {
  Row row;
  float x0, y0, x1, y1;
  Clock::time_point when;
};

struct State {
  // The slider row highlighted now (Offer).
  Row offered;
  Number offered_number;
  Clock::time_point offered_at{};
  // Typing.
  bool typing = false;
  Row typed_row;
  Number typed_number;
  std::string chars;
  Clock::time_point typing_since{};
  bool waiting_for_select = false;  // a click started it; the row isn't highlighted yet
  // Confirmed with Enter, for TakeTyped.
  bool ready = false;
  Row ready_row;
  double ready_value = 0.0;
  // The boxes drawn in the last frames.
  std::vector<Box> boxes;
};
std::mutex g_mutex;
State g_state;  // g_mutex

void StopTyping(State& s, const char* why) {
  if (!s.typing) return;
  s.typing = false;
  s.waiting_for_select = false;
  REXLOG_INFO("cod3: menu number box: typing in item {} dropped ({})", s.typed_row.item, why);
  s.chars.clear();
}

void StartTyping(State& s, const Row& row, const Number& n, bool from_click) {
  s.typing = true;
  s.typed_row = row;
  s.typed_number = n;
  s.chars.clear();
  s.typing_since = Clock::now();
  s.waiting_for_select = from_click;
  REXLOG_INFO("cod3: menu number box: typing in item {} ({})", row.item, from_click ? "clicked" : "a key");
}

bool OfferFresh(const State& s) { return s.offered.item >= 0 && Clock::now() - s.offered_at < kOfferFresh; }

// What the typed characters make: within the range, on a step. False when
// they make no number.
bool Parse(const std::string& chars, const Number& n, double* out) {
  if (chars.empty() || chars == "-" || chars == "." || chars == "-.") return false;
  char* end = nullptr;
  const double shown = std::strtod(chars.c_str(), &end);
  if (end == chars.c_str() || !std::isfinite(shown)) return false;
  const double per = n.shown_per_unit != 0.0 ? n.shown_per_unit : 1.0;
  double v = std::clamp(shown / per, n.min, n.max);
  if (n.step > 0.0) v = n.min + std::round((v - n.min) / n.step) * n.step;
  *out = std::clamp(v, n.min, n.max);
  return true;
}

// The character a key types, or 0.
char KeyChar(uint16_t key) {
  const auto k = static_cast<VirtualKey>(key);
  if (key >= uint16_t(VirtualKey::k0) && key <= uint16_t(VirtualKey::k9)) return char('0' + (key - uint16_t(VirtualKey::k0)));
  if (key >= uint16_t(VirtualKey::kNumpad0) && key <= uint16_t(VirtualKey::kNumpad9))
    return char('0' + (key - uint16_t(VirtualKey::kNumpad0)));
  if (k == VirtualKey::kOemPeriod || k == VirtualKey::kDecimal) return '.';
  if (k == VirtualKey::kOemMinus || k == VirtualKey::kSubtract) return '-';
  return 0;
}

// Whether `c` can go after the characters typed so far.
bool Fits(const std::string& chars, char c, const Number& n) {
  if (chars.size() >= kMaxChars) return false;
  if (c == '-') return chars.empty() && n.min < 0.0;
  if (c == '.') return n.decimals > 0 && chars.find('.') == std::string::npos;
  return true;
}

}  // namespace

std::string Format(double value, const Number& n) {
  const int decimals = std::clamp(n.decimals, 0, 4);
  double shown = std::isfinite(value) ? value * n.shown_per_unit : 0.0;
  if (std::abs(shown) < 0.5 * std::pow(10.0, -decimals)) shown = 0.0;  // no "-0"
  return fmt::format("{:.{}f}{}", shown, decimals, n.suffix);
}

double Fraction(double value, const Number& n) {
  if (!(n.max > n.min) || !std::isfinite(value)) return 0.0;
  return std::clamp((value - n.min) / (n.max - n.min), 0.0, 1.0);
}

void Offer(uint32_t page, int item, const Number& n) {
  std::lock_guard lock(g_mutex);
  State& s = g_state;
  const Row row{page, item};
  s.offered = row;
  s.offered_number = n;
  s.offered_at = Clock::now();
  if (!s.typing) return;
  if (Same(row, s.typed_row)) {
    s.waiting_for_select = false;
    s.typed_number = n;  // a range may follow another setting (FOV and FOV SCALE)
    return;
  }
  // A click on a box: its row is being highlighted.
  if (s.waiting_for_select && s.typed_row.page == page && Clock::now() - s.typing_since < kSelectWait) return;
  StopTyping(s, item < 0 ? "not a slider row" : "another row");
}

std::string BoxText(uint32_t page, int item, double value, const Number& n) {
  std::lock_guard lock(g_mutex);
  const State& s = g_state;
  if (!s.typing || !Same({page, item}, s.typed_row)) return Format(value, n);
  const auto since = Clock::now() - s.typing_since;
  const bool caret = (since / kCaretBlink) % 2 == 0;
  return s.chars + (caret ? "_" : " ");
}

bool Typing(uint32_t page, int item) {
  std::lock_guard lock(g_mutex);
  return g_state.typing && Same({page, item}, g_state.typed_row);
}

bool TakeTyped(uint32_t page, int item, double* value) {
  std::lock_guard lock(g_mutex);
  State& s = g_state;
  if (!s.ready || !Same({page, item}, s.ready_row)) return false;
  s.ready = false;
  *value = s.ready_value;
  return true;
}

void NoteBox(uint32_t page, int item, float x0, float y0, float x1, float y1) {
  std::lock_guard lock(g_mutex);
  State& s = g_state;
  const auto now = Clock::now();
  std::erase_if(s.boxes, [&](const Box& b) {
    return (b.row.page == page && b.row.item == item) || now - b.when > kOfferFresh;
  });
  if (s.boxes.size() < 64) s.boxes.push_back(Box{{page, item}, x0, y0, x1, y1, now});
}

int ClickAt(uint32_t page, float x, float y) {
  std::lock_guard lock(g_mutex);
  State& s = g_state;
  const auto now = Clock::now();
  constexpr float kMargin = 2.0f;  // menu units a click may miss the box by
  for (const Box& b : s.boxes) {
    if (b.row.page != page || now - b.when > kOfferFresh) continue;
    if (x < b.x0 - kMargin || x > b.x1 + kMargin || y < b.y0 - kMargin || y > b.y1 + kMargin) continue;
    if (s.typing && Same(b.row, s.typed_row)) return b.row.item;  // typing there already
    StopTyping(s, "another box clicked");
    // The row's number (its range) comes with the next Offer once it is highlighted.
    StartTyping(s, b.row, Same(b.row, s.offered) ? s.offered_number : Number{}, /*from_click=*/true);
    return b.row.item;
  }
  StopTyping(s, "a click elsewhere");
  return -1;
}

bool OnKey(uint16_t virtual_key, bool shift) {
  std::lock_guard lock(g_mutex);
  State& s = g_state;
  const auto key = static_cast<VirtualKey>(virtual_key);
  if (s.typing && !OfferFresh(s) && !s.waiting_for_select) StopTyping(s, "the menu closed");

  if (const char c = KeyChar(virtual_key)) {
    if (shift) return s.typing;  // %, _ ...: not a number's; swallowed while typing
    if (!s.typing) {
      if (!OfferFresh(s) || !Fits("", c, s.offered_number)) return false;
      StartTyping(s, s.offered, s.offered_number, /*from_click=*/false);
    }
    if (Fits(s.chars, c, s.typed_number)) s.chars.push_back(c);
    return true;
  }
  if (!s.typing) return false;
  switch (key) {
    case VirtualKey::kBack:
      if (s.chars.empty()) StopTyping(s, "Backspace");
      else s.chars.pop_back();
      return true;
    case VirtualKey::kDelete:
      s.chars.clear();
      return true;
    case VirtualKey::kEscape:
      StopTyping(s, "Esc");
      return true;
    case VirtualKey::kReturn: {
      double v = 0.0;
      if (Parse(s.chars, s.typed_number, &v)) {
        s.ready = true;
        s.ready_row = s.typed_row;
        s.ready_value = v;
        REXLOG_INFO("cod3: menu number box: \"{}\" typed in item {} = {}", s.chars, s.typed_row.item,
                    Format(v, s.typed_number));
        s.typing = false;
        s.waiting_for_select = false;
        s.chars.clear();
      } else {
        StopTyping(s, "Enter without a number");
      }
      return true;
    }
    case VirtualKey::kUp:
    case VirtualKey::kDown:
    case VirtualKey::kLeft:
    case VirtualKey::kRight:
    case VirtualKey::kTab:
      // The menu moves or steps the value as usual; what was typed is dropped.
      StopTyping(s, "an arrow key");
      return false;
    default:
      return false;
  }
}

}  // namespace cod3::menuslider
