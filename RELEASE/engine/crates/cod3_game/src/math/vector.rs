//! Vector lengths, distances and normalizing.
//!
//! Sums of squares are fused multiply-adds in the original (`fmadds`), in
//! the order kept here: they can differ from `x*x + y*y + z*z` in the last bit.

use crate::math::angles::Vec3;
use crate::ppc::{fmadds, fnmsubs};

/// `x² + y² + z²` the way the game sums it: y² first, then x² and z² fused in.
pub fn length_squared(v: &Vec3) -> f32 {
    fmadds(v[2], v[2], fmadds(v[0], v[0], v[1] * v[1]))
}

/// Scales `v` to length 1 and returns its old length (0 leaves it alone).
///
/// Original: `VectorNormalize` at 0x82526F38
pub fn normalize(v: &mut Vec3) -> f32 {
    let length = length_squared(v).sqrt();
    if length == 0.0 {
        return length;
    }
    let inverse = 1.0 / length;
    for c in v.iter_mut() {
        *c *= inverse;
    }
    length
}

/// `v` scaled to length 1 (or zero), and its length.
///
/// Original: `VectorNormalize2` at 0x82526E08
pub fn normalized(v: &Vec3) -> (Vec3, f32) {
    let length = length_squared(v).sqrt();
    if length == 0.0 {
        return ([0.0; 3], length);
    }
    let inverse = 1.0 / length;
    ([v[0] * inverse, v[1] * inverse, v[2] * inverse], length)
}

/// The magic number of the "fast inverse square root" estimate (Quake III's).
const INVERSE_SQRT_MAGIC: i32 = 0x5F37_59DF;

/// Scales `v` to about length 1 with Quake III's fast inverse square root:
/// the bit trick estimate and one Newton step, so the result is off by up
/// to about 0.2%. A zero vector becomes NaNs or infinities, as in the original.
///
/// Original: `VectorNormalizeFast` at 0x82526E78
pub fn normalize_fast(v: &mut Vec3) {
    let length_sq = length_squared(v);
    let half = length_sq * 0.5;
    let estimate =
        f32::from_bits(INVERSE_SQRT_MAGIC.wrapping_sub((length_sq.to_bits() as i32) >> 1) as u32);
    // One Newton-Raphson step: estimate * (1.5 - half * estimate²), with the PowerPC's fused subtract.
    let refined = fnmsubs(half * estimate, estimate, 1.5) * estimate;
    for c in v.iter_mut() {
        *c *= refined;
    }
}

/// Scales the x and y of `v` to a 2D length of 1 and returns the old 2D length.
///
/// Original: `Vec2Normalize` at 0x82526EF0
pub fn normalize_2d(v: &mut [f32; 2]) -> f32 {
    let length = fmadds(v[0], v[0], v[1] * v[1]).sqrt();
    if length == 0.0 {
        return length;
    }
    let inverse = 1.0 / length;
    v[0] *= inverse;
    v[1] *= inverse;
    length
}

/// The squared 2D distance between `from` and `to` (x and y only).
///
/// Original: `Vec2DistanceSq` at 0x82526F90
pub fn distance_squared_2d(from: &[f32; 2], to: &[f32; 2]) -> f32 {
    let dx = to[0] - from[0];
    let dy = to[1] - from[1];
    fmadds(dy, dy, dx * dx)
}

/// The 2D distance between `from` and `to` (x and y only).
///
/// Original: `Vec2Distance` at 0x82526FB8
pub fn distance_2d(from: &[f32; 2], to: &[f32; 2]) -> f32 {
    distance_squared_2d(from, to).sqrt()
}

/// The distance between two points: y² first, then z² and x² fused in.
///
/// Original: `Distance` at 0x82526FE0
pub fn distance(from: &Vec3, to: &Vec3) -> f32 {
    let dy = to[1] - from[1];
    let dz = to[2] - from[2];
    let dx = to[0] - from[0];
    fmadds(dx, dx, fmadds(dz, dz, dy * dy)).sqrt()
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn normalizes() {
        let mut v = [3.0, 0.0, 4.0];
        assert_eq!(normalize(&mut v), 5.0);
        assert_eq!(v, [0.6, 0.0, 0.8]);
        assert_eq!(normalized(&[0.0; 3]), ([0.0; 3], 0.0));
        let mut f = [3.0, 0.0, 4.0];
        normalize_fast(&mut f);
        assert!((f[0] - 0.6).abs() < 0.002);
        assert_eq!(distance(&[1.0, 2.0, 3.0], &[4.0, 6.0, 3.0]), 5.0);
    }
}
