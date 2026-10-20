//! Sliding along what the player runs into (`bg_slidemove.c`): the move
//! traced in up to four steps, the velocity clipped against each surface
//! hit, Call of Duty 2's versions (see [`super::pmove`] for the
//! conventions).

use super::pmove::{
    add_touch_ent, clip_velocity, footstep_event, jump_clamp_velocity, jump_get_step_height,
    player_trace, pm, pm_flags, pm_type, pml, ps, should_make_footsteps, MoveLocal, MoveSettings,
    Pmove, ENTITY_NONE, MAX_PS_EVENTS,
};
use super::prone::verify_prone_position;
use super::trace::MoveHost;
use crate::shared::math::{round_float_to_int, vec3_normalize, vec3_normalize_to};
use crate::x87::F24;

/// The most surfaces one slide move clips against (`MAX_CLIP_PLANES`).
pub const MAX_CLIP_PLANES: usize = 8;
/// How many traces one slide move makes at most (`numbumps`).
const BUMPS: i32 = 4;

/// The order of `planes` from the one `velocity` heads into most (the
/// smallest dot product) to the least, and that smallest dot product (each
/// `p . v` stored as a `float`; a plane goes before others of the same or a
/// NaN product). `planes` is not empty.
///
/// Original: `PM_PermuteRestrictiveClipPlanes` at 0x005301F0 (CoD2MP_s.exe 1.3; Call of Duty 4's name; planes in edx, then (velocity, planeCount, permutation) on the stack; the smallest on the x87 stack)
pub fn permute_restrictive_clip_planes(
    velocity: &[f32; 3],
    planes: &[[f32; 3]],
    permutation: &mut [usize],
) -> f32 {
    let mut dots = [0f32; MAX_CLIP_PLANES];
    let [v0, v1, v2] = velocity.map(F24::load);
    for (i, p) in planes.iter().enumerate() {
        let [p0, p1, p2] = p.map(F24::load);
        dots[i] = (p0 * v0 + p2 * v2 + p1 * v1).store();
        // (Insertion: `fcomp`, test ah,5, jnp: only a smaller one stops.)
        let mut j = i;
        while j > 0 && at_least_f32(dots[permutation[j - 1]], dots[i]) {
            permutation[j] = permutation[j - 1];
            j -= 1;
        }
        permutation[j] = i;
    }
    dots[permutation[0]]
}

/// `a >= b` as `fcomp` + test ah,1 + je takes it (C0 clear): not below,
/// not unordered.
fn at_least_f32(a: f32, b: f32) -> bool {
    // (Shifts unless below: equal and NaN shift too.)
    !matches!(a.partial_cmp(&b), Some(std::cmp::Ordering::Less))
}

/// `a > b` (an `fcomp` + test ah,0x41 taking only "above"; NaN isn't).
fn above(a: F24, b: F24) -> bool {
    matches!(a.partial_cmp(&b), Some(std::cmp::Ordering::Greater))
}

fn at_least(a: F24, b: F24) -> bool {
    matches!(
        a.partial_cmp(&b),
        Some(std::cmp::Ordering::Greater | std::cmp::Ordering::Equal)
    )
}

/// A velocity clipped against a plane as the slide move does it, every
/// step stored: `b = backoff` (the dot product, in the caller's order),
/// `change = -(b - |b| * 0.001)`, `v + change * n`.
fn clip_stored(v: [f32; 3], n: [f32; 3], backoff: F24) -> [f32; 3] {
    let b = backoff.store();
    let a = F24::load(b).abs().store();
    let c = (F24::load(b) - F24::load(a) * F24::load(0.001)).store();
    let change = F24::load((-F24::load(c)).store());
    let mut out = v;
    for (o, (vc, nc)) in out.iter_mut().zip(v.into_iter().zip(n)) {
        *o = (change * F24::load(nc) + F24::load(vc)).store();
    }
    out
}

/// `a . b` summed as `(a0*b0 + a1*b1) + a2*b2` with the components in the
/// order given (`order` the three indices).
fn dot_in(a: &[f32; 3], b: &[f32; 3], order: [usize; 3]) -> F24 {
    let term = |i: usize| F24::load(a[i]) * F24::load(b[i]);
    term(order[0]) + term(order[1]) + term(order[2])
}

/// The move over this frame's time against the world: up to four traces
/// from the origin along the velocity; after each the origin advances by
/// the part covered, the entity hit counts as touched, and the velocity
/// is clipped against every surface hit so far (up to
/// [`MAX_CLIP_PLANES`], with the ground and the original direction as the
/// first; one hit twice is pushed off; a crease of two slides along it; a
/// third stops the player). With `gravity`, half of the frame's gravity
/// counts at once (the rest at the end), and on the ground the velocity
/// first slides along it. Trapped in something solid (`allsolid`): no
/// vertical speed. While `pm_time` runs, the velocity stays as it started.
/// Also sets `impactSpeed` (how hard the move hit, the largest of the
/// frame). True: the move hit something.
///
/// Original: `PM_SlideMove` at 0x005302A0 (CoD2MP_s.exe 1.3; Quake III's name; (pm, pml, gravity) on the stack)
pub fn slide_move(
    pm: &mut Pmove,
    pml: &mut MoveLocal,
    gravity: bool,
    host: &mut dyn MoveHost,
) -> bool {
    let mut primal_velocity = pm.ps.velocity();
    let mut end_velocity = primal_velocity;
    if gravity {
        let g = F24::int(pm.ps.int(ps::GRAVITY)).store();
        end_velocity[2] =
            (F24::load(end_velocity[2]) - F24::load(g) * F24::load(pml.frame_time())).store();
        primal_velocity[2] = end_velocity[2];
        let mut v = pm.ps.velocity();
        v[2] = ((F24::load(end_velocity[2]) + F24::load(v[2])) * F24::load(0.5)).store();
        pm.ps.set_velocity(v);
        if pml.int(pml::GROUND_PLANE) != 0 {
            let n = pml.vec3(pml::GROUND_NORMAL);
            let v = pm.ps.velocity();
            pm.ps
                .set_velocity(clip_stored(v, n, dot_in(&v, &n, [1, 2, 0])));
        }
    }

    let mut time_left = pml.frame_time();
    let mut planes = [[0f32; 3]; MAX_CLIP_PLANES];
    let mut count = 0;
    if pml.int(pml::GROUND_PLANE) != 0 {
        planes[0] = pml.vec3(pml::GROUND_NORMAL);
        count = 1;
    }
    let v = pm.ps.velocity();
    vec3_normalize_to(&v, &mut planes[count]);
    count += 1;

    let mut permutation = [0usize; MAX_CLIP_PLANES];
    let mut bump = 0;
    while bump < BUMPS {
        let origin = pm.ps.vec3(ps::ORIGIN);
        let v = pm.ps.velocity();
        let mut end = [0f32; 3];
        for i in 0..3 {
            end[i] = (F24::load(time_left) * F24::load(v[i]) + F24::load(origin[i])).store();
        }
        let mins = pm.vec3(pm::MINS);
        let maxs = pm.vec3(pm::MAXS);
        let pass = pm.ps.int(ps::CLIENT_NUM);
        let mask = pm.int(pm::TRACE_MASK);
        let trace = player_trace(pm, &origin, &mins, &maxs, &end, pass, mask, host);

        if trace.all_solid() {
            // Trapped in something solid: no vertical speed.
            let mut v = pm.ps.velocity();
            v[2] = 0.0;
            pm.ps.set_velocity(v);
            return true;
        }
        let fraction = trace.fraction();
        // (`fcomp` 0, test ah,0x41, jne: only more than 0 moves; NaN doesn't.)
        if F24::load(fraction) > F24::load(0.0) {
            let mut o = pm.ps.vec3(ps::ORIGIN);
            for i in 0..3 {
                o[i] = ((F24::load(end[i]) - F24::load(o[i])) * F24::load(fraction)
                    + F24::load(o[i]))
                .store();
            }
            pm.ps.set_vec3(ps::ORIGIN, o);
        }
        if fraction.to_bits() == 1f32.to_bits() {
            break;
        }
        add_touch_ent(pm, i32::from(trace.entity_num()));
        time_left = (F24::load(time_left) - F24::load(fraction) * F24::load(time_left)).store();
        if count >= MAX_CLIP_PLANES {
            // (This shouldn't really happen.)
            pm.ps.set_velocity([0.0; 3]);
            return true;
        }

        let normal = trace.normal();
        // The same plane as before: pushed off it a little.
        let same = planes[..count]
            .iter()
            .any(|p| dot_in(&normal, p, [0, 1, 2]) > F24::load(0.999));
        if same {
            let v = pm.ps.velocity();
            let mut v = clip_stored(v, normal, dot_in(&normal, &v, [1, 2, 0]));
            for i in 0..3 {
                v[i] = (F24::load(normal[i]) + F24::load(v[i])).store();
            }
            pm.ps.set_velocity(v);
            bump += 1;
            continue;
        }

        planes[count] = normal;
        count += 1;
        let v = pm.ps.velocity();
        let into = permute_restrictive_clip_planes(&v, &planes[..count], &mut permutation);
        // (`fcomp` 0.1, test ah,1, je: 0.1 or more moves on; NaN clips.)
        if at_least(F24::load(into), F24::load(0.1)) {
            bump += 1;
            continue;
        }
        let hit = (-F24::load(into)).store();
        // (`fcomp`, test ah,0x41, jne: only harder replaces it.)
        if F24::load(hit) > F24::load(pml.float(pml::IMPACT_SPEED)) {
            pml.set_float(pml::IMPACT_SPEED, hit);
        }

        let first = planes[permutation[0]];
        let mut clip_velocity = clip_stored(v, first, dot_in(&v, &first, [1, 0, 2]));
        let mut end_clip_velocity = clip_stored(
            end_velocity,
            first,
            dot_in(&end_velocity, &first, [0, 2, 1]),
        );
        for j in 1..count {
            let plane = planes[permutation[j]];
            // (`fcomp` 0.1, test ah,1, je: 0.1 or more needs no clip; NaN clips.)
            let d = dot_in(&clip_velocity, &plane, [2, 1, 0]);
            if at_least(d, F24::load(0.1)) {
                continue;
            }
            clip_velocity = clip_stored(
                clip_velocity,
                plane,
                dot_in(&clip_velocity, &plane, [0, 2, 1]),
            );
            end_clip_velocity = clip_stored(
                end_clip_velocity,
                plane,
                dot_in(&end_clip_velocity, &plane, [0, 2, 1]),
            );
            // Back into the first plane? (`fcomp` 0, test ah,1, je: 0 or
            // more is fine; NaN goes on.)
            let d = dot_in(&clip_velocity, &first, [2, 1, 0]);
            if at_least(d, F24::load(0.0)) {
                continue;
            }
            // Slide along the crease of the two.
            let (p, q) = (first.map(F24::load), plane.map(F24::load));
            let mut dir = [
                (q[2] * p[1] - p[2] * q[1]).store(),
                (p[2] * q[0] - q[2] * p[0]).store(),
                (q[1] * p[0] - q[0] * p[1]).store(),
            ];
            vec3_normalize(&mut dir);
            let v = pm.ps.velocity();
            let d = dot_in(&dir, &v, [2, 1, 0]).store();
            clip_velocity = dir.map(|c| (F24::load(c) * F24::load(d)).store());
            let d = dot_in(&dir, &end_velocity, [1, 0, 2]).store();
            end_clip_velocity = dir.map(|c| (F24::load(c) * F24::load(d)).store());
            // A third plane the new move goes into: stop dead.
            for (k, &other) in permutation.iter().enumerate().take(count).skip(1) {
                if k == j {
                    continue;
                }
                let d = dot_in(&clip_velocity, &planes[other], [2, 1, 0]);
                // (`fcomp` 0.1, test ah,1, jne: below or NaN stops.)
                if !at_least(d, F24::load(0.1)) {
                    pm.ps.set_velocity([0.0; 3]);
                    return true;
                }
            }
        }
        pm.ps.set_velocity(clip_velocity);
        end_velocity = end_clip_velocity;
        bump += 1;
    }

    if gravity {
        pm.ps.set_velocity(end_velocity);
    }
    // (While a timer runs the velocity isn't changed.)
    if pm.ps.int(ps::PM_TIME) != 0 {
        pm.ps.set_velocity(primal_velocity);
    }
    bump != 0
}

/// The event of a step the view smooths out (`EV_STEP_VIEW`; its parameter
/// the step's height + 128).
pub const EV_STEP_VIEW: i32 = 0x8F;
/// The entities below this are players (`MAX_CLIENTS`): a step down onto
/// one is undone.
pub const MAX_CLIENTS: u16 = 64;

/// [`slide_move`] that also climbs steps: when the slide hit something,
/// the move is made again from up to a step higher (18, 10 prone; while
/// jumping, what the jump's height still allows) and set down again (on
/// the ground also 9 lower), and kept when it got further along the
/// velocity than the plain slide (and, jumping, stayed below the jump's
/// height). A step down onto a player, or onto ground steeper than 0.3, is
/// undone. On a ladder only when climbing. A stepping player on the ground
/// then gets [`EV_STEP_VIEW`] for steps of more than 0.5, is slowed by the
/// step's height, and makes a footstep for steps of more than 3; a prone
/// one must still fit ([`verify_prone_position`]).
///
/// Original: `PM_StepSlideMove` at 0x00530AC0 (CoD2MP_s.exe 1.3; Quake III's name; (pm, pml, gravity) on the stack)
pub fn step_slide_move(
    pm: &mut Pmove,
    pml: &mut MoveLocal,
    gravity: bool,
    settings: &MoveSettings,
    host: &mut dyn MoveHost,
) {
    let l = F24::load;
    let flags = pm.ps.pm_flags();
    let mut jumping = false;
    let mut step = 0f32;
    let clear_jump = |pm: &mut Pmove| {
        let f = pm.ps.pm_flags() & !pm_flags::JUMPING;
        pm.ps.set_int(ps::PM_FLAGS, f);
        pm.ps.set_float(ps::JUMP_ORIGIN_Z, 0.0);
    };
    let had_ground = if flags & pm_flags::LADDER != 0 {
        clear_jump(pm);
        false
    } else if pml.int(pml::GROUND_PLANE) != 0 {
        true
    } else {
        if flags & pm_flags::JUMPING != 0 && pm.ps.int(ps::PM_TIME) != 0 {
            clear_jump(pm);
        }
        false
    };

    let start_o = pm.ps.vec3(ps::ORIGIN);
    let start_v = pm.ps.velocity();
    let bumped = slide_move(pm, pml, gravity, host);

    let flags = pm.ps.pm_flags();
    let mut step_size: f32 = if flags & pm_flags::PRONE != 0 {
        10.0
    } else {
        18.0
    };
    if pm.ps.int(ps::GROUND_ENTITY_NUM) == ENTITY_NONE {
        if flags & pm_flags::JUMPING != 0 && pm.ps.int(ps::PM_TIME) != 0 {
            clear_jump(pm);
        }
        let mut climb = false;
        if bumped
            && pm.ps.pm_flags() & pm_flags::JUMPING != 0
            && jump_get_step_height(&pm.ps, &start_o, &mut step_size, settings)
        {
            // (`fcomp` 1, test ah,5, jnp: below 1 stops; NaN goes on.)
            if l(step_size) < l(1.0) {
                return;
            }
            jumping = true;
            climb = true;
        }
        if !climb {
            // On a ladder only when climbing.
            if pm.ps.pm_flags() & pm_flags::LADDER == 0 {
                return;
            }
            if !above(l(pm.ps.velocity()[2]), l(0.0)) {
                return;
            }
        }
    }

    let down_o = pm.ps.vec3(ps::ORIGIN);
    let moved = [
        (l(down_o[0]) - l(start_o[0])).store(),
        (l(down_o[1]) - l(start_o[1])).store(),
    ];
    let down_v = pm.ps.velocity();
    let mins = pm.vec3(pm::MINS);
    let maxs = pm.vec3(pm::MAXS);
    let pass = pm.ps.int(ps::CLIENT_NUM);

    if bumped {
        // Up a step.
        let up = [
            start_o[0],
            start_o[1],
            (l(start_o[2]) + l(step_size) + l(1.0)).store(),
        ];
        let mask = pm.int(pm::TRACE_MASK);
        let trace = player_trace(pm, &start_o, &mins, &maxs, &up, pass, mask, host);
        let rise = (l(step_size) + l(1.0)) * l(trace.fraction()) - l(1.0);
        step = rise.store();
        // (`fcomp` 1, test ah,5, jp: 1 or more (or NaN) steps.)
        if rise < l(1.0) {
            step = 0.0;
        } else {
            pm.ps.set_vec3(
                ps::ORIGIN,
                [up[0], up[1], (l(start_o[2]) + l(step)).store()],
            );
            pm.ps.set_velocity(start_v);
            slide_move(pm, pml, gravity, host);
        }
    }

    // (`fucompp` with 0: equal skips; NaN steps down.)
    if had_ground || step != 0.0 || step.is_nan() {
        // Down again.
        let o = pm.ps.vec3(ps::ORIGIN);
        let mut down = [o[0], o[1], (l(o[2]) - l(step)).store()];
        if had_ground {
            down[2] = (l(down[2]) - l(9.0)).store();
        }
        let mask = pm.int(pm::TRACE_MASK);
        let trace = player_trace(pm, &o, &mins, &maxs, &down, pass, mask, host);
        if trace.entity_num() < MAX_CLIENTS {
            // Onto a player: undone.
            pm.ps.set_vec3(ps::ORIGIN, down_o);
            pm.ps.set_velocity(down_v);
            return;
        }
        // (`fcomp` 1, test ah,5, jp: 1 or more, or NaN, didn't hit.)
        if l(trace.fraction()) < l(1.0) {
            // (`fcomp` 0.3, test ah,5, jp: 0.3 or more (or NaN) stands.)
            if l(trace.normal()[2]) < l(0.3) {
                pm.ps.set_vec3(ps::ORIGIN, down_o);
                pm.ps.set_velocity(down_v);
                return;
            }
            let o = pm.ps.vec3(ps::ORIGIN);
            let f = l(trace.fraction());
            let mut set = [0f32; 3];
            for i in 0..3 {
                set[i] = ((l(down[i]) - l(o[i])) * f + l(o[i])).store();
            }
            pm.ps.set_vec3(ps::ORIGIN, set);
            let v = pm.ps.velocity();
            pm.ps.set_velocity(clip_velocity(&v, &trace.normal()));
        } else if step != 0.0 || step.is_nan() {
            let mut o = pm.ps.vec3(ps::ORIGIN);
            o[2] = (l(o[2]) - l(step)).store();
            pm.ps.set_vec3(ps::ORIGIN, o);
        }
    }

    // Did the step get further along the velocity than the plain slide?
    let o = pm.ps.vec3(ps::ORIGIN);
    let v = pm.ps.velocity();
    let stepped = (l(o[1]) - l(start_o[1])) * l(v[1]) + (l(o[0]) - l(start_o[0])) * l(v[0]);
    let plain = l(moved[1]) * l(v[1]) + l(moved[0]) * l(v[0]) + l(0.001);
    // (`fcompp`, test ah,1, je: the plain slide at least as far; NaN steps.)
    let mut keep = !matches!(
        plain.partial_cmp(&stepped),
        Some(std::cmp::Ordering::Greater | std::cmp::Ordering::Equal)
    );
    if keep && jumping {
        // (Below the jump's height, or NaN, keeps it.)
        let top = l(settings.jump_height) + l(pm.ps.float(ps::JUMP_ORIGIN_Z));
        let z = l(pm.ps.float(ps::ORIGIN + 8));
        keep = !matches!(
            top.partial_cmp(&z),
            Some(std::cmp::Ordering::Less | std::cmp::Ordering::Equal)
        );
    }
    if !keep {
        pm.ps.set_vec3(ps::ORIGIN, down_o);
        pm.ps.set_velocity(down_v);
        if had_ground {
            // Down 9 after the plain slide.
            let o = down_o;
            let down = [o[0], o[1], (l(o[2]) - l(9.0)).store()];
            let mask = pm.int(pm::TRACE_MASK);
            let trace = player_trace(pm, &o, &mins, &maxs, &down, pass, mask, host);
            if l(trace.fraction()) < l(1.0) {
                let f = l(trace.fraction());
                let mut set = [0f32; 3];
                for i in 0..3 {
                    set[i] = ((l(down[i]) - l(o[i])) * f + l(o[i])).store();
                }
                pm.ps.set_vec3(ps::ORIGIN, set);
                let v = pm.ps.velocity();
                pm.ps.set_velocity(clip_velocity(&v, &trace.normal()));
            }
        }
    }
    if jumping {
        jump_clamp_velocity(&mut pm.ps, &down_o, settings);
    }

    if !had_ground || pm.ps.pm_type() >= pm_type::DEAD {
        return;
    }
    if !verify_prone_position(pm, &start_o, &start_v, host) {
        return;
    }
    let delta = (l(pm.ps.float(ps::ORIGIN + 8)) - l(down_o[2])).store();
    // (`fcomp` 0.5, test ah,0x41, jne: only more than 0.5; NaN isn't.)
    if !above(l(delta).abs(), l(0.5)) {
        return;
    }
    let mut rise = round_float_to_int(delta);
    if rise == 0 {
        return;
    }
    rise = rise.clamp(-16, 24);
    let sequence = pm.ps.int(ps::EVENT_SEQUENCE);
    let slot = (sequence & (MAX_PS_EVENTS - 1)) as usize;
    pm.ps.set_int(ps::EVENTS + 4 * slot, EV_STEP_VIEW);
    pm.ps
        .set_int(ps::EVENT_PARMS + 4 * slot, (rise + 128) & 0xFF);
    pm.ps.set_int(ps::EVENT_SEQUENCE, sequence.wrapping_add(1));

    // Slowed by the step's height.
    let climbed = l(pm.ps.float(ps::ORIGIN + 8)) - l(start_o[2]);
    let scale = (l(1.0) - climbed.abs() / l(step_size)) * l(0.8) + l(f32::from_bits(0x3E4C_CCCC));
    let v = pm.ps.velocity();
    pm.ps.set_velocity(v.map(|c| (scale * l(c)).store()));

    let size = rise.abs();
    if size <= 3 || pm.ps.int(ps::GROUND_ENTITY_NUM) == ENTITY_NONE {
        return;
    }
    if !should_make_footsteps(pm, settings) {
        return;
    }
    let n = (size / 2).min(4);
    let old = pm.ps.int(ps::BOB_CYCLE);
    let bob = F24::int(n) * l(1.25) + l(7.0) + F24::int(old);
    let new = (crate::net::msg::ftol2(bob.0) as i32) & 0xFF;
    pm.ps.set_int(ps::BOB_CYCLE, new);
    footstep_event(pm, pml, old, new, true, host);
}
