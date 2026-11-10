// cod2 - what the game says about itself, in the port's log. [acct2-cod2]
//
// Call of Duty 2's own console lines (Com_PrintMessage), its fatal errors
// (Com_Error), the menus it opens (Menus_OpenByName) and the zones it loads
// (DB_LoadXAssets) go to the log as "cod2: game: ..." lines, so a test run
// shows how far the game got without a picture. Addresses from the decomp
// (dev/waw-decomp/cod2_360/sp/names.tsv); what each register holds was read
// in the translated code. cod2_log_game_prints switches it off. The menu
// hook also tells game/test_start.cpp which menus open.
//
// A guest string is read only where the game itself reads it next (the
// argument the function is about to use), and only at addresses the guest
// can have strings at.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <string>
#include <vector>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

#include "game/test_start.h"

REXCVAR_DEFINE_BOOL(cod2_log_game_prints, true, "cod2/Debug",
                    "Copy the game's own console lines, errors, menus and zone loads into the log");
REXCVAR_DEFINE_BOOL(cod2_debug_menu_dump, false, "cod2/Debug",
                    "Tests: the first time each menu opens, log its items and the text each one holds "
                    "(for MENUS.md)");

namespace {

// At most this many game lines per run (a game in a loop could print without end).
constexpr int kMaxLines = 20000;
std::atomic<int> g_lines{0};

// The end of the program image (default.xex, default_mp.xex).
#if defined(COD2_MP)
constexpr uint32_t kImageEnd = 0x847C0000u;
#else
constexpr uint32_t kImageEnd = 0x84290000u;
#endif

// Where the guest keeps strings: its heaps and stacks, the program image, and
// physical memory.
bool IsStringAddress(uint32_t a) {
  return (a >= 0x40000000u && a < 0x80000000u) || (a >= 0x82000000u && a < kImageEnd) ||
         (a >= 0xA0000000u && a < 0xFFFF0000u);
}

// The guest's NUL-terminated string at `addr` (at most 400 characters, line
// ends dropped), or an empty string when it doesn't look like text.
std::string GuestString(const uint8_t* base, uint32_t addr) {
  if (!IsStringAddress(addr)) return {};
  std::string out;
  const char* s = reinterpret_cast<const char*>(base + addr);
  for (size_t i = 0; i < 400 && s[i]; ++i) {
    const unsigned char c = static_cast<unsigned char>(s[i]);
    if (c == '\n' || c == '\r') continue;
    if (c < 0x20 && c != '\t') return {};
    out.push_back(static_cast<char>(c));
  }
  return out;
}

bool Allowed() {
  if (!REXCVAR_GET(cod2_log_game_prints)) return false;
  const int n = g_lines.fetch_add(1, std::memory_order_relaxed);
  if (n == kMaxLines) REXLOG_INFO("cod2: game: (no more game lines in this run's log)");
  return n < kMaxLines;
}

uint32_t LoadU32(const uint8_t* base, uint32_t addr) {
  uint32_t v;
  std::memcpy(&v, base + addr, 4);
  return __builtin_bswap32(v);
}

// The menu dump (cod2_debug_menu_dump). Menus_OpenByName(context, name) looks
// the name up in the context's menus (Menus_FindByName 0x822F30E0: the menu
// pointers at +0x2C, their count at +0x12C; a menu's name at +0xC0). A menu
// holds its items at +0x27C (pointers) and their count at +0x218
// (Menu_ItemsMatchingGroup 0x822F3388); an item's name is at +0xC0 and its
// group at +0xC4, as a menu's. Every word of an item's 0x2F4 bytes that points
// at plain text is logged with its offset. Read from the first dumps (11-07):
// an item's text at +0x294, its action script +0x2B0, mouse-over script
// +0x2B8, the dvar it is shown by +0x2C4 and that dvar's values +0x2CC; a
// menu's open script at +0x244, close +0x248, escape +0x24C, music +0x254.
constexpr uint32_t kMenuPointers = 0x2C, kMenuCount = 0x12C, kMenuName = 0xC0, kMaxMenus = 64;
constexpr uint32_t kItemCount = 0x218, kItemPointers = 0x27C;
constexpr uint32_t kItemScanBytes = 0x2F4;  // an item's size (the next one's name follows at +0x3B4)
std::vector<std::string> g_dumped_menus;

bool IsDataAddress(uint32_t a) { return IsStringAddress(a) && (a & 3u) == 0; }

// Every word in [object, object + bytes) that points at text: " +OFF:"text"".
std::string TextWords(const uint8_t* base, uint32_t object, uint32_t bytes) {
  std::string text;
  for (uint32_t off = 0; off < bytes; off += 4) {
    const uint32_t p = LoadU32(base, object + off);
    if (!IsStringAddress(p) || p == object) continue;
    const std::string s = GuestString(base, p);
    if (s.empty()) continue;
    // Plain text only (numbers and pointers read as text have bytes above 0x7E).
    bool plain = true;
    for (char c : s)
      if (static_cast<unsigned char>(c) > 0x7E) plain = false;
    if (!plain) continue;
    char head[16];
    std::snprintf(head, sizeof head, " +%03X:", off);
    text += head;
    text += "\"" + s.substr(0, 300) + "\"";
  }
  return text;
}

void DumpMenu(const uint8_t* base, uint32_t context, const std::string& name) {
  if (!REXCVAR_GET(cod2_debug_menu_dump) || !IsDataAddress(context)) return;
  for (const std::string& done : g_dumped_menus)
    if (done == name) return;
  g_dumped_menus.push_back(name);
  // As Menus_FindByName: the names compared without case; room for 64 menus.
  auto same = [](const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
      if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i])))
        return false;
    return true;
  };
  const uint32_t count = std::min<uint32_t>(LoadU32(base, context + kMenuCount), kMaxMenus);
  uint32_t menu = 0;
  for (uint32_t i = 0; i < count && menu == 0; ++i) {
    const uint32_t m = LoadU32(base, context + kMenuPointers + 4 * i);
    if (IsDataAddress(m) && same(GuestString(base, LoadU32(base, m + kMenuName)), name)) menu = m;
  }
  if (menu == 0) {
    REXLOG_INFO("cod2: menu dump: \"{}\" not found among the context's {} menus", name, count);
    return;
  }
  const uint32_t items = std::min<uint32_t>(LoadU32(base, menu + kItemCount), 128);
  const uint32_t list = LoadU32(base, menu + kItemPointers);
  REXLOG_INFO("cod2: menu dump: \"{}\" at {:08X}, {} items;{}", name, menu, items,
              TextWords(base, menu, kItemPointers));  // the menu's own (its open and close scripts)
  if (!IsDataAddress(list)) return;
  for (uint32_t i = 0; i < items; ++i) {
    const uint32_t item = LoadU32(base, list + 4 * i);
    if (IsDataAddress(item))
      REXLOG_INFO("cod2: menu dump: \"{}\" item {}{}", name, i, TextWords(base, item, kItemScanBytes));
  }
}

}  // namespace

#if !defined(COD2_MP)

// Com_PrintMessage(channel r3, message r4): the game's console output. Channel
// 4 is dropped by the game itself.
REX_EXTERN(__imp__sub_8233B1F8);
REX_HOOK_RAW(sub_8233B1F8) {
  if (ctx.r3.u32 != 4 && Allowed()) {
    const std::string text = GuestString(base, ctx.r4.u32);
    if (!text.empty()) REXLOG_INFO("cod2: game: {}", text);
  }
  __imp__sub_8233B1F8(ctx, base);
}

// Com_Error(code r3, format r4, ...): logged before it unwinds (it doesn't return).
REX_EXTERN(__imp__sub_8233AB80);
REX_HOOK_RAW(sub_8233AB80) {
  if (REXCVAR_GET(cod2_log_game_prints)) {
    const std::string format = GuestString(base, ctx.r4.u32);
    // The first argument is read as text only when the format's first conversion is %s.
    const size_t percent = format.find('%');
    const bool first_is_text = percent != std::string::npos && format.compare(percent, 2, "%s") == 0;
    const std::string first = first_is_text ? GuestString(base, ctx.r5.u32) : std::string();
    REXLOG_WARN("cod2: game: Com_Error code {}: \"{}\"{}{}", ctx.r3.s32, format,
                first.empty() ? "" : ", first argument \"", first.empty() ? "" : first + "\"");
  }
  __imp__sub_8233AB80(ctx, base);
}

// Menus_OpenByName(context r3, name r4).
REX_EXTERN(__imp__sub_822FC8E8);
REX_HOOK_RAW(sub_822FC8E8) {
  const std::string name = GuestString(base, ctx.r4.u32);
  if (Allowed()) REXLOG_INFO("cod2: game: menu open \"{}\"", name);
  DumpMenu(base, ctx.r3.u32, name);
  __imp__sub_822FC8E8(ctx, base);
  cod2::test_start::OnMenuOpened(name);
}

// DB_LoadXAssets(zones r3, count r4, sync r5): the first word of each zone
// entry, when it points into the program image, is taken as the zone's name.
REX_EXTERN(__imp__sub_824030C0);
REX_HOOK_RAW(sub_824030C0) {
  if (Allowed()) {
    std::string names;
    const uint32_t zones = ctx.r3.u32, count = std::min<uint32_t>(ctx.r4.u32, 16);
    if (IsStringAddress(zones)) {
      for (uint32_t i = 0; i < count; ++i) {
        const uint32_t name = LoadU32(base, zones + i * 12);
        if (name >= 0x82000000u && name < kImageEnd) names += " " + GuestString(base, name);
      }
    }
    REXLOG_INFO("cod2: game: DB_LoadXAssets {:08X} x{} sync {}:{}", zones, ctx.r4.u32, ctx.r5.u32,
                names.empty() ? " (names not read)" : names);
  }
  __imp__sub_824030C0(ctx, base);
}

// CL_PlayCinematic_f 0x82434210: the "cinematic <name>" command starting a
// Bink film (the intro queues legal, atvi, IW_logo and cod2_intro; levels start
// theirs the same way). Logged with a count and the time since the last one, so
// a run shows each film's playing time against its own length (owner 22:33: a
// decoder stepped per frame, or synced to an audio clock that doesn't run,
// plays fast at high frame rates).
// Film pictures: BinkDoFrame 0x82497D90 runs once per picture decoded (called
// by the intro player 0x822D1A30 and by 0x82498660). Counted per film and per
// 10 s while films play, so the pictures a second can be set against the
// file's own rate (29.97 for cod2_intro; coordinator 23:2x: CoD3's films ran
// 17 % fast at 1000 fps).
namespace film {
std::atomic<uint64_t> pictures{0};     // since the current film started
std::atomic<int64_t> started_ns{0};    // when it started (0: none yet)
int64_t window_ns = 0, last_ns = 0;    // BinkDoFrame's thread only
uint64_t window_pictures = 0;
int64_t NowNs() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}
}  // namespace film

REX_EXTERN(__imp__sub_82497D90);
REX_HOOK_RAW(sub_82497D90) {
  __imp__sub_82497D90(ctx, base);
  film::pictures.fetch_add(1, std::memory_order_relaxed);
  const int64_t now = film::NowNs();
  // A pause of over a second (between films, or a film's end) starts a new window.
  if (film::window_ns == 0 || now - film::last_ns > 1000000000LL) {
    film::window_ns = now;
    film::window_pictures = 0;
  }
  film::last_ns = now;
  ++film::window_pictures;
  if (now - film::window_ns >= 10000000000LL) {
    const double s = double(now - film::window_ns) / 1e9;
    REXLOG_INFO("cod2: game: film pictures: {} in {:.2f} s ({:.2f} a second)", film::window_pictures, s,
                double(film::window_pictures) / s);
    film::window_ns = now;
    film::window_pictures = 0;
  }
}

REX_EXTERN(__imp__sub_82434210);
REX_HOOK_RAW(sub_82434210) {
  static int count = 0;
  static std::chrono::steady_clock::time_point last;
  const auto now = std::chrono::steady_clock::now();
  {
    const int64_t now_ns = film::NowNs();
    const uint64_t shown = film::pictures.exchange(0);
    const int64_t began = film::started_ns.exchange(now_ns);
    if (began != 0) {
      const double s = double(now_ns - began) / 1e9;
      REXLOG_INFO("cod2: game: film {}: {} pictures in {:.2f} s ({:.2f} a second)", count, shown, s,
                  s > 0 ? double(shown) / s : 0.0);
    }
  }
  ++count;
  if (count == 1) {
    REXLOG_INFO("cod2: game: film {} starts (cinematic command)", count);
  } else {
    REXLOG_INFO("cod2: game: film {} starts (cinematic command), {:.2f} s after film {}", count,
                std::chrono::duration<double>(now - last).count(), count - 1);
  }
  last = now;
  __imp__sub_82434210(ctx, base);
}

#else  // COD2_MP

// Menus_OpenByName(context r3, name r4) in the multiplayer program: 0x823190F0
// translates to exactly the campaign's 0x822FC8E8 (find_fn.py, score 1.000).
REX_EXTERN(__imp__sub_823190F0);
REX_HOOK_RAW(sub_823190F0) {
  const std::string name = GuestString(base, ctx.r4.u32);
  if (Allowed()) REXLOG_INFO("cod2mp: game: menu open \"{}\"", name);
  DumpMenu(base, ctx.r3.u32, name);  // the same UI code (score 1.000), so the same layout
  __imp__sub_823190F0(ctx, base);
  cod2::test_start::OnMenuOpened(name);
}

// The text of the multiplayer program's error dialog (error_popmenu): 0x823668E0
// takes the message (r3), localizes it and sets com_errorTitle and
// com_errorMessage (the dialog's text); a localized message is also copied to
// 0x841F0180. Both are logged.
constexpr uint32_t kErrorText = 0x841F0180u;
REX_EXTERN(__imp__sub_823668E0);
REX_HOOK_RAW(sub_823668E0) {
  const bool log = REXCVAR_GET(cod2_log_game_prints);
  const std::string message = log ? GuestString(base, ctx.r3.u32) : std::string();
  __imp__sub_823668E0(ctx, base);
  if (log) {
    REXLOG_WARN("cod2mp: game: error dialog \"{}\", shown as \"{}\"", message,
                GuestString(base, kErrorText));
  }
}

// Com_Error(code r3, format r4, ...) in the multiplayer program (0x82366AD0: the
// only user of a string the PC build's Com_Error alone uses).
REX_EXTERN(__imp__sub_82366AD0);
REX_HOOK_RAW(sub_82366AD0) {
  if (REXCVAR_GET(cod2_log_game_prints)) {
    const std::string format = GuestString(base, ctx.r4.u32);
    const size_t percent = format.find('%');
    const bool first_is_text = percent != std::string::npos && format.compare(percent, 2, "%s") == 0;
    const std::string first = first_is_text ? GuestString(base, ctx.r5.u32) : std::string();
    REXLOG_WARN("cod2mp: game: Com_Error code {}: \"{}\"{}{}", ctx.r3.s32, format,
                first.empty() ? "" : ", first argument \"", first.empty() ? "" : first + "\"");
  }
  __imp__sub_82366AD0(ctx, base);
}

REXCVAR_DEFINE_BOOL(cod2_debug_skip_mic, false, "cod2/Debug",
                    "Tests (cod2mp): answer the game's \"enable the microphone\" step with \"no microphone\" "
                    "instead of running it");

// "Enabling mic on game port %i" (0x822DF240, user index r3), from the System
// Link and Controls items' `xsignin` (0x822E95C8, once the profile is signed
// in). For a user not yet enabled it calls the voice engine (the object at
// 0x8467A294) through its table: +0x20 (likely RegisterLocalTalker), then
// +0x0C with the voice-chat mode (likely StartLocalProcessingModes), and +0x24
// to undo the first when the second fails. It returns 1 when the microphone is
// on, 0 when not. Both of the game's callers ignore the answer.
// This logs the engine and the answer; with cod2_debug_skip_mic it answers 0
// (no microphone) without running it. Runs of 11-07 22:03: it runs once at
// startup for user 0 and answers 1, and System Link doesn't reach it again, so
// it isn't why the game ends there.
constexpr uint32_t kVoiceEngine = 0x8467A294u;
REX_EXTERN(__imp__sub_822DF240);
REX_HOOK_RAW(sub_822DF240) {
  const uint32_t user = ctx.r3.u32;
  const bool skip = REXCVAR_GET(cod2_debug_skip_mic);
  const uint32_t engine = LoadU32(base, kVoiceEngine);
  const uint32_t table = IsDataAddress(engine) ? LoadU32(base, engine) : 0;
  if (IsDataAddress(table)) {
    REXLOG_INFO("cod2mp: game: enabling the microphone for user {}: voice engine {:08X}, its table {:08X} "
                "(+0x0C {:08X}, +0x20 {:08X}, +0x24 {:08X}){}",
                user, engine, table, LoadU32(base, table + 0x0C), LoadU32(base, table + 0x20),
                LoadU32(base, table + 0x24), skip ? "; skipped (test)" : "");
  } else {
    REXLOG_INFO("cod2mp: game: enabling the microphone for user {}: voice engine {:08X}, table {:08X}{}", user,
                engine, table, skip ? "; skipped (test)" : "");
  }
  if (skip) {
    ctx.r3.u64 = 0;
    return;
  }
  __imp__sub_822DF240(ctx, base);
  REXLOG_INFO("cod2mp: game: enabling the microphone for user {} returned {}", user, ctx.r3.u32);
}

REXCVAR_DEFINE_BOOL(cod2_debug_log_commands, false, "cod2/Debug",
                    "Tests (cod2mp): log every console command the game runs, and Sys_Quit");

// Cmd_ExecuteString 0x8236A4C0 (as the PC build's 004214C0 in the call graph):
// each console command the game runs, menu scripts' "exec" ones included. The
// text is in r3 (run 20261107-221835-cod2mp: the config's "set ..." lines
// there; r4 holds the rest of the command buffer). Blank lines aren't logged.
REX_EXTERN(__imp__sub_8236A4C0);
REX_HOOK_RAW(sub_8236A4C0) {
  if (REXCVAR_GET(cod2_debug_log_commands)) {
    const std::string text = GuestString(base, ctx.r3.u32);
    if (!text.empty() && Allowed()) REXLOG_INFO("cod2mp: game: command \"{}\"", text.substr(0, 200));
  }
  __imp__sub_8236A4C0(ctx, base);
}

// Sys_Quit 0x822E8458: the game ending itself.
REX_EXTERN(__imp__sub_822E8458);
REX_HOOK_RAW(sub_822E8458) {
  REXLOG_WARN("cod2mp: game: Sys_Quit");
  __imp__sub_822E8458(ctx, base);
}

#endif
