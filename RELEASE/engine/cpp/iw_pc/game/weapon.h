// The weapon's part of the movement (bg_weapons.c): aiming down the sights and the aim's
// spread, Call of Duty 2 (see the Rust version, iw_pc::game::weapon). The weapons' definitions
// are the host's (MoveHost::weapon_def).
#pragma once

#include <cstddef>
#include <cstdint>

#include "iw_pc/game/pmove.h"
#include "iw_pc/game/trace.h"

namespace iw_pc::game::weapon {

/// Bytes of Call of Duty 2's WeaponDef.
inline constexpr std::size_t kWeaponDefSize = 0x604;

/// Where a definition keeps the fields the movement reads (Call of Duty 4's names where the use matches).
namespace def {
inline constexpr std::size_t kPlayerAnimType = 0x74, kWeapType = 0x78, kWeapClass = 0x7C, kMoveSpeedScale = 0x264,
                             kOverlayReticle = 0x278, kHipSpreadDecayRate = 0x2A4, kHipSpreadTurnAdd = 0x2AC,
                             kHipSpreadMoveAdd = 0x2B0, kHipSpreadDuckedDecay = 0x2B4, kHipSpreadProneDecay = 0x2B8,
                             kAimDownSight = 0x32C, kRechamberWhileAds = 0x330, kAdsFire = 0x348,
                             kSegmentedReload = 0x35C, kReloadAdsTime = 0x524, kOoPosAnimLengthIn = 0x588,
                             kOoPosAnimLengthOut = 0x58C, kOffhandClass = 0x84, kAmmoIndex = 0x1C8, kClipIndex = 0x1D0,
                             kMeleeDelay = 0x200, kHoldFireTime = 0x210, kMeleeTime = 0x214, kQuickDropTime = 0x240,
                             kQuickRaiseTime = 0x244, kFuseTime = 0x248, kAdsTransInTime = 0x2C0, kAdsTransOutTime = 0x2C4,
                             kCookOffHold = 0x33C, kAltWeaponIndex = 0x36C;
}

/// The binoculars' weapon kind (WEAPTYPE_BINOCULARS).
inline constexpr std::int32_t kWeaptypeBinoculars = 3;
/// Weapon states the sights test (by number; Call of Duty 2's names aren't known).
namespace state {
inline constexpr std::int32_t kFiring = 3, kRechambering = 4, kReloading = 5, kReloadEnd = 9;
}

/// A weapon's definition: the game's structure as its bytes.
struct WeaponDef : pmove::Bytes {
    WeaponDef() : Bytes(kWeaponDefSize) {}
};

/// The first weapon (1 to weapon_count) that is the binoculars, as a byte; 0 for none.
/// Original: BG_GetBinocularWeaponIndex at 0x004F1D30 (CoD2MP_s.exe 1.3; our name; the result in al)
std::int32_t binocular_weapon_index(const trace::MoveHost& host);

/// Whether weapon state `state` holds the binoculars up to the eye (19 or 20).
/// Original: PM_IsBinocularsUp at 0x004F2620 (CoD2MP_s.exe 1.3; our name; the state in eax)
bool binoculars_up(std::int32_t state);

/// The weapon in hand as the sights and the animations see it (inlined in the original).
std::int32_t view_weapon(const pmove::PlayerState& ps, const trace::MoveHost& host);

/// Whether the player may aim down the sights (see the Rust version).
/// Original: PM_IsAdsAllowed at 0x004F2640 (CoD2MP_s.exe 1.3; Call of Duty 4's name; ps in esi, pml on the stack; al)
bool is_ads_allowed(const pmove::PlayerState& ps, const pmove::MoveLocal& pml, const trace::MoveHost& host);

/// The aim-down-the-sights flag for this move (see the Rust version).
/// Original: PM_UpdateAimDownSightFlag at 0x004F26F0 (CoD2MP_s.exe 1.3; Call of Duty 4's name; pm in edi, pml in eax)
void update_aim_down_sight_flag(pmove::Pmove& pm, const pmove::MoveLocal& pml, trace::MoveHost& host);

/// The weapon on its way to the sights or back (see the Rust version).
/// Original: PM_UpdateAimDownSightLerp at 0x004F2820 (CoD2MP_s.exe 1.3; Call of Duty 4's name; pm, pml on the stack)
void update_aim_down_sight_lerp(pmove::Pmove& pm, const pmove::MoveLocal& pml, const pmove::MoveSettings& settings,
                                const trace::MoveHost& host);

/// The aim's spread this move (see the Rust version).
/// Original: PM_UpdateAimSpreadScale at 0x004F3EF0 (CoD2MP_s.exe 1.3; Call of Duty 4's name; pm in ebx, pml on the stack)
void update_aim_spread_scale(pmove::Pmove& pm, const pmove::MoveLocal& pml, const pmove::MoveSettings& settings,
                             const trace::MoveHost& host);

/// The breath held through a scope, and the sway (see the Rust version).
/// Original: PM_UpdateHoldBreath at 0x004F2AA0 (CoD2MP_s.exe 1.3; Call of Duty 4's name; pm, pml on the stack)
void update_hold_breath(pmove::Pmove& pm, const pmove::MoveLocal& pml, const pmove::MoveSettings& settings,
                        const trace::MoveHost& host);

}  // namespace iw_pc::game::weapon
