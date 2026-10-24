// cod3 - the language the game's voices speak (see voice_language.h).

#include "game/voice_language.h"

#include <array>
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
                      "stays the game's own. The films change at once, the soldiers at the next level load")
    .allowed({"disc", "en", "de", "fr", "es", "it"})
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(cod3_debug_voices, false, "cod3/Audio",
                    "Log every sound bank the game registers and every one the Voices setting changes")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

// The game's own registration, called by the hook below and by RefreshBanks.
REX_EXTERN(__imp__sub_82478B80);

namespace cod3::voice {

namespace {

using cod3::guest::ReadS32;
using cod3::guest::ReadU32;
using cod3::guest::WriteU32;

// The game's language (u32, 0-4): which slot of each bank 0x82494E90 loads
// besides slot 5. Set once by 0x82458798 from the archives ("de": German,
// "eu": French, Spanish or Italian by the console language, else English).
constexpr uint32_t kGameLanguage = 0x82A2A150;
constexpr auto kRescan = std::chrono::seconds(5);

// The bank manager (docs/COD3_AUDIO.md, "How a sound plays"; default.xex):
//   0x82478B80 (mgr, key, path, slot, archive)  registers a bank file in a slot (hooked below)
//   0x82494E90 (mgr, key, async)                loads slot 5 and the game's language slot
//   0x82494C70 (mgr, key, async)                releases the loaded slots, closes their files
//   0x82493F00 (mgr)                            finishes loads and releases
//   0x82123650 (handle)                         closes a file
// mgr: entries of 0x6C bytes from +8, their number at +1736 (16 at most).
// An entry: the key (8 words, the bank's name hashed), the archive (+32),
// then for each slot 0-5 its state (+36: 0 nothing, 1 loading, 2 loaded,
// 3 releasing), its file (+60) and its loaded bank (+84); -1 = none.
constexpr uint32_t kLoadBank = 0x82494E90;
constexpr uint32_t kUnloadBank = 0x82494C70;
constexpr uint32_t kUpdateBanks = 0x82493F00;
constexpr uint32_t kCloseFile = 0x82123650;
constexpr uint32_t kEntries = 8, kEntrySize = 0x6C, kEntryCount = 1736;
constexpr int kMaxEntries = 16, kSlots = 6;
constexpr uint32_t kState = 36, kFile = 60, kLoaded = 84;
constexpr uint32_t kNone = 0xFFFFFFFFu;

using Key = std::array<uint32_t, 8>;

std::mutex g_mutex;               // g_root, g_available, g_scanned, g_logged
std::filesystem::path g_root;
std::array<int, kLanguages> g_available{};
std::chrono::steady_clock::time_point g_scanned{};
bool g_scanned_once = false;
std::set<std::string> g_logged;   // banks already logged (once per bank and voice)

std::atomic<int> g_chosen{-2};    // -2: not read yet
std::once_flag g_callback_once;

// What the game registered for each bank, and with which voice it is
// registered now, so a change can register it again (game thread only).
struct Original {
  std::string path;
  uint32_t slot;
  uint32_t archive;
};
struct Tracked {
  uint32_t table;
  Key key;
  std::vector<Original> regs;
  int voice;   // the voice its files were chosen for (-1 the game's own)
};
std::vector<Tracked> g_tracked;
std::recursive_mutex g_bank_mutex;         // g_tracked and the bank manager's entries while we change them

int ReadChosen() { return LanguageFromCode(std::string(REXCVAR_GET(cod3_voice_language))); }

std::filesystem::path Root() {
  std::lock_guard lock(g_mutex);
  return g_root;
}

bool SameName(std::string_view a, std::string_view b) { return detail::Lower(a) == detail::Lower(b); }

const char* VoiceName(int voice) { return voice < 0 ? "the game's own" : kNames[voice]; }

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

// --- the bank manager's entries ------------------------------------------------------
Key ReadKey(uint8_t* base, uint32_t addr) {
  Key k{};
  for (int w = 0; w < 8; ++w) k[w] = ReadU32(base, addr + 4 * w);
  return k;
}

uint32_t FindEntry(uint8_t* base, uint32_t table, const Key& key) {
  const int n = ReadS32(base, table + kEntryCount);
  for (int i = 0; i < n && i < kMaxEntries; ++i) {
    const uint32_t e = table + kEntries + uint32_t(i) * kEntrySize;
    if (ReadKey(base, e) == key) return e;
  }
  return 0;
}

bool AnyState(uint8_t* base, uint32_t e, uint32_t state) {
  for (int s = 0; s < kSlots; ++s)
    if (ReadU32(base, e + kState + 4 * s) == state) return true;
  return false;
}

// Nothing loaded, loading or being released in any slot.
bool Idle(uint8_t* base, uint32_t e) {
  for (int s = 0; s < kSlots; ++s)
    if (ReadU32(base, e + kState + 4 * s) != 0 || ReadU32(base, e + kLoaded + 4 * s) != kNone) return false;
  return true;
}

Tracked* FindTracked(uint32_t table, const Key& key) {
  for (Tracked& t : g_tracked)
    if (t.table == table && t.key == key) return &t;
  return nullptr;
}

std::string BankName(const Tracked& t) {
  if (t.regs.empty()) return "a bank";
  const std::string& p = t.regs.front().path;
  const size_t cut = p.find_last_of('\\');
  return cut == std::string::npos ? p : p.substr(cut + 1);
}

// A Call frame with the key copied onto its stack; returns the key's address.
uint32_t PushKey(cod3::guest::Call& call, uint8_t* base, const Key& key) {
  PPCContext& c = call.ctx();
  c.r1.u32 = (c.r1.u32 - 0x40) & ~0xFu;
  const uint32_t addr = c.r1.u32 + 0x10;
  for (int w = 0; w < 8; ++w) WriteU32(base, addr + 4 * w, key[w]);
  return addr;
}

// The game's own registration (not the hook), on a frame of its own.
void RegisterRaw(PPCContext& parent, uint8_t* base, uint32_t table, const Key& key, const Registration& r,
                 uint32_t archive);

// Closes the files of every slot (all of them idle) so that only what is
// registered next stays: a bank registered for another voice may have used
// other slots (a French sounds + speech pair, then English in one).
void ClearFiles(PPCContext& parent, uint8_t* base, uint32_t e) {
  for (int s = 0; s < kSlots; ++s) {
    const uint32_t f = ReadU32(base, e + kFile + 4 * s);
    if (f == kNone) continue;
    cod3::guest::Call call(parent, base);
    call.Invoke(kCloseFile, {f});
    WriteU32(base, e + kFile + 4 * s, kNone);
  }
}

// The files a bank's registrations come to for a voice.
std::vector<Registration> Files(uint8_t* base, const Tracked& t, int voice) {
  const int game_language = ReadS32(base, kGameLanguage);
  std::vector<Registration> files;
  for (const Original& o : t.regs)
    for (Registration& r : PlanBank(o.path, o.slot, game_language, voice, Exists)) {
      r.path = detail::Lower(r.path);
      files.push_back(std::move(r));
    }
  return files;
}

// Registers a bank's registrations as the voice wants them.
void RegisterAll(PPCContext& parent, uint8_t* base, const Tracked& t, int voice) {
  const int game_language = ReadS32(base, kGameLanguage);
  for (const Original& o : t.regs)
    for (const Registration& r : PlanBank(o.path, o.slot, game_language, voice, Exists))
      RegisterRaw(parent, base, t.table, t.key, r, o.archive);
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
      REXLOG_INFO("cod3: voices: {} (the films at once, the soldiers from the next level load)",
                  VoiceName(now));
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
      "from another disc. The game's text stays as it is. The films change at once; the soldiers "
      "when the next level loads (starting or loading a mission). Installed: " +
      (installed.empty() ? std::string("none found") : installed) + ".";
  Help(help.c_str());
  if (pick == -2 || pick == chosen) return false;
  rex::cvar::SetFlagByName("cod3_voice_language", pick < 0 ? "disc" : kCodes[pick]);
  g_chosen.store(pick, std::memory_order_relaxed);   // also when the change callback isn't there yet
  return true;
}

namespace {

void RegisterRaw(PPCContext& parent, uint8_t* base, uint32_t table, const Key& key, const Registration& r,
                 uint32_t archive) {
  cod3::guest::Call call(parent, base);
  const uint32_t path = call.PushString(GameSpelling(r.path));
  const uint32_t key_addr = PushKey(call, base, key);
  PPCContext& c = call.ctx();
  const uint32_t saved_r1 = c.r1.u32;
  c.r1.u32 -= 0x70;   // the callee's frame (back chain and LR save)
  c.r3.u64 = table;
  c.r4.u64 = key_addr;
  c.r5.u64 = path;
  c.r6.u64 = r.slot;
  c.r7.u64 = archive;
  __imp__sub_82478B80(c, base);
  c.r1.u32 = saved_r1;
}

// A level load (0x824631D0 below): every bank still registered for another
// voice than the chosen one is released if loaded, registered again for the
// chosen voice and loaded again. Banks of archives the load closed are gone by
// now, and those it opened were registered with the chosen voice; what is
// left are banks of archives that stayed open (the same level again, the
// battle chatter), which would keep their old voice until the game restarted.
void RefreshBanks(PPCContext& ctx, uint8_t* base) {
  std::lock_guard lock(g_bank_mutex);
  const int voice = Chosen();
  const bool debug = REXCVAR_GET(cod3_debug_voices);
  // Banks the game no longer has (their archive closed) are forgotten.
  std::erase_if(g_tracked, [&](const Tracked& t) { return FindEntry(base, t.table, t.key) == 0; });
  for (Tracked& t : g_tracked) {
    if (t.voice == voice) continue;
    const uint32_t e = FindEntry(base, t.table, t.key);
    if (!e) continue;
    if (Files(base, t, voice) == Files(base, t, t.voice)) {
      t.voice = voice;   // the same files either way (a bank without speech, or no files for the voice)
      continue;
    }
    if (AnyState(base, e, 1) || AnyState(base, e, 3)) {
      REXLOG_INFO("cod3: voices: {} is loading or being released: keeps {} for this level", BankName(t),
                  VoiceName(t.voice));
      continue;
    }
    const bool was_loaded = AnyState(base, e, 2);
    if (was_loaded) {
      cod3::guest::Call call(ctx, base);
      const uint32_t key_addr = PushKey(call, base, t.key);
      call.Invoke(kUnloadBank, {t.table, key_addr, 0});   // waits for the release
      for (int i = 0; i < 64 && !Idle(base, e); ++i) call.Invoke(kUpdateBanks, {t.table});
      if (!Idle(base, e)) {
        REXLOG_WARN("cod3: voices: {} wasn't released: keeps {} for this level", BankName(t),
                    VoiceName(t.voice));
        continue;
      }
    }
    ClearFiles(ctx, base, e);
    RegisterAll(ctx, base, t, voice);
    if (was_loaded) {
      cod3::guest::Call call(ctx, base);
      const uint32_t key_addr = PushKey(call, base, t.key);
      call.Invoke(kLoadBank, {t.table, key_addr, 1});   // as the game loads it (in the background)
    }
    REXLOG_INFO("cod3: voices: {} now {} (was {}){}", BankName(t), VoiceName(voice), VoiceName(t.voice),
                was_loaded ? ", loaded again" : "");
    if (debug) {
      const int game_language = ReadS32(base, kGameLanguage);
      for (const Original& o : t.regs)
        REXLOG_INFO("cod3: voices:   {} (slot {}) -> {}", o.path, o.slot,
                    Describe(PlanBank(o.path, o.slot, game_language, voice, Exists)));
    }
    t.voice = voice;
  }
}

}  // namespace

}  // namespace cod3::voice

// A sound bank of a level or of battle chatter: r3 the bank table, r4 the bank's key
// (its name hashed; every language of a bank shares it), r5 its path, r6 the
// language slot, r7 the archive. Registers the banks of the chosen voices in
// its place when assets/ has them (game/voice_paths.h), and remembers what the
// game asked for, so that a later change can register the bank again.
REX_HOOK_RAW(sub_82478B80) {
  using namespace cod3::voice;
  const int voice = Chosen();
  const uint32_t table = ctx.r3.u32;
  const Key key = ReadKey(base, ctx.r4.u32);
  const std::string path = cod3::guest::ReadString(base, ctx.r5.u32, 260);
  const uint32_t slot = ctx.r6.u32;
  const uint32_t archive = ctx.r7.u32;
  const int game_language = ReadS32(base, kGameLanguage);
  std::lock_guard lock(g_bank_mutex);

  // Remember the registration. A bank the game doesn't have yet starts afresh.
  const uint32_t entry = FindEntry(base, table, key);
  Tracked* t = FindTracked(table, key);
  if (!t) {
    g_tracked.push_back({table, key, {}, voice});
    t = &g_tracked.back();
  } else if (!entry) {
    t->regs.clear();
    t->voice = voice;
  }
  bool known = false;
  for (const Original& o : t->regs) known |= SameName(o.path, path) && o.slot == slot && o.archive == archive;
  if (!known) t->regs.push_back({path, slot, archive});

  if (REXCVAR_GET(cod3_debug_voices))
    REXLOG_INFO("cod3: voices: the game registers {} (slot {}, bank {:08X}, {})", path, slot, key[0],
                entry ? "known" : "new");

  // The bank is registered for another voice and nothing of it is loaded:
  // its files are registered again from the start, this one included.
  if (entry && t->voice != voice && Idle(base, entry)) {
    if (Files(base, *t, voice) != Files(base, *t, t->voice)) {
      ClearFiles(ctx, base, entry);
      RegisterAll(ctx, base, *t, voice);
      t->voice = voice;
      return;
    }
    t->voice = voice;
  }

  if (voice < 0) {
    __imp__sub_82478B80(ctx, base);
    return;
  }
  const auto plan = PlanBank(path, slot, game_language, voice, Exists);
  if (plan.size() == 1 && plan[0].path == path && plan[0].slot == slot) {
    if (voice != game_language && !Exists(path))
      LogOnce(voice, path, path + " isn't in assets/ as the game names it: kept");
    __imp__sub_82478B80(ctx, base);
    return;
  }
  LogOnce(voice, path, path + " (slot " + std::to_string(slot) + ") -> " + Describe(plan));
  const PPCRegister key_reg = ctx.r4;
  for (const Registration& r : plan) {
    // Each call on its own registers (the bank's file is opened by path; the
    // key stays the game's, so the bank's languages stay one entry).
    cod3::guest::Call call(ctx, base);
    const uint32_t p = SameName(r.path, path) ? ctx.r5.u32 : call.PushString(GameSpelling(r.path));
    PPCContext& c = call.ctx();
    const uint32_t saved_r1 = c.r1.u32;
    c.r1.u32 -= 0x70;   // the callee's frame (back chain and LR save)
    c.r3.u64 = table;
    c.r4 = key_reg;
    c.r5.u64 = p;
    c.r6.u64 = r.slot;
    c.r7.u64 = archive;
    __imp__sub_82478B80(c, base);
    c.r1.u32 = saved_r1;
  }
}

// SV_SpawnServer opens the level's own archive (sp\<map>\<map>.cod) with
// 0x824631D0 (archive list, type 3, path), after the archive list was brought
// up to date (0x82462FB0) and before the battle chatter banks are loaded:
// banks still registered for another voice are registered again here.
REX_EXTERN(__imp__sub_824631D0);
REX_HOOK_RAW(sub_824631D0) {
  __imp__sub_824631D0(ctx, base);
  cod3::voice::RefreshBanks(ctx, base);
}

// After 0x824EED20's XGetLanguage call (0x824EEDCC): r3 is the console
// language it picks a movie's sound track by. Movies without the chosen
// language fall back as the game's own code does (English first).
void Cod3VoiceMovieLanguage(PPCRegister& r3) {
  const int voice = cod3::voice::Chosen();
  if (voice >= 0) r3.u64 = cod3::voice::ConsoleLanguageFor(voice);
}
