//! The weapon's part of the movement (`bg_weapons.c`): aiming down the
//! sights and the aim's spread, as Call of Duty 2 has them. The weapons'
//! definitions (`WeaponDef`) are the host's
//! ([`super::trace::MoveHost::weapon_def`]): the loader of the weapon
//! files isn't ported yet.

use std::cmp::Ordering;

use super::anim::{self, ANIM_COND_WEAPON_POSITION};
use super::pmove::{
    button, cmd, exit_aim_down_sight, pm, pm_flags, pml, ps, MoveLocal, MoveSettings, PlayerState,
    Pmove, ENTITY_NONE,
};
use super::trace::MoveHost;
use crate::net::msg::ftol2;
use crate::shared::math::{angle_subtract, diff_track, i_fclamp};
use crate::x87::F24;

/// Bytes of Call of Duty 2's `WeaponDef` (what the loader allocates for
/// each).
pub const WEAPON_DEF_SIZE: usize = 0x604;

/// Where a weapon's definition keeps the fields the movement reads
/// (offsets in Call of Duty 2's `WeaponDef`; Call of Duty 4's names where
/// the use matches).
pub mod def {
    /// The player's animations' kind (`playerAnimType`), the weapon's kind
    /// (`weapType`: [`super::WEAPTYPE_BINOCULARS`] ...) and class
    /// (`weapClass`).
    pub const PLAYER_ANIM_TYPE: usize = 0x74;
    pub const WEAP_TYPE: usize = 0x78;
    pub const WEAP_CLASS: usize = 0x7C;
    /// The offhand's class (`offhandClass`; 0: not an offhand).
    pub const OFFHAND_CLASS: usize = 0x84;
    /// The weapon's ammunition and clip numbers (`iAmmoIndex`,
    /// `iClipIndex`: words of [`super::super::pmove::ps::AMMO`] and
    /// [`super::super::pmove::ps::AMMO_CLIP`]).
    pub const AMMO_INDEX: usize = 0x1C8;
    pub const CLIP_INDEX: usize = 0x1D0;
    /// Times (milliseconds): the melee's delay, how long an offhand is
    /// readied (`iHoldFireTime`), the melee, the quick drop and raise
    /// (offhand, binoculars), a grenade's fuse, the binoculars' or sights'
    /// way in and out (`iAdsTransInTime`, `iAdsTransOutTime`).
    pub const MELEE_DELAY: usize = 0x200;
    pub const HOLD_FIRE_TIME: usize = 0x210;
    pub const MELEE_TIME: usize = 0x214;
    pub const QUICK_DROP_TIME: usize = 0x240;
    pub const QUICK_RAISE_TIME: usize = 0x244;
    pub const FUSE_TIME: usize = 0x248;
    pub const ADS_TRANS_IN_TIME: usize = 0x2C0;
    pub const ADS_TRANS_OUT_TIME: usize = 0x2C4;
    /// A grenade that cooks while held (`cookOffHold`).
    pub const COOK_OFF_HOLD: usize = 0x33C;
    /// The weapon's alternate (`altWeaponIndex`).
    pub const ALT_WEAPON_INDEX: usize = 0x36C;
    /// How fast its holder moves (`moveSpeedScale`).
    pub const MOVE_SPEED_SCALE: usize = 0x264;
    /// A scope's reticle (`overlayReticle`; 0: none).
    pub const OVERLAY_RETICLE: usize = 0x278;
    /// The hip aim's spread: how fast it settles (`hipSpreadDecayRate`),
    /// what turning and moving add (`hipSpreadTurnAdd`,
    /// `hipSpreadMoveAdd`), and how much faster it settles crouching and
    /// lying down (`hipSpreadDuckedDecay`, `hipSpreadProneDecay`).
    pub const HIP_SPREAD_DECAY_RATE: usize = 0x2A4;
    pub const HIP_SPREAD_TURN_ADD: usize = 0x2AC;
    pub const HIP_SPREAD_MOVE_ADD: usize = 0x2B0;
    pub const HIP_SPREAD_DUCKED_DECAY: usize = 0x2B4;
    pub const HIP_SPREAD_PRONE_DECAY: usize = 0x2B8;
    /// The weapon has sights to aim down (`aimDownSight`).
    pub const AIM_DOWN_SIGHT: usize = 0x32C;
    /// The sights stay up while the bolt is worked (`rechamberWhileAds`).
    pub const RECHAMBER_WHILE_ADS: usize = 0x330;
    /// Firing brings the sights up (`adsFire`).
    pub const ADS_FIRE: usize = 0x348;
    /// The weapon reloads a round at a time (`bSegmentedReload`).
    pub const SEGMENTED_RELOAD: usize = 0x35C;
    /// The reload's time left after which the sights may come back up (our
    /// name; milliseconds).
    pub const RELOAD_ADS_TIME: usize = 0x524;
    /// How far the weapon goes up to the sights, and down from them, in a
    /// millisecond (`fOOPosAnimLength`: one over the times).
    pub const OO_POS_ANIM_LENGTH_IN: usize = 0x588;
    pub const OO_POS_ANIM_LENGTH_OUT: usize = 0x58C;
}

/// The weapon kind of the binoculars (Call of Duty 2's
/// `WEAPTYPE_BINOCULARS`).
pub const WEAPTYPE_BINOCULARS: i32 = 3;

/// The weapon states (`weaponstate`) the sights test, by number (Call of
/// Duty 2's names aren't known): 1 and 2 raising and dropping the weapon, 3
/// firing, 4 working the bolt, 5 to 9 reloading (5 a whole reload, 9 a
/// segmented one's end), 10 and 11, 12 to 16 and 17 to 22 (the binoculars';
/// 19 and 20 up to the eye).
pub mod state {
    pub const FIRING: i32 = 3;
    pub const RECHAMBERING: i32 = 4;
    pub const RELOADING: i32 = 5;
    pub const RELOAD_END: i32 = 9;
}

/// A weapon's definition: the game's structure as its bytes.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct WeaponDef {
    pub bytes: Vec<u8>,
}

impl Default for WeaponDef {
    fn default() -> Self {
        WeaponDef {
            bytes: vec![0; WEAPON_DEF_SIZE],
        }
    }
}

impl WeaponDef {
    /// The definition from its bytes (zeros after the end of a shorter
    /// slice).
    pub fn from_bytes(b: &[u8]) -> Self {
        let mut d = WeaponDef::default();
        let n = b.len().min(WEAPON_DEF_SIZE);
        d.bytes[..n].copy_from_slice(&b[..n]);
        d
    }

    pub fn int(&self, at: usize) -> i32 {
        let b = &self.bytes[at..at + 4];
        i32::from_le_bytes([b[0], b[1], b[2], b[3]])
    }

    pub fn float(&self, at: usize) -> f32 {
        f32::from_bits(self.int(at) as u32)
    }
}

/// The first weapon (from 1 to the host's [`MoveHost::weapon_count`])
/// that is the binoculars ([`WEAPTYPE_BINOCULARS`]), as a byte; 0 for
/// none.
///
/// Original: `BG_GetBinocularWeaponIndex` at 0x004F1D30 (CoD2MP_s.exe 1.3; our name; the result in al)
pub fn binocular_weapon_index(host: &dyn MoveHost) -> i32 {
    let count = host.weapon_count();
    let mut weapon = 1;
    while weapon <= count {
        if host.weapon_def(weapon).int(def::WEAP_TYPE) == WEAPTYPE_BINOCULARS {
            return weapon & 0xFF;
        }
        weapon += 1;
    }
    0
}

/// Whether weapon state `state` holds the binoculars up to the eye (19 or
/// 20).
///
/// Original: `PM_IsBinocularsUp` at 0x004F2620 (CoD2MP_s.exe 1.3; our name; the state in eax)
pub fn binoculars_up(state: i32) -> bool {
    state == 0x13 || state == 0x14
}

/// The weapon in hand as the sights and the animations see it: the
/// binoculars ([`pm_flags::BINOCULARS`]), the offhand
/// ([`pm_flags::OFFHAND`]), else [`ps::WEAPON`]. (Inlined wherever the
/// original needs it.)
pub fn view_weapon(ps: &PlayerState, host: &dyn MoveHost) -> i32 {
    let flags = ps.pm_flags();
    if flags & pm_flags::BINOCULARS != 0 {
        binocular_weapon_index(host)
    } else if flags & pm_flags::OFFHAND != 0 {
        ps.int(ps::OFFHAND_INDEX)
    } else {
        ps.int(ps::WEAPON)
    }
}

/// Whether the player may aim down the sights: not in `pm_type` 2 to 7
/// (in 1, not with the move's almost-ground plane), the weapon in hand
/// has sights, and its state isn't raising, dropping, 10 to 16, or the
/// binoculars' but up to the eye.
///
/// Original: `PM_IsAdsAllowed` at 0x004F2640 (CoD2MP_s.exe 1.3; Call of Duty 4's name; ps in esi, pml on the stack; al)
pub fn is_ads_allowed(ps: &PlayerState, pml: &MoveLocal, host: &dyn MoveHost) -> bool {
    match ps.pm_type() {
        1 => {
            if pml.int(pml::ALMOST_GROUND_PLANE) != 0 {
                return false;
            }
        }
        2..=7 => return false,
        _ => {}
    }
    let weapon = view_weapon(ps, host);
    if host.weapon_def(weapon).int(def::AIM_DOWN_SIGHT) == 0 {
        return false;
    }
    let state = ps.int(ps::WEAPON_STATE);
    if (0xC..=0x10).contains(&state) || matches!(state, 0xA | 0xB | 1 | 2) {
        return false;
    }
    !(0x11..=0x16).contains(&state) || binoculars_up(state)
}

/// [`pm_flags::ADS`] for this move: with the aim button held
/// ([`button::ADS`]) or the binoculars up to the eye, where
/// [`is_ads_allowed`]; lying down, only with the button newly pressed or
/// not moving, and then with [`pm_flags::ADS_HOLD`]. The animations'
/// weapon position condition follows.
///
/// Original: `PM_UpdateAimDownSightFlag` at 0x004F26F0 (CoD2MP_s.exe 1.3; Call of Duty 4's name; pm in edi, pml in eax)
pub fn update_aim_down_sight_flag(pm: &mut Pmove, pml: &MoveLocal, host: &mut dyn MoveHost) {
    let flags = pm.ps.pm_flags() & !pm_flags::ADS;
    pm.ps.set_int(ps::PM_FLAGS, flags);
    let allowed = is_ads_allowed(&pm.ps, pml, host);
    let held = pm.int(pm::CMD + cmd::BUTTONS) & button::ADS != 0;
    let state = pm.ps.int(ps::WEAPON_STATE);
    if (held || state == 0x13 || state == 0x14) && allowed {
        if flags & pm_flags::PRONE != 0 {
            let old = pm.int(pm::OLD_CMD + cmd::BUTTONS);
            let c = pm.cmd();
            if old & button::ADS == 0 || (c[cmd::FORWARD_MOVE] == 0 && c[cmd::RIGHT_MOVE] == 0) {
                pm.ps
                    .set_int(ps::PM_FLAGS, flags | pm_flags::ADS | pm_flags::ADS_HOLD);
            }
        } else {
            pm.ps.set_int(ps::PM_FLAGS, flags | pm_flags::ADS);
        }
    }
    let ads = i32::from(pm.ps.pm_flags() & pm_flags::ADS != 0);
    let info = host.client_anim_info(pm.ps.int(ps::CLIENT_NUM));
    anim::update_condition_value(info, ANIM_COND_WEAPON_POSITION, ads, true);
}

/// Whether the sights are wanted this move (see
/// [`update_aim_down_sight_lerp`]).
fn sights_wanted(ps: &PlayerState, d: &WeaponDef, flags: i32) -> bool {
    let state = ps.int(ps::WEAPON_STATE);
    let reload_left = ps
        .int(ps::WEAPON_TIME)
        .wrapping_sub(d.int(def::RELOAD_ADS_TIME));
    let blocked = if d.int(def::SEGMENTED_RELOAD) == 0 {
        state == state::RELOADING && reload_left > 0
    } else {
        (state::RELOADING..=8).contains(&state) || (state == state::RELOAD_END && reload_left > 0)
    };
    let mut wanted = !blocked
        && !(d.int(def::RECHAMBER_WHILE_ADS) == 0 && state == state::RECHAMBERING)
        && flags & pm_flags::ADS != 0;
    if d.int(def::ADS_FIRE) != 0 && ps.int(ps::WEAPON_DELAY) != 0 && state == state::FIRING {
        wanted = true;
    }
    wanted
}

/// The weapon on its way to the sights or back ([`ps::WEAPON_POS_FRAC`]):
/// hurt with `player_scopeExitOnDamage` and a scope, the sights are left
/// at once ([`exit_aim_down_sight`]); a weapon without sights stays at the
/// hip. Wanted (aiming, not reloading or working the bolt where the
/// weapon forbids it, or firing an `adsFire` weapon) it goes up by the
/// weapon's rate times the move's milliseconds, else down; once up, a
/// let-go aim keeps the sights for `player_adsExitDelay`. Kept in 0 to 1
/// ([`i_fclamp`]).
///
/// Original: `PM_UpdateAimDownSightLerp` at 0x004F2820 (CoD2MP_s.exe 1.3; Call of Duty 4's name; pm, pml on the stack)
pub fn update_aim_down_sight_lerp(
    pm: &mut Pmove,
    pml: &MoveLocal,
    settings: &MoveSettings,
    host: &dyn MoveHost,
) {
    let l = F24::load;
    let flags = pm.ps.pm_flags();
    let now = pm.server_time();
    let weapon = view_weapon(&pm.ps, host);
    let d = host.weapon_def(weapon);
    let ps = &mut pm.ps;
    if settings.scope_exit_on_damage
        && ps.int(ps::DAMAGE_COUNT) != 0
        && d.int(def::OVERLAY_RETICLE) != 0
    {
        exit_aim_down_sight(ps);
        ps.set_int(ps::WEAPON_POS_FRAC, 0);
        ps.set_int(ps::ADS_DELAY_TIME, 0);
        return;
    }
    if d.int(def::AIM_DOWN_SIGHT) == 0 {
        ps.set_int(ps::WEAPON_POS_FRAC, 0);
        ps.set_int(ps::ADS_DELAY_TIME, 0);
        return;
    }
    let mut up = sights_wanted(ps, d, flags);
    let one = 1f32.to_bits() as i32;
    // (The fraction's bits against 1.0's, as integers.)
    let at_sights = ps.int(ps::WEAPON_POS_FRAC) == one;
    let delay = settings.ads_exit_delay;
    if at_sights && !up && delay > 0 {
        if ps.int(ps::ADS_DELAY_TIME) == 0 {
            ps.set_int(ps::ADS_DELAY_TIME, now.wrapping_add(delay));
        }
        if ps.int(ps::ADS_DELAY_TIME) > now {
            up = true;
        } else {
            ps.set_int(ps::ADS_DELAY_TIME, 0);
        }
    } else {
        ps.set_int(ps::ADS_DELAY_TIME, 0);
    }
    if up {
        if ps.int(ps::WEAPON_POS_FRAC) == one {
            return;
        }
    // (`fucompp` with 0, jnp: only 0 stays; NaN moves.)
    } else if l(ps.float(ps::WEAPON_POS_FRAC)) == l(0.0) {
        return;
    }
    let msec = F24::int(pml.int(pml::MSEC));
    let frac = l(ps.float(ps::WEAPON_POS_FRAC));
    let moved = if up {
        msec * l(d.float(def::OO_POS_ANIM_LENGTH_IN)) + frac
    } else {
        frac - msec * l(d.float(def::OO_POS_ANIM_LENGTH_OUT))
    };
    ps.set_float(ps::WEAPON_POS_FRAC, moved.store());
    let clamped = i_fclamp(ps.float(ps::WEAPON_POS_FRAC), 0.0, 1.0);
    ps.set_float(ps::WEAPON_POS_FRAC, clamped);
}

/// `SHORT2ANGLE` of a command's angle: an integer times 360 / 65536,
/// stored.
fn short_to_angle(a: i32) -> f32 {
    (F24::int(a) * F24::load(360.0 / 65536.0)).store()
}

/// The aim's spread ([`ps::AIM_SPREAD_SCALE`], 0 to 255) this move: it
/// settles by the weapon's `hipSpreadDecayRate` (half in the air, the
/// weapon's crouched and prone rates) times the frame time; at the hip,
/// turning (the commands' pitch and yaw since the last, times
/// `hipSpreadTurnAdd`), moving faster than
/// `bg_aimSpreadMoveSpeedThreshold` (`hipSpreadMoveAdd`) and being in the
/// air (2.56) spread it, times the frame time. Both times 255. A weapon
/// that doesn't settle loses 255 a move.
///
/// Original: `PM_UpdateAimSpreadScale` at 0x004F3EF0 (CoD2MP_s.exe 1.3; Call of Duty 4's name; pm in ebx, pml on the stack)
pub fn update_aim_spread_scale(
    pm: &mut Pmove,
    pml: &MoveLocal,
    settings: &MoveSettings,
    host: &dyn MoveHost,
) {
    let l = F24::load;
    let d = host.weapon_def(pm.ps.int(ps::WEAPON));
    let frame_time = pml.frame_time();
    let rate = l(d.float(def::HIP_SPREAD_DECAY_RATE));
    let (decay, add) = if rate == l(0.0) {
        (1.0f32, l(0.0))
    } else {
        let in_air = pm.ps.int(ps::GROUND_ENTITY_NUM) == ENTITY_NONE && pm.ps.pm_type() != 1;
        let e_flags = pm.ps.int(ps::E_FLAGS);
        let mut r = rate;
        if in_air {
            r = r * l(0.5);
        } else if e_flags & 8 != 0 {
            r = r * l(d.float(def::HIP_SPREAD_PRONE_DECAY));
        } else if e_flags & 4 != 0 {
            r = r * l(d.float(def::HIP_SPREAD_DUCKED_DECAY));
        }
        let decay = (r * l(frame_time)).store();
        // (The fraction's bits against 1.0's, as integers.)
        if pm.ps.int(ps::WEAPON_POS_FRAC) == 1f32.to_bits() as i32 {
            (decay, l(0.0))
        } else {
            let mut add = l(0.0);
            let turn = d.float(def::HIP_SPREAD_TURN_ADD);
            // (`fucompp` with 0, jnp: only 0 adds nothing; NaN adds.)
            if l(turn) != l(0.0) {
                let per_second = (l(1.0) / l(frame_time)).store();
                let mut sum = 0f32;
                let c = pm.cmd();
                let word = |b: &[u8], at: usize| {
                    i32::from_le_bytes([b[at], b[at + 1], b[at + 2], b[at + 3]])
                };
                let old = &pm.bytes[pm::OLD_CMD..];
                for i in 0..2 {
                    let now = short_to_angle(word(c, cmd::ANGLES + 4 * i));
                    let before = short_to_angle(word(old, cmd::ANGLES + 4 * i));
                    let turned = l(angle_subtract(now, before)).abs();
                    add = turned * l(turn) * l(0.01) * l(per_second) + l(sum);
                    sum = add.store();
                }
            }
            let move_add = d.float(def::HIP_SPREAD_MOVE_ADD);
            // (`fucompp` with 0, jnp: only 0 adds nothing; NaN adds.)
            if l(move_add) != l(0.0) {
                let c = pm.cmd();
                if c[cmd::FORWARD_MOVE] != 0 || c[cmd::RIGHT_MOVE] != 0 {
                    let v = pm.ps.velocity();
                    let speed2 = l(v[0]) * l(v[0]) + l(v[1]) * l(v[1]);
                    let threshold = l(settings.aim_spread_move_speed_threshold);
                    // (`fcompp`, test ah,5, jp: only below adds; NaN doesn't.)
                    if (threshold * threshold).partial_cmp(&speed2) == Some(Ordering::Less) {
                        add = add + l(move_add);
                    }
                }
            }
            if pm.ps.int(ps::GROUND_ENTITY_NUM) == ENTITY_NONE && pm.ps.pm_type() != 1 {
                add = add + l(1.28) + l(1.28);
            }
            (decay, add * l(frame_time))
        }
    };
    let scale = (add - l(decay)) * l(255.0) + l(pm.ps.float(ps::AIM_SPREAD_SCALE));
    pm.ps.set_float(ps::AIM_SPREAD_SCALE, scale.store());
    // (`fcom` 0, test ah,5, jp: only below is raised; `fcomp` 255, test
    // ah,0x41, jne: only above is lowered; NaN stays.)
    if scale < l(0.0) {
        pm.ps.set_float(ps::AIM_SPREAD_SCALE, 0.0);
    } else if scale > l(255.0) {
        pm.ps.set_float(ps::AIM_SPREAD_SCALE, 255.0);
    }
}

/// The breath held through a scope: with the weapon at the sights, a
/// scope (not class 9) and [`button::HOLD_BREATH`], [`pm_flags::HOLD_BREATH`]
/// starts (once the last hold is over); the timer
/// ([`ps::HOLD_BREATH_TIMER`]) counts up while held and down after, held
/// past `player_breath_hold_time` it gasps (`player_breath_gasp_time`
/// more). The sway ([`ps::HOLD_BREATH_SCALE`]) follows: none held, up to
/// `player_breath_gasp_scale` gasping, by the weapon's way to the sights
/// ([`diff_track`] at the hold's or the gasp's rate). No hold time: no
/// hold, a sway of 1.
///
/// Original: `PM_UpdateHoldBreath` at 0x004F2AA0 (CoD2MP_s.exe 1.3; Call of Duty 4's name; pm, pml on the stack)
pub fn update_hold_breath(
    pm: &mut Pmove,
    pml: &MoveLocal,
    settings: &MoveSettings,
    host: &dyn MoveHost,
) {
    let l = F24::load;
    let mut flags = pm.ps.pm_flags();
    let weapon = view_weapon(&pm.ps, host);
    let ms = |s: f32| ftol2((l(s) * l(1000.0)).0) as i32;
    let hold = ms(settings.breath_hold_time);
    let gasp = ms(settings.breath_gasp_time);
    let d = host.weapon_def(weapon);
    let held = pm.int(pm::CMD + cmd::BUTTONS) & button::HOLD_BREATH != 0;
    let ps = &mut pm.ps;
    if hold <= 0 {
        ps.set_int(ps::PM_FLAGS, flags & !pm_flags::HOLD_BREATH);
        ps.set_float(ps::HOLD_BREATH_SCALE, 1.0);
        ps.set_int(ps::HOLD_BREATH_TIMER, 0);
        return;
    }
    // (The fraction's bits against 1.0's, as integers.)
    let at_sights = ps.int(ps::WEAPON_POS_FRAC) == 1f32.to_bits() as i32;
    if at_sights && d.int(def::OVERLAY_RETICLE) != 0 && d.int(def::WEAP_CLASS) != 9 && held {
        if ps.int(ps::HOLD_BREATH_TIMER) == 0 {
            flags |= pm_flags::HOLD_BREATH;
            ps.set_int(ps::PM_FLAGS, flags);
        }
    } else {
        flags &= !pm_flags::HOLD_BREATH;
        ps.set_int(ps::PM_FLAGS, flags);
    }
    let flags = ps.pm_flags();
    let msec = pml.int(pml::MSEC);
    let holding = flags & pm_flags::HOLD_BREATH != 0;
    let timer = if holding {
        ps.int(ps::HOLD_BREATH_TIMER).wrapping_add(msec)
    } else {
        ps.int(ps::HOLD_BREATH_TIMER).wrapping_sub(msec)
    };
    ps.set_int(ps::HOLD_BREATH_TIMER, timer.max(0));
    if holding && ps.int(ps::HOLD_BREATH_TIMER) > hold {
        ps.set_int(ps::HOLD_BREATH_TIMER, gasp.wrapping_add(hold));
        ps.set_int(ps::PM_FLAGS, flags & !pm_flags::HOLD_BREATH);
    }
    let (target, rate) = if ps.pm_flags() & pm_flags::HOLD_BREATH != 0 {
        (l(0.0), settings.breath_hold_lerp)
    } else {
        let part = F24::int(ps.int(ps::HOLD_BREATH_TIMER)) / F24::int(gasp.wrapping_add(hold));
        (
            part * (l(settings.breath_gasp_scale) - l(1.0)) + l(1.0),
            settings.breath_gasp_lerp,
        )
    };
    let goal = ((target - l(1.0)) * l(ps.float(ps::WEAPON_POS_FRAC)) + l(1.0)).store();
    let scale = diff_track(
        goal,
        ps.float(ps::HOLD_BREATH_SCALE),
        rate,
        pml.frame_time(),
    );
    ps.set_float(ps::HOLD_BREATH_SCALE, scale);
}
