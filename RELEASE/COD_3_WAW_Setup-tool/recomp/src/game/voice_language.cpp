// cod3 - the language the game's voices speak (see voice_language.h).

#include "game/voice_language.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
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

#include "game/engine.h"  // [acct2-decomp] QueueGuestTask: the mid-level swap runs on the game thread
#include "game/guest.h"
#include "game/voice_paths.h"

REXCVAR_DEFINE_STRING(cod3_voice_language, "disc", "cod3/Audio",
                      "Voices in the campaign: disc (the game's own) or en, de, fr, es, it, from the "
                      "voice files of another disc that the setup tool copied into assets/; the text "
                      "stays the game's own. The films change at once; the soldiers as soon as nothing "
                      "of their sound bank is playing, else at the next level load")
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

// [acct2-decomp] For a change in the middle of a level (default.xex, read 2026-11-04;
// dev/agents/acct2-decomp/NOTES.md "voices mid-level"):
//   0x82114B58 nslUnloadBank (bank): frees a loaded bank at once (a queued one is
//     cancelled) and advances its slot's generation. It does NOT stop the sounds
//     playing from it, and resident waves are played from the bank's memory
//     (FMOD_OPENMEMORY_POINT), so a bank is only released while nothing of it plays.
//   The sound instances: a handle pool at 0x8290C99C (0x821249B8 first handle,
//     0x821249D8 next, 0x821246E8 handle -> index or -1), 0x148-byte instances
//     from the pointer at 0x8290C9C8; an instance's sound is at +0x110:
//     the bank's handle (generation << 16 | 0xFFFF) with the wave index below.
//   The voices (what FMOD plays; 0x8211B7D0 hands them out, 0x8211B8D8 frees one
//     at once or marks it to stop at the next update): 0x140 bytes each from the
//     pointer at 0x8290CC0C, their number at 0x825A16AC; +0x108 a state byte
//     (0 free), +0x110 the sound, as an instance's. A voice can outlive its
//     instance by one update, so both are counted.
//   0x82493F00, the bank manager's update, runs every frame from CL_Frame: it
//     turns a slot that is releasing (3) into nothing (0) once NSL has let the
//     bank go, and a loading one (1) into loaded (2).
// A bank file's resident data size is the u32 (little-endian) at +0x30 of its
// NSLB header (docs/COD3_AUDIO.md).
constexpr uint32_t kNslUnloadBank = 0x82114B58;
// NSL holds 8 banks at most (the count at 0x825A16A8; 0x82118E80 counts those in
// use). A slot the game can't get a bank for is closed and stays silent, so a
// change that needs more banks than are free waits for a level load.
constexpr uint32_t kNslBankSlots = 0x825A16A8, kNslLoadedBankCount = 0x82118E80;
constexpr uint32_t kInstancePool = 0x8290C99C;
constexpr uint32_t kInstanceArray = 0x8290C9C8;
constexpr uint32_t kPoolFirst = 0x821249B8, kPoolNext = 0x821249D8, kPoolIndex = 0x821246E8;
constexpr uint32_t kInstanceSize = 0x148, kInstanceSound = 0x110;
constexpr uint32_t kVoiceArray = 0x8290CC0C, kVoiceCount = 0x825A16AC;
constexpr uint32_t kVoiceSize = 0x140, kVoiceState = 0x108, kVoiceSound = 0x110;
constexpr uint32_t kMaxVoices = 1024;      // NSL gives 247 and a few more; a bound in case it is damaged
constexpr int kPoolGuard = 4096;           // the pool has 512 instances; a bound in case it is damaged
constexpr int kQuietFrames = 2;            // a voice released in the last frame is stopped by the next update
constexpr auto kEveryFrameFor = std::chrono::seconds(30);   // then once a second
constexpr auto kReleaseTimeout = std::chrono::seconds(5);
constexpr uint32_t kStateIdle = 0, kStateLoading = 1, kStateLoaded = 2, kStateReleasing = 3;

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
// [acct2-decomp] A change in the middle of a level, per bank (game thread only).
enum class SwapPhase { kIdle, kWaiting, kReleasing, kDeferred };
struct Swap {
  SwapPhase phase = SwapPhase::kIdle;
  int target = -2;                   // the voice being swapped to
  int quiet = 0;                     // frames in a row with nothing of the changed slots playing
  std::chrono::steady_clock::time_point since{}, next_try{};
  std::vector<uint32_t> slots;       // the slots whose files change
  bool was_loaded = false;           // load the bank again when its new files are registered
  bool told = false;                 // "waits" logged
};
struct Tracked {
  uint32_t table;
  Key key;
  std::vector<Original> regs;
  int voice;   // the voice its files were chosen for (-1 the game's own)
  Swap swap{};
  uint64_t peak = 0;   // [acct2-decomp] the most resident data it has held since the level loaded
};
std::vector<Tracked> g_tracked;
std::recursive_mutex g_bank_mutex;         // g_tracked and the bank manager's entries while we change them
std::atomic<bool> g_swap_queued{false};    // a SwapTask is queued for the next frame
std::atomic<int> g_waiting{0};             // banks still waiting to change (for the settings row)

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

// --- [acct2-decomp] a change in the middle of a level --------------------------------

// Queues SwapTask for the next game frame (once).
void RequestSwap();

// The slots whose files differ between two voices' plans for a bank.
std::vector<uint32_t> ChangedSlots(const std::vector<Registration>& a, const std::vector<Registration>& b) {
  std::map<uint32_t, std::vector<std::string>> pa, pb;
  for (const auto& r : a) pa[r.slot].push_back(r.path);
  for (const auto& r : b) pb[r.slot].push_back(r.path);
  for (auto* m : {&pa, &pb})
    for (auto& [slot, paths] : *m) std::sort(paths.begin(), paths.end());
  std::vector<uint32_t> slots;
  for (uint32_t s = 0; s < uint32_t(kSlots); ++s) {
    const auto ia = pa.find(s), ib = pb.find(s);
    const bool has_a = ia != pa.end(), has_b = ib != pb.end();
    if (has_a != has_b || (has_a && ia->second != ib->second)) slots.push_back(s);
  }
  return slots;
}

bool InSlots(const std::vector<uint32_t>& slots, uint32_t s) {
  return std::find(slots.begin(), slots.end(), s) != slots.end();
}

// The resident data of the bank files of the given slots that the game loads
// (slot 5 and the game's language), from their NSLB headers in assets/; nothing
// when a file can't be read (the change then waits for a level load).
std::optional<uint64_t> ResidentBytes(const std::vector<Registration>& files, const std::vector<uint32_t>& slots,
                                      int game_language) {
  uint64_t total = 0;
  for (const auto& r : files) {
    if (!InSlots(slots, r.slot)) continue;
    if (r.slot != kEveryLanguage && int(r.slot) != game_language) continue;
    const auto p = Find(r.path);
    if (!p) return std::nullopt;
    std::ifstream f(*p, std::ios::binary);
    unsigned char h[0x34]{};
    if (!f.read(reinterpret_cast<char*>(h), sizeof h) || std::memcmp(h, "NSLB", 4) != 0) return std::nullopt;
    total += uint64_t(h[0x30]) | uint64_t(h[0x31]) << 8 | uint64_t(h[0x32]) << 16 | uint64_t(h[0x33]) << 24;
  }
  return total;
}

bool OfBanks(uint32_t sound, const std::vector<uint32_t>& banks) {
  return std::find(banks.begin(), banks.end(), sound | 0xFFFFu) != banks.end();
}

// How many sound instances and voices play from (or wait on) the given NSL banks.
int CountPlaying(PPCContext& ctx, uint8_t* base, const std::vector<uint32_t>& banks) {
  if (banks.empty()) return 0;
  int n = 0;
  if (const uint32_t array = ReadU32(base, kInstanceArray); array != 0) {
    cod3::guest::Call call(ctx, base);
    uint32_t h = uint32_t(call.Invoke(kPoolFirst, {kInstancePool}));
    for (int guard = 0; h != kNone && guard < kPoolGuard; ++guard) {
      const uint32_t cur = h;
      h = uint32_t(call.Invoke(kPoolNext, {kInstancePool, cur}));
      const int32_t index = int32_t(uint32_t(call.Invoke(kPoolIndex, {kInstancePool, cur})));
      if (index < 0 || index >= kPoolGuard) continue;
      n += OfBanks(ReadU32(base, array + uint32_t(index) * kInstanceSize + kInstanceSound), banks) ? 1 : 0;
    }
  }
  if (const uint32_t voices = ReadU32(base, kVoiceArray); voices != 0) {
    const uint32_t count = std::min(ReadU32(base, kVoiceCount), kMaxVoices);
    for (uint32_t i = 0; i < count; ++i) {
      const uint32_t v = voices + i * kVoiceSize;
      if (cod3::guest::ReadU8(base, v + kVoiceState) != 0 && OfBanks(ReadU32(base, v + kVoiceSound), banks)) ++n;
    }
  }
  return n;
}

// Releases the given slots of a bank as the game's 0x82494C70 does for each
// loaded slot (NSL frees it, the file is closed, the slot is "releasing" until
// the game's per-frame update clears it), and closes the files of the others.
void ReleaseSlots(PPCContext& ctx, uint8_t* base, uint32_t e, const std::vector<uint32_t>& slots) {
  for (uint32_t s : slots) {
    const uint32_t state = ReadU32(base, e + kState + 4 * s);
    const uint32_t file = ReadU32(base, e + kFile + 4 * s);
    cod3::guest::Call call(ctx, base);
    if (state == kStateLoaded) call.Invoke(kNslUnloadBank, {ReadU32(base, e + kLoaded + 4 * s)});
    if (file != kNone) call.Invoke(kCloseFile, {file});
    WriteU32(base, e + kFile + 4 * s, kNone);
    if (state == kStateLoaded) WriteU32(base, e + kState + 4 * s, kStateReleasing);
  }
}

// One frame of the change for one bank; true while it still has work to do.
bool SwapBank(PPCContext& ctx, uint8_t* base, Tracked& t, int voice) {
  using clock = std::chrono::steady_clock;
  Swap& sw = t.swap;
  const auto now = clock::now();
  const uint32_t e = FindEntry(base, t.table, t.key);
  if (!e) return false;   // the game no longer has it; RefreshBanks forgets it
  const int game_language = ReadS32(base, kGameLanguage);

  if (sw.phase == SwapPhase::kReleasing) {
    // Waiting for the game's update to finish releasing the old files.
    bool released = true;
    for (uint32_t s : sw.slots)
      released &= ReadU32(base, e + kState + 4 * s) == kStateIdle && ReadU32(base, e + kLoaded + 4 * s) == kNone;
    if (!released) {
      if (now - sw.since > kReleaseTimeout) {
        REXLOG_WARN("cod3: voices: {} wasn't released in time: it changes at the next level load", BankName(t));
        sw.phase = SwapPhase::kDeferred;
        return false;
      }
      return true;
    }
    bool untouched = true;   // nobody registered a file in these slots since they were released
    for (uint32_t s : sw.slots) untouched &= ReadU32(base, e + kFile + 4 * s) == kNone;
    if (!untouched) {
      sw = Swap{};
      return t.voice != voice;
    }
    for (const Original& o : t.regs)
      for (const Registration& r : PlanBank(o.path, o.slot, game_language, sw.target, Exists))
        if (InSlots(sw.slots, r.slot)) RegisterRaw(ctx, base, t.table, t.key, r, o.archive);
    if (sw.was_loaded) {
      cod3::guest::Call call(ctx, base);
      const uint32_t key_addr = PushKey(call, base, t.key);
      call.Invoke(kLoadBank, {t.table, key_addr, 1});   // in the background, as the game loads it
    }
    REXLOG_INFO("cod3: voices: {} now {} (was {}), in the middle of the level", BankName(t), VoiceName(sw.target),
                VoiceName(t.voice));
    t.voice = sw.target;
    sw = Swap{};
    return t.voice != voice;   // changed again meanwhile: go on
  }

  if (sw.phase == SwapPhase::kDeferred && sw.target == voice) return false;
  if (sw.phase == SwapPhase::kIdle || sw.target != voice) {
    sw = Swap{};
    sw.phase = SwapPhase::kWaiting;
    sw.target = voice;
    sw.since = now;
    const auto old_files = Files(base, t, t.voice), new_files = Files(base, t, voice);
    sw.slots = ChangedSlots(old_files, new_files);
    if (sw.slots.empty()) {
      t.voice = voice;   // the same files either way
      sw = Swap{};
      return false;
    }
    // Sound memory: the new files may not need more than this bank has held since the
    // level loaded (what it frees now, or more it held before a smaller change).
    const auto old_bytes = ResidentBytes(old_files, sw.slots, game_language);
    const auto new_bytes = ResidentBytes(new_files, sw.slots, game_language);
    if (old_bytes) t.peak = std::max(t.peak, *old_bytes);
    if (!old_bytes || !new_bytes || *new_bytes > t.peak) {
      REXLOG_INFO("cod3: voices: {} needs {} of sound memory ({} at most this level): it changes at the next "
                  "level load", BankName(t), new_bytes ? std::to_string(*new_bytes) : std::string("an unknown amount"),
                  old_bytes ? std::to_string(t.peak) : std::string("unknown"));
      sw.phase = SwapPhase::kDeferred;
      return false;
    }
    // NSL's banks: a whole bank replaced by a split pair needs one more.
    const auto loads = [&](const std::vector<Registration>& files) {
      int n = 0;
      for (uint32_t s : sw.slots) {
        if (s != kEveryLanguage && int(s) != game_language) continue;
        bool any = false;
        for (const auto& r : files) any |= r.slot == s;
        n += any ? 1 : 0;
      }
      return n;
    };
    if (const int extra = loads(new_files) - loads(old_files); extra > 0) {
      cod3::guest::Call call(ctx, base);
      const int in_use = int(uint32_t(call.Invoke(kNslLoadedBankCount)));
      const int slots = int(ReadU32(base, kNslBankSlots));
      if (in_use < 0 || slots <= 0 || slots - in_use < extra) {
        REXLOG_INFO("cod3: voices: {} needs {} more of the sound system's {} banks ({} in use): it changes at the "
                    "next level load", BankName(t), extra, slots, in_use);
        sw.phase = SwapPhase::kDeferred;
        return false;
      }
    }
  }
  if (sw.phase != SwapPhase::kWaiting) return false;
  if (now < sw.next_try) return true;

  // Busy: a slot being loaded or released waits; so does any sound of the slots
  // that change, until none has played for kQuietFrames frames in a row.
  std::vector<uint32_t> banks;
  bool busy = false, loaded = false;
  for (uint32_t s : sw.slots) {
    const uint32_t state = ReadU32(base, e + kState + 4 * s);
    busy |= state == kStateLoading || state == kStateReleasing;
    if (state == kStateLoaded) banks.push_back(ReadU32(base, e + kLoaded + 4 * s));
  }
  for (int s = 0; s < kSlots; ++s) loaded |= ReadU32(base, e + kState + 4 * s) == kStateLoaded;
  const int playing = busy ? 0 : CountPlaying(ctx, base, banks);
  if (busy || playing > 0) {
    sw.quiet = 0;
    // After a while, look once a second (a quiet look is followed by one the next frame).
    if (now - sw.since > kEveryFrameFor) sw.next_try = now + std::chrono::seconds(1);
    if (!sw.told && now - sw.since > std::chrono::seconds(2)) {
      sw.told = true;
      REXLOG_INFO("cod3: voices: {} is playing ({} sounds): it changes to {} when it's quiet or at the next "
                  "level load", BankName(t), playing, VoiceName(voice));
    }
    return true;
  }
  if (++sw.quiet < kQuietFrames) return true;
  sw.was_loaded = loaded;
  ReleaseSlots(ctx, base, e, sw.slots);
  sw.phase = SwapPhase::kReleasing;
  sw.since = now;
  return true;
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
      REXLOG_INFO("cod3: voices: {} (the films at once, the soldiers as soon as their bank is quiet)",
                  VoiceName(now));
      RequestSwap();   // [acct2-decomp] in the middle of a level too
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
  // [acct2-decomp] the soldiers change in the middle of a level too (SwapTask)
  const std::string help =
      "The language the soldiers speak in the campaign and its films. Another disc's voices "
      "(German, or French, Spanish and Italian) come from the setup tool: Play page > Add voices "
      "from another disc. The game's text stays as it is. The films change at once. The soldiers "
      "change as soon as nothing of their sound bank is playing (battle chatter within seconds); "
      "a bank that keeps playing, such as a level's own sounds on the English disc, changes when "
      "the next level loads. Nothing that is playing is cut off. Installed: " +
      (installed.empty() ? std::string("none found") : installed) + ".";
  Help(help.c_str());
  if (const int waiting = g_waiting.load(); waiting > 0) {
    const std::string w = std::to_string(waiting) + (waiting == 1 ? " sound bank still has" : " sound banks still have") +
                          " the old voices (playing, or waiting for the next level load).";
    Help(w.c_str());
  }
  if (pick == -2 || pick == chosen) return false;
  rex::cvar::SetFlagByName("cod3_voice_language", pick < 0 ? "disc" : kCodes[pick]);
  g_chosen.store(pick, std::memory_order_relaxed);   // also when the change callback isn't there yet
  RequestSwap();   // [acct2-decomp]
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
  for (Tracked& t : g_tracked) t.peak = 0;   // [acct2-decomp] a new level: memory measured afresh
  for (Tracked& t : g_tracked) {
    if (t.voice == voice) continue;
    const uint32_t e = FindEntry(base, t.table, t.key);
    if (!e) continue;
    if (Files(base, t, voice) == Files(base, t, t.voice)) {
      t.voice = voice;   // the same files either way (a bank without speech, or no files for the voice)
      t.swap = Swap{};
      continue;
    }
    if (AnyState(base, e, 1) || AnyState(base, e, 3)) {
      // [acct2-decomp] a mid-level change releasing it finishes it (SwapTask)
      if (t.swap.phase != SwapPhase::kReleasing)
        REXLOG_INFO("cod3: voices: {} is loading or being released: keeps {} for this level", BankName(t),
                    VoiceName(t.voice));
      continue;
    }
    // [acct2-decomp] released by a mid-level change that hadn't registered it again yet
    const bool was_loaded = AnyState(base, e, 2) || (t.swap.phase == SwapPhase::kReleasing && t.swap.was_loaded);
    t.swap = Swap{};
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
  // [acct2-decomp] what still waits (a bank that was loading or releasing), for the settings row
  int waiting = 0;
  for (Tracked& t : g_tracked) {
    if (t.voice == voice) continue;
    ++waiting;
    if (t.swap.phase == SwapPhase::kDeferred) t.swap = Swap{};   // the level load was its chance; try again
  }
  g_waiting.store(waiting);
  if (waiting) RequestSwap();
}

// [acct2-decomp] Each frame while a change is unfinished: every bank registered
// for another voice moves on one step (SwapBank). Runs on the game's main thread
// at the start of the frame (Engine::OnGuestFrame, before Cbuf_Execute).
void SwapTask(PPCContext& ctx, uint8_t* base) {
  g_swap_queued.store(false);
  bool more = false;
  int waiting = 0;
  {
    std::lock_guard lock(g_bank_mutex);
    const int voice = Chosen();
    for (Tracked& t : g_tracked) {
      if (t.voice == voice && t.swap.phase != SwapPhase::kReleasing) continue;
      const bool busy = SwapBank(ctx, base, t, voice);
      more |= busy;
      waiting += t.voice != voice ? 1 : 0;
    }
  }
  g_waiting.store(waiting);
  if (more) RequestSwap();
}

void RequestSwap() {
  if (!g_swap_queued.exchange(true)) cod3::Engine::Get().QueueGuestTask(SwapTask);
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
    t->swap = Swap{};   // [acct2-decomp]
    t->peak = 0;
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
    t->swap = Swap{};   // [acct2-decomp] the game registers it again: no mid-level change left to finish
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
