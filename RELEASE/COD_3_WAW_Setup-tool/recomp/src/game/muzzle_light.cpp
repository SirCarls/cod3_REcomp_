// cod3 - [claude-9b-think] 2026-10-27 UNTESTED: muzzle-flash lights held for a set time instead of one drawn frame.
// Both programs.
//  - Owner 15:10: "might be muzzle flash from near by teammate".
//  - claude-9b-test s143: the saint_lo flash at +26.6 / 34.6 / 43.1 / 51.1 s is a soldier's rifle firing. The ground
//    and sandbags around him are lit warm for exactly one presented frame, at any frame rate (also with a 30 fps cap).
//
// How the game does it. Campaign addresses; cod3mp's in brackets.
//  - A weapon's fire (Bullet_Fire_Extended -> sub_8244D1A0 / sub_8244D320) plays the weapon's muzzle-flash effect.
//    Its light element is an AbstractEffectLight.
//  - When the light's definition has its flash field set (def + 0x28), the constructor (AbstractEffectLight__ctor)
//    asks the light list for a light lasting "1 frame". The light list's allocator (sub_8241B050 [sub_82432F08])
//    turns that 1 into a 30 ms countdown: one frame at the console's 30 frames a second.
//  - AbstractEffectLight's "done?" test (vfunc_6, slot 6) answers yes at once for such an effect. So the effect system
//    deletes the effect at its next update, and the destructor takes the light out of the list. The light is drawn in
//    exactly one frame: 33 ms at the console's 30 fps, 3 ms at 300 fps (the one-frame warm flash of s143).
//  - The light list (sub_82423BA8, run every frame from sub_82430108 with Com_Frame's msec) also counts the 30 ms
//    down. It hands each live light to the renderer: sub_82150B50, a sphere test (sub_820C1A18), then the frame's light
//    list (sub_8214FF10). The effect is gone long before the countdown ends.
//  - Brightness and size: the light carries the definition's color (def + 0x04..0x0C) times 1.0, and its two radii
//    (def + 0x14 / 0x18; the sphere test uses the second). Nothing on that path depends on the resolution, the HDR
//    path or gamma, and the port hooks none of it. The lit patch is as large and as bright as the game's data makes
//    it. A run capped at 30 fps already shows it for 33 ms, which is what this setting gives at any frame rate.
//
// cod3_muzzle_light_ms N (default 30, the game's own figure; 0: as before). The "done?" test answers no for such an
// effect until N ms of real time after it was first asked, so its light is drawn in every frame of those N ms. The
// light's own countdown is set to N + 100 ms (the light list's msec), so the list still ends it if the effect were
// never deleted. After that, the game's own test runs: yes, the effect is deleted and its light removed, as before.
// The light keeps the position, color and radii the game gave it; nothing else is written.

#include <chrono>
#include <cstdint>
#include <mutex>
#include <unordered_map>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

#include "game/guest.h"

REXCVAR_DEFINE_INT32(cod3_muzzle_light_ms, 30, "cod3/Video",
                     "Muzzle flashes light the ground around the gun for this long (ms) at any frame rate; 30 is the "
                     "game's own figure (one frame at the console's 30 fps). 0: one drawn frame, as before (a flicker "
                     "at high frame rates)")
    .range(0, 100)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(cod3_debug_muzzle_light, false, "cod3",
                    "Log once a second how many flash lights cod3_muzzle_light_ms held and for how many of the game's "
                    "\"done?\" questions");

namespace {

namespace g = cod3::guest;
using Clock = std::chrono::steady_clock;

// AbstractEffectLight: its definition, and its flash light. The campaign keeps a handle to the light (the low 7 bits a
// slot of the light list's table, the rest the slot's generation); cod3mp keeps a pointer.
constexpr uint32_t kEffectDef = 0x30;
constexpr uint32_t kEffectFlashLight = 0x34;
constexpr uint32_t kDefFlash = 0x28;  // the definition's flash field: nonzero, a one-frame light
#if !defined(COD3_MP)
constexpr uint32_t kLightSlots = 0x82A9F290;  // per slot (8 bytes): the light, then the slot's generation
#endif
// A light of the light list (0x70 bytes).
constexpr uint32_t kLightLit = 0x30;      // byte: nonzero while lit
constexpr uint32_t kLightLeft = 0x34;     // float: ms left (-1000: no end)
constexpr uint32_t kLightInitial = 0x38;  // float: ms it was made with
constexpr float kOneFrameMs = 30.0f;      // what the allocator makes of "1 frame"
constexpr int kListMarginMs = 100;

bool Valid(uint32_t p) { return p >= 0x10000 && (p & 3) == 0; }

// The effect's flash light, or 0 (none, or no longer in the list).
uint32_t FlashLight(uint8_t* base, uint32_t effect) {
  const uint32_t v = g::ReadU32(base, effect + kEffectFlashLight);
  if (!v) return 0;
#if defined(COD3_MP)
  return Valid(v) ? v : 0;
#else
  const uint32_t slot = v & 0x7F;
  if (g::ReadU32(base, kLightSlots + slot * 8 + 4) != (v >> 7)) return 0;
  const uint32_t light = g::ReadU32(base, kLightSlots + slot * 8);
  return Valid(light) ? light : 0;
#endif
}

std::mutex g_mutex;
std::unordered_map<uint32_t, Clock::time_point> g_first_asked;  // flash effects being held: when first asked
struct Counts {
  uint64_t held = 0;     // flash lights held
  uint64_t answers = 0;  // "not yet" answers given
  Clock::time_point next_log{};
} g_counts;

// Every entry older than a second is from an effect deleted some other way (a level's end, say).
void Prune(Clock::time_point now) {
  for (auto it = g_first_asked.begin(); it != g_first_asked.end();) {
    if (now - it->second > std::chrono::seconds(1)) it = g_first_asked.erase(it);
    else ++it;
  }
}

void MaybeLog(Clock::time_point now) {
  if (!REXCVAR_GET(cod3_debug_muzzle_light)) return;
  if (now < g_counts.next_log) return;
  if (g_counts.next_log != Clock::time_point{}) {
    REXLOG_INFO("cod3: muzzle light: {} flash lights held for cod3_muzzle_light_ms {} ({} \"not yet\" answers, {} "
                "being held now)",
                g_counts.held, REXCVAR_GET(cod3_muzzle_light_ms), g_counts.answers, g_first_asked.size());
  }
  g_counts.held = g_counts.answers = 0;
  g_counts.next_log = now + std::chrono::seconds(1);
}

// True: answer "not yet" for this effect (a flash light still inside its N ms).
bool HoldFlash(uint8_t* base, uint32_t effect) {
  const int ms = REXCVAR_GET(cod3_muzzle_light_ms);
  if (ms <= 0 || !Valid(effect)) return false;
  const uint32_t def = g::ReadU32(base, effect + kEffectDef);
  if (!Valid(def) || g::ReadU32(base, def + kDefFlash) == 0) return false;
  const uint32_t light = FlashLight(base, effect);
  // Only the game's own one-frame lights, still lit.
  if (!light || g::ReadF32(base, light + kLightInitial) != kOneFrameMs || g::ReadU8(base, light + kLightLit) == 0) {
    return false;
  }
  const Clock::time_point now = Clock::now();
  const auto hold = std::chrono::milliseconds(ms);
  std::lock_guard<std::mutex> lock(g_mutex);
  MaybeLog(now);
  auto [it, fresh] = g_first_asked.try_emplace(effect, now);
  if (!fresh && now - it->second > hold + std::chrono::milliseconds(500)) {
    it->second = now;  // a deleted effect's entry, the address reused
    fresh = true;
  }
  if (fresh) {
    if (g_first_asked.size() > 512) Prune(now);
    g::WriteF32(base, light + kLightLeft, static_cast<float>(ms + kListMarginMs));
    ++g_counts.held;
  }
  if (now - it->second < hold) {
    ++g_counts.answers;
    return true;
  }
  g_first_asked.erase(it);
  return false;
}

}  // namespace

// AbstractEffectLight's "done?" test (vtable slot 6; campaign 0x8252CD90, cod3mp 0x8254D360; the same name in both
// programs' generated code). r3: the effect; returns r3 nonzero when done.
REX_EXTERN(__imp__AbstractEffectLight__vfunc_6);
REX_HOOK_RAW(AbstractEffectLight__vfunc_6) {
  if (HoldFlash(base, ctx.r3.u32)) {
    ctx.r3.u64 = 0;  // not done yet: the effect, and so its light, stay one more frame
    return;
  }
  __imp__AbstractEffectLight__vfunc_6(ctx, base);
}
