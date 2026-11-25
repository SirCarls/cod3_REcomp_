//! Jumping (Call of Duty 4 keeps it in `bg_jump.cpp`; Call of Duty 2's is
//! among its movement code): starting a jump from the ground or a ladder,
//! and the slowdown after landing. See [`super::pmove`] for the
//! conventions; the jump's first functions ([`super::pmove::jump_reduce_friction`],
//! [`super::pmove::jump_get_step_height`], [`super::pmove::jump_clamp_velocity`])
//! are there.

use super::pmove::{
    add_event, button, cmd, effective_stance, pm, pm_flags, pm_type, pml, ps, stance, MoveLocal,
    MoveSettings, PlayerState, Pmove, ENTITY_NONE, LADDER_SURFACE_TYPE, SURF_NOSTEPS,
};
use super::trace::{MoveHost, ANIM_ET_JUMP, ANIM_ET_JUMPBK};
use crate::shared::math::vec3_normalize;
use crate::x87::F24;

/// The first of the jump events, one for each surface type (our name; the
/// type is added: [`LADDER_SURFACE_TYPE`] off a ladder).
pub const EV_JUMP: i32 = 0x46;

/// How long a jump may wait for its landing's slowdown (`pm_time`,
/// milliseconds): past it the jump ends.
const JUMP_LAND_TIME: i32 = 1800;

/// Starts the landing's slowdown when none runs (`pm_time` 0):
/// [`pm_flags::JUMPING`] and 1.8 s of `pm_time`.
///
/// Original: `Jump_ActivateSlowdown` at 0x0052FC40 (CoD2MP_s.exe 1.3; Call of Duty 4's name; ps in eax)
pub fn jump_activate_slowdown(ps: &mut PlayerState) {
    if ps.int(ps::PM_TIME) == 0 {
        let flags = ps.pm_flags() | pm_flags::JUMPING;
        ps.set_int(ps::PM_FLAGS, flags);
        ps.set_int(ps::PM_TIME, JUMP_LAND_TIME);
    }
}

/// The landing's slowdown: once `pm_time` has run out, the velocity times
/// 0.65 for 1.8 s when the player lands more than 18 units below where he
/// jumped, else (no lower; or NaN) 0.5 for 1.2 s; after 1.8 s of `pm_time`
/// the jump ends (`PMF_JUMPING` and `jumpOriginZ` cleared) at 0.65; while
/// `pm_time` runs, 1. With `jump_slowdownEnable` off, 1 (the time still
/// set). Each component is stored as a `float`.
///
/// Original: `Jump_ApplySlowdown` at 0x0052FC60 (CoD2MP_s.exe 1.3; Call of Duty 4's name; ps in ecx)
pub fn jump_apply_slowdown(ps: &mut PlayerState, settings: &MoveSettings) {
    let l = F24::load;
    let time = ps.int(ps::PM_TIME);
    let mut scale = l(1.0);
    if time > JUMP_LAND_TIME {
        let flags = ps.pm_flags() & !pm_flags::JUMPING;
        ps.set_int(ps::PM_FLAGS, flags);
        ps.set_int(ps::JUMP_ORIGIN_Z, 0);
        scale = l(0.65);
    } else if time == 0 {
        let top = l(ps.float(ps::JUMP_ORIGIN_Z)) + l(18.0);
        // (`fcomp` origin[2], test ah,0x41, jne: at or below it, or NaN, is
        // the short one.)
        if top > l(ps.float(ps::ORIGIN + 8)) {
            scale = l(0.65);
            ps.set_int(ps::PM_TIME, JUMP_LAND_TIME);
        } else {
            scale = l(0.5);
            ps.set_int(ps::PM_TIME, 1200);
        }
    }
    if !settings.jump_slowdown_enable {
        scale = l(1.0);
    }
    let v = ps.velocity();
    ps.set_velocity(v.map(|c| (scale * l(c)).store()));
}

/// How much a jump soon after landing loses: with `jump_slowdownEnable`
/// off, nothing (1); from 1.7 s of `pm_time` on 2.5; before,
/// `pm_time * 0.00088235294 + 1`. The result is the x87 register's.
///
/// Original: `Jump_GetLandFactor` at 0x0052FDF0 (CoD2MP_s.exe 1.3; Call of Duty 4's name; ps on the stack, the result on the x87 stack)
pub fn jump_get_land_factor(ps: &PlayerState, settings: &MoveSettings) -> F24 {
    if !settings.jump_slowdown_enable {
        return F24::load(1.0);
    }
    let time = ps.int(ps::PM_TIME);
    if time >= 1700 {
        return F24::load(2.5);
    }
    F24::int(time) * F24::load(0.000_882_352_94) + F24::load(1.0)
}

/// Starts a jump `height` high: the upward speed `sqrt(gravity * (height +
/// height))` (the square stored as a `float` and divided by
/// [`jump_get_land_factor`] while a landing's slowdown runs); off the
/// ground (`walking`, `groundPlane`, `almostGroundPlane` 0, no ground
/// entity); the jump's time and height kept, `pm_time` 0, `PMF_JUMPING`
/// on; and the aim's spread grows by `jump_spreadAdd` (at most 255).
///
/// Original: `Jump_Start` at 0x0052FE30 (CoD2MP_s.exe 1.3; Call of Duty 4's name; pm in edi, pml in esi, height on the stack)
pub fn jump_start(pm: &mut Pmove, pml: &mut MoveLocal, height: f32, settings: &MoveSettings) {
    let l = F24::load;
    let time = pm.server_time();
    let ps = &mut pm.ps;
    let h = l(height);
    let squared = F24::int(ps.int(ps::GRAVITY)) * (h + h);
    let squared = if ps.pm_flags() & pm_flags::JUMPING != 0 && ps.int(ps::PM_TIME) <= JUMP_LAND_TIME
    {
        l(squared.store()) / jump_get_land_factor(ps, settings)
    } else {
        squared
    };
    let speed = squared.sqrt();
    for at in [pml::GROUND_PLANE, pml::ALMOST_GROUND_PLANE, pml::WALKING] {
        pml.set_int(at, 0);
    }
    ps.set_int(ps::GROUND_ENTITY_NUM, ENTITY_NONE);
    ps.set_int(ps::JUMP_TIME, time);
    let z = ps.int(ps::ORIGIN + 8);
    ps.set_int(ps::JUMP_ORIGIN_Z, z);
    ps.set_int(ps::PM_TIME, 0);
    let flags = ps.pm_flags() | pm_flags::JUMPING;
    ps.set_int(ps::PM_FLAGS, flags);
    ps.set_float(ps::VELOCITY + 8, speed.store());
    let spread = l(settings.jump_spread_add) + l(ps.float(ps::AIM_SPREAD_SCALE));
    ps.set_float(ps::AIM_SPREAD_SCALE, spread.store());
    // (`fcomp` 255 of the register, test ah,0x41, jne: only above caps; NaN doesn't.)
    if spread > l(255.0) {
        ps.set_float(ps::AIM_SPREAD_SCALE, 255.0);
    }
}

/// The jump's event: off a ladder [`EV_JUMP`] + [`LADDER_SURFACE_TYPE`];
/// else [`EV_JUMP`] + the ground's surface type, none on a surface without
/// footsteps or of type 0.
///
/// Original: `Jump_AddSurfaceEvent` at 0x0052FFB0 (CoD2MP_s.exe 1.3; Call of Duty 4's name; ps in eax, pml on the stack)
pub fn jump_add_surface_event(ps: &mut PlayerState, pml: &MoveLocal) {
    if ps.pm_flags() & pm_flags::LADDER != 0 {
        add_event(ps, EV_JUMP + LADDER_SURFACE_TYPE);
        return;
    }
    let flags = pml.int(pml::GROUND_SURFACE_FLAGS);
    if flags & SURF_NOSTEPS != 0 {
        return;
    }
    let surface = (flags >> 20) & 0x1F;
    if surface == 0 {
        return;
    }
    add_event(ps, EV_JUMP + surface);
}

/// A jump off a ladder: the upward speed times 0.75; the horizontal speed
/// `jump_ladderPushVel` along the view's level forward (normalized), or,
/// when the view faces the ladder (forward against the ladder's direction
/// below 0), along that direction mirrored in the ladder (normalized); off
/// the ladder.
///
/// Original: `Jump_PushOffLadder` at 0x0052FED0 (CoD2MP_s.exe 1.3; Call of Duty 4's name; ps in edi, pml in ebx)
pub fn jump_push_off_ladder(ps: &mut PlayerState, pml: &MoveLocal, settings: &MoveSettings) {
    let l = F24::load;
    let up = (l(ps.float(ps::VELOCITY + 8)) * l(0.75)).store();
    ps.set_float(ps::VELOCITY + 8, up);
    let forward = pml.vec3(pml::FORWARD);
    let mut flat = [forward[0], forward[1], 0.0];
    vec3_normalize(&mut flat);
    let ladder = ps.vec3(ps::LADDER_VEC);
    let facing =
        l(ladder[2]) * l(forward[2]) + l(ladder[1]) * l(forward[1]) + l(ladder[0]) * l(forward[0]);
    // (`fcomp` 0, test ah,5, jp: only below mirrors; NaN doesn't.)
    let (x, y) = if facing < l(0.0) {
        let d = (l(flat[2]) * l(ladder[2]) + l(flat[1]) * l(ladder[1]) + l(flat[0]) * l(ladder[0]))
            * l(-2.0);
        let mut push = [0f32; 3];
        for i in 0..3 {
            push[i] = (d * l(ladder[i]) + l(flat[i])).store();
        }
        vec3_normalize(&mut push);
        (push[0], push[1])
    } else {
        (flat[0], flat[1])
    };
    let speed = l(settings.jump_ladder_push_vel);
    ps.set_float(ps::VELOCITY, (l(x) * speed).store());
    ps.set_float(ps::VELOCITY + 4, (l(y) * speed).store());
    let flags = ps.pm_flags() & !pm_flags::LADDER;
    ps.set_int(ps::PM_FLAGS, flags);
}

/// Whether the command jumps, and if so the jump: not within 0.5 s of the
/// last one, nor with [`pm_flags::NO_JUMP`], dead, crouching or lying down;
/// the jump button pressed (held from the previous command, it is cleared
/// instead). Then [`jump_start`] to `jump_height`, [`jump_add_surface_event`],
/// [`jump_push_off_ladder`] on a ladder, and the jump's animation (backward
/// for a backward move).
///
/// Original: `Jump_Check` at 0x00530020 (CoD2MP_s.exe 1.3; Call of Duty 4's name; pml in eax, pm on the stack; al)
pub fn jump_check(
    pm: &mut Pmove,
    pml: &mut MoveLocal,
    settings: &MoveSettings,
    host: &mut dyn MoveHost,
) -> bool {
    if pm.server_time().wrapping_sub(pm.ps.int(ps::JUMP_TIME)) < 500
        || pm.ps.pm_flags() & pm_flags::NO_JUMP != 0
        || pm.ps.pm_type() >= pm_type::DEAD
        || effective_stance(&pm.ps) != stance::STAND
    {
        return false;
    }
    let buttons = pm.int(pm::CMD + cmd::BUTTONS);
    if buttons & button::JUMP == 0 {
        return false;
    }
    if pm.int(pm::OLD_CMD + cmd::BUTTONS) & button::JUMP != 0 {
        pm.set_int(pm::CMD + cmd::BUTTONS, buttons & !button::JUMP);
        return false;
    }
    jump_start(pm, pml, settings.jump_height, settings);
    jump_add_surface_event(&mut pm.ps, pml);
    if pm.ps.pm_flags() & pm_flags::LADDER != 0 {
        jump_push_off_ladder(&mut pm.ps, pml, settings);
    }
    let event = if (pm.cmd()[cmd::FORWARD_MOVE] as i8) < 0 {
        ANIM_ET_JUMPBK
    } else {
        ANIM_ET_JUMP
    };
    host.anim_script_event(&mut pm.ps, event, false, true);
    true
}
