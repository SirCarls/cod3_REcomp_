//! The weapon's states (`PM_Weapon`'s parts in `bg_weapons.c`): raising,
//! dropping, reloading, melee, the offhand grenade, the binoculars, as Call
//! of Duty 2 has them. The states are numbers ([`super::pmove::ps::WEAPON_STATE`]);
//! their names in Call of Duty 2 aren't known (see [`super::weapon::state`]).

use super::pmove::{
    add_predictable_event, button, cmd, pm, pm_flags, pm_type, pml, ps, MoveLocal, MoveSettings,
    PlayerState, Pmove,
};
use super::trace::MoveHost;
use super::weapon::{binocular_weapon_index, def, WeaponDef};

/// The bit an animation number flips when the same one starts again
/// (`ANIM_TOGGLEBIT`).
const ANIM_TOGGLE_BIT: i32 = 0x200;

/// The weapon's animations the states start (by number; our names).
pub mod weap_anim {
    pub const IDLE: i32 = 0;
    pub const QUICK_DROP: i32 = 0x11;
    pub const QUICK_RAISE: i32 = 0x12;
    pub const OFFHAND_PREPARE: i32 = 0x13;
}

/// The events the states add (Call of Duty 2's numbers; our names).
pub mod ev {
    /// The sights left (as [`super::super::pmove::EV_ADS_EXIT`]).
    pub const ADS_EXIT: i32 = 0x95;
    /// No offhand to throw.
    pub const OFFHAND_EMPTY: i32 = 0x94;
    /// A melee swing.
    pub const MELEE: i32 = 0xA4;
    /// The offhand grenade readied (its weapon the parameter).
    pub const OFFHAND_PREPARE: i32 = 0xA5;
    /// The offhand switched (the new one the parameter).
    pub const OFFHAND_SWITCH: i32 = 0xA7;
    /// The binoculars up, and away.
    pub const BINOCULARS_RAISE: i32 = 0xA8;
    pub const BINOCULARS_DROP: i32 = 0xA9;
    /// The binoculars lowered from the eye.
    pub const BINOCULARS_LOWER: i32 = 0xAC;
    /// A cooked grenade gone off in hand.
    pub const GRENADE_COOKED_OFF: i32 = 0xC5;
}

/// The weapon kind of grenades (`WEAPTYPE_GRENADE`).
pub const WEAPTYPE_GRENADE: i32 = 1;

/// The weapon's animation started: unless dead, `anim` with the toggle bit
/// flipped.
///
/// Original: `PM_StartWeaponAnim` at 0x004F1A80 (CoD2MP_s.exe 1.3; Call of Duty 4's name; ps in eax, the animation in edx)
pub fn start_weapon_anim(ps: &mut PlayerState, anim: i32) {
    if ps.pm_type() < pm_type::DEAD {
        let a = ps.int(ps::WEAP_ANIM);
        ps.set_int(ps::WEAP_ANIM, (!a & ANIM_TOGGLE_BIT) | anim);
    }
}

/// [`start_weapon_anim`] unless `anim` is already playing.
///
/// Original: `PM_ContinueWeaponAnim` at 0x004F1AA0 (CoD2MP_s.exe 1.3; Call of Duty 4's name; ps in ecx, the animation in edx)
pub fn continue_weapon_anim(ps: &mut PlayerState, anim: i32) {
    if ps.int(ps::WEAP_ANIM) & !ANIM_TOGGLE_BIT != anim {
        start_weapon_anim(ps, anim);
    }
}

fn def_of<'a>(host: &'a dyn MoveHost, ps: &PlayerState, at: usize) -> &'a WeaponDef {
    host.weapon_def(ps.int(at))
}

/// Where word `index` of the array at `base` is in the player state, if
/// it is in it (the original reads and writes past the arrays for numbers
/// outside them; the port reads 0 there and writes nothing).
fn word_at(base: usize, index: i32) -> Option<usize> {
    let at = base as i64 + i64::from(index) * 4;
    usize::try_from(at)
        .ok()
        .filter(|&a| a + 4 <= super::pmove::PLAYER_STATE_SIZE)
}

fn word(ps: &PlayerState, base: usize, index: i32) -> i32 {
    word_at(base, index).map_or(0, |a| ps.int(a))
}

/// Whether the player holds weapon `weapon` (its bit in [`ps::WEAPONS`]).
fn holds(ps: &PlayerState, weapon: i32) -> bool {
    word(ps, ps::WEAPONS, weapon >> 5) & 1 << (weapon & 31) != 0
}

/// The weapon in hand's ammunition in its clip.
///
/// Original: `PM_WeaponAmmoInClip` at 0x004F2DD0 (CoD2MP_s.exe 1.3; our name; ps in eax, the result in eax)
pub fn ammo_in_clip(ps: &PlayerState, host: &dyn MoveHost) -> i32 {
    let clip = def_of(host, ps, ps::WEAPON).int(def::CLIP_INDEX);
    word(ps, ps::AMMO_CLIP, clip)
}

/// The first weapon (1 to the highest number) of offhand class `class`
/// the player holds with ammunition left (in reserve or the clip); 0 for
/// none.
///
/// Original: `BG_GetFirstOffhandOfClass` at 0x004F1DA0 (CoD2MP_s.exe 1.3; our name; ps in edx, the class in edi)
pub fn first_offhand_of_class(ps: &PlayerState, class: i32, host: &dyn MoveHost) -> i32 {
    let count = host.weapon_count();
    let mut weapon = 1;
    while weapon <= count {
        let d = host.weapon_def(weapon);
        if d.int(def::OFFHAND_CLASS) == class && holds(ps, weapon) {
            let ammo = word(ps, ps::AMMO_CLIP, d.int(def::CLIP_INDEX));
            let reserve = word(ps, ps::AMMO, d.int(def::AMMO_INDEX));
            if ammo.wrapping_add(reserve) != 0 {
                return weapon;
            }
        }
        weapon += 1;
    }
    0
}

/// Whether the player may use weapon `weapon`: a weapon number (0 to the
/// highest), held, and an offhand, one of his two primaries, or another's
/// alternate (the definition's `altWeaponIndex`).
///
/// Original: `BG_IsWeaponValid` at 0x004F1EE0 (CoD2MP_s.exe 1.3; Call of Duty 4's name; ps in eax, the weapon in edx; al)
pub fn is_weapon_valid(ps: &PlayerState, weapon: i32, host: &dyn MoveHost) -> bool {
    let mut valid = (0..=host.weapon_count()).contains(&weapon);
    if !holds(ps, weapon) {
        valid = false;
    }
    let d = host.weapon_def(weapon);
    if d.int(def::OFFHAND_CLASS) == 0 {
        let primary = |k: usize| i32::from(ps.bytes[ps::PRIMARY_WEAPONS + k] as i8);
        if primary(0) != weapon && primary(1) != weapon && d.int(def::ALT_WEAPON_INDEX) != weapon {
            valid = false;
        }
    }
    valid
}

/// A raise finished: ready, idle.
///
/// Original: `PM_Weapon_FinishWeaponRaise` at 0x004F3840 (CoD2MP_s.exe 1.3; Call of Duty 4's name; ps in eax)
pub fn finish_weapon_raise(ps: &mut PlayerState) {
    ps.set_int(ps::WEAPON_STATE, 0);
    start_weapon_anim(ps, weap_anim::IDLE);
}

/// A segmented reload's end finished: ready, idle (the same code as
/// [`finish_weapon_raise`]).
///
/// Original: `PM_Weapon_FinishReloadEnd` at 0x004F3C80 (CoD2MP_s.exe 1.3; Call of Duty 4's name; ps in eax)
pub fn finish_reload_end(ps: &mut PlayerState) {
    ps.set_int(ps::WEAPON_STATE, 0);
    start_weapon_anim(ps, weap_anim::IDLE);
}

/// The bolt worked: ready, idle unless it already is.
///
/// Original: `PM_Weapon_FinishRechamber` at 0x004F2E20 (CoD2MP_s.exe 1.3; Call of Duty 4's name; ps in ecx)
pub fn finish_rechamber(ps: &mut PlayerState) {
    ps.set_int(ps::WEAPON_STATE, 0);
    continue_weapon_anim(ps, weap_anim::IDLE);
}

/// A melee swing over: ready, idle unless it already is (the same code as
/// [`finish_rechamber`]).
///
/// Original: `PM_Weapon_MeleeEnd` at 0x004F48B0 (CoD2MP_s.exe 1.3; Call of Duty 4's name; ps in ecx)
pub fn melee_end(ps: &mut PlayerState) {
    ps.set_int(ps::WEAPON_STATE, 0);
    continue_weapon_anim(ps, weap_anim::IDLE);
}

/// The melee swing: the weapon's time at least `meleeTime - meleeDelay`,
/// [`ev::MELEE`], state 11; lying down, [`pm_flags::ADS_HOLD`].
///
/// Original: `PM_Weapon_MeleeFire` at 0x004F4830 (CoD2MP_s.exe 1.3; Call of Duty 4's name; ps in eax)
pub fn melee_fire(ps: &mut PlayerState, host: &dyn MoveHost) {
    let d = def_of(host, ps, ps::WEAPON);
    let time = d.int(def::MELEE_TIME).wrapping_sub(d.int(def::MELEE_DELAY));
    if ps.int(ps::WEAPON_TIME) < time {
        ps.set_int(ps::WEAPON_TIME, time);
    }
    add_predictable_event(ev::MELEE, 0, ps);
    ps.set_int(ps::WEAPON_STATE, 0xB);
    let flags = ps.pm_flags();
    if flags & pm_flags::PRONE != 0 {
        ps.set_int(ps::PM_FLAGS, flags | pm_flags::ADS_HOLD);
    }
}

/// The weapon goes down for the offhand: state 12 for the weapon's
/// `quickDropTime`, not the offhand yet, the sights left
/// ([`ev::ADS_EXIT`]), the quick drop.
///
/// Original: `PM_Weapon_OffHandInit` at 0x004F4AD0 (CoD2MP_s.exe 1.3; Call of Duty 4's name; ps in eax)
pub fn offhand_init(ps: &mut PlayerState, host: &dyn MoveHost) {
    let time = def_of(host, ps, ps::WEAPON).int(def::QUICK_DROP_TIME);
    ps.set_int(ps::WEAPON_STATE, 0xC);
    ps.set_int(ps::WEAPON_TIME, time);
    ps.set_int(ps::PM_FLAGS, ps.pm_flags() & !pm_flags::OFFHAND);
    ps.set_int(ps::WEAPON_DELAY, 0);
    add_predictable_event(ev::ADS_EXIT, 0, ps);
    ps.set_int(ps::PM_FLAGS, ps.pm_flags() & !pm_flags::ADS);
    start_weapon_anim(ps, weap_anim::QUICK_DROP);
}

/// The offhand readied: state 13 for its `holdFireTime`, the offhand in
/// hand, [`ev::OFFHAND_PREPARE`], its animation; lying down,
/// [`pm_flags::ADS_HOLD`].
///
/// Original: `PM_Weapon_OffHandPrepare` at 0x004F4B60 (CoD2MP_s.exe 1.3; Call of Duty 4's name; ps in eax)
pub fn offhand_prepare(ps: &mut PlayerState, host: &dyn MoveHost) {
    let offhand = ps.int(ps::OFFHAND_INDEX);
    let time = host.weapon_def(offhand).int(def::HOLD_FIRE_TIME);
    ps.set_int(ps::WEAPON_STATE, 0xD);
    ps.set_int(ps::WEAPON_TIME, time);
    ps.set_int(ps::PM_FLAGS, ps.pm_flags() | pm_flags::OFFHAND);
    ps.set_int(ps::WEAPON_DELAY, 0);
    add_predictable_event(ev::OFFHAND_PREPARE, offhand & 0xFF, ps);
    start_weapon_anim(ps, weap_anim::OFFHAND_PREPARE);
    let flags = ps.pm_flags();
    if flags & pm_flags::PRONE != 0 {
        ps.set_int(ps::PM_FLAGS, flags | pm_flags::ADS_HOLD);
    }
}

/// The offhand held: state 14, its fuse ([`ps::GRENADE_TIME_LEFT`]) from
/// its `fuseTime`.
///
/// Original: `PM_Weapon_OffHandHold` at 0x004F4BF0 (CoD2MP_s.exe 1.3; Call of Duty 4's name; ps in eax)
pub fn offhand_hold(ps: &mut PlayerState, host: &dyn MoveHost) {
    let fuse = def_of(host, ps, ps::OFFHAND_INDEX).int(def::FUSE_TIME);
    ps.set_int(ps::WEAPON_TIME, 0);
    ps.set_int(ps::WEAPON_DELAY, 0);
    ps.set_int(ps::PM_FLAGS, ps.pm_flags() | pm_flags::OFFHAND);
    ps.set_int(ps::WEAPON_STATE, 0xE);
    ps.set_int(ps::GRENADE_TIME_LEFT, fuse);
}

/// The offhand thrown: the weapon comes back up (its `quickRaiseTime`;
/// none in hand: a moment's delay), state 16, the offhand bits cleared.
///
/// Original: `PM_Weapon_OffHandEnd` at 0x004F4DA0 (CoD2MP_s.exe 1.3; Call of Duty 4's name; ps in eax)
pub fn offhand_end(ps: &mut PlayerState, host: &dyn MoveHost) {
    raise_after(ps, host);
    let flags = ps.pm_flags() & !(pm_flags::ADS_HOLD | pm_flags::OFFHAND);
    ps.set_int(ps::WEAPON_STATE, 0x10);
    ps.set_int(ps::PM_FLAGS, flags);
}

/// No weapon: no time and a delay of 1; else the weapon's `quickRaiseTime`
/// and the quick raise.
fn raise_after(ps: &mut PlayerState, host: &dyn MoveHost) {
    if ps.int(ps::WEAPON) == 0 {
        ps.set_int(ps::WEAPON_TIME, 0);
        ps.set_int(ps::WEAPON_DELAY, 1);
    } else {
        let time = def_of(host, ps, ps::WEAPON).int(def::QUICK_RAISE_TIME);
        ps.set_int(ps::WEAPON_TIME, time);
        ps.set_int(ps::WEAPON_DELAY, 0);
        start_weapon_anim(ps, weap_anim::QUICK_RAISE);
    }
}

/// [`ev::OFFHAND_EMPTY`]: no offhand to throw.
///
/// Original: `PM_Weapon_OffHandEmpty` at 0x004F4E00 (CoD2MP_s.exe 1.3; our name; ps in eax)
pub fn offhand_empty(ps: &mut PlayerState) {
    add_predictable_event(ev::OFFHAND_EMPTY, 0, ps);
}

/// The weapon goes down for the binoculars: state 18 for its
/// `quickDropTime`, the quick drop.
///
/// Original: `PM_Binoculars_DropWeapon` at 0x004F5170 (CoD2MP_s.exe 1.3; our name; ps in eax)
pub fn binoculars_drop_weapon(ps: &mut PlayerState, host: &dyn MoveHost) {
    let time = def_of(host, ps, ps::WEAPON).int(def::QUICK_DROP_TIME);
    ps.set_int(ps::WEAPON_STATE, 0x12);
    ps.set_int(ps::WEAPON_TIME, time);
    ps.set_int(ps::WEAPON_DELAY, 0);
    start_weapon_anim(ps, weap_anim::QUICK_DROP);
}

/// The binoculars up: in hand ([`pm_flags::BINOCULARS`]), state 19 for
/// their `adsTransInTime`, [`ev::BINOCULARS_RAISE`], idle.
///
/// Original: `PM_Binoculars_Raise` at 0x004F51C0 (CoD2MP_s.exe 1.3; our name; ps in esi)
pub fn binoculars_raise(ps: &mut PlayerState, host: &dyn MoveHost) {
    let time = host
        .weapon_def(binocular_weapon_index(host))
        .int(def::ADS_TRANS_IN_TIME);
    ps.set_int(ps::PM_FLAGS, ps.pm_flags() | pm_flags::BINOCULARS);
    ps.set_int(ps::WEAPON_STATE, 0x13);
    ps.set_int(ps::WEAPON_TIME, time);
    ps.set_int(ps::WEAPON_DELAY, 0);
    add_predictable_event(ev::BINOCULARS_RAISE, 0, ps);
    start_weapon_anim(ps, weap_anim::IDLE);
}

/// The binoculars down from the eye: state 21 for their `adsTransOutTime`
/// and `player_adsExitDelay`, [`ev::BINOCULARS_LOWER`], idle.
///
/// Original: `PM_Binoculars_Lower` at 0x004F5260 (CoD2MP_s.exe 1.3; our name; ps in esi)
pub fn binoculars_lower(ps: &mut PlayerState, settings: &MoveSettings, host: &dyn MoveHost) {
    let d = host.weapon_def(binocular_weapon_index(host));
    ps.set_int(ps::WEAPON_STATE, 0x15);
    let time = d
        .int(def::ADS_TRANS_OUT_TIME)
        .wrapping_add(settings.ads_exit_delay);
    ps.set_int(ps::WEAPON_TIME, time);
    ps.set_int(ps::WEAPON_DELAY, 0);
    add_predictable_event(ev::BINOCULARS_LOWER, 0, ps);
    start_weapon_anim(ps, weap_anim::IDLE);
}

/// The binoculars put away: not in hand, state 22, [`ev::BINOCULARS_DROP`];
/// the weapon the command wants comes back up (none: a moment's delay).
///
/// Original: `PM_Binoculars_DropBinoculars` at 0x004F52E0 (CoD2MP_s.exe 1.3; our name; pm in ecx)
pub fn binoculars_drop(pm: &mut Pmove, host: &dyn MoveHost) {
    let ps = &mut pm.ps;
    ps.set_int(ps::PM_FLAGS, ps.pm_flags() & !pm_flags::BINOCULARS);
    ps.set_int(ps::WEAPON_STATE, 0x16);
    add_predictable_event(ev::BINOCULARS_DROP, 0, ps);
    if pm.bytes[pm::CMD + cmd::WEAPON] == 0 {
        pm.ps.set_int(ps::WEAPON_TIME, 0);
        pm.ps.set_int(ps::WEAPON_DELAY, 1);
    } else {
        let time = def_of(host, &pm.ps, ps::WEAPON).int(def::QUICK_RAISE_TIME);
        pm.ps.set_int(ps::WEAPON_TIME, time);
        pm.ps.set_int(ps::WEAPON_DELAY, 0);
        start_weapon_anim(&mut pm.ps, weap_anim::QUICK_RAISE);
    }
}

/// From the sights to the binoculars: [`ev::ADS_EXIT`], the sights left,
/// state 17 after the weapon's `adsTransOutTime` and
/// `player_adsExitDelay`.
///
/// Original: `PM_Binoculars_ExitSights` at 0x004F5100 (CoD2MP_s.exe 1.3; our name; ps in eax)
pub fn binoculars_exit_sights(ps: &mut PlayerState, settings: &MoveSettings, host: &dyn MoveHost) {
    let out = def_of(host, ps, ps::WEAPON).int(def::ADS_TRANS_OUT_TIME);
    add_predictable_event(ev::ADS_EXIT, 0, ps);
    ps.set_int(ps::PM_FLAGS, ps.pm_flags() & !pm_flags::ADS);
    ps.set_int(ps::WEAPON_TIME, 0);
    ps.set_int(ps::WEAPON_DELAY, out.wrapping_add(settings.ads_exit_delay));
    ps.set_int(ps::WEAPON_STATE, 0x11);
}

/// The binoculars gone at once: the weapon at the hip, [`ev::ADS_EXIT`],
/// neither binoculars nor sights, [`ev::BINOCULARS_DROP`].
///
/// Original: `PM_ExitBinoculars` at 0x004F5090 (CoD2MP_s.exe 1.3; our name; ps in eax)
pub fn exit_binoculars(ps: &mut PlayerState) {
    ps.set_int(ps::WEAPON_POS_FRAC, 0);
    add_predictable_event(ev::ADS_EXIT, 0, ps);
    let flags = ps.pm_flags() & !(pm_flags::BINOCULARS | pm_flags::ADS);
    ps.set_int(ps::PM_FLAGS, flags);
    add_predictable_event(ev::BINOCULARS_DROP, 0, ps);
}

fn reset(ps: &mut PlayerState) {
    ps.set_int(
        ps::PM_FLAGS,
        ps.pm_flags() & !(pm_flags::ADS_HOLD | pm_flags::OFFHAND),
    );
    if (0x11..=0x16).contains(&ps.int(ps::WEAPON_STATE)) {
        exit_binoculars(ps);
    }
    ps.set_int(ps::WEAPON_TIME, 0);
    ps.set_int(ps::WEAPON_DELAY, 0);
    ps.set_int(ps::WEAPON_STATE, 0);
    start_weapon_anim(ps, weap_anim::IDLE);
}

/// The weapon back to ready: the offhand bits cleared, the binoculars gone
/// ([`exit_binoculars`]), no time or delay, idle.
///
/// Original: `PM_ResetWeaponState` at 0x004F4A80 (CoD2MP_s.exe 1.3; Call of Duty 4's name; ps in eax)
pub fn reset_weapon_state(ps: &mut PlayerState) {
    reset(ps);
}

/// The same as [`reset_weapon_state`] (a second copy of its code, which
/// `PmoveSingle` calls while climbing over an obstacle).
///
/// Original: `PM_Weapon_Idle` at 0x004F5810 (CoD2MP_s.exe 1.3; Call of Duty 4's name, by its use; ps in eax)
pub fn weapon_idle(ps: &mut PlayerState) {
    reset(ps);
}

/// Ready when the fire button is up and nothing waits
/// (`delayed_action` 0): leaving the firing state, idle unless already;
/// the result 1. Else 0.
///
/// Original: `PM_Weapon_FireButtonReleased` at 0x004F4360 (CoD2MP_s.exe 1.3; our name; pm in eax, delayedAction on the stack; eax)
pub fn fire_button_released(pm: &mut Pmove, delayed_action: i32) -> i32 {
    if pm.bytes[pm::CMD + cmd::BUTTONS] as i32 & button::ATTACK != 0 || delayed_action != 0 {
        return 0;
    }
    let ps = &mut pm.ps;
    if ps.int(ps::WEAPON_STATE) == super::weapon::state::FIRING {
        continue_weapon_anim(ps, weap_anim::IDLE);
    }
    ps.set_int(ps::WEAPON_STATE, 0);
    1
}

/// A cooking grenade in the offhand counts down by the move's
/// milliseconds; at 0 it goes off in hand: one less in the clip,
/// [`ev::GRENADE_COOKED_OFF`], true.
///
/// Original: `PM_Weapon_GrenadeCookOff` at 0x004F4F70 (CoD2MP_s.exe 1.3; our name; ps in eax, pml on the stack; al)
pub fn grenade_cook_off(ps: &mut PlayerState, pml: &MoveLocal, host: &dyn MoveHost) -> bool {
    if ps.pm_flags() & pm_flags::OFFHAND == 0 {
        return false;
    }
    let d = def_of(host, ps, ps::OFFHAND_INDEX);
    if d.int(def::WEAP_TYPE) != WEAPTYPE_GRENADE || ps.int(ps::GRENADE_TIME_LEFT) <= 0 {
        return false;
    }
    if d.int(def::COOK_OFF_HOLD) != 0 {
        let left = ps
            .int(ps::GRENADE_TIME_LEFT)
            .wrapping_sub(pml.int(pml::MSEC));
        ps.set_int(ps::GRENADE_TIME_LEFT, left);
    }
    if ps.int(ps::GRENADE_TIME_LEFT) > 0 {
        return false;
    }
    if let Some(at) = word_at(ps::AMMO_CLIP, d.int(def::CLIP_INDEX)) {
        ps.set_int(at, ps.int(at).wrapping_sub(1));
    }
    add_predictable_event(ev::GRENADE_COOKED_OFF, 0, ps);
    true
}

/// Whether the weapon lets a prone player crawl: raising, dropping,
/// reloading, working the bolt or ready, 1; firing, melee or the offhand,
/// 0; anything else (the binoculars) is reset ([`reset_weapon_state`]),
/// 1.
///
/// Original: `PM_Weapon_ResetUnlessBusy` at 0x004F2C40 (CoD2MP_s.exe 1.3; our name; ps in eax, the result in eax)
pub fn reset_unless_busy(ps: &mut PlayerState) -> i32 {
    match ps.int(ps::WEAPON_STATE) {
        0..=2 | 4..=9 => 1,
        3 | 0xB..=0x10 => 0,
        _ => {
            reset_weapon_state(ps);
            1
        }
    }
}
