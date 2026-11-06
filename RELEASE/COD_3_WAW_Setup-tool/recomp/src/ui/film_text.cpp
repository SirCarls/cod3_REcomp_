// cod3 - the films' burned-in English text, in the chosen language (see film_text.h). [acct2-ocr]

#include "ui/film_text.h"

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include <imgui.h>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

#include "game/addresses.h"
#include "game/guest.h"
#include "game/text_language.h"
#include "ui/text_overlay.h"

REXCVAR_DEFINE_INT32(cod3_film_text_delay_ms, 0, "cod3/Language",
                     "Film title cards in another language: show ours this much later (+) or earlier (-), in ms")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
// [acct2-ocr] 255 (was 225): owner 2026-11-05, the boxes were slightly see-through.
REXCVAR_DEFINE_INT32(cod3_film_text_plate, 255, "cod3/Language",
                     "Film title cards: how dark the plate over the English line is (0-255; 255 = solid)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(cod3_debug_film_text, false, "cod3/Language",
                    "Log each film the layer sees, and the card lines it draws")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DECLARE(bool, cod3_text_film_cards);   // game/text_language.cpp (F2 > Sound > Text > Forced)

namespace cod3::film_text {

namespace {

using Clock = std::chrono::steady_clock;

struct Row {
  std::string film;
  float start = 0, end = 0;        // seconds
  float x = 0, y = 0, w = 0, h = 0;  // in the film's 1280x720 picture
  std::string key;
};

constexpr float kFilmWidth = 1280.0f, kFilmHeight = 720.0f;
constexpr float kLeadIn = 0.25f;   // s before a line's first reading (frames were 1-2 s apart)
constexpr float kLeadOut = 0.75f;  // s after its last
constexpr float kFade = 0.3f;

std::mutex g_mutex;   // everything below
std::vector<Row> g_rows;
bool g_rows_read = false;
uint8_t* g_base = nullptr;
std::string g_film;            // "" = none
uint32_t g_handle = 0;
Clock::time_point g_started{};
std::vector<std::string> g_logged;

void ReadRows() {
  g_rows_read = true;
  const auto path = cod3::text::GameDataRoot() / "text" / "film_cards.txt";
  std::ifstream in(path);
  if (!in) {
    REXLOG_WARN("cod3: film text: no text/film_cards.txt in the game's folder: the films keep their English cards");
    return;
  }
  std::string line;
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty() || line[0] == '#') continue;
    std::vector<std::string> c;
    std::stringstream ss(line);
    for (std::string f; std::getline(ss, f, '\t');) c.push_back(f);
    if (c.size() < 8) continue;
    Row r;
    r.film = c[0];
    r.start = std::strtof(c[1].c_str(), nullptr);
    r.end = std::strtof(c[2].c_str(), nullptr);
    r.x = std::strtof(c[3].c_str(), nullptr);
    r.y = std::strtof(c[4].c_str(), nullptr);
    r.w = std::strtof(c[5].c_str(), nullptr);
    r.h = std::strtof(c[6].c_str(), nullptr);
    r.key = c[7];
    if (r.end > r.start && r.h > 0) g_rows.push_back(std::move(r));
  }
  REXLOG_INFO("cod3: film text: {} card lines", g_rows.size());
}

// "movies\\crssrds.wmv" -> "crssrds".
std::string BareName(std::string s) {
  if (const size_t slash = s.find_last_of("/\\"); slash != std::string::npos) s.erase(0, slash + 1);
  if (const size_t dot = s.rfind('.'); dot != std::string::npos) s.erase(dot);
  for (char& c : s) c = char(std::tolower(uint8_t(c)));
  return s;
}

// The first `count` code points of UTF-8 `s`.
std::string_view FirstCodePoints(std::string_view s, size_t count) {
  size_t at = 0;
  for (size_t n = 0; at < s.size() && n < count; ++n) {
    ++at;
    while (at < s.size() && (uint8_t(s[at]) & 0xC0) == 0x80) ++at;
  }
  return s.substr(0, at);
}

size_t CodePoints(std::string_view s) {
  size_t n = 0;
  for (char c : s) n += (uint8_t(c) & 0xC0) != 0x80;
  return n;
}

bool IsCjkLead(std::string_view s, size_t at) {   // a 3-byte lead E3-E9 or EF: break anywhere
  const uint8_t c = uint8_t(s[at]);
  return (c >= 0xE3 && c <= 0xE9) || c == 0xEF;
}

// Lines of `text` (its own \n kept) no wider than `width` at `size`: breaks at
// spaces, and between any two CJK characters.
std::vector<std::string> Wrap(ImFont* font, float size, std::string_view text, float width) {
  std::vector<std::string> out;
  size_t para = 0;
  while (para <= text.size()) {
    size_t nl = text.find('\n', para);
    if (nl == std::string_view::npos) nl = text.size();
    std::string_view p = text.substr(para, nl - para);
    std::string line;
    size_t at = 0;
    while (at < p.size()) {
      // The next piece: a word with its following space, or one CJK character.
      size_t end = at;
      if (IsCjkLead(p, at)) {
        end = at + 1;
        while (end < p.size() && (uint8_t(p[end]) & 0xC0) == 0x80) ++end;
      } else {
        while (end < p.size() && p[end] != ' ' && !IsCjkLead(p, end)) ++end;
        while (end < p.size() && p[end] == ' ') ++end;
      }
      const std::string piece(p.substr(at, end - at));
      const std::string tried = line + piece;
      const float w = font->CalcTextSizeA(size, FLT_MAX, 0.0f, tried.c_str()).x;
      if (w > width && !line.empty()) {
        while (!line.empty() && line.back() == ' ') line.pop_back();
        out.push_back(line);
        line = piece;
      } else {
        line = tried;
      }
      at = end;
    }
    while (!line.empty() && line.back() == ' ') line.pop_back();
    out.push_back(line);
    para = nl + 1;
  }
  return out;
}

}  // namespace

void OnFilmStart(uint8_t* base, const std::string& name, bool opened) {
  std::lock_guard lock(g_mutex);
  g_base = base;
  if (!g_rows_read) ReadRows();
  g_film = opened ? BareName(name) : std::string();
  g_handle = opened ? cod3::guest::ReadU32(base, addr::movie::kHandle) : 0;
  g_started = Clock::now();
  g_logged.clear();
  if (REXCVAR_GET(cod3_debug_film_text))
    REXLOG_INFO("cod3: film text: film \"{}\" {} (movie object {:08X})", g_film.empty() ? name : g_film,
                opened ? "opened" : "didn't open", g_handle);
}

void Draw(float ax, float ay, float aw, float ah) {
  if (aw < 1.0f || ah < 1.0f || !REXCVAR_GET(cod3_text_film_cards)) return;
  struct Shown {
    Row row;
    float t;
  };
  std::vector<Shown> shown;
  {
    std::lock_guard lock(g_mutex);
    if (g_film.empty() || !g_base) return;
    const uint32_t handle = cod3::guest::ReadU32(g_base, addr::movie::kHandle);
    if (handle == 0 || (g_handle != 0 && handle != g_handle)) {   // ended, skipped or replaced
      if (REXCVAR_GET(cod3_debug_film_text)) REXLOG_INFO("cod3: film text: film \"{}\" over", g_film);
      g_film.clear();
      return;
    }
    const float t = std::chrono::duration<float>(Clock::now() - g_started).count() -
                    float(REXCVAR_GET(cod3_film_text_delay_ms)) / 1000.0f;
    for (const Row& r : g_rows)
      if (r.film == g_film && t >= r.start - kLeadIn && t < r.end + kLeadOut) shown.push_back({r, t});
  }
  if (shown.empty()) return;
  const std::string locale = cod3::text::Chosen();
  ImFont* font = cod3::text_overlay::Ready(locale) ? cod3::text_overlay::Font() : ImGui::GetFont();
  if (!font) return;
  const float sx = aw / kFilmWidth, sy = ah / kFilmHeight;
  ImDrawList* dl = ImGui::GetBackgroundDrawList();
  const int plate = std::clamp(int(REXCVAR_GET(cod3_film_text_plate)), 0, 255);
  for (const Shown& s : shown) {
    const std::string text = cod3::text::FilmText(s.row.key);
    if (text.empty()) continue;   // the game's English stays
    const Row& r = s.row;
    const float from = r.start - kLeadIn, to = r.end + kLeadOut;
    float alpha = std::clamp(std::min(s.t - from, to - s.t) / kFade, 0.0f, 1.0f);
    if (REXCVAR_GET(cod3_debug_film_text)) {
      std::lock_guard lock(g_mutex);
      if (std::find(g_logged.begin(), g_logged.end(), r.key) == g_logged.end()) {
        g_logged.push_back(r.key);
        REXLOG_INFO("cod3: film text: {} at {:.1f} s: \"{}\"", r.key, s.t, text);
      }
    }
    if (r.w >= kFilmWidth * 0.9f && r.h >= kFilmHeight * 0.9f) {
      // A legal screen: black over the whole picture, our text centered.
      dl->AddRectFilled(ImVec2(ax, ay), ImVec2(ax + aw, ay + ah), IM_COL32(0, 0, 0, int(255 * alpha)));
      const float size = 24.0f * sy;
      const std::vector<std::string> lines = Wrap(font, size, text, aw * 0.8f);
      const float gap = size * 1.45f;
      float y = ay + (ah - gap * float(lines.size())) * 0.5f;
      for (const std::string& l : lines) {
        const float w = font->CalcTextSizeA(size, FLT_MAX, 0.0f, l.c_str()).x;
        dl->AddText(font, size, ImVec2(ax + (aw - w) * 0.5f, y), IM_COL32(255, 255, 255, int(255 * alpha)), l.c_str());
        y += gap;
      }
      continue;
    }
    // A card line: the plate over the English line (and over ours if it is
    // longer), ours typed on over the first second as the film's own are.
    const float cap = r.h * sy;                 // the box is the capitals' height
    const float size = cap / 0.72f;
    const float x = ax + r.x * sx, top = ay + r.y * sy;
    const float full = font->CalcTextSizeA(size, FLT_MAX, 0.0f, text.c_str()).x;
    const float pad_x = size * 0.35f, pad_y = size * 0.22f;
    const float right = std::max(x + r.w * sx, x + full);
    dl->AddRectFilled(ImVec2(x - pad_x, top - pad_y), ImVec2(right + pad_x, top + cap + pad_y),
                      IM_COL32(0, 0, 0, int(plate * alpha)), size * 0.12f);
    const float typing = std::clamp((s.t - from) / std::min(1.2f, std::max(0.4f, (r.end - r.start) * 0.4f)), 0.0f, 1.0f);
    const size_t n = CodePoints(text);
    const std::string_view part = FirstCodePoints(text, size_t(std::ceil(float(n) * typing)));
    // The font's capitals sit about 0.2 em under the line box's top.
    const float ty = top - size * 0.2f;
    const std::string drawn(part);
    dl->AddText(font, size, ImVec2(x, ty), IM_COL32(255, 255, 255, int(255 * alpha)), drawn.c_str());
  }
}

}  // namespace cod3::film_text

#if !defined(COD3_MP)
// Game_PlayCinematic (r3 the film's name; returns whether it opened). Its callers:
// the level loader (the briefing) and the blocking film loop (legal screens,
// logos, attract, bonus films, finale).
REX_EXTERN(__imp__sub_824EED20);
REX_HOOK_RAW(sub_824EED20) {
  const uint32_t name_at = ctx.r3.u32;
  const std::string name = name_at ? cod3::guest::ReadString(base, name_at, 64) : std::string();
  __imp__sub_824EED20(ctx, base);
  cod3::film_text::OnFilmStart(base, name, (ctx.r3.u32 & 0xFF) != 0);
}
#endif
