// The weapon's states (PM_Weapon's parts in bg_weapons.c): raising, dropping, reloading, melee,
// the offhand grenade, the binoculars, Call of Duty 2 (see the Rust version,
// iw_pc::game::weapon_state).
#pragma once

#include <cstdint>

#include "iw_pc/game/pmove.h"
#include "iw_pc/game/trace.h"

namespace iw_pc::game::weapon_state {

/// The weapon's animations the states start (by number; our names).
namespace weap_anim {
inline constexpr std::int32_t kIdle = 0, kQuickDrop = 0x11, kQuickRaise = 0x12, kOffhandPrepare = 0x13;
}
/// The events the states add (Call of Duty 2's numbers; our names).
namespace ev {
inline constexpr std::int32_t kAdsExit = 0x95, kOffhandEmpty = 0x94, kMelee = 0xA4, kOffhandPrepare = 0xA5,
                              kOffhandSwitch = 0xA7, kBinocularsRaise = 0xA8, kBinocularsDrop = 0xA9,
                              kBinocularsLower = 0xAC, kGrenadeCookedOff = 0xC5;
}
/// The weapon kind of grenades (WEAPTYPE_GRENADE).
inline constexpr std::int32_t kWeaptypeGrenade = 1;

/// Original: PM_StartWeaponAnim at 0x004F1A80 (CoD2MP_s.exe 1.3; Call of Duty 4's name; ps in eax, the animation in edx)
void start_weapon_anim(pmove::PlayerState& ps, std::int32_t anim);
/// Original: PM_ContinueWeaponAnim at 0x004F1AA0 (CoD2MP_s.exe 1.3; Call of Duty 4's name; ps in ecx, the animation in edx)
void continue_weapon_anim(pmove::PlayerState& ps, std::int32_t anim);
/// Original: PM_WeaponAmmoInClip at 0x004F2DD0 (CoD2MP_s.exe 1.3; our name; ps in eax, the result in eax)
std::int32_t ammo_in_clip(const pmove::PlayerState& ps, const trace::MoveHost& host);
/// Original: BG_GetFirstOffhandOfClass at 0x004F1DA0 (CoD2MP_s.exe 1.3; our name; ps in edx, the class in edi)
std::int32_t first_offhand_of_class(const pmove::PlayerState& ps, std::int32_t cls, const trace::MoveHost& host);
/// Original: BG_IsWeaponValid at 0x004F1EE0 (CoD2MP_s.exe 1.3; Call of Duty 4's name; ps in eax, the weapon in edx; al)
bool is_weapon_valid(const pmove::PlayerState& ps, std::int32_t weapon, const trace::MoveHost& host);
/// Original: PM_Weapon_FinishWeaponRaise at 0x004F3840 (CoD2MP_s.exe 1.3; Call of Duty 4's name; ps in eax)
void finish_weapon_raise(pmove::PlayerState& ps);
/// Original: PM_Weapon_FinishReloadEnd at 0x004F3C80 (CoD2MP_s.exe 1.3; Call of Duty 4's name; ps in eax)
void finish_reload_end(pmove::PlayerState& ps);
/// Original: PM_Weapon_FinishRechamber at 0x004F2E20 (CoD2MP_s.exe 1.3; Call of Duty 4's name; ps in ecx)
void finish_rechamber(pmove::PlayerState& ps);
/// Original: PM_Weapon_MeleeEnd at 0x004F48B0 (CoD2MP_s.exe 1.3; Call of Duty 4's name; ps in ecx)
void melee_end(pmove::PlayerState& ps);
/// Original: PM_Weapon_MeleeFire at 0x004F4830 (CoD2MP_s.exe 1.3; Call of Duty 4's name; ps in eax)
void melee_fire(pmove::PlayerState& ps, const trace::MoveHost& host);
/// Original: PM_Weapon_OffHandInit at 0x004F4AD0 (CoD2MP_s.exe 1.3; Call of Duty 4's name; ps in eax)
void offhand_init(pmove::PlayerState& ps, const trace::MoveHost& host);
/// Original: PM_Weapon_OffHandPrepare at 0x004F4B60 (CoD2MP_s.exe 1.3; Call of Duty 4's name; ps in eax)
void offhand_prepare(pmove::PlayerState& ps, const trace::MoveHost& host);
/// Original: PM_Weapon_OffHandHold at 0x004F4BF0 (CoD2MP_s.exe 1.3; Call of Duty 4's name; ps in eax)
void offhand_hold(pmove::PlayerState& ps, const trace::MoveHost& host);
/// Original: PM_Weapon_OffHandEnd at 0x004F4DA0 (CoD2MP_s.exe 1.3; Call of Duty 4's name; ps in eax)
void offhand_end(pmove::PlayerState& ps, const trace::MoveHost& host);
/// Original: PM_Weapon_OffHandEmpty at 0x004F4E00 (CoD2MP_s.exe 1.3; our name; ps in eax)
void offhand_empty(pmove::PlayerState& ps);
/// Original: PM_Binoculars_DropWeapon at 0x004F5170 (CoD2MP_s.exe 1.3; our name; ps in eax)
void binoculars_drop_weapon(pmove::PlayerState& ps, const trace::MoveHost& host);
/// Original: PM_Binoculars_Raise at 0x004F51C0 (CoD2MP_s.exe 1.3; our name; ps in esi)
void binoculars_raise(pmove::PlayerState& ps, const trace::MoveHost& host);
/// Original: PM_Binoculars_Lower at 0x004F5260 (CoD2MP_s.exe 1.3; our name; ps in esi)
void binoculars_lower(pmove::PlayerState& ps, const pmove::MoveSettings& settings, const trace::MoveHost& host);
/// Original: PM_Binoculars_DropBinoculars at 0x004F52E0 (CoD2MP_s.exe 1.3; our name; pm in ecx)
void binoculars_drop(pmove::Pmove& pm, const trace::MoveHost& host);
/// Original: PM_Binoculars_ExitSights at 0x004F5100 (CoD2MP_s.exe 1.3; our name; ps in eax)
void binoculars_exit_sights(pmove::PlayerState& ps, const pmove::MoveSettings& settings, const trace::MoveHost& host);
/// Original: PM_ExitBinoculars at 0x004F5090 (CoD2MP_s.exe 1.3; our name; ps in eax)
void exit_binoculars(pmove::PlayerState& ps);
/// Original: PM_ResetWeaponState at 0x004F4A80 (CoD2MP_s.exe 1.3; Call of Duty 4's name; ps in eax)
void reset_weapon_state(pmove::PlayerState& ps);
/// Original: PM_Weapon_Idle at 0x004F5810 (CoD2MP_s.exe 1.3; Call of Duty 4's name, by its use; ps in eax)
void weapon_idle(pmove::PlayerState& ps);
/// Original: PM_Weapon_FireButtonReleased at 0x004F4360 (CoD2MP_s.exe 1.3; our name; pm in eax, delayedAction on the stack; eax)
std::int32_t fire_button_released(pmove::Pmove& pm, std::int32_t delayed_action);
/// Original: PM_Weapon_GrenadeCookOff at 0x004F4F70 (CoD2MP_s.exe 1.3; our name; ps in eax, pml on the stack; al)
bool grenade_cook_off(pmove::PlayerState& ps, const pmove::MoveLocal& pml, const trace::MoveHost& host);
/// Original: PM_Weapon_ResetUnlessBusy at 0x004F2C40 (CoD2MP_s.exe 1.3; our name; ps in eax, the result in eax)
std::int32_t reset_unless_busy(pmove::PlayerState& ps);

}  // namespace iw_pc::game::weapon_state
