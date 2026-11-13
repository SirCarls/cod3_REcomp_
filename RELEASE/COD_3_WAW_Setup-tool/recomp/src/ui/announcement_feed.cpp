// cod3 - the HUD announcement column (see announcement_feed.h).

#include "ui/announcement_feed.h"

#include <algorithm>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <deque>
#include <mutex>
#include <utility>

#include <imgui.h>

#include "game/text_language.h"  // [acct2-ocr] 2026-11-07 NoticeText
#include "ui/hud_overlay.h"
#include "ui/text_overlay.h"  // [acct2-ocr] 2026-11-07 the text layer's font
#include "ui/theme.h"

namespace cod3::hud {

namespace {

using Clock = std::chrono::steady_clock;

constexpr size_t kMaxLines = 6;          // shown at once, newest at the bottom
constexpr size_t kMaxTextChars = 120;    // longer lines are cut (one HUD line each)
constexpr double kHoldSeconds = 7.0;     // fully visible
constexpr double kFadeSeconds = 1.5;     // then fading out

struct Line {
  std::string text;
  Clock::time_point since;
};

std::mutex g_mutex;
std::deque<Line> g_lines;

}  // namespace

void AnnouncementFeed::Push(std::string text) {
  // [acct2-ocr] 2026-11-07 (owner 17:48 "every way a text string is shown"): the port's own notices in the chosen
  // language (game/text_language.h NoticeText, the packs' "@notices" rows); any other line as written.
  if (std::string ours = cod3::text::NoticeText(text); !ours.empty()) text = std::move(ours);
  // One line of printable text: control characters become spaces.
  for (char& c : text)
    if (static_cast<unsigned char>(c) < 0x20) c = ' ';
  // [acct2-ocr] 2026-11-07 at most kMaxTextChars characters, cut before a character, never inside one (a notice in
  // Russian or Chinese, or another player's line, is UTF-8: a cut by bytes left half a character at the end).
  size_t chars = 0, at = 0;
  for (; at < text.size(); ++at)
    if ((static_cast<unsigned char>(text[at]) & 0xC0) != 0x80 && chars++ == kMaxTextChars) break;
  text.resize(at);
  if (text.find_first_not_of(' ') == std::string::npos) return;
  std::lock_guard lock(g_mutex);
  g_lines.push_back({std::move(text), Clock::now()});
  while (g_lines.size() > kMaxLines) g_lines.pop_front();
}

void AnnouncementFeed::Clear() {
  std::lock_guard lock(g_mutex);
  g_lines.clear();
}

void AnnouncementFeed::Draw(ImGuiIO& io) {
  std::deque<Line> lines;
  {
    std::lock_guard lock(g_mutex);
    const auto now = Clock::now();
    while (!g_lines.empty() &&
           std::chrono::duration<double>(now - g_lines.front().since).count() > kHoldSeconds + kFadeSeconds)
      g_lines.pop_front();
    lines = g_lines;
  }
  if (lines.empty()) return;

  const ui::HudArea area = ui::CurrentHudArea(io);
  const float scale = std::max(0.6f, area.height / 1080.0f);
  const float size = 22.0f * scale;
  ImFont* hud_font = ui::FontFor(size);
  // Right column, under the frame rate counter / frame cost readout (top
  // right, hud_overlay.cpp), like the game's own feed: one line each,
  // right-aligned.
  const float margin = std::floor(14.0f * scale);
  const float right = area.x + area.width - margin;
  float y = area.y + std::floor(150.0f * scale);
  const ImVec4 gold = ui::CurrentPalette().accent_text;
  const auto now = Clock::now();
  ImDrawList* dl = ImGui::GetForegroundDrawList();
  for (const Line& line : lines) {
    const double age = std::chrono::duration<double>(now - line.since).count();
    const float alpha = age <= kHoldSeconds
                            ? 1.0f
                            : float(std::clamp(1.0 - (age - kHoldSeconds) / kFadeSeconds, 0.0, 1.0));
    if (alpha <= 0.0f) continue;
    // [acct2-ocr] 2026-11-07 a line with characters past Latin-1 (a notice in Russian, Chinese, Japanese or Korean)
    // is drawn with the text layer's font, which has them; the HUD font has Latin-1 only.
    ImFont* font = hud_font;
    for (unsigned char c : line.text)
      if (c >= 0xC4 && c <= 0xF4) {
        if (ImFont* layer = cod3::text_overlay::Font()) font = layer;
        break;
      }
    const ImVec2 ext = font->CalcTextSizeA(size, FLT_MAX, 0.0f, line.text.c_str());
    const ImVec2 pos(std::floor(right - ext.x), std::floor(y));
    const int a = static_cast<int>(alpha * 255.0f);
    dl->AddText(font, size, ImVec2(pos.x + 1.0f, pos.y + 1.0f), IM_COL32(0, 0, 0, a * 3 / 4),
                line.text.c_str());
    dl->AddText(font, size, pos,
                IM_COL32(int(gold.x * 255), int(gold.y * 255), int(gold.z * 255), a), line.text.c_str());
    y += ext.y + 3.0f * scale;
  }
}

}  // namespace cod3::hud
