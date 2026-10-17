// cod3 - the spinner for stick battle actions (see qte_spinner.h).

#include "ui/qte_spinner.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <string>

#include <rex/cvar.h>

#include "game/qte_actions.h"
#include "game/qte_math.h"
#include "ui/hud_overlay.h"
#include "ui/theme.h"

REXCVAR_DECLARE(bool, cod3_qte_spinner_with_pad);
REXCVAR_DECLARE(double, cod3_qte_spinner_scale);

namespace cod3::ui {
namespace {

namespace q = cod3::qte;
namespace m = cod3::qte::math;

// Parchment white while nothing is decided, green and red that read on
// bright and dark scenes alike, the game's brass gold for the catch zone.
constexpr ImVec4 kNeutral{0.95f, 0.90f, 0.76f, 1.0f};
constexpr ImVec4 kGood{0.36f, 0.90f, 0.42f, 1.0f};
constexpr ImVec4 kFail{0.95f, 0.26f, 0.21f, 1.0f};
constexpr ImVec4 kGuide{1.0f, 1.0f, 1.0f, 1.0f};
constexpr ImVec4 kGold{0.84f, 0.67f, 0.41f, 1.0f};

ImU32 Col(const ImVec4& c, float alpha) {
  return ImGui::ColorConvertFloat4ToU32(ImVec4(c.x, c.y, c.z, std::clamp(c.w * alpha, 0.0f, 1.0f)));
}

ImVec4 Mix(const ImVec4& a, const ImVec4& b, float t) {
  return ImVec4(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t);
}

// The last positions of a dot, oldest first (a fixed ring, no allocation).
class Trail {
 public:
  void Add(ImVec2 p, float now) {
    points_[next_] = {p, now};
    next_ = (next_ + 1) % kCount;
    count_ = std::min(count_ + 1, kCount);
  }
  void Clear() { count_ = 0; }
  // Lines from each point to the next, fading out over `life` seconds.
  void Draw(ImDrawList* dl, float now, float life, const ImVec4& color, float alpha, float thickness) const {
    for (int i = 1; i < count_; ++i) {
      const Point& a = At(i - 1);
      const Point& b = At(i);
      const float fresh = 1.0f - (now - b.time) / life;
      if (fresh <= 0.0f) continue;
      dl->AddLine(a.pos, b.pos, Col(color, alpha * fresh), std::max(1.0f, thickness * (0.3f + 0.7f * fresh)));
    }
  }

 private:
  struct Point {
    ImVec2 pos;
    float time;
  };
  static constexpr int kCount = 48;
  const Point& At(int i) const { return points_[(next_ - count_ + i + 2 * kCount) % kCount]; }
  Point points_[kCount] = {};
  int next_ = 0;
  int count_ = 0;
};

// What the spinner remembers between frames (UI thread).
struct SpinnerState {
  uint32_t serial = 0;
  float time = 0.0f;        // seconds since this action's spinner appeared
  float guide = 0.0f;       // the guide dot: swirl angle (radians) or cycle time
  ImVec4 color = kNeutral;  // eased toward the verdict's color
  uint32_t events = 0;      // flips or strokes seen
  float flash = 0.0f;       // seconds of flash left after one
  Trail guide_trail;
  Trail player_trail;
};
SpinnerState g_state;

ImVec2 OnCircle(ImVec2 c, float radius, float angle) {
  // Screen angles: 0 = 3 o'clock, growing clockwise (y points down).
  return ImVec2(c.x + std::cos(angle) * radius, c.y + std::sin(angle) * radius);
}

void Arc(ImDrawList* dl, ImVec2 c, float radius, float from, float to, ImU32 col, float thickness) {
  const int segments = std::clamp(int(std::fabs(to - from) * 16.0f), 4, 96);
  dl->PathArcTo(c, radius, from, to, segments);
  dl->PathStroke(col, 0, thickness);
}

// Dots along an arc (the way back, shown faint).
void DottedArc(ImDrawList* dl, ImVec2 c, float radius, float from, float to, int dots, ImU32 col, float size) {
  for (int i = 0; i <= dots; ++i)
    dl->AddCircleFilled(OnCircle(c, radius, from + (to - from) * float(i) / float(dots)), size, col, 8);
}

// The guide dot with a fading arc behind it.
void GuideDotOnRing(ImDrawList* dl, ImVec2 c, float radius, float angle, float direction, float alpha, float px) {
  constexpr int kSegments = 18;
  constexpr float kStep = 0.07f;  // radians a segment
  for (int i = 0; i < kSegments; ++i) {
    const float fresh = 1.0f - float(i) / float(kSegments);
    const ImVec2 a = OnCircle(c, radius, angle - direction * kStep * float(i));
    const ImVec2 b = OnCircle(c, radius, angle - direction * kStep * float(i + 1));
    dl->AddLine(a, b, Col(kGuide, alpha * 0.6f * fresh), std::max(1.0f, 9.0f * px * fresh));
  }
  dl->AddCircleFilled(OnCircle(c, radius, angle), 11.0f * px, Col(kGuide, alpha), 24);
}

// Small arrowheads around the ring pointing the way to turn.
void Chevrons(ImDrawList* dl, ImVec2 c, float radius, float direction, float spin, ImU32 col, float px) {
  for (int k = 0; k < 4; ++k) {
    const float a = spin + float(k) * m::kPi * 0.5f + m::kPi * 0.25f;
    const float r = radius * 1.12f;
    const ImVec2 tip = OnCircle(c, r, a + direction * 0.07f);
    const ImVec2 back_in = OnCircle(c, r - 7.0f * px, a - direction * 0.03f);
    const ImVec2 back_out = OnCircle(c, r + 7.0f * px, a - direction * 0.03f);
    dl->AddTriangleFilled(tip, back_in, back_out, col);
  }
}

// The progress meter: an arc inside the ring from 12 o'clock, clockwise.
void Meter(ImDrawList* dl, ImVec2 c, float radius, float quality, const ImVec4& color, float alpha, float px) {
  dl->AddCircle(c, radius * 0.84f, Col(kNeutral, alpha * 0.15f), 96, 6.0f * px);
  if (quality > 0.01f)
    Arc(dl, c, radius * 0.84f, -m::kPi * 0.5f, -m::kPi * 0.5f + m::kTwoPi * std::min(quality, 1.0f),
        Col(color, alpha * 0.9f), 6.0f * px);
}

void TextCentered(ImDrawList* dl, float size, ImVec2 center_top, ImU32 col, float alpha, const char* text) {
  ImFont* font = FontFor(size);
  const ImVec2 ext = font->CalcTextSizeA(size, FLT_MAX, 0.0f, text);
  const ImVec2 pos(std::floor(center_top.x - ext.x * 0.5f), std::floor(center_top.y));
  const float shadow = std::max(1.0f, size / 14.0f);
  dl->AddText(font, size, ImVec2(pos.x + shadow, pos.y + shadow), IM_COL32(0, 0, 0, int(190 * alpha)), text);
  dl->AddText(font, size, pos, col, text);
}

// What to do, for the device in use (no allocation: a stack buffer).
void Instruction(const q::Snapshot& s, bool mouse, char* out, size_t size) {
  const char* stick = s.left_stick ? "left" : "right";
  switch (s.kind) {
    case q::Kind::kSwirl:
      if (mouse)
        std::snprintf(out, size, "Circle the mouse %s", s.clockwise ? "clockwise" : "counterclockwise");
      else
        std::snprintf(out, size, "Rotate the %s stick %s", stick, s.clockwise ? "clockwise" : "counterclockwise");
      return;
    case q::Kind::kToggleHorizontal:
      if (mouse)
        std::snprintf(out, size, "Move the mouse left and right");
      else
        std::snprintf(out, size, "Move the %s stick left and right", stick);
      return;
    case q::Kind::kToggleVertical:
      if (mouse)
        std::snprintf(out, size, "Move the mouse up and down");
      else
        std::snprintf(out, size, "Move the %s stick up and down", stick);
      return;
    case q::Kind::kRow:
      if (mouse)
        std::snprintf(out, size, "Circle the mouse clockwise to row");
      else
        std::snprintf(out, size, "Push the %s stick up, sweep it down the right side", stick);
      return;
    case q::Kind::kNone:
      break;
  }
  out[0] = '\0';
}

void DrawSwirl(ImDrawList* dl, const q::Snapshot& s, SpinnerState& st, ImVec2 c, float radius, float dt,
               float alpha, float px) {
  const float direction = s.clockwise ? 1.0f : -1.0f;
  Meter(dl, c, radius, s.quality, st.color, alpha, px);
  dl->AddCircle(c, radius, Col(st.color, alpha * 0.9f), 96, 4.0f * px);
  Chevrons(dl, c, radius, direction, direction * st.time * 0.6f, Col(st.color, alpha * 0.7f), px);
  if (s.ready) st.guide += direction * s.guide_speed * dt;
  const float pulse = s.ready ? 1.0f : 0.55f + 0.45f * std::sin(st.time * 6.0f);
  GuideDotOnRing(dl, c, radius, -m::kPi * 0.5f + st.guide, direction, alpha * pulse, px);
}

void DrawToggle(ImDrawList* dl, const q::Snapshot& s, SpinnerState& st, ImVec2 c, float radius, float dt,
                float alpha, float px, float player_t) {
  const bool vertical = s.kind == q::Kind::kToggleVertical;
  auto along = [&](float t) { return vertical ? ImVec2(c.x, c.y - t * radius) : ImVec2(c.x + t * radius, c.y); };
  Meter(dl, c, radius, s.quality, st.color, alpha, px);
  dl->AddCircle(c, radius, Col(st.color, alpha * 0.3f), 96, 2.0f * px);
  dl->AddLine(along(-1.0f), along(1.0f), Col(st.color, alpha * 0.55f), 4.0f * px);
  // The two ends: past them a flip counts (the game's 70 of 128).
  for (float side : {-1.0f, 1.0f}) {
    const bool in = player_t * side > m::kToggleFlipRaw;
    const float lit = in ? 1.0f : 0.55f + st.flash * 1.5f;
    dl->AddLine(along(side * m::kToggleFlipRaw), along(side * 1.05f), Col(in ? st.color : kNeutral, alpha * lit),
                14.0f * px);
  }
  // The guide sweeps end to end at the guide's flips a second.
  if (s.ready) st.guide += dt;
  const float t = 0.95f * std::sin(m::kPi * s.guide_speed * st.guide);
  const ImVec2 dot = along(t);
  st.guide_trail.Add(dot, st.time);
  st.guide_trail.Draw(dl, st.time, 0.25f, kGuide, alpha * 0.6f, 9.0f * px);
  const float pulse = s.ready ? 1.0f : 0.55f + 0.45f * std::sin(st.time * 6.0f);
  dl->AddCircleFilled(dot, 11.0f * px, Col(kGuide, alpha * pulse), 24);
}

void DrawRow(ImDrawList* dl, const q::Snapshot& s, SpinnerState& st, ImVec2 c, float radius, float dt,
             float alpha, float px, bool mouse) {
  const float top = -m::kPi * 0.5f, bottom = m::kPi * 0.5f;
  dl->AddCircle(c, radius, Col(st.color, alpha * 0.2f), 96, 2.0f * px);
  // The stroke: 12 to 6 o'clock down the right side.
  Arc(dl, c, radius, top, bottom, Col(st.color, alpha * std::min(1.0f, 0.75f + st.flash * 2.0f)), 5.0f * px);
  // The way back: the left half with the mouse, through the middle with a
  // stick (a stick pushed left in a stroke slips).
  const ImU32 faint = Col(kNeutral, alpha * 0.35f);
  if (mouse) {
    DottedArc(dl, c, radius, bottom, top + m::kTwoPi, 14, faint, 2.5f * px);
  } else {
    for (int i = 0; i <= 10; ++i)
      dl->AddCircleFilled(ImVec2(c.x, c.y + radius - 2.0f * radius * float(i) / 10.0f), 2.5f * px, faint, 8);
  }
  // The catch zone at the top.
  Arc(dl, c, radius, top - 0.22f, top + 0.22f, Col(kGold, alpha), 12.0f * px);
  // The stroke so far (the game's phase).
  if (s.row_phase >= m::kRowStrokeStart) {
    const float done = (s.row_phase - m::kRowStrokeStart) / (1.0f - m::kRowStrokeStart);
    Arc(dl, c, radius, top, top + m::kPi * std::min(done, 1.0f), Col(st.color, alpha), 10.0f * px);
  }
  // The guide: the stroke, then the way back, over and over.
  if (s.ready) st.guide += dt * std::clamp(s.guide_speed, 0.05f, 2.0f);
  const float cycle = std::fmod(st.guide, m::kRowGuideStroke + m::kRowGuideReturn);
  const float pulse = s.ready ? 1.0f : 0.55f + 0.45f * std::sin(st.time * 6.0f);
  if (cycle < m::kRowGuideStroke) {
    GuideDotOnRing(dl, c, radius, top + m::kPi * cycle / m::kRowGuideStroke, 1.0f, alpha * pulse, px);
  } else {
    const float u = (cycle - m::kRowGuideStroke) / m::kRowGuideReturn;
    const ImVec2 dot = mouse ? OnCircle(c, radius, bottom + m::kPi * u)
                             : ImVec2(c.x, c.y + radius - 2.0f * radius * u);
    dl->AddCircleFilled(dot, 8.0f * px, Col(kGuide, alpha * 0.45f * pulse), 20);
  }
}

}  // namespace

void DrawQteSpinner(ImGuiIO& io) {
  const q::Snapshot s = q::CurrentSnapshot();
  SpinnerState& st = g_state;
  if (!s.active || s.kind == q::Kind::kNone) {
    st.serial = 0;
    return;
  }
  if (s.serial != st.serial) {
    st = SpinnerState{};
    st.serial = s.serial;
    st.events = s.events;
  }
  const float dt = std::clamp(io.DeltaTime, 0.0f, 0.1f);
  st.time += dt;
  const HudArea area = CurrentHudArea(io);
  if (area.width <= 1.0f || area.height <= 1.0f) return;
  const float px = std::max(0.5f, area.height / 1080.0f);
  const float size = float(std::clamp(REXCVAR_GET(cod3_qte_spinner_scale), 0.5, 2.0));
  const float radius = std::floor(area.height * 0.15f * size);
  const ImVec2 c(std::floor(area.x + area.width * 0.5f), std::floor(area.y + area.height * 0.5f));
  q::NoteDrawnRadius(radius);
  ImDrawList* dl = ImGui::GetForegroundDrawList();
  const float alpha = std::min(1.0f, st.time / 0.2f);  // fades in

  float cx = 0.0f, cy = 0.0f;
  const bool have_cursor = q::CursorPosition(&cx, &cy);
  const bool mouse = s.device == q::Device::kMouse && have_cursor;

  if (!q::SpinnerShown()) {
    // The game's own arrow shows. With the mouse a small ring still shows
    // where the cursor is (it works by position).
    if (mouse) {
      const float r = radius * 0.35f;
      const ImVec2 rc(c.x, std::floor(c.y + area.height * 0.2f));
      dl->AddCircle(rc, r, Col(kNeutral, 0.5f * alpha), 48, 2.0f * px);
      dl->AddCircleFilled(ImVec2(rc.x + cx * r, rc.y + cy * r), 5.0f * px, Col(kGuide, 0.85f * alpha), 16);
    }
    return;
  }

  const ImVec4 target = s.verdict == q::Verdict::kGood ? kGood : s.verdict == q::Verdict::kFail ? kFail : kNeutral;
  st.color = Mix(st.color, target, std::min(1.0f, dt / 0.12f));
  if (s.events != st.events) {
    st.events = s.events;
    st.flash = 0.25f;
  }
  st.flash = std::max(0.0f, st.flash - dt);

  // Where the player is: the cursor (mouse) or the stick as pushed (on a
  // toggle, along its line).
  float px_t = mouse ? cx : s.pad_x;       // -1..1 along a horizontal line
  float py_t = mouse ? -cy : s.pad_y_up;   // up positive
  if (s.kind == q::Kind::kToggleHorizontal) py_t = 0.0f;
  if (s.kind == q::Kind::kToggleVertical) px_t = 0.0f;
  const ImVec2 player(c.x + px_t * radius, c.y - py_t * radius);

  dl->AddCircleFilled(c, radius * 1.24f, IM_COL32(0, 0, 0, int(60 * alpha)), 64);
  switch (s.kind) {
    case q::Kind::kSwirl:
      DrawSwirl(dl, s, st, c, radius, dt, alpha, px);
      break;
    case q::Kind::kToggleHorizontal:
    case q::Kind::kToggleVertical:
      DrawToggle(dl, s, st, c, radius, dt, alpha, px, s.kind == q::Kind::kToggleVertical ? py_t : px_t);
      break;
    case q::Kind::kRow:
      DrawRow(dl, s, st, c, radius, dt, alpha, px, mouse);
      break;
    case q::Kind::kNone:
      break;
  }
  // The player's dot and its trail.
  st.player_trail.Add(player, st.time);
  st.player_trail.Draw(dl, st.time, 0.3f, st.color, alpha * 0.8f, 7.0f * px);
  dl->AddCircleFilled(player, 8.0f * px, Col(st.color, alpha), 24);
  dl->AddCircle(player, 8.0f * px, IM_COL32(0, 0, 0, int(200 * alpha)), 24, 1.5f * px);

  // What to do, and why it failed.
  char line[96];
  Instruction(s, mouse, line, sizeof(line));
  const float text_size = std::floor(32.0f * px * std::max(0.8f, size));
  float y = c.y + radius * 1.32f;
  TextCentered(dl, text_size, ImVec2(c.x, y), Col(kNeutral, alpha), alpha, line);
  y += text_size * 1.15f;
  if (!s.ready) {
    TextCentered(dl, text_size, ImVec2(c.x, y), Col(kGold, alpha), alpha, "Get ready");
  } else if (s.verdict == q::Verdict::kFail && s.reason && *s.reason) {
    TextCentered(dl, text_size, ImVec2(c.x, y), Col(kFail, alpha), alpha, s.reason);
  }
  if (s.demo) TextCentered(dl, text_size * 0.7f, ImVec2(c.x, c.y - radius * 1.32f - text_size), Col(kGold, alpha * 0.8f), alpha, "BATTLE ACTION TEST (cod3_qte_demo)");
}

// ---------------------------------------------------------------------------
// F2 > Controls
// ---------------------------------------------------------------------------
namespace {
// A short, plain description under an option (like the rest of F2).
void Help(const char* text) {
  ImGui::PushTextWrapPos(0.0f);
  ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
  ImGui::TextUnformatted(text);
  ImGui::PopStyleColor();
  ImGui::PopTextWrapPos();
  ImGui::Spacing();
}
}  // namespace

bool DrawQteSettings() {
  bool changed = false;
  Section("Battle actions");
  int style = rex::cvar::GetFlagByName("cod3_qte_style") == "original" ? 1 : 0;
  const char* styles[] = {"Spinner", "Original (the game's arrow)"};
  if (ImGui::Combo("Stick actions", &style, styles, 2)) {
    rex::cvar::SetFlagByName("cod3_qte_style", style == 1 ? "original" : "spinner");
    changed = true;
  }
  Help("How rotate, toggle and row prompts (valves, cranes, crowbars, rowing) look; the mouse works with both.");
  if (style == 0) {
    bool with_pad = REXCVAR_GET(cod3_qte_spinner_with_pad);
    if (ImGui::Checkbox("Spinner with a controller", &with_pad)) {
      REXCVAR_SET(cod3_qte_spinner_with_pad, with_pad);
      changed = true;
    }
    Help("Off: the game's arrow while you play with a controller.");
    float scale = float(REXCVAR_GET(cod3_qte_spinner_scale));
    if (ImGui::SliderFloat("Spinner size", &scale, 0.5f, 2.0f, "%.2f")) {
      REXCVAR_SET(cod3_qte_spinner_scale, double(std::clamp(scale, 0.5f, 2.0f)));
      changed = true;
    }
    Help("How big the spinner is.");
  }
  return changed;
}

}  // namespace cod3::ui
