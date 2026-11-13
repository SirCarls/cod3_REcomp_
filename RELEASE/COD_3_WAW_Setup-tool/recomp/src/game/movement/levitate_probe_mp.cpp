// cod3mp - a probe for the "levitation" glitch: two players pressed together against a wall slowly rise.
//
// [acct2-ocr] 2026-11-05, owner 19:30: "please look at the [Eder] dam levitation glitch. It's pretty well know.
// Please find what causes it"; 19:34: "there's also a glitch that involves a player proning and the other player
// standing. Both are against the wall and walk forward. It then starts to slowly levitate them."
// Dev only, off by default; nothing here changes how anyone moves unless a test sets cod3_test_levitate.
// The fix is game/movement/stuck_fix.h (levitation_fix); this probe measures it.
//
// What the game does after each move (ClientThink_real 0x82592CD0, read in the generated code):
//   - before Pmove it keeps the origin; after Pmove, when the player's stick was pushed (usercmd forwardmove or
//     rightmove not 0), the origin did not change at all and pm_type is not 2, it calls 0x8249F070 ("stuck fix",
//     radius 15.1): three times, for spheres at the feet + 15.1, half the height and the top - 15.1, it pushes the
//     position out of the players and brushes it overlaps and writes the result back into the playerState;
//   - when a move was longer than 15 units (225 squared), it traces from the old to the new spot (both 35 up,
//     0x8249EE30, radius 15) and puts the player back where it was if that is blocked ("warp check").
//
// cod3_debug_levitate: every half second, one line per local player (split screen): the height, how much the
// movement code moved it up or down and where (PmoveSingle 0x824B39B8 as a whole, PM_StepSlideMove 0x824A87E0
// inside it, the stuck fix, and anything else outside PmoveSingle, split into "between" (from the end of one
// ClientThink_real's moves to the next think) and "pre" (from there to its PmoveSingle)), the warp check's calls and
// put-backs, and
// PM_PlayerTrace 0x824A1828's calls by caller, counting those that started inside something (startsolid) or stayed
// inside (allsolid):
//   up       0x824A8A48  PM_StepSlideMove's trace up by the step height
//   down     0x824A8B44 / 0x824A8D3C  its traces back down
//   ground   0x824A6018  PM_GroundTrace's trace down 0.25
//   jitter   0x824A6138  PM_GroundTrace's retries from higher points while the start is inside something
//   post     0x824A6200  PM_GroundTrace's trace after the retries
// cod3_test_levitate "T,f,s,u": T seconds after split-screen player 2 first moves, player 2 is put at player 1's
// position plus f along the way player 1 faces, s to player 1's left and u up, once (before its move, at
// 0x82483190, the last call ClientThink_real makes before it keeps the origin, so the warp check doesn't undo it).
// cod3_test_levitate_steer: after that, whenever player 2's stick is pushed, player 2 walks at a point 16 units past
// player 1 the way player 1 faces (its usercmd turned that way for PmoveSingle), to press it into player 1 and what
// player 1 presses against.
// Everything below is cod3mp-only (inside COD3_MP, so tools/mp_aliases.py treats the hooks as this program's own).

#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

#include "game/guest.h"
#include "game/movement/stuck_fix.h"  // the stuck fix's hook (the levitation fix) and its observer

#if defined(COD3_MP)

REXCVAR_DEFINE_BOOL(cod3_debug_levitate, false, "cod3",
                    "Tests: every half second, per local player, the height, how much each part of the movement "
                    "code moved it, and the traces that started inside something (the levitation glitch)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_STRING(cod3_test_levitate, "", "cod3",
                      "Tests only: \"T,f,s,u\": T seconds after split-screen player 2 first moves, put player 2 at "
                      "player 1's position plus f forward, s left, u up (player 1's facing), once; empty: off")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(cod3_test_levitate_steer, false, "cod3",
                    "Tests only: after cod3_test_levitate placed player 2, player 2 walks into player 1 (at a point "
                    "16 units past it, the way player 1 faces) whenever its stick is pushed")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace {
namespace g = cod3::guest;
using Clock = std::chrono::steady_clock;

constexpr uint32_t kGentities = 0x82BCC7FC;  // *(it) + 4 * (local player + 1): the player's entity
constexpr uint32_t kEntClient = 0x200;       // the entity's playerState (ClientThink_real's ent + 512, as touch_pace_mp.cpp)
constexpr uint32_t kPmGlobal = 0x82B922C8;   // pmove_t* (valid inside Pmove)
constexpr uint32_t kPml = 0x82C74880;        // pml_t; + 0: the view's forward vector, set by PmoveSingle
constexpr uint32_t kPmCmd = 4;               // pmove_t + 4: the usercmd
constexpr uint32_t kCmdForward = 24;         // usercmd + 24: forwardmove (signed char); + 25: rightmove
constexpr uint32_t kPsOrigin = 0;            // vec3
constexpr uint32_t kPsVelocity = 16;         // vec3
constexpr uint32_t kPsPmFlags = 44;          // 0x1 prone, 0x2 crouched
constexpr uint32_t kTraceAllSolid = 60;
constexpr uint32_t kTraceStartSolid = 61;
constexpr int kPlayers = 4;

bool Valid(uint32_t p) { return p >= 0x10000 && (p & 3) == 0; }

enum Site { kUp, kDown, kGround, kJitter, kPost, kOther, kSites };
Site SiteOf(uint32_t lr) {
  switch (lr) {
    case 0x824A8A48: return kUp;
    case 0x824A8B44:
    case 0x824A8D3C: return kDown;
    case 0x824A6018: return kGround;
    case 0x824A6138: return kJitter;
    case 0x824A6200: return kPost;
    default: return kOther;
  }
}

struct Stats {
  uint64_t pmoves = 0;
  double pmove_dz = 0, outside_dz = 0;
  uint64_t steps = 0, step_up = 0, step_down = 0;
  double step_dz = 0, step_max = 0;
  uint64_t calls[kSites] = {}, startsolid[kSites] = {}, allsolid[kSites] = {};
  double up_inside = 0;  // the up trace's climb when it started inside something
  uint64_t fix_calls = 0, fix_moved = 0, fix_applied = 0;  // fix_applied: with the players left out (levitation_fix)
  double fix_dz = 0, fix_dxy = 0, fix_max_dz = 0;
  uint64_t warp_calls = 0, warp_back = 0;
  // ClientThink_real calls (seen at 0x82483190) and the height changes between the end of one think's moves (its
  // PmoveSingle or stuck fix) and the next think ("between"), and from there to its PmoveSingle ("pre").
  uint64_t thinks = 0;
  double between_dz = 0, pre_dz = 0;
  // The stuck fix's parts, by call site (kFixParts below): calls, calls that moved the sphere, its height change and
  // sideways change summed, and calls whose r4 is a connected player's entity.
  uint64_t part_calls[5] = {}, part_moved[5] = {}, part_player[5] = {};
  double part_dz[5] = {}, part_dxy[5] = {};
};

// The calls inside the stuck fix 0x8249F070 that move its sphere (r3 points at it), by return address.
struct FixPart {
  uint32_t lr;
  const char* name;
};
constexpr FixPart kFixParts[5] = {{0x8249F6AC, "b150a"}, {0x8249F738, "b020a"}, {0x8249F928, "b150b"},
                                  {0x8249F9AC, "b020b"}, {0x8249FD10, "acb8"}};
int g_fix_k = -1;  // the local player whose stuck fix is running

struct Player {
  uint32_t client = 0;
  bool seen = false;
  bool have_z = false;
  bool placed = false;
  bool have_fwd = false;
  Clock::time_point first{};
  float z = 0;
  bool have_end = false, have_think = false;
  float z_end = 0, z_think = 0;  // the height after the last move of a think; at the start of this think
  float fwd[2] = {1, 0};  // the flat forward of the last PmoveSingle (pml + 0)
  Stats s;
};
std::array<Player, kPlayers> g_players;
int g_cur = -1;  // the local player whose PmoveSingle is running
Clock::time_point g_last_log{};

uint32_t ClientOf(uint8_t* base, int k) {
  const uint32_t table = g::ReadU32(base, kGentities);
  if (!Valid(table)) return 0;
  const uint32_t ent = g::ReadU32(base, table + 4 * static_cast<uint32_t>(k + 1));
  if (!Valid(ent)) return 0;
  const uint32_t client = g::ReadU32(base, ent + kEntClient);
  return Valid(client) ? client : 0;
}

int LocalIndex(uint8_t* base, uint32_t ps) {
  for (int k = 0; k < kPlayers; ++k) {
    const uint32_t c = ClientOf(base, k);
    g_players[k].client = c;
    if (c != 0 && c == ps) return k;
  }
  return -1;
}

// Before player 2's move (see the header): put it next to player 1, in player 1's frame.
void MaybePlace(uint8_t* base, uint32_t ps, const std::string& spec, Player& p) {
  double t = 0, f = 0, s = 0, u = 0;
  if (std::sscanf(spec.c_str(), "%lf,%lf,%lf,%lf", &t, &f, &s, &u) < 1) return;
  if (!p.seen || std::chrono::duration<double>(Clock::now() - p.first).count() < t) return;
  const uint32_t other = ClientOf(base, 0);
  const Player& p1 = g_players[0];
  if (other == 0 || other == ps || !p1.have_fwd) return;
  p.placed = true;
  const float fx = p1.fwd[0], fy = p1.fwd[1];
  const float x = g::ReadF32(base, other + kPsOrigin) + static_cast<float>(f) * fx - static_cast<float>(s) * fy;
  const float y = g::ReadF32(base, other + kPsOrigin + 4) + static_cast<float>(f) * fy + static_cast<float>(s) * fx;
  const float z = g::ReadF32(base, other + kPsOrigin + 8) + static_cast<float>(u);
  g::WriteF32(base, ps + kPsOrigin, x);
  g::WriteF32(base, ps + kPsOrigin + 4, y);
  g::WriteF32(base, ps + kPsOrigin + 8, z);
  for (uint32_t i = 0; i < 3; ++i) g::WriteF32(base, ps + kPsVelocity + 4 * i, 0.0f);
  p.have_z = false;  // not a move of the game's
  REXLOG_INFO("cod3mp: levitate test: player 2 put at player 1 + ({:.1f} forward, {:.1f} left, {:.1f} up), player 1 "
              "facing ({:.2f}, {:.2f}): ({:.1f}, {:.1f}, {:.1f})",
              f, s, u, fx, fy, x, y, z);
}

// Player 2's usercmd for this PmoveSingle turned to walk at a point 16 units past player 1 the way player 1 faces
// (into what player 1 presses against; full speed), seen from player 2's own facing (its last PmoveSingle's forward;
// a view that turns slowly). Aiming at player 1 itself keeps the two pressed together when they slide.
void Steer(uint8_t* base, uint32_t pm, uint32_t ps, const Player& p2) {
  const Player& p1 = g_players[0];
  const uint32_t other = ClientOf(base, 0);
  if (!p1.have_fwd || !p2.have_fwd || other == 0) return;
  const uint32_t cmd = pm + kPmCmd;
  const int8_t fm = static_cast<int8_t>(g::ReadU8(base, cmd + kCmdForward));
  const int8_t rm = static_cast<int8_t>(g::ReadU8(base, cmd + kCmdForward + 1));
  if (fm == 0 && rm == 0) return;
  float dx = g::ReadF32(base, other + kPsOrigin) + 16.0f * p1.fwd[0] - g::ReadF32(base, ps + kPsOrigin);
  float dy = g::ReadF32(base, other + kPsOrigin + 4) + 16.0f * p1.fwd[1] - g::ReadF32(base, ps + kPsOrigin + 4);
  const float len = std::hypot(dx, dy);
  if (len < 0.01f) return;
  dx /= len;
  dy /= len;
  const float fx = p2.fwd[0], fy = p2.fwd[1];  // player 2's forward; its right is (fy, -fx)
  const float f = dx * fx + dy * fy, r = dx * fy - dy * fx;
  g::WriteU8(base, cmd + kCmdForward, static_cast<uint8_t>(static_cast<int8_t>(std::lround(127.0f * f))));
  g::WriteU8(base, cmd + kCmdForward + 1, static_cast<uint8_t>(static_cast<int8_t>(std::lround(127.0f * r))));
}

void MaybeLog(uint8_t* base) {
  const auto now = Clock::now();
  if (now - g_last_log < std::chrono::milliseconds(500)) return;
  g_last_log = now;
  float pos[kPlayers][3] = {};
  for (int k = 0; k < kPlayers; ++k) {
    const uint32_t c = g_players[k].client;
    if (c == 0) continue;
    for (uint32_t i = 0; i < 3; ++i) pos[k][i] = g::ReadF32(base, c + kPsOrigin + 4 * i);
  }
  for (int k = 0; k < kPlayers; ++k) {
    Player& p = g_players[k];
    if (!p.seen || p.client == 0) continue;
    const Stats& s = p.s;
    const uint32_t flags = g::ReadU32(base, p.client + kPsPmFlags);
    const int o = k == 0 ? 1 : 0;  // the other player the distance is to
    const float dxy = std::hypot(pos[k][0] - pos[o][0], pos[k][1] - pos[o][1]);
    const float dzo = pos[k][2] - pos[o][2];
    const float speed =
        std::hypot(g::ReadF32(base, p.client + kPsVelocity), g::ReadF32(base, p.client + kPsVelocity + 4));
    REXLOG_INFO("cod3mp: levitate p{} z {:.2f} {} speed {:.0f} (to p{}: {:.1f} across, {:+.1f} up) | {} pmoves {:+.2f}, "
                "outside {:+.2f} | stuck fix {} calls ({} players out), {} moved, {:+.3f} up (max {:+.3f}), {:.2f} "
                "across | warp check "
                "{} calls, {} put back | thinks {} between {:+.3f} pre {:+.3f} | step {} calls {:+.2f} ({} up, {} down, "
                "max {:.2f}) | traces "
                "(calls/startsolid/allsolid): up {}/{}/{} climb-in-solid {:+.2f}; down {}/{}/{}; ground {}/{}/{}; "
                "jitter {}/{}/{}; post {}/{}/{}; other {}/{}/{}",
                k + 1, pos[k][2], (flags & 1) ? "prone" : (flags & 2) ? "crouched" : "standing", speed, o + 1, dxy,
                dzo, s.pmoves, s.pmove_dz, s.outside_dz, s.fix_calls, s.fix_applied, s.fix_moved, s.fix_dz, s.fix_max_dz, s.fix_dxy,
                s.warp_calls, s.warp_back, s.thinks, s.between_dz, s.pre_dz, s.steps, s.step_dz, s.step_up, s.step_down, s.step_max, s.calls[kUp],
                s.startsolid[kUp], s.allsolid[kUp], s.up_inside, s.calls[kDown], s.startsolid[kDown],
                s.allsolid[kDown], s.calls[kGround], s.startsolid[kGround], s.allsolid[kGround], s.calls[kJitter],
                s.startsolid[kJitter], s.allsolid[kJitter], s.calls[kPost], s.startsolid[kPost], s.allsolid[kPost],
                s.calls[kOther], s.startsolid[kOther], s.allsolid[kOther]);
    if (s.fix_calls != 0) {
      std::string parts;
      for (int i = 0; i < 5; ++i) {
        char buf[96];
        std::snprintf(buf, sizeof(buf), "%s%s %llu/%llu %+.3f up %.3f across (player r4 %llu)", i ? "; " : "",
                      kFixParts[i].name, static_cast<unsigned long long>(s.part_calls[i]),
                      static_cast<unsigned long long>(s.part_moved[i]), s.part_dz[i], s.part_dxy[i],
                      static_cast<unsigned long long>(s.part_player[i]));
        parts += buf;
      }
      REXLOG_INFO("cod3mp: levitate parts p{}: {}", k + 1, parts);
    }
    p.s = Stats{};
  }
}

uint32_t CurrentPs(uint8_t* base) {
  const uint32_t pm = g::ReadU32(base, kPmGlobal);
  if (!Valid(pm)) return 0;
  const uint32_t ps = g::ReadU32(base, pm);
  return Valid(ps) ? ps : 0;
}

bool Active() { return REXCVAR_GET(cod3_debug_levitate) || !REXCVAR_GET(cod3_test_levitate).empty(); }

// The local player whose pmove_t this is (r3 of PmoveSingle, the stuck fix and the warp check), or -1.
int PlayerOfPm(uint8_t* base, uint32_t pm) {
  if (!Active() || !Valid(pm)) return -1;
  const uint32_t ps = g::ReadU32(base, pm);
  return Valid(ps) ? LocalIndex(base, ps) : -1;
}
}  // namespace

REX_EXTERN(__imp__sub_824B39B8);
REX_HOOK_RAW(sub_824B39B8) {  // PmoveSingle(pm)
  const bool debug = REXCVAR_GET(cod3_debug_levitate);
  const uint32_t pm = ctx.r3.u32;
  const int k = PlayerOfPm(base, pm);
  if (k < 0) {
    __imp__sub_824B39B8(ctx, base);
    return;
  }
  const uint32_t ps = g::ReadU32(base, pm);
  Player& p = g_players[k];
  if (!p.seen) {
    p.seen = true;
    p.first = Clock::now();
  }
  if (k == 1 && p.placed && REXCVAR_GET(cod3_test_levitate_steer)) Steer(base, pm, ps, p);
  const float z0 = g::ReadF32(base, ps + kPsOrigin + 8);
  if (p.have_z) p.s.outside_dz += z0 - p.z;
  if (p.have_think) p.s.pre_dz += z0 - p.z_think;
  p.have_think = false;
  g_cur = debug ? k : -1;
  __imp__sub_824B39B8(ctx, base);
  g_cur = -1;
  const float fx = g::ReadF32(base, kPml), fy = g::ReadF32(base, kPml + 4);
  const float len = std::hypot(fx, fy);
  if (len > 0.01f) {
    p.fwd[0] = fx / len;
    p.fwd[1] = fy / len;
    p.have_fwd = true;
  }
  const float z1 = g::ReadF32(base, ps + kPsOrigin + 8);
  p.z = z1;
  p.have_z = true;
  p.z_end = z1;
  p.have_end = true;
  ++p.s.pmoves;
  p.s.pmove_dz += z1 - z0;
  if (debug) MaybeLog(base);
}

// ClientThink_real's last call before it keeps the origin for the warp check (r3 = the entity): the placement, and
// the start of each think for the "between" / "pre" heights.
REX_EXTERN(__imp__sub_82483190);
REX_HOOK_RAW(sub_82483190) {
  const uint32_t ent = ctx.r3.u32;
  __imp__sub_82483190(ctx, base);
  if (!Active() || !Valid(ent)) return;
  const uint32_t ps = g::ReadU32(base, ent + kEntClient);
  const int k = Valid(ps) ? LocalIndex(base, ps) : -1;
  if (k < 0) return;
  Player& p = g_players[k];
  const std::string& place = REXCVAR_GET(cod3_test_levitate);
  if (k == 1 && !place.empty() && !p.placed) {
    MaybePlace(base, ps, place, p);
    if (p.placed) p.have_end = false;
  }
  const float z = g::ReadF32(base, ps + kPsOrigin + 8);
  if (p.have_end) p.s.between_dz += z - p.z_end;
  p.have_end = false;
  p.z_think = z;
  p.have_think = true;
  ++p.s.thinks;
}

// The stuck fix (ClientThink_real, after a pushed stick moved nothing; also from inside Pmove): how far it moves
// each player. Its hook is stuck_fix_mp.cpp's (the levitation fix); this sees each call before and after it.
namespace {
int g_fix_player = -1;
float g_fix_a[3] = {};
void OnStuckFix(uint8_t* base, uint32_t pm, bool after, bool applied) {
  if (!after) {
    const int k = PlayerOfPm(base, pm);
    g_fix_player = k;
    g_fix_k = k;
    if (k < 0) return;
    const uint32_t ps = g::ReadU32(base, pm);
    for (uint32_t i = 0; i < 3; ++i) g_fix_a[i] = g::ReadF32(base, ps + kPsOrigin + 4 * i);
    if (applied) ++g_players[k].s.fix_applied;
    return;
  }
  const int k = g_fix_player;
  g_fix_player = -1;
  g_fix_k = -1;
  if (k < 0 || !Valid(pm)) return;
  const uint32_t ps = g::ReadU32(base, pm);
  if (!Valid(ps)) return;
  float b[3];
  for (uint32_t i = 0; i < 3; ++i) b[i] = g::ReadF32(base, ps + kPsOrigin + 4 * i);
  g_players[k].z_end = b[2];
  g_players[k].have_end = true;
  Stats& s = g_players[k].s;
  ++s.fix_calls;
  const double dz = b[2] - g_fix_a[2];
  const double dxy = std::hypot(b[0] - g_fix_a[0], b[1] - g_fix_a[1]);
  if (dz != 0 || dxy != 0) ++s.fix_moved;
  s.fix_dz += dz;
  s.fix_dxy += dxy;
  if (dz > s.fix_max_dz) s.fix_max_dz = dz;
}
[[maybe_unused]] const bool kObserving = (cod3::movement::stuck_fix::SetObserver(&OnStuckFix), true);
}  // namespace

// The stuck fix's parts (dev attribution): how each call site moves the sphere r3 points at.
namespace {
bool IsPlayerEntity(uint8_t* base, uint32_t ent) {
  if (!Valid(ent)) return false;
  const uint32_t client = g::ReadU32(base, ent + kEntClient);
  return Valid(client) && g::ReadU32(base, client + 1932) == 2;  // gclient + 1932 == 2: connected
}
void TrackFixPart(PPCContext& ctx, uint8_t* base, void (*orig)(PPCContext&, uint8_t*)) {
  const int k = g_fix_k;
  const uint32_t lr = static_cast<uint32_t>(ctx.lr);
  int part = -1;
  for (int i = 0; i < 5; ++i)
    if (kFixParts[i].lr == lr) part = i;
  const uint32_t pos = ctx.r3.u32;
  if (k < 0 || part < 0 || !Valid(pos)) {
    orig(ctx, base);
    return;
  }
  const bool player = IsPlayerEntity(base, ctx.r4.u32);
  float a[3], b[3];
  for (uint32_t i = 0; i < 3; ++i) a[i] = g::ReadF32(base, pos + 4 * i);
  orig(ctx, base);
  for (uint32_t i = 0; i < 3; ++i) b[i] = g::ReadF32(base, pos + 4 * i);
  Stats& s = g_players[k].s;
  ++s.part_calls[part];
  if (player) ++s.part_player[part];
  const double dz = b[2] - a[2], dxy = std::hypot(b[0] - a[0], b[1] - a[1]);
  if (dz != 0 || dxy != 0) ++s.part_moved[part];
  s.part_dz[part] += dz;
  s.part_dxy[part] += dxy;
}
}  // namespace

REX_EXTERN(__imp__sub_8248B150);
REX_HOOK_RAW(sub_8248B150) {
  TrackFixPart(ctx, base, [](PPCContext& c, uint8_t* b) { __imp__sub_8248B150(c, b); });
}
REX_EXTERN(__imp__sub_8248B020);
REX_HOOK_RAW(sub_8248B020) {
  TrackFixPart(ctx, base, [](PPCContext& c, uint8_t* b) { __imp__sub_8248B020(c, b); });
}
REX_EXTERN(__imp__sub_8248ACB8);
REX_HOOK_RAW(sub_8248ACB8) {
  TrackFixPart(ctx, base, [](PPCContext& c, uint8_t* b) { __imp__sub_8248ACB8(c, b); });
}

// The warp check (ClientThink_real, after a move longer than 15): true puts the player back.
REX_EXTERN(__imp__sub_8249EE30);
REX_HOOK_RAW(sub_8249EE30) {
  const int k = PlayerOfPm(base, ctx.r3.u32);
  __imp__sub_8249EE30(ctx, base);
  if (k < 0) return;
  Stats& s = g_players[k].s;
  ++s.warp_calls;
  if ((ctx.r3.u32 & 0xFF) != 0) ++s.warp_back;
}

REX_EXTERN(__imp__sub_824A87E0);
REX_HOOK_RAW(sub_824A87E0) {  // PM_StepSlideMove(gravity)
  const int k = g_cur;
  const uint32_t ps = k >= 0 ? CurrentPs(base) : 0;
  if (ps == 0) {
    __imp__sub_824A87E0(ctx, base);
    return;
  }
  const float z0 = g::ReadF32(base, ps + kPsOrigin + 8);
  __imp__sub_824A87E0(ctx, base);
  const float d = g::ReadF32(base, ps + kPsOrigin + 8) - z0;
  Stats& s = g_players[k].s;
  ++s.steps;
  s.step_dz += d;
  if (d > 0.01f) ++s.step_up;
  if (d < -0.01f) ++s.step_down;
  if (d > s.step_max) s.step_max = d;
}

REX_EXTERN(__imp__sub_824A1828);
REX_HOOK_RAW(sub_824A1828) {  // PM_PlayerTrace(trace, start, mins, maxs, end, context)
  const int k = g_cur;
  const uint32_t tr = ctx.r3.u32, start = ctx.r4.u32, end = ctx.r7.u32;
  if (k < 0 || !Valid(tr) || !Valid(start) || !Valid(end)) {
    __imp__sub_824A1828(ctx, base);
    return;
  }
  const Site site = SiteOf(static_cast<uint32_t>(ctx.lr));
  const float sz = g::ReadF32(base, start + 8);
  __imp__sub_824A1828(ctx, base);
  Stats& s = g_players[k].s;
  ++s.calls[site];
  const bool ss = g::ReadU8(base, tr + kTraceStartSolid) != 0;
  const bool as = g::ReadU8(base, tr + kTraceAllSolid) != 0;
  if (ss) ++s.startsolid[site];
  if (as) ++s.allsolid[site];
  if (site == kUp && ss && !as) s.up_inside += g::ReadF32(base, tr + 8) - sz;
}

#endif  // COD3_MP
