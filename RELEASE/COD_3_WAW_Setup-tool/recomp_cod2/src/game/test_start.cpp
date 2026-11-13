// cod2 - test runs that get into the game by themselves. [acct2-cod2]
//
// Test runs are muted and their window sits behind the one in use, so nobody
// presses START at the game's "Press START" screen (the main_lockout menu)
// and nobody picks a level. For tests only, two settings do it; both are off
// by default and each acts once per run:
//
//   cod2_debug_press_start    buttons pressed on the first controller for a
//                             moment, as a player would: START 2 s after the
//                             Press START screen opens (the game then takes
//                             that controller and asks for a storage device,
//                             which its saves need; the SDK's device selector
//                             answers with the hard drive), and A 3 s after a
//                             level's loading screen says it is ready (the
//                             "pregame" menu), which starts the level. (START
//                             there would open the objectives screen.)
//   cod2_debug_start_command  a console command (e.g. "map moscow") handed to
//                             the game's own command buffer (Cbuf_AddText)
//                             cod2_debug_start_delay seconds after the Press
//                             START screen opened; the game runs it on its
//                             next frame, as if typed.
//   cod2_debug_level_tour     levels loaded one after another ("all" = the 27
//                             on the disc, or names with commas), each
//                             cod2_debug_level_seconds long, starting when
//                             cod2_debug_start_command would; with
//                             cod2_debug_press_start each level is started.
//                             The log says when each map command went out,
//                             when its ready screen opened, and when the game
//                             fell back to its main menu (an error).
//   cod2_debug_skip_intro     A pressed every 1.5 s until the Press START
//                             screen opens, to skip what the intro movies let
//                             a player skip.
//   cod2_debug_commands       more console commands at set times, as
//                             "SECONDS=command;SECONDS=command" (seconds after
//                             the Press START screen opened), e.g. to change
//                             com_maxfps during a run, or to move the player
//                             ("+forward", "-forward", "goprone"; letters,
//                             digits, spaces and _ . - + only).
//   cod2_debug_buttons        controller buttons pressed at set times, as
//                             "SECONDS=button;SECONDS=button" (seconds after
//                             the Press START screen opened, tenths allowed;
//                             a b x y start back up down left right lb rb),
//                             120 ms each, to walk through menus (a 300 ms
//                             press moved cod2mp's main menu two items: the
//                             menus repeat a held direction sooner).
//   cod2_debug_timing_seconds (campaign) every N seconds a log line with the
//                             frames the game ran (Com_Frame's counter), how
//                             far the server's clock went (sv.time, 50 ms
//                             steps) and the real time, to see what follows
//                             the clock and what follows the frames.
//
// Both go through the game's own controller read (XInputGetState 0x8228A6A0,
// a thunk to XamInputGetState(user, 0, state)), which the game's main thread
// calls every frame, so the command is added on the thread that runs it.

#include "game/test_start.h"

#include "cod2_f2.h"  // [f2-menus] the F2 window's console commands
#include "cod2_pc_input.h"  // [f2-menus] the PC copy's keys: their console commands
#include "game/dvar_peek.h"  // [f2-menus] tests: the game's dvars in the log (cod2_debug_dvars)
#include "game/pc_movement.h"  // [f2-menus] the PC copy's movement numbers (cod2_pc_movement; the campaign)

#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

REXCVAR_DEFINE_BOOL(cod2_debug_press_start, false, "cod2/Debug",
                    "Tests: press START at the Press START screen, and A when a level has "
                    "loaded, as a player would");
REXCVAR_DEFINE_STRING(cod2_debug_start_command, "", "cod2/Debug",
                      "Tests: a console command the game runs once, cod2_debug_start_delay "
                      "seconds after its Press START screen opens (e.g. map moscow); empty = none");
REXCVAR_DEFINE_INT32(cod2_debug_start_delay, 8, "cod2/Debug",
                     "Tests: seconds from the Press START screen to cod2_debug_start_command")
    .range(0, 600);
REXCVAR_DEFINE_STRING(cod2_debug_level_tour, "", "cod2/Debug",
                      "Tests: levels to load one after another, each cod2_debug_level_seconds "
                      "long: \"all\" (the disc's 27) or map names with commas; empty = none");
REXCVAR_DEFINE_INT32(cod2_debug_level_seconds, 150, "cod2/Debug",
                     "Tests: seconds for each level of cod2_debug_level_tour, its loading included")
    .range(20, 3600);
REXCVAR_DEFINE_BOOL(cod2_debug_skip_intro, false, "cod2/Debug",
                    "Tests: press A every 1.5 s until the Press START screen opens");
REXCVAR_DEFINE_STRING(cod2_debug_commands, "", "cod2/Debug",
                      "Tests: console commands at set times: SECONDS=command;SECONDS=command "
                      "(seconds after the Press START screen opened)");
REXCVAR_DEFINE_STRING(cod2_debug_buttons, "", "cod2/Debug",
                      "Tests: controller buttons at set times: SECONDS=button;... (seconds after the Press "
                      "START screen opened; a b x y start back up down left right lb rb)");
REXCVAR_DEFINE_INT32(cod2_debug_timing_seconds, 0, "cod2/Debug",
                     "Tests (campaign): log frames, server time and real time every N s; 0 = off")
    .range(0, 600);

// The two game functions used, in each program (cod2mp's translate to the
// same code as the campaign's: find_fn.py MATCH). The second macro level makes
// the address names expand before the SDK's macros paste them.
#if defined(COD2_MP)
#define COD2_CBUF_ADD_TEXT sub_82369CE0
#define COD2_XINPUT_GET_STATE sub_822897B0
// cod2mp has no Press START screen: its main menu ("main_text") is the start.
constexpr const char* kStartMenu = "main_text";
#else
#define COD2_CBUF_ADD_TEXT sub_8233EAB8
#define COD2_XINPUT_GET_STATE sub_8228A6A0
constexpr const char* kStartMenu = "main_lockout";
#endif
#define COD2_IMP_(name) __imp__##name
#define COD2_IMP(name) COD2_IMP_(name)
#define COD2_EXTERN(name) REX_EXTERN(COD2_IMP(name))
#define COD2_HOOK_RAW(name) REX_HOOK_RAW(name)

COD2_EXTERN(COD2_CBUF_ADD_TEXT);     // Cbuf_AddText(text r3): appends to the command buffer
COD2_EXTERN(COD2_XINPUT_GET_STATE);  // XInputGetState(user r3, state r4) -> error code r3

namespace {

using Clock = std::chrono::steady_clock;

constexpr uint16_t kButtonStart = 0x0010;  // XINPUT_GAMEPAD_START
constexpr uint16_t kButtonA = 0x1000;      // XINPUT_GAMEPAD_A
constexpr int64_t kPressMs = 300;          // how long a press lasts (Press START, the intro skip)
constexpr int64_t kTimedPressMs = 120;     // cod2_debug_buttons: short enough not to repeat

// When the Press START screen and a level's "ready" screen last opened (ms on
// the steady clock; -1 = not yet).
std::atomic<int64_t> g_lockout_ms{-1};
std::atomic<int64_t> g_pregame_ms{-1};

// The presses: after which screen, how long after it opened, which button.
struct Press {
  std::atomic<int64_t>* opened;
  int64_t after_ms;
  uint16_t button;
  const char* what;
  std::atomic<bool> logged{false};
};
Press g_presses[] = {
    {&g_lockout_ms, 2000, kButtonStart, "START at the Press START screen"},
    {&g_pregame_ms, 3000, kButtonA, "A at the level's ready screen"},
};
std::atomic<bool> g_command_sent{false};
std::atomic<uint32_t> g_results_logged{0};  // one bit per controller
std::atomic<int64_t> g_first_read_ms{-1};    // the game's first controller read

// The level tour (cod2_debug_level_tour): the levels, the one being played
// (-1 = not started) and when its map command went out.
constexpr const char* kDiscLevels[] = {
    "moscow",          "demolition",       "tankhunt",        "cityhall",       "eldaba",
    "elalamein",       "libya",            "toujane_ride",    "toujane",        "matmata",
    "88ridge",         "duhoc_assault",    "duhoc_defend",    "silotown_assault", "beltot",
    "crossroads",      "newvillers",       "breakout",        "bergstein",      "hill400_assault",
    "hill400_defend",  "rhine",            "trainyard",       "downtown_assault", "downtown_sniper",
    "decoytown",       "decoytrenches"};
std::vector<std::string> g_tour;
// cod2_debug_commands, parsed once: (ms after the Press START screen, command).
std::vector<std::pair<int64_t, std::string>> g_timed;
size_t g_timed_next = 0;
bool g_timed_parsed = false;
// cod2_debug_timing_seconds: the last sample (-1 = none yet).
int64_t g_timing_wall_ms = -1;
uint32_t g_timing_frames = 0, g_timing_server_ms = 0;
std::atomic<int> g_tour_index{-1};
std::atomic<int64_t> g_tour_ms{-1};

int64_t NowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch())
      .count();
}

// The tour's levels from the setting: "all", or names separated by commas
// (letters, digits and '_' only, so nothing else reaches the command line).
std::vector<std::string> ParseTour(const std::string& setting) {
  std::vector<std::string> levels;
  if (setting == "all") {
    for (const char* name : kDiscLevels) levels.emplace_back(name);
    return levels;
  }
  std::string name;
  for (size_t i = 0; i <= setting.size(); ++i) {
    const char c = i < setting.size() ? setting[i] : ',';
    if (c == ',' || c == ' ') {
      if (!name.empty() && name.size() < 40) levels.push_back(name);
      name.clear();
    } else if (std::isalnum(static_cast<unsigned char>(c)) || c == '_') {
      name.push_back(c);
    }
  }
  return levels;
}

// "SECONDS=command;SECONDS=command", commands kept to letters, digits, '_',
// '.', '-' and spaces (nothing else reaches the command line).
std::vector<std::pair<int64_t, std::string>> ParseTimed(const std::string& setting) {
  std::vector<std::pair<int64_t, std::string>> out;
  size_t start = 0;
  while (start < setting.size()) {
    size_t end = setting.find(';', start);
    if (end == std::string::npos) end = setting.size();
    const std::string item = setting.substr(start, end - start);
    start = end + 1;
    const size_t eq = item.find('=');
    if (eq == std::string::npos || eq == 0) continue;
    int64_t secs = 0;
    bool ok = true;
    for (size_t i = 0; i < eq; ++i) {
      if (!std::isdigit(static_cast<unsigned char>(item[i]))) ok = false;
      else secs = secs * 10 + (item[i] - '0');
    }
    std::string command = item.substr(eq + 1);
    for (char c : command)
      if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_' && c != '.' && c != '-' && c != '+' &&
          c != ' ')
        ok = false;
    if (ok && !command.empty() && command.size() < 120 && secs < 100000)
      out.emplace_back(secs * 1000, command);
  }
  return out;
}

// cod2_debug_buttons, parsed once: (ms after the Press START screen, button
// bits, name). The XINPUT_GAMEPAD_* bits.
struct TimedButton {
  int64_t at_ms;
  uint16_t bits;
  std::string name;
  bool logged = false;
};
std::vector<TimedButton> ParseButtons(const std::string& setting) {
  static const std::pair<const char*, uint16_t> kNames[] = {
      {"up", 0x0001},    {"down", 0x0002}, {"left", 0x0004}, {"right", 0x0008},
      {"start", 0x0010}, {"back", 0x0020}, {"lb", 0x0100},   {"rb", 0x0200},
      {"a", 0x1000},     {"b", 0x2000},    {"x", 0x4000},    {"y", 0x8000}};
  std::vector<TimedButton> out;
  size_t start = 0;
  while (start < setting.size()) {
    size_t end = setting.find(';', start);
    if (end == std::string::npos) end = setting.size();
    const std::string item = setting.substr(start, end - start);
    start = end + 1;
    const size_t eq = item.find('=');
    if (eq == std::string::npos || eq == 0) continue;
    int64_t tenths = 0;
    bool ok = true, fraction = false;
    int fraction_digits = 0;
    for (size_t i = 0; i < eq; ++i) {
      const char c = item[i];
      if (c == '.' && !fraction) {
        fraction = true;
      } else if (std::isdigit(static_cast<unsigned char>(c)) && (!fraction || fraction_digits == 0)) {
        tenths = tenths * 10 + (c - '0');
        if (fraction) ++fraction_digits;
      } else {
        ok = false;
      }
    }
    if (!fraction_digits) tenths *= 10;
    const std::string name = item.substr(eq + 1);
    uint16_t bits = 0;
    for (const auto& [n, b] : kNames)
      if (name == n) bits = b;
    if (ok && bits && tenths < 1000000) out.push_back({tenths * 100, bits, name});
  }
  return out;
}
std::vector<TimedButton> g_buttons;
bool g_buttons_parsed = false;

uint32_t LoadBE32(const uint8_t* p) {
  uint32_t v;
  std::memcpy(&v, p, 4);
  return __builtin_bswap32(v);
}
void StoreBE32(uint8_t* p, uint32_t v) {
  v = __builtin_bswap32(v);
  std::memcpy(p, &v, 4);
}
uint16_t LoadBE16(const uint8_t* p) {
  uint16_t v;
  std::memcpy(&v, p, 2);
  return __builtin_bswap16(v);
}
void StoreBE16(uint8_t* p, uint16_t v) {
  v = __builtin_bswap16(v);
  std::memcpy(p, &v, 2);
}

// Appends `command` and a line end to the game's command buffer, from inside a
// hook on the game's own thread. The text goes on the guest stack below the
// current frame, with a fresh frame below it for the call; every register is
// put back afterwards, so the hooked function's caller sees no difference.
void AddCommandText(PPCContext& ctx, uint8_t* base, const std::string& command) {
  const std::string text = command + "\n";
  const PPCContext saved = ctx;
  const uint32_t buffer = (ctx.r1.u32 - 0x200 - uint32_t(text.size())) & ~0xFu;
  std::memcpy(base + buffer, text.c_str(), text.size() + 1);
  ctx.r1.u32 = buffer - 0x100;
  ctx.r3.u64 = buffer;
  COD2_IMP(COD2_CBUF_ADD_TEXT)(ctx, base);
  ctx = saved;
  ctx.fpscr.restoreGuestBits(saved.fpscr.csr);
}

}  // namespace

namespace cod2::test_start {

void OnMenuOpened(std::string_view name) {
  if (name == kStartMenu) {
    int64_t expected = -1;
    if (g_lockout_ms.compare_exchange_strong(expected, NowMs()) &&
        (REXCVAR_GET(cod2_debug_press_start) || !REXCVAR_GET(cod2_debug_start_command).empty()))
      REXLOG_INFO("cod2: test start: the Press START screen is open");
  } else if (name == "pregame") {
    g_pregame_ms.store(NowMs());
    for (Press& p : g_presses)
      if (p.opened == &g_pregame_ms) p.logged.store(false);  // each level's screen again
  }
  const int index = g_tour_index.load();
  if (index >= 0 && index < int(g_tour.size()) && (name == "pregame" || name == "main")) {
    const double secs = double(NowMs() - g_tour_ms.load()) / 1000.0;
    if (name == "pregame")
      REXLOG_INFO("cod2: tour: \"{}\" ready (pregame) {:.1f} s after its map command",
                  g_tour[index], secs);
    else
      REXLOG_WARN("cod2: tour: back at the main menu {:.1f} s into \"{}\"", secs, g_tour[index]);
  }
}

}  // namespace cod2::test_start

COD2_HOOK_RAW(COD2_XINPUT_GET_STATE) {
  const uint32_t user = ctx.r3.u32, state = ctx.r4.u32;
  COD2_IMP(COD2_XINPUT_GET_STATE)(ctx, base);

  // [f2-menus] Commands from the F2 window (typed on its CONSOLE page, or a row that sets one of the game's dvars):
  // added to the game's command buffer here, on the game's main thread, as the test commands below are.
  if (user == 0 && cod2::f2ui::HasCommands()) {
    static std::vector<std::string> f2_commands;
    cod2::f2ui::TakeCommands(f2_commands);
    for (const std::string& command : f2_commands) AddCommandText(ctx, base, command);
    f2_commands.clear();
  }
  // [f2-menus] The PC copy's keys and mouse (cod2_pc_input.h): once a read of the first controller it looks at
  // whether a level is playing, and the commands of the keys pressed since the last read go to the command buffer.
  if (user == 0) {
    cod2::dvar_peek::OnGameFrame(ctx, base);  // tests: cod2_debug_dvars
#if !defined(COD2_MP)
    cod2::pc_movement::OnGameFrame(ctx, base);  // cod2_pc_movement
#endif
    cod2::pc_input::OnGameFrame(base);
    static std::vector<std::string> key_commands;
    if (cod2::pc_input::TakeCommands(key_commands)) {
      for (const std::string& command : key_commands) AddCommandText(ctx, base, command);
      key_commands.clear();
    }
  }

  // What each controller answers, once (0 = connected, 0x48F = not connected).
  if (user < 4 && !(g_results_logged.fetch_or(1u << user) & (1u << user)))
    REXLOG_INFO("cod2: XInputGetState controller {} -> {:#x}", user, ctx.r3.u32);

  const int64_t opened = g_lockout_ms.load(std::memory_order_relaxed);
  if (opened < 0) {
    // Before the Press START screen: A every 1.5 s, for 300 ms, when asked.
    const int64_t now = NowMs();
    int64_t first = g_first_read_ms.load(std::memory_order_relaxed);
    if (first < 0) {
      g_first_read_ms.compare_exchange_strong(first, now);
      first = g_first_read_ms.load();
    }
    if (REXCVAR_GET(cod2_debug_skip_intro) && user == 0 && state && (now - first) % 1500 < kPressMs) {
      uint8_t* s = base + state;
      if (ctx.r3.u32 != 0) {
        std::memset(s, 0, 16);
        ctx.r3.u64 = 0;
      }
      StoreBE32(s, LoadBE32(s) + 1);
      StoreBE16(s + 4, LoadBE16(s + 4) | kButtonA);
    }
    return;
  }
  const int64_t now = NowMs(), since = now - opened;

  if (REXCVAR_GET(cod2_debug_press_start) && user == 0 && state) {
    for (Press& p : g_presses) {
      const int64_t at = p.opened->load(std::memory_order_relaxed);
      if (at < 0 || now < at + p.after_ms || now >= at + p.after_ms + kPressMs) continue;
      uint8_t* s = base + state;
      if (ctx.r3.u32 != 0) {  // no controller: answer as a connected one at rest
        std::memset(s, 0, 16);
        ctx.r3.u64 = 0;
      }
      StoreBE32(s, LoadBE32(s) + 1);  // dwPacketNumber: the state changed
      StoreBE16(s + 4, LoadBE16(s + 4) | p.button);
      if (!p.logged.exchange(true)) REXLOG_INFO("cod2: test start: {} pressed", p.what);
    }
  }

  // Timed buttons (cod2_debug_buttons), each held for kTimedPressMs.
  if (user == 0 && state) {
    if (!g_buttons_parsed) {
      g_buttons_parsed = true;
      g_buttons = ParseButtons(REXCVAR_GET(cod2_debug_buttons));
    }
    for (TimedButton& b : g_buttons) {
      if (since < b.at_ms || since >= b.at_ms + kTimedPressMs) continue;
      uint8_t* s = base + state;
      if (ctx.r3.u32 != 0) {
        std::memset(s, 0, 16);
        ctx.r3.u64 = 0;
      }
      StoreBE32(s, LoadBE32(s) + 1);
      StoreBE16(s + 4, LoadBE16(s + 4) | b.bits);
      if (!b.logged) {
        b.logged = true;
        REXLOG_INFO("cod2: test: button {} pressed", b.name);
      }
    }
  }

  if (!g_command_sent.load(std::memory_order_relaxed) &&
      since >= int64_t(REXCVAR_GET(cod2_debug_start_delay)) * 1000) {
    const std::string command = REXCVAR_GET(cod2_debug_start_command);
    if (!g_command_sent.exchange(true)) {
      g_tour = ParseTour(REXCVAR_GET(cod2_debug_level_tour));
      if (!g_tour.empty()) {
        REXLOG_INFO("cod2: tour: {} levels, {} s each", g_tour.size(),
                    REXCVAR_GET(cod2_debug_level_seconds));
        g_tour_index.store(-1);
        g_tour_ms.store(now - int64_t(REXCVAR_GET(cod2_debug_level_seconds)) * 1000);
      } else if (!command.empty() && command.size() < 200) {
        REXLOG_INFO("cod2: test start: command \"{}\" sent to the game", command);
        AddCommandText(ctx, base, command);
      }
    }
  }

  // Timed commands (cod2_debug_commands), in order.
  if (user == 0) {
    if (!g_timed_parsed) {
      g_timed_parsed = true;
      g_timed = ParseTimed(REXCVAR_GET(cod2_debug_commands));
    }
    if (g_timed_next < g_timed.size() && since >= g_timed[g_timed_next].first) {
      const std::string& command = g_timed[g_timed_next++].second;
      REXLOG_INFO("cod2: test: timed command \"{}\" sent to the game", command);
      AddCommandText(ctx, base, command);
    }
  }

#if !defined(COD2_MP)
  // Frames against clocks (cod2_debug_timing_seconds). Com_Frame counts its
  // frames at 0x83D376B4; the server's clock (sv.time, ms, 50 ms steps in
  // SV_FrameInternal) is at 0x83FEC4CC.
  if (const int32_t every = REXCVAR_GET(cod2_debug_timing_seconds); every > 0 && user == 0) {
    const uint32_t frames = LoadBE32(base + 0x83D376B4u), server_ms = LoadBE32(base + 0x83FEC4CCu);
    if (g_timing_wall_ms < 0 || now - g_timing_wall_ms >= int64_t(every) * 1000) {
      if (g_timing_wall_ms >= 0) {
        const double wall = double(now - g_timing_wall_ms) / 1000.0;
        REXLOG_INFO("cod2: timing: {:.2f} s: {} frames ({:.1f} a second), server clock +{} ms "
                    "({:.3f} of real time)",
                    wall, frames - g_timing_frames, double(frames - g_timing_frames) / wall,
                    int32_t(server_ms - g_timing_server_ms),
                    double(int32_t(server_ms - g_timing_server_ms)) / (wall * 1000.0));
      }
      g_timing_wall_ms = now;
      g_timing_frames = frames;
      g_timing_server_ms = server_ms;
    }
  }
#endif

  // The tour's next level when the current one's time is up.
  if (!g_tour.empty() && user == 0 &&
      now - g_tour_ms.load(std::memory_order_relaxed) >=
          int64_t(REXCVAR_GET(cod2_debug_level_seconds)) * 1000) {
    const int next = g_tour_index.load() + 1;
    g_tour_ms.store(now);
    g_tour_index.store(next);
    if (next < int(g_tour.size())) {
      REXLOG_INFO("cod2: tour: level {}/{} \"{}\": map command sent", next + 1, g_tour.size(),
                  g_tour[next]);
      AddCommandText(ctx, base, "map " + g_tour[next]);
    } else if (next == int(g_tour.size())) {
      REXLOG_INFO("cod2: tour: done ({} levels)", g_tour.size());
    }
  }
}
