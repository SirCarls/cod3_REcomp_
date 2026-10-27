// cod3 - [claude-9b] 2026-10-27 why some explosive barrels burst without flames (plan item 7): a measurement, no
// change in behavior. dev/agents/claude-9b-think/FINDINGS-2026-10-27.md section 2. Campaign only for now (the cod3mp
// functions aren't mapped yet).
//
// A destroyed barrel's effect group (0x82533978) picks one of up to 5 effect names at random and registers it with
// FX_RegisterEffect (0x824293E8), which returns 0 for an effect the level didn't load: the play then does nothing, with
// only a developer print (Com_DPrintf). An effect instance comes from a pool of 128 (0x820EB270, the "aps" effects);
// when it is empty, or the instance can't start, it returns 0 and only the game's own message ring hears of it.
// With cod3_debug_effects:
//   "cod3: effects: 'NAME' isn't loaded (FX_RegisterEffect returned 0)"   once per name
//   "cod3: effects: an effect instance wasn't created for 'NAME' (pool empty or the start failed), N so far"
//                                                                          at most once a second

#include <chrono>
#include <cstdint>
#include <mutex>
#include <set>
#include <string>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

#include "game/guest.h"

REXCVAR_DEFINE_BOOL(cod3_debug_effects, false, "cod3",
                    "Log effects the level doesn't have (each name once) and effect instances that weren't created "
                    "(the effect pool full); for barrels that burst without flames")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace {

namespace g = cod3::guest;
using Clock = std::chrono::steady_clock;

constexpr uint32_t kEffectDefName = 116;  // the effect definition (0x820EB270's r3) + 116: its name
constexpr size_t kMaxNames = 512;

std::mutex g_mutex;
std::set<std::string> g_missing;
uint64_t g_failed_instances = 0;
Clock::time_point g_last_instance_log{};

bool Valid(uint32_t p) { return p >= 0x10000; }

}  // namespace

REX_EXTERN(__imp__FX_RegisterEffect);
REX_HOOK_RAW(FX_RegisterEffect) {
  const bool debug = REXCVAR_GET(cod3_debug_effects);
  const uint32_t name_ptr = ctx.r3.u32;
  std::string name;
  if (debug && Valid(name_ptr)) name = g::ReadString(base, name_ptr, 128);
  __imp__FX_RegisterEffect(ctx, base);
  if (!debug || ctx.r3.u32 != 0) return;
  std::lock_guard<std::mutex> lock(g_mutex);
  if (g_missing.size() >= kMaxNames || !g_missing.insert(name).second) return;
  REXLOG_INFO("cod3: effects: '{}' isn't loaded (FX_RegisterEffect returned 0)", name);
}

REX_EXTERN(__imp__sub_820EB270);
REX_HOOK_RAW(sub_820EB270) {
  const bool debug = REXCVAR_GET(cod3_debug_effects);
  const uint32_t def = ctx.r3.u32;
  __imp__sub_820EB270(ctx, base);
  if (!debug || ctx.r3.u32 != 0) return;
  const std::string name = Valid(def) ? g::ReadString(base, def + kEffectDefName, 64) : std::string("?");
  std::lock_guard<std::mutex> lock(g_mutex);
  ++g_failed_instances;
  const auto now = Clock::now();
  if (now - g_last_instance_log < std::chrono::seconds(1)) return;
  g_last_instance_log = now;
  REXLOG_INFO("cod3: effects: an effect instance wasn't created for '{}' (pool empty or the start failed), {} so far",
              name, g_failed_instances);
}
