//! Vector math the games share (Quake III's `q_math.c` family), at the
//! game's x87 precision.
//!
//! Call of Duty 2 runs at 24-bit precision ([`crate::x87::F24`]); these are
//! its versions. Each keeps the original's order of operations and stores.

use crate::x87::F24;

/// Scales `v` to length 1 and returns its length before; a vector of length
/// 0 stays as it is. The length is `sqrt((x*x + y*y) + z*z)`: the sum stored
/// as a `float`, its root stored as one; then `1 / length` (stored as a
/// `float`) multiplies each component. A NaN length isn't 0: every component
/// becomes NaN. (The sum can overflow to infinity only when it is stored.)
///
/// Original: `Vec3Normalize` at 0x00426CF0 (CoD2MP_s.exe 1.3; v in esi, the length on the x87 stack)
pub fn vec3_normalize(v: &mut [f32; 3]) -> f32 {
    let [x, y, z] = v.map(F24::load);
    let squared = (x * x + y * y + z * z).store();
    normalize_by(v, squared)
}

/// The same for a 2D vector (`x`, `y`): `sqrt(x*x + y*y)`.
///
/// Original: `Vec2Normalize` at 0x00426D80 (CoD2MP_s.exe 1.3; our name; v in esi, the length on the x87 stack)
pub fn vec2_normalize(v: &mut [f32; 2]) -> f32 {
    let [x, y] = v.map(F24::load);
    let squared = (x * x + y * y).store();
    normalize_by(v, squared)
}

/// The rest of both: the root (the C runtime's `sqrt`, stored as a
/// `float`), and the components scaled by its stored inverse.
fn normalize_by(v: &mut [f32], squared: f32) -> f32 {
    let length = F24::load(squared).sqrt().store();
    // (`fucompp` with 0: equal skips; unordered divides.)
    if length != 0.0 {
        let inverse = (F24::load(1.0) / F24::load(length)).store();
        for c in v.iter_mut() {
            *c = (F24::load(inverse) * F24::load(*c)).store();
        }
    }
    length
}

/// [`vec3_normalize`] into `out`, `v` kept; a vector of length 0 gives
/// zeros.
///
/// Original: `Vec3NormalizeTo` at 0x00426E90 (CoD2MP_s.exe 1.3; v in edi, out in esi, the length on the x87 stack)
pub fn vec3_normalize_to(v: &[f32; 3], out: &mut [f32; 3]) -> f32 {
    let [x, y, z] = v.map(F24::load);
    let squared = (x * x + y * y + z * z).store();
    let length = F24::load(squared).sqrt().store();
    if length != 0.0 {
        let inverse = (F24::load(1.0) / F24::load(length)).store();
        for (o, c) in out.iter_mut().zip(v) {
            *o = (F24::load(inverse) * F24::load(*c)).store();
        }
    } else {
        *out = [0.0; 3];
    }
    length
}

/// The C runtime's `atan2` as the games call it on two `float`s: computed
/// in double precision, stored as a `float`. A NaN argument gives the x87's
/// choice of NaN (`fpatan`); both infinite gives its default NaN (the
/// runtime's own check, where the x87 would give a multiple of 45 degrees).
fn atan2_stored(y: f32, x: f32) -> f32 {
    let (y, x) = (f64::from(y), f64::from(x));
    if let Some(nan) = crate::x87::propagate(y, x) {
        return nan as f32;
    }
    if x.is_infinite() && y.is_infinite() {
        return f32::from_bits(0xFFC0_0000);
    }
    y.atan2(x) as f32
}

/// Radians (a stored `float`) to degrees as the games compute it: `a * k /
/// 3.1415927`, stored, plus 360 (stored) when below 0.
fn degrees(radians: f32, k: f32) -> f32 {
    let d = (F24::load(radians) * F24::load(k) / F24::load(std::f32::consts::PI)).store();
    // (`fcomp` 0, test ah,5, jp: only below adds; NaN doesn't.)
    if d < 0.0 {
        (F24::load(d) + F24::load(360.0)).store()
    } else {
        d
    }
}

/// The yaw of a direction, in degrees from 0 to 360 (`atan2(y, x)`); 0 for
/// a vector with x and y both 0.
///
/// Original: `vectoyaw` at 0x004274B0 (CoD2MP_s.exe 1.3; Quake III's name; v in ecx, the yaw on the x87 stack)
pub fn vectoyaw(v: &[f32; 3]) -> f32 {
    // (`fucompp` with 0: only equal counts; NaN isn't 0.)
    if v[1] == 0.0 && v[0] == 0.0 {
        return 0.0;
    }
    degrees(atan2_stored(v[1], v[0]), 180.0)
}

/// A direction's angles: pitch, yaw, roll 0, in degrees from 0 to 360.
/// Straight up or down (x and y 0): yaw 0, pitch 270 up (z above 0), 90
/// otherwise. The pitch is `-atan2(z, sqrt(x*x + y*y))`.
///
/// Original: `VecToAngles` at 0x00427750 (CoD2MP_s.exe 1.3; v in esi, angles in edi)
pub fn vec_to_angles(v: &[f32; 3], angles: &mut [f32; 3]) {
    if v[1] == 0.0 && v[0] == 0.0 {
        // (`fcomp` 0, test ah,0x41, jne: 0, below or NaN is 90.)
        let pitch = if v[2] > 0.0 { 270.0 } else { 90.0 };
        *angles = [pitch, 0.0, 0.0];
        return;
    }
    let yaw = degrees(atan2_stored(v[1], v[0]), 180.0);
    let (x, y) = (F24::load(v[0]), F24::load(v[1]));
    let squared = (y * y + x * x).store();
    let forward = F24::load(squared).sqrt().store();
    let pitch = degrees(atan2_stored(v[2], forward), -180.0);
    *angles = [pitch, yaw, 0.0];
}

/// An angle in degrees brought to (-180, 180]: as 16 bits of a turn
/// (`ftol2(a * 182.04445) & 0xFFFF`, times 0.0054931640625, each stored as
/// a `float`), less 360 when above 180.
///
/// Original: `AngleNormalize180` at 0x00429C60 (CoD2MP_s.exe 1.3; the angle on the stack, the result on the x87 stack)
pub fn angle_normalize180(angle: f32) -> f32 {
    let turn = crate::net::msg::ftol2((F24::load(angle) * F24::load(182.044_45)).0) as i32 & 0xFFFF;
    let a = F24::int(turn).store();
    let a = (F24::load(a) * F24::load(360.0 / 65536.0)).store();
    // (`fcomp` 180, test ah,0x41, jne: only above subtracts.)
    if a > 180.0 {
        (F24::load(a) - F24::load(360.0)).store()
    } else {
        a
    }
}

/// `AngleVectors`' sine and cosine of an angle in degrees: `a * 0.017453292`
/// stored as a `float`, then `fsincos` (which the checker's emulator, and
/// these ports, compute in double precision), each stored as a `float`.
fn sin_cos_degrees(a: f32) -> (f32, f32) {
    let radians = f64::from((F24::load(a) * F24::load(0.017_453_292)).store());
    (radians.sin() as f32, radians.cos() as f32)
}

/// The forward, right and up directions of `angles` (pitch, yaw, roll),
/// each only when asked for (right and up need the roll). Quake III's
/// formulas, each product stored as the original stores it.
///
/// Original: `AngleVectors` at 0x004279C0 (CoD2MP_s.exe 1.3; Quake III's name; angles in edx, forward in esi, right in edi, up in ebx)
pub fn angle_vectors(
    angles: &[f32; 3],
    forward: Option<&mut [f32; 3]>,
    right: Option<&mut [f32; 3]>,
    up: Option<&mut [f32; 3]>,
) {
    let (sy, cy) = sin_cos_degrees(angles[1]);
    let (sp, cp) = sin_cos_degrees(angles[0]);
    let l = F24::load;
    if let Some(f) = forward {
        *f = [
            (l(cp) * l(cy)).store(),
            (l(cp) * l(sy)).store(),
            (-l(sp)).store(),
        ];
    }
    if right.is_none() && up.is_none() {
        return;
    }
    let (sr, cr) = sin_cos_degrees(angles[2]);
    if let Some(r) = right {
        let t = (l(sr) * l(sp)).store();
        *r = [
            (l(cr) * l(sy) - l(t) * l(cy)).store(),
            (l(cr) * l(cy) * l(-1.0) - l(t) * l(sy)).store(),
            (l(sr) * l(cp) * l(-1.0)).store(),
        ];
    }
    if let Some(u) = up {
        let t = (l(cr) * l(sp)).store();
        *u = [
            (l(t) * l(cy) + l(sr) * l(sy)).store(),
            (l(t) * l(sy) - l(sr) * l(cy)).store(),
            (l(cr) * l(cp)).store(),
        ];
    }
}

/// The point `fraction` of the way from `from` to `to`: `(to - from) *
/// fraction + from` per component, stored.
///
/// Original: `Vec3Lerp` at 0x004936E0 (CoD2MP_s.exe 1.3; Call of Duty 4's name; from in eax, to in edx, out in ecx, fraction on the stack)
pub fn vec3_lerp(from: &[f32; 3], to: &[f32; 3], fraction: f32) -> [f32; 3] {
    let mut out = [0f32; 3];
    for i in 0..3 {
        out[i] = ((F24::load(to[i]) - F24::load(from[i])) * F24::load(fraction)
            + F24::load(from[i]))
        .store();
    }
    out
}

/// The pitch of a direction, in degrees from 0 to 360 ([`vec_to_angles`]'s
/// pitch: straight up 270, down or level 90 when x and y are 0).
///
/// Original: `vectopitch` at 0x004275B0 (CoD2MP_s.exe 1.3; Call of Duty 4's name; v in esi, the pitch on the x87 stack)
pub fn vectopitch(v: &[f32; 3]) -> f32 {
    let mut angles = [0f32; 3];
    vec_to_angles(v, &mut angles);
    angles[0]
}

/// `a1 - a2` brought to [-180, 180] by whole turns (each step stored).
/// The original never returns once a turn no longer changes the value (an
/// infinite or huge angle); the port then returns it as it is.
///
/// Original: `AngleSubtract` at 0x00429B70 (CoD2MP_s.exe 1.3; Quake III's name; (a1, a2) on the stack, the result on the x87 stack)
pub fn angle_subtract(a1: f32, a2: f32) -> f32 {
    let mut a = (F24::load(a1) - F24::load(a2)).store();
    // (`fcomp` 180, test ah,0x41, jne: only above turns back; NaN doesn't.)
    while a > 180.0 {
        let next = (F24::load(a) - F24::load(360.0)).store();
        if next == a {
            return a;
        }
        a = next;
    }
    // (`fcomp` -180, test ah,5, jp: only below turns on.)
    while a < -180.0 {
        let next = (F24::load(a) + F24::load(360.0)).store();
        if next == a {
            return a;
        }
        a = next;
    }
    a
}

/// A `float` rounded to the nearest integer (ties to even): `x + 2^-30`
/// at the x87's precision, then `fistp` (NaN and out of range give
/// `i32::MIN`).
///
/// Original: `RoundFloatToInt` at 0x00403710 (CoD2MP_s.exe 1.3; our name; x on the stack, the result in eax)
pub fn round_float_to_int(x: f32) -> i32 {
    (F24::load(x) + F24::double(9.313_225_746_154_785e-10)).round_to_int()
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn normalizes_and_keeps_zero() {
        let mut v = [3.0, 4.0, 0.0];
        assert_eq!(vec3_normalize(&mut v), 5.0);
        assert_eq!(v, [0.6, 0.8, 0.0]);
        let mut z = [0.0f32; 2];
        assert_eq!(vec2_normalize(&mut z), 0.0);
        assert_eq!(z, [0.0, 0.0]);
    }
}
