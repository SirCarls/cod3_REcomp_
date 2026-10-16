//! The level scripts' math built-ins.

use crate::math::angles::Vec3;
use crate::math::{crt, vector};
use crate::ppc::fmadds;

/// `abs`. Original: `Broc_Abs` at 0x820A0098 (level slot 0x174)
pub fn abs(x: f32) -> f32 {
    x.abs()
}

/// `sin` in single precision, through the runtime's double `sin`.
///
/// Original: `Broc_Sin` at 0x820A00C8 (level slot 0x154)
pub fn sin(x: f32) -> f32 {
    crt::sin(f64::from(x)) as f32
}

/// `cos`. Original: `Broc_Cos` at 0x820A00A0 (level slot 0x158)
pub fn cos(x: f32) -> f32 {
    crt::cos(f64::from(x)) as f32
}

/// `sqrt`. Original: `Broc_Sqrt` at 0x824995C0 (level slot 0x178)
pub fn sqrt(x: f32) -> f32 {
    x.sqrt()
}

/// `lengthsquared`. Original: `Broc_LengthSquared` at 0x820B2170 (level slot 0x190)
pub fn length_squared(v: &Vec3) -> f32 {
    vector::length_squared(v)
}

/// `length`. Original: `Broc_Length` at 0x820B2150 (level slot 0x18C)
pub fn length(v: &Vec3) -> f32 {
    vector::length_squared(v).sqrt()
}

/// `vectordot`: y·y' first, then z·z' and x·x' fused in.
///
/// Original: `Broc_VectorDot` at 0x820B2190 (level slot 0x198)
pub fn dot(a: &Vec3, b: &Vec3) -> f32 {
    fmadds(a[0], b[0], fmadds(a[2], b[2], a[1] * b[1]))
}

/// `distancesquared`: the squared length of `to - from`, summed like [`vector::distance`].
///
/// Original: `Broc_DistanceSquared` at 0x820B21F0 (level slot 0x188)
pub fn distance_squared(from: &Vec3, to: &Vec3) -> f32 {
    let dy = to[1] - from[1];
    let dz = to[2] - from[2];
    let dx = to[0] - from[0];
    fmadds(dx, dx, fmadds(dz, dz, dy * dy))
}

/// `distance`. Original: `Broc_Distance` at 0x820B21B8 (level slot 0x184)
pub fn distance(from: &Vec3, to: &Vec3) -> f32 {
    distance_squared(from, to).sqrt()
}

/// `closer(reference, a, b)`: whether `a` is nearer to `reference` than `b` is.
///
/// Original: `Broc_Closer` at 0x820B2228 (level slot 0x194)
pub fn closer(reference: &Vec3, a: &Vec3, b: &Vec3) -> bool {
    let squared = |p: &Vec3| {
        let dy = reference[1] - p[1];
        let dz = reference[2] - p[2];
        let dx = reference[0] - p[0];
        fmadds(dx, dx, fmadds(dz, dz, dy * dy))
    };
    squared(a) < squared(b)
}

/// `vectornormalize`: `v` scaled to length 1 (a zero vector stays zero).
///
/// Original: `Broc_VectorNormalize` at 0x820B2290 (level slot 0x19C)
pub fn normalize(v: &Vec3) -> Vec3 {
    let mut out = *v;
    let length = vector::length_squared(&out).sqrt();
    if length != 0.0 {
        let inverse = 1.0 / length;
        for c in out.iter_mut() {
            *c *= inverse;
        }
    }
    out
}
