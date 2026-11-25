//! The ground under the player (`bg_pmove.c`'s ground trace): the trace
//! down that finds it, the way out of a solid, leaving it, and landing on
//! it (the fall's damage, slowdown and events). Call of Duty 2's versions;
//! see [`super::pmove`] for the conventions.

use super::pmove::{
    add_event, add_predictable_event, add_touch_ent, cmd, player_trace, pm, pm_flags, pm_type, pml,
    ps, MoveLocal, MoveSettings, PlayerState, Pmove, ENTITY_NONE, EV_FOOTSTEP_RUN,
    EV_FOOTSTEP_WALK, E_FLAGS_MOUNTED, SURF_NODAMAGE, SURF_NOSTEPS, SURF_SLICK,
};
use super::trace::{MoveHost, Trace, ANIM_ET_JUMP, ANIM_ET_JUMPBK, ANIM_ET_LAND};
use crate::net::msg::ftol2;
use crate::x87::F24;
use std::cmp::Ordering;

/// The first landing events (each `+` the ground's surface type): a hard
/// landing (`EV_LANDING`?, its parameter how hard), and one that hurts
/// (its parameter the damage). Our names.
pub const EV_LANDING: i32 = 0x5D;
pub const EV_LANDING_PAIN: i32 = 0x74;

/// What [`crash_land`] warns of when the fall damage's heights are the
/// wrong way round (the game's text).
pub const FALL_DAMAGE_HEIGHTS_WARNING: &[u8] =
    b"bg_fallDamageMaxHeight must be greater than bg_fallDamageMinHeight\n";

/// The steps [`correct_all_solid`] tries out of a solid, in its order (the
/// game's table at 0x005A1490: up, the four sides up, the four sides, down,
/// the four sides down, then the corners up, level and down).
const ALL_SOLID_STEPS: [[f32; 3]; 26] = [
    [0.0, 0.0, 1.0],
    [-1.0, 0.0, 1.0],
    [0.0, -1.0, 1.0],
    [1.0, 0.0, 1.0],
    [0.0, 1.0, 1.0],
    [-1.0, 0.0, 0.0],
    [0.0, -1.0, 0.0],
    [1.0, 0.0, 0.0],
    [0.0, 1.0, 0.0],
    [0.0, 0.0, -1.0],
    [-1.0, 0.0, -1.0],
    [0.0, -1.0, -1.0],
    [1.0, 0.0, -1.0],
    [0.0, 1.0, -1.0],
    [-1.0, -1.0, 1.0],
    [1.0, -1.0, 1.0],
    [1.0, 1.0, 1.0],
    [-1.0, 1.0, 1.0],
    [-1.0, -1.0, 0.0],
    [1.0, -1.0, 0.0],
    [1.0, 1.0, 0.0],
    [-1.0, 1.0, 0.0],
    [-1.0, -1.0, -1.0],
    [1.0, -1.0, -1.0],
    [1.0, 1.0, -1.0],
    [-1.0, 1.0, -1.0],
];

/// `a` above `b`, or either NaN (an `fcomp` whose C0 and C3 are both clear
/// or both set: neither below nor equal).
fn above_or_unordered(a: F24, b: F24) -> bool {
    !matches!(a.partial_cmp(&b), Some(Ordering::Less | Ordering::Equal))
}

/// The ground's surface type for a landing: 0 on a surface without
/// footsteps, else the surface flags' bits 20 to 24.
fn landing_surface(pml: &MoveLocal) -> i32 {
    let flags = pml.int(pml::GROUND_SURFACE_FLAGS);
    if flags & SURF_NOSTEPS != 0 {
        0
    } else {
        (flags >> 20) & 0x1F
    }
}

/// The event of a fall of 8 to 12 units: a running footstep on the
/// ground's surface type ([`EV_FOOTSTEP_RUN`] + the type); none (0) on a
/// surface without footsteps or of type 0.
///
/// Original: `PM_MediumLandingForSurface` at 0x005169C0 (CoD2MP_s.exe 1.3; our name; pml in eax)
pub fn medium_landing_for_surface(pml: &MoveLocal) -> i32 {
    match landing_surface(pml) {
        0 => 0,
        surface => surface + EV_FOOTSTEP_RUN,
    }
}

/// The event of a fall of more than 12 units: [`EV_LANDING`] + the
/// ground's surface type (type 0 on a surface without footsteps).
///
/// Original: `PM_HardLandingForSurface` at 0x005169E0 (CoD2MP_s.exe 1.3; our name; pml in eax)
pub fn hard_landing_for_surface(pml: &MoveLocal) -> i32 {
    EV_LANDING + landing_surface(pml)
}

/// Landing: the height the player fell, from the velocity he lands at
/// (solving the fall from the previous origin under `gravity`; none when
/// the motion can't reach the ground: `b² - 4ac` below 0), squared over
/// `2 gravity`. Then:
///
/// - the damage: none at or below `bg_fallDamageMinHeight`, on a surface
///   without damage or for the dead; 100 at `bg_fallDamageMaxHeight` and
///   above; between, the percentage of the way (truncated; above 0 and
///   at most 100 kept). With the heights the wrong way round the host
///   prints [`FALL_DAMAGE_HEIGHTS_WARNING`] and there is none.
/// - above 12 units, the landing's animation ([`ANIM_ET_LAND`], forced),
///   and how hard it was: `((height - 12) / 26 + 1) * 4` (truncated, at
///   most 24).
/// - with damage: below 100 and not on a slick surface, `pm_time` `damage
///   * 35 + 500` ms (at most 2000) of [`pm_flags::SLIDE`] and the
///   velocity times 0.5 (to 500 ms), 0.2 (from 1500 ms), between them
///   falling evenly; else the velocity times 0.67. Then the pain event
///   ([`EV_LANDING_PAIN`] + the surface type), the damage its parameter.
/// - without: nothing to 4 units; below 8 a walking footstep, below 12 a
///   running one ([`medium_landing_for_surface`]); else the velocity
///   times 0.67 and [`hard_landing_for_surface`] with how hard it was.
///
/// Original: `PM_CrashLand` at 0x00516A20 (CoD2MP_s.exe 1.3; Quake III's name; ps in esi, pml on the stack)
pub fn crash_land(
    ps: &mut PlayerState,
    pml: &MoveLocal,
    settings: &MoveSettings,
    host: &mut dyn MoveHost,
) {
    let l = F24::load;
    let gravity = F24::int(ps.int(ps::GRAVITY));
    let vel = pml.float(pml::PREVIOUS_VELOCITY + 8);
    let acc = (-gravity).store();
    let a = (-gravity * l(0.5)).store();
    let c = l(pml.float(pml::PREVIOUS_ORIGIN + 8)) - l(ps.float(ps::ORIGIN + 8));
    let den = l(vel) * l(vel) - c * l(a) * l(4.0);
    // (`fcom` 0, test ah,5, jnp: only below returns; NaN goes on.)
    if den < l(0.0) {
        return;
    }
    let t = (-l(vel) - den.sqrt()) / (l(a) + l(a));
    let landing = (t * l(acc) + l(vel)) * l(-1.0);
    let height = l((landing * landing / (gravity + gravity)).store());

    let (min, max) = (
        l(settings.fall_damage_min_height),
        l(settings.fall_damage_max_height),
    );
    let mut damage = 0;
    // (`fcomp` min of max, test ah,0x41, jp: at or below warns; NaN goes on.)
    if max <= min {
        host.print(FALL_DAMAGE_HEIGHTS_WARNING);
    // (`fcomp` min, test ah,0x41, jnp: at or below is none; NaN goes on.)
    } else if above_or_unordered(height, min)
        && pml.int(pml::GROUND_SURFACE_FLAGS) & SURF_NODAMAGE == 0
        && ps.pm_type() < pm_type::DEAD
    {
        // (`fcomp` max, test ah,1, jne: below, or NaN, is a share.)
        damage = if height >= max {
            100
        } else {
            let n = ftol2(((height - min) / (max - min) * l(100.0)).0) as i32;
            let capped = if n.wrapping_sub(100) >= 0 { 100 } else { n };
            if n.wrapping_neg() >= 0 {
                0
            } else {
                capped
            }
        };
    }

    let mut hard = 0;
    // (`fcomp` 12, test ah,0x41, jp: above, or NaN, lands hard.)
    if above_or_unordered(height, l(12.0)) {
        let n = ftol2((((height - l(12.0)) * l(0.038_461_54) + l(1.0)) * l(4.0)).0) as i32;
        hard = n.min(24);
        // (`BG_AnimScriptEvent`, inlined: the same checks.)
        host.anim_script_event(ps, ANIM_ET_LAND, false, true);
    }

    if damage != 0 {
        if damage < 100 && pml.int(pml::GROUND_SURFACE_FLAGS) & SURF_SLICK == 0 {
            let time = damage * 35 + 500;
            let (scale, time) = if time > 2000 {
                (l(0.2), 2000)
            } else if time <= 500 {
                (l(0.5), time)
            } else if time >= 1500 {
                (l(0.2), time)
            } else {
                (
                    l(0.5) - (F24::int(time) - l(500.0)) * l(0.001) * l(0.3),
                    time,
                )
            };
            ps.set_int(ps::PM_TIME, time);
            let flags = ps.pm_flags() | pm_flags::SLIDE;
            ps.set_int(ps::PM_FLAGS, flags);
            let v = ps.velocity();
            ps.set_velocity(v.map(|c| (scale * l(c)).store()));
        } else {
            let v = ps.velocity();
            ps.set_velocity(v.map(|c| (l(c) * l(0.67)).store()));
        }
        add_predictable_event(EV_LANDING_PAIN + landing_surface(pml), damage, ps);
        return;
    }

    // (`fcomp` 4, test ah,0x41, jne: at or below, or NaN, is nothing.)
    if !matches!(height.partial_cmp(&l(4.0)), Some(Ordering::Greater)) {
        return;
    }
    // (`fcomp` 8, then 12, test ah,5, jp: only below; NaN can't be here.)
    if height < l(8.0) {
        let surface = landing_surface(pml);
        add_event(
            ps,
            if surface == 0 {
                0
            } else {
                surface + EV_FOOTSTEP_WALK
            },
        );
    } else if height < l(12.0) {
        add_event(ps, medium_landing_for_surface(pml));
    } else {
        let v = ps.velocity();
        ps.set_velocity(v.map(|c| (l(c) * l(0.67)).store()));
        add_predictable_event(hard_landing_for_surface(pml), hard, ps);
    }
}

/// Off the ground ([`ENTITY_NONE`], not on or near a surface, not
/// walking): what the ground trace leaves when there is none or it can't
/// be stood on.
fn leave_ground(ps: &mut PlayerState, pml: &mut MoveLocal) {
    ps.set_int(ps::GROUND_ENTITY_NUM, ENTITY_NONE);
    for at in [pml::GROUND_PLANE, pml::ALMOST_GROUND_PLANE, pml::WALKING] {
        pml.set_int(at, 0);
    }
}

/// The jump's flag and height cleared ([`pm_flags::JUMPING`],
/// `jumpOriginZ`).
fn end_jump(ps: &mut PlayerState) {
    let flags = ps.pm_flags() & !pm_flags::JUMPING;
    ps.set_int(ps::PM_FLAGS, flags);
    ps.set_int(ps::JUMP_ORIGIN_Z, 0);
}

/// The way out when the player's box is all inside something solid: a
/// trace of the box where it is, one unit off in each of 26 directions
/// (the game's order, [`ALL_SOLID_STEPS`]); at the first that doesn't start
/// solid, the player moves there, then down as far as a trace one unit down
/// goes (that trace the ground's, and `trace`'s). None: off the ground, the
/// jump ended, false.
///
/// Original: `PM_CorrectAllSolid` at 0x00516DA0 (CoD2MP_s.exe 1.3; Quake III's name; pm in ecx, trace in eax, pml on the stack)
pub fn correct_all_solid(
    pm: &mut Pmove,
    pml: &mut MoveLocal,
    trace: &mut Trace,
    host: &mut dyn MoveHost,
) -> bool {
    let l = F24::load;
    for step in ALL_SOLID_STEPS {
        let mask = pm.int(pm::TRACE_MASK);
        let origin = pm.ps.vec3(ps::ORIGIN);
        let point: [f32; 3] = std::array::from_fn(|i| (l(step[i]) + l(origin[i])).store());
        let client = pm.ps.int(ps::CLIENT_NUM);
        let (mins, maxs) = (pm.vec3(pm::MINS), pm.vec3(pm::MAXS));
        *trace = player_trace(pm, &point, &mins, &maxs, &point, client, mask, host);
        if trace.start_solid() {
            continue;
        }
        pm.ps.set_vec3(ps::ORIGIN, point);
        let down = [point[0], point[1], (l(point[2]) - l(1.0)).store()];
        let mask = pm.int(pm::TRACE_MASK);
        let client = pm.ps.int(ps::CLIENT_NUM);
        *trace = player_trace(pm, &point, &mins, &maxs, &down, client, mask, host);
        pml.set_ground_trace(trace);
        let f = l(trace.fraction());
        let origin = pm.ps.vec3(ps::ORIGIN);
        let moved: [f32; 3] =
            std::array::from_fn(|i| ((l(down[i]) - l(origin[i])) * f + l(origin[i])).store());
        pm.ps.set_vec3(ps::ORIGIN, moved);
        return true;
    }
    leave_ground(&mut pm.ps, pml);
    end_jump(&mut pm.ps);
    false
}

/// No ground below: off it, not walking. Stepping off the ground (it was
/// on some), a trace of the box from the origin to 64 units down (by the
/// game's trace function itself, not [`player_trace`]): nothing there
/// plays the jump's animation (backward for a backward move, forced) and
/// leaves `almostGroundPlane` 0; something within 1/64 of the way sets it.
/// Already off: a trace one unit down; `almostGroundPlane` is whether it
/// hit anything.
///
/// Original: `PM_GroundTraceMissed` at 0x00516F10 (CoD2MP_s.exe 1.3; Quake III's name; pm in eax, pml in ebx)
pub fn ground_trace_missed(pm: &mut Pmove, pml: &mut MoveLocal, host: &mut dyn MoveHost) {
    let l = F24::load;
    let origin = pm.ps.vec3(ps::ORIGIN);
    let handler = pm.bytes[pm::HANDLER];
    let mask = pm.int(pm::TRACE_MASK);
    let client = pm.ps.int(ps::CLIENT_NUM);
    let (mins, maxs) = (pm.vec3(pm::MINS), pm.vec3(pm::MAXS));
    if pm.ps.int(ps::GROUND_ENTITY_NUM) != ENTITY_NONE {
        let point = [origin[0], origin[1], (l(origin[2]) - l(64.0)).store()];
        let trace = host.trace(handler, &origin, &mins, &maxs, &point, client, mask);
        let fraction = l(trace.fraction());
        // (`fucompp` with 1.0, test ah,0x44, jp: only equal falls; NaN doesn't.)
        if fraction == F24::double(1.0) {
            let event = if (pm.cmd()[cmd::FORWARD_MOVE] as i8) < 0 {
                ANIM_ET_JUMPBK
            } else {
                ANIM_ET_JUMP
            };
            // (`BG_AnimScriptEvent`, inlined: the same checks.)
            host.anim_script_event(&mut pm.ps, event, false, true);
            pml.set_int(pml::ALMOST_GROUND_PLANE, 0);
        } else {
            // (`fcomp` 1/64, test ah,5, jp: only below is near; NaN isn't.)
            let near = fraction < l(0.015_625);
            pml.set_int(pml::ALMOST_GROUND_PLANE, i32::from(near));
        }
    } else {
        let point = [origin[0], origin[1], (l(origin[2]) - l(1.0)).store()];
        let trace = host.trace(handler, &origin, &mins, &maxs, &point, client, mask);
        // (`fucompp` with 1.0, test ah,0x44, jp: unequal, or NaN, hit something.)
        let hit = l(trace.fraction()) != l(1.0);
        pml.set_int(pml::ALMOST_GROUND_PLANE, i32::from(hit));
    }
    pm.ps.set_int(ps::GROUND_ENTITY_NUM, ENTITY_NONE);
    pml.set_int(pml::GROUND_PLANE, 0);
    pml.set_int(pml::WALKING, 0);
}

/// The ground under the player: a trace of the box from a quarter unit
/// above the origin to a quarter below (on a mounted weapon from the origin
/// to a unit below), kept as the ground's. All in a solid:
/// [`correct_all_solid`] (no way out ends the move). Starting in one: a
/// trace again from just below the origin (0.001 units; still starting in
/// one: off the ground). Nothing hit: [`ground_trace_missed`]. Moving up
/// and off the surface (the velocity's upward part and its part along the
/// normal both positive, the latter above 10; not on a ladder): off the
/// ground with the jump's animation (backward for a backward move). Too
/// steep to stand on (the normal's height below 0.7): on a surface but not
/// walking, off the ground, the jump ended. Else on the ground: walking,
/// [`crash_land`] if it was off the ground, the entity hit the ground's and
/// touched.
///
/// Original: `PM_GroundTrace` at 0x00517080 (CoD2MP_s.exe 1.3; Quake III's name; pm, pml on the stack)
pub fn ground_trace(
    pm: &mut Pmove,
    pml: &mut MoveLocal,
    settings: &MoveSettings,
    host: &mut dyn MoveHost,
) {
    let l = F24::load;
    let origin = pm.ps.vec3(ps::ORIGIN);
    let (mut start, end) = if pm.ps.int(ps::E_FLAGS) & E_FLAGS_MOUNTED != 0 {
        (
            origin,
            [origin[0], origin[1], (l(origin[2]) - l(1.0)).store()],
        )
    } else {
        (
            [origin[0], origin[1], (l(origin[2]) + l(0.25)).store()],
            [origin[0], origin[1], (l(origin[2]) - l(0.25)).store()],
        )
    };
    let (mins, maxs) = (pm.vec3(pm::MINS), pm.vec3(pm::MAXS));
    let mask = pm.int(pm::TRACE_MASK);
    let client = pm.ps.int(ps::CLIENT_NUM);
    let mut trace = player_trace(pm, &start, &mins, &maxs, &end, client, mask, host);
    pml.set_ground_trace(&trace);
    if trace.all_solid() && !correct_all_solid(pm, pml, &mut trace, host) {
        return;
    }
    if trace.start_solid() {
        // (Only the height is the origin's now: the rest stays.)
        start[2] = (l(pm.ps.float(ps::ORIGIN + 8)) - l(0.001)).store();
        let mask = pm.int(pm::TRACE_MASK);
        let client = pm.ps.int(ps::CLIENT_NUM);
        trace = player_trace(pm, &start, &mins, &maxs, &end, client, mask, host);
        if trace.start_solid() {
            leave_ground(&mut pm.ps, pml);
            return;
        }
        pml.set_ground_trace(&trace);
    }
    // (`fucompp` with 1.0, test ah,0x44, jp: only equal misses; NaN doesn't.)
    if l(trace.fraction()) == F24::double(1.0) {
        ground_trace_missed(pm, pml, host);
        return;
    }

    let v = pm.ps.velocity();
    let n = trace.normal();
    // (`fcomp` 0, then 10, test ah,0x41, jne: only above; NaN isn't.)
    if pm.ps.pm_flags() & pm_flags::LADDER == 0
        && l(v[2]) > l(0.0)
        && l(n[1]) * l(v[1]) + l(n[2]) * l(v[2]) + l(n[0]) * l(v[0]) > l(10.0)
    {
        let event = if (pm.cmd()[cmd::FORWARD_MOVE] as i8) < 0 {
            ANIM_ET_JUMPBK
        } else {
            ANIM_ET_JUMP
        };
        // (`BG_AnimScriptEvent`, inlined: the same checks.)
        host.anim_script_event(&mut pm.ps, event, false, false);
        leave_ground(&mut pm.ps, pml);
        return;
    }
    // (`fcomp` 0.7, test ah,5, jp: only below is too steep; NaN isn't.)
    if l(n[2]) < l(0.7) {
        pm.ps.set_int(ps::GROUND_ENTITY_NUM, ENTITY_NONE);
        pml.set_int(pml::GROUND_PLANE, 1);
        pml.set_int(pml::ALMOST_GROUND_PLANE, 1);
        pml.set_int(pml::WALKING, 0);
        end_jump(&mut pm.ps);
        return;
    }
    pml.set_int(pml::GROUND_PLANE, 1);
    pml.set_int(pml::ALMOST_GROUND_PLANE, 1);
    pml.set_int(pml::WALKING, 1);
    if pm.ps.int(ps::GROUND_ENTITY_NUM) == ENTITY_NONE {
        crash_land(&mut pm.ps, pml, settings, host);
    }
    let entity = i32::from(trace.entity_num());
    pm.ps.set_int(ps::GROUND_ENTITY_NUM, entity);
    add_touch_ent(pm, entity);
}
