// cod3 - aim assist on/off (the game's "Sticky Aim").
//
// Call of Duty 3's aim assist is the Options > Gameplay > Sticky Aim row,
// saved in the player's profile (controls settings +67, see addresses.h).
// The F2 settings offer the same switch (owner, 2026-10-09). To keep one
// truth: the profile value is what the game uses; a choice made in F2 is kept
// in cod3_aim_assist and re-applied whenever the profile is loaded, until the
// player changes the row in the game's own menu again (which hands the
// decision back to the profile).

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/system/kernel_state.h>

#include "game/addresses.h"
#include "game/aim_assist.h"
#include "game/guest.h"

REXCVAR_DEFINE_INT32(cod3_aim_assist, -1, "cod3/Game",
                     "Aim assist (the game's Sticky Aim): -1 = as set in Options > Gameplay, "
                     "0 = off, 1 = on")
    .range(-1, 1);

namespace {
namespace c = cod3::addr::controls;

uint8_t* GuestBase() {
  auto* ks = rex::system::kernel_state();
  return ks && ks->memory() ? ks->memory()->virtual_membase() : nullptr;
}

void ApplyOverride(uint8_t* base) {
  const int32_t v = REXCVAR_GET(cod3_aim_assist);
  if (base && v >= 0) cod3::guest::WriteU8(base, c::kSettings + c::kStickyAim, v ? 1 : 0);
}
}  // namespace

namespace cod3 {

std::optional<bool> AimAssistEnabled() {
  uint8_t* base = GuestBase();
  if (!base) return std::nullopt;
  return guest::ReadU8(base, c::kSettings + c::kStickyAim) != 0;
}

void SetAimAssist(bool on) {
  REXCVAR_SET(cod3_aim_assist, on ? 1 : 0);
  ApplyOverride(GuestBase());
}

}  // namespace cod3

// Profile loaded: re-apply a choice made in F2.
REX_EXTERN(__imp__Profile_LoadSettings);
REX_HOOK_RAW(Profile_LoadSettings) {
  __imp__Profile_LoadSettings(ctx, base);
  ApplyOverride(base);
}

// Options > Gameplay closed: the player chose in the game's menu, so the
// profile decides again.
REX_EXTERN(__imp__OptionsGameplayMenu__Apply);
REX_HOOK_RAW(OptionsGameplayMenu__Apply) {
  const uint8_t before = cod3::guest::ReadU8(base, c::kSettings + c::kStickyAim);
  __imp__OptionsGameplayMenu__Apply(ctx, base);
  if (cod3::guest::ReadU8(base, c::kSettings + c::kStickyAim) != before) REXCVAR_SET(cod3_aim_assist, -1);
}
