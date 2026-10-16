//! The multiplayer program's fast yaw: the direction a vector points in the
//! horizontal plane, without the C runtime's `atan2`.
//!
//! Quake III's `vectoyaw` calls `atan2`. CoD3's multiplayer program (only
//! that one; the campaign has no such code) works it out in single
//! precision instead: it scales (x, y) to length 1, takes the arcsine of
//! the smaller of |x| and |y| from a short series, and puts the angle in
//! the right quadrant. The series is Taylor's below 0.5 and
//! `asin(s) = π/2 - 2·asin(√((1 - s)/2))` above it, so its error stays near
//! 1e-5 radians. The result goes into position messages (as 8 bits of
//! movement direction), so a port must give the same bits, fused
//! multiply-adds included.

use crate::math::angles::Vec3;
use crate::ppc::{fmadds, fmsubs, fnmsubs};

/// π/2 rounded to `f32` (0x3FC90FDB).
const HALF_PI: f32 = std::f32::consts::FRAC_PI_2;
/// π/2 three steps below its `f32` rounding (0x3FC90FD8), as the original's
/// second series ends; the difference is part of the result's last bits.
#[allow(clippy::approx_constant)] // deliberately not π/2's own rounding
const HALF_PI_LOW: f32 = 1.570_796;
/// π rounded to `f32` (0x40490FDB).
const PI: f32 = std::f32::consts::PI;
/// 180/π as the game stores it (0x42652EE0).
const RADIANS_TO_DEGREES: f32 = 57.295_776;

/// The series' coefficients below 0.5: s + s³·(1/6) + s⁵·0.075 + s⁷·0.054 (0x3E2AAAAD, 0x3D99999A, 0x3D5D1B6F).
const SMALL_S3: f32 = 0.166_666_7;
const SMALL_S5: f32 = 0.075;
const SMALL_S7: f32 = 0.053_981_24;
/// Above 0.5, in r = √((1 - s)/2): π/2 - 2r - r³/3 - 0.15·r⁵ - 0.108·r⁷ (0x3EAAAAAA, 0x3E19999A, 0xBDDD1B71).
const LARGE_R3: f32 = 0.333_333_3;
const LARGE_R5: f32 = 0.15;
const LARGE_R7: f32 = -0.107_962_5;

/// asin(s) for 0 <= s <= 1, as the original sums it.
fn arcsine(s: f32) -> f32 {
    if s < 0.5 {
        let s2 = s * s;
        let s3 = s2 * s;
        let s5 = s3 * s2;
        let s7 = s5 * s2;
        fmadds(s3, SMALL_S3, fmadds(s7, SMALL_S7, s5 * SMALL_S5)) + s
    } else {
        let r = ((1.0 - s) * 0.5).abs().sqrt();
        let r2 = r * r;
        let r3 = r2 * r;
        let r5 = r3 * r2;
        let r7 = r5 * r2;
        let series = fnmsubs(
            r,
            2.0,
            fnmsubs(r3, LARGE_R3, fmsubs(r7, LARGE_R7, r5 * LARGE_R5)),
        );
        series + HALF_PI_LOW
    }
}

/// The angle of (x, y) in radians, in [-π, π] (`atan2(y, x)`, approximately).
fn angle_of(x: f32, y: f32) -> f32 {
    let ax = x.abs();
    let ay = y.abs();
    if ay + ax == 0.0 {
        return 0.0;
    }
    let inverse_length = 1.0 / fmadds(y, y, x * x).sqrt();
    let mut angle = if ax < ay {
        HALF_PI - arcsine(inverse_length * ax)
    } else {
        arcsine(inverse_length * ay)
    };
    if x < 0.0 {
        angle = PI - angle;
    }
    if y < 0.0 {
        angle = -angle;
    }
    angle
}

/// The yaw of `v` in degrees, in [0, 360) (0 for a vertical or zero vector).
///
/// Original: `vectoyaw` at 0x82550680
pub fn vector_to_yaw(v: &Vec3) -> f32 {
    if v[1] == 0.0 && v[0] == 0.0 {
        return 0.0;
    }
    let yaw = angle_of(v[0], v[1]) * RADIANS_TO_DEGREES;
    if yaw < 0.0 {
        yaw + 360.0
    } else {
        yaw
    }
}

/// The yaw of `v` in degrees, in [-180, 180] (0 for a vertical or zero vector).
///
/// Original: `vectoyaw_signed` at 0x82550478
pub fn vector_to_yaw_signed(v: &Vec3) -> f32 {
    if v[1] == 0.0 && v[0] == 0.0 {
        return 0.0;
    }
    angle_of(v[0], v[1]) * RADIANS_TO_DEGREES
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn constants_have_the_games_bits() {
        assert_eq!(HALF_PI.to_bits(), 0x3FC9_0FDB);
        assert_eq!(HALF_PI_LOW.to_bits(), 0x3FC9_0FD8);
        assert_eq!(PI.to_bits(), 0x4049_0FDB);
        assert_eq!(RADIANS_TO_DEGREES.to_bits(), 0x4265_2EE0);
        assert_eq!(SMALL_S3.to_bits(), 0x3E2A_AAAD);
        assert_eq!(SMALL_S5.to_bits(), 0x3D99_999A);
        assert_eq!(SMALL_S7.to_bits(), 0x3D5D_1B6F);
        assert_eq!(LARGE_R3.to_bits(), 0x3EAA_AAAA);
        assert_eq!(LARGE_R5.to_bits(), 0x3E19_999A);
        assert_eq!(LARGE_R7.to_bits(), 0xBDDD_1B71);
    }

    #[test]
    fn close_to_atan2() {
        for i in 0..3600 {
            let degrees = i as f32 * 0.1;
            let radians = degrees.to_radians();
            let v = [radians.cos() * 37.0, radians.sin() * 37.0, 5.0];
            let yaw = vector_to_yaw(&v);
            let error = (yaw - degrees).abs().min(360.0 - (yaw - degrees).abs());
            assert!(error < 0.01, "{degrees}: {yaw}");
            let signed = vector_to_yaw_signed(&v);
            assert!((-180.0..=180.0).contains(&signed));
        }
        assert_eq!(vector_to_yaw(&[0.0, 0.0, 9.0]), 0.0);
        assert_eq!(vector_to_yaw(&[0.0, -1.0, 0.0]), 270.0);
        assert_eq!(vector_to_yaw_signed(&[0.0, -1.0, 0.0]), -90.0);
    }
}
