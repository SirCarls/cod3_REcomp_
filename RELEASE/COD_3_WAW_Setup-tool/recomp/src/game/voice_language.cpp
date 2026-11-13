// cod3 - the language the game's voices speak (see voice_language.h).

#include "game/voice_language.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
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
                      "Voices: disc (the game's own) or en, de, fr, es, it, from the voice files of "
                      "another disc that the setup tool copied into assets/; the text has its own setting. "
                      "Campaign: films change from the next film; the soldiers within seconds (a level's "
                      "own lines by loading the new speech next to its sounds), or at the next level load "
                      "when that doesn't fit. Multiplayer: from the next map")
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
#if !defined(COD3_MP)
constexpr uint32_t kGameLanguage = 0x82A2A150;
#else
// [acct2-ocr] 2026-11-07 cod3mp (codmp_xenonf.xex): its load (0x824AE270) reads the slot from here
// (lis r11,-32067; lwz r30,-16860(r11)), as the campaign's 0x82494E90 reads 0x82A2A150.
constexpr uint32_t kGameLanguage = 0x82BCBE24;
#endif
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
#if !defined(COD3_MP)
constexpr uint32_t kLoadBank = 0x82494E90;
constexpr uint32_t kUnloadBank = 0x82494C70;
constexpr uint32_t kUpdateBanks = 0x82493F00;
constexpr uint32_t kCloseFile = 0x82123650;
#else
// [acct2-ocr] 2026-11-07 cod3mp's bank manager is the same code (its registration 0x824964E8, the same entries:
// +1736 the count, 0x6C each from +8, states +36, files +60, loaded +84). Read on the PC: 0x824AE270 (mgr, key)
// loads slot 5 and the language slot (no async argument; the extra one is ignored), 0x824AE0B8 (mgr, key, async)
// releases (AudioBankMgr's vfunc 1 calls it where the campaign's calls 0x82494C70; the same 72 instructions),
// 0x824AD6B8 (mgr) the update it calls, 0x82135980 (handle) the file close it calls.
constexpr uint32_t kLoadBank = 0x824AE270;
constexpr uint32_t kUnloadBank = 0x824AE0B8;
constexpr uint32_t kUpdateBanks = 0x824AD6B8;
constexpr uint32_t kCloseFile = 0x82135980;
#endif
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
#if !defined(COD3_MP)
constexpr uint32_t kNslUnloadBank = 0x82114B58;
#else
// [acct2-ocr] 2026-11-07 cod3mp changes voices at the next map only (RequestSwap): the addresses of the change in
// the middle of a level (the sound instances, NSL's banks and pool) are the campaign's alone, never read there.
constexpr uint32_t kNslUnloadBank = 0;
#endif
// NSL holds 8 banks at most (the count at 0x825A16A8; 0x82118E80 counts those in
// use). A slot the game can't get a bank for is closed and stays silent, so a
// change that needs more banks than are free waits for a level load.
#if !defined(COD3_MP)
constexpr uint32_t kNslBankSlots = 0x825A16A8, kNslLoadedBankCount = 0x82118E80;
constexpr uint32_t kInstancePool = 0x8290C99C;
constexpr uint32_t kInstanceArray = 0x8290C9C8;
constexpr uint32_t kPoolFirst = 0x821249B8, kPoolNext = 0x821249D8, kPoolIndex = 0x821246E8;
#else
constexpr uint32_t kNslBankSlots = 0, kNslLoadedBankCount = 0, kInstancePool = 0, kInstanceArray = 0;
constexpr uint32_t kPoolFirst = 0, kPoolNext = 0, kPoolIndex = 0;
#endif
constexpr uint32_t kInstanceSize = 0x148, kInstanceSound = 0x110;
#if !defined(COD3_MP)
constexpr uint32_t kVoiceArray = 0x8290CC0C, kVoiceCount = 0x825A16AC;
#else
constexpr uint32_t kVoiceArray = 0, kVoiceCount = 0;
#endif
constexpr uint32_t kVoiceSize = 0x140, kVoiceState = 0x108, kVoiceSound = 0x110;
constexpr uint32_t kMaxVoices = 1024;      // NSL gives 247 and a few more; a bound in case it is damaged
constexpr int kPoolGuard = 4096;           // the pool has 512 instances; a bound in case it is damaged
constexpr int kQuietFrames = 2;            // a voice released in the last frame is stopped by the next update
constexpr auto kEveryFrameFor = std::chrono::seconds(30);   // then once a second
constexpr auto kReleaseTimeout = std::chrono::seconds(5);
constexpr uint32_t kStateIdle = 0, kStateLoading = 1, kStateLoaded = 2, kStateReleasing = 3;
// [acct2-decomp] 2026-11-04 13:35. Where a bank's resident data lives (default.xex, read with
// wip/fdis.py): NSL's bank pool, a fixed block that hands out space from both ends, like two
// stacks, with one table of 64 sizes for both:
//   0x8290CF68 the pool's start, 0x8290CF6C its size, 0x825A184C the granularity;
//   0x8290CE68 the table (u32 each: the size, bit 31 = freed), 0x825A1848 the last entry from
//     the bottom (-1 none), 0x825A1844 the first from the top (64 none);
//   0x8211CB00 (size, flags) takes the size rounded up to the granularity off the bottom (or off
//     the top with flag 1) when the gap between the two is big enough and an entry is free, else
//     the load fails (the slot stays silent); the loader (0x8211CDA8) first rounds the bank's
//     resident size up to 4 KB;
//   0x8211CBE8 (address) only marks its entry freed; 0x8211C9D8 then drops the freed entries at
//     the two ends, so space comes back only from an end, and stores the gap at 0x8290CF70.
//   A loaded bank: NSL's slot (handle >> 16) % 0x825A16A8 of the 0x34-byte slots at 0x8290CA8C,
//     +0 its handle, +24 its header; the header's +96 is its resident data (byte 6 bit 7 set:
//     another kind of bank, not in the pool).
// So a bank changed in the middle of a level fits when the gap, after its old data is freed the
// way the game frees it, holds its new files; the other changes of the same moment are counted
// too, until their loads are done.
#if !defined(COD3_MP)
constexpr uint32_t kPoolStart = 0x8290CF68, kPoolSize = 0x8290CF6C, kPoolGrain = 0x825A184C;
constexpr uint32_t kPoolTable = 0x8290CE68, kPoolBottom = 0x825A1848, kPoolTop = 0x825A1844;
constexpr uint32_t kNslSlots = 0x8290CA8C;
#else
constexpr uint32_t kPoolStart = 0, kPoolSize = 0, kPoolGrain = 0, kPoolTable = 0, kPoolBottom = 0, kPoolTop = 0;
constexpr uint32_t kNslSlots = 0;
#endif
constexpr int kPoolEntries = 64;
constexpr uint32_t kNslSlotSize = 0x34, kNslHeader = 24, kHeaderResident = 96;
// [acct2-ocr] 2026-11-07 (owner 10:32 "the audio switching still seems broken"): a level's own bank (sp\<level>\
// <level>.wbk on the USA disc) always has a sound playing, and another voice's pair (its sounds .wul + speech .wfr)
// doesn't fit the bank pool beside it, so the soldiers kept the old voice until the next level load (run
// acct2-ocr-voiceB-mid: "Blkbrn.wbk is playing (37 sounds)", "doesn't fit ... 17834112 needed, 15146496 free"). Now the
// chosen voice's speech is loaded next to such a bank (an "overlay": a free slot of the same entry) and found first:
//   0x82494D90 (mgr, entry, slot, async)  loads one slot of a bank (0x82494E90 calls it for slot 5 and the game's slot)
//   0x821196E0 nslFindSound (hash)        searches NSL's loaded banks in slot order (the 0x34-byte slots above: +4 the
//                                         state, 3 loaded) and answers the first one's (handle & 0xFFFF0000) | index;
//   0x821195E8 (header, hash)             the index of a sound in one bank, -1 when it has none
// The pool stays the game's (about 36 MB; nslFmodInit 0x8211AD98 puts 0x23D7840 in config+28): a bigger one (56 or
// 80 MB) ended the game at its start (runs acct2-ocr-voiceC-mid, voiceD-56), the guest's physical memory has no
// such block then. So a whole bank of another voice (German, 18 MB for blkbrn) waits for the next level load when
// the level's free bank memory (15 MB in blkbrn) can't hold it; a split pair's speech (1.6 MB) fits.
#if !defined(COD3_MP)
constexpr uint32_t kLoadSlot = 0x82494D90;
constexpr uint32_t kNslFindInBank = 0x821195E8;
#else
constexpr uint32_t kLoadSlot = 0, kNslFindInBank = 0;   // cod3mp: no change in the middle of a match
#endif
constexpr uint32_t kNslSlotState = 4, kNslSlotLoaded = 3;
constexpr auto kOverlayAfter = std::chrono::milliseconds(1500);   // a bank still playing by then gets its speech next to it
constexpr auto kOverlayFirstFor = std::chrono::seconds(10);   // a change waits so long for the voice before's to go (SwapBank)
constexpr int kNoOverlay = -2;

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
  std::chrono::steady_clock::time_point next_overlay{};   // [acct2-ocr] when to try the speech next to it again
  bool overlay_told = false;                              // [acct2-ocr] why it couldn't, logged
  bool held_told = false;                                 // [acct2-ocr] waiting for the voice before's speech, logged
};
// [acct2-ocr] 2026-11-07 A voice's speech loaded in a free slot of a bank that keeps playing (see kLoadSlot); the
// bank's own files stay. nslFindSound looks in it first while that voice is chosen. Let go when quiet, once
// another voice is chosen, and at the next level load (RefreshBanks registers the bank afresh).
struct Overlay {
  int voice = kNoOverlay;
  uint32_t slot = 0;
  std::string path;
  bool releasing = false;
  int quiet = 0;
  std::chrono::steady_clock::time_point since{};
  bool in = false;   // its load is done (cod3_debug_voices: the pool logged then)
};
struct Tracked {
  uint32_t table;
  Key key;
  std::vector<Original> regs;
  int voice;   // the voice its files were chosen for (-1 the game's own)
  Swap swap{};
  uint64_t peak = 0;   // [acct2-decomp] the most resident data it has held since the level loaded
  // [acct2-decomp] Pool space promised to this bank's new files until their load is done.
  uint64_t pending_bytes = 0;
  int pending_entries = 0;
  std::vector<uint32_t> pending_slots;
  Overlay overlay{};   // [acct2-ocr]
};
std::vector<Tracked> g_tracked;
// [acct2-ocr] NSL handles of the overlays of the chosen voice that are loaded: nslFindSound's hook looks in them first.
std::mutex g_prefer_mutex;
std::vector<uint32_t> g_prefer;
std::atomic<int> g_prefer_count{0};
std::atomic<uint32_t> g_prefer_hits{0};
std::atomic<uint32_t> g_prefer_looks{0};   // [acct2-ocr] lookups while one is there (cod3_debug_voices: and from where)
std::array<std::pair<uint32_t, uint32_t>, 8> g_prefer_callers{};   // (return address, count), under g_prefer_mutex
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
    // [acct2-ocr] 2026-11-07 nor a drive ("C:" joined on Windows replaces the folder before it): only names below it
    if (part.find(':') != std::string::npos) return std::nullopt;
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
  // The program's own banks: what the setting changes ([acct2-ocr] cod3mp: mp/, whole banks .wbk/.wde/.wfr).
#if defined(COD3_MP)
  const char* const folder = "mp";
#else
  const char* const folder = "sp";
#endif
  for (std::filesystem::recursive_directory_iterator it(root / folder, ec), end; !ec && it != end;
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

// [acct2-decomp] The resident sizes of the files that would load, one each, as the loader asks
// the pool for them (rounded up to 4 KB here, to the granularity by the pool).
std::optional<std::vector<uint32_t>> ResidentSizes(const std::vector<Registration>& files,
                                                   const std::vector<uint32_t>& slots, int game_language) {
  std::vector<uint32_t> out;
  for (const auto& r : files) {
    if (!InSlots(slots, r.slot)) continue;
    if (r.slot != kEveryLanguage && int(r.slot) != game_language) continue;
    const auto one = ResidentBytes({r}, slots, game_language);
    if (!one || *one > 0x7FFFF000u) return std::nullopt;
    if (*one) out.push_back((uint32_t(*one) + 0xFFFu) & ~0xFFFu);   // no resident data: no space
  }
  return out;
}

// [acct2-decomp] NSL's bank pool as the game keeps it (see kPoolStart).
struct Pool {
  uint32_t start = 0, size = 0, grain = 0;
  int bottom = -1, top = kPoolEntries;
  std::array<uint32_t, kPoolEntries> table{};
};

std::optional<Pool> ReadPool(uint8_t* base) {
  Pool p;
  p.start = ReadU32(base, kPoolStart);
  p.size = ReadU32(base, kPoolSize);
  p.grain = ReadU32(base, kPoolGrain);
  p.bottom = ReadS32(base, kPoolBottom);
  p.top = ReadS32(base, kPoolTop);
  for (int i = 0; i < kPoolEntries; ++i) p.table[i] = ReadU32(base, kPoolTable + 4 * uint32_t(i));
  // Not set up, or not what was read from default.xex: no change in the middle of the level.
  if (p.start == 0 || p.size == 0 || p.size > 0x10000000u || p.grain == 0 || p.grain > 0x100000u) return std::nullopt;
  if (p.bottom < -1 || p.bottom >= kPoolEntries || p.top < 0 || p.top > kPoolEntries || p.bottom >= p.top)
    return std::nullopt;
  uint64_t used = 0;
  for (int i = 0; i <= p.bottom; ++i) used += p.table[i] & 0x7FFFFFFFu;
  for (int i = p.top; i < kPoolEntries; ++i) used += p.table[i] & 0x7FFFFFFFu;
  if (used > p.size) return std::nullopt;
  return p;
}

// 0x8211C9D8 without its stores: drops the freed entries at the two ends, returns the gap.
uint64_t PoolGap(Pool& p) {
  while (p.bottom >= 0 && (p.table[p.bottom] & 0x80000000u)) p.table[p.bottom--] = 0;
  while (p.top < kPoolEntries && (p.table[p.top] & 0x80000000u)) p.table[p.top++] = 0;
  uint64_t low = 0, high = p.size;
  for (int i = 0; i <= p.bottom; ++i) low += p.table[i] & 0x7FFFFFFFu;
  for (int i = p.top; i < kPoolEntries; ++i) high -= p.table[i] & 0x7FFFFFFFu;
  return high > low ? high - low : 0;
}

// 0x8211CBE8: marks the entry at that address freed (nothing when no entry starts there).
void PoolFree(Pool& p, uint32_t address) {
  if (address < p.start || address - p.start >= p.size) return;
  const uint32_t at = address - p.start;
  uint64_t offset = 0;
  for (int i = 0; i <= p.bottom; ++i) {
    if (offset == at) {
      p.table[i] |= 0x80000000u;
      return;
    }
    offset += p.table[i] & 0x7FFFFFFFu;
  }
  uint64_t end = p.size;
  for (int i = kPoolEntries - 1; i >= p.top; --i) {
    end -= p.table[i] & 0x7FFFFFFFu;
    if (end == at) {
      p.table[i] |= 0x80000000u;
      return;
    }
  }
}

// A loaded NSL bank's resident data, 0 when there is none in the pool.
uint32_t ResidentAddress(uint8_t* base, uint32_t handle) {
  const uint32_t count = ReadU32(base, kNslBankSlots);
  const uint32_t slots = ReadU32(base, kNslSlots);
  if (count == 0 || count > 64 || slots == 0) return 0;
  const uint32_t slot = slots + ((handle >> 16) % count) * kNslSlotSize;
  if (ReadU32(base, slot) != handle) return 0;
  const uint32_t header = ReadU32(base, slot + kNslHeader);
  if (header == 0) return 0;
  if (base[header + 6] & 0x80) return 0;
  return ReadU32(base, header + kHeaderResident);
}

uint32_t RoundUp(uint32_t v, uint32_t grain) { return uint32_t((uint64_t(v) + grain - 1) / grain * grain); }

// Whether new files of these sizes fit in the pool once the loaded banks of the bank entry's
// given slots are freed, with what other changes have been promised. `detail` says how it came out.
bool PoolHasRoom(uint8_t* base, uint32_t e, const std::vector<uint32_t>& slots, const std::vector<uint32_t>& sizes,
                 const Tracked& self, std::string& detail) {
  auto read = ReadPool(base);
  if (!read) {
    detail = "the sound system's bank memory couldn't be read";
    return false;
  }
  Pool p = *read;
  for (uint32_t s : slots)
    if (ReadU32(base, e + kState + 4 * s) == kStateLoaded)
      PoolFree(p, ResidentAddress(base, ReadU32(base, e + kLoaded + 4 * s)));
  const uint64_t gap = PoolGap(p);
  uint64_t need = 0, promised = 0;
  int entries = int(sizes.size()), promised_entries = 0;
  for (uint32_t v : sizes) need += RoundUp(v, p.grain);
  for (const Tracked& o : g_tracked)
    if (&o != &self) {
      promised += o.pending_bytes;
      promised_entries += o.pending_entries;
    }
  const int free_entries = p.top - p.bottom - 1;
  detail = std::to_string(need) + " needed, " + std::to_string(gap) + " free of " + std::to_string(p.size) +
           (promised ? " (" + std::to_string(promised) + " promised to other changes)" : std::string());
  return need + promised <= gap && entries + promised_entries <= free_entries;
}

// Forgets a promise once the bank's new files have loaded (or their load is over).
void SettlePromise(uint8_t* base, Tracked& t) {
  if (!t.pending_entries && !t.pending_bytes) return;
  if (t.swap.phase == SwapPhase::kReleasing) return;
  const uint32_t e = FindEntry(base, t.table, t.key);
  bool loading = false;
  if (e)
    for (uint32_t s : t.pending_slots) loading |= ReadU32(base, e + kState + 4 * s) == kStateLoading;
  if (loading) return;
  t.pending_bytes = 0;
  t.pending_entries = 0;
  t.pending_slots.clear();
}

// [acct2-ocr] 2026-11-07 cod3_debug_voices: NSL's bank pool as it is, for the log: each end's entries in KB from the
// end inwards (* freed; =bank/slot when one of ours starts there, "next to it" for an overlay), and the free gap.
// Space comes back only from an end (PoolGap), so a freed entry under one in use stays lost until that one goes.
std::string PoolText(uint8_t* base) {
  if (kPoolStart == 0) return "no pool here";
  const auto read = ReadPool(base);
  if (!read) return "the pool couldn't be read";
  const Pool& p = *read;
  std::map<uint32_t, std::string> owner;
  for (const Tracked& t : g_tracked) {
    const uint32_t e = FindEntry(base, t.table, t.key);
    if (!e) continue;
    for (uint32_t s = 0; s < uint32_t(kSlots); ++s) {
      if (ReadU32(base, e + kState + 4 * s) != kStateLoaded) continue;
      const uint32_t a = ResidentAddress(base, ReadU32(base, e + kLoaded + 4 * s));
      if (a >= p.start && a - p.start < p.size)
        owner[a - p.start] = BankName(t) + "/" + std::to_string(s) +
                             (t.overlay.voice != kNoOverlay && t.overlay.slot == s ? " next to it" : "");
    }
  }
  const auto one = [&](uint32_t v, uint64_t at) {
    std::string s = " " + std::to_string((v & 0x7FFFFFFFu) >> 10) + ((v & 0x80000000u) ? "*" : "");
    if (const auto it = owner.find(uint32_t(at)); it != owner.end()) s += "=" + it->second;
    return s;
  };
  std::string out = "bottom:";
  uint64_t at = 0;
  for (int i = 0; i <= p.bottom; ++i) {
    out += one(p.table[i], at);
    at += p.table[i] & 0x7FFFFFFFu;
  }
  out += "; top:";
  uint64_t end = p.size;
  for (int i = kPoolEntries - 1; i >= p.top; --i) {
    end -= p.table[i] & 0x7FFFFFFFu;
    out += one(p.table[i], end);
  }
  Pool q = p;
  out += "; free " + std::to_string(PoolGap(q) >> 10) + " KB of " + std::to_string(p.size >> 10);
  return out;
}
int g_pool_voice = -3;   // [acct2-ocr] the voice the pool was last logged for (cod3_debug_voices)
std::string g_overlay_first;   // [acct2-ocr] a bank with the speech of a voice before still next to it (SwapTask), or ""
// [acct2-ocr] 2026-11-07 when the chosen voice last changed (SwapTask), for kOverlayFirstFor
int g_change_voice = -3;
std::chrono::steady_clock::time_point g_change_since{};
constexpr const char* kOthersFirst = "the other banks change first";

// [acct2-ocr] Whether another bank's change is still under way (waiting to release what it has loaded, releasing, or
// loading its new files): speech loaded next to a bank before them would be under them in the pool.
bool OthersChanging(uint8_t* base, const Tracked& self) {
  for (const Tracked& o : g_tracked) {
    if (&o == &self) continue;
    if (o.swap.phase == SwapPhase::kReleasing || o.pending_entries > 0) return true;
    if (o.swap.phase != SwapPhase::kWaiting) continue;
    const uint32_t e = FindEntry(base, o.table, o.key);
    if (!e) continue;
    for (uint32_t s : o.swap.slots)
      if (ReadU32(base, e + kState + 4 * s) == kStateLoaded) return true;
  }
  return false;
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

// --- [acct2-ocr] 2026-11-07 a voice's speech next to a bank that keeps playing ----------
// The resident data of one bank file in assets/ (its NSLB header), nothing when it can't be read.
std::optional<uint32_t> ResidentOf(const std::string& game_path) {
  const auto p = Find(game_path);
  if (!p) return std::nullopt;
  std::ifstream f(*p, std::ios::binary);
  unsigned char h[0x34]{};
  if (!f.read(reinterpret_cast<char*>(h), sizeof h) || std::memcmp(h, "NSLB", 4) != 0) return std::nullopt;
  return uint32_t(h[0x30]) | uint32_t(h[0x31]) << 8 | uint32_t(h[0x32]) << 16 | uint32_t(h[0x33]) << 24;
}

// NSL's slot of a loaded bank (its handle), 0 when the handle isn't loaded there.
uint32_t LoadedNslSlot(uint8_t* base, uint32_t handle) {
  const uint32_t count = ReadU32(base, kNslBankSlots);
  const uint32_t slots = ReadU32(base, kNslSlots);
  if (count == 0 || count > 64 || slots == 0 || handle == kNone) return 0;
  const uint32_t slot = slots + ((handle >> 16) % count) * kNslSlotSize;
  if (ReadU32(base, slot) != handle || ReadU32(base, slot + kNslSlotState) != kNslSlotLoaded) return 0;
  return slot;
}

// The loaded overlays of the chosen voice, for nslFindSound's hook (game thread).
void UpdatePreferred(uint8_t* base, int voice) {
  std::vector<uint32_t> handles;
  for (const Tracked& t : g_tracked) {
    const Overlay& ov = t.overlay;
    if (ov.voice == kNoOverlay || ov.releasing || ov.voice != voice) continue;
    const uint32_t e = FindEntry(base, t.table, t.key);
    if (!e || ReadU32(base, e + kState + 4 * ov.slot) != kStateLoaded) continue;
    handles.push_back(ReadU32(base, e + kLoaded + 4 * ov.slot));
  }
  std::lock_guard lock(g_prefer_mutex);
  if (handles != g_prefer && REXCVAR_GET(cod3_debug_voices)) {
    std::string callers;
    for (const auto& [lr, count] : g_prefer_callers)
      if (lr) {
        char one[32];
        std::snprintf(one, sizeof one, " %08X x%u", unsigned(lr), unsigned(count));
        callers += one;
      }
    REXLOG_INFO("cod3: voices: {} speech bank(s) next to the level's sounds are heard first (so far {} of {} sounds "
                "looked up while one was there were found in it; asked from:{})", handles.size(),
                g_prefer_hits.load(std::memory_order_relaxed), g_prefer_looks.load(std::memory_order_relaxed),
                callers.empty() ? " -" : callers);
  }
  g_prefer = std::move(handles);
  g_prefer_count.store(int(g_prefer.size()));
}

// Loads `voice`'s speech in a free slot of a bank that keeps playing; false (and why) when it can't now.
bool TryOverlay(PPCContext& ctx, uint8_t* base, Tracked& t, uint32_t e, int voice, std::string& why) {
  // [acct2-ocr] 2026-11-07 voice -1 (the game's own, after a level that started in another voice): its own files
  const std::string speech = voice < 0 ? std::string("the game's own speech") : std::string("its ") + kNames[voice] + " speech";
  if (t.regs.empty()) {
    why = "the game registered nothing for it";
    return false;
  }
  if (t.overlay.voice != kNoOverlay) {
    why = "the speech of the voice before is still being let go";
    return false;
  }
  // [acct2-ocr] 2026-11-07 the other banks of this change first: one loaded after this speech goes on top of it and
  // keeps its memory from coming back once it is let go (run acct2-ocr-lang4-hold: the French next to mace2.wbk under
  // Global.wbk's, 3.6 MB). kOverlayFirstFor at most.
  if (std::chrono::steady_clock::now() - g_change_since < kOverlayFirstFor && OthersChanging(base, t)) {
    why = kOthersFirst;
    return false;
  }
  const int game_language = ReadS32(base, kGameLanguage);
  std::vector<std::string> candidates;
  if (voice >= 0) {
    candidates = SpeechFilesFor(t.regs.front().path, voice);
  } else {
    // The game's own speech: the speech of its split pair (the game's language slot), else its whole bank.
    for (const Original& o : t.regs)
      if (int(o.slot) == game_language) candidates.push_back(o.path);
    for (const Original& o : t.regs)
      if (o.slot == kEveryLanguage) candidates.push_back(o.path);
  }
  std::string path;
  const auto current = Files(base, t, t.voice);
  for (const std::string& p : candidates) {
    if (!Exists(p)) continue;
    bool there = false;
    for (const auto& r : current) there |= SameName(r.path, p);
    if (there) {
      why = "that speech is the one it has";
      return false;
    }
    path = p;
    break;
  }
  if (path.empty()) {
    why = std::string("assets/ has no ") + (voice < 0 ? std::string("speech of the game's own") : std::string(kNames[voice]) + " speech") +
          " for it";
    return false;
  }
  const auto free_slot = [&](int s) {
    return ReadU32(base, e + kState + 4 * uint32_t(s)) == kStateIdle && ReadU32(base, e + kFile + 4 * uint32_t(s)) == kNone &&
           ReadU32(base, e + kLoaded + 4 * uint32_t(s)) == kNone;
  };
  int slot = -1;
  if (game_language >= 0 && game_language < kSlots && free_slot(game_language)) slot = game_language;
  for (int s = 0; slot < 0 && s < kSlots; ++s)
    if (free_slot(s)) slot = s;
  if (slot < 0) {
    why = "it has no free slot";
    return false;
  }
  const auto bytes = ResidentOf(path);
  if (!bytes) {
    why = path + " couldn't be read";
    return false;
  }
  const uint32_t size = (*bytes + 0xFFFu) & ~0xFFFu;
  std::string room;
  if (!PoolHasRoom(base, e, {}, size ? std::vector<uint32_t>{size} : std::vector<uint32_t>{}, t, room)) {
    why = speech + " doesn't fit in the sound system's bank memory (" + room + ")";
    return false;
  }
  {
    cod3::guest::Call call(ctx, base);
    const int in_use = int(uint32_t(call.Invoke(kNslLoadedBankCount)));
    const int slots = int(ReadU32(base, kNslBankSlots));
    if (in_use < 0 || slots <= 0 || slots - in_use < 1) {
      why = "all of the sound system's " + std::to_string(slots) + " banks are in use";
      return false;
    }
  }
  RegisterRaw(ctx, base, t.table, t.key, {path, uint32_t(slot)}, t.regs.front().archive);
  if (ReadU32(base, e + kFile + 4 * uint32_t(slot)) == kNone) {
    why = "the game didn't open " + path;
    return false;
  }
  {
    cod3::guest::Call call(ctx, base);
    call.Invoke(kLoadSlot, {t.table, e, uint32_t(slot), 1});   // in the background, as the game loads a slot
  }
  t.overlay = Overlay{voice, uint32_t(slot), path, false, 0, std::chrono::steady_clock::now()};
  if (const auto pool = ReadPool(base)) t.pending_bytes = size ? RoundUp(size, pool->grain) : 0;
  t.pending_entries = size ? 1 : 0;
  t.pending_slots = {uint32_t(slot)};
  REXLOG_INFO("cod3: voices: {} keeps playing: {} ({}, slot {}) is loaded next to it and heard first ({})",
              BankName(t), speech, path, slot, room);
  return true;
}

// One frame of an overlay: loading (wanted: keep the preferences current), let go once another voice is chosen and
// nothing of it plays; true while it has work to do.
bool StepOverlay(PPCContext& ctx, uint8_t* base, Tracked& t, int voice) {
  Overlay& ov = t.overlay;
  if (ov.voice == kNoOverlay) return false;
  const uint32_t e = FindEntry(base, t.table, t.key);
  if (!e) {
    ov = Overlay{};
    return false;
  }
  const uint32_t state = ReadU32(base, e + kState + 4 * ov.slot);
  if (!ov.in && state != kStateLoading) {
    ov.in = true;
    if (REXCVAR_GET(cod3_debug_voices))
      REXLOG_INFO("cod3: voices: {}: the speech next to it ({}) is {}; the bank memory: {}", BankName(t),
                  VoiceName(ov.voice), state == kStateLoaded ? "in" : "not in (its load failed)", PoolText(base));
  }
  if (!ov.releasing && ov.voice == voice) return state == kStateLoading;
  const auto now = std::chrono::steady_clock::now();
  if (!ov.releasing) {
    if (state == kStateLoading) return true;
    std::vector<uint32_t> banks;
    if (state == kStateLoaded) banks.push_back(ReadU32(base, e + kLoaded + 4 * ov.slot));
    if (CountPlaying(ctx, base, banks) > 0) {
      ov.quiet = 0;
      return true;
    }
    if (++ov.quiet < kQuietFrames) return true;
    ReleaseSlots(ctx, base, e, {ov.slot});
    ov.releasing = true;
    ov.since = now;
    return true;
  }
  const uint32_t loaded = ReadU32(base, e + kLoaded + 4 * ov.slot);
  if (state == kStateIdle && loaded == kNone) {
    REXLOG_INFO("cod3: voices: {}: the speech next to it ({}) let go", BankName(t), VoiceName(ov.voice));
    if (REXCVAR_GET(cod3_debug_voices)) REXLOG_INFO("cod3: voices: the bank memory: {}", PoolText(base));
    ov = Overlay{};
    return false;
  }
  if (now - ov.since > kReleaseTimeout) {   // [acct2-ocr] 2026-11-07 was logged as let go too
    REXLOG_WARN("cod3: voices: {}: the speech next to it ({}) wasn't let go in {} s (slot {}: state {}, bank {:08X}); "
                "no longer followed: what it holds may stay in use until the next level load", BankName(t),
                VoiceName(ov.voice), kReleaseTimeout.count(), ov.slot, state, loaded);
    if (REXCVAR_GET(cod3_debug_voices)) REXLOG_INFO("cod3: voices: the bank memory: {}", PoolText(base));
    ov = Overlay{};
    return false;
  }
  return true;
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
    // Sound memory: the new files must fit in NSL's bank pool once this bank's old data is
    // freed the way the game frees it (13:35; before, at most what the bank had held since
    // the level loaded, which was too strict when the pool had room and not exact when the
    // old data wasn't at an end of the pool). Checked again just before the release.
    const auto old_bytes = ResidentBytes(old_files, sw.slots, game_language);
    if (old_bytes) t.peak = std::max(t.peak, *old_bytes);
    const auto new_sizes = ResidentSizes(new_files, sw.slots, game_language);
    // [acct2-ocr] 2026-11-07 a bank with nothing loaded (bcen_can in mace2) is only registered again: it takes no
    // memory and no NSL bank now (run acct2-ocr-lang3-pool: "doesn't fit" for English while it wasn't loaded)
    bool bank_in_use = false;
    for (int s = 0; s < kSlots; ++s) {
      const uint32_t st = ReadU32(base, e + kState + 4 * uint32_t(s));
      bank_in_use |= st == kStateLoaded || st == kStateLoading;
    }
    std::string room;
    if (bank_in_use && (!new_sizes || !PoolHasRoom(base, e, sw.slots, *new_sizes, t, room))) {
      std::string why;   // [acct2-ocr] the new speech next to its sounds instead
      if (TryOverlay(ctx, base, t, e, voice, why)) {
        sw.phase = SwapPhase::kDeferred;
        return false;
      }
      if (t.overlay.voice != kNoOverlay || why == kOthersFirst) {   // the voice before's speech goes first: look again
        sw = Swap{};
        return true;
      }
      REXLOG_INFO("cod3: voices: {} doesn't fit in the sound system's bank memory now ({}; {}): it changes at the "
                  "next level load", BankName(t), new_sizes ? room : std::string("its new files couldn't be read"), why);
      sw.phase = SwapPhase::kDeferred;
      return false;
    }
    if (REXCVAR_GET(cod3_debug_voices)) {
      if (bank_in_use)
        REXLOG_INFO("cod3: voices: {} fits ({})", BankName(t), room);
      else
        REXLOG_INFO("cod3: voices: {} isn't loaded: only registered again", BankName(t));   // [acct2-ocr]
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
    if (const int extra = loads(new_files) - loads(old_files); bank_in_use && extra > 0) {   // [acct2-ocr] in use
      cod3::guest::Call call(ctx, base);
      const int in_use = int(uint32_t(call.Invoke(kNslLoadedBankCount)));
      const int slots = int(ReadU32(base, kNslBankSlots));
      if (in_use < 0 || slots <= 0 || slots - in_use < extra) {
        std::string why;   // [acct2-ocr]
        if (TryOverlay(ctx, base, t, e, voice, why)) {
          sw.phase = SwapPhase::kDeferred;
          return false;
        }
        if (t.overlay.voice != kNoOverlay || why == kOthersFirst) {
          sw = Swap{};
          return true;
        }
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
    // [acct2-ocr] still playing after a moment (a level's own sounds): its new speech goes next to it
    if (!busy && now - sw.since > kOverlayAfter && now >= sw.next_overlay) {
      std::string why;
      if (TryOverlay(ctx, base, t, e, voice, why)) {
        sw.phase = SwapPhase::kDeferred;
        return false;
      }
      sw.next_overlay = now + std::chrono::seconds(1);
      if (!sw.overlay_told) {
        sw.overlay_told = true;
        REXLOG_INFO("cod3: voices: {} keeps playing, and its new speech can't go next to it now: {}", BankName(t), why);
      }
    }
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
  // [acct2-ocr] 2026-11-07 NSL's pool gives memory back only from its end. While the speech of a voice before is still
  // next to a bank (let go when its lines end), a bank released and loaded again now goes on top of it, and that
  // speech's memory doesn't come back for the rest of the level (run acct2-ocr-lang3-pool: 13.9 MB next to mace2.wbk,
  // free 2.9 MB -> 0.5 MB, so its later voices waited for the next level load). So wait for it, kOverlayFirstFor at most.
  if (loaded && !g_overlay_first.empty() && now - sw.since < kOverlayFirstFor) {
    if (!sw.held_told) {
      sw.held_told = true;
      REXLOG_INFO("cod3: voices: {} changes once the speech of the voice before next to {} is let go (its memory comes "
                  "back only then)", BankName(t), g_overlay_first);
    }
    return true;
  }
  sw.was_loaded = loaded;
  if (loaded) {
    // [acct2-decomp] The pool may have changed while the bank was playing: look again, and
    // promise the space until the new files have loaded.
    const auto new_sizes = ResidentSizes(Files(base, t, sw.target), sw.slots, game_language);
    std::string room;
    if (!new_sizes || !PoolHasRoom(base, e, sw.slots, *new_sizes, t, room)) {
      std::string why;   // [acct2-ocr]
      if (TryOverlay(ctx, base, t, e, voice, why)) {
        sw.phase = SwapPhase::kDeferred;
        return false;
      }
      if (t.overlay.voice != kNoOverlay || why == kOthersFirst) {
        sw = Swap{};
        return true;
      }
      REXLOG_INFO("cod3: voices: {} no longer fits in the sound system's bank memory ({}): it changes at the "
                  "next level load", BankName(t), new_sizes ? room : std::string("its new files couldn't be read"));
      sw.phase = SwapPhase::kDeferred;
      return false;
    }
    if (const auto pool = ReadPool(base)) {
      t.pending_bytes = 0;
      for (uint32_t v : *new_sizes) t.pending_bytes += RoundUp(v, pool->grain);
    }
    t.pending_entries = int(new_sizes->size());
    t.pending_slots = sw.slots;
  }
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
#if defined(COD3_MP)
      REXLOG_INFO("cod3: voices: {} (from the next map)", VoiceName(now));   // [acct2-ocr]
#else
      REXLOG_INFO("cod3: voices: {} (films from the next one; the soldiers within seconds, or at the next level "
                  "load when their speech doesn't fit)", VoiceName(now));   // [acct2-ocr]
#endif
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
#if defined(COD3_MP)
  // [acct2-ocr] 2026-11-07 cod3mp: at the next map (RefreshBanks); the Germany disc has German, the Fr/Es/It disc French
  const std::string help =
      "The language the soldiers speak. Another disc's voices (German, or French) come from the setup "
      "tool: Play page > Add voices from another disc. The text has its own setting (Text, below). In "
      "multiplayer a change is heard from the next map. Installed: " +
      (installed.empty() ? std::string("none found") : installed) + ".";
#else
  const std::string help =
      "The language the soldiers speak in the campaign and its films. Another disc's voices "
      "(German, or French, Spanish and Italian) come from the setup tool: Play page > Add voices "
      "from another disc. The text has its own setting (Text, below). Films change from the next "
      "film. The soldiers change within a few seconds: the battle chatter as soon as it is quiet, "
      // [acct2-ocr] 2026-11-07 a level's own speech next to its sounds (TryOverlay)
      "a level's own lines by loading the new voice's speech next to the level's sounds; only "
      "when that doesn't fit in the sound memory do they wait for the next level load. Nothing "
      "that is playing is cut off. Installed: " +
      (installed.empty() ? std::string("none found") : installed) + ".";
#endif
  Help(help.c_str());
  if (const int waiting = g_waiting.load(); waiting > 0) {
    const std::string w = std::to_string(waiting) + (waiting == 1 ? " sound bank still has" : " sound banks still have") +
#if defined(COD3_MP)
                          " the old voices (waiting for the next map).";
#else
                          " the old voices (playing, or waiting for the next level load).";
#endif
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
  for (Tracked& t : g_tracked) {   // [acct2-decomp] a new level: memory measured afresh
    t.peak = 0;
    t.pending_bytes = 0;
    t.pending_entries = 0;
    t.pending_slots.clear();
  }
  for (Tracked& t : g_tracked) {
    const bool overlay = t.overlay.voice != kNoOverlay;   // [acct2-ocr] registered afresh, without it
    if (t.voice == voice && !overlay) continue;
    const uint32_t e = FindEntry(base, t.table, t.key);
    if (!e) continue;
    if (!overlay && Files(base, t, voice) == Files(base, t, t.voice)) {
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
    t.overlay = Overlay{};   // [acct2-ocr] its file was closed with the others
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
  UpdatePreferred(base, voice);   // [acct2-ocr]
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
    if (voice != g_pool_voice && REXCVAR_GET(cod3_debug_voices)) {   // [acct2-ocr] before this change's first step
      g_pool_voice = voice;
      REXLOG_INFO("cod3: voices: the bank memory as {} is chosen: {}", VoiceName(voice), PoolText(base));
    }
    for (Tracked& t : g_tracked) SettlePromise(base, t);   // [acct2-decomp]
    if (voice != g_change_voice) {   // [acct2-ocr]
      g_change_voice = voice;
      g_change_since = std::chrono::steady_clock::now();
    }
    g_overlay_first.clear();   // [acct2-ocr] the speech of a voice before still next to a bank: SwapBank waits for it
    for (const Tracked& t : g_tracked)
      if (t.overlay.voice != kNoOverlay && t.overlay.voice != voice) {
        g_overlay_first = BankName(t);
        break;
      }
    for (Tracked& t : g_tracked) {
      more |= StepOverlay(ctx, base, t, voice);   // [acct2-ocr]
      if (t.voice == voice && t.swap.phase != SwapPhase::kReleasing) continue;
      const bool busy = SwapBank(ctx, base, t, voice);
      more |= busy;
      // [acct2-ocr] speech just asked for next to it: step on until it is in, so UpdatePreferred lists it (run
      // acct2-ocr-voiceE-own: the French speech loaded after the last step and was never looked in)
      if (t.overlay.voice != kNoOverlay && !t.overlay.releasing) {
        const uint32_t oe = FindEntry(base, t.table, t.key);
        more |= oe && ReadU32(base, oe + kState + 4 * t.overlay.slot) == kStateLoading;
      }
      waiting += t.voice != voice && t.overlay.voice != voice ? 1 : 0;   // [acct2-ocr] its speech next to it counts
    }
    UpdatePreferred(base, voice);   // [acct2-ocr]
  }
  g_waiting.store(waiting);
  if (more) RequestSwap();
}

void RequestSwap() {
#if defined(COD3_MP)
  // [acct2-ocr] 2026-11-07 cod3mp: a change is heard from the next map (RefreshBanks at its load); a match's banks
  // keep playing, and the change in the middle of a level reads the campaign's sound instances. What waits, for the
  // settings row:
  std::lock_guard lock(g_bank_mutex);
  const int voice = Chosen();
  int waiting = 0;
  for (const Tracked& t : g_tracked) waiting += t.voice != voice ? 1 : 0;
  g_waiting.store(waiting);
  static_cast<void>(&SwapTask);   // the campaign's
#else
  if (!g_swap_queued.exchange(true)) cod3::Engine::Get().QueueGuestTask(SwapTask);
#endif
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
    g_tracked.push_back({table, key, {}, voice, Swap{}, 0, 0, 0, {}});
    t = &g_tracked.back();
  } else if (!entry) {
    t->regs.clear();
    t->voice = voice;
    t->swap = Swap{};   // [acct2-decomp]
    t->overlay = Overlay{};   // [acct2-ocr]
    t->peak = 0;
    t->pending_bytes = 0;
    t->pending_entries = 0;
    t->pending_slots.clear();
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
    if (Files(base, *t, voice) != Files(base, *t, t->voice) || t->overlay.voice != kNoOverlay) {   // [acct2-ocr]
      t->overlay = Overlay{};   // [acct2-ocr] ClearFiles closes its file too
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

#if !defined(COD3_MP)   // [acct2-ocr] cod3mp plays no films and changes voices at the next map
// After 0x824EED20's XGetLanguage call (0x824EEDCC): r3 is the console
// language it picks a movie's sound track by. Movies without the chosen
// language fall back as the game's own code does (English first).
void Cod3VoiceMovieLanguage(PPCRegister& r3) {
  const int voice = cod3::voice::Chosen();
  if (REXCVAR_GET(cod3_debug_voices))   // [acct2-ocr] 2026-11-07 which sound track a film asks for first
    REXLOG_INFO("cod3: voices: a film's sound track: {} first (console language {} -> {})",
                voice >= 0 ? cod3::voice::kNames[voice] : "the game's own", r3.u32,
                voice >= 0 ? cod3::voice::ConsoleLanguageFor(voice) : r3.u32);
  if (voice >= 0) r3.u64 = cod3::voice::ConsoleLanguageFor(voice);
}

namespace cod3::voice {
namespace {
// [acct2-ocr] 2026-11-07 cod3_debug_voices: where the game's sound lookups land, by the bank's number of sounds
// (header+16: blkbrn.wbk 748, its .wul 620, its .wfr/.wit 128), and a check of kNslFindInBank against what
// nslFindSound found (the same index from that bank's header). Logged every 30 s.
struct LookupStats {
  std::mutex mutex;
  std::map<uint32_t, uint32_t> by_bank;
  uint32_t misses = 0, checked = 0, check_bad = 0;
  std::chrono::steady_clock::time_point since = std::chrono::steady_clock::now();
};
LookupStats g_lookups;

uint32_t NslSlotOf(uint8_t* base, uint32_t handle) {
  const uint32_t count = ReadU32(base, kNslBankSlots);
  const uint32_t slots = ReadU32(base, kNslSlots);
  if (count == 0 || count > 64 || slots == 0) return 0;
  return slots + ((handle >> 16) % count) * kNslSlotSize;
}

void NoteLookup(PPCContext& ctx, uint8_t* base, uint32_t name, uint32_t found) {
  uint32_t sounds = 0;
  int check = -1;   // -1 not checked, 0 bad, 1 good
  if (found != kNone) {
    if (const uint32_t slot = NslSlotOf(base, found)) {
      const uint32_t header = ReadU32(base, slot + kNslHeader);
      if (header) {
        sounds = ReadU32(base, header + 16);
        cod3::guest::Call call(ctx, base);
        const uint32_t index = uint32_t(call.Invoke(kNslFindInBank, {header, name}));
        check = index == (found & 0xFFFFu) ? 1 : 0;
      }
    }
  }
  std::lock_guard lock(g_lookups.mutex);
  if (found == kNone) ++g_lookups.misses;
  else ++g_lookups.by_bank[sounds];
  if (check >= 0) {
    ++g_lookups.checked;
    g_lookups.check_bad += check == 0 ? 1 : 0;
  }
  const auto now = std::chrono::steady_clock::now();
  if (now - g_lookups.since < std::chrono::seconds(30)) return;
  std::string banks;
  for (const auto& [n, c] : g_lookups.by_bank) banks += " " + std::to_string(n) + ":" + std::to_string(c);
  REXLOG_INFO("cod3: voices: sound lookups in 30 s, found in banks of N sounds (N:times):{}; not found {}; "
              "kNslFindInBank agreed {} of {}", banks.empty() ? " -" : banks, g_lookups.misses,
              g_lookups.checked - g_lookups.check_bad, g_lookups.checked);
  g_lookups.by_bank.clear();
  g_lookups.misses = g_lookups.checked = g_lookups.check_bad = 0;
  g_lookups.since = now;
}
}  // namespace
}  // namespace cod3::voice

// [acct2-ocr] 2026-11-07 nslFindSound (hash): a sound is looked for in the chosen voice's speech next to a bank that
// keeps playing first (see kLoadSlot), then as the game does (NSL's loaded banks in slot order).
REX_EXTERN(__imp__sub_821196E0);
REX_HOOK_RAW(sub_821196E0) {
  using namespace cod3::voice;
  const uint32_t hash = ctx.r3.u32;
  if (hash != 0 && g_prefer_count.load(std::memory_order_relaxed) > 0) {
    std::array<uint32_t, 8> handles{};
    size_t n = 0;
    g_prefer_looks.fetch_add(1, std::memory_order_relaxed);   // [acct2-ocr]
    {
      std::lock_guard lock(g_prefer_mutex);
      for (uint32_t h : g_prefer)
        if (n < handles.size()) handles[n++] = h;
      if (REXCVAR_GET(cod3_debug_voices)) {   // [acct2-ocr] who asks (a caller that keeps a handle never does)
        const uint32_t lr = uint32_t(ctx.lr);
        auto* slot = &g_prefer_callers[0];
        for (auto& c : g_prefer_callers)
          if (c.first == lr || c.first == 0) { slot = &c; break; }
        slot->first = lr;
        ++slot->second;
      }
    }
    for (size_t i = 0; i < n; ++i) {
      const uint32_t slot = LoadedNslSlot(base, handles[i]);
      if (!slot) continue;
      const uint32_t header = ReadU32(base, slot + kNslHeader);
      if (!header) continue;
      cod3::guest::Call call(ctx, base);
      const int32_t index = int32_t(uint32_t(call.Invoke(kNslFindInBank, {header, hash})));
      if (index == -1) continue;
      g_prefer_hits.fetch_add(1, std::memory_order_relaxed);
      ctx.r3.u64 = (ReadU32(base, slot) & 0xFFFF0000u) | uint32_t(index);
      if (REXCVAR_GET(cod3_debug_voices)) NoteLookup(ctx, base, hash, ctx.r3.u32);   // [acct2-ocr]
      return;
    }
  }
  __imp__sub_821196E0(ctx, base);
  if (REXCVAR_GET(cod3_debug_voices)) NoteLookup(ctx, base, hash, ctx.r3.u32);   // [acct2-ocr]
}

#endif  // !defined(COD3_MP)
