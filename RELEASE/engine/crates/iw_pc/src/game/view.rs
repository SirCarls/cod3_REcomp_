//! The view: the command's angles kept within what the player may look
//! at (a mounted weapon's arcs, a ladder, a prone body, the pitch limits),
//! and leaning. Call of Duty 2's versions (see [`super::pmove`] for the
//! conventions).

use super::mantle::mantle_cap_view;
use super::pmove::{
    button, cmd, effective_stance, pm_flags, pm_type, ps, stance, MoveSettings, PlayerState,
    ENTITY_NONE,
};
use super::prone::{check_prone, check_prone_turned, ProneAngles, ProneQuery};
use super::trace::MoveHost;
use crate::net::msg::ftol2;
use crate::shared::math::{
    add_lean_to_position, angle_delta, angle_normalize180, angle_wrap180, angle_wrap360, vectoyaw,
};
use crate::x87::F24;

/// What a lean's trace collides with (`MASK_PLAYERSOLID` and more; the
/// original's 0x2810011).
const LEAN_MASK: i32 = 0x0281_0011;

/// `1` or `-1` by a `float`'s sign bit (`x < 0` on its bits as an integer).
fn sign_bit_unit(x: f32) -> i32 {
    if (x.to_bits() as i32) < 0 {
        -1
    } else {
        1
    }
}

/// The lean (`leanf`) after `msec`: toward the side the command's lean
/// buttons hold ([`button::LEAN_LEFT`] -1, [`button::LEAN_RIGHT`] +1; none
/// when dead, with [`pm_flags::NO_LEAN`], in the air (but `pm_type` 1), or
/// with an eFlags 0x300 bit) by `msec / 350` of the most (0.25 lying down,
/// else 0.5), back to 0 by `msec / 280` of it with none, never past; then
/// kept to what room a trace (a box of 8 from the eye toward the full lean,
/// [`add_lean_to_position`] with roll 16 and 20 units, by the game's trace
/// function `handler`) leaves: `1 - sqrt(1 - fraction)`.
///
/// Original: `PM_UpdateLean` at 0x00518FF0 (CoD2MP_s.exe 1.3; Call of Duty 4's name; ps in esi, cmd in eax, then (msec, capsuleTrace) on the stack: the trace function `handler` names)
pub fn update_lean(
    ps: &mut PlayerState,
    msec: f32,
    command: &[u8],
    handler: u8,
    host: &mut dyn MoveHost,
) {
    let l = F24::load;
    let buttons = i32::from_le_bytes([
        command[cmd::BUTTONS],
        command[cmd::BUTTONS + 1],
        command[cmd::BUTTONS + 2],
        command[cmd::BUTTONS + 3],
    ]);
    let mut leaning = 0;
    let kind = ps.pm_type();
    if buttons & (button::LEAN_LEFT | button::LEAN_RIGHT) != 0
        && ps.pm_flags() & pm_flags::NO_LEAN == 0
        && kind < pm_type::DEAD
        && (ps.int(ps::GROUND_ENTITY_NUM) != ENTITY_NONE || kind == 1)
    {
        if buttons & button::LEAN_LEFT != 0 {
            leaning = -1;
        }
        if buttons & button::LEAN_RIGHT != 0 {
            leaning += 1;
        }
    }
    if ps.int(ps::E_FLAGS) & 0x300 != 0 {
        leaning = 0;
    }
    let most = if effective_stance(ps) == stance::PRONE {
        0.25f32
    } else {
        0.5
    };
    let step = |k: f32| l(msec) * l(k) * l(most);
    let mut lean = l(ps.float(ps::LEANF));
    let zero = l(0.0);
    match leaning {
        0 => {
            // (`fcom` 0, test ah,0x41: above comes back down, never past 0;
            // below (test ah,5) comes back up; 0 or NaN stays.)
            if lean > zero {
                lean = lean - step(0.003_571_428_6);
                if lean < zero {
                    lean = zero;
                }
            } else if lean < zero {
                lean = lean + step(0.003_571_428_6);
                if lean > zero {
                    lean = zero;
                }
            }
        }
        1 => {
            // (`fcom` most, test ah,5: only below grows; test ah,0x41: above is the most.)
            if lean < l(most) {
                lean = lean + step(0.002_857_142_8);
            }
            if lean > l(most) {
                lean = l(most);
            }
        }
        _ => {
            let least = l((-l(most)).store());
            // (`fcom` -most, test ah,0x41: only above shrinks; test ah,5: below is -most.)
            if lean > least {
                lean = lean - step(0.002_857_142_8);
            }
            if lean < least {
                lean = least;
            }
        }
    }
    ps.set_float(ps::LEANF, lean.store());
    // (`fucompp` with 0 of the register, jnp: only 0 needs no room; NaN does.)
    if lean == zero {
        return;
    }
    let side = sign_bit_unit(lean.store());
    let o = ps.vec3(ps::ORIGIN);
    let start = [
        o[0],
        o[1],
        (l(o[2]) + l(ps.float(ps::VIEW_HEIGHT_CURRENT))).store(),
    ];
    let mut end = start;
    add_lean_to_position(
        &mut end,
        ps.float(ps::VIEW_ANGLES + 4),
        F24::int(side).store(),
        16.0,
        20.0,
    );
    let trace = host.trace(
        handler,
        &start,
        &[-8.0; 3],
        &[8.0; 3],
        &end,
        ps.int(ps::CLIENT_NUM),
        LEAN_MASK,
    );
    let room = l(1.0) - (l(1.0) - l(trace.fraction())).sqrt();
    let now = ps.float(ps::LEANF);
    // (`fcomp` with the room, test ah,0x41: only above is cut; NaN isn't.)
    if l(now).abs() > room {
        ps.set_float(ps::LEANF, (F24::int(sign_bit_unit(now)) * room).store());
    }
}

/// A 16-bit angle's degrees (`360 / 65536`, exact as a `float`).
const SHORT_TO_DEGREES: f32 = 360.0 / 65536.0;

/// `ANGLE2SHORT`: `ftol2(angle * 182.04445) & 0xFFFF` (the product the
/// register's).
fn angle_to_short(angle: f32) -> i32 {
    ftol2((F24::load(angle) * F24::load(182.044_45)).0) as i32 & 0xFFFF
}

/// `SHORT2ANGLE` of a signed 16-bit angle: as a `float`, times `360 /
/// 65536`, stored.
fn short_to_angle(short: i16) -> f32 {
    (F24::load(F24::int(i32::from(short)).store()) * F24::load(SHORT_TO_DEGREES)).store()
}

/// A command angle (an integer at [`cmd::ANGLES`] + 4 * `i`).
fn command_angle(command: &[u8], i: usize) -> i32 {
    let at = cmd::ANGLES + 4 * i;
    i32::from_le_bytes([
        command[at],
        command[at + 1],
        command[at + 2],
        command[at + 3],
    ])
}

/// The command's angle `i` plus the player's `delta_angles` in 16 bits
/// (both words added, sign extended).
fn view_short(ps: &PlayerState, command: &[u8], i: usize) -> i16 {
    (ps.int(ps::DELTA_ANGLES + 4 * i) as i16).wrapping_add(command_angle(command, i) as i16)
}

/// Keeps view angle `i` within `cap` of `base`: past it (`delta`, the turn
/// from the view to `base`), the excess `e` (`delta - cap` above, `delta +
/// cap` below; stored) goes to `delta_angles` (`ANGLE2SHORT`) and the view
/// becomes `base - cap` (`e` above 0) or `base + cap`, wrapped by `wrap`.
/// NaN stays.
fn cap_view(ps: &mut PlayerState, i: usize, delta: f32, base: f32, cap: f32, wrap: fn(f32) -> f32) {
    let l = F24::load;
    // (`fcomp` cap, test ah,0x41: above; `-cap` against delta: below.)
    if !(delta > cap || -cap > delta) {
        return;
    }
    let excess = if delta > cap {
        l(delta) - l(cap)
    } else {
        l(delta) + l(cap)
    }
    .store();
    let at = ps::DELTA_ANGLES + 4 * i;
    let turned = ps.int(at).wrapping_add(angle_to_short(excess));
    ps.set_int(at, turned);
    let edge = if excess > 0.0 {
        l(base) - l(cap)
    } else {
        l(base) + l(cap)
    };
    ps.set_float(ps::VIEW_ANGLES + 4 * i, wrap(edge.store()));
}

/// [`check_prone`] for a prone player's view: already prone, on the ground
/// unless in the air, a box of [`ps::PRONE_CHECK_SIZE`] by 30, the feet 45
/// behind, no angles asked for.
fn prone_fits(ps: &PlayerState, yaw: f32, handler: u8, host: &mut dyn MoveHost) -> bool {
    let q = ProneQuery {
        pass_entity: ps.int(ps::CLIENT_NUM),
        origin: ps.vec3(ps::ORIGIN),
        size: ps.float(ps::PRONE_CHECK_SIZE),
        height: 30.0,
        yaw,
        already_prone: true,
        on_ground: ps.int(ps::GROUND_ENTITY_NUM) != ENTITY_NONE,
        ground_normal: None,
        handler,
        check_type: 0,
        feet_distance: 45.0,
    };
    check_prone(&q, ProneAngles::default(), host)
}

/// The unit step toward `delta` (`1`, `-1`) when it is more than 1 away,
/// else `delta` itself, which ends the walk (`false`).
fn degree_step(delta: f32) -> (f32, bool) {
    // (`fcomp` 1, test ah,0x41: only above takes a unit; NaN doesn't.)
    if F24::load(F24::load(delta).abs().store()) > F24::load(1.0) {
        (if delta > 0.0 { 1.0 } else { -1.0 }, true)
    } else {
        (delta, false)
    }
}

/// A prone player's view and body: the body turns toward the view (by
/// `msec * 0.055` a move, when the view is near the cap or the player
/// moves) as far as it fits (then one degree at a time back toward its old
/// direction) and the view must fit too; else the view turns toward the
/// body until both fit; then the view is kept within `bg_prone_yawcap` of
/// the body and, having been turned, eased back by 0.98 of a small turn
/// against the command's, and its pitch within 45 of the prone pitch.
#[allow(clippy::too_many_arguments)]
fn prone_view(
    ps: &mut PlayerState,
    msec: f32,
    command: &[u8],
    handler: u8,
    old_yaw: f32,
    command_yaw: f32,
    settings: &MoveSettings,
    host: &mut dyn MoveHost,
) {
    let l = F24::load;
    let view_yaw = |ps: &PlayerState| ps.float(ps::VIEW_ANGLES + 4);
    let mut turned = false;
    let mut delta =
        angle_normalize180((l(ps.float(ps::PRONE_DIRECTION)) - l(view_yaw(ps))).store());
    let edge = (l(settings.prone_yaw_cap) - l(5.0)).store();
    let moving = command[cmd::FORWARD_MOVE] != 0 || command[cmd::RIGHT_MOVE] != 0;
    // (`fcomp`, test ah,0x41: beyond the edge either way; or moving with a
    // turn (`fucompp`: only 0 is none; NaN is one).)
    if delta > edge || -edge > delta || (moving && delta != 0.0) {
        // (The game's constant is 0.055000003, a bit above `0.055f`.)
        let step = (l(msec) * l(0.055_000_003)).store();
        let direction = ps.float(ps::PRONE_DIRECTION);
        // (`fcomp` step, test ah,5: only below takes the view's yaw.)
        let mut target = if l(delta).abs().store() < step {
            view_yaw(ps)
        } else if delta > 0.0 {
            (l(direction) - l(step)).store()
        } else {
            (l(step) + l(direction)).store()
        };
        let mut more = true;
        let mut fits = check_prone_turned(ps, target, handler, host);
        while !fits && more {
            let back = angle_normalize180((l(ps.float(ps::PRONE_DIRECTION)) - l(target)).store());
            let (unit, again) = degree_step(back);
            if !again {
                turned = true;
            }
            more = again;
            target = angle_wrap360((l(target) + l(unit)).store());
            fits = check_prone_turned(ps, target, handler, host);
        }
        if fits {
            if prone_fits(ps, view_yaw(ps), handler, host) && prone_fits(ps, target, handler, host)
            {
                ps.set_float(ps::PRONE_DIRECTION, target);
            } else {
                turned = true;
            }
        }
    }

    delta = angle_normalize180((l(ps.float(ps::PRONE_DIRECTION)) - l(view_yaw(ps))).store());
    // (`fucompp` with 0, jnp: only no turn skips; NaN goes on.)
    if delta != 0.0 {
        let mut test_yaw = ps.float(ps::PRONE_DIRECTION);
        let mut again = true;
        loop {
            let ok = prone_fits(ps, test_yaw, handler, host);
            if ok && check_prone_turned(ps, test_yaw, handler, host) {
                ps.set_float(ps::PRONE_DIRECTION, test_yaw);
                break;
            }
            if !again {
                break;
            }
            let (unit, more) = degree_step(delta);
            again = more;
            turned = true;
            let yaw = ps.int(ps::DELTA_YAW).wrapping_add(angle_to_short(unit));
            ps.set_int(ps::DELTA_YAW, yaw);
            let view = angle_wrap360((l(unit) + l(view_yaw(ps))).store());
            ps.set_float(ps::VIEW_ANGLES + 4, view);
            delta = angle_normalize180((l(ps.float(ps::PRONE_DIRECTION)) - l(view)).store());
            if !ok {
                test_yaw = angle_wrap360((l(test_yaw) + l(delta)).store());
            }
        }
    }
    let direction = ps.float(ps::PRONE_DIRECTION);
    cap_view(
        ps,
        1,
        delta,
        direction,
        settings.prone_yaw_cap,
        angle_wrap360,
    );

    if turned {
        let flags = ps.pm_flags() | pm_flags::PRONE_VIEW_TURNED;
        ps.set_int(ps::PM_FLAGS, flags);
        let back = angle_normalize180((l(old_yaw) - l(view_yaw(ps))).store());
        // (`fcomp` 1, test ah,0x41, jp: at most 1 eases; NaN doesn't.)
        if l(l(back).abs().store()) <= l(1.0) {
            let against = angle_delta(command_yaw, view_yaw(ps));
            // (`fcomp` 0, test ah,0x41: only the same way, above 0.)
            if l(against) * l(back) > l(0.0) {
                let ease = (l(back) * l(0.98)).store();
                let view = angle_wrap360((l(ease) + l(view_yaw(ps))).store());
                ps.set_float(ps::VIEW_ANGLES + 4, view);
                let yaw = ps.int(ps::DELTA_YAW).wrapping_add(angle_to_short(ease));
                ps.set_int(ps::DELTA_YAW, yaw);
            }
        }
    }

    let base = ps.float(ps::PRONE_PITCH_BASE);
    let pitch = angle_normalize180((l(base) - l(ps.float(ps::VIEW_ANGLES))).store());
    cap_view(ps, 0, pitch, base, 45.0, angle_wrap180);
}

/// The view's angles from the command: in the intermission, none; dead,
/// the dead yaw is kept (once) and only the lean moves. Else the command's
/// angles plus `delta_angles` (pitch kept within `player_view_pitch_up` and
/// `_down`, the excess moved into `delta_angles`); then a mounted weapon's
/// arcs (and nothing more); a climb's cap ([`mantle_cap_view`], and nothing
/// more); on a ladder in the air `bg_ladder_yawcap` from facing the ladder;
/// lying down, the body and the view ([`prone_view`]); then the lean, but
/// for noclip, ufo and spectators.
///
/// Original: `PM_UpdateViewAngles` at 0x00519340 (CoD2MP_s.exe 1.3; Quake III's name; ps in eax, then (msec, cmd, handler) on the stack)
pub fn update_view_angles(
    ps: &mut PlayerState,
    msec: f32,
    command: &[u8],
    handler: u8,
    settings: &MoveSettings,
    host: &mut dyn MoveHost,
) {
    let l = F24::load;
    let kind = ps.pm_type();
    if kind == pm_type::INTERMISSION {
        return;
    }
    if kind >= pm_type::DEAD {
        if ps.int(ps::DEAD_YAW) == 999 {
            let yaw = short_to_angle(view_short(ps, command, 1));
            ps.set_int(ps::DEAD_YAW, ftol2(f64::from(yaw)) as i32);
        }
        update_lean(ps, msec, command, handler, host);
        return;
    }
    let old_yaw = ps.float(ps::VIEW_ANGLES + 4);
    let up = angle_to_short(settings.view_pitch_up);
    let down = angle_to_short(settings.view_pitch_down);
    let pitch = i32::from(view_short(ps, command, 0));
    let pitch = if pitch > down {
        ps.set_int(
            ps::DELTA_ANGLES,
            down.wrapping_sub(command_angle(command, 0)),
        );
        down
    } else if pitch < -up {
        let delta = command_angle(command, 0).wrapping_add(up).wrapping_neg();
        ps.set_int(ps::DELTA_ANGLES, delta);
        -up
    } else {
        pitch
    };
    ps.set_float(ps::VIEW_ANGLES, short_to_angle(pitch as i16));
    for i in 1..3 {
        let angle = short_to_angle(view_short(ps, command, i));
        ps.set_float(ps::VIEW_ANGLES + 4 * i, angle);
    }
    let command_yaw = ps.float(ps::VIEW_ANGLES + 4);

    if ps.int(ps::E_FLAGS) & 0x300 != 0 {
        // A mounted weapon: within its arcs.
        for i in 0..2 {
            let base = ps.float(ps::VIEW_ANGLE_CLAMP_BASE + 4 * i);
            let view = ps.float(ps::VIEW_ANGLES + 4 * i);
            let short = ftol2(((l(base) - l(view)) * l(182.044_45)).0) as i32 & 0xFFFF;
            let mut delta = (l(F24::int(short).store()) * l(SHORT_TO_DEGREES)).store();
            // (`fcomp` 180, test ah,0x41: only above turns back.)
            if delta > 180.0 {
                delta = (l(delta) - l(360.0)).store();
            }
            let range = ps.float(ps::VIEW_ANGLE_CLAMP_RANGE + 4 * i);
            cap_view(ps, i, delta, base, range, angle_wrap360);
        }
        return;
    }
    let flags = ps.pm_flags();
    if flags & pm_flags::MANTLE != 0 {
        mantle_cap_view(ps, &settings.mantle);
        return;
    }
    // (`fucompp` with 0, jnp: only a cap of 0 skips; NaN caps.)
    if flags & pm_flags::LADDER != 0
        && ps.int(ps::GROUND_ENTITY_NUM) == ENTITY_NONE
        && settings.ladder_yaw_cap != 0.0
    {
        let ladder_yaw = (l(vectoyaw(&ps.vec3(ps::LADDER_VEC))) + l(180.0)).store();
        let delta = angle_normalize180((l(ladder_yaw) - l(ps.float(ps::VIEW_ANGLES + 4))).store());
        cap_view(
            ps,
            1,
            delta,
            ladder_yaw,
            settings.ladder_yaw_cap,
            angle_wrap360,
        );
    }
    if flags & pm_flags::PRONE != 0 {
        prone_view(
            ps,
            msec,
            command,
            handler,
            old_yaw,
            command_yaw,
            settings,
            host,
        );
    }
    if !matches!(kind, pm_type::NOCLIP | pm_type::UFO | pm_type::SPECTATOR) {
        update_lean(ps, msec, command, handler, host);
    }
}
