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

#include "cod4_native_side.h"  // [waw-decomp-cod4] 2026-11-11 the native renderer's side check

REXCVAR_DEFINE_INT32(cod4_debug_frame_audit, 0, "cod4",
                     "Debug (the frame-rate audit): every N seconds, log drawn frames, server frames (G_RunFrame) and "
                     "the client's prediction runs (CG_PredictPlayerState_Internal) per second; 0 = off. Record only");
REXCVAR_DEFINE_INT32(cod4_debug_native_census, 0, "cod4",
                     "Debug: every N seconds, log what the backend drew a frame on average (techniques with their "
                     "surface types and passes, render commands); 0 = off. Record only");
REXCVAR_DEFINE_INT32(cod4_debug_native_capture, 0, "cod4",
                     "Debug (the native renderer's struct map): every N seconds, log one frame's indexed draws as "
                     "the Direct3D layer gets them, the first of each technique and surface type, with the backend's "
                     "state for it (material, technique, pass, streams); 0 = off. Record only");

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

// [waw-decomp] 2026-11-11, the native renderer's struct map (cod4_debug_native_capture). The draw-list loop's context
// of the run being drawn, kept by cod4_census_tess on the thread that draws it, and the run's surface type.
thread_local uint32_t t_run_context = 0;
thread_local uint32_t t_run_type = 0;

// One captured frame: armed by the frame's start every N seconds; the first draw of each (technique, surface type)
// is logged, up to kCaptureLines.
constexpr uint32_t kCaptureLines = 40;
struct Capture {
  std::mutex mutex;
  bool armed = false;
  uint32_t logged = 0;
  uint64_t frame_draws = 0;  // indexed draws since the frame's start
  uint64_t last_frame_draws = 0;
  std::vector<std::pair<uint32_t, uint32_t>> seen;  // (technique, surface type) logged this frame
  uint32_t detailed[kTessSlots] = {};  // [waw-decomp-cod4] lit draws logged in full this frame, by surface type
  std::chrono::steady_clock::time_point since = std::chrono::steady_clock::now();
};

Capture& TheCapture() {
  static Capture capture;
  return capture;
}

// Whether a guest address can be read: the program's own image and data, its heaps, or physical memory.
bool Readable(uint32_t address) {
  return (address >= 0x40000000u && address < 0x7FFF0000u) || (address >= 0x82000000u && address < 0x90000000u) ||
         (address >= 0xA0000000u && address < 0xE0000000u);
}

// `count` words from `address` as hex, for a record whose layout is being mapped.
std::string Words(const uint8_t* base, uint32_t address, uint32_t count) {
  if (!Readable(address)) return "-";
  std::string text;
  char word[12];
  for (uint32_t i = 0; i < count; ++i) {
    std::snprintf(word, sizeof word, "%s%08X", i ? " " : "", Load32(base, address + 4 * i));
    text += word;
  }
  return text;
}

// The name a record starts with (a material, a technique, a shader), or "?".
std::string NameOf(const uint8_t* base, uint32_t record) {
  return Readable(record) && Readable(Load32(base, record)) ? GuestName(base, Load32(base, record)) : "?";
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
  t_run_context = ctx.r31.u32;  // [waw-decomp] for cod4_capture_draw: the run a Direct3D draw belongs to
  t_run_type = (ctx.r28.u32 >> 2) & (kTessSlots - 1);
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

// [waw-decomp-cod4] 2026-11-11: the start of the back end's frame (campaign 0x82128488, config/default.toml; it runs
// 0x82330CD0 and the scene's views 0x82128268, whose render command loop is 0x82123B90): the native renderer's side
// check ends the frame before (cod4_native_side.h).
void cod4_native_frame(PPCContext& ctx, uint8_t* base) {
  (void)ctx;
  cod4::native::SideFrame(base);
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

// [waw-decomp-cod4] 2026-11-11, for cod4_debug_native_capture: `count` floats from `address`, for constants.
namespace {

std::string Floats(const uint8_t* base, uint32_t address, uint32_t count) {
  if (!Readable(address)) return "-";
  std::string text;
  char number[20];
  for (uint32_t i = 0; i < count; ++i) {
    const uint32_t bits = Load32(base, address + 4 * i);
    float value;
    std::memcpy(&value, &bits, sizeof value);
    std::snprintf(number, sizeof number, "%s%.5g", i ? (i % 4 ? " " : " | ") : "", value);
    text += number;
  }
  return text;
}

// [waw-decomp-cod4] 2026-11-11. One lit draw in full, for the native renderer's struct map (the layouts are in
// dev/agents/waw-decomp/COD4-NATIVE.md): the pass's vertex shader (pass + 4 + 4 x the vertex declaration's type:
// Xenon's MaterialPass keeps one vertex shader per declaration type), its arguments (8 bytes each: type u16, dest
// u16, then a code constant's index u16, first row u8 and row count u8, or a sampler's code index, or a name hash,
// or a pointer to a literal), the material's texture and constant tables, what each sampler has bound (state + 0x88)
// and how (state + 0x20), the code constants the frame's source state holds (source + 0x800, 16 bytes each), the
// shader constants as the device holds them (vertex c0 at device + 0x780, pixel c0 at device + 0x1780: 0x820D59C8
// writes a pixel constant to device + (register + 0x178) x 16), and the draw's first corners.
void LogDrawDetail(const uint8_t* base, uint32_t n, uint32_t device, uint32_t state, uint32_t source,
                   uint32_t first_index) {
  const uint32_t material = Load32(base, state + 0x118), pass = Load32(base, state + 0x124);
  if (!Readable(material) || !Readable(pass)) return;
  const uint32_t decl_type = Load32(base, state + 0xF8);
  const uint32_t vs = decl_type < 15 ? Load32(base, pass + 4 + 4 * decl_type) : 0;
  const uint32_t arg_count = uint32_t(base[pass + 0x48]) + base[pass + 0x49] + base[pass + 0x4A];
  const uint32_t args = Load32(base, pass + 0x50);
  REXLOG_INFO("cod4: capture {} detail: vs \"{}\" (declaration type {}); declaration (pass + 0) {:08X}: {}; {} "
              "arguments at {:08X}: {}", n, NameOf(base, vs), decl_type, Load32(base, pass),
              Words(base, Load32(base, pass), 12), arg_count, args, Words(base, args, 2 * std::min(arg_count, 40u)));
  const uint32_t texture_count = base[material + 0x32], constant_count = base[material + 0x33];
  const uint32_t textures = Load32(base, material + 0x3C), constants = Load32(base, material + 0x40);
  std::string table;
  for (uint32_t i = 0; i < texture_count && i < 8 && Readable(textures); ++i) {
    const uint32_t image = Load32(base, textures + 12 * i + 8);
    table += " [" + Words(base, textures + 12 * i, 3) + " -> " + Words(base, image, 22) + "]";
  }
  REXLOG_INFO("cod4: capture {} detail: material +0x30: {}; {} textures at {:08X}:{}; {} constants at {:08X}: {}",
              n, Words(base, material + 0x30, 8), texture_count, textures, table, constant_count, constants,
              Words(base, constants, 8 * std::min(constant_count, 4u)));
  std::string bound;
  for (uint32_t i = 0; i < 16; ++i) {
    const uint32_t texture = Load32(base, state + 0x88 + 4 * i);
    if (!texture) continue;
    char head[48];
    std::snprintf(head, sizeof head, " s%u (state %08X) %08X: ", i, Load32(base, state + 0x20 + 4 * i), texture);
    bound += head + Words(base, texture, 14) + ";";
  }
  REXLOG_INFO("cod4: capture {} detail: samplers:{}", n, bound);
  REXLOG_INFO("cod4: capture {} detail: device vertex c0-c27: {}", n, Floats(base, device + 0x780, 112));
  REXLOG_INFO("cod4: capture {} detail: device pixel c0-c15: {}", n, Floats(base, device + 0x1780, 64));
  REXLOG_INFO("cod4: capture {} detail: source code constants 0-57: {}", n, Floats(base, source + 0x800, 232));
  REXLOG_INFO("cod4: capture {} detail: source code images (source + 0xBA0): {}; viewport (state + 0xA5C): {}", n,
              Words(base, source + 0xBA0, 27), Words(base, state + 0xA5C, 6));
  // the first triangle's corners: the index buffer's data (+0x18 its address), stream 0's (its vertex buffer's
  // fetch constant at +0x18: the address in the first word) plus the stream's offset
  const uint32_t index_buffer = Load32(base, state + 0xF4), vertex_buffer = Load32(base, state + 0x100);
  if (!Readable(index_buffer) || !Readable(vertex_buffer)) return;
  const uint32_t indices = Load32(base, index_buffer + 0x18) + 2 * first_index;
  const uint32_t stride = Load32(base, state + 0xFC);
  const uint32_t vertices = (Load32(base, vertex_buffer + 0x18) & ~3u) + Load32(base, state + 0x104);
  if (!Readable(indices) || !Readable(vertices) || stride < 12 || stride > 64) return;
  std::string corners;
  for (uint32_t i = 0; i < 3; ++i) {
    const uint32_t index = Load16(base, indices + 2 * i);
    corners += " [" + std::to_string(index) + ": " + Floats(base, vertices + index * stride, 3) + " / " +
               Words(base, vertices + index * stride + 12, (stride - 12) / 4) + "]";
  }
  REXLOG_INFO("cod4: capture {} detail: indices at {:08X}, vertices at {:08X} stride {}:{}", n, indices, vertices,
              stride, corners);
}

}  // namespace

// [waw-decomp] 2026-11-11. The Direct3D layer's indexed draw, at its start (campaign 0x8209BD80; read in the
// decompile: the world's draw 0x8210C468 calls it as (device, 4, 0, first index, index count), after setting the
// vertex stream with the world's 44-byte corners). r3 the device, r4 the primitive type (4: a triangle list), r5 to
// r7 as that call has them. The backend's state for the draw is the run's command buffer state (context + 4), read
// by KisakCOD's layout moved to where the 360 program keeps it (seen in the decompile of the world's tess function
// 0x821043B0 and of 0x820D3D30): +0xF0 the device, +0xF4 the index buffer, +0xF8 the vertex declaration's type,
// +0xFC two streams of {stride, vertex buffer, offset}, +0x114 the vertex declaration, +0x118 the material, +0x11C
// the technique type, +0x120 the technique, +0x124 the pass (+0x40 its vertex shader, +0x44 its pixel shader, +0x48
// to +0x4A its argument counts, +0x50 its arguments), +0x12C the depth range's kind. Logged to check exactly that.
void cod4_capture_draw(PPCContext& ctx, uint8_t* base) {
  // [waw-decomp-cod4] 2026-11-11 cod4_debug_native_side (off by default): the draw kept for the native picture
  cod4::native::SideDraw(base, t_run_context, t_run_type, ctx.r3.u32, uint32_t(ctx.lr), ctx.r5.u32, ctx.r6.u32,
                         ctx.r7.u32);
  if (REXCVAR_GET(cod4_debug_native_capture) <= 0) return;
  Capture& c = TheCapture();
  std::lock_guard<std::mutex> lock(c.mutex);
  ++c.frame_draws;
  if (!c.armed || c.logged >= kCaptureLines) return;
  const uint32_t context = t_run_context;
  const uint32_t state = Readable(context) ? Load32(base, context + 4) : 0;
  const uint32_t source = Readable(context) ? Load32(base, context) : 0;
  const uint32_t technique = Readable(state) ? Load32(base, state + 0x120) : 0;
  const std::pair<uint32_t, uint32_t> key{technique, t_run_type};
  if (std::find(c.seen.begin(), c.seen.end(), key) != c.seen.end()) return;
  c.seen.push_back(key);
  ++c.logged;
  if (!Readable(state)) {
    REXLOG_INFO("cod4: capture {}: draw type {} base {} first {} count {} from {:08X}: no run's state (context "
                "{:08X})", c.logged, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, uint32_t(ctx.lr), context);
    return;
  }
  const uint32_t material = Load32(base, state + 0x118), pass = Load32(base, state + 0x124);
  const bool pass_ok = Readable(pass);
  REXLOG_INFO("cod4: capture {}: surface type {}, draw type {} base {} first {} count {} from {:08X}; state {:08X} "
              "source {:08X} device {:08X}; material \"{}\", technique type {} \"{}\", pass {:08X}: vs \"{}\" ps "
              "\"{}\" args {} {} {}; prim +0xF0: {}; depth range +0x12C: {}",
              c.logged, t_run_type, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, uint32_t(ctx.lr), state, source,
              ctx.r3.u32, NameOf(base, material), Load32(base, state + 0x11C), NameOf(base, technique), pass,
              pass_ok ? NameOf(base, Load32(base, pass + 0x40)) : "?",
              pass_ok ? NameOf(base, Load32(base, pass + 0x44)) : "?", pass_ok ? uint32_t(base[pass + 0x48]) : 0u,
              pass_ok ? uint32_t(base[pass + 0x49]) : 0u, pass_ok ? uint32_t(base[pass + 0x4A]) : 0u,
              Words(base, state + 0xF0, 10), Words(base, state + 0x12C, 3));
  if (c.logged <= 4) {  // the first ones whole: the state's start, the material, the buffers, for the map
    REXLOG_INFO("cod4: capture {}: state +0x00: {}", c.logged, Words(base, state, 60));
    REXLOG_INFO("cod4: capture {}: material +0x00: {}; technique +0x00: {}; pass +0x00: {}", c.logged,
                Words(base, material, 20), Words(base, technique, 4), Words(base, pass, 22));
    REXLOG_INFO("cod4: capture {}: index buffer: {}; vertex buffer 0: {}; vertex buffer 1: {}; declaration: {}",
                c.logged, Words(base, Load32(base, state + 0xF4), 8), Words(base, Load32(base, state + 0x100), 8),
                Words(base, Load32(base, state + 0x10C), 8), Words(base, Load32(base, state + 0x114), 8));
  }
  // [waw-decomp-cod4] 2026-11-11: the lit draws (technique types 7 and up) in full, the first two of each surface
  // type in a captured frame (LogDrawDetail)
  if (Load32(base, state + 0x11C) >= 7 && t_run_type < kTessSlots && c.detailed[t_run_type] < 2) {
    ++c.detailed[t_run_type];
    LogDrawDetail(base, c.logged, ctx.r3.u32, state, source, ctx.r6.u32);
  }
}

namespace {

// The capture's frame: counts the last frame's draws and arms the next frame every N seconds.
void CaptureFrame() {
  const int32_t seconds = REXCVAR_GET(cod4_debug_native_capture);
  if (seconds <= 0) return;
  Capture& c = TheCapture();
  std::lock_guard<std::mutex> lock(c.mutex);
  if (c.armed)
    REXLOG_INFO("cod4: capture: the frame had {} indexed draws; {} lines logged", c.frame_draws, c.logged);
  c.last_frame_draws = c.frame_draws;
  c.frame_draws = 0;
  c.armed = false;
  if (std::chrono::steady_clock::now() - c.since >= std::chrono::seconds(seconds)) {
    c.since = std::chrono::steady_clock::now();
    c.armed = true;
    c.logged = 0;
    c.seen.clear();
    std::fill(c.detailed, c.detailed + kTessSlots, 0u);  // [waw-decomp-cod4]
  }
}

}  // namespace

// Called once a frame from cod4_frame_start (cod4_hooks.cpp): counts frames and logs the averages every N seconds.
void Cod4CensusFrame(uint8_t* base) {
  CaptureFrame();  // [waw-decomp] cod4_debug_native_capture
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
