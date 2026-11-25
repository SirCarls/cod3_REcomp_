// bo1 - Call of Duty: Black Ops (Xbox 360) static recompilation: hooks in the translated code. [waw-decomp]
//
// Each function here is named by a [[midasm_hook]] block in config/default.toml, which says where the codegen calls
// it and why. They are World at War's (../recomp_waw/src/waw_hooks.cpp), carried to Black Ops' addresses through
// the pairing of the two games' programs (dev/waw-decomp/bo1_360/pairs_sp.tsv), each checked against Black Ops' own
// code (and KisakBlack, read as information only, for the arguments).

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/memory/utils.h>
#include <rex/system.h>

#include "generated/default/bo1_pch.h"

REXCVAR_DEFINE_STRING(bo1_debug_command, "", "bo1",
                      "Debug: a console command the game runs once, bo1_debug_command_delay seconds after its first "
                      "frame (for example \"devmap cuba\")");
REXCVAR_DEFINE_INT32(bo1_debug_command_delay, 30, "bo1", "Debug: seconds from the first frame to bo1_debug_command");
REXCVAR_DEFINE_STRING(bo1_debug_commands, "", "bo1",
                      "Debug: more console commands at set times, \"<seconds>:<command>|<seconds>:<command>...\", "
                      "the seconds from the first frame (for example \"15:devmap cuba|80:+forward|110:-forward\")");
REXCVAR_DEFINE_BOOL(bo1_log_menus, true, "bo1", "Log each menu the game opens (\"bo1: menu open <name>\")");
REXCVAR_DEFINE_BOOL(bo1_auto_save_device, true, "bo1",
                    "Campaign: choose the save device once (the game's own select_save_device command; the SDK "
                    "answers the device selector with its one device), when the main menu first opens or before the "
                    "first debug command, so a level never opens the \"Game save Error\" popup because no device "
                    "was chosen");
REXCVAR_DEFINE_INT32(bo1_log_frames, 10, "bo1",
                     "Log how many frames the main loop ran every this many seconds (\"bo1: frames ...\"; 0: off)");
REXCVAR_DEFINE_INT32(bo1_move_snap_hz, 60, "bo1",
                     "The player's velocity is cut to whole units (Sys_SnapVector, toward zero on the 360) at the end "
                     "of every movement step, so each step takes up to 1 unit/s off a jump's upward speed and more "
                     "steps make a lower jump. N (20-1000): cut it only on the steps that cross a boundary of a fixed "
                     "N Hz clock on the command time, so the jump stops depending on the frame rate. 60 (the "
                     "default): the 360's 60 Hz display, the jump the console gives at any frame rate. 0: the "
                     "game's own, every step");

DECLARE_REX_FUNC(sub_8232EA30);  // Cbuf_AddText(local client r3, text r4)

namespace {

// The name a guest pointer points to, or an empty string when it isn't one: printable characters, 1 to 63 of them.
uint32_t ReadGuestU32(const uint8_t* base, uint32_t address) {
  uint32_t value;
  std::memcpy(&value, base + address, sizeof(value));
  return __builtin_bswap32(value);
}

std::string GuestName(const uint8_t* base, uint32_t address) {
  constexpr size_t kMaxName = 64;
  if (address < 0x10000) return {};
  const char* p = reinterpret_cast<const char*>(base + address);
  const size_t n = strnlen(p, kMaxName);
  if (n == 0 || n == kMaxName) return {};
  for (size_t i = 0; i < n; ++i) {
    if (p[i] < 0x20 || p[i] > 0x7E) return {};
  }
  return std::string(p, n);
}

// Calls Cbuf_AddText(0, command + "\n") from inside a translated function: the text goes on the guest stack below
// the current frame (the callee's own frame is made below that), and every register is put back afterwards. The same
// as World at War's; Black Ops' Cbuf_AddText is 0x8232EA30 (World at War's 0x82238978 in the pairing: 31 of its 32
// paired callers call it; KisakBlack: Cbuf_AddText(localClientNum, text)).
void QueueConsoleCommand(PPCContext& ctx, uint8_t* base, const std::string& command) {
  constexpr uint32_t kRoom = 0x800;        // stack taken for the call
  constexpr uint32_t kTextOffset = 0x200;  // the text, above the callee's frame and its argument area
  std::string line = command.substr(0, kRoom - kTextOffset - 2) + "\n";
  const PPCContext saved = ctx;
  const uint32_t frame = (ctx.r1.u32 - kRoom) & ~0xFu;
  std::memcpy(base + frame + kTextOffset, line.c_str(), line.size() + 1);
  ctx.r1.u64 = frame;
  ctx.r3.u64 = 0;
  ctx.r4.u64 = frame + kTextOffset;
  sub_8232EA30(ctx, base);
  ctx = saved;
}

// The velocity cuts since the last frame line (bo1_move_snap below): done as the game does, skipped by
// bo1_move_snap_hz, and done because the step was not the one recorded (none should be).
struct SnapCounts {
  std::atomic<uint64_t> done{0};
  std::atomic<uint64_t> skipped{0};
  std::atomic<uint64_t> unmatched{0};
};
SnapCounts g_snaps;

uint64_t TakeSnapCount(std::atomic<uint64_t>& count) { return count.exchange(0, std::memory_order_relaxed); }

// The save device (bo1_auto_save_device), as Call of Duty 4's port does (../recomp_cod4/src/cod4_hooks.cpp,
// cod4_auto_save_device). On the console the player picks it at the title screen; a level reached any other way
// (devmap, a test, the port's own flow) has none, and its level-start checkpoint save opens popup_savegame_error,
// then popup_selectsavedevice_warning and the pause menu (waw-bo1-boot6, Cuba, 2026-11-09). The game's own
// select_save_device command asks the device selector, which the SDK answers at once with its one device (the
// userdata folder; Call of Duty 3's port relies on the same answer). Queued once: when the main menu first opens
// (bo1_menu_open sets the flag; the frame hook queues it, outside the menu code), or before the first debug command.
std::atomic<bool> g_main_menu_seen{false};
bool g_save_device_done = false;

void EnsureSaveDevice(PPCContext& ctx, uint8_t* base) {
  if (g_save_device_done || !REXCVAR_GET(bo1_auto_save_device)) return;
  g_save_device_done = true;
  QueueConsoleCommand(ctx, base, "select_save_device");
  REXLOG_INFO("bo1: save device: select_save_device queued");
}

}  // namespace

// Com_Frame (0x82336208, World at War's 0x8223D8C0 in the pairing; it calls setjmp 0x825FB3A0 for its error return):
// once a frame on the main thread. Logs the frame count every bo1_log_frames seconds and runs the debug commands.
void bo1_frame_start(PPCContext& ctx, uint8_t* base) {
  using clock = std::chrono::steady_clock;
  static const auto first_frame = clock::now();
  const auto now = clock::now();

  static uint64_t frames = 0;
  ++frames;
  if (const int32_t every = REXCVAR_GET(bo1_log_frames); every > 0) {
    static auto last_log = first_frame;
    static uint64_t last_frames = 0;
    const double seconds = std::chrono::duration<double>(now - last_log).count();
    if (seconds >= every) {
      REXLOG_INFO("bo1: frames over {:.2f} s: {} ({:.1f}/s); velocity cuts done {}, skipped {}, unmatched {}",
                  seconds, frames - last_frames, (frames - last_frames) / seconds, TakeSnapCount(g_snaps.done),
                  TakeSnapCount(g_snaps.skipped), TakeSnapCount(g_snaps.unmatched));
      last_log = now;
      last_frames = frames;
    }
  }

  // bo1_debug_commands: parsed once; each entry runs once, in order, when its time has come.
  struct TimedCommand {
    double seconds;
    std::string command;
  };
  static const std::vector<TimedCommand> timed = [] {
    std::vector<TimedCommand> out;
    const std::string all = REXCVAR_GET(bo1_debug_commands);
    size_t start = 0;
    while (start < all.size()) {
      size_t end = all.find('|', start);
      if (end == std::string::npos) end = all.size();
      const std::string entry = all.substr(start, end - start);
      start = end + 1;
      const size_t colon = entry.find(':');
      if (colon == std::string::npos || colon == 0 || colon + 1 >= entry.size()) continue;
      out.push_back({std::strtod(entry.c_str(), nullptr), entry.substr(colon + 1)});
    }
    return out;
  }();
  if (g_main_menu_seen.load(std::memory_order_relaxed)) EnsureSaveDevice(ctx, base);

  static size_t next_timed = 0;
  const double since_first = std::chrono::duration<double>(now - first_frame).count();
  while (next_timed < timed.size() && since_first >= timed[next_timed].seconds) {
    EnsureSaveDevice(ctx, base);
    QueueConsoleCommand(ctx, base, timed[next_timed].command);
    REXLOG_INFO("bo1: debug command queued: {}", timed[next_timed].command);
    ++next_timed;
  }

  static bool command_done = false;
  if (command_done) return;
  if (now - first_frame < std::chrono::seconds(REXCVAR_GET(bo1_debug_command_delay))) return;
  command_done = true;
  const std::string command = REXCVAR_GET(bo1_debug_command);
  if (command.empty()) return;
  EnsureSaveDevice(ctx, base);
  QueueConsoleCommand(ctx, base, command);
  REXLOG_INFO("bo1: debug command queued: {}", command);
}

// Menus_OpenByName (0x8236DF18, World at War's 0x8225FDF0 in the pairing: all 8 of its paired callers call it). In
// Black Ops it takes the local client first (KisakBlack: Menus_OpenByName(localClientNum, dc, name)), so the name is
// in r5; World at War's has no client number and keeps it in r4. Each menu the game opens goes in the log, so a test
// can tell which screen it is on.
void bo1_menu_open(PPCContext& ctx, uint8_t* base) {
  std::string name = GuestName(base, ctx.r5.u32);
  if (name.empty()) name = GuestName(base, ctx.r4.u32);
  if (name.empty()) return;
  if (name == "main") g_main_menu_seen.store(true, std::memory_order_relaxed);
  if (!REXCVAR_GET(bo1_log_menus)) return;
  REXLOG_INFO("bo1: menu open {}", name);
}

// UI_SetActiveMenu (0x82373528; x360-family's name from KisakBlack, and World at War's 0x82267380 in the pairing; r3
// the local client, r4 the front end's state, a number): in the log beside the menus.
void bo1_set_active_menu(PPCContext& ctx, uint8_t* base) {
  (void)base;
  if (!REXCVAR_GET(bo1_log_menus)) return;
  REXLOG_INFO("bo1: UI_SetActiveMenu({}, {})", ctx.r3.s32, ctx.r4.s32);
}

// `bo1_move_snap_hz`: the jump height at any frame rate, World at War's `waw_move_snap_hz` (../recomp_waw/src/
// waw_hooks.cpp, where the measurements are: with it at 60, 37.8-38.3 units at 500 fps and 37.9-38.2 at 60 fps;
// without it, 31.3-31.4 at 500 fps). PmoveSingle (0x82153A10) ends with Sys_SnapVector(ps->velocity), a cut
// toward zero to whole units on every movement step; while a jump rises each cut takes up to 1 unit/s of upward speed,
// so the more steps a jump has (the higher the frame rate), the lower it ends. With N > 0 the cut runs only on a step
// whose end crosses a boundary of a fixed N Hz clock on the command time (floor(end * N / 1000) > floor(start * N /
// 1000)), so a jump gets the same cuts at any frame rate. The server's moves and the client's prediction use the same
// command times, so the two agree. Black Ops' code was checked for each address (2026-11-09, the generated code of
// waw-bo1-codegen2): the same loop and the same single call as World at War's, at its own addresses; playerState's
// commandTime is at +0 and the velocity at +0x30 (PmoveSingle's `addi r28,r30,48` before the call).
namespace {
constexpr uint32_t kPlayerStateCommandTime = 0x00;
constexpr uint32_t kPlayerStateVelocity = 0x30;
constexpr uint32_t kSnapReturnAddress = 0x82154D54;  // after PmoveSingle's `bl 0x823C51A8`

// The step this thread's Pmove is about to run, for the cut at the end of that step's PmoveSingle. The server thread
// and the main thread (prediction) each have their own.
struct MoveStepRecord {
  int32_t start = 0;
  int32_t end = 0;
  bool valid = false;
};
thread_local MoveStepRecord t_move_step;

int64_t FloorDiv(int64_t value, int64_t divisor) {
  const int64_t quotient = value / divisor;
  return (value % divisor != 0 && (value < 0) != (divisor < 0)) ? quotient - 1 : quotient;
}
}  // namespace

// Pmove at 0x82154E54, after `add r11,r10,r11`: r10 the step's start, r11 its end.
void bo1_move_step(PPCContext& ctx, uint8_t* base) {
  (void)base;
  t_move_step = {ctx.r10.s32, ctx.r11.s32, true};
}

// Sys_SnapVector 0x823C51A8 (r3 the vector): returns true to skip the cut.
bool bo1_move_snap(PPCContext& ctx, uint8_t* base) {
  const uint32_t velocity = ctx.r3.u32;
  if (static_cast<uint32_t>(ctx.lr) != kSnapReturnAddress || velocity < 0x10000 + kPlayerStateVelocity) return false;
  const uint32_t ps = velocity - kPlayerStateVelocity;
  const int32_t hz = REXCVAR_GET(bo1_move_snap_hz);
  bool skip = false;
  if (hz > 0) {
    const auto command_time = static_cast<int32_t>(ReadGuestU32(base, ps + kPlayerStateCommandTime));
    if (t_move_step.valid && t_move_step.end == command_time) {
      const int64_t rate = std::clamp(hz, 20, 1000);
      skip = FloorDiv(int64_t{t_move_step.end} * rate, 1000) == FloorDiv(int64_t{t_move_step.start} * rate, 1000);
    } else {
      g_snaps.unmatched.fetch_add(1, std::memory_order_relaxed);
    }
  }
  t_move_step.valid = false;
  (skip ? g_snaps.skipped : g_snaps.done).fetch_add(1, std::memory_order_relaxed);
  return skip;
}
