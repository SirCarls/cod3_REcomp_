// cod3mp - the levitation fix's hook (game/movement/stuck_fix.h explains the glitch, the rule and the toggle).
//
// [acct2-ocr] 2026-11-05. When a player pushed the stick and the move left the origin where it was, the game's
// unstick (0x824A13F0, from ClientThink_real) runs two passes on the pmove_t (r3 of each call, 0x82B922C8):
//   1. up to five calls of 0x8249AD48 with the tracemask set to CONTENTS_PLAYER only: it pushes the player out of
//      the other players (radius 15.1, a little wider than a player's 15);
//   2. up to five calls of 0x8249F070 with CONTENTS_PLAYER cleared from the mask: it pushes the player out of
//      walls and the rest of the world.
// Pass 1 pushes a standing player who walks into a prone one into the wall behind, and pass 2 then frees them from
// the wall upward, exactly 12.11 at a time; the two players climb each other (the levitation glitch). Measured in
// cod3_debug_stuck_fix logs: every 0x8249F070 call comes from 0x824A13F0 with the mask 0x00810011.
// While levitation_fix is 1, pass 1 is skipped (0x8249AD48 returns 0, "nothing to push", as when nobody
// overlaps): players are never moved out of each other by the game, as in Call of Duty 4, where a move that starts
// inside a player ignores players (CoD3 does the same in 0x824A1828) and nobody is pushed up. Pass 2, the walls,
// stays as it is. 0x8249AD48 has no other caller.
//
// The toggle is the match's rule levitation_fix (protected_dvars::Get; F2 > Game > Server, host only), the same
// setting as the campaign's. Each console runs ClientThink_real and the unstick for its own players (acct2-main T2,
// 2026-11-05; the system link test acct2-ocr-sl-a: host and joiner each called the stuck fix 60 times a second for
// their own player); on a guest Get gives the server's value or the match default (on), never the guest's own
// (stuck_fix.h). [acct2-ocr] 2026-11-05 21:45: cod3mp_levitation_fix, the per-player setting this read before (the
// F2 row changed nothing in cod3mp), is gone.
// [acct2-ocr] 2026-11-07: the match's host is the Demonware session's (game/mp/session.cpp IsHost; a split-screen
// match is this console's own). Its value is the match's: game/mp/session.cpp sends the host's match rules to the
// other consoles of a system link match (the port's 0x70 notices), and a guest uses them (ApplyServerInfo), or the
// match default (on) until they arrive. Before (runs acct2-ocr-lev7-off) every console was locked to the default.
// Everything below is cod3mp-only (inside COD3_MP, so tools/mp_aliases.py treats the hooks as this program's own).

#include "game/movement/stuck_fix.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

#include "game/guest.h"
#include "game/protected_dvars.h"

#if defined(COD3_MP)

REXCVAR_DEFINE_BOOL(cod3_debug_stuck_fix, false, "cod3",
                    "Multiplayer: log every 10 s this console's unstick calls: the player push (skipped while "
                    "levitation_fix is on) and the wall push, how often each moved or lifted the player")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace {
namespace g = cod3::guest;
namespace sf = cod3::movement::stuck_fix;
namespace pd = cod3::protected_dvars;
using Clock = std::chrono::steady_clock;

// The match's rule: the host's own value, or on a guest the server's (or the match default; today in every match,
// see the top of this file).
int FixOn() { return pd::Get(pd::Id::kLevitationFix); }

std::atomic<sf::Observer> g_observer{nullptr};

struct Pass {
  uint64_t calls = 0, skipped = 0, moved = 0, lifted = 0;
  double max_lift = 0;
};
Pass g_players_pass, g_walls_pass;  // the thread that runs ClientThink_real only
Clock::time_point g_next_log{};
bool g_logged_first = false;

bool Valid(uint32_t p) { return p >= 0x10000 && (p & 3) == 0; }

uint32_t PsOf(uint8_t* base, uint32_t pm) {  // pmove_t + 0: the playerState (its origin at + 0)
  if (!Valid(pm)) return 0;
  const uint32_t ps = g::ReadU32(base, pm);
  return Valid(ps) ? ps : 0;
}

void Count(Pass& p, uint8_t* base, uint32_t ps, const float (&a)[3]) {
  ++p.calls;
  if (ps == 0) return;
  float b[3];
  for (uint32_t i = 0; i < 3; ++i) b[i] = g::ReadF32(base, ps + 4 * i);
  const double dz = b[2] - a[2];
  if (dz != 0 || b[0] != a[0] || b[1] != a[1]) ++p.moved;
  if (dz > 0.5) ++p.lifted;
  if (dz > p.max_lift) p.max_lift = dz;
}

void MaybeLog() {
  if (!g_logged_first) {
    g_logged_first = true;
    REXLOG_INFO("cod3mp: stuck fix: the first unstick on this console (levitation_fix {}{})", FixOn(),
                pd::IsLocked() ? ", the server's" : "");
  }
  const auto now = Clock::now();
  if (now < g_next_log) return;
  const bool first = g_next_log == Clock::time_point{};
  g_next_log = now + std::chrono::seconds(10);
  if (first) return;
  const Pass& p = g_players_pass;
  const Pass& w = g_walls_pass;
  REXLOG_INFO("cod3mp: stuck fix: 10 s, levitation_fix {}: player push {} calls, {} skipped, {} moved, {} "
              "lifted (most {:.2f}); wall push {} calls, {} moved, {} lifted (most {:.2f})",
              FixOn(), p.calls, p.skipped, p.moved, p.lifted, p.max_lift, w.calls, w.moved, w.lifted, w.max_lift);
  g_players_pass = Pass{};
  g_walls_pass = Pass{};
}
}  // namespace

namespace cod3::movement::stuck_fix {
void SetObserver(Observer observer) { g_observer.store(observer, std::memory_order_relaxed); }
}  // namespace cod3::movement::stuck_fix

// Pass 1, the player push (only ever called from 0x824A13F0, at 0x824A1428 and 0x824A1440).
REX_EXTERN(__imp__sub_8249AD48);
REX_HOOK_RAW(sub_8249AD48) {
  const bool debug = REXCVAR_GET(cod3_debug_stuck_fix);
  if (FixOn() != 0) {
    ctx.r3.u64 = 0;  // nothing pushed: 0x824A13F0 goes on to the wall pass
    if (debug) {
      ++g_players_pass.calls;
      ++g_players_pass.skipped;
      MaybeLog();
    }
    return;
  }
  const uint32_t ps = debug ? PsOf(base, ctx.r3.u32) : 0;
  float a[3] = {};
  if (ps != 0)
    for (uint32_t i = 0; i < 3; ++i) a[i] = g::ReadF32(base, ps + 4 * i);
  __imp__sub_8249AD48(ctx, base);
  if (!debug) return;
  Count(g_players_pass, base, ps, a);
  MaybeLog();
}

// Pass 2, the wall push: unchanged; seen by the probe (levitate_probe_mp.cpp) and the debug log.
REX_EXTERN(__imp__sub_8249F070);
REX_HOOK_RAW(sub_8249F070) {
  const uint32_t pm = ctx.r3.u32;
  const bool debug = REXCVAR_GET(cod3_debug_stuck_fix);
  const sf::Observer observer = g_observer.load(std::memory_order_relaxed);
  const uint32_t ps = debug ? PsOf(base, pm) : 0;
  float a[3] = {};
  if (ps != 0)
    for (uint32_t i = 0; i < 3; ++i) a[i] = g::ReadF32(base, ps + 4 * i);
  if (observer) observer(base, pm, false, false);
  __imp__sub_8249F070(ctx, base);
  if (observer) observer(base, pm, true, false);
  if (!debug) return;
  Count(g_walls_pass, base, ps, a);
  MaybeLog();
}

#endif  // COD3_MP
