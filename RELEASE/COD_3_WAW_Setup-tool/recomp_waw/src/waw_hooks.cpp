// waw - Call of Duty: World at War (Xbox 360) static recompilation: hooks in the translated code. [acct2-waw]
//
// Each function here is named by a [[midasm_hook]] block in config/default.toml, which says where the codegen calls
// it and why.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <immintrin.h>
#if defined(__linux__)
#include <pthread.h>
#include <sched.h>
#endif

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/memory/utils.h>
#include <rex/system.h>

#include "generated/default/waw_pch.h"
#include "waw_f2.h"  // [f2-menus] the F2 window's console page and its commands
#include "waw_keys.h"  // [f2-menus] keys the PC way: key events into the game's CL_KeyEvent

REXCVAR_DEFINE_BOOL(waw_log_console, true, "waw", "Copy the game's console output into the log (\"waw: [console N]\")");
REXCVAR_DEFINE_STRING(waw_debug_command, "", "waw",
                      "Debug: a console command the game runs once, waw_debug_command_delay seconds after its first "
                      "frame (for example \"devmap mak\")");
REXCVAR_DEFINE_INT32(waw_debug_command_delay, 30, "waw", "Debug: seconds from the first frame to waw_debug_command");
REXCVAR_DEFINE_STRING(waw_debug_commands, "", "waw",
                      "Debug: more console commands at set times, \"<seconds>:<command>|<seconds>:<command>...\", "
                      "the seconds from the first frame (for example \"15:devmap mak|80:+forward|110:-forward\")");
REXCVAR_DEFINE_BOOL(waw_log_menus, true, "waw", "Log each menu the game opens (\"waw: menu open <name>\")");
REXCVAR_DEFINE_BOOL(waw_log_menu_items, false, "waw",
                    "Log each loaded menu once, with every string its items point to (text, actions, dvars), when "
                    "a menu opens (for MENUS.md)");
REXCVAR_DEFINE_INT32(waw_log_ragdolls, 0, "waw",
                     "Debug: every this many seconds, the ragdoll pool's bodies into the log (0: off)");
REXCVAR_DEFINE_INT32(waw_debug_native_census, 0, "waw",
                     "Debug: every this many seconds, the render thread's passes and draws by technique type and "
                     "material, for the native renderer's design (0: off)");
REXCVAR_DEFINE_INT32(waw_max_fps, 0, "waw",
                     "The frame limit on the host's clock, 1-1000 frames a second, one frame at a time as the PC game "
                     "does (0: the game's own limit, which on the 360 code caps three frames at 3 x com_maxfps). Above "
                     "255 also set com_maxfps 0, and above the display's rate turn vsync off (--vsync=false, r_vsync 0)");
REXCVAR_DEFINE_INT32(waw_log_frame_parts, 0, "waw",
                     "Debug: every this many seconds, where the main thread's frame time went: the frame wait, the "
                     "server, the client, the screen update and its parts, in ms a frame (0: off)");
REXCVAR_DEFINE_INT32(waw_log_clocks, 0, "waw",
                     "Debug: every this many seconds, how many frames, server frames, player moves ... the game ran "
                     "and how far its clocks went (the frame-rate audit; 0: off)");
REXCVAR_DEFINE_BOOL(waw_extra_worker, false, "waw",
                    "One more scene worker thread (the 360 code makes two), in game thread slot 9, the one slot whose "
                    "per-thread tables have room");
REXCVAR_DEFINE_INT32(waw_move_hz, 0, "waw",
                     "The player's movement in fixed steps of 1000/N ms (a whole number of ms: 60 gives 17 ms, 125 "
                     "gives 8), 20-1000, for the server's moves and the client's prediction alike, so a jump or a walk "
                     "is the same at any frame rate (0: the game's own, one step per drawn frame of at most 66 ms). "
                     "Above N frames a second the view moves once per step");
// [waw-decomp] 11-09: on by default at 60 Hz. The 360 ran World at War with com_maxfps 85 and r_vsync 1 on a 60 Hz
// display, so its jumps had about 60 cuts a second. The 11-08 evening runs in pel1a (acct2-waw's evening28b) gave
// 31.3-31.4 units at 500 fps with the game's own cut, 37.8-38.3 at 500 fps and 38.2-38.5 at 125 fps with this at 60,
// and 0 steps left unmatched. Below 60 fps every step is longer than a 60 Hz tick and crosses a boundary, so
// nothing changes there.
REXCVAR_DEFINE_INT32(waw_move_snap_hz, 60, "waw",
                     "The player's velocity is cut to whole units (Sys_SnapVector, toward zero on the 360) at the end "
                     "of every movement step, so each step takes up to 1 unit/s off a jump's upward speed and more "
                     "steps make a lower jump (38.2 at 30 fps, 35.8 with 125 Hz steps, about 31 at 250 fps and up). "
                     "N (20-1000): cut it only on the steps that cross a boundary of a fixed N Hz clock on the "
                     "command time, so the jump stops depending on the step rate. 60 (the default): the 360's "
                     "60 Hz display, about 38 units at any frame rate. 0: the game's own, every step");
REXCVAR_DEFINE_DOUBLE(waw_fov, 0.0, "waw",
                      "Hold the game's cg_fov at this many degrees (1-160) every frame, over what the level sets "
                      "(the owner's FOV 100 for 500 fps tests, 11-07 21:50; 0: the game's own)");
REXCVAR_DEFINE_INT32(waw_native_skin, 0, "waw",
                     "Model skinning (the scene workers' queue 23) in native code: rigid vertices (0x824472B0) and "
                     "vertices with one to four bones (0x82446A98, 0x82446C28, 0x82446E18, 0x82447048): 0 the "
                     "translated code, 1 native, 2 both and compare (the translated result is kept; the log counts "
                     "the vertices that differ)");

DECLARE_REX_FUNC(sub_82238978);  // Cbuf_AddText(local client r3, text r4)

// A `bctr` that is a call through a table of function pointers, not a switch. The codegen took these for jump tables
// (`lis/addi/lwzx/mtctr/bctr` is also how a switch looks) and wrote a switch with one or two wrong cases, which
// trapped ("Switch case out of range") on the fastfile thread at the first fastfile. The hook stands before the
// `bctr`, when CTR already holds the target: it calls the target the way an ordinary indirect tail call does, and the
// config's `return = true` leaves the translated function right after, so the bad switch is never reached.
void waw_ctr_tail_call(PPCContext& ctx, uint8_t* base) {
  REX_CALL_INDIRECT_FUNC(ctx.ctr.u32);
}

namespace {

// Guest memory, big-endian, at `base + address` (as the translated code reads it).
uint32_t ReadGuestU32(const uint8_t* base, uint32_t address) {
  uint32_t value;
  std::memcpy(&value, base + address, sizeof(value));
  return __builtin_bswap32(value);
}

float ReadGuestFloat(const uint8_t* base, uint32_t address) {
  const uint32_t bits = ReadGuestU32(base, address);
  float value;
  std::memcpy(&value, &bits, sizeof(value));
  return value;
}

// The sound code's table of per-processor state: its address is stored at 0x84A81EB0 (`lis r11,-31576`, `lwz
// r8,7856(r11)` in sub_823B1CD8), and processor N's two words are at +12 + N * 8. The game fills only the slots of
// the processor its sound engine was made for (processor 4 on this disc, 2026-11-07).
constexpr uint32_t kSoundProcessorTablePointer = 0x84A81EB0;
constexpr uint32_t kSoundProcessorSlots = 12;
constexpr uint32_t kProcessorCount = 6;
// The processor number in the PCR (r13): KPRCB at +0x100, its current processor at +0xC.
constexpr uint32_t kPcrCurrentProcessor = 0x10C;

}  // namespace

// The start of the sound callback the runtime's audio worker calls (0x82396FB0, registered with
// XAudioRegisterRenderDriverClient). Two things the console gives that callback and the runtime's worker thread
// doesn't:
//  - Its processor. On the console the callback runs on the processor whose slot the sound engine filled; the
//    runtime's audio worker is given the next processor in turn, so the callback found an empty slot and read a null
//    pointer (sub_82395AD8, 2026-11-07 15:28). When the worker's processor has no slot, it takes the processor that
//    has one. The runtime reads the processor number from the PCR, so both agree.
//  - Floating-point exceptions off. The worker's context isn't given the host control word that guest threads get
//    (InitHost, every exception masked), and the vector code's flush-to-zero switch writes the context's whole word to
//    the host, so a float divide by zero in sub_82292A18 (a PowerPC fdivs, which gives infinity there) trapped
//    (SIGFPE, 2026-11-07 15:34). InitHost here masks them again.
void waw_sound_callback_start(PPCContext& ctx, uint8_t* base) {
  ctx.fpscr.InitHost();  // the host's control word with every exception masked (cheap; once per sound frame)

  const uint32_t table = ReadGuestU32(base, kSoundProcessorTablePointer);
  if (table == 0 || ctx.r13.u32 == 0) return;
  uint8_t* current = base + ctx.r13.u32 + kPcrCurrentProcessor;
  auto has_slot = [&](uint32_t processor) {
    return ReadGuestU32(base, table + kSoundProcessorSlots + processor * 8) != 0;
  };
  if (*current < kProcessorCount && has_slot(*current)) return;
  for (uint32_t processor = 0; processor < kProcessorCount; ++processor) {
    if (has_slot(processor)) {
      *current = static_cast<uint8_t>(processor);
      return;
    }
  }
}

// The game's console output: Com_PrintMessage(channel r3, text r4, error r5) at 0x8223A538, the one place every
// Com_Printf, warning and error line goes through. Each line goes into the log as "waw: [console <channel>] text".
// A line the same as the one before (the front end's menu sets a dvar every frame, and with the dvar echo that's one
// line a frame) is counted instead, and the count is logged before the next different line.
void waw_console_print(PPCContext& ctx, uint8_t* base) {
  if (!REXCVAR_GET(waw_log_console)) return;
  const uint32_t text = ctx.r4.u32;
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
  if (line == last && ctx.r3.s32 == last_channel) {
    ++repeats;
    return;
  }
  if (repeats) REXLOG_INFO("waw: [console {}] (the line before {} more time(s))", last_channel, repeats);
  repeats = 0;
  last.assign(line);
  last_channel = ctx.r3.s32;
  REXLOG_INFO("waw: [console {}] {}", last_channel, line);
  waw::f2ui::NoteConsoleLine(last_channel, line);  // [f2-menus] F2's CONSOLE page
}

namespace {

// Calls Cbuf_AddText(0, command + "\n") from inside a translated function: the text goes on the guest stack below
// the current frame (the callee's own frame is made below that), and every register is put back afterwards.
void QueueConsoleCommand(PPCContext& ctx, uint8_t* base, const std::string& command) {
  constexpr uint32_t kRoom = 0x800;      // stack taken for the call
  constexpr uint32_t kTextOffset = 0x200;  // the text, above the callee's frame and its argument area
  std::string line = command.substr(0, kRoom - kTextOffset - 2) + "\n";
  const PPCContext saved = ctx;
  const uint32_t frame = (ctx.r1.u32 - kRoom) & ~0xFu;
  std::memcpy(base + frame + kTextOffset, line.c_str(), line.size() + 1);
  ctx.r1.u64 = frame;
  ctx.r3.u64 = 0;
  ctx.r4.u64 = frame + kTextOffset;
  sub_82238978(ctx, base);
  ctx = saved;
}

}  // namespace

namespace {

// The ragdoll pool (2026-11-07, from the decomp's Ragdoll_CreateRagdollForDObj 0x82241F58 and the functions it calls):
// 32 bodies of 0xA10 bytes from 0x832135B0. A body's first word counts its users (0: free; the allocator 0x82241A78
// takes the first body with less than 1, Ragdoll_Remove 0x82242098 counts one down and frees it at the last), +8 is
// the object (DObj) it was made for, and +0x24 its state (3 and 4 are the ones simulating, the ones
// ragdoll_max_simulating limits; 0x82241E00 counts them).
constexpr uint32_t kRagdollBodies = 0x832135B0;
constexpr uint32_t kRagdollBodyCount = 32;
constexpr uint32_t kRagdollBodySize = 0xA10;
constexpr uint32_t kRagdollBodyObject = 0x8;
constexpr uint32_t kRagdollBodyState = 0x24;

// One line: how many bodies are taken, how many in each state, and each taken body as object:state:users.
void LogRagdollBodies(const uint8_t* base) {
  uint32_t taken = 0;
  uint32_t per_state[8] = {};
  std::string bodies;
  for (uint32_t i = 0; i < kRagdollBodyCount; ++i) {
    const uint32_t body = kRagdollBodies + i * kRagdollBodySize;
    const int32_t users = static_cast<int32_t>(ReadGuestU32(base, body));
    if (users < 1) continue;
    ++taken;
    const uint32_t state = ReadGuestU32(base, body + kRagdollBodyState);
    ++per_state[state < 7 ? state : 7];
    bodies += " " + std::to_string(ReadGuestU32(base, body + kRagdollBodyObject)) + ":" + std::to_string(state) + ":" +
              std::to_string(users);
  }
  std::string states;
  for (uint32_t s = 0; s < 8; ++s) {
    if (per_state[s]) {
      states += " " + std::string(s < 7 ? std::to_string(s) : "7+") + "=" + std::to_string(per_state[s]);
    }
  }
  REXLOG_INFO("waw: ragdoll bodies taken {}/{}; states{}; object:state:users{}", taken, kRagdollBodyCount,
              states.empty() ? " -" : states, bodies.empty() ? " -" : bodies);
}

}  // namespace

namespace {

// The frame-rate audit's counters (2026-11-07; the functions are the decomp's names). Each hook below counts calls
// of one function; waw_frame_start logs them every waw_log_clocks seconds of real time and starts again from zero.
struct ClockCounts {
  std::atomic<uint64_t> frames{0};          // Com_Frame: the main loop, one a drawn frame
  std::atomic<uint64_t> server_frames{0};   // SV_Frame(msec r3)
  std::atomic<int64_t> server_msec{0};      //   the sum of its msec
  std::atomic<uint64_t> game_frames{0};     // G_RunFrame(levelTime r3): one game (level) update
  std::atomic<int32_t> game_time{0};        //   the last levelTime
  std::atomic<uint64_t> usercmds{0};        // ClientThink_real: one player command run on the server
  std::atomic<uint64_t> pmoves{0};          // Pmove: one player move
  std::atomic<uint64_t> pmove_steps{0};     // PmoveSingle: one step of it
  std::atomic<uint64_t> client_frames{0};   // CG_DrawActiveFrame(local client r3, serverTime r4)
  std::atomic<int32_t> client_time{0};      //   the last serverTime
  std::atomic<uint64_t> script_ticks{0};    // Scr_IncTime: the script clock's ticks
  std::atomic<uint64_t> anim_lerps{0};      // BG_RunLerpFrameRate: an entity's animation advanced
  std::atomic<uint64_t> bink_pictures{0};   // BinkDoFrame 0x824733A0: a movie picture decoded
  std::atomic<uint32_t> player_state{0};    // the last Pmove's pm->ps (pmove_t's first word)
  // The lowest and highest origin height any Pmove started from in the interval, in tenths of a unit: on flat ground
  // the difference is a jump's height, to compare at 30, 60 and uncapped fps.
  std::atomic<int32_t> height_min{INT32_MAX};
  std::atomic<int32_t> height_max{INT32_MIN};
  // The velocity snap at the end of PmoveSingle (Sys_SnapVector 0x822A30B0) and PM_StepSlideMove's step-down
  // (11-08, for the jump height). Snaps done, skipped by waw_move_snap_hz, and done as the game does because the step
  // wasn't one this thread's Pmove recorded; the upward speed the snaps took while a jump rose (thousandths of a unit
  // a second); the step-down's velocity changes in all and while a jump rose (the projection after a stepped move,
  // 0x820EA6DC; the clip after the 9-unit trace down, 0x820EA810); the jump clamp's calls (0x820EA824).
  std::atomic<uint64_t> snaps{0};
  std::atomic<uint64_t> snaps_skipped{0};
  std::atomic<uint64_t> snaps_unmatched{0};
  std::atomic<int64_t> snap_rise_taken{0};
  std::atomic<uint64_t> step_down_projects{0};
  std::atomic<uint64_t> step_down_projects_rising{0};
  std::atomic<uint64_t> step_down_clips{0};
  std::atomic<uint64_t> step_down_clips_rising{0};
  std::atomic<uint64_t> jump_clamps{0};
};

// playerState_s: origin at +0x20 and velocity at +0x2C (three floats each). T4 has one word more than IW3 before them
// (IW3: +0x1C / +0x28). Read from the log, 2026-11-07: at +0x1C the "x" stayed 0.0 and the "velocity x" was the height.
// +0x00 is commandTime (Pmove's loop compares it with the command's time) and +0x0C the pm_flags (0x4000 PMF_JUMPING,
// set by the jump start 0x820DB740).
constexpr uint32_t kPlayerStateCommandTime = 0x00;
constexpr uint32_t kPlayerStateFlags = 0x0C;
constexpr uint32_t kPlayerStateOrigin = 0x20;
constexpr uint32_t kPlayerStateVelocity = 0x2C;
constexpr uint32_t kPlayerFlagJumping = 0x4000;
ClockCounts g_clocks;

void LogClocks(const uint8_t* base, double seconds) {
  static int32_t last_game_time = 0;
  static int32_t last_client_time = 0;
  auto take = [](auto& counter) { return counter.exchange(0, std::memory_order_relaxed); };
  const uint64_t frames = take(g_clocks.frames);
  const uint64_t server_frames = take(g_clocks.server_frames);
  const int64_t server_msec = take(g_clocks.server_msec);
  const uint64_t game_frames = take(g_clocks.game_frames);
  const uint64_t usercmds = take(g_clocks.usercmds);
  const uint64_t pmoves = take(g_clocks.pmoves);
  const uint64_t pmove_steps = take(g_clocks.pmove_steps);
  const uint64_t client_frames = take(g_clocks.client_frames);
  const uint64_t script_ticks = take(g_clocks.script_ticks);
  const uint64_t anim_lerps = take(g_clocks.anim_lerps);
  const uint64_t bink_pictures = take(g_clocks.bink_pictures);
  const int32_t game_time = g_clocks.game_time.load(std::memory_order_relaxed);
  const int32_t client_time = g_clocks.client_time.load(std::memory_order_relaxed);
  REXLOG_INFO(
      "waw: clocks over {:.2f} s: frames {} ({:.1f}/s); server frames {} (msec {}); game frames {} (game time +{} "
      "ms); usercmds {}; Pmove {} (steps {}); client frames {} (client time +{} ms); script ticks {}; anim lerps {}; "
      "movie pictures {}",
      seconds, frames, frames / seconds, server_frames, server_msec, game_frames,
      last_game_time ? game_time - last_game_time : 0, usercmds, pmoves, pmove_steps, client_frames,
      last_client_time ? client_time - last_client_time : 0, script_ticks, anim_lerps, bink_pictures);
  last_game_time = game_time;
  last_client_time = client_time;
  if (const uint32_t ps = g_clocks.player_state.load(std::memory_order_relaxed); ps >= 0x10000) {
    auto f = [&](uint32_t offset) { return ReadGuestFloat(base, ps + offset); };
    const int32_t low = g_clocks.height_min.exchange(INT32_MAX, std::memory_order_relaxed);
    const int32_t high = g_clocks.height_max.exchange(INT32_MIN, std::memory_order_relaxed);
    REXLOG_INFO("waw: player at ({:.1f} {:.1f} {:.1f}), velocity ({:.1f} {:.1f} {:.1f}); height {:.1f} to {:.1f}",
                f(kPlayerStateOrigin), f(kPlayerStateOrigin + 4), f(kPlayerStateOrigin + 8), f(kPlayerStateVelocity),
                f(kPlayerStateVelocity + 4), f(kPlayerStateVelocity + 8), low == INT32_MAX ? 0.0 : low / 10.0,
                high == INT32_MIN ? 0.0 : high / 10.0);
  }
  const uint64_t snaps = take(g_clocks.snaps);
  const uint64_t snaps_skipped = take(g_clocks.snaps_skipped);
  const uint64_t snaps_unmatched = take(g_clocks.snaps_unmatched);
  const int64_t rise_taken = take(g_clocks.snap_rise_taken);
  const uint64_t projects = take(g_clocks.step_down_projects);
  const uint64_t projects_rising = take(g_clocks.step_down_projects_rising);
  const uint64_t clips = take(g_clocks.step_down_clips);
  const uint64_t clips_rising = take(g_clocks.step_down_clips_rising);
  const uint64_t clamps = take(g_clocks.jump_clamps);
  if (snaps + snaps_skipped + projects + clips + clamps > 0) {
    REXLOG_INFO(
        "waw: movement: velocity snaps {} (skipped {}, unmatched {}), upward speed they took while a jump rose {:.1f} "
        "units/s; step-down projections {} ({} while a jump rose), step-down clips {} ({} while a jump rose), jump "
        "clamps {}",
        snaps, snaps_skipped, snaps_unmatched, rise_taken / 1000.0, projects, projects_rising, clips, clips_rising,
        clamps);
  }
}

}  // namespace

namespace {

// Where the main thread's frame time goes (owner 18:59: the 500 fps target; 2026-11-07). Each mark below is a hook on
// the main thread's path through one frame, in this order (from Com_Frame_Try_Block_Function 0x8223C200's decomp):
// the time from one mark to the next goes to the first one's part. Marks on other threads are ignored.
enum FramePart { kPartWait, kPartEvents, kPartServer, kPartClient, kPartScreen, kPartClientGame, kPartScene,
                 kPartScene2, kPartScene3, kPartScene4, kPart2D, kPartScreenField, kPartEndFrame, kPartTail,
                 kFramePartCount };
constexpr const char* kFramePartNames[kFramePartCount] = {
    "frame wait",         // 0x8223C200, the frame's start: the com_maxfps / vsync wait loop
    "events and msec",    // 0x8223C3D4, the wait's end: Cbuf, Com_ModifyMsec
    "server",             // SV_Frame 0x822C0010: the server frame (G_RunFrame at 20 Hz, the usercmds and Pmove)
    "client",             // 0x822CD980, called after SV_Frame: the client frame, input, usercmds, then the UI
    "screen start",       // SCR_UpdateScreen 0x821458F8, up to the client game
    "client game",        // CG_DrawActiveFrame 0x8212C8A8: prediction, entities, up to the scene
    "scene start",        // R_RenderScene 0x82405400, up to 0x82405078
    "scene 0x82405078",   // R_RenderScene's third call
    "scene 0x8243FA60",   // its fourth
    "scene 0x82403400",   // its last, to the 2D
    "2D",                 // CG_Draw2D 0x82101540: the HUD
    "screen field",       // SCR_DrawScreenField 0x82145288: menus, console, up to the frame's end
    "end frame",          // 0x82418CF0, SCR_UpdateFrame's last renderer call (R_EndFrame's place)
    "tail",               // Com_Statmon 0x8223C130, to the next frame
};

struct FrameParts {
  using clock = std::chrono::steady_clock;
  std::thread::id main_thread;
  bool started = false;
  int part = kPartWait;
  clock::time_point last, since;
  double ms[kFramePartCount] = {};
  uint64_t frames = 0;
};
FrameParts g_parts;

// The scene step's worker commands (19:34): 0x8242CF40 queues one (it checks r_smp_worker; the PC's R_AddWorkerCmd
// place), r3 its type. Counted per type from any thread, logged with the frame parts as counts a frame.
constexpr uint32_t kWorkerCmdTypes = 32;
std::atomic<uint64_t> g_worker_cmds[kWorkerCmdTypes];
std::atomic<uint64_t> g_worker_cmds_other{0};

// The time each command queue takes (19:48). 0x8242C870 runs one command (or up to ten) from queue r3 (27 queues,
// one per command type, 0x80 bytes each from 0x84E72890). The two workers' loop 0x8242CBD8 calls it from four
// places, and 0x8242CCA8, where the main thread helps while it waits for the scene step, from four more. A hook
// before each call notes the queue and the time; one after it adds the time to the queue's total, the main
// thread's and the workers' apart. The workers' totals are summed over both workers.
std::atomic<uint64_t> g_queue_worker_ns[kWorkerCmdTypes];
std::atomic<uint64_t> g_queue_main_ns[kWorkerCmdTypes];
std::atomic<std::thread::id> g_frame_thread{};
thread_local uint32_t t_queue = UINT32_MAX;
thread_local std::chrono::steady_clock::time_point t_queue_start;

// Where FX_GenerateVerts 0x82165E38 (worker command 21) spends its time (20:52, its first call added 21:02): marks
// after each of its calls, from any thread, summed over all of them. The parts, in order: 0x82101D68 (which reaches
// 0x82416388 through 0x82416DD0), 0x8216AF08, 0x821623C8, FX_PostLight_GenerateVerts 0x82169D08, the two
// FX_DrawTrailsForEffect 0x82165C50 calls, and 0x821736B0.
constexpr int kFxParts = 6;
constexpr const char* kFxPartNames[kFxParts] = {"0x82101D68", "0x8216AF08", "0x821623C8", "postlight", "trails",
                                                "0x821736B0"};
std::atomic<uint64_t> g_fx_part_ns[kFxParts];
std::atomic<uint64_t> g_fx_calls{0};
thread_local int t_fx_part = -1;
thread_local std::chrono::steady_clock::time_point t_fx_last;

// Inside 0x82101D68 (21:13): its two big calls timed apart, the tube meshes 0x82416DD0 -> 0x82416388 (a list of
// tubes at 0x84C5BF00, their count at +0xA80; the PC's 0x006EC3D0) and the flames 0x822B5188 (dvar flame_render).
constexpr int kFxInner = 2;
constexpr const char* kFxInnerNames[kFxInner] = {"tubes 0x82416388", "flames 0x822B5188"};
constexpr uint32_t kTubeList = 0x84C5BF00;
constexpr uint32_t kTubeCount = 0xA80;
std::atomic<uint64_t> g_fx_inner_ns[kFxInner];
std::atomic<uint64_t> g_fx_tubes{0};
thread_local int t_fx_inner = -1;
thread_local std::chrono::steady_clock::time_point t_fx_inner_start;

void MarkFramePart(int part) {
  const int32_t every = REXCVAR_GET(waw_log_frame_parts);
  if (every <= 0) return;
  FrameParts& p = g_parts;
  const auto now = FrameParts::clock::now();
  if (!p.started) {
    if (part != kPartWait) return;
    p.started = true;
    p.main_thread = std::this_thread::get_id();
    g_frame_thread.store(p.main_thread, std::memory_order_relaxed);
    p.last = p.since = now;
    p.part = kPartWait;
    return;
  }
  if (std::this_thread::get_id() != p.main_thread) return;
  p.ms[p.part] += std::chrono::duration<double, std::milli>(now - p.last).count();
  p.last = now;
  p.part = part;
  if (part != kPartWait) return;
  ++p.frames;
  if (now - p.since < std::chrono::seconds(every)) return;
  const double seconds = std::chrono::duration<double>(now - p.since).count();
  std::string parts;
  double total = 0;
  for (int i = 0; i < kFramePartCount; ++i) {
    char text[80];
    std::snprintf(text, sizeof(text), "%s%s %.2f", i ? ", " : "", kFramePartNames[i], p.ms[i] / p.frames);
    parts += text;
    total += p.ms[i];
    p.ms[i] = 0;
  }
  REXLOG_INFO("waw: frame parts over {} frames ({:.1f}/s), ms a frame: {}; total {:.2f}", p.frames, p.frames / seconds,
              parts, total / p.frames);
  std::string commands;
  for (uint32_t type = 0; type < kWorkerCmdTypes; ++type) {
    const uint64_t count = g_worker_cmds[type].exchange(0, std::memory_order_relaxed);
    if (!count) continue;
    char text[32];
    std::snprintf(text, sizeof(text), " %u:%.1f", type, static_cast<double>(count) / p.frames);
    commands += text;
  }
  const uint64_t other = g_worker_cmds_other.exchange(0, std::memory_order_relaxed);
  REXLOG_INFO("waw: worker commands a frame (type:count){}; other types {:.1f}", commands.empty() ? " -" : commands,
              static_cast<double>(other) / p.frames);
  std::string times;
  double workers_total = 0, main_total = 0;
  for (uint32_t queue = 0; queue < kWorkerCmdTypes; ++queue) {
    const double workers_ms = g_queue_worker_ns[queue].exchange(0, std::memory_order_relaxed) / 1e6 / p.frames;
    const double main_ms = g_queue_main_ns[queue].exchange(0, std::memory_order_relaxed) / 1e6 / p.frames;
    workers_total += workers_ms;
    main_total += main_ms;
    if (workers_ms + main_ms < 0.005) continue;
    char text[48];
    std::snprintf(text, sizeof(text), " %u:%.2f/%.2f", queue, workers_ms, main_ms);
    times += text;
  }
  REXLOG_INFO("waw: command queue time, ms a frame (queue:workers/main thread){}; in all workers {:.2f}, main "
              "thread {:.2f}", times.empty() ? " -" : times, workers_total, main_total);
  const uint64_t fx_calls = g_fx_calls.exchange(0, std::memory_order_relaxed);
  if (fx_calls) {
    std::string fx;
    for (int i = 0; i < kFxParts; ++i) {
      char text[48];
      std::snprintf(text, sizeof(text), "%s%s %.2f", i ? ", " : "", kFxPartNames[i],
                    g_fx_part_ns[i].exchange(0, std::memory_order_relaxed) / 1e6 / p.frames);
      fx += text;
    }
    std::string inner;
    for (int i = 0; i < kFxInner; ++i) {
      char text[48];
      std::snprintf(text, sizeof(text), "%s%s %.2f", i ? ", " : "", kFxInnerNames[i],
                    g_fx_inner_ns[i].exchange(0, std::memory_order_relaxed) / 1e6 / p.frames);
      inner += text;
    }
    REXLOG_INFO("waw: FX_GenerateVerts {:.1f} calls a frame, ms a frame in all threads: {}; in 0x82101D68: {} "
                "({:.1f} tubes a call)", static_cast<double>(fx_calls) / p.frames, fx, inner,
                static_cast<double>(g_fx_tubes.exchange(0, std::memory_order_relaxed)) / fx_calls);
  }
  p.frames = 0;
  p.since = now;
}

}  // namespace

// The frame-part marks (config/default.toml), each at the address its name in kFramePartNames gives. The server and
// client game marks are in the audit's SV_Frame and CG_DrawActiveFrame counters below (one hook per address).
void waw_part_frame_start(PPCContext& ctx, uint8_t* base) { (void)ctx; (void)base; MarkFramePart(kPartWait); }
void waw_part_wait_done(PPCContext& ctx, uint8_t* base) { (void)ctx; (void)base; MarkFramePart(kPartEvents); }
void waw_part_client(PPCContext& ctx, uint8_t* base) { (void)ctx; (void)base; MarkFramePart(kPartClient); }
void waw_part_screen(PPCContext& ctx, uint8_t* base) { (void)ctx; (void)base; MarkFramePart(kPartScreen); }
void waw_part_scene(PPCContext& ctx, uint8_t* base) { (void)ctx; (void)base; MarkFramePart(kPartScene); }
void waw_part_tail(PPCContext& ctx, uint8_t* base) { (void)ctx; (void)base; MarkFramePart(kPartTail); }
void waw_part_scene2(PPCContext& ctx, uint8_t* base) { (void)ctx; (void)base; MarkFramePart(kPartScene2); }
void waw_part_scene3(PPCContext& ctx, uint8_t* base) { (void)ctx; (void)base; MarkFramePart(kPartScene3); }
void waw_part_scene4(PPCContext& ctx, uint8_t* base) { (void)ctx; (void)base; MarkFramePart(kPartScene4); }
void waw_part_2d(PPCContext& ctx, uint8_t* base) { (void)ctx; (void)base; MarkFramePart(kPart2D); }
void waw_part_screen_field(PPCContext& ctx, uint8_t* base) { (void)ctx; (void)base; MarkFramePart(kPartScreenField); }
void waw_part_end_frame(PPCContext& ctx, uint8_t* base) { (void)ctx; (void)base; MarkFramePart(kPartEndFrame); }
void waw_count_worker_cmd(PPCContext& ctx, uint8_t* base) {
  (void)base;
  if (REXCVAR_GET(waw_log_frame_parts) <= 0) return;
  if (ctx.r3.u32 < kWorkerCmdTypes) {
    g_worker_cmds[ctx.r3.u32].fetch_add(1, std::memory_order_relaxed);
  } else {
    g_worker_cmds_other.fetch_add(1, std::memory_order_relaxed);
  }
}
// Before and after each of the eight calls to 0x8242C870 (config/default.toml); see g_queue_worker_ns.
void waw_queue_call(PPCContext& ctx, uint8_t* base) {
  (void)base;
  if (REXCVAR_GET(waw_log_frame_parts) <= 0) return;
  t_queue = ctx.r3.u32;
  t_queue_start = std::chrono::steady_clock::now();
}
void waw_queue_return(PPCContext& ctx, uint8_t* base) {
  (void)ctx;
  (void)base;
  const uint32_t queue = t_queue;
  if (queue >= kWorkerCmdTypes) return;
  t_queue = UINT32_MAX;
  const uint64_t ns = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() -
                                                                           t_queue_start).count();
  const bool on_main_thread = std::this_thread::get_id() == g_frame_thread.load(std::memory_order_relaxed);
  std::atomic<uint64_t>* totals = on_main_thread ? g_queue_main_ns : g_queue_worker_ns;
  totals[queue].fetch_add(ns, std::memory_order_relaxed);
}

// The FX_GenerateVerts marks (config/default.toml): 0x82165E48 (before its first call) starts; 0x82165E54,
// 0x82165E78, 0x82165E80, 0x82165E8C, 0x82165F40 and 0x82165F4C each end one part.
namespace {
void MarkFxPart(int ended) {
  if (REXCVAR_GET(waw_log_frame_parts) <= 0) return;
  const auto now = std::chrono::steady_clock::now();
  if (ended >= 0 && t_fx_part == ended) {
    const uint64_t ns = std::chrono::duration_cast<std::chrono::nanoseconds>(now - t_fx_last).count();
    g_fx_part_ns[ended].fetch_add(ns, std::memory_order_relaxed);
  }
  t_fx_last = now;
  t_fx_part = ended + 1 < kFxParts ? ended + 1 : -1;
}
}  // namespace
void waw_fx_start(PPCContext& ctx, uint8_t* base) {
  (void)ctx; (void)base;
  if (REXCVAR_GET(waw_log_frame_parts) <= 0) return;
  g_fx_calls.fetch_add(1, std::memory_order_relaxed);
  MarkFxPart(-1);
}
void waw_fx_part0(PPCContext& ctx, uint8_t* base) { (void)ctx; (void)base; MarkFxPart(0); }
void waw_fx_part1(PPCContext& ctx, uint8_t* base) { (void)ctx; (void)base; MarkFxPart(1); }
void waw_fx_part2(PPCContext& ctx, uint8_t* base) { (void)ctx; (void)base; MarkFxPart(2); }
void waw_fx_part3(PPCContext& ctx, uint8_t* base) { (void)ctx; (void)base; MarkFxPart(3); }
void waw_fx_part4(PPCContext& ctx, uint8_t* base) { (void)ctx; (void)base; MarkFxPart(4); }
void waw_fx_part5(PPCContext& ctx, uint8_t* base) { (void)ctx; (void)base; MarkFxPart(5); }

// The marks inside 0x82101D68: 0x82101DF8 and 0x82101DFC around its call of 0x82416DD0, 0x82101E08 and 0x82101E0C
// around its call of 0x822B5188.
namespace {
void StartFxInner(int part) {
  if (REXCVAR_GET(waw_log_frame_parts) <= 0) return;
  t_fx_inner = part;
  t_fx_inner_start = std::chrono::steady_clock::now();
}
void EndFxInner(int part) {
  if (REXCVAR_GET(waw_log_frame_parts) <= 0 || t_fx_inner != part) return;
  const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() -
                                                                       t_fx_inner_start).count();
  g_fx_inner_ns[part].fetch_add(ns, std::memory_order_relaxed);
  t_fx_inner = -1;
}
}  // namespace
void waw_fx_tubes_start(PPCContext& ctx, uint8_t* base) {
  (void)ctx;
  if (REXCVAR_GET(waw_log_frame_parts) <= 0) return;
  g_fx_tubes.fetch_add(ReadGuestU32(base, kTubeList + kTubeCount), std::memory_order_relaxed);
  StartFxInner(0);
}
void waw_fx_tubes_end(PPCContext& ctx, uint8_t* base) { (void)ctx; (void)base; EndFxInner(0); }
void waw_fx_flames_start(PPCContext& ctx, uint8_t* base) { (void)ctx; (void)base; StartFxInner(1); }
void waw_fx_flames_end(PPCContext& ctx, uint8_t* base) { (void)ctx; (void)base; EndFxInner(1); }

// One more scene worker (`waw_extra_worker`, 11-07 20:25). R_InitWorkerThreads 0x8242CED0 makes worker i (i < 2) with
// 0x82240AD8(0x8242CEA0, i), which creates game thread slot i + 2 through 0x82240990(func, slot). Slot 9 is the one
// slot whose per-thread tables all have room: the thread ids at 0x832134CC (the next words are other data), the
// handles at 0x832134FC (slots 10-17 belong to other threads), the 16-byte thread blocks at 0x83213408 (10 of them)
// and the three per-thread buffers 0x8227DD98 gives each thread (10 each). Five hooks, all at init:
//   - the loop's compare (0x8242CF20): once the two workers exist, the loop body runs once more;
//   - 0x82240AD8 (0x82240AEC, the slot in r31): that extra call makes slot 9;
//   - 0x82240990 before CreateThread (0x822409D4, the slot in r29): slot 9 starts at once. The two workers start
//     suspended, and R_Init 0x823F9670 resumes them from its own table, which has two entries;
//   - the two thread-name calls (0x82240A14, the slot in r29; 0x82240944 in the thread start, the slot in r30): the
//     name table at 0x824966E0 ends at slot 8, so slot 9 is named with the program's string "r_smp_worker_thread2".
// The extra worker isn't in 0x8242D1A8's suspend and resume of the two workers (around a device reset).
namespace {
constexpr uint32_t kExtraWorkerSlot = 9;
constexpr uint32_t kExtraWorkerName = 0x820AB578;  // "r_smp_worker_thread2"
bool g_extra_worker_pending = false;
std::atomic<bool> g_extra_worker_made{false};
}  // namespace

void waw_extra_worker_loop(PPCContext& ctx, uint8_t* base) {
  (void)base;
  if (!REXCVAR_GET(waw_extra_worker) || g_extra_worker_made.load() || ctx.r31.s32 != 2) return;
  ctx.r31.u64 = 1;  // the compare sees 1 < 2, so the loop body runs once more
  g_extra_worker_pending = true;
}
void waw_extra_worker_slot(PPCContext& ctx, uint8_t* base) {
  (void)base;
  if (!g_extra_worker_pending) return;
  g_extra_worker_pending = false;
  g_extra_worker_made.store(true);
  ctx.r31.u64 = kExtraWorkerSlot;
  REXLOG_INFO("waw: an extra scene worker, game thread slot {}", kExtraWorkerSlot);
}
void waw_extra_worker_create(PPCContext& ctx, uint8_t* base) {
  (void)base;
  if (ctx.r29.u32 == kExtraWorkerSlot && g_extra_worker_made.load()) ctx.r7.u64 = 0;  // not suspended
}
void waw_thread_name_create(PPCContext& ctx, uint8_t* base) {
  (void)base;
  if (ctx.r29.u32 == kExtraWorkerSlot && g_extra_worker_made.load()) ctx.r4.u64 = kExtraWorkerName;
}
// The SDK runs each game thread on one host CPU, its 360 hardware thread's number (0-5; XThread::SetActiveCpu,
// `1 << cpu_index`). The game puts its threads on 0-5 with XSetThreadProcessor 0x8237BF98: the main thread and the
// database thread 0, the render thread 1, the workers 2 and 3, three small threads 4, the server thread 5. Slot 9's
// XSetThreadProcessor(9) fails (only 0-5 exist), so the extra worker would keep the CPU it was made on. Started on
// its own thread (0x82240944 is in the thread start 0x82240920), it moves itself to the host's CPUs past the sixth,
// where no game thread runs (Linux; nothing on a host with 6 CPUs or fewer).
namespace {
void MoveExtraWorkerOffGameCpus() {
  const unsigned cpus = std::thread::hardware_concurrency();
  if (cpus <= 6) return;
#if defined(__linux__)
  cpu_set_t set;
  CPU_ZERO(&set);
  for (unsigned cpu = 6; cpu < cpus && cpu < CPU_SETSIZE; ++cpu) CPU_SET(cpu, &set);
  const int rc = pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
  REXLOG_INFO("waw: the extra scene worker runs on host CPUs 6-{} (rc {})", cpus - 1, rc);
#endif  // Windows: not yet (the Windows build keeps the CPU the worker was made on)
}
}  // namespace

void waw_thread_name_start(PPCContext& ctx, uint8_t* base) {
  (void)base;
  if (ctx.r30.u32 != kExtraWorkerSlot || !g_extra_worker_made.load()) return;
  ctx.r4.u64 = kExtraWorkerName;
  MoveExtraWorkerOffGameCpus();
}

// The extra worker's collision-trace state (11-07 21:31, the pel1a crashes). CM_LoadMap 0x8222CF50 (from
// SV_SpawnServer and CL_CM_LoadMap) sets up each thread context's trace state with CM_InitThreadData 0x8222CE88 for
// contexts 0-5 only; that function's static arrays have six entries. The trace-state table at 0x83513EF8 (0x14 bytes
// an entry) has ten, and each thread's value 3 points at its own entry (0x8227DD98), so slot 9's entry stayed zero
// and a trace on the extra worker read null + brush * 4 (its per-brush check counts: reads of guest 0x571C, 0x5C38,
// 0x5F1C in the first second of pel1a). Right after CM_LoadMap's own calls (0x8222CFDC), slot 9's entry gets the same
// set-up as the others, in system-heap memory of its own: +0 a zero, +4 a zeroed check-count word per brush (the
// count at 0x83160704), +8 a copy of the box model (0x50 bytes from the pointer at 0x83160738), +0xC a copy of the box
// brush (0x48 bytes at 0x8316073C), +0x10 a zeroed word.
namespace {
constexpr uint32_t kTraceStates = 0x83513EF8;
constexpr uint32_t kTraceStateSize = 0x14;
constexpr uint32_t kMapBrushCount = 0x83160704;
constexpr uint32_t kBoxModelPointer = 0x83160738;
constexpr uint32_t kBoxBrush = 0x8316073C;
constexpr uint32_t kBoxModelSize = 0x50;
constexpr uint32_t kBoxBrushSize = 0x48;

void WriteGuestU32(uint8_t* base, uint32_t address, uint32_t value) {
  value = __builtin_bswap32(value);
  std::memcpy(base + address, &value, sizeof(value));
}

std::mutex g_extra_trace_mutex;
uint32_t g_extra_trace_counts = 0;  // guest address of the check-count words
uint32_t g_extra_trace_counts_size = 0;
uint32_t g_extra_trace_block = 0;   // the box model, the box brush and the zeroed word
}  // namespace

void waw_extra_worker_trace(PPCContext& ctx, uint8_t* base) {
  (void)ctx;
  if (!g_extra_worker_made.load()) return;
  auto* kernel = rex::system::kernel_state();
  auto* memory = kernel ? kernel->memory() : nullptr;
  if (!memory) return;
  std::lock_guard<std::mutex> lock(g_extra_trace_mutex);
  const uint32_t brushes = ReadGuestU32(base, kMapBrushCount);
  const uint32_t counts_size = std::max<uint32_t>(brushes * 4, 4);
  if (counts_size > g_extra_trace_counts_size) {
    if (g_extra_trace_counts) memory->SystemHeapFree(g_extra_trace_counts);
    g_extra_trace_counts = memory->SystemHeapAlloc(counts_size);
    g_extra_trace_counts_size = g_extra_trace_counts ? counts_size : 0;
  }
  if (!g_extra_trace_block) g_extra_trace_block = memory->SystemHeapAlloc(kBoxModelSize + kBoxBrushSize + 0x10);
  const uint32_t box_model_source = ReadGuestU32(base, kBoxModelPointer);
  if (!g_extra_trace_counts || !g_extra_trace_block || box_model_source < 0x10000) {
    REXLOG_INFO("waw: the extra scene worker's trace state could not be set up (memory {:08X} {:08X}, box model "
                 "{:08X})", g_extra_trace_counts, g_extra_trace_block, box_model_source);
    return;
  }
  std::memset(base + g_extra_trace_counts, 0, g_extra_trace_counts_size);
  const uint32_t box_model = g_extra_trace_block;
  const uint32_t box_brush = box_model + kBoxModelSize;
  const uint32_t zero_word = box_brush + kBoxBrushSize;
  std::memcpy(base + box_model, base + box_model_source, kBoxModelSize);
  std::memcpy(base + box_brush, base + kBoxBrush, kBoxBrushSize);
  WriteGuestU32(base, zero_word, 0);
  const uint32_t state = kTraceStates + kExtraWorkerSlot * kTraceStateSize;
  WriteGuestU32(base, state + 0x0, 0);
  WriteGuestU32(base, state + 0x4, g_extra_trace_counts);
  WriteGuestU32(base, state + 0x8, box_model);
  WriteGuestU32(base, state + 0xC, box_brush);
  WriteGuestU32(base, state + 0x10, zero_word);
  REXLOG_INFO("waw: the extra scene worker's trace state set up for the map ({} brushes)", brushes);
}

// The audit's counting hooks, each at its function's first instruction (config/default.toml).
void waw_count_server_frame(PPCContext& ctx, uint8_t* base) {
  (void)base;
  MarkFramePart(kPartServer);
  g_clocks.server_frames.fetch_add(1, std::memory_order_relaxed);
  g_clocks.server_msec.fetch_add(ctx.r3.s32, std::memory_order_relaxed);
}
void waw_count_game_frame(PPCContext& ctx, uint8_t* base) {
  (void)base;
  g_clocks.game_frames.fetch_add(1, std::memory_order_relaxed);
  g_clocks.game_time.store(ctx.r3.s32, std::memory_order_relaxed);
}
void waw_count_usercmd(PPCContext& ctx, uint8_t* base) {
  (void)ctx; (void)base;
  g_clocks.usercmds.fetch_add(1, std::memory_order_relaxed);
}
void waw_count_pmove(PPCContext& ctx, uint8_t* base) {
  g_clocks.pmoves.fetch_add(1, std::memory_order_relaxed);
  if (ctx.r3.u32 < 0x10000) return;
  const uint32_t ps = ReadGuestU32(base, ctx.r3.u32);
  g_clocks.player_state.store(ps, std::memory_order_relaxed);
  if (ps < 0x10000 || REXCVAR_GET(waw_log_clocks) <= 0) return;
  const float z = ReadGuestFloat(base, ps + kPlayerStateOrigin + 8);
  if (!(z > -1e6f && z < 1e6f)) return;
  const int32_t tenths = static_cast<int32_t>(z * 10.0f);
  int32_t seen = g_clocks.height_min.load(std::memory_order_relaxed);
  while (tenths < seen && !g_clocks.height_min.compare_exchange_weak(seen, tenths, std::memory_order_relaxed)) {
  }
  seen = g_clocks.height_max.load(std::memory_order_relaxed);
  while (tenths > seen && !g_clocks.height_max.compare_exchange_weak(seen, tenths, std::memory_order_relaxed)) {
  }
}
void waw_count_pmove_step(PPCContext& ctx, uint8_t* base) {
  (void)ctx; (void)base;
  g_clocks.pmove_steps.fetch_add(1, std::memory_order_relaxed);
}

// `waw_move_hz`: fixed movement steps. Pmove 0x820E9BD0 (pm in r30, pm->ps in r29) runs PmoveSingle from
// ps->commandTime up to the command's time in pieces of at most 66 ms, so each step is one drawn frame and the move's
// arithmetic (a jump's height above all) follows the frame rate. Both of its callers that matter go through it: the
// server's ClientThink_real and the client's CG_PredictPlayerState_Internal, so the two stay in agreement.
//   - at 0x820E9C40, r31 holds the command's time: it is moved up to the next multiple of the step, so every move
//     ends on the step grid (a command that ends inside the step already moved runs no step at all);
//   - at 0x820E9C88, after `add r11,r10,r11` (r10 the step's start, r11 its end): no step is longer than one.
// Off (0), both hooks return at once.
namespace {
int32_t MoveStepMs() {
  const int32_t hz = REXCVAR_GET(waw_move_hz);
  if (hz <= 0) return 0;
  const int32_t clamped = std::clamp(hz, 20, 1000);
  return std::max(1, static_cast<int32_t>(std::lround(1000.0 / clamped)));
}
}  // namespace

void waw_move_grid(PPCContext& ctx, uint8_t* base) {
  (void)base;
  const int32_t step = MoveStepMs();
  if (step <= 0) return;
  const int32_t end = ctx.r31.s32;
  if (end <= 0) return;
  const int32_t rounded = ((end + step - 1) / step) * step;
  ctx.r31.s64 = rounded;
}
namespace {
// The step this thread's Pmove is about to run (recorded by waw_move_step on every step, fixed steps or not), for the
// velocity snap at the end of that step's PmoveSingle. The server thread and the main thread (prediction) each have
// their own.
struct MoveStepRecord {
  int32_t start = 0;
  int32_t end = 0;
  bool valid = false;
};
thread_local MoveStepRecord t_move_step;
}  // namespace

void waw_move_step(PPCContext& ctx, uint8_t* base) {
  (void)base;
  const int32_t step = MoveStepMs();
  const int32_t start = ctx.r10.s32;
  if (step > 0 && ctx.r11.s32 - start > step) ctx.r11.s64 = start + step;
  t_move_step = {start, ctx.r11.s32, true};
}

// `waw_move_snap_hz` (11-08): the velocity snap. PmoveSingle 0x820E8E10 ends (after its "moved less than half the
// velocity" check) with 0x822A30B0(&ps->velocity), Sys_SnapVector: each component through fctiwz and back, a cut
// toward zero to whole units, on every step. While a jump rises, each cut takes 0 to 1 unit/s of upward speed (a
// steady fraction g * dt of a unit each step: with g 800, 6.4 a step at 8 ms, so a cut of 0.6), and so the more steps
// a jump has, the lower it ends: 38.2 units at 30 fps, 37.9 at 60, 35.8 at 125 Hz steps, about 31 at 250 fps and up
// (the upward speed then falls by a whole unit or more each step, 1.25 x gravity), against jump_height 39 without
// the cut (NOTES 11-08 00:00). PmoveSingle's call at 0x820E9BB8 is the function's only caller in the whole program.
// With N > 0, the cut runs only on a step whose end is past a boundary of a fixed N Hz clock on the command time
// (floor(end * N / 1000) > floor(start * N / 1000)), so a jump has the same cuts at any frame rate or step size;
// the step comes from waw_move_step's record when its end is this player's commandTime, else the game's cut runs.
// The server's moves and the client's prediction use the same times, so the two agree. Returns true to skip the cut.
namespace {
constexpr uint32_t kSnapReturnAddress = 0x820E9BBC;  // after PmoveSingle's `bl 0x822A30B0`

int64_t FloorDiv(int64_t value, int64_t divisor) {
  const int64_t quotient = value / divisor;
  return (value % divisor != 0 && (value < 0) != (divisor < 0)) ? quotient - 1 : quotient;
}

// True while a jump rises: PMF_JUMPING set and the upward speed above 0.
bool JumpRising(const uint8_t* base, uint32_t ps) {
  return (ReadGuestU32(base, ps + kPlayerStateFlags) & kPlayerFlagJumping) != 0 &&
         ReadGuestFloat(base, ps + kPlayerStateVelocity + 8) > 0.0f;
}
}  // namespace

bool waw_move_snap(PPCContext& ctx, uint8_t* base) {
  const uint32_t velocity = ctx.r3.u32;
  if (static_cast<uint32_t>(ctx.lr) != kSnapReturnAddress || velocity < 0x10000 + kPlayerStateVelocity) return false;
  const uint32_t ps = velocity - kPlayerStateVelocity;
  const bool counting = REXCVAR_GET(waw_log_clocks) > 0;
  const int32_t hz = REXCVAR_GET(waw_move_snap_hz);
  bool skip = false;
  if (hz > 0) {
    const auto command_time = static_cast<int32_t>(ReadGuestU32(base, ps + kPlayerStateCommandTime));
    if (t_move_step.valid && t_move_step.end == command_time) {
      const int64_t rate = std::clamp(hz, 20, 1000);
      skip = FloorDiv(int64_t{t_move_step.end} * rate, 1000) == FloorDiv(int64_t{t_move_step.start} * rate, 1000);
    } else if (counting) {
      g_clocks.snaps_unmatched.fetch_add(1, std::memory_order_relaxed);
    }
  }
  t_move_step.valid = false;
  if (counting) {
    if (skip) {
      g_clocks.snaps_skipped.fetch_add(1, std::memory_order_relaxed);
    } else {
      g_clocks.snaps.fetch_add(1, std::memory_order_relaxed);
      if (JumpRising(base, ps)) {
        const float up = ReadGuestFloat(base, velocity + 8);
        g_clocks.snap_rise_taken.fetch_add(static_cast<int64_t>((up - std::trunc(up)) * 1000.0f),
                                           std::memory_order_relaxed);
      }
    }
  }
  return skip;
}

// PM_StepSlideMove 0x820EA328 (ps in r29 throughout): its step-down's velocity changes and the jump clamp, counted
// for the jump question (11-08). The earlier lead was that the 9-unit step-down catches a jump's first short steps;
// in the code it runs only with ground under the player (pml.groundPlane) or after a stepped move, and in the air
// without a bump the function returns right after PM_SlideMove, so these should stay near 0 while a jump rises.
namespace {
void CountStepDown(const uint8_t* base, uint32_t ps, std::atomic<uint64_t>& all, std::atomic<uint64_t>& rising) {
  if (REXCVAR_GET(waw_log_clocks) <= 0 || ps < 0x10000) return;
  all.fetch_add(1, std::memory_order_relaxed);
  if (JumpRising(base, ps)) rising.fetch_add(1, std::memory_order_relaxed);
}
}  // namespace

void waw_count_step_down_project(PPCContext& ctx, uint8_t* base) {
  CountStepDown(base, ctx.r29.u32, g_clocks.step_down_projects, g_clocks.step_down_projects_rising);
}
void waw_count_step_down_clip(PPCContext& ctx, uint8_t* base) {
  CountStepDown(base, ctx.r29.u32, g_clocks.step_down_clips, g_clocks.step_down_clips_rising);
}
void waw_count_jump_clamp(PPCContext& ctx, uint8_t* base) {
  (void)ctx; (void)base;
  if (REXCVAR_GET(waw_log_clocks) > 0) g_clocks.jump_clamps.fetch_add(1, std::memory_order_relaxed);
}
void waw_count_client_frame(PPCContext& ctx, uint8_t* base) {
  (void)base;
  MarkFramePart(kPartClientGame);
  g_clocks.client_frames.fetch_add(1, std::memory_order_relaxed);
  g_clocks.client_time.store(ctx.r4.s32, std::memory_order_relaxed);
}
void waw_count_script_tick(PPCContext& ctx, uint8_t* base) {
  (void)ctx; (void)base;
  g_clocks.script_ticks.fetch_add(1, std::memory_order_relaxed);
}
void waw_count_anim_lerp(PPCContext& ctx, uint8_t* base) {
  (void)ctx; (void)base;
  g_clocks.anim_lerps.fetch_add(1, std::memory_order_relaxed);
}
// BinkDoFrame 0x824733A0 (R_Cinematic's update calls it once a new picture is due, after BinkWait): the movie pictures
// decoded, for the films' pictures a second against their own rate (11-07 23:25).
void waw_count_bink_picture(PPCContext& ctx, uint8_t* base) {
  (void)ctx; (void)base;
  g_clocks.bink_pictures.fetch_add(1, std::memory_order_relaxed);
}

namespace {

// The native renderer's census (NATIVE-RENDERER.md, step 1; 2026-11-07). The render thread's pass setup 0x82437A90
// (r3 the command buffer context, r4 the pass index) and indexed draw R_DrawIndexedPrimitive 0x82432268 (r4 the draw's
// arguments: +4 triangle count) are counted per technique type and per material. Context: +0x128 the material, +0x12C
// the technique type. A material's first word points at its name. The draw-list loop 0x824223D0 calls each surface
// type's tess function through the 16-word table at 0x820ABEC4, logged once.
// The first census (11-07 18:41) saw the loading screen's 9,079 unlit passes with no indexed draw: 2D and the other
// tess-buffer surfaces draw through 0x824322B8, which the tess path 0x82428578 calls after its pass setup. That draw is
// counted on its own ("tess draws"; its arguments aren't read yet).
constexpr uint32_t kContextMaterial = 0x128;
constexpr uint32_t kContextTechniqueType = 0x12C;
constexpr uint32_t kContextTechnique = 0x130;   // the material's technique for that type
constexpr uint32_t kContextPass = 0x134;        // the technique's pass: technique + 8 + index * 0x58
constexpr uint32_t kContextStateBits = 0x1154;  // the render state bits the pass set
constexpr uint32_t kPassSize = 0x58;
constexpr uint32_t kMaterialDumpWords = 24;
// For the 360 struct map (NATIVE-RENDERER.md step 2): the first time each technique type is set up, the context's
// words, the material, the technique's header and the pass go into the log as raw words ("native census struct"); so
// do the first indexed draw's prim state and arguments for each type, and the first tess draws' registers. Only
// pointers the game itself reads are followed.
constexpr uint32_t kTessTable = 0x820ABEC4;
constexpr uint32_t kTessTableCount = 16;
constexpr uint32_t kTechniqueTypes = 64;

struct Census {
  std::mutex mutex;
  uint64_t passes = 0, draws = 0, indices = 0, tess_draws = 0;
  uint64_t passes_by_type[kTechniqueTypes] = {};
  uint64_t draws_by_type[kTechniqueTypes] = {};
  uint64_t tess_draws_by_type[kTechniqueTypes] = {};
  std::map<uint32_t, uint64_t> draws_by_material;
  uint32_t material = 0, technique_type = 0;
  std::chrono::steady_clock::time_point last = std::chrono::steady_clock::now();
  bool pass_logged[kTechniqueTypes] = {};
  bool draw_logged[kTechniqueTypes] = {};
  uint32_t tess_logged = 0;
};
Census g_census;
std::atomic<uint64_t> g_census_frames{0};  // Com_Frame calls since the last census line (main thread)

// `count` big-endian words from `address`, as " XXXXXXXX ...".
std::string GuestWords(const uint8_t* base, uint32_t address, uint32_t count) {
  if (address < 0x10000) return " -";
  std::string out;
  for (uint32_t i = 0; i < count; ++i) {
    char word[12];
    std::snprintf(word, sizeof(word), " %08X", ReadGuestU32(base, address + i * 4));
    out += word;
  }
  return out;
}

std::string GuestName(const uint8_t* base, uint32_t address) {
  if (address < 0x10000) return "?";
  const char* p = reinterpret_cast<const char*>(base + address);
  const size_t n = strnlen(p, 64);
  if (n == 0 || n == 64) return "?";
  for (size_t i = 0; i < n; ++i) {
    if (p[i] < 0x20 || p[i] > 0x7E) return "?";
  }
  return std::string(p, n);
}

void LogCensus(const uint8_t* base, double seconds) {  // g_census.mutex held
  Census& c = g_census;
  std::string types;
  for (uint32_t t = 0; t < kTechniqueTypes; ++t) {
    if (c.passes_by_type[t] || c.draws_by_type[t] || c.tess_draws_by_type[t])
      types += " " + std::to_string(t) + ":" + std::to_string(c.passes_by_type[t]) + "/" +
               std::to_string(c.draws_by_type[t]) + "/" + std::to_string(c.tess_draws_by_type[t]);
  }
  std::vector<std::pair<uint64_t, uint32_t>> top;
  for (const auto& [material, draws] : c.draws_by_material) top.push_back({draws, material});
  std::sort(top.rbegin(), top.rend());
  std::string materials;
  for (size_t i = 0; i < top.size() && i < 12; ++i) {
    const uint32_t name = top[i].second >= 0x10000 ? ReadGuestU32(base, top[i].second) : 0;
    materials += " " + GuestName(base, name) + ":" + std::to_string(top[i].first);
  }
  const uint64_t frames = g_census_frames.exchange(0, std::memory_order_relaxed);
  REXLOG_INFO("waw: native census over {:.1f} s, {} frames: passes {}, draws {}, indices {}, tess draws {}, "
              "materials {}",
              seconds, frames, c.passes, c.draws, c.indices, c.tess_draws, c.draws_by_material.size());
  REXLOG_INFO("waw: native census technique type:passes/draws/tess draws{}", types.empty() ? " -" : types);
  REXLOG_INFO("waw: native census top materials (draws){}", materials.empty() ? " -" : materials);
  c.passes = c.draws = c.indices = c.tess_draws = 0;
  std::fill(std::begin(c.passes_by_type), std::end(c.passes_by_type), 0);
  std::fill(std::begin(c.draws_by_type), std::end(c.draws_by_type), 0);
  std::fill(std::begin(c.tess_draws_by_type), std::end(c.tess_draws_by_type), 0);
  c.draws_by_material.clear();
}

}  // namespace

// The pass setup 0x82437A90 (census only; nothing is changed).
void waw_native_pass(PPCContext& ctx, uint8_t* base) {
  if (REXCVAR_GET(waw_debug_native_census) <= 0 || ctx.r3.u32 < 0x10000) return;
  std::lock_guard<std::mutex> lock(g_census.mutex);
  static bool table_logged = false;
  if (!table_logged) {
    table_logged = true;
    std::string table;
    for (uint32_t i = 0; i < kTessTableCount; ++i) {
      char word[12];
      std::snprintf(word, sizeof(word), " %08X", ReadGuestU32(base, kTessTable + i * 4));
      table += word;
    }
    REXLOG_INFO("waw: native census tess table (surface type 0-15):{}", table);
  }
  const uint32_t context = ctx.r3.u32;
  g_census.material = ReadGuestU32(base, context + kContextMaterial);
  g_census.technique_type = ReadGuestU32(base, context + kContextTechniqueType);
  ++g_census.passes;
  const uint32_t type = g_census.technique_type;
  if (type >= kTechniqueTypes) return;
  ++g_census.passes_by_type[type];
  if (g_census.pass_logged[type]) return;
  g_census.pass_logged[type] = true;
  const uint32_t material = g_census.material;
  const uint32_t technique = ReadGuestU32(base, context + kContextTechnique);
  const uint32_t pass = ReadGuestU32(base, context + kContextPass);
  REXLOG_INFO("waw: native census struct type {}: context {:08X} +0x128:{}; state bits:{}; r4 {:08X}", type, context,
              GuestWords(base, context + kContextMaterial, 6), GuestWords(base, context + kContextStateBits, 2),
              ctx.r4.u32);
  REXLOG_INFO("waw: native census struct type {}: material {:08X} {}:{}", type, material,
              GuestName(base, material >= 0x10000 ? ReadGuestU32(base, material) : 0),
              GuestWords(base, material, kMaterialDumpWords));
  REXLOG_INFO("waw: native census struct type {}: technique {:08X} {}:{}; pass {:08X}:{}", type, technique,
              GuestName(base, technique >= 0x10000 ? ReadGuestU32(base, technique) : 0),
              GuestWords(base, technique, 2), pass, GuestWords(base, pass, kPassSize / 4));
}

// R_DrawIndexedPrimitive 0x82432268 (census only).
void waw_native_draw(PPCContext& ctx, uint8_t* base) {
  const int32_t every = REXCVAR_GET(waw_debug_native_census);
  if (every <= 0) return;
  std::lock_guard<std::mutex> lock(g_census.mutex);
  Census& c = g_census;
  ++c.draws;
  if (ctx.r4.u32 >= 0x10000) c.indices += 3ull * ReadGuestU32(base, ctx.r4.u32 + 4);
  if (c.technique_type < kTechniqueTypes) ++c.draws_by_type[c.technique_type];
  if (c.technique_type < kTechniqueTypes && !c.draw_logged[c.technique_type]) {
    c.draw_logged[c.technique_type] = true;
    REXLOG_INFO("waw: native census struct type {}: indexed draw r3 {:08X}:{}; r4 {:08X}:{}", c.technique_type,
                ctx.r3.u32, GuestWords(base, ctx.r3.u32, 8), ctx.r4.u32, GuestWords(base, ctx.r4.u32, 6));
  }
  ++c.draws_by_material[c.material];
  const auto now = std::chrono::steady_clock::now();
  if (now - c.last >= std::chrono::seconds(every)) {
    LogCensus(base, std::chrono::duration<double>(now - c.last).count());
    c.last = now;
  }
}

// The tess buffer's draw 0x824322B8 (census only): 2D, effects and the other surfaces the tess path 0x82428578 draws.
void waw_native_draw_tess(PPCContext& ctx, uint8_t* base) {
  (void)base;
  if (REXCVAR_GET(waw_debug_native_census) <= 0) return;
  std::lock_guard<std::mutex> lock(g_census.mutex);
  Census& c = g_census;
  ++c.tess_draws;
  if (c.technique_type < kTechniqueTypes) ++c.tess_draws_by_type[c.technique_type];
  if (c.tess_logged < 3) {
    ++c.tess_logged;
    REXLOG_INFO("waw: native census struct tess draw: r3 {:08X} r4 {:08X} r5 {:08X} r6 {:08X} r7 {:08X} r8 {:08X}",
                ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, ctx.r8.u32);
  }
}

// The start of Com_Frame (0x8223D8C0, once a frame on the main thread): runs waw_debug_command once, after
// waw_debug_command_delay seconds, through the console buffer (the way the MW2 recomp's tests reach a level); counts
// the frame and logs the audit's counters every waw_log_clocks seconds; logs the ragdoll pool every waw_log_ragdolls
// seconds.
// `waw_fov`: cg_fov's dvar (its pointer at 0x844C9C04, set by CG_RegisterDvars 0x822EBC40) keeps its current value
// as a float at +0x0C (`lfs f28,12(r11)` in 0x820F6EA0). Levels set cg_fov themselves (pel1a set 65 at its start in
// jmp_off2, 11-07 22:07, after the console's `set cg_fov 100`), so the value is written once a frame instead.
namespace {
void HoldFov(uint8_t* base) {
  const double wanted = REXCVAR_GET(waw_fov);
  if (wanted <= 0.0) return;
  constexpr uint32_t kCgFovDvarPointer = 0x844C9C04;
  const uint32_t dvar = ReadGuestU32(base, kCgFovDvarPointer);
  if (dvar < 0x40000000) return;  // not registered yet
  const float fov = static_cast<float>(std::clamp(wanted, 1.0, 160.0));
  const float current = ReadGuestFloat(base, dvar + 0x0C);
  if (current == fov) return;
  static std::atomic<int> logged{0};
  if (logged.fetch_add(1) < 5) REXLOG_INFO("waw: cg_fov held at {} (the game had {})", fov, current);
  uint32_t bits;
  std::memcpy(&bits, &fov, sizeof(bits));
  bits = __builtin_bswap32(bits);
  std::memcpy(base + dvar + 0x0C, &bits, sizeof(bits));
}
}  // namespace

void waw_frame_start(PPCContext& ctx, uint8_t* base) {
  using clock = std::chrono::steady_clock;
  static const auto first_frame = clock::now();

  // waw_max_fps: wait here, at the frame's start, until this frame's time on the host's clock. Most of the wait sleeps;
  // the last millisecond yields, so the limit holds to well under a millisecond. A frame that ran late moves the
  // schedule on instead of running the next ones fast to catch up.
  if (const int32_t max_fps = REXCVAR_GET(waw_max_fps); max_fps > 0) {
    static clock::time_point next_frame = clock::now();
    const auto period = std::chrono::nanoseconds(1000000000LL / std::min(max_fps, 1000));
    auto waiting = clock::now();
    if (next_frame + period < waiting) next_frame = waiting;
    while (waiting < next_frame) {
      const auto left = next_frame - waiting;
      if (left > std::chrono::microseconds(1500)) {
        std::this_thread::sleep_for(left - std::chrono::milliseconds(1));
      } else {
        std::this_thread::yield();
      }
      waiting = clock::now();
    }
    next_frame += period;
  }
  const auto now = clock::now();

  HoldFov(base);
  // [f2-menus] Commands from the F2 window (typed on its CONSOLE page, or a row that sets a game dvar), run through
  // the game's command buffer like the debug commands below. Nothing is copied on a frame without any.
  if (waw::f2ui::HasCommands()) {
    static std::vector<std::string> f2_commands;
    waw::f2ui::TakeCommands(f2_commands);
    for (const std::string& command : f2_commands) QueueConsoleCommand(ctx, base, command);
    f2_commands.clear();
  }
  waw::keys::RunFrame(ctx, base);  // [f2-menus] keys the PC way (waw_keys.cpp): queued key events into CL_KeyEvent
  g_clocks.frames.fetch_add(1, std::memory_order_relaxed);
  g_census_frames.fetch_add(1, std::memory_order_relaxed);
  if (const int32_t every = REXCVAR_GET(waw_log_clocks); every > 0) {
    static auto last_clocks = first_frame;
    if (now - last_clocks >= std::chrono::seconds(every)) {
      LogClocks(base, std::chrono::duration<double>(now - last_clocks).count());
      last_clocks = now;
    }
  }

  if (const int32_t every = REXCVAR_GET(waw_log_ragdolls); every > 0) {
    static auto last_dump = first_frame;
    if (now - last_dump >= std::chrono::seconds(every)) {
      last_dump = now;
      LogRagdollBodies(base);
    }
  }

  // waw_debug_commands: parsed once; each entry runs once, in order, when its time has come.
  struct TimedCommand {
    double seconds;
    std::string command;
  };
  static const std::vector<TimedCommand> timed = [] {
    std::vector<TimedCommand> out;
    const std::string all = REXCVAR_GET(waw_debug_commands);
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
  static size_t next_timed = 0;
  const double since_first = std::chrono::duration<double>(now - first_frame).count();
  while (next_timed < timed.size() && since_first >= timed[next_timed].seconds) {
    QueueConsoleCommand(ctx, base, timed[next_timed].command);
    REXLOG_INFO("waw: debug command queued: {}", timed[next_timed].command);
    ++next_timed;
  }

  static bool command_done = false;
  if (command_done) return;
  if (now - first_frame < std::chrono::seconds(REXCVAR_GET(waw_debug_command_delay))) return;
  command_done = true;
  const std::string command = REXCVAR_GET(waw_debug_command);
  if (command.empty()) return;
  QueueConsoleCommand(ctx, base, command);
  REXLOG_INFO("waw: debug command queued: {}", command);
}

// `waw_log_menu_items` (11-07 21:18, for MENUS.md): the UI context Menus_OpenByName gets in r3 keeps its loaded menus
// at +0x30 (menuDef_t pointers, up to 120) and their count at +0x210 (0x8225A1F8 walks them by name). A menu: its name
// at +0, its item count at +0xB0, its item pointers at +0x134 (Menu_HandleKey 0x82261BB8); +0xDC its open script
// (Menus_Open 0x8225FC78), +0xE4 its close script (Menus_Close 0x8225F248), +0xE8 its back-button script. The other
// fields aren't mapped yet, so each menu and item is logged as every readable string one of its words points to, by
// offset ("+134=..."). Each menu goes in once a run. Strings up to 1,024 characters are kept (menus1 at 160 lost the
// longer button scripts).
namespace {
constexpr uint32_t kUiMenus = 0x30;
constexpr uint32_t kUiMenuCount = 0x210;
constexpr uint32_t kMaxUiMenus = 120;
constexpr uint32_t kMenuItemCount = 0xB0;
constexpr uint32_t kMenuItems = 0x134;
constexpr uint32_t kMaxMenuItems = 256;
constexpr uint32_t kMenuWords = 0x140 / 4;
constexpr uint32_t kItemWords = 0x200 / 4;
constexpr uint32_t kMaxString = 1024;

// Whether the guest bytes [address, address + length) can be read now (both ends' pages).
bool GuestReadable(const uint8_t* base, uint32_t address, uint32_t length) {
  if (address < 0x10000 || address + length < address) return false;
  for (const uint32_t at : {address, address + length - 1}) {
    void* host = const_cast<uint8_t*>(base) + at;
#if REX_PLATFORM_LINUX
    const int probe = rex::memory::ProbeAccess(host, false);
    if (probe == 1) continue;
    if (probe == 0) return false;
#endif
    size_t span = 1;
    rex::memory::PageAccess access = rex::memory::PageAccess::kNoAccess;
    if (!rex::memory::QueryProtect(host, span, access) || access == rex::memory::PageAccess::kNoAccess) return false;
  }
  return true;
}

// The printable string at `address`, or "" when it isn't one (2 to kMaxString characters). It reads one 4 KB page at
// a time, so a short string at the end of a mapped region is still found.
std::string GuestText(const uint8_t* base, uint32_t address) {
  if (address < 0x40000000 || address == 0xFFFFFFFF) return {};
  std::string text;
  uint64_t at = address;
  while (text.size() < kMaxString && at <= 0xFFFFFFFFull) {
    if (!GuestReadable(base, static_cast<uint32_t>(at), 1)) return {};
    const uint64_t page_end = std::min<uint64_t>((at | 0xFFF) + 1, 0x100000000ull);
    for (; at < page_end && text.size() < kMaxString; ++at) {
      char c = static_cast<char>(base[at]);
      if (c == 0) return text.size() >= 2 ? text : std::string();
      if (c == '\t' || c == '\n' || c == '\r') c = ' ';
      if (c < 0x20 || c > 0x7E) return {};
      text.push_back(c);
    }
  }
  return {};  // longer than kMaxString: not a string we keep
}

// " +OFF=\"text\"" for each of `words` words at `object` that points to a string.
std::string StringFields(const uint8_t* base, uint32_t object, uint32_t words) {
  std::string out;
  for (uint32_t i = 0; i < words; ++i) {
    const std::string text = GuestText(base, ReadGuestU32(base, object + i * 4));
    if (text.empty()) continue;
    char offset[16];
    std::snprintf(offset, sizeof(offset), " +%X=\"", i * 4);
    out += offset + text + "\"";
  }
  return out;
}

std::mutex g_menus_logged_mutex;
std::vector<uint32_t> g_menus_logged;

void LogMenuItems(const uint8_t* base, uint32_t context) {
  if (!GuestReadable(base, context, kUiMenuCount + 4)) return;
  const uint32_t count = std::min(ReadGuestU32(base, context + kUiMenuCount), kMaxUiMenus);
  std::lock_guard<std::mutex> lock(g_menus_logged_mutex);
  for (uint32_t m = 0; m < count; ++m) {
    const uint32_t menu = ReadGuestU32(base, context + kUiMenus + m * 4);
    if (!GuestReadable(base, menu, kMenuWords * 4)) continue;
    if (std::find(g_menus_logged.begin(), g_menus_logged.end(), menu) != g_menus_logged.end()) continue;
    g_menus_logged.push_back(menu);
    const uint32_t items = std::min(ReadGuestU32(base, menu + kMenuItemCount), kMaxMenuItems);
    const uint32_t list = ReadGuestU32(base, menu + kMenuItems);
    const std::string name = GuestText(base, ReadGuestU32(base, menu));
    REXLOG_INFO("waw: menu items: menu {} ({} of {} loaded) {:08X}, {} items:{}", name.empty() ? "?" : name, m + 1,
                count, menu, items, StringFields(base, menu, kMenuWords));
    if (items == 0 || !GuestReadable(base, list, items * 4)) continue;
    for (uint32_t i = 0; i < items; ++i) {
      const uint32_t item = ReadGuestU32(base, list + i * 4);
      if (!GuestReadable(base, item, kItemWords * 4)) continue;
      REXLOG_INFO("waw: menu items: {} item {}/{}:{}", name.empty() ? "?" : name, i + 1, items,
                  StringFields(base, item, kItemWords));
    }
  }
}
}  // namespace

// Menus_OpenByName (0x8225FDF0; r4 the menu's name): each menu the game opens, in the log, so a test can tell which
// screen it is on.
void waw_menu_open(PPCContext& ctx, uint8_t* base) {
  if (!REXCVAR_GET(waw_log_menus)) return;
  const uint32_t text = ctx.r4.u32;
  if (text < 0x10000) return;
  constexpr size_t kMaxName = 64;
  const char* p = reinterpret_cast<const char*>(base + text);
  const size_t n = strnlen(p, kMaxName);
  if (n == 0 || n == kMaxName) return;
  for (size_t i = 0; i < n; ++i) {
    if (p[i] < 0x20 || p[i] > 0x7E) return;  // not a name
  }
  REXLOG_INFO("waw: menu open {}", std::string_view(p, n));
  if (REXCVAR_GET(waw_log_menu_items)) LogMenuItems(base, ctx.r3.u32);
}

// Menus_Close (0x8225F248; r3 the UI context, r4 the menu): called by Menus_CloseByName 0x8225F380, Script_Close
// 0x8225F480 and Script_InGameClose 0x8225F530 (the B button's "close self" goes this way), so a test can tell when a
// screen went away. It is called for menus that aren't open too (it looks for the menu in the open stack first).
void waw_menu_close(PPCContext& ctx, uint8_t* base) {
  if (!REXCVAR_GET(waw_log_menus)) return;
  const uint32_t menu = ctx.r4.u32;
  if (menu < 0x10000 || !GuestReadable(base, menu, 4)) return;
  const std::string name = GuestText(base, ReadGuestU32(base, menu));
  if (name.empty()) return;
  REXLOG_INFO("waw: menu close {}", name);
}

// UI_SetActiveMenu (0x82267380; r3 the local client, r4 the front end's state, a number): in the log beside the menus.
void waw_set_active_menu(PPCContext& ctx, uint8_t* base) {
  (void)base;
  if (!REXCVAR_GET(waw_log_menus)) return;
  REXLOG_INFO("waw: UI_SetActiveMenu({}, {})", ctx.r3.s32, ctx.r4.s32);
}


// `waw_native_skin` (11-07 22:15): the rigid part of model skinning, 0x824472B0, in native code. In Makin, queue 23
// (model skinning, 0x82447748) is the scene workers' biggest command: 8-10 ms of their time and 2 ms of the main
// thread's a frame (xw0c, 11-07 20:58). 0x82447748 builds each bone's matrix, then skins each surface: rigid ones here
// (every vertex group under one bone), weighted ones in 0x82447438 (one to four bones a vertex).
//
// What 0x824472B0 does, read from its code (r3 the surface, r5 the bone matrices, r6 the output):
//   source vertices at surface +24, 32 bytes each; groups at surface +64, 12 bytes each (+0 the bone matrix's byte
//   offset from r5, +2 the group's vertex count); the group count at surface +60. For each vertex:
//   - position: p' = M0 * p.x + M1 * p.y + M2 * p.z + M3 (the bone matrix's four rows), its fourth word kept from p
//     (a vperm with the control at 0x824BFC60);
//   - the words at +28 and +24 (packed 2:10:10:10 normal and tangent, vupkd3d128 type 2) are unpacked, scaled (the
//     constants at 0x82000DC0 and 0x82000DD0), turned by M0-M2, scaled back (0x82000DB0, 0x820A69C0) and packed
//     again (vpkd3d128 type 2); the words at +16 and +20 are copied.
// The arithmetic is done in the same order with the same separate multiplies and adds as the SDK's translation of
// each instruction (vmaddfp is a multiply, then an add), under the same flush-to-zero mode, and the pack and unpack
// follow the SDK's (src/codegen/builders/vector.cpp), so the results should be the same bits. `waw_native_skin=2`
// checks that on every call.
//
// 0x82446A98 (11-07 22:55), the weighted path's first part (0x82447438 calls it for the vertices with one bone each):
// r3 the source vertices, r4 one u16 a vertex (that vertex's bone matrix, a byte offset from r6), r5 the vertex
// count, r6 the bone matrices, r7 the output. Each vertex gets the same arithmetic as a rigid one, with its own
// matrix (read from its code: the same instructions in the same order, the same constants and permute).
//
// 0x82446C28 (11-07 23:00), two bones a vertex: r4 is six bytes a vertex (+0 and +2 the two bone matrices' byte offsets
// from r6, +4 a 16-bit weight w for the second). The weight comes in with an lvlx128 of its two bytes, a vperm (the
// control at 0x84E9C140, the second source at 0x82000B20) and vcuxwfp128 (unsigned word to float, times 2^-16). The
// position is (1 - w) * (bone 0's) + w * (bone 1's), 1 being the vector at 0x82000B40; the normal and the tangent turn
// with bone 0's matrix alone.
//
// 0x82446E18 and 0x82447048 (11-07 23:05), three and four bones a vertex, the same way: a record of 4N - 2 bytes (bone
// 0's offset at +0, then for bone k = 1 to N - 1 its offset at +4k - 2 and its weight at +4k). The position is
// w1 * p1 + w2 * p2 (+ w3 * p3), summed in that order, plus (1 - (w1 + w2 (+ w3))) * p0; the normal and tangent with
// bone 0 alone.
namespace {

struct alignas(16) GuestVec {
  float f[4];  // f[i] is the guest's element i
};

inline __m128 LoadGuestVec(const uint8_t* base, uint32_t address) {
  address &= ~15u;  // lvx128 ignores the low four bits
  uint32_t words[4];
  std::memcpy(words, base + address, sizeof(words));
  for (uint32_t& w : words) w = __builtin_bswap32(w);
  return _mm_castsi128_ps(_mm_loadu_si128(reinterpret_cast<const __m128i*>(words)));
}

inline uint32_t FloatBits(float value) {
  uint32_t bits;
  std::memcpy(&bits, &value, sizeof(bits));
  return bits;
}

inline float BitsFloat(uint32_t bits) {
  float value;
  std::memcpy(&value, &bits, sizeof(value));
  return value;
}

inline __m128 Splat(__m128 v, int lane) {
  switch (lane) {
    case 0: return _mm_shuffle_ps(v, v, _MM_SHUFFLE(0, 0, 0, 0));
    case 1: return _mm_shuffle_ps(v, v, _MM_SHUFFLE(1, 1, 1, 1));
    case 2: return _mm_shuffle_ps(v, v, _MM_SHUFFLE(2, 2, 2, 2));
    default: return _mm_shuffle_ps(v, v, _MM_SHUFFLE(3, 3, 3, 3));
  }
}

// vmaddfp as the SDK translates it: a * b, then + c.
inline __m128 MulAdd(__m128 a, __m128 b, __m128 c) { return _mm_add_ps(_mm_mul_ps(a, b), c); }

// vupkd3d128 type 2 (NORMPACKED32) of one packed word: x, y, z (signed 10 bits each) in "3.0 + X" form, w (2 bits) in
// "1.0 + w" form, as the SDK unpacks them (x = -512 gives a NaN).
inline __m128 UnpackNormPacked32(uint32_t raw) {
  const int32_t x = static_cast<int32_t>(raw << 22) >> 22;
  const int32_t y = static_cast<int32_t>(raw << 12) >> 22;
  const int32_t z = static_cast<int32_t>(raw << 2) >> 22;
  const uint32_t bits[4] = {x == -512 ? 0x7FC00000u : static_cast<uint32_t>(x + 0x40400000),
                            static_cast<uint32_t>(y + 0x40400000), static_cast<uint32_t>(z + 0x40400000),
                            (raw >> 30) | 0x3F800000u};
  return _mm_castsi128_ps(_mm_loadu_si128(reinterpret_cast<const __m128i*>(bits)));
}

// vpkd3d128 type 2 (NORMPACKED32) of one vector into one word, clamped as the SDK does (a NaN passes the clamps).
inline uint32_t PackNormPacked32(__m128 v) {
  constexpr uint32_t kMin10 = 0x403FFE01u, kMax10 = 0x404001FFu, kMin2 = 0x40400000u, kMax2 = 0x40400003u;
  alignas(16) float e[4];
  _mm_store_ps(e, v);
  auto clamp = [](float f, uint32_t lo, uint32_t hi) {
    const float low = BitsFloat(lo), high = BitsFloat(hi);
    return f < low ? low : (f > high ? high : f);
  };
  uint32_t packed = FloatBits(clamp(e[0], kMin10, kMax10)) & 0x3FF;
  packed |= (FloatBits(clamp(e[1], kMin10, kMax10)) & 0x3FF) << 10;
  packed |= (FloatBits(clamp(e[2], kMin10, kMax10)) & 0x3FF) << 20;
  packed |= (FloatBits(clamp(e[3], kMin2, kMax2)) & 0x3) << 30;
  return packed;
}

constexpr uint32_t kSkinPermControl = 0x824BFC60;  // v6: the position's permute
constexpr uint32_t kSkinPackScale = 0x82000DB0;    // v8
constexpr uint32_t kSkinPackBias = 0x820A69C0;     // v9
constexpr uint32_t kSkinUnpackBias = 0x82000DD0;   // v10
constexpr uint32_t kSkinUnpackScale = 0x82000DC0;  // v11

// The position's vperm, taken apart once: which of the eight words (0-3 the transformed position, 4-7 the source
// position's fourth word, splatted) each output word is. -1 when the control isn't whole words (then the native
// path isn't used).
std::atomic<int> g_skin_perm_ready{0};  // 0 not read, 1 whole words, 2 not usable
int g_skin_perm_words[4];

bool ReadSkinPermControl(const uint8_t* base) {
  const int ready = g_skin_perm_ready.load(std::memory_order_acquire);
  if (ready != 0) return ready == 1;
  uint8_t control[16];
  std::memcpy(control, base + kSkinPermControl, sizeof(control));
  bool whole_words = true;
  int words[4];
  for (int i = 0; i < 4; ++i) {
    const int first = control[i * 4] & 0x1F;
    whole_words = whole_words && first % 4 == 0;
    for (int b = 1; b < 4; ++b) whole_words = whole_words && (control[i * 4 + b] & 0x1F) == first + b;
    words[i] = first / 4;
  }
  if (whole_words) std::memcpy(g_skin_perm_words, words, sizeof(words));
  REXLOG_INFO("waw: native skinning: the position permute at {:08X} is {} ({} {} {} {})", kSkinPermControl,
              whole_words ? "whole words" : "not whole words, so the native path stays off", words[0], words[1],
              words[2], words[3]);
  g_skin_perm_ready.store(whole_words ? 1 : 2, std::memory_order_release);
  return whole_words;
}

// The constants every skinned vertex uses.
struct SkinConstants {
  __m128 pack_scale, pack_bias, unpack_bias, unpack_scale;
};

inline SkinConstants LoadSkinConstants(const uint8_t* base) {
  return {LoadGuestVec(base, kSkinPackScale), LoadGuestVec(base, kSkinPackBias), LoadGuestVec(base, kSkinUnpackBias),
          LoadGuestVec(base, kSkinUnpackScale)};
}

struct BoneMatrix {
  __m128 m0, m1, m2, m3;  // its four rows
};

inline BoneMatrix LoadBoneMatrix(const uint8_t* base, uint32_t matrix) {
  return {LoadGuestVec(base, matrix), LoadGuestVec(base, matrix + 16), LoadGuestVec(base, matrix + 32),
          LoadGuestVec(base, matrix + 48)};
}

// The position p under one bone's matrix, in the skinning functions' order.
inline __m128 TransformPosition(const BoneMatrix& m, __m128 p) {
  __m128 moved = MulAdd(m.m2, Splat(p, 2), m.m3);
  moved = MulAdd(Splat(p, 1), m.m1, moved);
  return MulAdd(Splat(p, 0), m.m0, moved);
}

// Writes one skinned vertex big-endian to `out`: the moved position (its fourth word from the source, by the
// permute), and the source's second 16 bytes with the normal and tangent turned by `m`.
inline void WriteSkinnedVertex(const uint8_t* base, uint32_t source, __m128 p, __m128 moved, const BoneMatrix& m,
                               const SkinConstants& c, uint8_t* out) {
  uint32_t extra[4];  // the second 16 bytes: two words copied, then the packed tangent and normal
  std::memcpy(extra, base + (source & ~15u) + 16, sizeof(extra));
  for (uint32_t& w : extra) w = __builtin_bswap32(w);
  const __m128 m0 = m.m0, m1 = m.m1, m2 = m.m2;
  alignas(16) float words_f[8];
  _mm_store_ps(words_f, moved);
  const float w_source = _mm_cvtss_f32(Splat(p, 3));
  for (int k = 4; k < 8; ++k) words_f[k] = w_source;
  uint32_t position[4];
  for (int k = 0; k < 4; ++k) position[k] = FloatBits(words_f[g_skin_perm_words[k]]);

  // The normal (word 3) and tangent (word 2).
  const __m128 n = MulAdd(UnpackNormPacked32(extra[3]), c.unpack_scale, c.unpack_bias);
  const __m128 t = MulAdd(UnpackNormPacked32(extra[2]), c.unpack_scale, c.unpack_bias);
  __m128 nn = _mm_mul_ps(Splat(n, 2), m2);
  __m128 tt = _mm_mul_ps(Splat(t, 2), m2);
  nn = MulAdd(Splat(n, 1), m1, nn);
  tt = MulAdd(Splat(t, 1), m1, tt);
  nn = MulAdd(Splat(n, 0), m0, nn);
  tt = MulAdd(Splat(t, 0), m0, tt);
  nn = MulAdd(nn, c.pack_scale, c.pack_bias);
  tt = MulAdd(tt, c.pack_scale, c.pack_bias);
  extra[3] = PackNormPacked32(nn);
  extra[2] = PackNormPacked32(tt);

  uint32_t bytes[8];
  for (int k = 0; k < 4; ++k) bytes[k] = __builtin_bswap32(position[k]);
  for (int k = 0; k < 4; ++k) bytes[4 + k] = __builtin_bswap32(extra[k]);
  std::memcpy(out, bytes, sizeof(bytes));
}

// One vertex (32 bytes at `source`) under the bone matrix at `matrix`, written big-endian to `out`.
inline void SkinOneVertex(const uint8_t* base, uint32_t source, uint32_t matrix, const SkinConstants& c,
                          uint8_t* out) {
  const BoneMatrix m = LoadBoneMatrix(base, matrix);
  const __m128 p = LoadGuestVec(base, source);
  WriteSkinnedVertex(base, source, p, TransformPosition(m, p), m, c, out);
}

// One rigid surface, written to `out` (guest memory, or a host buffer of the same layout when checking): the bytes
// at `out + (vertex index) * 32`. Returns the vertex count.
uint32_t SkinRigidSurface(const uint8_t* base, uint32_t surface, uint32_t bones, uint8_t* out) {
  const SkinConstants constants = LoadSkinConstants(base);
  uint32_t source = ReadGuestU32(base, surface + 24);
  const uint32_t groups = ReadGuestU32(base, surface + 60);
  uint32_t group = ReadGuestU32(base, surface + 64);
  uint32_t done = 0;
  for (uint32_t g = 0; g < groups; ++g, group += 12) {
    uint16_t offset_be, count_be;
    std::memcpy(&offset_be, base + group, 2);
    std::memcpy(&count_be, base + group + 2, 2);
    const uint32_t matrix = static_cast<uint32_t>(__builtin_bswap16(offset_be)) + bones;
    const uint32_t count = __builtin_bswap16(count_be);
    for (uint32_t i = 0; i < count; ++i, source += 32, ++done) {
      SkinOneVertex(base, source, matrix, constants, out + static_cast<size_t>(done) * 32);
    }
  }
  return done;
}

// The one-bone vertices of a weighted surface (0x82446A98's arguments), written to `out` like SkinRigidSurface.
void SkinOneBoneVertices(const uint8_t* base, uint32_t source, uint32_t offsets, uint32_t count, uint32_t bones,
                         uint8_t* out) {
  const SkinConstants constants = LoadSkinConstants(base);
  for (uint32_t i = 0; i < count; ++i, source += 32, offsets += 2) {
    uint16_t offset_be;
    std::memcpy(&offset_be, base + offsets, 2);
    const uint32_t matrix = static_cast<uint32_t>(__builtin_bswap16(offset_be)) + bones;
    SkinOneVertex(base, source, matrix, constants, out + static_cast<size_t>(i) * 32);
  }
}

constexpr uint32_t kSkinOne = 0x82000B40;          // v62: 1.0 in each word
constexpr uint32_t kSkinWeightSource = 0x82000B20;  // v63: the weight vperm's second source
constexpr uint32_t kSkinWeightControl = 0x84E9C140; // v2: the weight vperm's control

// The weight vector of one bone (k >= 1) of a weighted vertex: lvlx128 of its address (the bytes from there to the
// end of its 16-byte block, the rest zero), the vperm, then unsigned word to float times 2^-16, element by element in
// guest order.
inline __m128 LoadSkinWeight(const uint8_t* base, uint32_t address, const uint8_t* source16, const uint8_t* control16) {
  uint8_t loaded[16] = {};
  std::memcpy(loaded, base + address, 16 - (address & 15));
  alignas(16) float weight[4];
  for (int e = 0; e < 4; ++e) {
    uint32_t word = 0;
    for (int b = 0; b < 4; ++b) {
      const uint8_t select = control16[e * 4 + b] & 0x1F;
      word = (word << 8) | (select < 16 ? loaded[select] : source16[select - 16]);
    }
    weight[e] = static_cast<float>(word) * BitsFloat(0x37800000u);
  }
  return _mm_load_ps(weight);
}

// The weighted vertices with `bones_per_vertex` (2-4) bones each (0x82446C28, 0x82446E18 and 0x82447048's arguments),
// written to `out` like SkinRigidSurface.
void SkinWeightedVertices(const uint8_t* base, uint32_t source, uint32_t records, uint32_t count, uint32_t bones,
                          int bones_per_vertex, uint8_t* out) {
  const SkinConstants constants = LoadSkinConstants(base);
  const __m128 one = LoadGuestVec(base, kSkinOne);
  uint8_t weight_source[16], weight_control[16];  // guest byte order
  std::memcpy(weight_source, base + kSkinWeightSource, 16);
  std::memcpy(weight_control, base + kSkinWeightControl, 16);
  const uint32_t record_size = 4 * bones_per_vertex - 2;
  for (uint32_t i = 0; i < count; ++i, source += 32, records += record_size) {
    const __m128 p = LoadGuestVec(base, source);
    auto bone_matrix = [&](uint32_t offset_address) {
      uint16_t offset_be;
      std::memcpy(&offset_be, base + offset_address, 2);
      return LoadBoneMatrix(base, static_cast<uint32_t>(__builtin_bswap16(offset_be)) + bones);
    };
    const BoneMatrix m0 = bone_matrix(records);
    __m128 sum = _mm_setzero_ps(), blended = _mm_setzero_ps();
    for (int k = 1; k < bones_per_vertex; ++k) {
      const __m128 w = LoadSkinWeight(base, records + 4 * k, weight_source, weight_control);
      const __m128 moved = TransformPosition(bone_matrix(records + 4 * k - 2), p);
      if (k == 1) {
        sum = w;
        blended = _mm_mul_ps(moved, w);
      } else {
        sum = _mm_add_ps(sum, w);
        blended = MulAdd(w, moved, blended);
      }
    }
    const __m128 moved = MulAdd(_mm_sub_ps(one, sum), TransformPosition(m0, p), blended);
    WriteSkinnedVertex(base, source, p, moved, m0, constants, out + static_cast<size_t>(i) * 32);
  }
}

// The check (`waw_native_skin=2`): each worker thread's native result for the call in progress, compared with the
// translated code's at the function's end. One of these for each skinning function done natively.
struct SkinCheck {
  const char* name;
  std::atomic<uint64_t> calls{0}, vertices{0}, differ{0};
  std::atomic<int64_t> last_log_ms{0};
  std::atomic<bool> first_difference_logged{false};
};
SkinCheck g_skin_rigid{"rigid"};
SkinCheck g_skin_one_bone{"one bone"};
SkinCheck g_skin_two_bones{"two bones"};
SkinCheck g_skin_three_bones{"three bones"};
SkinCheck g_skin_four_bones{"four bones"};

struct SkinPending {
  std::vector<uint8_t> native;
  uint32_t dest = 0;
  uint32_t vertices = 0;
};
thread_local SkinPending t_skin_rigid_pending;
thread_local SkinPending t_skin_one_bone_pending;
thread_local SkinPending t_skin_two_bones_pending;
thread_local SkinPending t_skin_three_bones_pending;
thread_local SkinPending t_skin_four_bones_pending;

void LogSkinCounts(SkinCheck& check, bool checking) {
  const int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::steady_clock::now().time_since_epoch()).count();
  int64_t last = check.last_log_ms.load(std::memory_order_relaxed);
  if (now - last < 5000 || !check.last_log_ms.compare_exchange_strong(last, now)) return;
  const uint64_t calls = check.calls.exchange(0), vertices = check.vertices.exchange(0),
                 differ = check.differ.exchange(0);
  char differ_text[64] = "";
  if (checking) std::snprintf(differ_text, sizeof(differ_text), ", %llu differ from the translated code",
                              static_cast<unsigned long long>(differ));
  REXLOG_INFO("waw: native skinning ({}, {}) over 5 s: {} calls, {} vertices{}", check.name,
              checking ? "checked" : "on", calls, vertices, differ_text);
}

void CountSkinned(SkinCheck& check, uint32_t vertices) {
  check.calls.fetch_add(1, std::memory_order_relaxed);
  check.vertices.fetch_add(vertices, std::memory_order_relaxed);
  LogSkinCounts(check, false);
}

// At the translated function's end: compare its output with the native result kept in `pending`.
void CompareSkinned(SkinCheck& check, SkinPending& pending, const uint8_t* base) {
  if (pending.dest == 0) return;
  const uint32_t dest = pending.dest;
  const uint32_t vertices = pending.vertices;
  pending.dest = 0;
  uint64_t differ = 0;
  for (uint32_t i = 0; i < vertices; ++i) {
    const uint8_t* mine = pending.native.data() + static_cast<size_t>(i) * 32;
    const uint8_t* theirs = base + dest + static_cast<size_t>(i) * 32;
    if (std::memcmp(mine, theirs, 32) == 0) continue;
    ++differ;
    bool expected = false;
    if (check.first_difference_logged.compare_exchange_strong(expected, true)) {
      uint32_t a[8], b[8];
      std::memcpy(a, mine, 32);
      std::memcpy(b, theirs, 32);
      std::string text;
      for (int k = 0; k < 8; ++k) {
        char word[24];
        std::snprintf(word, sizeof(word), " %08X/%08X", __builtin_bswap32(a[k]), __builtin_bswap32(b[k]));
        text += word;
      }
      REXLOG_INFO("waw: native skinning ({}): the first vertex that differs (native/translated, by word):{}",
                  check.name, text);
    }
  }
  check.calls.fetch_add(1, std::memory_order_relaxed);
  check.vertices.fetch_add(vertices, std::memory_order_relaxed);
  check.differ.fetch_add(differ, std::memory_order_relaxed);
  LogSkinCounts(check, true);
}

}  // namespace

// 0x824472B0's entry (with `return_on_true`): true when the native code skinned the surface, so the translated
// function returns at once; false to run the translated function (off, or checking).
bool waw_native_skin_rigid(PPCContext& ctx, uint8_t* base) {
  const int mode = REXCVAR_GET(waw_native_skin);
  if (mode <= 0 || !ReadSkinPermControl(base)) return false;
  ctx.fpscr.enableFlushMode();  // as the translated vector code runs
  const uint32_t surface = ctx.r3.u32, bones = ctx.r5.u32, dest = ctx.r6.u32;
  // The vector loads and stores ignore the low four bits; the native path assumes 16-byte-aligned vertices.
  if ((dest & 15) != 0 || (ReadGuestU32(base, surface + 24) & 15) != 0) return false;
  if (mode == 1) {
    CountSkinned(g_skin_rigid, SkinRigidSurface(base, surface, bones, base + dest));
    return true;
  }
  // Checking: count the vertices first for the buffer's size.
  uint32_t total = 0;
  const uint32_t groups = ReadGuestU32(base, surface + 60);
  uint32_t group = ReadGuestU32(base, surface + 64);
  for (uint32_t g = 0; g < groups; ++g, group += 12) {
    uint16_t count_be;
    std::memcpy(&count_be, base + group + 2, 2);
    total += __builtin_bswap16(count_be);
  }
  SkinPending& pending = t_skin_rigid_pending;
  pending.native.resize(static_cast<size_t>(total) * 32);
  pending.vertices = SkinRigidSurface(base, surface, bones, pending.native.data());
  pending.dest = dest;
  return false;
}

// 0x82447434, the translated rigid function's one exit: compare (checking only).
void waw_native_skin_rigid_end(PPCContext& ctx, uint8_t* base) {
  (void)ctx;
  CompareSkinned(g_skin_rigid, t_skin_rigid_pending, base);
}

// 0x82446A98's entry (with `return_on_true`), the one-bone vertices: as waw_native_skin_rigid.
bool waw_native_skin_one_bone(PPCContext& ctx, uint8_t* base) {
  const int mode = REXCVAR_GET(waw_native_skin);
  if (mode <= 0 || !ReadSkinPermControl(base)) return false;
  ctx.fpscr.enableFlushMode();
  const uint32_t source = ctx.r3.u32, offsets = ctx.r4.u32, count = ctx.r5.u32, bones = ctx.r6.u32,
                 dest = ctx.r7.u32;
  if ((dest & 15) != 0 || (source & 15) != 0) return false;
  if (mode == 1) {
    SkinOneBoneVertices(base, source, offsets, count, bones, base + dest);
    CountSkinned(g_skin_one_bone, count);
    return true;
  }
  SkinPending& pending = t_skin_one_bone_pending;
  pending.native.resize(static_cast<size_t>(count) * 32);
  SkinOneBoneVertices(base, source, offsets, count, bones, pending.native.data());
  pending.vertices = count;
  pending.dest = dest;
  return false;
}

// 0x82446C20, the translated one-bone function's one exit (both its loop's end and the no-vertices branch lead
// here): compare (checking only).
void waw_native_skin_one_bone_end(PPCContext& ctx, uint8_t* base) {
  (void)ctx;
  CompareSkinned(g_skin_one_bone, t_skin_one_bone_pending, base);
}

namespace {

// The entry of a weighted skinning function with `bones_per_vertex` bones a vertex (2-4), as waw_native_skin_rigid.
bool SkinWeightedEntry(PPCContext& ctx, uint8_t* base, int bones_per_vertex, SkinCheck& check,
                       SkinPending& pending) {
  const int mode = REXCVAR_GET(waw_native_skin);
  if (mode <= 0 || !ReadSkinPermControl(base)) return false;
  ctx.fpscr.enableFlushMode();
  const uint32_t source = ctx.r3.u32, records = ctx.r4.u32, count = ctx.r5.u32, bones = ctx.r6.u32,
                 dest = ctx.r7.u32;
  if ((dest & 15) != 0 || (source & 15) != 0) return false;
  if (mode == 1) {
    SkinWeightedVertices(base, source, records, count, bones, bones_per_vertex, base + dest);
    CountSkinned(check, count);
    return true;
  }
  pending.native.resize(static_cast<size_t>(count) * 32);
  SkinWeightedVertices(base, source, records, count, bones, bones_per_vertex, pending.native.data());
  pending.vertices = count;
  pending.dest = dest;
  return false;
}

}  // namespace

// The entries (with `return_on_true`) and the one exit of the two-, three- and four-bone functions (0x82446C28 to
// 0x82446E14, 0x82446E18 to 0x82447040, 0x82447048 to 0x824472AC; each no-vertices branch lands on the exit too).
bool waw_native_skin_two_bones(PPCContext& ctx, uint8_t* base) {
  return SkinWeightedEntry(ctx, base, 2, g_skin_two_bones, t_skin_two_bones_pending);
}
void waw_native_skin_two_bones_end(PPCContext& ctx, uint8_t* base) {
  (void)ctx;
  CompareSkinned(g_skin_two_bones, t_skin_two_bones_pending, base);
}
bool waw_native_skin_three_bones(PPCContext& ctx, uint8_t* base) {
  return SkinWeightedEntry(ctx, base, 3, g_skin_three_bones, t_skin_three_bones_pending);
}
void waw_native_skin_three_bones_end(PPCContext& ctx, uint8_t* base) {
  (void)ctx;
  CompareSkinned(g_skin_three_bones, t_skin_three_bones_pending, base);
}
bool waw_native_skin_four_bones(PPCContext& ctx, uint8_t* base) {
  return SkinWeightedEntry(ctx, base, 4, g_skin_four_bones, t_skin_four_bones_pending);
}
void waw_native_skin_four_bones_end(PPCContext& ctx, uint8_t* base) {
  (void)ctx;
  CompareSkinned(g_skin_four_bones, t_skin_four_bones_pending, base);
}
