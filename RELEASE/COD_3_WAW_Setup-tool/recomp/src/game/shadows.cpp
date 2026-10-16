// cod3 - the game's dynamic shadows and the "Shadow quality" setting (see
// shadows.h).

#include "game/shadows.h"

#include <atomic>
#include <mutex>
#include <string>

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
