//! The kinds of move (`bg_pmove.c`'s `PM_*Move`): how a command's
//! forward and right moves become velocity in the air, on the ground, on a
//! ladder ..., Call of Duty 2's versions (see [`super::pmove`] for the
//! conventions).

use super::jump::{jump_apply_slowdown, jump_check};
use super::pmove::{
    accelerate, button, cmd, cmd_scale, cmd_scale_walk, damage_scale_walk, effective_stance,
    friction, move_scale, player_trace, pm, pm_flags, pm_type, pml, ps, set_movement_dir, stance,
    MoveLocal, MoveSettings, PlayerState, Pmove, CROUCH_VIEW_HEIGHT, ENTITY_NONE,
    PRONE_VIEW_HEIGHT, STAND_VIEW_HEIGHT, SURF_SLICK, USERCMD_SIZE,
};
use super::slide::step_slide_move;
use super::trace::{MoveHost, ANIM_ET_JUMP};
use crate::net::msg::ftol2;
use crate::shared::math::{
    angle_normalize180, vec2_normalize, vec3_normalize, vec3_normalize_to, vectoyaw,
};
use crate::x87::F24;

/// A command's forward and right moves (signed bytes at 0x18 and 0x19) as
/// `float`s.
fn moves(pm: &Pmove) -> (f32, f32) {
    let cmd = pm.cmd();
    (f32::from(cmd[0x18] as i8), f32::from(cmd[0x19] as i8))
}

/// The move in the air: friction, then the command's moves along the
/// view's level forward and right (normalized), at the command's scale
/// ([`cmd_scale`]), accelerated at 1 (`pm_airaccelerate`); on a surface
/// the velocity first slides along it; then the step slide move with
/// gravity, and the movement direction.
///
/// Original: `PM_AirMove` at 0x00515FA0 (CoD2MP_s.exe 1.3; Quake III's name; pml in eax, pm on the stack)
pub fn air_move(
    pm: &mut Pmove,
    pml: &mut MoveLocal,
    settings: &MoveSettings,
    host: &mut dyn MoveHost,
) {
    let l = F24::load;
    friction(&mut pm.ps, pml, settings);
    let (fmove, smove) = moves(pm);
    let cmd: [u8; USERCMD_SIZE] = pm.cmd().try_into().expect("a command's bytes");
    let scale = cmd_scale(&pm.ps, &cmd, settings).store();

    let mut forward = pml.vec3(pml::FORWARD);
    let mut right = pml.vec3(pml::RIGHT);
    forward[2] = 0.0;
    right[2] = 0.0;
    vec3_normalize(&mut forward);
    vec3_normalize(&mut right);
    pml.set_vec3(pml::FORWARD, forward);
    pml.set_vec3(pml::RIGHT, right);

    let mut wish = [0f32; 3];
    for i in 0..2 {
        wish[i] = (l(smove) * l(right[i]) + l(fmove) * l(forward[i])).store();
    }
    let wish_speed = (F24::load(vec3_normalize(&mut wish)) * l(scale)).store();
    accelerate(&mut pm.ps, pml, &wish, wish_speed, 1.0, settings);

    if pml.int(pml::GROUND_PLANE) != 0 {
        // Along the surface (its own order of the dot product).
        let n = pml.vec3(pml::GROUND_NORMAL);
        let v = pm.ps.velocity();
        let backoff = l(n[0]) * l(v[0]) + l(v[2]) * l(n[2]) + l(v[1]) * l(n[1]);
        let change = -(backoff - backoff.abs() * l(0.001));
        let mut out = v;
        for i in 0..3 {
            out[i] = (change * l(n[i]) + l(v[i])).store();
        }
        pm.ps.set_velocity(out);
    }

    step_slide_move(pm, pml, true, settings, host);
    set_movement_dir(pm, pml);
}

/// `v` along a plane of normal `n`: `v + n * -(b - |b| * 0.001)`, `b = v . n`
/// (in [`walk_move`]'s order, `(v0*n0 + v1*n1) + n2*v2`; [`super::pmove::clip_velocity`]'s
/// is another).
fn along_ground(v: [f32; 3], n: [f32; 3]) -> [f32; 3] {
    let l = F24::load;
    let b = l(v[0]) * l(n[0]) + l(v[1]) * l(n[1]) + l(n[2]) * l(v[2]);
    let change = -(b - b.abs() * l(0.001));
    [0, 1, 2].map(|i| (change * l(n[i]) + l(v[i])).store())
}

/// The move on the ground: after a jump the landing's slowdown
/// ([`jump_apply_slowdown`]); a jump ([`jump_check`]) moves in the air
/// instead. Else friction; the view's level forward and right along the
/// ground (normalized); the command's moves along them at the walk's scale
/// ([`cmd_scale_walk`], times [`damage_scale_walk`], whose timer runs down
/// by the frame's milliseconds, truncated); accelerated at 9 (12 crouching,
/// 19 lying down; 1 on a slick surface or in knockback, then gravity too; a
/// quarter of it sliding); the velocity along the ground at its old speed
/// (unless that turns it around); the step slide move without gravity
/// unless the horizontal velocity is 0; and the movement direction.
///
/// Original: `PM_WalkMove` at 0x005160E0 (CoD2MP_s.exe 1.3; Quake III's name; pm, then pml, on the stack)
pub fn walk_move(
    pm: &mut Pmove,
    pml: &mut MoveLocal,
    settings: &MoveSettings,
    host: &mut dyn MoveHost,
) {
    let l = F24::load;
    if pm.ps.pm_flags() & pm_flags::JUMPING != 0 {
        jump_apply_slowdown(&mut pm.ps, settings);
    }
    if jump_check(pm, pml, settings, host) {
        air_move(pm, pml, settings, host);
        return;
    }
    friction(&mut pm.ps, pml, settings);
    let (fmove, smove) = moves(pm);
    let cmd: [u8; USERCMD_SIZE] = pm.cmd().try_into().expect("a command's bytes");
    let scale = cmd_scale_walk(pm, &cmd, settings, &*host).store();
    let timer = pm.ps.int(ps::DAMAGE_TIMER);
    let scale = (damage_scale_walk(timer, settings) * l(scale)).store();
    let msec = ftol2((l(pml.frame_time()) * l(1000.0)).0) as i32;
    let timer = timer.wrapping_sub(msec);
    pm.ps
        .set_int(ps::DAMAGE_TIMER, if timer <= 0 { 0 } else { timer });

    let n = pml.vec3(pml::GROUND_NORMAL);
    let mut forward = pml.vec3(pml::FORWARD);
    // (The forward's z is 0, so the original's dot product takes `n2 * 0`
    // and its z `change * n2` alone.)
    let b = l(forward[0]) * l(n[0]) + l(n[1]) * l(forward[1]) + l(n[2]) * l(0.0);
    let change = -(b - b.abs() * l(0.001));
    forward = [
        (change * l(n[0]) + l(forward[0])).store(),
        (change * l(n[1]) + l(forward[1])).store(),
        (change * l(n[2])).store(),
    ];
    let mut right = pml.vec3(pml::RIGHT);
    right[2] = 0.0;
    let mut right = along_ground(right, n);
    vec3_normalize(&mut forward);
    vec3_normalize(&mut right);
    pml.set_vec3(pml::FORWARD, forward);
    pml.set_vec3(pml::RIGHT, right);

    let mut wish = [0f32; 3];
    for i in 0..3 {
        wish[i] = (l(smove) * l(right[i]) + l(fmove) * l(forward[i])).store();
    }
    let wish_speed = (l(vec3_normalize(&mut wish)) * l(scale)).store();
    let slick = pml.int(pml::GROUND_SURFACE_FLAGS) & SURF_SLICK != 0
        || pm.ps.pm_flags() & pm_flags::TIME_KNOCKBACK != 0;
    let mut accel = if slick {
        1.0
    } else {
        match effective_stance(&pm.ps) {
            stance::PRONE => 19.0,
            stance::CROUCH => 12.0,
            _ => 9.0,
        }
    };
    if pm.ps.pm_flags() & pm_flags::SLIDE != 0 {
        accel = (l(accel) * l(0.25)).store();
    }
    accelerate(&mut pm.ps, pml, &wish, wish_speed, accel, settings);
    if slick {
        let fall = F24::int(pm.ps.int(ps::GRAVITY)) * l(pml.frame_time());
        let z = (l(pm.ps.float(ps::VELOCITY + 8)) - fall).store();
        pm.ps.set_float(ps::VELOCITY + 8, z);
    }

    // Along the ground, at the old speed unless that turns it around.
    let old = pm.ps.velocity();
    let [o0, o1, o2] = old.map(l);
    let speed = (o0 * o0 + o1 * o1 + o2 * o2).sqrt().store();
    let b = l(n[0]) * o0 + l(n[1]) * o1 + o2 * l(n[2]);
    let change = -(b - b.abs() * l(0.001));
    let mut v = [0, 1, 2].map(|i| (change * l(n[i]) + l(old[i])).store());
    pm.ps.set_velocity(v);
    // (`fcomp` 0, test ah,0x41, jne: only above keeps the speed.)
    if o2 * l(v[2]) + o1 * l(v[1]) + o0 * l(v[0]) > l(0.0) {
        vec3_normalize(&mut v);
        pm.ps.set_velocity(v.map(|c| (l(speed) * l(c)).store()));
    }

    // (`fucompp` with 0: only both exactly 0 skip; NaN doesn't.)
    let v = pm.ps.velocity();
    if !(v[0] == 0.0 && v[1] == 0.0) {
        step_slide_move(pm, pml, false, settings, host);
    }
    set_movement_dir(pm, pml);
}

/// The move of a flying player (noclip, ufo, a spectator): friction; the
/// command's moves along the view's forward and right at [`cmd_scale`]
/// (nothing at a scale of 0), and with a speed [`button::LEAN_LEFT`] and
/// [`button::LEAN_RIGHT`] lower and raise it by `127 * move_scale(0, 0, 127)`;
/// accelerated at 8; the step slide move without gravity.
///
/// Original: `PM_FlyMove` at 0x00515E60 (CoD2MP_s.exe 1.3; Quake III's name; pml in eax, pm on the stack)
pub fn fly_move(
    pm: &mut Pmove,
    pml: &mut MoveLocal,
    settings: &MoveSettings,
    host: &mut dyn MoveHost,
) {
    let l = F24::load;
    friction(&mut pm.ps, pml, settings);
    let command: [u8; USERCMD_SIZE] = pm.cmd().try_into().expect("a command's bytes");
    let scale = cmd_scale(&pm.ps, &command, settings);
    let (forward, right) = (pml.vec3(pml::FORWARD), pml.vec3(pml::RIGHT));
    let mut wish = [0f32; 3];
    let mut z = l(0.0);
    // (`fucompp` with 0, jp: only 0 moves nowhere; NaN goes on.)
    if scale != l(0.0) {
        let f = F24::int(i32::from(command[cmd::FORWARD_MOVE] as i8));
        let r = F24::int(i32::from(command[cmd::RIGHT_MOVE] as i8));
        wish[0] = ((r * l(right[0]) + f * l(forward[0])) * scale).store();
        wish[1] = ((f * l(forward[1]) + r * l(right[1])) * scale).store();
        z = (f * l(forward[2]) + r * l(right[2])) * scale;
    }
    if pm.ps.int(ps::SPEED) != 0 {
        let up = move_scale(&pm.ps, 0.0, 0.0, 127.0, settings).store();
        let buttons = pm.int(pm::CMD + cmd::BUTTONS);
        let stored = z.store();
        z = if buttons & button::LEAN_LEFT != 0 {
            l(stored) - l(up) * l(127.0)
        } else {
            l(stored)
        };
        if buttons & button::LEAN_RIGHT != 0 {
            z = z + l(up) * l(127.0);
        }
    }
    wish[2] = z.store();
    let wish_speed = vec3_normalize(&mut wish);
    accelerate(&mut pm.ps, pml, &wish, wish_speed, 8.0, settings);
    step_slide_move(pm, pml, false, settings, host);
}

/// The move on a ladder: a jump ([`jump_check`]) moves in the air instead.
/// Else up and down with the forward move, at [`cmd_scale`] times `(forward
/// z + 0.25) * 2.5` (within -1 to 1) and a half; sideways with the right
/// move, along the view's level right turned into the ladder's plane, at
/// 0.2 of the scale; accelerated at 9. Without a forward move the vertical
/// speed runs toward 0 by gravity (never past it); without a right move the
/// sideways speed (along the level right) goes, but for what is left over
/// `frametime * 16` of it (at least 1). Off the ground the velocity loses
/// what goes into the ladder, and with a vertical speed at least the
/// horizontal one it is pushed 50 off it. Then the step slide move without
/// gravity, and the movement direction from the ladder (within 75 degrees).
///
/// Original: `PM_LadderMove` at 0x0051A360 (CoD2MP_s.exe 1.3; Return to Castle Wolfenstein's name; pml in eax, pm on the stack)
pub fn ladder_move(
    pm: &mut Pmove,
    pml: &mut MoveLocal,
    settings: &MoveSettings,
    host: &mut dyn MoveHost,
) {
    let l = F24::load;
    if jump_check(pm, pml, settings, host) {
        air_move(pm, pml, settings, host);
        return;
    }
    // (`fcomp` 1, test ah,0x41: only above is 1; `fcomp` -1, test ah,5:
    // only below is -1; NaN stays, as `clamp` keeps it.)
    let up_scale = ((l(pml.float(pml::FORWARD + 8)) + l(0.25)) * l(2.5))
        .store()
        .clamp(-1.0, 1.0);
    let mut forward = pml.vec3(pml::FORWARD);
    forward[2] = 0.0;
    vec3_normalize(&mut forward);
    pml.set_vec3(pml::FORWARD, forward);
    let mut right = pml.vec3(pml::RIGHT);
    right[2] = 0.0;
    let mut flat = [0f32; 3];
    vec3_normalize_to(&right, &mut flat);
    let ladder = pm.ps.vec3(ps::LADDER_VEC);
    let d = (-(l(flat[0]) * l(ladder[0]) + l(flat[1]) * l(ladder[1]) + l(flat[2]) * l(ladder[2])))
        .store();
    for i in 0..3 {
        right[i] = (l(d) * l(ladder[i]) + l(flat[i])).store();
    }
    pml.set_vec3(pml::RIGHT, right);

    let command: [u8; USERCMD_SIZE] = pm.cmd().try_into().expect("a command's bytes");
    let mut scale = cmd_scale(&pm.ps, &command, settings).store();
    let fmove = command[cmd::FORWARD_MOVE] as i8;
    let smove = command[cmd::RIGHT_MOVE] as i8;
    let mut wish = [0f32; 3];
    if fmove != 0 {
        let f = F24::int(i32::from(fmove)).store();
        wish[2] = (l(f) * l(scale) * l(up_scale) * l(0.5)).store();
    }
    if smove != 0 {
        let r = F24::int(i32::from(smove)).store();
        scale = (l(r) * l(scale) * l(0.2)).store();
        wish[0] = (l(scale) * l(right[0]) + l(0.0)).store();
        wish[1] = (l(scale) * l(right[1]) + l(0.0)).store();
        wish[2] = (l(scale) * l(right[2]) + l(wish[2])).store();
    }
    let mut dir = [0f32; 3];
    let wish_speed = vec3_normalize_to(&wish, &mut dir);
    accelerate(&mut pm.ps, pml, &dir, wish_speed, 9.0, settings);

    if fmove == 0 {
        // Toward 0 by gravity, never past it.
        let v = pm.ps.float(ps::VELOCITY + 8);
        let fall = l(F24::int(pm.ps.int(ps::GRAVITY)).store()) * l(pml.frame_time());
        // (`fcomp` 0, test ah,0x41: above falls; else (or NaN) rises.)
        let z = if v > 0.0 {
            let z = (l(v) - fall).store();
            // (`fcomp` 0, test ah,5: only below stops.)
            if z < 0.0 {
                0.0
            } else {
                z
            }
        } else {
            let z = (fall + l(v)).store();
            // (test ah,0x41: only above stops.)
            if z > 0.0 {
                0.0
            } else {
                z
            }
        };
        pm.ps.set_float(ps::VELOCITY + 8, z);
    }
    if smove == 0 {
        side_friction(pm, pml, right);
    }
    if !pml.walking() {
        // Nothing into the ladder; climbing faster than moving away pushes off.
        let v = pm.ps.velocity();
        let d = -(l(v[1]) * l(ladder[1]) + l(v[0]) * l(ladder[0])).store();
        let v0 = (l(d) * l(ladder[0]) + l(v[0])).store();
        let v1 = (l(d) * l(ladder[1]) + l(v[1])).store();
        pm.ps.set_float(ps::VELOCITY, v0);
        pm.ps.set_float(ps::VELOCITY + 4, v1);
        let v2 = v[2];
        // (`fcompp`, test ah,1: below, or NaN, keeps it.)
        let horizontal = l(v0) * l(v0) + l(v1) * l(v1);
        let vertical = l(v2) * l(v2);
        if vertical >= horizontal {
            pm.ps
                .set_float(ps::VELOCITY, (l(v0) - l(ladder[0]) * l(50.0)).store());
            pm.ps
                .set_float(ps::VELOCITY + 4, (l(v1) - l(ladder[1]) * l(50.0)).store());
        }
    }
    step_slide_move(pm, pml, false, settings, host);

    let yaw = (l(vectoyaw(&ladder)) + l(180.0)).store();
    let turn = (l(yaw) - l(pm.ps.float(ps::VIEW_ANGLES + 4))).store();
    let mut dir = ftol2(f64::from(angle_normalize180(turn))) as i32;
    if dir.wrapping_abs() > 75 {
        dir = if dir <= 0 { -75 } else { 75 };
    }
    pm.ps.set_int(ps::MOVEMENT_DIR, i32::from(dir as i8));
}

/// [`ladder_move`]'s sideways friction: the velocity along the level right
/// (`right`'s x and y, normalized; `dot = r1*v1 + r0*v0`) goes, unless 0,
/// and comes back but for `x = dot * frametime * 16` (stored; `±1` when
/// below 1, by its sign bit) when `|dot|` is above `|x|`.
fn side_friction(pm: &mut Pmove, pml: &MoveLocal, right: [f32; 3]) {
    let l = F24::load;
    let mut side = [right[0], right[1]];
    vec2_normalize(&mut side);
    let v = pm.ps.velocity();
    let dot = (l(side[1]) * l(v[1]) + l(side[0]) * l(v[0])).store();
    // (`fucompp` with 0, jnp: only 0 skips; NaN goes on.)
    if dot == 0.0 {
        return;
    }
    let back = (-l(dot)).store();
    let v0 = (l(back) * l(side[0]) + l(v[0])).store();
    let v1 = (l(side[1]) * l(back) + l(v[1])).store();
    pm.ps.set_float(ps::VELOCITY, v0);
    pm.ps.set_float(ps::VELOCITY + 4, v1);
    let mut x = (l(dot) * l(pml.frame_time()) * l(16.0)).store();
    let ax = x.abs();
    // (`fcomp`, test ah,0x41: only above goes on; NaN doesn't.)
    if dot.abs() <= ax || dot.is_nan() || ax.is_nan() {
        return;
    }
    // (`fcomp` 1, test ah,5: below.)
    if ax < 1.0 {
        x = if (x.to_bits() as i32) < 0 { -1.0 } else { 1.0 };
    }
    let rest = (l(dot) - l(x)).store();
    pm.ps
        .set_float(ps::VELOCITY, (l(side[0]) * l(rest) + l(v0)).store());
    pm.ps
        .set_float(ps::VELOCITY + 4, (l(side[1]) * l(rest) + l(v1)).store());
}

/// The move of a dead player on the ground: the speed drops by 20 (the
/// velocity stops at 0 or below; NaN keeps its direction).
///
/// Original: `PM_DeadMove` at 0x00516450 (CoD2MP_s.exe 1.3; Quake III's name; pml in eax, ps on the stack)
pub fn dead_move(ps: &mut PlayerState, pml: &MoveLocal) {
    let l = F24::load;
    if !pml.walking() {
        return;
    }
    let [v0, v1, v2] = ps.velocity().map(l);
    let forward = (v0 * v0 + v1 * v1 + v2 * v2).sqrt() - l(20.0);
    // (`fcomp` 0, test ah,0x41, jp: 0 or below stops; NaN goes on.)
    if forward <= l(0.0) {
        ps.set_velocity([0.0; 3]);
        return;
    }
    let speed = forward.store();
    let mut v = ps.velocity();
    vec3_normalize(&mut v);
    ps.set_velocity(v.map(|c| (l(speed) * l(c)).store()));
}

/// A noclip or UFO move's up move from the lean buttons:
/// [`button::LEAN_RIGHT`] 127, [`button::LEAN_LEFT`] 127 less (`computed`:
/// 0 + 127 as the UFO move adds it; the noclip move writes 127).
fn up_move(buttons: i32, computed: bool) -> f32 {
    let l = F24::load;
    let mut up = 0.0f32;
    if buttons & button::LEAN_RIGHT != 0 {
        up = if computed {
            (l(0.0) + l(127.0)).store()
        } else {
            127.0
        };
    }
    if buttons & button::LEAN_LEFT != 0 {
        up = (l(up) - l(127.0)).store();
    }
    up
}

/// The command's forward and right moves as `float`s (signed bytes).
fn command_moves(pm: &Pmove) -> (f32, f32) {
    let command = pm.cmd();
    (
        F24::int(i32::from(command[cmd::FORWARD_MOVE] as i8)).store(),
        F24::int(i32::from(command[cmd::RIGHT_MOVE] as i8)).store(),
    )
}

/// The origin moved by the velocity over the frame.
fn fly_through(ps: &mut PlayerState, frame_time: f32) {
    let l = F24::load;
    let v = ps.velocity();
    let origin = ps.vec3(ps::ORIGIN);
    let moved: [f32; 3] = std::array::from_fn(|i| (l(frame_time) * l(v[i]) + l(origin[i])).store());
    ps.set_vec3(ps::ORIGIN, moved);
}

/// Flying through walls (`pm_type` noclip): standing's view height; the
/// velocity slowed by friction 1.5 times `friction` (below
/// `stopspeed` as at it; under a speed of 1 it stops); the command's moves
/// along the view's forward, right and up (the up move from the lean
/// buttons), at their scale ([`move_scale`]), accelerated at 9; then the
/// origin moves by the velocity, through everything.
///
/// Original: `PM_NoclipMove` at 0x005164D0 (CoD2MP_s.exe 1.3; Quake III's name; pml in eax, pm in ecx)
pub fn noclip_move(pm: &mut Pmove, pml: &MoveLocal, settings: &MoveSettings) {
    let l = F24::load;
    pm.ps.set_int(ps::VIEW_HEIGHT_TARGET, STAND_VIEW_HEIGHT);
    let v = pm.ps.velocity();
    let speed = (l(v[0]) * l(v[0]) + l(v[1]) * l(v[1]) + l(v[2]) * l(v[2])).sqrt();
    // (`fcom` 1, test ah,5, jp: only below stops; NaN goes on.)
    if speed < l(1.0) {
        pm.ps.set_velocity([0.0; 3]);
    } else {
        let friction = l(settings.friction) * l(1.5);
        let stop = l(settings.stop_speed);
        // (`fcomp` stopspeed, test ah,5, jp: only below is raised to it.)
        let control = if speed < stop { stop } else { speed };
        let drop = control * l(pml.frame_time()) * friction;
        let mut new_speed = speed - drop;
        if new_speed < l(0.0) {
            new_speed = l(0.0);
        }
        let ratio = new_speed / speed;
        pm.ps.set_velocity(v.map(|c| (ratio * l(c)).store()));
    }
    let (forward_move, right_move) = command_moves(pm);
    let up_move = up_move(pm.int(pm::CMD + cmd::BUTTONS), false);
    let scale = move_scale(&pm.ps, forward_move, right_move, up_move, settings).store();
    let (forward, right, up) = (
        pml.vec3(pml::FORWARD),
        pml.vec3(pml::RIGHT),
        pml.vec3(pml::UP),
    );
    let mut wish: [f32; 3] = std::array::from_fn(|i| {
        (l(up_move) * l(up[i]) + l(right_move) * l(right[i]) + l(forward_move) * l(forward[i]))
            .store()
    });
    let length = vec3_normalize(&mut wish);
    let wish_speed = (l(length) * l(scale)).store();
    accelerate(&mut pm.ps, pml, &wish, wish_speed, 9.0, settings);
    fly_through(&mut pm.ps, pml.frame_time());
}

/// Flying (`pm_type` UFO): standing's view height; with no move at all
/// the velocity stops; else it slows as in [`noclip_move`] (the speed and
/// each step stored as `float`s); the command's forward move along the
/// view's level forward (the up direction crossed with the right), the
/// right move along the right, the up move (the lean buttons) straight
/// up, at their scale, accelerated at 9; then the origin moves by the
/// velocity, through everything.
///
/// Original: `PM_UFOMove` at 0x00516690 (CoD2MP_s.exe 1.3; our name; pm in eax, pml in ecx)
pub fn ufo_move(pm: &mut Pmove, pml: &MoveLocal, settings: &MoveSettings) {
    let l = F24::load;
    pm.ps.set_int(ps::VIEW_HEIGHT_TARGET, STAND_VIEW_HEIGHT);
    let (forward_move, right_move) = command_moves(pm);
    let up_move = up_move(pm.int(pm::CMD + cmd::BUTTONS), true);
    // (`fucompp` with 0, jp: any move but 0 (NaN too) slows instead.)
    let still = l(forward_move) == l(0.0) && l(right_move) == l(0.0) && l(up_move) == l(0.0);
    let v = pm.ps.velocity();
    let squared = (l(v[0]) * l(v[0]) + l(v[1]) * l(v[1]) + l(v[2]) * l(v[2])).store();
    let speed = l(squared).sqrt().store();
    // (`fcomp` 1, test ah,5, jp: only below stops; NaN goes on.)
    if still || l(speed) < l(1.0) {
        pm.ps.set_velocity([0.0; 3]);
    } else {
        let friction = (l(settings.friction) * l(1.5)).store();
        // (`fcomp` stopspeed, test ah,5, jp: only below is raised to it.)
        let control = if l(speed) < l(settings.stop_speed) {
            settings.stop_speed
        } else {
            speed
        };
        let drop = (l(control) * l(pml.frame_time()) * l(friction) + l(0.0)).store();
        let mut new_speed = (l(speed) - l(drop)).store();
        if l(new_speed) < l(0.0) {
            new_speed = 0.0;
        }
        let ratio = (l(new_speed) / l(speed)).store();
        pm.ps.set_velocity(v.map(|c| (l(ratio) * l(c)).store()));
    }
    let scale = move_scale(&pm.ps, forward_move, right_move, up_move, settings).store();
    let right = pml.vec3(pml::RIGHT);
    let t = (l(right[2]) * l(0.0)).store();
    let level = [
        (l(t) - l(right[1])).store(),
        (l(right[0]) - l(t)).store(),
        (l(right[1]) * l(0.0) - l(right[0]) * l(0.0)).store(),
    ];
    let lift = (l(up_move) * l(0.0)).store();
    let (f, r) = (l(forward_move), l(right_move));
    let mut wish = [
        (l(level[0]) * f + r * l(right[0]) + l(lift)).store(),
        (r * l(right[1]) + l(level[1]) * f + l(lift)).store(),
        (r * l(right[2]) + l(level[2]) * f + l(up_move)).store(),
    ];
    let length = vec3_normalize(&mut wish);
    let wish_speed = (l(length) * l(scale)).store();
    accelerate(&mut pm.ps, pml, &wish, wish_speed, 9.0, settings);
    fly_through(&mut pm.ps, pml.frame_time());
}

/// The surface flag of ladders (`SURF_LADDER`).
pub const SURF_LADDER: i32 = 0x8;

/// Off a ladder: [`pm_flags::LADDER`] cleared, [`pm_flags::LADDER_FALL`]
/// set if it was on.
///
/// Original: `PM_LeaveLadder` at 0x0051A000 (CoD2MP_s.exe 1.3; our name; ps in eax)
pub fn leave_ladder(ps: &mut PlayerState) {
    let mut flags = ps.pm_flags();
    if flags & pm_flags::LADDER != 0 {
        flags |= pm_flags::LADDER_FALL;
    }
    ps.set_int(ps::PM_FLAGS, flags & !pm_flags::LADDER);
}

/// Whether the player is on a ladder ([`pm_flags::LADDER`]): walking drops
/// [`pm_flags::LADDER_FALL`]; a timed knockback or slide off a ladder
/// leaves it all as it is. Dead: off the ground and the ladder. Not with
/// [`pm_flags::LADDER_FALL`], lying down, or within 300 ms of a jump.
/// Else a box (6 in from the sides, 8 from the floor, at least 8 high)
/// traced 8 units (30 in the air) along the view's level forward (or,
/// hanging on a ladder in the air, into it): a ladder surface hit there is
/// the ladder (unless walking without a forward move): its normal becomes
/// the ladder's direction, and a trace into it must find it again. Off a
/// ladder in the air, the jump's animation plays (forced).
///
/// Original: `PM_CheckLadderMove` at 0x0051A020 (CoD2MP_s.exe 1.3; Return to Castle Wolfenstein's name; pm, pml on the stack)
pub fn check_ladder_move(pm: &mut Pmove, pml: &mut MoveLocal, host: &mut dyn MoveHost) {
    let l = F24::load;
    let walking = pml.int(pml::WALKING) != 0;
    if walking {
        let flags = pm.ps.pm_flags() & !pm_flags::LADDER_FALL;
        pm.ps.set_int(ps::PM_FLAGS, flags);
    }
    let flags = pm.ps.pm_flags();
    if pm.ps.int(ps::PM_TIME) != 0
        && flags & pm_flags::LADDER == 0
        && flags & (pm_flags::SLIDE | pm_flags::TIME_KNOCKBACK) != 0
    {
        return;
    }
    let trace_dist = if walking { 8.0f32 } else { 30.0 };
    let was_on = flags & pm_flags::LADDER != 0;
    let hanging = was_on && pm.ps.int(ps::GROUND_ENTITY_NUM) == ENTITY_NONE;
    let dir = if hanging {
        pm.ps.vec3(ps::LADDER_VEC).map(|c| (-l(c)).store())
    } else {
        let forward = pml.vec3(pml::FORWARD);
        let mut d = [forward[0], forward[1], 0.0];
        vec3_normalize(&mut d);
        d
    };
    if pm.ps.pm_type() >= pm_type::DEAD {
        pm.ps.set_int(ps::GROUND_ENTITY_NUM, ENTITY_NONE);
        for at in [pml::GROUND_PLANE, pml::ALMOST_GROUND_PLANE, pml::WALKING] {
            pml.set_int(at, 0);
        }
        leave_ladder(&mut pm.ps);
        return;
    }
    let target = pm.ps.int(ps::VIEW_HEIGHT_TARGET);
    if flags & pm_flags::LADDER_FALL != 0
        || (target != CROUCH_VIEW_HEIGHT && target == PRONE_VIEW_HEIGHT)
        || pm.server_time().wrapping_sub(pm.ps.int(ps::JUMP_TIME)) < 300
    {
        leave_ladder(&mut pm.ps);
        return;
    }
    let (box_mins, box_maxs) = (pm.vec3(pm::MINS), pm.vec3(pm::MAXS));
    let mins = [
        (l(box_mins[0]) + l(6.0)).store(),
        (l(box_mins[1]) + l(6.0)).store(),
        8.0,
    ];
    let mut maxs = [
        (l(box_maxs[0]) - l(6.0)).store(),
        (l(box_maxs[1]) - l(6.0)).store(),
        box_maxs[2],
    ];
    // (`fcomp` 8, test ah,5, jp: only below is raised; NaN isn't.)
    if l(maxs[2]) < l(8.0) {
        maxs[2] = 8.0;
    }
    let origin = pm.ps.vec3(ps::ORIGIN);
    let client = pm.ps.int(ps::CLIENT_NUM);
    let ladder_hit = |t: &super::trace::Trace| {
        // (`fcomp` 1, test ah,5, jp: only below hit; NaN didn't.)
        l(t.fraction()) < l(1.0) && t.int(super::trace::at::SURFACE_FLAGS) & SURF_LADDER != 0
    };
    let spot: [f32; 3] =
        std::array::from_fn(|i| (l(dir[i]) * l(trace_dist) + l(origin[i])).store());
    let mask = pm.int(pm::TRACE_MASK);
    let trace = player_trace(pm, &origin, &mins, &maxs, &spot, client, mask, host);
    let backing = walking && (pm.cmd()[cmd::FORWARD_MOVE] as i8) <= 0;
    if ladder_hit(&trace) && !backing {
        if pm.ps.pm_flags() & pm_flags::LADDER != 0 {
            return;
        }
        pm.ps.set_vec3(ps::LADDER_VEC, trace.normal());
        let ladder = pm.ps.vec3(ps::LADDER_VEC);
        let into: [f32; 3] =
            std::array::from_fn(|i| ((-l(ladder[i])) * l(trace_dist) + l(origin[i])).store());
        let mask = pm.int(pm::TRACE_MASK);
        let again = player_trace(pm, &origin, &mins, &maxs, &into, client, mask, host);
        if ladder_hit(&again) {
            let flags = pm.ps.pm_flags() | pm_flags::LADDER;
            pm.ps.set_int(ps::PM_FLAGS, flags);
            return;
        }
    }
    leave_ladder(&mut pm.ps);
    if hanging {
        // (`BG_AnimScriptEvent`, inlined: the same checks.)
        host.anim_script_event(&mut pm.ps, ANIM_ET_JUMP, false, true);
    }
}
