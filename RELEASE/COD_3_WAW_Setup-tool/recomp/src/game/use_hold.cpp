// cod3 - how long Use is held (see use_hold.h). [acct2-decomp] 2026-11-05.

#include "game/use_hold.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <string>  // [acct2-ocr]
#include <mutex>          // [acct2-main] NoteWords
#include <unordered_set>  // [acct2-main] NoteWords

#include <rex/cvar.h>
#include <rex/hook.h>  // PPCRegister, for the midasm hook below
#include <rex/logging.h>

#include "game/addresses.h"
#include "game/guest.h"
#include "game/program.h"

REXCVAR_DEFINE_INT32(cod3_use_hold_ms, -1, "cod3/Game",
                     "How long Use is held to pick up a weapon or ammo, man a gun or get into a vehicle, in "
                     "milliseconds. -1: the game's own (g_useholdtime: 250 with every controller layout); 0: a "
                     "press uses at once, and the hints say \"Press\" instead of \"Press and hold\". The "
                     "controller's Use button also reloads: a press shorter than this reloads, so at 0 it uses "
                     "what is in reach instead of reloading (the keyboard's Use and Reload keys are separate). "
                     "Scripted holds (planting a charge, opening a gate) keep their own time. Multiplayer: player "
                     "1's; split-screen players 2 to 4 have cod3_use_hold_ms_p2 .. _p4")  // [acct2-ocr] 2026-11-07
    .range(-1, 1000)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
#if defined(COD3_MP)
// [acct2-ocr] 2026-11-07 per player (owner 23:40: "it's per person"; use_hold.h SettingMsFor).
#define COD3_USE_HOLD_PLAYER(n)                                                                              \
  REXCVAR_DEFINE_INT32(cod3_use_hold_ms_p##n, -1, "cod3/Game",                                               \
                       "Split screen: player " #n "'s own Use hold in milliseconds (0: a press); -1 = player "  \
                       "1's cod3_use_hold_ms; -2 = the game's own hold")                                        \
      .range(-2, 1000)                                                                                       \
      .lifecycle(rex::cvar::Lifecycle::kHotReload)
COD3_USE_HOLD_PLAYER(2);
COD3_USE_HOLD_PLAYER(3);
COD3_USE_HOLD_PLAYER(4);
#undef COD3_USE_HOLD_PLAYER
#endif
// [acct2-main] 2026-11-13 in both programs (was cod3mp's only): also each text with a Use word as the prompts show it.
REXCVAR_DEFINE_BOOL(cod3_debug_use_hold, false, "cod3",
                    "Log whose Use hold the server's check takes, once per entity and value, and each text with a Use "
                    "word as the prompts lay it out (game/use_hold.h)");

namespace cod3::use_hold {
namespace {

namespace addr = cod3::addr;
using cod3::guest::ReadU32;

// g_useholdtime's vmCvar {handle, modificationCount, value, integer}: the
// address the one read builds (lis/addi before `lwz r11,12(r11)`).
constexpr uint32_t kUseHoldTimeCvar = COD3_ADDR(0x829B7AB0, 0x82B33DF0);
constexpr uint32_t kCvarInteger = 12;

int GameMs(uint8_t* base) { return static_cast<int>(ReadU32(base, kUseHoldTimeCvar + kCvarInteger)); }

// [acct2-main] 2026-11-13 ServerHere (sv_running) is gone: in system link each console times its own players' Use
// (CurrentMsFor), so the hints no longer ask whether this console hosts.

// Where `words` + " [USE]" starts in `text` at or after `from` (the first
// letter in either case), or npos. [acct2-main] 2026-11-13 any action `word` ("USE", "ACTIVATE").
size_t FindWords(const std::string& text, const std::string& words, const std::string& word, size_t from) {
  const std::string target = words + " [" + word + "]";
  if (words.empty()) return std::string::npos;
  for (size_t i = from; i + target.size() <= text.size(); ++i) {
    if (text.compare(i + 1, target.size() - 1, target, 1, std::string::npos) != 0) continue;
    const unsigned char a = static_cast<unsigned char>(text[i]), b = static_cast<unsigned char>(target[0]);
    if (a == b || (a < 0x80 && b < 0x80 && std::tolower(a) == std::tolower(b))) return i;
  }
  return std::string::npos;
}

// `press` with the first letter's case of the words it replaces.
std::string SameCase(std::string press, char first) {
  const unsigned char c = static_cast<unsigned char>(first), p = static_cast<unsigned char>(press[0]);
  if (c < 0x80 && p < 0x80 && std::isalpha(c) && std::isalpha(p))
    press[0] = static_cast<char>(std::isupper(c) ? std::toupper(p) : std::tolower(p));
  return press;
}

// [acct2-main] 2026-11-13 `word` the action ("USE", "ACTIVATE").
void Replace(std::string* text, const std::string& press_and_hold, const std::string& press, const std::string& word) {
  if (press_and_hold.empty() || press.empty() || press_and_hold == press) return;
  for (size_t at = FindWords(*text, press_and_hold, word, 0); at != std::string::npos;) {
    const std::string now = SameCase(press, (*text)[at]);
    text->replace(at, press_and_hold.size(), now);
    at = FindWords(*text, press_and_hold, word, at + now.size());
  }
}

}  // namespace

int SettingMs() { return std::clamp<int>(REXCVAR_GET(cod3_use_hold_ms), -1, 1000); }

// [acct2-ocr] 2026-11-07 per player (use_hold.h).
int OwnSettingMs(int player) {
  switch (player) {
    case 0: return SettingMs();
#if defined(COD3_MP)
    case 1: return std::clamp<int>(REXCVAR_GET(cod3_use_hold_ms_p2), -2, 1000);
    case 2: return std::clamp<int>(REXCVAR_GET(cod3_use_hold_ms_p3), -2, 1000);
    case 3: return std::clamp<int>(REXCVAR_GET(cod3_use_hold_ms_p4), -2, 1000);
#endif
    default: return -1;
  }
}

int SettingMsFor(int player) {
  if (player <= 0 || player >= kPlayers || !cod3::kMultiplayer) return SettingMs();
  const int own = OwnSettingMs(player);
  return own == -1 ? SettingMs() : own == -2 ? -1 : own;
}

const char* SettingName(int player) {
  static constexpr const char* kNames[kPlayers] = {"cod3_use_hold_ms", "cod3_use_hold_ms_p2", "cod3_use_hold_ms_p3",
                                                   "cod3_use_hold_ms_p4"};
  if (player == 0) return kNames[0];
  return cod3::kMultiplayer && player > 0 && player < kPlayers ? kNames[player] : nullptr;
}

// [acct2-main] 2026-11-13 the player's own setting on every console (was: only where the server runs, else the
// game's). cod3mp's ClientThink_real (0x82592CD0) calls the Use check (0x8256E678 -> 0x8256B478) for this console's
// own players only, a joined console's included (MP_MOVEMENT_PLAN.md T2: the host never runs it for the joiner), and
// the check's hook below takes that player's setting there; so a joined player at 0 read "Press and hold" for a Use
// that a press already did.
int CurrentMsFor(uint8_t* base, int player) {
  const int ms = SettingMsFor(player);
  if (!base) return std::max(ms, 0);
  if (ms >= 0) return ms;
  return GameMs(base);
}

int CurrentMs(uint8_t* base) {
  int player = 0;
  if (cod3::kMultiplayer && base) {
    const uint32_t active = ReadU32(base, addr::kActiveLocalClient);
    if (active < uint32_t(kPlayers)) player = int(active);
  }
  return CurrentMsFor(base, player);
}

std::string SettingsKey() {
  std::string key;
  for (int p = 0; p < (cod3::kMultiplayer ? kPlayers : 1); ++p) key += std::to_string(OwnSettingMs(p)) + ',';
  return key;
}

// [acct2-main] 2026-11-13 (use_hold.h NoteWords)
void NoteWords(const std::string& text, const std::string& shown, int ms, const char* how) {
  if (!REXCVAR_GET(cod3_debug_use_hold)) return;
  static std::mutex mutex;
  static std::unordered_set<std::string> seen;
  std::lock_guard lock(mutex);
  if (seen.size() >= 200 || !seen.insert(text + '\x01' + shown + '\x01' + how).second) return;
  REXLOG_INFO("cod3: Use hold words ({}, the hold {} ms): \"{}\" -> \"{}\"", how, ms, text.substr(0, 200),
              shown.substr(0, 200));
}

// [acct2-main] 2026-11-13 (owner 11-12 23:51: "KBM ... Also consider QTE events") USE and ACTIVATE: the
// campaign's levels write their use triggers' hints with [HOLD_ACTIVATE] ("[HOLD_ACTIVATE] to plant charge.", the
// boosts, "to pick up the M1 Garand", "to enter boat", "to Drive"; Forest's strings: trigger_use, HINT_ACTIVATE,
// SCRIPT_HINTSTR_PLANTEXPLOSIVES). A trigger's hint is the cursor hint the server's one Use check times
// (sub_8254A460), so its hold is this setting too, the battle actions that start that way included; their own
// steps ("Press [BATTLE_ACTION_BUTTON] to insert fuse") are presses already. A plain "Hold [ACTIVATE] ..." (Fuel
// plant's "Hold [ACTIVATE] to open Main Gates") stays as written: the levels can read the buttons themselves
// (rexglue-native NOTES, F0), so such a hold may be the level's own time, not this one.
bool HasUseWord(const std::string& text) {
  return text.find("USE]") != std::string::npos || text.find("ACTIVATE]") != std::string::npos;
}

std::string Pressed(const std::string& text, const std::string& press_and_hold, const std::string& press) {
  std::string out = text;
  for (const char* word : {"USE", "ACTIVATE"}) {  // [acct2-main] ACTIVATE too
    const std::string w = word, hold = "[HOLD_" + w + "]", verb = "[VERB_" + w + "]";
    for (size_t at = out.find(hold); at != std::string::npos; at = out.find(hold, at + 1))
      out.replace(at, hold.size(), verb);
    Replace(&out, press_and_hold, press, w);
    Replace(&out, "Press and hold", "Press", w);  // English texts, whatever language the words are in
  }
  return out;
}

}  // namespace cod3::use_hold

// ---------------------------------------------------------------------------
// midasm hook on `cmpw cr6,r10,r11` right after the game loads g_useholdtime's
// integer into r11 (campaign 0x8254A4F8, cod3mp 0x8256B57C: the server's hold
// check, for each client): the hold cod3_use_hold_ms asks for, when it is 0
// or more. The game's own value stays in its dvar.
// ---------------------------------------------------------------------------
#if defined(COD3_MP)
// [acct2-ocr] 2026-11-07 Whose check runs now (use_hold.h): the local client whose entity 0x8256B478 was called
// with (its r3), -1 an entity of none of this console's players, -2 outside that call. The server's thread only.
namespace {
thread_local int t_use_player = -2;
constexpr uint32_t kLocalEntities = 0x82BCC7FC;  // game/movement/program_mp.h mp_fn::kLocalEntities
bool Valid(uint32_t p) { return p >= 0x10000 && p < 0xC0000000u && (p & 3) == 0; }
int LocalPlayerOfEntity(uint8_t* base, uint32_t ent) {
  const uint32_t table = Valid(ent) ? cod3::guest::ReadU32(base, kLocalEntities) : 0;
  if (!Valid(table)) return -1;
  for (int p = 0; p < cod3::use_hold::kPlayers; ++p)
    if (cod3::guest::ReadU32(base, table + 4 * uint32_t(p + 1)) == ent) return p;
  return -1;
}
}  // namespace

REX_EXTERN(__imp__sub_8256B478);
REX_HOOK_RAW(sub_8256B478) {
  const int outer = t_use_player;
  t_use_player = LocalPlayerOfEntity(base, ctx.r3.u32);
  if (REXCVAR_GET(cod3_debug_use_hold)) {
    static std::atomic<uint64_t> last[8] = {};
    const uint32_t ent = ctx.r3.u32;
    const int ms = t_use_player >= 0 ? cod3::use_hold::SettingMsFor(t_use_player) : -1;
    const uint64_t key = (uint64_t(ent) << 32) | uint32_t(ms + 3) << 8 | uint32_t(t_use_player + 2);
    if (last[(ent >> 4) & 7].exchange(key, std::memory_order_relaxed) != key)
      REXLOG_INFO("cod3: Use hold check: entity {:08X} = {} -> {}", ent,
                  t_use_player >= 0 ? "local player " + std::to_string(t_use_player + 1)
                                    : std::string("none of this console's players"),
                  ms >= 0 ? std::to_string(ms) + " ms" : std::string("the game's own"));
  }
  __imp__sub_8256B478(ctx, base);
  t_use_player = outer;
}
#endif

void Cod3UseHoldTime(PPCRegister& r11) {
  // [acct2-ocr] 2026-11-07 the checked player's own (cod3mp; the campaign's player 1)
  int player = 0;
#if defined(COD3_MP)
  player = t_use_player == -2 ? 0 : t_use_player;  // outside the call: player 1's, as before
#endif
  const int ms = player >= 0 ? cod3::use_hold::SettingMsFor(player) : -1;
  static std::atomic<int> logged[cod3::use_hold::kPlayers] = {};  // ms + 3 last logged; 0: not yet
  const int slot = player >= 0 ? player : 0;
  if (player >= 0 && logged[slot].exchange(ms + 3, std::memory_order_relaxed) != ms + 3) {
    const int game = static_cast<int>(r11.u32);
    if (ms >= 0)
      REXLOG_INFO("cod3: Use is held {} ms (player {}: {}; the game's g_useholdtime is {})", ms, player + 1,
                  cod3::use_hold::SettingName(player) ? cod3::use_hold::SettingName(player) : "?", game);
    else
      REXLOG_INFO("cod3: Use is held the game's {} ms (g_useholdtime; player {})", game, player + 1);
  }
  if (ms >= 0) r11.u64 = static_cast<uint32_t>(ms);
}
