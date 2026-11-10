// cod4 / cod4mp - Call of Duty 4: Modern Warfare (Xbox 360) static recompilation: hooks in the translated code.
// [acct2-cod4]
//
// Each function here is named by a [[midasm_hook]] block in config/default.toml (the campaign) or
// mp/config/default.toml (multiplayer), which says where the codegen calls it and why. The engine's names are
// x360-family's for CoD4's 360 programs (dev/x360-family/cod4_360/<sp|mp>/names.tsv), checked in the generated code.

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <string_view>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

#ifdef COD4_MP
#include "generated/default_mp/cod4mp_pch.h"
#else
#include "generated/default/cod4_pch.h"
#endif

REXCVAR_DEFINE_BOOL(cod4_log_console, true, "cod4",
                    "Copy the game's console output into the log (\"cod4: [console N]\")");
REXCVAR_DEFINE_STRING(cod4_debug_command, "", "cod4",
                      "Debug: a console command the game runs once, cod4_debug_command_delay seconds after its first "
                      "frame (for example \"devmap killhouse\")");
REXCVAR_DEFINE_INT32(cod4_debug_command_delay, 30, "cod4",
                     "Debug: seconds from the first frame to cod4_debug_command");
REXCVAR_DEFINE_BOOL(cod4_log_menus, true, "cod4", "Log each menu the game opens (\"cod4: menu open <name>\")");
#ifndef COD4_MP
REXCVAR_DEFINE_BOOL(cod4_auto_save_device, true, "cod4",
                    "Campaign: choose the save device once, a few seconds after start (the game's own "
                    "select_save_device command; the SDK answers the device selector with its one device), so a level "
                    "never opens the \"Game save Error\" popup because no device was chosen");
#endif

REXCVAR_DEFINE_DOUBLE(cod4_fov, 0.0, "cod4",
                      "Field of view in degrees for the player's view when not aiming (the game's cg_fov; the console "
                      "uses 65). 0 = the game's own. Any value is allowed, beyond the console's 65-80 limits; "
                      "aiming down the sights still zooms from it");

// cg_fov's dvar_t*, as the game registers it (found 2026-11-07 by results/acct2-cod4-scripts/dvarreads.py: the
// pointer Dvar_RegisterFloat returns is stored here; CG_CalcFov and two more read it). On the 360 a dvar's current
// value is at +12 (the reads of com_maxfps and timescale use that offset).
#ifdef COD4_MP
constexpr uint32_t kCgFovDvar = 0x823F54F8;
#else
constexpr uint32_t kCgFovDvar = 0x82488A18;
#endif
constexpr uint32_t kDvarCurrent = 12;

// Cbuf_AddText(local client r3, text r4): the console's command buffer (x360-family's name in each program).
#ifdef COD4_MP
DECLARE_REX_FUNC(sub_82237C50);
#define COD4_CBUF_ADD_TEXT sub_82237C50
#else
DECLARE_REX_FUNC(sub_821DE9D8);
#define COD4_CBUF_ADD_TEXT sub_821DE9D8
#endif

// A `bctr` that is a call through a table of function pointers, not a switch. The codegen can take these for jump
// tables (`lis/addi/lwzx/mtctr/bctr` is also how a switch looks; World at War had two). The hook stands before the
// `bctr`, when CTR already holds the target: it calls the target the way an ordinary indirect tail call does, and the
// config's `return = true` leaves the translated function right after, so the wrong switch is never reached.
void cod4_ctr_tail_call(PPCContext& ctx, uint8_t* base) {
  REX_CALL_INDIRECT_FUNC(ctx.ctr.u32);
}

// The game's console output. On this Xbox 360 build Com_Printf formats its text and hands it straight to
// CL_ConsolePrint(local client r3, channel r4, text r5, duration r6, pixel width r7, flags r8), the console's own
// printer (PC CoD4's Com_PrintMessage, the step between them, isn't in it), so the hook is there: each line goes into
// the log as "cod4: [console <channel>] text". A line the same as the one before is counted instead, and the count is
// logged before the next different line.
void cod4_console_print(PPCContext& ctx, uint8_t* base) {
  if (!REXCVAR_GET(cod4_log_console)) return;
  const uint32_t text = ctx.r5.u32;
  const int32_t channel = ctx.r4.s32;
  if (text < 0x10000) return;
  const char* p = reinterpret_cast<const char*>(base + text);
  size_t n = strnlen(p, 2048);
  while (n && (p[n - 1] == '\n' || p[n - 1] == '\r')) --n;
  if (!n) return;
  static std::mutex mutex;
  static std::string last;
  static int32_t last_channel = 0;
  static uint64_t repeats = 0;
  const std::string_view line(p, n);
  std::lock_guard<std::mutex> lock(mutex);
  if (line == last && channel == last_channel) {
    ++repeats;
    return;
  }
  if (repeats) REXLOG_INFO("cod4: [console {}] (the line before {} more time(s))", last_channel, repeats);
  repeats = 0;
  last.assign(line);
  last_channel = channel;
  REXLOG_INFO("cod4: [console {}] {}", last_channel, line);
}

void Cod4CensusFrame(uint8_t* base);  // cod4_census.cpp

namespace {

// Calls Cbuf_AddText(0, command + "\n") from inside a translated function: the text goes on the guest stack below
// the current frame (the callee's own frame is made below that), and every register is put back afterwards.
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
  COD4_CBUF_ADD_TEXT(ctx, base);
  ctx = saved;
}

// The FOV setting (cod4_fov): writes cg_fov's current value each frame, so the game's own menus, configs and
// scripts can't put it back. A value the dvar's own range refuses (65-80 on the console) is set directly.
void ApplyFov(uint8_t* base) {
  const double fov = REXCVAR_GET(cod4_fov);
  if (fov <= 0.0) return;
  uint32_t dvar;
  std::memcpy(&dvar, base + kCgFovDvar, sizeof(dvar));
  dvar = __builtin_bswap32(dvar);
  if (dvar < 0x10000) return;  // not registered yet
  const float value = static_cast<float>(std::min(fov, 160.0));
  uint32_t bits;
  std::memcpy(&bits, &value, sizeof(bits));
  bits = __builtin_bswap32(bits);
  std::memcpy(base + dvar + kDvarCurrent, &bits, sizeof(bits));
}

}  // namespace

// Once a frame on the main thread (the start of Com_Frame): in the campaign, chooses the save device once (5 s after
// the first frame); runs cod4_debug_command once, after cod4_debug_command_delay seconds, through the console buffer
// (how a test reaches a level, as World at War's).
void cod4_frame_start(PPCContext& ctx, uint8_t* base) {
  Cod4CensusFrame(base);  // the census and the audit's counters (cod4_census.cpp), off unless asked for
  ApplyFov(base);         // cod4_fov, off unless set
  static const auto first_frame = std::chrono::steady_clock::now();
  const auto since_first_frame = std::chrono::steady_clock::now() - first_frame;
#ifndef COD4_MP
  // The save device. On the console the player picks it when pressing START at the title; a level reached any other
  // way (devmap, a test, later the port's own menus) has none, and its level-start save fails with
  // GAME_SAVE_UNABLE_TO_ACCESS_DEVICE. The game's select_save_device (allowed while no game is running; it says
  // "device is already active" when one is) asks the device selector, which the SDK answers with its one device.
  // Campaign only: multiplayer keeps its data in the profile and has no such command.
  constexpr auto kSaveDeviceDelay = std::chrono::seconds(5);
  static bool save_device_done = false;
  if (!save_device_done && since_first_frame >= kSaveDeviceDelay) {
    save_device_done = true;
    if (REXCVAR_GET(cod4_auto_save_device)) {
      QueueConsoleCommand(ctx, base, "select_save_device");
      REXLOG_INFO("cod4: save device: select_save_device queued");
    }
  }
#endif
  static bool done = false;
  if (done) return;
  if (since_first_frame < std::chrono::seconds(REXCVAR_GET(cod4_debug_command_delay))) return;
  done = true;
  const std::string command = REXCVAR_GET(cod4_debug_command);
  if (command.empty()) return;
  QueueConsoleCommand(ctx, base, command);
  REXLOG_INFO("cod4: debug command queued: {}", command);
}

// Menus_OpenByName (r4 the menu's name): each menu the game opens, in the log, so a test can tell which screen it is
// on.
void cod4_menu_open(PPCContext& ctx, uint8_t* base) {
  if (!REXCVAR_GET(cod4_log_menus)) return;
  const uint32_t text = ctx.r4.u32;
  if (text < 0x10000) return;
  constexpr size_t kMaxName = 64;
  const char* p = reinterpret_cast<const char*>(base + text);
  const size_t n = strnlen(p, kMaxName);
  if (n == 0 || n == kMaxName) return;
  for (size_t i = 0; i < n; ++i) {
    if (p[i] < 0x20 || p[i] > 0x7E) return;  // not a name
  }
  REXLOG_INFO("cod4: menu open {}", std::string_view(p, n));
}

// UI_SetActiveMenu (r3 the local client, r4 the front end's state, a number): in the log beside the menus.
void cod4_set_active_menu(PPCContext& ctx, uint8_t* base) {
  (void)base;
  if (!REXCVAR_GET(cod4_log_menus)) return;
  REXLOG_INFO("cod4: UI_SetActiveMenu({}, {})", ctx.r3.s32, ctx.r4.s32);
}

// Debug (cod4_debug_stack): the guest stack pointer at a function's start, logged each time it reaches a new low (and
// the first time), to see how much of the thread's stack a load uses. Used on the script compiler's string copy
// (0x821AE290), where the first level load wrote to guest 0x70160000 (2026-11-07 18:00).
REXCVAR_DEFINE_BOOL(cod4_debug_stack, false, "cod4", "Debug: log the guest stack pointer's new lows at the traced functions");

void cod4_trace_stack(PPCContext& ctx, uint8_t* base) {
  (void)base;
  if (!REXCVAR_GET(cod4_debug_stack)) return;
  static std::mutex mutex;
  static uint32_t lowest = 0xFFFFFFFFu;
  std::lock_guard<std::mutex> lock(mutex);
  if (ctx.r1.u32 >= lowest) return;
  lowest = ctx.r1.u32;
  REXLOG_INFO("cod4: stack low r1={:08X} lr={:08X} r3={:08X} r4={:08X}", ctx.r1.u32, uint32_t(ctx.lr), ctx.r3.u32,
              ctx.r4.u32);
}

#ifndef COD4_MP
// The campaign's start-up films, left out with cod4_skip_intro_movies (default off, as CoD3's cod3_skip_movies and
// CoD2's cod2_skip_intro_movies). They come from one function, COM_PlayIntroMovies 0x821DB338, called once from the
// start-up (0x821DD488). When neither of two launch bytes is set (0x83362DB8 and 0x83362A34; with either set it
// clears the first and hands launch data back with XamLoaderSetLaunchData instead), com_introPlayed (dvar_t* at
// 0x82DED424) is false and a second switch (dvar_t* at 0x82DEC3F8) is on, it queues "cinematic legal" and puts the
// next films ("cinematic IW_logo", then the intro) in the dvar at 0x82DEC28C, which runs when that film ends. The hook
// leaves out only the films; the launch-data path stays the game's. cod4mp has no start-up films (no
// "cinematic legal" in its image).
REXCVAR_DEFINE_BOOL(cod4_skip_intro_movies, false, "cod4",
                    "Campaign: skip the start-up films (the legal screen, Infinity Ward, the intro), so the front end "
                    "comes at once");

REX_EXTERN(__imp__sub_821DB338);
REX_HOOK_RAW(sub_821DB338) {
  constexpr uint32_t kLaunchByte = 0x83362DB8u;   // set: launch data to hand back instead
  constexpr uint32_t kLaunchByte2 = 0x83362A34u;  // set: the same
  if (REXCVAR_GET(cod4_skip_intro_movies) && base[kLaunchByte] == 0 && base[kLaunchByte2] == 0) {
    REXLOG_INFO("cod4: start-up films skipped (cod4_skip_intro_movies)");
    return;
  }
  __imp__sub_821DB338(ctx, base);
}
#endif  // !COD4_MP
