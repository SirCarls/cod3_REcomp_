// cod3 - the language of the game's text (see text_language.h). [acct2-ocr]

#include "game/text_language.h"

#include "game/font_fill.h"
#include "game/guest.h"       // [acct2-ocr] RawTextFor
#include "ui/text_native.h"   // [acct2-ocr] ScriptReady
#include "ui/text_overlay.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include <imgui.h>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>

REXCVAR_DEFINE_STRING(cod3_text_language, "disc", "cod3/Language",
                      "The game's text: disc (the game's own) or a language from assets/text/<locale>.txt "
                      "(en-US, en-GB, fr-FR, fr-CA, fr-BE, de-DE, it-IT, es-ES, es-419, pt-BR, pt-PT; ru-RU, "
                      "zh-Hans, zh-Hant-TW, zh-Hant-HK, ja-JP, ko-KR are drawn from our own fonts, ui/text_native.*). "
                      "Texts change as the game asks for them again: the next menu page, the next level load")
    .allowed({"disc", "en-US", "en-GB", "fr-FR", "fr-CA", "fr-BE", "de-DE", "it-IT", "es-ES", "es-419", "pt-BR",
              "pt-PT", "ru-RU", "zh-Hans", "zh-Hant-TW", "zh-Hant-HK", "ja-JP", "ko-KR"})
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

// [acct2-ocr] 2026-11-05 For tests (devrun --set): "SECONDS:LOCALE" changes cod3_text_language that many seconds
// after the first text, as F2 > Sound > Text does, to see the lines the game still holds from the language before.
// 2026-11-07: several, "60:ja-JP,90:fr-FR,120:ja-JP" (each counted from the first text), to go back and forth.
REXCVAR_DEFINE_STRING(cod3_test_text_language_at, "", "cod3/Language",
                      "Testing: \"SECONDS:LOCALE\" changes the text language that many seconds after the start; "
                      "several, comma-separated, change it more than once")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
// [acct2-ocr] 2026-11-05 For tests: "ENGLISH|OURS" adds one raw row (RawTextFor) to the chosen language, so a known
// line shows on a known screen ("Loading ...|«Test» ..." on the boot screen: the font fill checked by OCR on the PC).
REXCVAR_DEFINE_STRING(cod3_test_raw_line, "", "cod3/Language",
                      "Testing: \"ENGLISH|OURS\" draws OURS where the game draws the line ENGLISH")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

// [acct2-ocr] Each kind of text can stay in the game's own English while the rest
// is translated (F2 > Sound > Text by kind); the packs group their rows under
// "@<kind>" lines.
REXCVAR_DEFINE_BOOL(cod3_text_menus, true, "cod3/Language",
                    "Translate the menus and the interface (off: the game's own English)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(cod3_text_prompts, true, "cod3/Language",
                    "Translate the button prompts and hints (off: the game's own English)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(cod3_text_objectives, true, "cod3/Language",
                    "Translate the mission objectives (off: the game's own English)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(cod3_text_subtitles, true, "cod3/Language",
                    "Translate the subtitles and dialogue lines (off: the game's own English)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(cod3_text_loading, true, "cod3/Language",
                    "Translate the loading screens and tips (off: the game's own English)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(cod3_text_credits, true, "cod3/Language",
                    "Translate the credits (off: the game's own English)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(cod3_text_film_cards, true, "cod3/Language",
                    "Forced: the English title cards burned into the mission films, shown translated over the "
                    "film (ui/film_text.*)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(cod3_debug_text, false, "cod3/Language",
                    "Log each key the text language answers for the first time")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace cod3::text {

namespace {

struct Locale {
  const char* code;
  const char* name;   // in the settings list (ImGui's font is Latin only)
  bool overlay;       // its script is drawn by ui/text_overlay.*
};

constexpr std::array<Locale, 17> kLocales{{
    {"en-US", "English (United States)", false},
    {"en-GB", "English (United Kingdom)", false},
    {"fr-FR", "Francais (France)", false},
    {"fr-CA", "Francais (Canada)", false},
    {"fr-BE", "Francais (Belgique)", false},
    {"de-DE", "Deutsch", false},
    {"it-IT", "Italiano", false},
    {"es-ES", "Espanol (Espana)", false},
    {"es-419", "Espanol (Latinoamerica)", false},
    {"pt-BR", "Portugues (Brasil)", false},
    {"pt-PT", "Portugues (Portugal)", false},
    {"ru-RU", "Russian", true},
    {"zh-Hans", "Chinese (Simplified)", true},
    {"zh-Hant-TW", "Chinese (Traditional, Taiwan)", true},
    {"zh-Hant-HK", "Chinese (Traditional, Hong Kong)", true},
    {"ja-JP", "Japanese", true},
    {"ko-KR", "Korean", true},
}};

// The kinds of text, in the packs' "@<kind>" lines and the settings.
struct Kind {
  const char* tag;     // "@menus"
  const char* cvar;
  const char* label;
};
constexpr std::array<Kind, 7> kKinds{{
    {"menus", "cod3_text_menus", "Menus & interface"},
    {"prompts", "cod3_text_prompts", "Prompts & hints"},
    {"objectives", "cod3_text_objectives", "Objectives"},
    {"subtitles", "cod3_text_subtitles", "Subtitles & dialogue"},
    {"loading", "cod3_text_loading", "Loading screens & tips"},
    {"credits", "cod3_text_credits", "Credits"},
    // Forced: the films' burned-in cards (keys FILMCARD_<FILM>_<n>), asked for by the films' text layer, not the game.
    {"films", "cod3_text_film_cards", "Film title cards"},
}};
constexpr uint8_t kPromptsKind = 1;
constexpr uint8_t kTickedKinds = 6;   // the ones with a tick under "Text by kind"; the rest are under Forced
constexpr uint8_t kFilmsKind = 6;

bool KindOn(uint8_t kind) {
  switch (kind) {
    case 0: return REXCVAR_GET(cod3_text_menus);
    case 1: return REXCVAR_GET(cod3_text_prompts);
    case 2: return REXCVAR_GET(cod3_text_objectives);
    case 3: return REXCVAR_GET(cod3_text_subtitles);
    case 4: return REXCVAR_GET(cod3_text_loading);
    case 5: return REXCVAR_GET(cod3_text_credits);
    case 6: return REXCVAR_GET(cod3_text_film_cards);
    default: return true;
  }
}

constexpr auto kRescan = std::chrono::seconds(5);
constexpr uint32_t kArenaChunk = 256 * 1024;

int IndexOf(std::string_view code) {
  for (size_t i = 0; i < kLocales.size(); ++i)
    if (code == kLocales[i].code) return int(i);
  return -1;   // "disc" or unknown: the game's own text
}

// The chosen language's texts, converted to the fonts' bytes, and where each
// one was copied into guest memory (copied on first use).
struct Table {
  std::unordered_map<uint32_t, std::string> text;
  std::unordered_map<uint32_t, uint32_t> guest;
  std::unordered_map<uint32_t, uint32_t> entry;   // [acct2-ocr] key hash -> our copy of its table entry (GuestEntryFor)
  std::unordered_map<uint32_t, uint8_t> kind;   // index in kKinds (rows before any "@" line: menus)
  std::unordered_map<uint32_t, std::string> film;   // "@films" rows as written (UTF-8), for ui/film_text.*
  std::unordered_map<uint32_t, bool> port;   // [acct2-ocr] "@port" rows: the port's own FEMENU_ keys (PortText)
  // A button verb's text as handed to the game ("drücken") -> what goes before
  // and after its button ("", " drücken"); only verbs whose button isn't last.
  std::unordered_map<std::string, std::pair<std::string, std::string>> button_at;
  // [acct2-ocr] "@raw" rows (RawTextFor): the English line in the fonts' bytes -> ours (converted) and its kind;
  // where each drawn line's answer was copied (with its kind, so a kind turned off later is still honoured).
  struct Raw {
    std::string text;
    uint8_t kind;
  };
  std::unordered_map<std::string, Raw> raw;
  std::unordered_map<std::string, std::pair<uint32_t, uint8_t>> raw_guest;
  size_t raw_longest = 0;
  // [acct2-ocr] 2026-11-07 "@notices" rows (NoticeText): the port's English HUD notice -> ours, UTF-8 as written.
  std::unordered_map<std::string, std::string> notice;
  // [acct2-ocr] 2026-11-07 name tags left as drawn, each logged once (cod3_debug_text).
  std::unordered_map<std::string, bool> name_missed;
  // [acct2-ocr] 2026-11-07 notices asked for, each logged once with what it gave (cod3_debug_text).
  std::unordered_map<std::string, bool> notice_seen;
};

// "  a  b " -> "a b".
std::string Tidy(std::string_view s) {
  std::string out;
  for (char c : s) {
    if (c == ' ' && (out.empty() || out.back() == ' ')) continue;
    out += c;
  }
  while (!out.empty() && out.back() == ' ') out.pop_back();
  return out;
}

std::mutex g_mutex;   // everything below
std::filesystem::path g_root;
int g_loaded = -2;    // the locale g_table holds (-2: none read yet)
uint32_t g_loaded_fill = 0;   // font_fill::Generation() when it was read
Table g_table;
std::array<bool, kLocales.size()> g_available{};
std::chrono::steady_clock::time_point g_scanned{};
bool g_scanned_once = false;
// Guest memory for our texts: never freed, so a pointer the game keeps stays
// good after a change (a few hundred KB per language read).
uint32_t g_arena = 0, g_arena_used = 0, g_arena_size = 0;

std::atomic<int> g_chosen{-2};   // -2: not read yet
std::atomic<uint32_t> g_generation{1};
std::once_flag g_callback_once;

// [acct2-ocr] 2026-11-05 Whether a script of ours (Russian, Chinese, Japanese, Korean) can be shown now: drawn inside
// the game's own text (ui/text_native.h: from the first frame, no restart), or by our layer once its font is in.
// (The text layer's font is added only at start, so before, the boot loading screen's "Loading ..." stayed English.)
bool ScriptReady(std::string_view code) { return cod3::text_native::Active(code) || cod3::text_overlay::Ready(code); }

int ReadChosen() { return IndexOf(std::string(REXCVAR_GET(cod3_text_language))); }

std::filesystem::path FileFor(const std::filesystem::path& root, int locale) {
  return root / "text" / (std::string(kLocales[size_t(locale)].code) + ".txt");
}

// A copy of `bytes` (and its terminating zero) in guest memory; 0 when the
// game's heap has no room.
uint32_t GuestCopy(uint8_t* base, const std::string& bytes) {
  const uint32_t need = (uint32_t(bytes.size()) + 1 + 3) & ~3u;
  if (g_arena == 0 || g_arena_used + need > g_arena_size) {
    const uint32_t size = need > kArenaChunk ? need : kArenaChunk;
    const uint32_t at = rex::system::kernel_state()->memory()->SystemHeapAlloc(size);
    if (!at) return 0;
    g_arena = at, g_arena_used = 0, g_arena_size = size;
  }
  const uint32_t at = g_arena + g_arena_used;
  std::memcpy(base + at, bytes.c_str(), bytes.size() + 1);
  g_arena_used += need;
  return at;
}

// Reads assets/text/<locale>.txt into g_table (g_mutex held).
void Load(int locale) {
  g_table = Table{};
  g_loaded = locale;
  g_loaded_fill = cod3::font_fill::Generation();
  // The glyphs the fonts lack (game/font_fill.*): used when they are there, else stand-ins.
  const bool fill = cod3::font_fill::Ready();
  g_generation.fetch_add(1, std::memory_order_relaxed);
  if (locale < 0) return;
  const std::filesystem::path path = FileFor(g_root, locale);
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    REXLOG_WARN("cod3: text: no {} in the game's folder: the game's own text stays",
                "text/" + std::string(kLocales[size_t(locale)].code) + ".txt");
    return;
  }
  const bool overlay = kLocales[size_t(locale)].overlay;
  const std::string_view code = kLocales[size_t(locale)].code;
  const bool breaks = code.starts_with("zh-") || code == "ja-JP";   // Korean has its own spaces
  std::string line;
  int read = 0, left_out = 0;
  uint8_t kind = 0;
  bool port = false;   // [acct2-ocr] in the "@port" rows (kind: menus)
  bool raw = false;    // [acct2-ocr] in the "@raw[:<kind>]" rows (RawTextFor)
  bool notice = false;   // [acct2-ocr] 2026-11-07 in the "@notices" rows (NoticeText)
  bool first = true;
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (first && line.rfind("\xEF\xBB\xBF", 0) == 0) line.erase(0, 3);   // a BOM
    first = false;
    if (line.empty() || line[0] == '#') continue;
    if (line[0] == '@') {   // "@subtitles": the rows below are that kind
      std::string_view tag = std::string_view(line).substr(1);
      kind = 0;
      raw = tag == "raw" || tag.starts_with("raw:");   // [acct2-ocr] "@raw:loading": raw rows of that kind
      if (raw) tag = tag.size() > 4 ? tag.substr(4) : std::string_view("menus");
      port = !raw && tag == "port";
      notice = !raw && tag == "notices";   // [acct2-ocr] 2026-11-07
      for (size_t k = 0; k < kKinds.size(); ++k)
        if (tag == kKinds[k].tag) kind = uint8_t(k);
      continue;
    }
    const size_t tab = line.find('\t');
    if (tab == std::string::npos || tab == 0 || tab + 1 >= line.size()) {
      ++left_out;
      continue;
    }
    std::string key = line.substr(0, tab);
    std::string text = line.substr(tab + 1);
    for (size_t at = text.find("\\n"); at != std::string::npos; at = text.find("\\n", at + 1)) text.replace(at, 2, "\n");
    if (notice) {   // [acct2-ocr] 2026-11-07 the port's HUD notices: English -> ours, both UTF-8 (drawn by ImGui)
      if (text != key) g_table.notice[key] = text;
      ++read;
      continue;
    }
    if (raw) {   // [acct2-ocr] English as the game draws it (the fonts' bytes) -> ours
      for (size_t at = key.find("\\n"); at != std::string::npos; at = key.find("\\n", at + 1)) key.replace(at, 2, "\n");
      std::string english = ToGameFontBytes(key, false);
      std::string ours = !overlay ? ToGameFontBytes(text, fill) : breaks ? WithLineBreaks(text) : text;
      if (english.size() >= 2 && ours != english) {
        g_table.raw_longest = std::max(g_table.raw_longest, english.size());
        g_table.raw[std::move(english)] = Table::Raw{std::move(ours), kind};
        ++read;
      }
      continue;
    }
    bool hex = key.size() == 8;
    for (char c : key) hex = hex && std::isxdigit(uint8_t(c)) != 0;
    const uint32_t hash = hex ? uint32_t(std::strtoul(key.c_str(), nullptr, 16)) : KeyHash(key);
    if (port) g_table.port[hash] = true;
    // The overlay's scripts stay UTF-8 (the text layer draws them); Latin ones
    // become the fonts' bytes.
    auto convert = [&](const std::string& t) {
      return !overlay ? ToGameFontBytes(t, fill) : breaks ? WithLineBreaks(t) : t;
    };
    if (const size_t b = text.find("{b}"); b != std::string::npos) {
      std::string before = text.substr(0, b), after = text.substr(b + 3);
      const std::string game = convert(Tidy(before + " " + after));
      while (!after.empty() && after.back() == ' ') after.pop_back();
      if (!Tidy(after).empty()) {
        // "{b} drücken" -> before "", after " drücken" (the text's own spacing kept).
        while (!before.empty() && before.front() == ' ') before.erase(0, 1);
        g_table.button_at.emplace(game, std::make_pair(convert(before), convert(after)));
      }
      g_table.text[hash] = game;
      g_table.kind[hash] = kind;
      ++read;
      continue;
    }
    if (kind == kFilmsKind) g_table.film[hash] = text;   // drawn by ui/film_text.*, in UTF-8
    g_table.text[hash] = convert(text);
    g_table.kind[hash] = kind;
    ++read;
  }
  // [acct2-ocr] cod3_test_raw_line
  if (const std::string t = std::string(REXCVAR_GET(cod3_test_raw_line)); t.find('|') != std::string::npos) {
    const size_t bar = t.find('|');
    std::string english = ToGameFontBytes(t.substr(0, bar), false);
    const std::string text = t.substr(bar + 1);
    std::string ours = !overlay ? ToGameFontBytes(text, fill) : breaks ? WithLineBreaks(text) : text;
    if (english.size() >= 2) {
      const auto it = g_table.raw.find(english);
      const uint8_t k = it != g_table.raw.end() ? it->second.kind : 0;
      g_table.raw_longest = std::max(g_table.raw_longest, english.size());
      g_table.raw[std::move(english)] = Table::Raw{std::move(ours), k};
      REXLOG_INFO("cod3: text: cod3_test_raw_line: a raw row for \"{}\"", t.substr(0, bar));
    }
  }
  REXLOG_INFO("cod3: text: {} ({} texts, {} lines left out; {})", kLocales[size_t(locale)].name, read, left_out,
              fill ? "with the added glyphs" : "stand-ins for the glyphs the fonts lack");
}

void Scan() {
  std::filesystem::path root;
  {
    std::lock_guard lock(g_mutex);
    root = g_root;
  }
  std::array<bool, kLocales.size()> have{};
  std::error_code ec;
  for (size_t i = 0; i < kLocales.size(); ++i)
    have[i] = !root.empty() && std::filesystem::is_regular_file(FileFor(root, int(i)), ec);
  std::lock_guard lock(g_mutex);
  g_available = have;
  g_scanned = std::chrono::steady_clock::now();
  g_scanned_once = true;
}

std::array<bool, kLocales.size()> Available() {
  bool stale;
  {
    std::lock_guard lock(g_mutex);
    stale = !g_scanned_once || std::chrono::steady_clock::now() - g_scanned > kRescan;
  }
  if (stale) Scan();
  std::lock_guard lock(g_mutex);
  return g_available;
}

int ChosenIndex() {
  int v = g_chosen.load(std::memory_order_relaxed);
  if (v != -2) return v;
  std::call_once(g_callback_once, [] {
    rex::cvar::RegisterChangeCallback("cod3_text_language", [](std::string_view, std::string_view) {
      const int now = ReadChosen();
      g_chosen.store(now, std::memory_order_relaxed);
      g_generation.fetch_add(1, std::memory_order_relaxed);
      REXLOG_INFO("cod3: text: {} (from the next text the game asks for)",
                  now < 0 ? "the game's own" : kLocales[size_t(now)].name);
    });
    for (const Kind& k : kKinds)
      rex::cvar::RegisterChangeCallback(k.cvar, [](std::string_view, std::string_view) {
        g_generation.fetch_add(1, std::memory_order_relaxed);
      });
    // [acct2-ocr] cod3_test_text_language_at ("S:L", or "S:L,S:L,..." since 2026-11-07)
    const std::string at = std::string(REXCVAR_GET(cod3_test_text_language_at));
    std::vector<std::pair<int, std::string>> steps;
    for (size_t from = 0; from < at.size();) {
      const size_t comma = std::min(at.find(',', from), at.size());
      const std::string step = at.substr(from, comma - from);
      from = comma + 1;
      const size_t colon = step.find(':');
      if (colon == std::string::npos || colon == 0) continue;
      const int seconds = std::atoi(step.substr(0, colon).c_str());
      const std::string locale = step.substr(colon + 1);
      if (seconds > 0 && (locale == "disc" || IndexOf(locale) >= 0)) steps.emplace_back(seconds, locale);
    }
    if (!steps.empty())
      std::thread([steps] {
        const auto start = std::chrono::steady_clock::now();
        for (const auto& [seconds, locale] : steps) {
          std::this_thread::sleep_until(start + std::chrono::seconds(seconds));
          REXLOG_INFO("cod3: text: cod3_test_text_language_at: {} now", locale);
          rex::cvar::SetFlagByName("cod3_text_language", locale);
          g_chosen.store(IndexOf(locale), std::memory_order_relaxed);   // as the F2 row does
          g_generation.fetch_add(1, std::memory_order_relaxed);
        }
      }).detach();
  });
  v = ReadChosen();
  g_chosen.store(v, std::memory_order_relaxed);
  return v;
}

void Help(const char* text) {
  ImGui::PushTextWrapPos(0.0f);
  ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
  ImGui::TextUnformatted(text);
  ImGui::PopStyleColor();
  ImGui::PopTextWrapPos();
  ImGui::Spacing();
}

// One code point from UTF-8 at `at` (advanced); U+FFFD for a bad sequence.
char32_t NextCodePoint(std::string_view s, size_t& at) {
  const uint8_t c = uint8_t(s[at++]);
  if (c < 0x80) return c;
  const int extra = c >= 0xF8 ? -1 : c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : -1;
  if (extra < 0) return 0xFFFD;
  char32_t cp = c & (0x3F >> extra);
  for (int i = 0; i < extra; ++i) {
    if (at >= s.size() || (uint8_t(s[at]) & 0xC0) != 0x80) return 0xFFFD;
    cp = (cp << 6) | (uint8_t(s[at++]) & 0x3F);
  }
  // [acct2-ocr] 2026-11-07 only what UTF-8 allows: no overlong forms, surrogates or code points past U+10FFFF
  static constexpr char32_t kLeast[4] = {0, 0x80, 0x800, 0x10000};
  if (cp < kLeast[extra] || cp > 0x10FFFF || (cp >= 0xD800 && cp < 0xE000)) return 0xFFFD;
  return cp;
}

// Windows-1252's 0x80-0x9F row (0 = unused there).
constexpr std::array<char16_t, 32> kCp1252High{{
    0x20AC, 0, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0, 0x017D, 0,
    0, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0, 0x017E,
    0x0178,
}};

}  // namespace

void SetGameDataRoot(const std::filesystem::path& root) {
  std::lock_guard lock(g_mutex);
  g_root = root;
  g_scanned_once = false;
  g_loaded = -2;   // read again from the new folder
}

std::string Chosen() {
  const int i = ChosenIndex();
  return i < 0 ? std::string() : std::string(kLocales[size_t(i)].code);
}

// [acct2-ocr] see text_language.h.
std::string ForChosenFonts(std::string_view utf8) {
  const int locale = ChosenIndex();
  if (locale < 0 || utf8.empty()) return "";
  const auto& l = kLocales[size_t(locale)];
  if (!l.overlay) return ToGameFontBytes(utf8, cod3::font_fill::Ready());
  if (!ScriptReady(l.code)) return "";
  const std::string_view code = l.code;
  return code.starts_with("zh-") || code == "ja-JP" ? WithLineBreaks(utf8) : std::string(utf8);
}

uint32_t Generation() {
  ChosenIndex();   // registers the change callback
  return g_generation.load(std::memory_order_relaxed);
}

uint32_t KeyHash(std::string_view key) {
  uint32_t h = 0;
  for (char c : key) h = h * 33 + uint32_t(uint8_t(std::tolower(uint8_t(c))));
  return h;
}

uint32_t GuestTextFor(uint8_t* base, uint32_t key_hash) {
  const int locale = ChosenIndex();
  if (locale < 0) return 0;
  // Our text layer draws these scripts; without its font (not added at start:
  // chosen since, or its files missing) they would show as wrong glyphs.
  if (kLocales[size_t(locale)].overlay && !ScriptReady(kLocales[size_t(locale)].code)) return 0;
  std::lock_guard lock(g_mutex);
  if (g_loaded != locale || g_loaded_fill != cod3::font_fill::Generation()) Load(locale);
  // A kind the player keeps in English: the game's own text.
  if (auto k = g_table.kind.find(key_hash); k != g_table.kind.end() && !KindOn(k->second)) return 0;
  if (auto it = g_table.guest.find(key_hash); it != g_table.guest.end()) return it->second;
  const auto text = g_table.text.find(key_hash);
  if (text == g_table.text.end()) return 0;
  const uint32_t at = GuestCopy(base, text->second);
  if (!at) return 0;
  g_table.guest[key_hash] = at;
  if (REXCVAR_GET(cod3_debug_text)) REXLOG_INFO("cod3: text: {:08X} -> \"{}\"", key_hash, text->second);
  return at;
}

// [acct2-ocr] see text_language.h.
uint32_t PortText(uint8_t* base, const char* key) {
  if (!key || !*key) return 0;   // [acct2-ocr] any key under "@port" (FEMENU_, MPFRONTEND_, COD3_)
  const uint32_t hash = KeyHash(key);
  const uint32_t text = GuestTextFor(base, hash);   // reads the chosen language's file first when needed
  if (!text) return 0;
  std::lock_guard lock(g_mutex);
  return g_table.port.count(hash) ? text : 0;
}

// [acct2-ocr] see text_language.h. 16 bytes: the callers read only +8.
uint32_t GuestEntryFor(uint8_t* base, uint32_t key_hash, uint32_t original_entry) {
  const uint32_t text = GuestTextFor(base, key_hash);
  if (!text) return 0;
  std::lock_guard lock(g_mutex);
  if (auto it = g_table.entry.find(key_hash); it != g_table.entry.end()) {
    const uint8_t* e = base + it->second + 8;   // still ours? (the table may have been read again)
    if ((uint32_t(e[0]) << 24 | uint32_t(e[1]) << 16 | uint32_t(e[2]) << 8 | e[3]) == text) return it->second;
  }
  std::string record(16, '\0');
  if (original_entry >= 0x10000 && original_entry < 0xFFFF0000u) std::memcpy(record.data(), base + original_entry, 16);
  record[8] = char(text >> 24), record[9] = char(text >> 16), record[10] = char(text >> 8), record[11] = char(text);
  const uint32_t at = GuestCopy(base, record);
  if (!at) return 0;
  g_table.entry[key_hash] = at;
  return at;
}

// [acct2-ocr] 2026-11-05 see text_language.h. Called for every line drawn: lines longer than the longest English
// row are passed over before any lookup.
uint32_t RawTextFor(uint8_t* base, uint32_t text) {
  if (text < 0x10000 || text >= 0xF0000000u) return 0;
  const int locale = ChosenIndex();
  if (locale < 0) return 0;
  if (kLocales[size_t(locale)].overlay && !ScriptReady(kLocales[size_t(locale)].code)) return 0;
  std::lock_guard lock(g_mutex);
  if (g_loaded != locale || g_loaded_fill != cod3::font_fill::Generation()) Load(locale);
  if (g_table.raw.empty()) return 0;
  const size_t longest = g_table.raw_longest + 2;   // + a "^N" in front
  const std::string drawn = cod3::guest::ReadString(base, text, longest + 1);
  if (drawn.size() < 2 || drawn.size() > longest) return 0;
  if (auto it = g_table.raw_guest.find(drawn); it != g_table.raw_guest.end())
    return KindOn(it->second.second) ? it->second.first : 0;
  const size_t skip = drawn[0] == '^' && drawn.size() > 2 ? 2 : 0;   // "^3Pvt. Wilson": the color stays in front
  const std::string line = drawn.substr(skip);
  std::string ours;
  uint8_t kind = 0;
  const size_t end = line.find_last_not_of(' ');   // "Press " (a verb before a key cap): the spaces stay after it
  const auto row = g_table.raw.find(end != std::string::npos && end + 1 < line.size() ? line.substr(0, end + 1) : line);
  if (row != g_table.raw.end()) {
    ours = row->second.text + (end != std::string::npos ? line.substr(end + 1) : std::string());
    kind = row->second.kind;
  } else {
#if defined(COD3_MP)
    return 0;   // [acct2-ocr] 2026-11-07 cod3mp: names there are the players' own (and each new one would be kept below)
#endif
    // [acct2-ocr] A name tag no row has ("Pvt. Hale": the friendlies' names are many): a "Pvt. *" row gives the rank,
    // the name stays as drawn. Only for a rank and a capitalized name ("O'Neil", "Smith-Jones").
    // [acct2-ocr] 2026-11-07 (owner 17:48 "soldier names included"): also a name of up to three words ("Van Dyke"),
    // one with the fonts' accented letters ("Müller", in their single-byte order), a rank written without its period
    // ("Pvt Hale" takes the "Pvt. *" row); in Russian, Chinese, Japanese and Korean the name is put in UTF-8 like the
    // rest of the line (it was left in the fonts' bytes: half a line of mojibake). cod3_debug_text logs each name tag
    // still left in English once.
    const size_t space = line.find(' ');
    const auto missed = [&](const char* why) {
      if (REXCVAR_GET(cod3_debug_text) && g_table.name_missed.size() < 256 && g_table.name_missed.emplace(drawn, true).second)
        REXLOG_INFO("cod3: text: name tag left as drawn ({}): \"{}\"", why, drawn);
      return 0u;
    };
    if (space == std::string::npos || space + 1 >= line.size()) return 0;
    const std::string token = line.substr(0, space);
    const bool rank_like = token.size() >= 2 && token.size() <= 6 && std::isupper(uint8_t(token[0])) &&
                           std::all_of(token.begin() + 1, token.end(), [](char c) { return std::islower(uint8_t(c)) || c == '.'; });
    if (!rank_like) return 0;
    // [acct2-ocr] 2026-11-07 the rank's row first: a line of ours that starts like a rank (most German lines, a line
    // typed out letter by letter) has none and is passed over without a word; only an English rank is logged.
    auto rank = g_table.raw.find(token + " *");
    if (rank == g_table.raw.end() && token.back() != '.') rank = g_table.raw.find(token + ". *");
    if (rank == g_table.raw.end()) {
      static constexpr std::string_view kRanks[] = {"Pvt", "Pfc", "Pte", "Cpl", "Sgt", "Lt", "Capt", "Maj"};
      const std::string_view bare = token.back() == '.' ? std::string_view(token).substr(0, token.size() - 1)
                                                        : std::string_view(token);
      return std::find(std::begin(kRanks), std::end(kRanks), bare) != std::end(kRanks)
                 ? missed("no row for the rank: the same in this language?")
                 : 0u;
    }
    const std::string name = line.substr(space + 1);
    if (name.size() > 32) return missed("a long name");
    const auto letter = [](uint8_t c) {   // ASCII or the fonts' Windows-1252 letters
      return std::isalpha(c) || (c >= 0xC0 && c != 0xD7 && c != 0xF7) || c == 0x8A || c == 0x8C || c == 0x8E ||
             c == 0x9A || c == 0x9C || c == 0x9E || c == 0x9F;
    };
    const auto upper = [](uint8_t c) { return std::isupper(c) || (c >= 0xC0 && c <= 0xDE && c != 0xD7) || c == 0x8A || c == 0x8C || c == 0x8E || c == 0x9F; };
    int words = 0;
    for (size_t i = 0; i < name.size(); ++i) {
      const uint8_t c = uint8_t(name[i]);
      if (i == 0 || name[i - 1] == ' ') {
        if (!upper(c)) return missed("not a capitalized name");
        if (++words > 3) return missed("more than three words");
      } else if (c == ' ') {
        if (i + 1 >= name.size()) return missed("a space at the end");
      } else if (!letter(c) && c != '\'' && c != '-') {
        return missed("not a name");
      }
    }
    const size_t star = rank->second.text.find('*');
    if (star == std::string::npos) return 0;
    const bool utf8 = kLocales[size_t(locale)].overlay;   // the row is UTF-8 there: the name goes in UTF-8 too
    std::string shown;
    for (char ch : name) {
      const uint8_t c = uint8_t(ch);
      if (!utf8 || c < 0x80) {
        shown += ch;
      } else {
        // Windows-1252 -> UTF-8 (0xA0-0xFF are U+00A0-U+00FF; the few letters at 0x8A-0x9F have their own code points)
        uint32_t cp = c;
        switch (c) {
          case 0x8A: cp = 0x160; break;
          case 0x8C: cp = 0x152; break;
          case 0x8E: cp = 0x17D; break;
          case 0x9A: cp = 0x161; break;
          case 0x9C: cp = 0x153; break;
          case 0x9E: cp = 0x17E; break;
          case 0x9F: cp = 0x178; break;
          default: break;
        }
        if (cp < 0x800) {
          shown += char(0xC0 | (cp >> 6));
          shown += char(0x80 | (cp & 0x3F));
        }
      }
    }
    ours = rank->second.text.substr(0, star) + shown + rank->second.text.substr(star + 1);
    kind = rank->second.kind;
  }
  // [acct2-ocr] 2026-11-07 each copy stays in guest memory: at most so many (a level shows a few hundred; other players'
  // names and chat reach here too)
  if (g_table.raw_guest.size() >= 4096) return 0;
  const uint32_t at = GuestCopy(base, drawn.substr(0, skip) + ours);
  if (!at) return 0;
  g_table.raw_guest[drawn] = {at, kind};
  if (REXCVAR_GET(cod3_debug_text)) REXLOG_INFO("cod3: text: raw \"{}\" -> \"{}\"", drawn, ours);
  return KindOn(kind) ? at : 0;
}

std::string FilmText(std::string_view key) {
  const int locale = ChosenIndex();
  if (locale < 0 || !KindOn(kFilmsKind)) return "";
  if (kLocales[size_t(locale)].overlay && !cod3::text_overlay::Ready(kLocales[size_t(locale)].code)) return "";  // films: our layer draws them
  std::lock_guard lock(g_mutex);
  if (g_loaded != locale || g_loaded_fill != cod3::font_fill::Generation()) Load(locale);
  const auto it = g_table.film.find(KeyHash(key));
  return it == g_table.film.end() ? std::string() : it->second;
}

// [acct2-ocr] 2026-11-07 see text_language.h.
std::string NoticeText(std::string_view english) {
  const int locale = ChosenIndex();
  if (locale < 0 || english.empty() || !KindOn(0)) return "";
  // Drawn with ImGui: Russian, Chinese, Japanese and Korean need the text layer's font, added at start.
  if (kLocales[size_t(locale)].overlay && !cod3::text_overlay::Ready(kLocales[size_t(locale)].code)) return "";
  std::lock_guard lock(g_mutex);
  if (g_loaded != locale || g_loaded_fill != cod3::font_fill::Generation()) Load(locale);
  const auto it = g_table.notice.find(std::string(english));
  std::string ours = it == g_table.notice.end() ? std::string() : it->second;
  // [acct2-ocr] cod3_debug_text: each notice once per language, with what it gave (the shots don't show the HUD layer)
  if (REXCVAR_GET(cod3_debug_text) && g_table.notice_seen.size() < 64 &&
      g_table.notice_seen.emplace(std::string(english), true).second)
    REXLOG_INFO("cod3: text: notice \"{}\" -> \"{}\"", english, ours.empty() ? std::string("(as written)") : ours);
  return ours;
}

std::filesystem::path GameDataRoot() {
  std::lock_guard lock(g_mutex);
  return g_root;
}

std::string PlaceButton(std::string_view sentence) {
  if (ChosenIndex() < 0 || !KindOn(kPromptsKind)) return "";   // English prompts keep the game's order
  std::lock_guard lock(g_mutex);
  // The longest verb the sentence starts with ("gedrückt halten", not "gedrückt").
  const std::pair<std::string, std::string>* best = nullptr;
  size_t best_size = 0;
  for (const auto& [verb, around] : g_table.button_at)
    if (verb.size() > best_size && sentence.size() > verb.size() + 1 && sentence.substr(0, verb.size()) == verb &&
        sentence[verb.size()] == ' ')
      best = &around, best_size = verb.size();
  if (!best) return "";
  std::string_view button = sentence.substr(best_size);
  while (!button.empty() && button.front() == ' ') button.remove_prefix(1);
  if (button.empty()) return "";
  return best->first + std::string(button) + best->second;
}

bool IsCjk(char32_t cp) {
  return (cp >= 0x3000 && cp <= 0x9FFF) || (cp >= 0xF900 && cp <= 0xFAFF) || (cp >= 0xFF00 && cp <= 0xFFEF);
}

std::string WithLineBreaks(std::string_view utf8) {
  // Kinsoku: characters a line must not start with, and ones it must not end with.
  static constexpr std::u32string_view kNoStart =
      U"、。，．・：；？！ー）」』】〕〉》｝〟〜々ぁぃぅぇぉっゃゅょゎァィゥェォッャュョヮヵヶ％";
  static constexpr std::u32string_view kNoEnd = U"（「『【〔〈《｛〝";
  std::string out;
  out.reserve(utf8.size() + utf8.size() / 3);
  char32_t prev = 0;
  for (size_t at = 0; at < utf8.size();) {
    const size_t from = at;
    const char32_t cp = NextCodePoint(utf8, at);
    if (IsCjk(prev) && IsCjk(cp) && kNoStart.find(cp) == std::u32string_view::npos &&
        kNoEnd.find(prev) == std::u32string_view::npos)
      out += ' ';
    out.append(utf8.substr(from, at - from));
    prev = cp;
  }
  return out;
}

std::string ToGameFontBytes(std::string_view utf8, bool with_fill) {
  std::string out;
  out.reserve(utf8.size());
  for (size_t at = 0; at < utf8.size();) {
    const char32_t cp = NextCodePoint(utf8, at);
    if (cp == 0x2026) {   // … : three dots (no font has it; as wide as the font's own "...")
      out += "...";
      continue;
    }
    // The glyphs the fonts lack: stand-ins unless game/font_fill.* added them.
    if (!with_fill) {
      switch (cp) {
        case 0x2018: case 0x2019: out += '\''; continue;   // ‘ ’ (the font's ' is straight)
        case 0x201C: case 0x201E: case 0x00AB: case 0x00BB: out += '"'; continue;   // “ „ « »
        case 0x2013: out += '-'; continue;            // –
        case 0x00AA: out += 'a'; continue;            // ª
        case 0x00BA: out += 'o'; continue;            // º
        case 0x00C3: out += 'A'; continue;            // Ã
        case 0x00D5: out += 'O'; continue;            // Õ
        default: break;
      }
    }
    if (cp < 0x80 || (cp >= 0xA0 && cp <= 0xFF)) {
      out += char(uint8_t(cp));
      continue;
    }
    bool found = false;
    for (size_t i = 0; i < kCp1252High.size(); ++i) {
      if (kCp1252High[i] != 0 && char32_t(kCp1252High[i]) == cp) {
        out += char(uint8_t(0x80 + i));
        found = true;
        break;
      }
    }
    if (found) continue;
    out += '?';   // not in the fonts' 224 characters
  }
  return out;
}

bool DrawSettingsRow() {
  const auto have = Available();
  const int chosen = ChosenIndex();
  constexpr const char* kOwn = "The game's own";
  std::string preview = chosen < 0 ? kOwn : kLocales[size_t(chosen)].name;
  if (chosen >= 0 && !have[size_t(chosen)]) preview += " (not installed)";
  else if (chosen >= 0 && kLocales[size_t(chosen)].overlay && !ScriptReady(kLocales[size_t(chosen)].code))
    preview += " (applies after a restart)";
  int pick = -2;
  if (ImGui::BeginCombo("Text", preview.c_str())) {
    if (ImGui::Selectable(kOwn, chosen < 0)) pick = -1;
    for (size_t i = 0; i < kLocales.size(); ++i) {
      std::string label = kLocales[i].name;
      const bool layer = !kLocales[i].overlay || cod3::text_overlay::Available(kLocales[i].code);
      const bool usable = have[i] && layer;
      if (!have[i]) label += " (not installed)";
      else if (!layer) label += " (its font isn't installed)";
      else if (kLocales[i].overlay && !ScriptReady(kLocales[i].code)) label += " (after a restart)";
      ImGui::BeginDisabled(!usable && int(i) != chosen);
      if (ImGui::Selectable(label.c_str(), int(i) == chosen)) pick = int(i);
      ImGui::EndDisabled();
    }
    ImGui::EndCombo();
  }
  Help("The language of the game's menus, prompts, hints, objectives and subtitles, from text/<language>.txt "
       "in the game's folder. Changes from the next text the game asks for: open the menu page again, or "
       "load a level. Russian, Chinese, Japanese and Korean are drawn by the port itself, "
       "from text/fonts/overlay/; a change to or between them applies after a restart. The voices have their own "
       "setting above.");
  // [acct2-ocr] Kinds of text kept in the game's own English, and the forced ones.
  bool kinds_changed = false;
  ImGui::BeginDisabled(chosen < 0);
  if (ImGui::TreeNode("Text by kind")) {
    for (uint8_t k = 0; k < kTickedKinds; ++k) {
      bool on = KindOn(k);
      if (ImGui::Checkbox(kKinds[k].label, &on)) {
        rex::cvar::SetFlagByName(kKinds[k].cvar, on ? "true" : "false");
        g_generation.fetch_add(1, std::memory_order_relaxed);
        kinds_changed = true;
      }
    }
    Help("Unticked: that kind stays in the game's own English while the rest is in the chosen language "
         "(for example English menus with translated subtitles). Changes from the next text the game asks for.");
    ImGui::SeparatorText("Forced");
    bool cards = REXCVAR_GET(cod3_text_film_cards);
    if (ImGui::Checkbox("Film title cards", &cards)) {
      rex::cvar::SetFlagByName("cod3_text_film_cards", cards ? "true" : "false");
      kinds_changed = true;
    }
    Help("The English place names and dates burned into the mission films, and the legal screens: ours are "
         "drawn over them while the film plays, whatever the ticks above (ui/film_text.h).");
    ImGui::TreePop();
  }
  ImGui::EndDisabled();
  if (pick == -2 || pick == chosen) return kinds_changed;
  rex::cvar::SetFlagByName("cod3_text_language", pick < 0 ? "disc" : kLocales[size_t(pick)].code);
  g_chosen.store(pick, std::memory_order_relaxed);   // also when the change callback isn't there yet
  g_generation.fetch_add(1, std::memory_order_relaxed);
  return true;
}

// [acct2-ocr] 2026-11-07 The in-game Options rows' (text_language.h): the same choice as the F2 row above.
int LanguageCount() { return int(kLocales.size()); }

const char* LanguageCode(int index) {
  return index >= 0 && index < LanguageCount() ? kLocales[size_t(index)].code : "";
}

const char* LanguageName(int index) {
  return index >= 0 && index < LanguageCount() ? kLocales[size_t(index)].name : "";
}

bool LanguageUsable(int index) {
  if (index < 0) return true;  // the game's own
  if (index >= LanguageCount()) return false;
  const Locale& l = kLocales[size_t(index)];
  return Available()[size_t(index)] && (!l.overlay || cod3::text_overlay::Available(l.code));
}

bool LanguageAfterRestart(int index) {
  if (index < 0 || index >= LanguageCount()) return false;
  const Locale& l = kLocales[size_t(index)];
  return l.overlay && !ScriptReady(l.code);
}

int ChosenLanguage() { return ChosenIndex(); }

bool ChooseLanguage(int index) {
  if (index < -1 || index >= LanguageCount() || index == ChosenIndex() || !LanguageUsable(index)) return false;
  rex::cvar::SetFlagByName("cod3_text_language", index < 0 ? "disc" : kLocales[size_t(index)].code);
  g_chosen.store(index, std::memory_order_relaxed);   // also when the change callback isn't there yet
  g_generation.fetch_add(1, std::memory_order_relaxed);
  return true;
}

}  // namespace cod3::text

#if defined(COD3_MP)
// cod3mp's string-table lookup by key hash (r3 the tables, r4 the hash), the
// campaign's 0x82527FD0 (game/button_prompts.cpp hooks that one): our text for
// the key in the chosen language first, else the game's own.
REX_EXTERN(__imp__sub_82548B68);
REX_HOOK_RAW(sub_82548B68) {
  const uint32_t hash = ctx.r4.u32;
  __imp__sub_82548B68(ctx, base);
  // [acct2-ocr] r3 is the table entry (text at +8), so ours goes in a copy of it.
  if (const uint32_t ours = cod3::text::GuestEntryFor(base, hash, ctx.r3.u32)) ctx.r3.u64 = ours;
}
#endif
