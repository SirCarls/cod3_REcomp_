//! Stances (`bg_pmove.c`'s duck check): standing, crouching and lying down
//! as the command's buttons ask and the room allows, the box and the view
//! height each gives, and the view height's way from one to the next.
//! Call of Duty 2's versions; see [`super::pmove`] for the conventions.

use super::jump::jump_activate_slowdown;
use super::pmove::{
    add_predictable_event, button, exit_aim_down_sight, pm, pm_flags, pm_type, pml, ps, MoveLocal,
    MoveSettings, PlayerState, Pmove, CROUCH_VIEW_HEIGHT, ENTITY_NONE, E_FLAGS_MOUNTED,
    PRONE_VIEW_HEIGHT, STAND_VIEW_HEIGHT,
};
use super::prone::{check_prone, ProneAngles, ProneQuery};
use super::slide::step_slide_move;
use super::trace::{
    MoveHost, ANIM_ET_CROUCH_TO_STAND, ANIM_ET_PRONE_TO_CROUCH, ANIM_ET_PRONE_TO_STAND,
    ANIM_ET_STAND_TO_CROUCH, CONTENTS_BODY,
};
use crate::net::msg::ftol2;
use crate::shared::math::{angle_normalize180, pitch_for_yaw_on_normal, vec3_lerp, vec3_normalize};
use crate::x87::F24;

/// The view height of the dead (`DEAD_VIEWHEIGHT`).
pub const DEAD_VIEW_HEIGHT: i32 = 8;

/// The events of a stance change refused: the player stays standing,
/// crouching or lying down (Call of Duty 4's `EV_STANCE_FORCE_*`; Call of
/// Duty 2's numbers). Lying down's parameter is the stance asked for (1
/// standing, 2 crouching), crouching's 1.
pub const EV_STANCE_FORCE_STAND: i32 = 0x8C;
pub const EV_STANCE_FORCE_CROUCH: i32 = 0x8D;
pub const EV_STANCE_FORCE_PRONE: i32 = 0x8E;

/// `eFlags` bits of the stance, for the other players' view (Quake III's
/// `EF_CROUCHING` and our `EF_PRONE`).
pub const E_FLAGS_CROUCHING: i32 = 0x4;
pub const E_FLAGS_PRONE: i32 = 0x8;

/// The heights of the box crouching and lying down (`maxs[2]`); standing
/// it is the player state's own ([`ps::MAXS`]).
pub const CROUCH_MAXS_Z: f32 = 50.0;
pub const PRONE_MAXS_Z: f32 = 30.0;

/// A point of a view height's way (Call of Duty 4's `viewLerpWaypoint_s`):
/// how far along (percent), the view height there, and the position
/// offset (none in Call of Duty 2's tables).
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct ViewLerpWaypoint {
    pub fraction: i32,
    pub view_height: f32,
    pub offset: i32,
}

const fn w(fraction: i32, view_height: f32) -> ViewLerpWaypoint {
    ViewLerpWaypoint {
        fraction,
        view_height,
        offset: 0,
    }
}

/// The view height's ways (the game's tables, each ending in fraction -1):
/// standing to crouching (0x005DB630), crouching to standing (0x005DB6A0),
/// crouching to lying down (0x005DB710), lying down to crouching
/// (0x005DB7B8). The way down to lying down dips below and comes back.
pub const STAND_TO_CROUCH: [ViewLerpWaypoint; 9] = [
    w(0, 60.0),
    w(1, 59.5),
    w(4, 58.5),
    w(30, 56.0),
    w(80, 44.0),
    w(90, 41.5),
    w(95, 40.5),
    w(100, 40.0),
    w(-1, 0.0),
];
pub const CROUCH_TO_STAND: [ViewLerpWaypoint; 9] = [
    w(0, 40.0),
    w(5, 40.5),
    w(10, 41.5),
    w(20, 44.0),
    w(70, 56.0),
    w(96, 58.5),
    w(99, 59.5),
    w(100, 60.0),
    w(-1, 0.0),
];
pub const CROUCH_TO_PRONE: [ViewLerpWaypoint; 11] = [
    w(0, 40.0),
    w(11, 38.0),
    w(22, 33.0),
    w(34, 25.0),
    w(45, 16.0),
    w(50, 15.0),
    w(55, 16.0),
    w(70, 18.0),
    w(90, 17.0),
    w(100, 11.0),
    w(-1, 0.0),
];
pub const PRONE_TO_CROUCH: [ViewLerpWaypoint; 8] = [
    w(0, 11.0),
    w(5, 10.0),
    w(30, 21.0),
    w(50, 25.0),
    w(67, 31.0),
    w(83, 34.0),
    w(100, 40.0),
    w(-1, 0.0),
];

/// How long the view height takes to reach `target` (milliseconds): 400
/// to lying down, 400 up to crouching (200 down to it), else 200.
///
/// Original: `PM_GetViewHeightLerpTime` at 0x00517330 (CoD2MP_s.exe 1.3; Call of Duty 4's name; the target in eax, down on the stack)
pub fn view_height_lerp_time(target: i32, down: i32) -> i32 {
    if target == PRONE_VIEW_HEIGHT {
        400
    } else if target == CROUCH_VIEW_HEIGHT {
        if down != 0 {
            200
        } else {
            400
        }
    } else {
        200
    }
}

/// The view height `fraction` percent along a way (`table`, ending in
/// fraction -1), and its position offset: a waypoint's own where the
/// fraction is one's (and 0, or past the last, the first's), else in
/// between the two around it (the offset as `float`s from the integers).
/// The height is the x87 register's.
///
/// Original: `PM_ViewHeightTableLerp` at 0x00517360 (CoD2MP_s.exe 1.3; Call of Duty 4's name; fraction in eax, table in edi, the offset out in ebx; the height on the x87 stack)
pub fn view_height_table_lerp(fraction: i32, table: &[ViewLerpWaypoint]) -> (F24, f32) {
    let l = F24::load;
    let first = table[0];
    let at = |p: ViewLerpWaypoint| (l(p.view_height), F24::int(p.offset).store());
    if fraction == 0 {
        return at(first);
    }
    for k in 1..table.len() {
        let p = table[k];
        if p.fraction == -1 {
            break;
        }
        if p.fraction == fraction {
            return at(p);
        }
        if p.fraction > fraction {
            let prev = table[k - 1];
            let t = F24::int(fraction.wrapping_sub(prev.fraction))
                / F24::int(p.fraction.wrapping_sub(prev.fraction));
            let offset =
                (F24::int(p.offset.wrapping_sub(prev.offset)) * t + F24::int(prev.offset)).store();
            let height = t * (l(p.view_height) - l(prev.view_height)) + l(prev.view_height);
            return (height, offset);
        }
    }
    at(first)
}

/// The way a view height change to `target` takes (going `down` or not).
fn lerp_table(target: i32, down: bool) -> &'static [ViewLerpWaypoint] {
    if target == PRONE_VIEW_HEIGHT {
        &CROUCH_TO_PRONE
    } else if target == CROUCH_VIEW_HEIGHT {
        if down {
            &STAND_TO_CROUCH
        } else {
            &PRONE_TO_CROUCH
        }
    } else {
        &CROUCH_TO_STAND
    }
}

/// The view height on its way to `viewHeightTarget`:
///
/// - no target, or a view height of 0 (or NaN-free equal): the target
///   (spectators 0).
/// - a target not a stance's (the dead's 8): 180 units a second toward it,
///   no further.
/// - a stance's, with a change going: its fraction from the start
///   (`serverTime - viewHeightLerpTime` over [`view_height_lerp_time`],
///   kept to 0 to 100; at 100 it's done, the height the change's target)
///   along its way ([`view_height_table_lerp`]); a position offset moving
///   by more than 0.05 slides the player (the step slide move with
///   gravity, at the change over the frame along the view's level forward,
///   half in the air; the velocity kept). A change going the wrong way
///   turns round: the rest of the way, toward the next stance.
/// - none going and not there: a change starts (to crouching from above
///   40 then down to lying down; to standing from below 40 by crouching).
///
/// Original: `PM_ViewHeightAdjust` at 0x005174D0 (CoD2MP_s.exe 1.3; Call of Duty 4's name; pm, pml on the stack)
pub fn view_height_adjust(
    pm: &mut Pmove,
    pml: &mut MoveLocal,
    settings: &MoveSettings,
    host: &mut dyn MoveHost,
) {
    let l = F24::load;
    let target = pm.ps.int(ps::VIEW_HEIGHT_TARGET);
    let current = l(pm.ps.float(ps::VIEW_HEIGHT_CURRENT));
    // (`fucompp` with 0, test ah,0x44, jnp: only equal is none; NaN goes on.)
    if target == 0 || current == l(0.0) {
        let height = if pm.ps.pm_type() == pm_type::SPECTATOR {
            0.0
        } else {
            F24::int(target).store()
        };
        pm.ps.set_float(ps::VIEW_HEIGHT_CURRENT, height);
        return;
    }
    let goal = F24::int(target).store();
    // (`fucompp` with the integer's register, not the `float` kept: test
    // ah,0x44, jp: only equal stays.)
    if current == F24::int(target) && pm.ps.int(ps::VIEW_HEIGHT_LERP_TIME) == 0 {
        return;
    }
    if target != PRONE_VIEW_HEIGHT && target != CROUCH_VIEW_HEIGHT && target != STAND_VIEW_HEIGHT {
        pm.ps.set_int(ps::VIEW_HEIGHT_LERP_TIME, 0);
        let step = l(pml.frame_time()) * l(180.0);
        // (`fcomp`, test ah,5, jp: only below goes up; NaN goes down.)
        if current < l(goal) {
            let up = step + current;
            pm.ps.set_float(ps::VIEW_HEIGHT_CURRENT, up.store());
            // (test ah,1, jne: below, or NaN, keeps going.)
            if !matches!(
                up.partial_cmp(&l(goal)),
                Some(std::cmp::Ordering::Less) | None
            ) {
                pm.ps.set_float(ps::VIEW_HEIGHT_CURRENT, goal);
            }
        } else {
            let down = current - step;
            pm.ps.set_float(ps::VIEW_HEIGHT_CURRENT, down.store());
            // (test ah,0x41, jp: above, or NaN, keeps going.)
            if down <= l(goal) {
                pm.ps.set_float(ps::VIEW_HEIGHT_CURRENT, goal);
            }
        }
        return;
    }

    let mut fraction = 0;
    let start = pm.ps.int(ps::VIEW_HEIGHT_LERP_TIME);
    if start != 0 {
        let lerp_target = pm.ps.int(ps::VIEW_HEIGHT_LERP_TARGET);
        let down = pm.ps.int(ps::VIEW_HEIGHT_LERP_DOWN);
        let time = view_height_lerp_time(lerp_target, down);
        fraction = pm
            .server_time()
            .wrapping_sub(start)
            .wrapping_mul(100)
            .wrapping_div(time);
        if fraction >= 100 {
            fraction = 100;
            pm.ps
                .set_float(ps::VIEW_HEIGHT_CURRENT, F24::int(lerp_target).store());
            pm.ps.set_int(ps::VIEW_HEIGHT_LERP_TIME, 0);
            pm.ps.set_int(ps::VIEW_HEIGHT_LERP_POS_ADJ, 0);
        } else {
            fraction = fraction.max(0);
            let (height, offset) =
                view_height_table_lerp(fraction, lerp_table(lerp_target, down != 0));
            pm.ps.set_float(ps::VIEW_HEIGHT_CURRENT, height.store());
            let last = l(pm.ps.float(ps::VIEW_HEIGHT_LERP_POS_ADJ));
            // (`fcomp` 0.05, test ah,0x41, jne: only above slides; NaN doesn't.)
            if (last - l(offset)).abs() > l(0.05) {
                let saved = pm.ps.velocity();
                let mut change = l(offset) - last;
                if pm.ps.int(ps::GROUND_ENTITY_NUM) == ENTITY_NONE {
                    change = change * l(0.5);
                }
                let speed = (change / l(pml.frame_time())).store();
                let forward = pml.vec3(pml::FORWARD);
                let mut dir = [forward[0], forward[1], 0.0];
                vec3_normalize(&mut dir);
                pm.ps.set_velocity(dir.map(|c| (l(c) * l(speed)).store()));
                step_slide_move(pm, pml, true, settings, host);
                pm.ps.set_velocity(saved);
                pm.ps.set_float(ps::VIEW_HEIGHT_LERP_POS_ADJ, offset);
            }
        }
    }

    if pm.ps.int(ps::VIEW_HEIGHT_LERP_TIME) != 0 {
        let target = pm.ps.int(ps::VIEW_HEIGHT_TARGET);
        let lerp_target = pm.ps.int(ps::VIEW_HEIGHT_LERP_TARGET);
        let down = pm.ps.int(ps::VIEW_HEIGHT_LERP_DOWN);
        let wrong_way = (target < lerp_target && down == 0) || (target > lerp_target && down != 0);
        if !wrong_way {
            return;
        }
        let rest = 100 - fraction;
        let down = down ^ 1;
        pm.ps.set_int(ps::VIEW_HEIGHT_LERP_DOWN, down);
        let next = match (down != 0, lerp_target) {
            (true, STAND_VIEW_HEIGHT) => CROUCH_VIEW_HEIGHT,
            (true, CROUCH_VIEW_HEIGHT) => PRONE_VIEW_HEIGHT,
            (false, PRONE_VIEW_HEIGHT) => CROUCH_VIEW_HEIGHT,
            (false, CROUCH_VIEW_HEIGHT) => STAND_VIEW_HEIGHT,
            (_, other) => other,
        };
        pm.ps.set_int(ps::VIEW_HEIGHT_LERP_TARGET, next);
        if rest == 100 {
            pm.ps
                .set_float(ps::VIEW_HEIGHT_CURRENT, F24::int(next).store());
            pm.ps.set_int(ps::VIEW_HEIGHT_LERP_TIME, 0);
            pm.ps.set_int(ps::VIEW_HEIGHT_LERP_POS_ADJ, 0);
            return;
        }
        let time = view_height_lerp_time(next, down);
        let gone = ftol2((F24::int(time) * F24::int(rest) * l(0.01)).0) as i32;
        pm.ps.set_int(
            ps::VIEW_HEIGHT_LERP_TIME,
            pm.server_time().wrapping_sub(gone),
        );
        let (_, offset) = view_height_table_lerp(rest, lerp_table(next, down != 0));
        pm.ps.set_float(ps::VIEW_HEIGHT_LERP_POS_ADJ, offset);
        return;
    }

    let target = pm.ps.int(ps::VIEW_HEIGHT_TARGET);
    let goal = F24::int(target).store();
    let current = l(pm.ps.float(ps::VIEW_HEIGHT_CURRENT));
    // (`fucompp` with the integer's register, test ah,0x44, jnp: only equal
    // is there; NaN starts one.)
    if current == F24::int(target) {
        return;
    }
    pm.ps.set_int(ps::VIEW_HEIGHT_LERP_TIME, pm.server_time());
    let (lerp_target, down) = match target {
        // (`fcomp` 40, test ah,0x41, jne: only above stops at crouching.)
        PRONE_VIEW_HEIGHT if current > l(40.0) => (CROUCH_VIEW_HEIGHT, 1),
        PRONE_VIEW_HEIGHT => (PRONE_VIEW_HEIGHT, 1),
        // (`fcomp` 40: only above goes down.)
        CROUCH_VIEW_HEIGHT => (CROUCH_VIEW_HEIGHT, i32::from(current > l(goal))),
        // (`fcomp` 40, test ah,5, jnp: only below stops at crouching.)
        STAND_VIEW_HEIGHT if current < l(40.0) => (CROUCH_VIEW_HEIGHT, 0),
        STAND_VIEW_HEIGHT => (STAND_VIEW_HEIGHT, 0),
        _ => return,
    };
    pm.ps.set_int(ps::VIEW_HEIGHT_LERP_DOWN, down);
    pm.ps.set_int(ps::VIEW_HEIGHT_LERP_TARGET, lerp_target);
}

/// Whether the box fits where the player stands with `maxs[2]` at
/// `height`: a trace of it from the origin to the origin (not colliding
/// with bodies) not all in a solid.
fn fits(pm: &mut Pmove, height: f32, host: &mut dyn MoveHost) -> bool {
    pm.set_float(pm::MAXS + 8, height);
    let origin = pm.ps.vec3(ps::ORIGIN);
    let (mins, maxs) = (pm.vec3(pm::MINS), pm.vec3(pm::MAXS));
    let mask = pm.int(pm::TRACE_MASK) & !CONTENTS_BODY;
    let handler = pm.bytes[pm::HANDLER];
    let client = pm.ps.int(ps::CLIENT_NUM);
    !host
        .trace(handler, &origin, &mins, &maxs, &origin, client, mask)
        .all_solid()
}

/// A refused stance change's event (none with
/// [`button::NO_STANCE_EVENT`]).
fn refused(pm: &mut Pmove, event: i32, parm: i32) {
    if pm.int(pm::CMD + super::pmove::cmd::BUTTONS) & button::NO_STANCE_EVENT == 0 {
        add_predictable_event(event, parm, &mut pm.ps);
    }
}

/// Sets `pm_flags`' stance bits: lying down, crouching, or neither.
fn set_stance_flags(ps: &mut PlayerState, prone: bool, ducked: bool) {
    let mut flags = ps.pm_flags() & !(pm_flags::PRONE | pm_flags::DUCKED);
    if prone {
        flags |= pm_flags::PRONE;
    }
    if ducked {
        flags |= pm_flags::DUCKED;
    }
    ps.set_int(ps::PM_FLAGS, flags);
}

/// The stance and the box the move has: `proneChange` 0; a spectator a
/// box of 8 (16 high), standing, his prone button dropped (with
/// [`EV_STANCE_FORCE_STAND`]), no view height. Else the box is the player
/// state's (standing height); the dead's view height 8 ([`view_height_adjust`]).
/// On a mounted weapon its stance (0x100 prone, 0x200 crouching, both
/// standing). Else, unless [`pm_flags::NO_LEAN`]:
///
/// - a ladder drops the stance buttons ([`EV_STANCE_FORCE_STAND`]).
/// - the prone button: lying down if he does, else (on the ground) if
///   [`check_prone`] finds room (66 feet, a box `maxs[0]` wide and 30
///   high, no room check above), else refused ([`pm_flags::PRONE_VIEW_TURNED`]
///   set; crouching's or standing's event).
/// - the crouch button: from lying down if the crouching box fits (the
///   animation), else refused (lying down's, 2); from standing at once
///   (the animation).
/// - neither: up from lying down if the standing box fits (the
///   animation), else to crouching if that fits, else refused (lying
///   down's, 1); up from crouching if standing fits (the animation), else
///   refused (crouching's, 1).
///
/// Then, unless a view height change is going, `viewHeightTarget` follows
/// the stance (lying down from standing goes by crouching; reaching lying
/// down sets `proneChange`, plays the torso's animation 0 and the jump's
/// landing slowdown; leaving it `proneChange` and the animation), and
/// [`view_height_adjust`]. The box's height and the stance's `eFlags` and
/// `pm_flags` follow the view height's target. Having just lain down: the
/// aim ends when moving ([`exit_aim_down_sight`]), the box goes 10 up and
/// back down (traced) to settle, the prone body points the view's way,
/// pitched as the ground under it is, and the view's prone pitch is that
/// within 45 of the view's.
///
/// Original: `PM_CheckDuck` at 0x005179B0 (CoD2MP_s.exe 1.3; Quake III's name; pm in edi, pml on the stack)
pub fn check_duck(
    pm: &mut Pmove,
    pml: &mut MoveLocal,
    settings: &MoveSettings,
    host: &mut dyn MoveHost,
) {
    let l = F24::load;
    pm.set_int(pm::PRONE_CHANGE, 0);
    let buttons_at = pm::CMD + super::pmove::cmd::BUTTONS;
    if pm.ps.pm_type() == pm_type::SPECTATOR {
        for (k, v) in [-8.0f32, -8.0, -8.0].into_iter().enumerate() {
            pm.set_float(pm::MINS + 4 * k, v);
        }
        for (k, v) in [8.0f32, 8.0, 16.0].into_iter().enumerate() {
            pm.set_float(pm::MAXS + 4 * k, v);
        }
        set_stance_flags(&mut pm.ps, false, false);
        let buttons = pm.int(buttons_at);
        if buttons & button::PRONE != 0 {
            pm.set_int(buttons_at, buttons & !button::PRONE);
            add_predictable_event(EV_STANCE_FORCE_STAND, 0, &mut pm.ps);
        }
        pm.ps.set_int(ps::VIEW_HEIGHT_TARGET, 0);
        pm.ps.set_int(ps::VIEW_HEIGHT_CURRENT, 0);
        return;
    }
    let was_prone = pm.ps.pm_flags() & pm_flags::PRONE != 0;
    let mins = pm.ps.vec3(ps::MINS);
    let maxs = pm.ps.vec3(ps::MAXS);
    pm.set_float(pm::MINS, mins[0]);
    pm.set_float(pm::MINS + 4, mins[1]);
    pm.set_float(pm::MAXS, maxs[0]);
    pm.set_float(pm::MAXS + 4, maxs[1]);
    pm.set_float(pm::MINS + 8, mins[2]);
    if pm.ps.pm_type() >= pm_type::DEAD {
        pm.set_float(pm::MAXS + 8, maxs[2]);
        pm.ps.set_int(ps::VIEW_HEIGHT_TARGET, DEAD_VIEW_HEIGHT);
        view_height_adjust(pm, pml, settings, host);
        return;
    }

    let e_flags = pm.ps.int(ps::E_FLAGS);
    if e_flags & E_FLAGS_MOUNTED != 0 {
        match (e_flags & 0x100 != 0, e_flags & 0x200 != 0) {
            (true, false) => set_stance_flags(&mut pm.ps, true, false),
            (false, true) => set_stance_flags(&mut pm.ps, false, true),
            _ => set_stance_flags(&mut pm.ps, false, false),
        }
    } else if pm.ps.pm_flags() & pm_flags::NO_LEAN == 0 {
        let stance_buttons = button::PRONE | button::CROUCH;
        let buttons = pm.int(buttons_at);
        if pm.ps.pm_flags() & pm_flags::LADDER != 0 && buttons & stance_buttons != 0 {
            pm.set_int(buttons_at, buttons & !stance_buttons);
            add_predictable_event(EV_STANCE_FORCE_STAND, 0, &mut pm.ps);
        }
        let buttons = pm.int(buttons_at);
        let flags = pm.ps.pm_flags();
        if buttons & button::PRONE != 0 {
            if flags & pm_flags::PRONE != 0 {
                set_stance_flags(&mut pm.ps, true, false);
            } else if pm.ps.int(ps::GROUND_ENTITY_NUM) != ENTITY_NONE {
                let q = ProneQuery {
                    pass_entity: pm.ps.int(ps::CLIENT_NUM),
                    origin: pm.ps.vec3(ps::ORIGIN),
                    size: pm.float(pm::MAXS),
                    height: PRONE_MAXS_Z,
                    yaw: pm.ps.float(ps::VIEW_ANGLES + 4),
                    already_prone: false,
                    on_ground: true,
                    ground_normal: None,
                    handler: pm.bytes[pm::HANDLER],
                    check_type: 0,
                    feet_distance: 66.0,
                };
                let mut angles = [
                    pm.ps.float(ps::PRONE_TORSO_HEIGHT),
                    pm.ps.float(ps::PRONE_TORSO_PITCH),
                    pm.ps.float(ps::PRONE_WAIST_PITCH),
                ];
                let [h, t, w] = &mut angles;
                let out = ProneAngles {
                    torso_height: Some(h),
                    torso_pitch: Some(t),
                    waist_pitch: Some(w),
                };
                let fits = check_prone(&q, out, host);
                pm.ps.set_float(ps::PRONE_TORSO_HEIGHT, angles[0]);
                pm.ps.set_float(ps::PRONE_TORSO_PITCH, angles[1]);
                pm.ps.set_float(ps::PRONE_WAIST_PITCH, angles[2]);
                if fits {
                    set_stance_flags(&mut pm.ps, true, false);
                } else if pm.ps.int(ps::GROUND_ENTITY_NUM) != ENTITY_NONE {
                    let flags = pm.ps.pm_flags() | pm_flags::PRONE_VIEW_TURNED;
                    pm.ps.set_int(ps::PM_FLAGS, flags);
                    let event = if flags & pm_flags::DUCKED != 0 {
                        EV_STANCE_FORCE_CROUCH
                    } else {
                        EV_STANCE_FORCE_STAND
                    };
                    refused(pm, event, 0);
                }
            }
        } else if buttons & button::CROUCH != 0 {
            if flags & pm_flags::PRONE != 0 {
                if fits(pm, CROUCH_MAXS_Z, host) {
                    // (`BG_AnimScriptEvent`, inlined: the same checks.)
                    host.anim_script_event(&mut pm.ps, ANIM_ET_PRONE_TO_CROUCH, false, false);
                    set_stance_flags(&mut pm.ps, false, true);
                } else {
                    refused(pm, EV_STANCE_FORCE_PRONE, 2);
                }
            } else {
                host.anim_script_event(&mut pm.ps, ANIM_ET_STAND_TO_CROUCH, false, false);
                let flags = pm.ps.pm_flags() | pm_flags::DUCKED;
                pm.ps.set_int(ps::PM_FLAGS, flags);
            }
        } else if flags & pm_flags::PRONE != 0 {
            if fits(pm, maxs[2], host) {
                host.anim_script_event(&mut pm.ps, ANIM_ET_PRONE_TO_STAND, false, false);
                set_stance_flags(&mut pm.ps, false, false);
            } else if fits(pm, CROUCH_MAXS_Z, host) {
                set_stance_flags(&mut pm.ps, false, true);
            } else {
                refused(pm, EV_STANCE_FORCE_PRONE, 1);
            }
        } else if flags & pm_flags::DUCKED != 0 {
            if fits(pm, maxs[2], host) {
                host.anim_script_event(&mut pm.ps, ANIM_ET_CROUCH_TO_STAND, false, false);
                let flags = pm.ps.pm_flags() & !pm_flags::DUCKED;
                pm.ps.set_int(ps::PM_FLAGS, flags);
            } else {
                refused(pm, EV_STANCE_FORCE_CROUCH, 1);
            }
        }
    }

    if pm.ps.int(ps::VIEW_HEIGHT_LERP_TIME) == 0 {
        let target = pm.ps.int(ps::VIEW_HEIGHT_TARGET);
        if pm.ps.pm_flags() & pm_flags::PRONE != 0 {
            if target == STAND_VIEW_HEIGHT {
                pm.ps.set_int(ps::VIEW_HEIGHT_TARGET, CROUCH_VIEW_HEIGHT);
            } else if target != PRONE_VIEW_HEIGHT {
                pm.ps.set_int(ps::VIEW_HEIGHT_TARGET, PRONE_VIEW_HEIGHT);
                pm.set_int(pm::PRONE_CHANGE, 1);
                play_torso_anim_zero(&mut pm.ps, host);
                jump_activate_slowdown(&mut pm.ps);
            }
        } else if target == PRONE_VIEW_HEIGHT {
            pm.ps.set_int(ps::VIEW_HEIGHT_TARGET, CROUCH_VIEW_HEIGHT);
            pm.set_int(pm::PRONE_CHANGE, 1);
            play_torso_anim_zero(&mut pm.ps, host);
        } else {
            let ducked = pm.ps.pm_flags() & pm_flags::DUCKED != 0;
            pm.ps.set_int(
                ps::VIEW_HEIGHT_TARGET,
                if ducked {
                    CROUCH_VIEW_HEIGHT
                } else {
                    STAND_VIEW_HEIGHT
                },
            );
        }
    }
    view_height_adjust(pm, pml, settings, host);

    let target = pm.ps.int(ps::VIEW_HEIGHT_TARGET);
    let e_flags = pm.ps.int(ps::E_FLAGS) & !(E_FLAGS_CROUCHING | E_FLAGS_PRONE);
    if target == CROUCH_VIEW_HEIGHT {
        pm.set_float(pm::MAXS + 8, CROUCH_MAXS_Z);
        pm.ps.set_int(ps::E_FLAGS, e_flags | E_FLAGS_CROUCHING);
        set_stance_flags(&mut pm.ps, false, true);
    } else if target == PRONE_VIEW_HEIGHT {
        pm.set_float(pm::MAXS + 8, PRONE_MAXS_Z);
        pm.ps.set_int(ps::E_FLAGS, e_flags | E_FLAGS_PRONE);
        set_stance_flags(&mut pm.ps, true, false);
    } else {
        pm.set_float(pm::MAXS + 8, pm.ps.float(ps::MAXS + 8));
        pm.ps.set_int(ps::E_FLAGS, e_flags);
        set_stance_flags(&mut pm.ps, false, false);
    }

    if pm.ps.pm_flags() & pm_flags::PRONE == 0 || was_prone {
        return;
    }
    let command = pm.cmd();
    if command[super::pmove::cmd::FORWARD_MOVE] != 0 || command[super::pmove::cmd::RIGHT_MOVE] != 0
    {
        let flags = pm.ps.pm_flags() & !pm_flags::ADS_HOLD;
        pm.ps.set_int(ps::PM_FLAGS, flags);
        exit_aim_down_sight(&mut pm.ps);
    }
    let handler = pm.bytes[pm::HANDLER];
    let client = pm.ps.int(ps::CLIENT_NUM);
    let mask = pm.int(pm::TRACE_MASK) & !CONTENTS_BODY;
    let (mins, maxs) = (pm.vec3(pm::MINS), pm.vec3(pm::MAXS));
    let origin = pm.ps.vec3(ps::ORIGIN);
    let up = [origin[0], origin[1], (l(origin[2]) + l(10.0)).store()];
    let trace = host.trace(handler, &origin, &mins, &maxs, &up, client, mask);
    let f = l(trace.fraction());
    let point: [f32; 3] =
        std::array::from_fn(|i| ((l(up[i]) - l(origin[i])) * f + l(origin[i])).store());
    let back = host.trace(handler, &point, &mins, &maxs, &origin, client, mask);
    let settled = vec3_lerp(&point, &origin, back.fraction());
    pm.ps.set_vec3(ps::ORIGIN, settled);
    let yaw = pm.ps.float(ps::VIEW_ANGLES + 4);
    pm.ps.set_float(ps::PRONE_DIRECTION, yaw);
    let origin = settled;
    let below = [origin[0], origin[1], (l(origin[2]) - l(0.25)).store()];
    let ground = host.trace(handler, &origin, &mins, &maxs, &below, client, mask);
    // (`fcomp` 1, test ah,5, jp: only below hit the ground; NaN didn't.)
    let pitch = if !ground.start_solid() && l(ground.fraction()) < l(1.0) {
        pitch_for_yaw_on_normal(pm.ps.float(ps::PRONE_DIRECTION), &ground.normal())
    } else {
        0.0
    };
    pm.ps.set_float(ps::PRONE_DIRECTION_PITCH, pitch);
    let view_pitch = pm.ps.float(ps::VIEW_ANGLES);
    let delta = l(angle_normalize180((l(pitch) - l(view_pitch)).store()));
    // (`fcomp` -45, test ah,5, jp; then 45, test ah,0x41, jne: NaN keeps the body's.)
    let base = if delta < l(-45.0) {
        (l(view_pitch) - l(45.0)).store()
    } else if delta > l(45.0) {
        (l(view_pitch) + l(45.0)).store()
    } else {
        pitch
    };
    pm.ps.set_float(ps::PRONE_PITCH_BASE, base);
}

/// The torso's animation 0 started (Call of Duty 2's `BG_PlayAnim` for the
/// torso, inlined; forced): unless it is that already (the toggle bit
/// aside), the toggle bit flips and its duration is animation 0's + 50.
fn play_torso_anim_zero(ps: &mut PlayerState, host: &mut dyn MoveHost) {
    let anim = ps.int(ps::TORSO_ANIM);
    if anim & !ANIM_TOGGLE_BIT != 0 {
        ps.set_int(ps::TORSO_ANIM, !anim & ANIM_TOGGLE_BIT);
        ps.set_int(
            ps::TORSO_ANIM_DURATION,
            host.anim_duration(0).wrapping_add(50),
        );
    }
}

/// The bit an animation number flips when the same one starts again
/// (`ANIM_TOGGLEBIT`).
pub const ANIM_TOGGLE_BIT: i32 = 0x200;
