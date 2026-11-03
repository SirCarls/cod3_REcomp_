// cod3 - the game's dynamic shadows and the "Shadow quality" setting (see
// shadows.h).

#include "game/shadows.h"

#include <atomic>
#include <chrono>
#include <mutex>
#include <string>

#include <fmt/format.h>
#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

#include "game/addresses.h"
#include "game/guest.h"

// "high" (every map remade at 1024x1024) was retired; a saved "high" is
// refused when the settings load, which leaves the default, "original".
REXCVAR_DEFINE_STRING(cod3_shadow_quality, "original", "cod3/Display",
                      "Shadows of soldiers, vehicles and other models: original (the game's own) "
                      "or off")
    .allowed({"original", "off"})
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
#if defined(COD3_MP)
// [claude-9b-think] 2026-10-27 UNTESTED (owner 15:50: "how can we optimize the way shadows are drawn/called";
// FINDINGS-SHADOWS 1b). Split screen only (2-4 local players), like cod3mp_split_lod.
REXCVAR_DEFINE_STRING(cod3mp_split_shadows, "game", "cod3/Display",
                      "Split screen: the shadow maps each view aims at nearby soldiers and vehicles. game (the "
                      "nearest group and the next one, as the game does), nearest (the nearest group only), off "
                      "(none; the sunlight stays)")
    .allowed({"game", "nearest", "off"})
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
#endif
REXCVAR_DEFINE_BOOL(cod3_debug_shadows, false, "cod3",
                    "Log once a second, per view, the caster groups the shadow maps were aimed at and those "
                    "cod3mp_split_shadows left out, and the models offered to the shadow maps");

namespace {

namespace sh = cod3::addr::shadows;
using cod3::guest::ReadU32;
using cod3::guest::WriteU32;

// "off", read by the caster hook below.
std::atomic<bool> g_off{false};
std::once_flag g_start_once;

std::string Quality() {
  const std::string q = rex::cvar::GetFlagByName("cod3_shadow_quality");
  return q.empty() ? "original" : q;
}

void WatchSetting() {
  rex::cvar::RegisterChangeCallback(
      "cod3_shadow_quality", [](std::string_view, std::string_view) { cod3::ApplyShadowQuality(); });
}

// [claude-9b-think] cod3_debug_shadows: per view (the add-caster's view index, 0-3; 4: any other), the models
// offered to the shadow maps, the caster groups a view's maps were aimed at, and those cod3mp_split_shadows left out.
struct ShadowCounts {
  std::atomic<uint64_t> offers[5]{}, aimed[5]{}, skipped[5]{};
  std::atomic<uint64_t> setups{0}, aim_passes{0};  // the shadow-light set-ups (five fresh scenes) and aim passes
};
ShadowCounts g_counts;
std::atomic<int64_t> g_next_log_ms{0};

int64_t NowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

uint32_t ViewSlot(uint8_t* base) {
  const uint32_t view = ReadU32(base, sh::kShadowView);
  return view < 4 ? view : 4;
}

void MaybeLogCounts() {
  const int64_t now = NowMs();
  int64_t next = g_next_log_ms.load(std::memory_order_relaxed);
  if (now < next || !g_next_log_ms.compare_exchange_strong(next, now + 1000)) return;
  if (next == 0) return;  // the first second starts now
  // Set-ups against aim passes: one each per view, or one set-up a frame for all views (FINDINGS-SHADOWS, open).
  std::string line = fmt::format(" | {} shadow set-ups, {} aim passes", g_counts.setups.exchange(0),
                                 g_counts.aim_passes.exchange(0));
  for (int v = 0; v < 5; ++v) {
    const uint64_t offers = g_counts.offers[v].exchange(0), aimed = g_counts.aimed[v].exchange(0),
                   skipped = g_counts.skipped[v].exchange(0);
    if (!offers && !aimed && !skipped) continue;
    line += fmt::format(" | view {}: {} models offered, {} caster maps aimed, {} left out", v, offers, aimed, skipped);
  }
  REXLOG_INFO("cod3: shadows in the last second{}", line);
}

void CountCasterOffer(uint8_t* base) {
  if (!REXCVAR_GET(cod3_debug_shadows)) return;
  g_counts.offers[ViewSlot(base)].fetch_add(1, std::memory_order_relaxed);
  MaybeLogCounts();
}

}  // namespace

namespace cod3 {

void ApplyShadowQuality() {
  const bool off = Quality() == "off";
  if (g_off.exchange(off, std::memory_order_relaxed) != off)
    REXLOG_INFO("cod3: shadow quality: dynamic shadows {}", off ? "off" : "on");
}

}  // namespace cod3

// The game makes its five shadow maps here: at start (Sys_Main), and the
// shadow-light set-up 0x8241B9C8 calls it at every frame's start (game
// thread), when it returns at once because slot 4 has a map (see
// addresses.h, namespace shadows). The maps are always the game's own; this
// only starts following the setting once they exist.
REX_EXTERN(__imp__sub_82419098);
REX_HOOK_RAW(sub_82419098) {
  const bool making = ReadU32(base, sh::kMaps + 16) == 0;
  __imp__sub_82419098(ctx, base);
  if (!making) return;
  std::call_once(g_start_once, WatchSetting);
  cod3::ApplyShadowQuality();
}

// "off": no model is added to a shadow map. 0x82425BD0 adds a model to every
// slot that has a light this frame; here it takes the game's own path for a
// frame where no slot has one (its early returns, else the model's shadow node
// at +144 cleared and 0 returned). The slot lights themselves stay: they also
// carry the sun's light onto the ground and the models (without them the world
// near the camera goes dark; run D, NOTES), so the maps are drawn empty and
// nothing is in shadow. The maps in use are still drawn and resolved each
// frame, so they come out empty at once, also when switched live (run E);
// maps aimed at casters get no light while there are none, so they aren't
// read. r_testshadow 0 is not an "off": it only stops the camera cascades
// and the maps aimed at the casters still draw.
REX_EXTERN(__imp__sub_82425BD0);
REX_HOOK_RAW(sub_82425BD0) {
  CountCasterOffer(base);  // [claude-9b-think] cod3_debug_shadows
  if (!g_off.load(std::memory_order_relaxed)) {
    __imp__sub_82425BD0(ctx, base);
    return;
  }
  ctx.r3.u64 = 0;
  // The early returns: the model casts in no view of this pass (a 64-bit mask
  // and a byte per view in the record r10 points to), or no "white" texture.
  const uint32_t record = ReadU32(base, ctx.r10.u32);
  const uint32_t view = ReadU32(base, sh::kShadowView);
  const uint64_t mask = (uint64_t(ReadU32(base, record)) << 32) | ReadU32(base, record + 4);
  if (view >= 64 || !((mask >> view) & 1)) return;
  if (cod3::guest::ReadU8(base, record + (view + 2) * 4)) return;
  if (!ReadU32(base, sh::kWhiteTexture)) return;
  if (ctx.r4.u32) WriteU32(base, ctx.r4.u32 + sh::kShadowNode, 0);
}

#if defined(COD3_MP)
// [claude-9b-think] 2026-10-27 UNTESTED: cod3mp_split_shadows (FINDINGS-SHADOWS 1b). How cod3mp aims its shadow maps
// in split screen (campaign addresses in brackets):
//  * Each view's scene build (0x824483C0 [0x82430210], once per view from RE_RenderScene) aims the camera
//    cascades only with one local player: 0x82442A20 [0x8242BAA0] (three cascades, 384 / 1024 / 2048 units, slots
//    2-4). With 2-4 local players (kMpLocalPlayers < 2 fails) it calls 0x82437838 instead: one 2048-unit sun light
//    added to the scene as its light, not to a shadow slot. So split screen already has no cascade maps; that
//    choice is the game's own.
//  * At the end of each view's build 0x82443110 [0x8242C170] aims slots 0 and 1 at caster groups: 0x820D3050
//    [0x820C2E40] finds the nearest caster not yet in a group (the caster list 0x82BA3514 [0x82A11D34], each
//    record's +0x98 its squared distance) within 200 units for slot 0 and within a run-time range for slot 1, and
//    grows the group (+0x24 the group id). 0x82438240 [0x824202C0] then aims the slot's light at the group's
//    sphere, draws that slot's ShadowMapLight scene and gives the light to the casters inside it. When the finder
//    finds nothing, the loop stops and the game clears the unused slots' scenes (its own "no one nearby" path).
//  * Slots 2 and 3 are aimed again at the casters' bounds (0x820D3210 / 0x82440E78) only when a cascade
//    filled them, which split screen doesn't.
// So with N local players, each frame has up to 2N caster maps (each drawn and resolved). "nearest" answers
// "nothing found" to the second search of each view, and "off" to both: the game's own path, as when no one is
// near. With one local player nothing changes.
namespace {
constexpr uint32_t kMpLocalPlayers = 0x82BCC878;  // local players in the match (as ultrawide.cpp)
thread_local int t_group_search = -1;             // the searches so far in this aim pass; -1: outside one
}  // namespace

// The aim pass (one per view).
REX_EXTERN(__imp__sub_82443110);
REX_HOOK_RAW(sub_82443110) {
  if (REXCVAR_GET(cod3_debug_shadows)) g_counts.aim_passes.fetch_add(1, std::memory_order_relaxed);
  t_group_search = 0;
  __imp__sub_82443110(ctx, base);
  t_group_search = -1;
}

// The caster-group finder: f1 the range, r3 the caster list, r4 / r5 out: the group's center and radius;
// returns r3 nonzero when it found a group.
REX_EXTERN(__imp__sub_820D3050);
REX_HOOK_RAW(sub_820D3050) {
  const int search = t_group_search >= 0 ? t_group_search++ : -1;
  const bool debug = REXCVAR_GET(cod3_debug_shadows);
  const uint32_t view = debug ? ViewSlot(base) : 0;
  if (search >= 0 && ReadU32(base, kMpLocalPlayers) >= 2) {
    const std::string mode = rex::cvar::GetFlagByName("cod3mp_split_shadows");
    if (mode == "off" || (mode == "nearest" && search >= 1)) {
      ctx.r3.u64 = 0;  // nothing found: the game's own path
      if (debug) {
        g_counts.skipped[view].fetch_add(1, std::memory_order_relaxed);
        MaybeLogCounts();
      }
      return;
    }
  }
  __imp__sub_820D3050(ctx, base);
  if (debug && ctx.r3.u32) {
    g_counts.aimed[view].fetch_add(1, std::memory_order_relaxed);
    MaybeLogCounts();
  }
}
#endif  // COD3_MP

// [claude-9b-think] cod3_debug_shadows: the shadow-light set-up (campaign 0x8241B9C8, cod3mp 0x82433718; no direct
// caller, reached through a pointer): the slot lights cleared and five fresh "ShadowMapLight" scenes made.
#if defined(COD3_MP)
REX_EXTERN(__imp__sub_82433718);
REX_HOOK_RAW(sub_82433718) {
  if (REXCVAR_GET(cod3_debug_shadows)) g_counts.setups.fetch_add(1, std::memory_order_relaxed);
  __imp__sub_82433718(ctx, base);
}
#else
REX_EXTERN(__imp__sub_8241B9C8);
REX_HOOK_RAW(sub_8241B9C8) {
  if (REXCVAR_GET(cod3_debug_shadows)) g_counts.setups.fetch_add(1, std::memory_order_relaxed);
  __imp__sub_8241B9C8(ctx, base);
}
#endif
