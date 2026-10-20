//! Whether a player fits lying down where he is (`BG_CheckProne`): a box
//! for the body, traces for the feet behind him, the torso in front, the
//! ground under both, and the angles the body would bend at. Call of Duty
//! 2's version (see [`super::pmove`] for the conventions).

use super::pmove::{pm, pm_flags, ps, Pmove};
use super::trace::MoveHost;
use crate::shared::math::{
    angle_normalize180, angle_subtract, angle_vectors, vec3_lerp, vec3_normalize,
    vec3_normalize_to, vectopitch,
};
use crate::x87::F24;

/// What [`check_prone`] tests.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct ProneQuery {
    /// The entity the traces pass through (the player).
    pub pass_entity: i32,
    /// Where the player stands.
    pub origin: [f32; 3],
    /// Half the width of the player's box.
    pub size: f32,
    /// Its height.
    pub height: f32,
    /// The direction the body would point.
    pub yaw: f32,
    /// Already lying down (no room check above the box).
    pub already_prone: bool,
    /// Standing on something (a failed check then fails; in the air it
    /// passes with flat angles).
    pub on_ground: bool,
    /// The ground's normal, if known: steeper than 0.7 fails.
    pub ground_normal: Option<[f32; 3]>,
    /// The game's trace function to use (see [`MoveHost::trace`]).
    pub handler: u8,
    /// Not 0: the traces collide with `0x820011` instead of `0x810011`
    /// (the original's `proneCheckType`).
    pub check_type: i32,
    /// How far behind the player the feet reach.
    pub feet_distance: f32,
}

/// What [`check_prone`] writes where the caller asks for it (`None`: not
/// asked); a failed check leaves them as they are.
#[derive(Debug, Default)]
pub struct ProneAngles<'a> {
    /// Always 0 in Call of Duty 2.
    pub torso_height: Option<&'a mut f32>,
    /// The torso's pitch, from the body's middle up to the shoulders.
    pub torso_pitch: Option<&'a mut f32>,
    /// The waist's pitch, from the feet up to the middle.
    pub waist_pitch: Option<&'a mut f32>,
}

/// `a` (an `fcomp`'s first operand) above `b`: test ah,0x41 + je.
fn above(a: F24, b: F24) -> bool {
    matches!(a.partial_cmp(&b), Some(std::cmp::Ordering::Greater))
}

/// `a` below `b`: test ah,5 + jnp (or test ah,1 + jne without NaN).
fn below(a: F24, b: F24) -> bool {
    matches!(a.partial_cmp(&b), Some(std::cmp::Ordering::Less))
}

/// Whether the player fits lying down (see the module): true also for a
/// player in the air whose check failed (with the angles then 0).
///
/// In order: unless already prone, the box (`size` wide, `height` high)
/// must move 10 up without being stuck; on the ground the ground must not
/// be steeper than 0.7; a small box (6 each way) traced from `height - 6`
/// up toward the feet (`feet_distance - 6` behind, along `yaw - 180`); on
/// the ground, feet that hit something closer than `size + 2` fail, and
/// ones closer than `(height - 6) * 0.7 + 48` try again 22 higher (failing
/// when still closer). Then the torso: the box 48 in front, from `height -
/// 6` up, traced down `size * 2.5 + height - 12`: nothing under it fails, a
/// ground steeper than 0.7 fails; when the feet hit something, the torso
/// must not be too far below them, and the feet's point is traced again
/// along the body (and 18 higher). The ground under the feet is traced (a
/// miss fails, a steep one fails); the body may not bend by less than -50
/// or more than 70 degrees, and the rays from the origin to the torso and
/// from the torso to the feet (5 up) must be clear.
///
/// Original: `BG_CheckProne` at 0x00514700 (CoD2MP_s.exe 1.3; Call of Duty 4's name; 14 arguments on the stack: passEntityNum, vPos, fSize, fHeight, fYaw, pfTorsoHeight, pfTorsoPitch, pfWaistPitch, bAlreadyProne, bOnGround, vGroundNormal, handler, proneCheckType, prone_feet_dist)
pub fn check_prone(q: &ProneQuery, out: ProneAngles<'_>, host: &mut dyn MoveHost) -> bool {
    let l = F24::load;
    // (`0x810011 + 0x10000`: an addition, so the bit moves up.)
    let mask = if q.check_type != 0 {
        0x0082_0011
    } else {
        0x0081_0011
    };
    let mut trace = |start: &[f32; 3], mins: &[f32; 3], maxs: &[f32; 3], end: &[f32; 3]| {
        host.trace(q.handler, start, mins, maxs, end, q.pass_entity, mask)
    };
    let v = q.origin;

    if !q.already_prone {
        let mins = [(-l(q.size)).store(), (-l(q.size)).store(), 0.0];
        let maxs = [q.size, q.size, q.height];
        let end = [v[0], v[1], (l(v[2]) + l(10.0)).store()];
        if trace(&v, &mins, &maxs, &end).all_solid() {
            return false;
        }
    }
    if q.on_ground {
        if let Some(n) = q.ground_normal {
            if below(l(n[2]), l(0.7)) {
                return false;
            }
        }
    }

    let angles = [0.0, (l(q.yaw) - l(180.0)).store(), 0.0];
    let mut forward = [0f32; 3];
    let (mut right, mut up) = ([0f32; 3], [0f32; 3]);
    angle_vectors(&angles, Some(&mut forward), Some(&mut right), Some(&mut up));
    let small_mins = [-6.0f32; 3];
    let small_maxs = [6.0f32; 3];
    let mid = (l(q.height) - l(6.0)).store();
    let reach = (l(q.feet_distance) - l(6.0)).store();

    // The feet: behind, from the middle's height.
    let start = [v[0], v[1], (l(mid) + l(v[2])).store()];
    let mut end = [0f32; 3];
    for i in 0..3 {
        end[i] = (l(forward[i]) * l(reach) + l(start[i])).store();
    }
    let mut tr = trace(&start, &small_mins, &small_maxs, &end);
    let mut feet_hit = false;
    let feet_dist: f32;
    if below(l(tr.fraction()), l(1.0)) {
        if !q.on_ground {
            return false;
        }
        feet_hit = true;
        let d = (l(reach) * l(tr.fraction()) + l(6.0)).store();
        if above(l(q.size) + l(2.0), l(d)) {
            return false;
        }
        let limit = (l(mid) * l(0.7) + l(48.0)).store();
        if below(l(d), l(limit)) {
            // Too close: try 22 higher.
            end[2] = (l(end[2]) + l(22.0)).store();
            feet_hit = false;
            let dir = [
                (l(end[0]) - l(start[0])).store(),
                (l(end[1]) - l(start[1])).store(),
                (l(end[2]) - l(start[2])).store(),
            ];
            let length = vec3_normalize_to(&dir, &mut forward);
            tr = trace(&start, &small_mins, &small_maxs, &end);
            if below(l(tr.fraction()), l(1.0)) {
                feet_hit = true;
                let d = l(length) * l(tr.fraction()) + l(6.0);
                if below(d, l(limit)) {
                    return false;
                }
                feet_dist = d.store();
            } else {
                feet_dist = q.feet_distance;
            }
        } else {
            feet_dist = d;
        }
    } else {
        feet_dist = q.feet_distance;
    }
    // Where the feet got to.
    let mut feet = vec3_lerp(&start, &end, tr.fraction());

    // The torso: in front, traced down.
    let mut start = [0f32; 3];
    for i in 0..3 {
        start[i] = (l(forward[i]) * l(48.0) + l(v[i])).store();
    }
    start[2] = (l(forward[2]) * l(48.0) + l(v[2]) + l(mid)).store();
    let drop = (l(q.size) * l(2.5) + l(mid) - l(6.0)).store();
    let end = [start[0], start[1], (l(start[2]) - l(drop)).store()];
    let tr = trace(&start, &small_mins, &small_maxs, &end);
    let fail = |out: ProneAngles<'_>| {
        if q.on_ground {
            return false;
        }
        for o in [out.torso_height, out.torso_pitch, out.waist_pitch]
            .into_iter()
            .flatten()
        {
            *o = 0.0;
        }
        true
    };
    if tr.fraction() == 1.0 {
        return fail(out);
    }
    if below(l(tr.normal()[2]), l(0.7)) {
        return false;
    }
    let torso_dist = l(drop) * l(tr.fraction()) + l(6.0);
    let mut torso = [0f32; 3];
    for i in 0..2 {
        torso[i] = ((l(end[i]) - l(start[i])) * l(tr.fraction()) + l(start[i])).store();
    }
    torso[2] = ((l(end[2]) - l(start[2])) * l(tr.fraction()) + l(start[2]) - l(6.0)).store();

    if feet_hit {
        // (`fcompp`, test ah,0x41, je: the torso too far below the feet.)
        if above(torso_dist * l(-0.75), l(feet_dist) - torso_dist) {
            return fail(out);
        }
        // The feet again, along the body from the torso's start.
        let a0 = l(feet[0]) - l(torso[0]);
        let a1 = l(feet[1]) - l(torso[1]);
        let dz = (l(feet[2]) - l(torso[2])).store();
        let mut w = [
            (l(forward[0]) * l(6.0) + a0).store(),
            (l(forward[1]) * l(6.0) + a1).store(),
            (l(forward[2]) * l(6.0) + l(dz) + l(6.0)).store(),
        ];
        vec3_normalize(&mut w);
        let k = l(reach) - l(48.0);
        let t1 = (l(w[1]) * k + l(start[1])).store();
        let t0 = l(w[0]) * k + l(start[0]);
        let mut end = [0f32; 3];
        end[2] = (k * l(w[2]) + l(start[2])).store();
        end[0] = ((l(reach) * l(forward[0]) + l(v[0]) + t0) * l(0.5)).store();
        end[1] = ((l(reach) * l(forward[1]) + l(v[1]) + l(t1)) * l(0.5)).store();
        let mut start = start;
        let mut tr = trace(&start, &small_mins, &small_maxs, &end);
        if below(l(tr.fraction()), l(1.0)) {
            start = vec3_lerp(&start, &end, tr.fraction());
            start[2] = (l(start[2]) + l(18.0)).store();
            end[2] = (l(end[2]) + l(18.0)).store();
            tr = trace(&start, &small_mins, &small_maxs, &end);
            if below(l(tr.fraction()), l(1.0)) {
                return fail(out);
            }
        }
        feet = vec3_lerp(&start, &end, tr.fraction());
    }

    // The ground under the feet.
    let start = feet;
    let reach_down = (l(feet[2]) - ((l(feet[2]) - l(torso[2])) * l(2.0) + l(q.size))).store();
    let end = [feet[0], feet[1], reach_down];
    let tr = trace(&start, &small_mins, &small_maxs, &end);
    if tr.fraction() == 1.0 {
        return fail(out);
    }
    if below(l(tr.normal()[2]), l(0.7)) {
        return false;
    }
    let mut feet = vec3_lerp(&start, &end, tr.fraction());
    feet[2] = (l(feet[2]) - l(6.0)).store();

    let sub = |a: &[f32; 3], b: &[f32; 3]| {
        [
            (l(a[0]) - l(b[0])).store(),
            (l(a[1]) - l(b[1])).store(),
            (l(a[2]) - l(b[2])).store(),
        ]
    };
    let mut ok = true;
    let up_pitch = vectopitch(&sub(&torso, &v));
    let bend = angle_subtract(vectopitch(&sub(&feet, &torso)), up_pitch);
    // (-50 `fcom`, test ah,5, jnp: below fails; 70 `fcomp`, test ah,0x41,
    // jne: only above fails; NaN passes.)
    if below(l(bend), l(-50.0)) || above(l(bend), l(70.0)) {
        ok = false;
    }
    let rays = [(v, torso), (torso, feet)];
    for (from, to) in rays {
        let s = [from[0], from[1], (l(from[2]) + l(5.0)).store()];
        let e = [to[0], to[1], (l(to[2]) + l(5.0)).store()];
        if below(l(trace(&s, &[0.0; 3], &[0.0; 3], &e).fraction()), l(1.0)) {
            ok = false;
        }
    }

    let ProneAngles {
        torso_height,
        torso_pitch,
        waist_pitch,
    } = out;
    if let Some(h) = torso_height {
        *h = 0.0;
    }
    let mut torso_pitch = torso_pitch;
    let mut waist_pitch = waist_pitch;
    if let Some(p) = torso_pitch.as_deref_mut() {
        *p = angle_normalize180(vectopitch(&sub(&v, &torso)));
    }
    if let Some(p) = waist_pitch.as_deref_mut() {
        *p = angle_normalize180(vectopitch(&sub(&torso, &feet)));
    }
    if ok {
        return true;
    }
    if q.on_ground {
        return false;
    }
    for o in [torso_pitch, waist_pitch].into_iter().flatten() {
        *o = 0.0;
    }
    // (The torso height was set to 0 above.)
    true
}

/// A prone player's move checked: he must still fit lying down where the
/// move took him ([`check_prone`] with the body's direction, already prone
/// and on the ground, a box of [`ps::PRONE_CHECK_SIZE`] by 30, the feet 66
/// behind; the angles go to the player state). If not, he goes back to
/// where the move started (`start_origin`, `start_velocity`). Not prone:
/// true.
///
/// Original: `PM_VerifyPronePosition` at 0x00530130 (CoD2MP_s.exe 1.3; Call of Duty 4's name; pm in eax, the start velocity in ebx, the start origin on the stack)
pub fn verify_prone_position(
    pm: &mut Pmove,
    start_origin: &[f32; 3],
    start_velocity: &[f32; 3],
    host: &mut dyn MoveHost,
) -> bool {
    if pm.ps.pm_flags() & pm_flags::PRONE == 0 {
        return true;
    }
    let q = ProneQuery {
        pass_entity: pm.ps.int(ps::CLIENT_NUM),
        origin: pm.ps.vec3(ps::ORIGIN),
        size: pm.ps.float(ps::PRONE_CHECK_SIZE),
        height: 30.0,
        yaw: pm.ps.float(ps::PRONE_DIRECTION),
        already_prone: true,
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
    if !fits {
        pm.ps.set_vec3(ps::ORIGIN, *start_origin);
        pm.ps.set_velocity(*start_velocity);
    }
    fits
}
