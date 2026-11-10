// cod4 - Call of Duty 4: Modern Warfare (Xbox 360) static recompilation: the native renderer's census. [acct2-cod4]
//
// Step 1 of dev/agents/acct2-cod4/NATIVE-RENDERER.md: what the game's own backend draws each frame, read from its
// draw lists, so the shared IW3 native renderer (WaW's and CoD4's) knows which techniques, surface types, passes and
// render commands a frame has. Record only: nothing the game does changes. Off unless cod4_debug_native_census is set.
//
// Where it reads (campaign, found 2026-11-07 in the generated code; KisakCOD's names, information only):
//   0x82123950  the draw-surface list loop (IW3's R_DrawSurfs). Its argument r3 is the command buffer context:
//               +0 the source state, +4 the command buffer state, whose +288 is the current technique
//               (MaterialTechnique: +0 name, +6 pass count, u16). For each run of surfaces with one material it
//               sets the material (0x821037D8), then for each pass sets the pass (0x82103530) and calls the tess
//               function of the run's surface type through the table at 0x8202FDA0; the type is
//               (draw surface >> 50) & 0xF of the 64-bit draw-surface key.
//   0x82123A88  that call (`bctrl`): r28 = type * 4, r30 = the pass, r31 = the context.
//   0x82123B90  the backend's command loop (IW3's RB_ExecuteRenderCommandsLoop): u16 command ids through the table
//               at 0x8202FDC0; 0x82123BD0 is that call (`bctrl`), CTR the command's function.
// Multiplayer has the same code at other addresses, read the same way (checked instruction by instruction):
// R_DrawSurfs 0x82154F90 with its call at 0x821550C8 and the tess table at 0x82035934; the command loop 0x821551D0
// with its call at 0x82155210 and the table at 0x82035980 (mp/config/default.toml hooks them).
//
// The same file counts the frame-rate audit's first numbers (cod4_debug_frame_audit): drawn frames, server frames
// (G_RunFrame 0x82252FE0; multiplayer's isn't named yet, so not counted there) and the client's prediction runs
// (CG_PredictPlayerState_Internal 0x82329358, multiplayer 0x822F8FA8) per second. In multiplayer it also lists each
// client's movement type (from ClientEndFrame 0x82286510, the host's side), so a system-link test can tell that the
// players spawned.

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include <rex/cvar.h>
#include <rex/logging.h>

#ifdef COD4_MP
#include "generated/default_mp/cod4mp_pch.h"
#else
#include "generated/default/cod4_pch.h"
#endif

REXCVAR_DEFINE_INT32(cod4_debug_frame_audit, 0, "cod4",
                     "Debug (the frame-rate audit): every N seconds, log drawn frames, server frames (G_RunFrame) and "
                     "the client's prediction runs (CG_PredictPlayerState_Internal) per second; 0 = off. Record only");
REXCVAR_DEFINE_INT32(cod4_debug_native_census, 0, "cod4",
                     "Debug: every N seconds, log what the backend drew a frame on average (techniques with their "
                     "surface types and passes, render commands); 0 = off. Record only");

namespace {

#ifdef COD4_MP
constexpr uint32_t kTessTable = 0x82035934;     // the tess functions, one per surface type
constexpr uint32_t kCommandTable = 0x82035980;  // the backend's render commands, by id
#else
constexpr uint32_t kTessTable = 0x8202FDA0;
constexpr uint32_t kCommandTable = 0x8202FDC0;
#endif
constexpr uint32_t kTessSlots = 16;
constexpr uint32_t kCommandSlots = 32;

// The 14 draw-list passes: the functions that call R_DrawSurfs's wrapper (campaign 0x82123AB0, multiplayer
// 0x821550F0), found 2026-11-07 by results/acct2-cod4-scripts/callers.py. Each one's start is hooked
// (cod4_census_pass_N), so a tess call or a flush is counted under the pass that issued it. The backend's command loop
// sets "no pass" before each command, so what a command draws outside these passes is counted apart.
#ifdef COD4_MP
constexpr uint32_t kPassFunctions[] = {0x82356C30, 0x823605D0, 0x82360678, 0x82360748, 0x823607E8,
                                       0x82360838, 0x823608D8, 0x82360978, 0x82360A18, 0x82363C20,
                                       0x82363E00, 0x82363FA8, 0x823641D0, 0x82364290};
#else
constexpr uint32_t kPassFunctions[] = {0x823256C8, 0x8232F420, 0x8232F4C8, 0x8232F598, 0x8232F638,
                                       0x8232F688, 0x8232F728, 0x8232F7C8, 0x8232F868, 0x823323B8,
                                       0x82332598, 0x82332740, 0x82332968, 0x82332A28};
#endif
constexpr uint32_t kPasses = sizeof(kPassFunctions) / sizeof(kPassFunctions[0]);
constexpr uint32_t kNoPass = kPasses;  // the last slot: outside the 14 passes

uint32_t Load32(const uint8_t* base, uint32_t address) {
  uint32_t value;
  std::memcpy(&value, base + address, sizeof(value));
  return __builtin_bswap32(value);
}

uint16_t Load16(const uint8_t* base, uint32_t address) {
  uint16_t value;
  std::memcpy(&value, base + address, sizeof(value));
  return __builtin_bswap16(value);
}

std::string GuestName(const uint8_t* base, uint32_t address) {
  if (address < 0x10000) return "?";
  const char* p = reinterpret_cast<const char*>(base + address);
  const size_t n = strnlen(p, 64);
  for (size_t i = 0; i < n; ++i) {
    if (p[i] < 0x20 || p[i] > 0x7E) return "?";
  }
  return std::string(p, n);
}

struct Census {
  std::mutex mutex;
  // (technique name, surface type) -> {tess calls, the largest pass index + 1 seen}
  std::map<std::pair<std::string, uint32_t>, std::pair<uint64_t, uint32_t>> draws;
  std::map<uint32_t, std::string> technique_names;  // technique address -> name (read once each)
  uint64_t command_calls[kCommandSlots] = {};
  uint64_t other_commands = 0;
  uint64_t tess_calls = 0;
  uint64_t pass_runs[kPasses + 1] = {};     // tess calls by the pass that issued them
  uint64_t flushes = 0;                      // the 2D and code-mesh path's draws (cod4_census_flush)
  uint64_t pass_flushes[kPasses + 1] = {};  // the same, by pass
  uint32_t current_pass = kNoPass;
  uint64_t frames = 0;
  std::chrono::steady_clock::time_point since = std::chrono::steady_clock::now();
};

Census& TheCensus() {
  static Census census;
  return census;
}

void Report(Census& c, const uint8_t* base) {
  if (c.frames == 0) return;
  const double frames = static_cast<double>(c.frames);
  REXLOG_INFO("cod4: census over {} frames: {:.0f} tess calls a frame (one per run of surfaces, material and pass)",
              c.frames, c.tess_calls / frames);
  std::vector<std::pair<uint64_t, std::string>> rows;
  for (const auto& [key, value] : c.draws) {
    rows.emplace_back(value.first, key.first + " type " + std::to_string(key.second) + " passes " +
                                       std::to_string(value.second));
  }
  std::sort(rows.rbegin(), rows.rend());
  for (size_t i = 0; i < rows.size() && i < 30; ++i) {
    REXLOG_INFO("cod4: census   {:8.1f} a frame  {}", rows[i].first / frames, rows[i].second);
  }
  REXLOG_INFO("cod4: census   {} technique/type pairs in all", rows.size());
  std::string types;
  uint32_t seen = 0;
  for (const auto& [key, value] : c.draws) seen |= 1u << key.second;
  for (uint32_t type = 0; type < kTessSlots; ++type) {
    if (!(seen & (1u << type))) continue;
    char entry[32];
    std::snprintf(entry, sizeof(entry), " %u=%08X", type, Load32(base, kTessTable + 4 * type));
    types += entry;
  }
  REXLOG_INFO("cod4: census   surface types seen, type=tess function:{}", types);
  std::string commands;
  for (uint32_t id = 0; id < kCommandSlots; ++id) {
    if (!c.command_calls[id]) continue;
    char line[64];
    std::snprintf(line, sizeof(line), " %u(%08X):%.1f", id, Load32(base, kCommandTable + 4 * id),
                  c.command_calls[id] / frames);
    commands += line;
  }
  REXLOG_INFO("cod4: census   render commands a frame, id(function):count:{}{}", commands,
              c.other_commands ? " (+ calls outside the table)" : "");
  std::string passes;
  for (uint32_t i = 0; i <= kPasses; ++i) {
    if (!c.pass_runs[i] && !c.pass_flushes[i]) continue;
    char entry[64];
    if (i == kNoPass) {
      std::snprintf(entry, sizeof(entry), " none:%.1f+%.1f", c.pass_runs[i] / frames, c.pass_flushes[i] / frames);
    } else {
      std::snprintf(entry, sizeof(entry), " %u(%08X):%.1f+%.1f", i, kPassFunctions[i], c.pass_runs[i] / frames,
                    c.pass_flushes[i] / frames);
    }
    passes += entry;
  }
  REXLOG_INFO("cod4: census   2D/code-mesh flushes a frame: {:.1f}", c.flushes / frames);
  REXLOG_INFO("cod4: census   by pass, pass(function):tess calls+flushes a frame:{}", passes);
  c.draws.clear();
  std::memset(c.pass_runs, 0, sizeof(c.pass_runs));
  std::memset(c.pass_flushes, 0, sizeof(c.pass_flushes));
  c.flushes = 0;
  std::memset(c.command_calls, 0, sizeof(c.command_calls));
  c.other_commands = 0;
  c.tess_calls = 0;
  c.frames = 0;
  c.since = std::chrono::steady_clock::now();
}

// The frame-rate audit's counters (cod4_debug_frame_audit).
constexpr uint32_t kAuditClients = 18;  // multiplayer's client slots (sv_maxclients up to 18 on the 360)

struct Audit {
  std::mutex mutex;
  uint64_t frames = 0;
  uint64_t server_frames = 0;
  uint64_t predictions = 0;
  int32_t pm_type[kAuditClients];  // each client's last movement type this period, -1 = not seen
  Audit() { std::fill(pm_type, pm_type + kAuditClients, -1); }
  std::chrono::steady_clock::time_point since = std::chrono::steady_clock::now();
};

Audit& TheAudit() {
  static Audit audit;
  return audit;
}

}  // namespace

// G_RunFrame's start (0x82252FE0): one server frame (sv_fps 20 on the console: every 50 ms of game time).
void cod4_audit_server_frame(PPCContext& ctx, uint8_t* base) {
  (void)ctx;
  (void)base;
  if (REXCVAR_GET(cod4_debug_frame_audit) <= 0) return;
  Audit& a = TheAudit();
  std::lock_guard<std::mutex> lock(a.mutex);
  ++a.server_frames;
}

// CG_PredictPlayerState_Internal's start (0x82329358): the client's player movement prediction.
void cod4_audit_prediction(PPCContext& ctx, uint8_t* base) {
  (void)ctx;
  (void)base;
  if (REXCVAR_GET(cod4_debug_frame_audit) <= 0) return;
  Audit& a = TheAudit();
  std::lock_guard<std::mutex> lock(a.mutex);
  ++a.predictions;
}

#ifdef COD4_MP
// ClientEndFrame's start (multiplayer 0x82286510), on the host once a server frame for each client: r3 is the
// client's gentity_s. IW3's layout: the entity's number at +0, its gclient_s pointer at +0x15C; gclient_s starts with
// the playerState_t, whose pm_type is at +4 (0 playing, 4 spectator, 7 dead). Both pointers must lie in the program's
// own data (where g_entities and the clients are) before they are read.
void cod4_audit_client_end_frame(PPCContext& ctx, uint8_t* base) {
  if (REXCVAR_GET(cod4_debug_frame_audit) <= 0) return;
  constexpr uint32_t kDataLo = 0x82000000, kDataHi = 0x90000000;
  const uint32_t entity = ctx.r3.u32;
  if (entity < kDataLo || entity >= kDataHi - 0x200) return;
  const uint32_t number = Load32(base, entity);
  const uint32_t client = Load32(base, entity + 0x15C);
  if (number >= kAuditClients || client < kDataLo || client >= kDataHi - 8) return;
  const int32_t pm_type = int32_t(Load32(base, client + 4));
  Audit& a = TheAudit();
  std::lock_guard<std::mutex> lock(a.mutex);
  a.pm_type[number] = pm_type;
}
#endif

// The draw-surface list loop's call of a tess function (0x82123A88, before the bctrl).
void cod4_census_tess(PPCContext& ctx, uint8_t* base) {
  if (REXCVAR_GET(cod4_debug_native_census) <= 0) return;
  const uint32_t type = (ctx.r28.u32 >> 2) & (kTessSlots - 1);
  const uint32_t pass = ctx.r30.u32;
  const uint32_t state = Load32(base, ctx.r31.u32 + 4);
  const uint32_t technique = state ? Load32(base, state + 288) : 0;
  Census& c = TheCensus();
  std::lock_guard<std::mutex> lock(c.mutex);
  ++c.tess_calls;
  ++c.pass_runs[c.current_pass];
  auto name = c.technique_names.find(technique);
  if (name == c.technique_names.end()) {
    name = c.technique_names.emplace(technique, technique ? GuestName(base, Load32(base, technique)) : "?").first;
  }
  auto& entry = c.draws[{name->second, type}];
  ++entry.first;
  const uint32_t passes = technique ? Load16(base, technique + 6) : 0;
  entry.second = std::max(entry.second, std::max(pass + 1, passes));
}

// The backend's command loop's call (0x82123BD0, before the bctrl): CTR is the command's function.
void cod4_census_command(PPCContext& ctx, uint8_t* base) {
  if (REXCVAR_GET(cod4_debug_native_census) <= 0) return;
  const uint32_t target = ctx.ctr.u32;
  Census& c = TheCensus();
  std::lock_guard<std::mutex> lock(c.mutex);
  c.current_pass = kNoPass;
  for (uint32_t id = 0; id < kCommandSlots; ++id) {
    if (Load32(base, kCommandTable + 4 * id) == target) {
      ++c.command_calls[id];
      return;
    }
  }
  ++c.other_commands;
}

namespace {

void EnterPass(uint32_t pass) {
  if (REXCVAR_GET(cod4_debug_native_census) <= 0) return;
  Census& c = TheCensus();
  std::lock_guard<std::mutex> lock(c.mutex);
  c.current_pass = pass;
}

}  // namespace

// The start of draw-list pass N (kPassFunctions[N]).
#define COD4_CENSUS_PASS(n)                                    \
  void cod4_census_pass_##n(PPCContext& ctx, uint8_t* base) { \
    (void)ctx;                                                 \
    (void)base;                                                \
    EnterPass(n);                                              \
  }
COD4_CENSUS_PASS(0)
COD4_CENSUS_PASS(1)
COD4_CENSUS_PASS(2)
COD4_CENSUS_PASS(3)
COD4_CENSUS_PASS(4)
COD4_CENSUS_PASS(5)
COD4_CENSUS_PASS(6)
COD4_CENSUS_PASS(7)
COD4_CENSUS_PASS(8)
COD4_CENSUS_PASS(9)
COD4_CENSUS_PASS(10)
COD4_CENSUS_PASS(11)
COD4_CENSUS_PASS(12)
COD4_CENSUS_PASS(13)
#undef COD4_CENSUS_PASS
static_assert(kPasses == 14, "one cod4_census_pass_N hook per pass function");

// The start of the 2D and code-mesh path's draw (campaign 0x8211F460, called only from 0x8211F560, which 33 functions
// call, the command loop among them; multiplayer 0x82150AA0). It sets a pass (0x82103530 / 0x821344D0) and draws,
// like R_DrawSurfs does for the draw lists. Counted as one flush.
void cod4_census_flush(PPCContext& ctx, uint8_t* base) {
  (void)ctx;
  (void)base;
  if (REXCVAR_GET(cod4_debug_native_census) <= 0) return;
  Census& c = TheCensus();
  std::lock_guard<std::mutex> lock(c.mutex);
  ++c.flushes;
  ++c.pass_flushes[c.current_pass];
}

// Called once a frame from cod4_frame_start (cod4_hooks.cpp): counts frames and logs the averages every N seconds.
void Cod4CensusFrame(uint8_t* base) {
  if (const int32_t audit_seconds = REXCVAR_GET(cod4_debug_frame_audit); audit_seconds > 0) {
    Audit& a = TheAudit();
    std::lock_guard<std::mutex> lock(a.mutex);
    ++a.frames;
    const auto now = std::chrono::steady_clock::now();
    const double elapsed = std::chrono::duration<double>(now - a.since).count();
    if (elapsed >= audit_seconds) {
      std::string clients;
      for (uint32_t i = 0; i < kAuditClients; ++i) {
        if (a.pm_type[i] < 0) continue;
        clients += (clients.empty() ? "; clients (number:pm_type)" : "") + std::string(" ") + std::to_string(i) +
                   ":" + std::to_string(a.pm_type[i]);
        a.pm_type[i] = -1;
      }
      REXLOG_INFO("cod4: audit over {:.1f} s: {:.1f} drawn frames, {:.1f} server frames, {:.1f} predictions a second{}",
                  elapsed, a.frames / elapsed, a.server_frames / elapsed, a.predictions / elapsed, clients);
      a.frames = a.server_frames = a.predictions = 0;
      a.since = now;
    }
  }
  const int32_t seconds = REXCVAR_GET(cod4_debug_native_census);
  if (seconds <= 0) return;
  Census& c = TheCensus();
  std::lock_guard<std::mutex> lock(c.mutex);
  ++c.frames;
  if (std::chrono::steady_clock::now() - c.since >= std::chrono::seconds(seconds)) Report(c, base);
}
