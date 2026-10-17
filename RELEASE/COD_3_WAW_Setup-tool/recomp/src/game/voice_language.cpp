// cod3 - the language the game's voices speak (see voice_language.h).

#include "game/voice_language.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <imgui.h>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

#include "game/guest.h"
#include "game/voice_paths.h"

REXCVAR_DEFINE_STRING(cod3_voice_language, "disc", "cod3/Audio",
                      "Voices in the campaign: disc (the game's own) or en, de, fr, es, it, from the "
                      "voice files of another disc that the setup tool copied into assets/; the text "
                      "stays the game's own. Applies from the next level load")
    .allowed({"disc", "en", "de", "fr", "es", "it"})
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace cod3::voice {

namespace {

// The game's language (u32, 0-4): which slot of each bank 0x82494E90 loads
// besides slot 5. Set once by 0x82458798 from the archives ("de": German,
// "eu": French, Spanish or Italian by the console language, else English).
constexpr uint32_t kGameLanguage = 0x82A2A150;
constexpr auto kRescan = std::chrono::seconds(5);

std::mutex g_mutex;               // g_root, g_available, g_scanned, g_logged
std::filesystem::path g_root;
std::array<int, kLanguages> g_available{};
std::chrono::steady_clock::time_point g_scanned{};
bool g_scanned_once = false;
std::set<std::string> g_logged;   // banks already logged (once per bank and voice)

std::atomic<int> g_chosen{-2};    // -2: not read yet
std::once_flag g_callback_once;

int ReadChosen() { return LanguageFromCode(std::string(REXCVAR_GET(cod3_voice_language))); }

std::filesystem::path Root() {
  std::lock_guard lock(g_mutex);
  return g_root;
}

bool SameName(std::string_view a, std::string_view b) { return detail::Lower(a) == detail::Lower(b); }

// "sp\blkbrn\blkbrn.wbk" (the game's archives name banks relative to the
// game's folder; a "d:\" or "game:\" device is dropped) -> its parts.
std::vector<std::string> Parts(std::string_view game_path) {
  if (const size_t colon = game_path.find(':'); colon != std::string_view::npos && colon < 16)
    game_path.remove_prefix(colon + 1);
  std::vector<std::string> parts;
  std::string cur;
  for (char c : game_path) {
    if (c == '\\' || c == '/') {
      if (!cur.empty()) parts.push_back(std::move(cur));
      cur.clear();
    } else {
      cur += c;
    }
  }
  if (!cur.empty()) parts.push_back(std::move(cur));
  return parts;
}

// The file a game path names under assets/, matching names without case as
// the game's own file lookups do; nothing when there is none.
std::optional<std::filesystem::path> Find(const std::string& game_path) {
  std::filesystem::path p = Root();
  if (p.empty()) return std::nullopt;
  std::error_code ec;
  for (const std::string& part : Parts(game_path)) {
    if (part == "." || part == "..") return std::nullopt;
    std::filesystem::path exact = p / part;
    if (std::filesystem::exists(exact, ec)) {
      p = std::move(exact);
      continue;
    }
    bool found = false;
    for (std::filesystem::directory_iterator it(p, ec), end; !ec && it != end; it.increment(ec)) {
      if (SameName(it->path().filename().string(), part)) {
        p = it->path();
        found = true;
        break;
      }
    }
    if (!found) return std::nullopt;
  }
  if (!std::filesystem::is_regular_file(p, ec)) return std::nullopt;
  return p;
}

bool Exists(const std::string& game_path) { return Find(game_path).has_value(); }

// A planned path as the files in assets/ spell it (the game keeps the
// device, if any, of its own path).
std::string GameSpelling(const std::string& planned) {
  const auto found = Find(planned);
  if (!found) return planned;
  std::string device;
  if (const size_t colon = planned.find(':'); colon != std::string::npos && colon < 16)
    device = planned.substr(0, colon + 1) + "\\";
  std::string rel = found->lexically_relative(Root()).generic_string();
  for (char& c : rel)
    if (c == '/') c = '\\';
  return device + rel;
}

std::string Describe(const std::vector<Registration>& plan) {
  if (plan.empty()) return "nothing (the bank above takes its place)";
  std::string s;
  for (const auto& r : plan) {
    if (!s.empty()) s += " + ";
    s += r.path + " (slot " + std::to_string(r.slot) + ")";
  }
  return s;
}

void LogOnce(int voice, const std::string& path, const std::string& text) {
  {
    std::lock_guard lock(g_mutex);
    if (!g_logged.insert(std::to_string(voice) + detail::Lower(path)).second) return;
  }
  REXLOG_INFO("cod3: voices ({}): {}", kNames[voice], text);
}

void Scan() {
  std::array<int, kLanguages> counts{};
  const std::filesystem::path root = Root();
  std::error_code ec;
  // Campaign banks only: what the setting changes.
  for (std::filesystem::recursive_directory_iterator it(root / "sp", ec), end; !ec && it != end;
       it.increment(ec)) {
    if (!it->is_regular_file(ec)) continue;
    const std::string ext = detail::Lower(it->path().extension().string());
    if (ext == kWholeExt[0]) ++counts[0];   // .wbk: the English discs' banks
    for (int i = 0; i < kLanguages; ++i)
      if (ext == kTagExt[i]) ++counts[i];
  }
  std::lock_guard lock(g_mutex);
  g_available = counts;
  g_scanned = std::chrono::steady_clock::now();
  g_scanned_once = true;
}

void Help(const char* text) {
  ImGui::PushTextWrapPos(0.0f);
  ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
  ImGui::TextUnformatted(text);
  ImGui::PopStyleColor();
  ImGui::PopTextWrapPos();
  ImGui::Spacing();
}

}  // namespace

void SetGameDataRoot(const std::filesystem::path& root) {
  std::lock_guard lock(g_mutex);
  g_root = root;
  g_scanned_once = false;
}

int Chosen() {
  int v = g_chosen.load(std::memory_order_relaxed);
  if (v != -2) return v;
  std::call_once(g_callback_once, [] {
    rex::cvar::RegisterChangeCallback("cod3_voice_language", [](std::string_view, std::string_view) {
      const int now = ReadChosen();
      g_chosen.store(now, std::memory_order_relaxed);
      REXLOG_INFO("cod3: voices: {} (from the next level load)", now < 0 ? "the game's own" : kNames[now]);
    });
  });
  v = ReadChosen();
  g_chosen.store(v, std::memory_order_relaxed);
  return v;
}

std::array<int, kLanguages> Available() {
  bool stale;
  {
    std::lock_guard lock(g_mutex);
    stale = !g_scanned_once || std::chrono::steady_clock::now() - g_scanned > kRescan;
  }
  if (stale) Scan();
  std::lock_guard lock(g_mutex);
  return g_available;
}

bool DrawSettingsRow() {
  const auto have = Available();
  const int chosen = Chosen();
  constexpr const char* kOwn = "The game's own";
  std::string preview = chosen < 0 ? kOwn : kNames[chosen];
  if (chosen >= 0 && have[chosen] == 0) preview += " (not installed)";
  int pick = -2;
  if (ImGui::BeginCombo("Voices", preview.c_str())) {
    if (ImGui::Selectable(kOwn, chosen < 0)) pick = -1;
    for (int i = 0; i < kLanguages; ++i) {
      std::string label = kNames[i];
      if (have[i] == 0) label += " (not installed)";
      ImGui::BeginDisabled(have[i] == 0 && i != chosen);
      if (ImGui::Selectable(label.c_str(), i == chosen)) pick = i;
      ImGui::EndDisabled();
    }
    ImGui::EndCombo();
  }
  std::string installed;
  for (int i = 0; i < kLanguages; ++i) {
    if (have[i] == 0) continue;
    if (!installed.empty()) installed += ", ";
    installed += kNames[i];
  }
  const std::string help =
      "The language the soldiers speak in the campaign and its films. Another disc's voices "
      "(German, or French, Spanish and Italian) come from the setup tool: Play page > Add voices "
      "from another disc. The game's text stays as it is. Applies from the next level. Installed: " +
      (installed.empty() ? std::string("none found") : installed) + ".";
  Help(help.c_str());
  if (pick == -2 || pick == chosen) return false;
  rex::cvar::SetFlagByName("cod3_voice_language", pick < 0 ? "disc" : kCodes[pick]);
  g_chosen.store(pick, std::memory_order_relaxed);   // also when the change callback isn't there yet
  return true;
}

}  // namespace cod3::voice

// A sound bank of a level or of battle chatter: r3 the bank table, r4 the bank's key
// (its name hashed; every language of a bank shares it), r5 its path, r6 the
// language slot, r7 the archive. Registers the banks of the chosen voices in
// its place when assets/ has them (game/voice_paths.h).
REX_EXTERN(__imp__sub_82478B80);
REX_HOOK_RAW(sub_82478B80) {
  using namespace cod3::voice;
  const int voice = Chosen();
  if (voice < 0) {
    __imp__sub_82478B80(ctx, base);
    return;
  }
  const std::string path = cod3::guest::ReadString(base, ctx.r5.u32, 260);
  const uint32_t slot = ctx.r6.u32;
  const int game_language = cod3::guest::ReadS32(base, kGameLanguage);
  const auto plan = PlanBank(path, slot, game_language, voice, Exists);
  if (plan.size() == 1 && plan[0].path == path && plan[0].slot == slot) {
    if (voice != game_language && !Exists(path))
      LogOnce(voice, path, path + " isn't in assets/ as the game names it: kept");
    __imp__sub_82478B80(ctx, base);
    return;
  }
  LogOnce(voice, path, path + " (slot " + std::to_string(slot) + ") -> " + Describe(plan));
  const PPCRegister table = ctx.r3, key = ctx.r4, archive = ctx.r7;
  for (const Registration& r : plan) {
    // Each call on its own registers (the bank's file is opened by path; the
    // key stays the game's, so the bank's languages stay one entry).
    cod3::guest::Call call(ctx, base);
    const uint32_t p = SameName(r.path, path) ? ctx.r5.u32 : call.PushString(GameSpelling(r.path));
    PPCContext& c = call.ctx();
    const uint32_t saved_r1 = c.r1.u32;
    c.r1.u32 -= 0x70;   // the callee's frame (back chain and LR save)
    c.r3 = table;
    c.r4 = key;
    c.r5.u64 = p;
    c.r6.u64 = r.slot;
    c.r7 = archive;
    __imp__sub_82478B80(c, base);
    c.r1.u32 = saved_r1;
  }
}

// After 0x824EED20's XGetLanguage call (0x824EEDCC): r3 is the console
// language it picks a movie's sound track by. Movies without the chosen
// language fall back as the game's own code does (English first).
void Cod3VoiceMovieLanguage(PPCRegister& r3) {
  const int voice = cod3::voice::Chosen();
  if (voice >= 0) r3.u64 = cod3::voice::ConsoleLanguageFor(voice);
}
