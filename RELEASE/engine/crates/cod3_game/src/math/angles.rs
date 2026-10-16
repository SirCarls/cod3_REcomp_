//! Angles in degrees, and the 16-bit angle steps the game rounds them to.
//!
//! Like Quake III, CoD3 stores and sends angles as 16-bit "shorts"
//! (65536 steps for 360°), so normalizing an angle also rounds it toward
//! zero to a multiple of 360/65536°: `angle_normalize_180(190.0)` is
//! -170.00244, not -170.

use crate::ppc;

pub type Vec3 = [f32; 3];

/// 65536 / 360 as the game stores it (0x43360B61; `ANGLE2SHORT`'s factor).
pub const ANGLE_TO_SHORT: f32 = 182.04445;
/// 360 / 65536, exact (0x3BB40000; `SHORT2ANGLE`'s factor).
pub const SHORT_TO_ANGLE: f32 = 360.0 / 65536.0;

/// An angle as one of the 65536 steps: `(int)(angle * 65536 / 360) & 65535`.
pub fn angle_to_short(angle: f32) -> u16 {
    (ppc::fctiwz(f64::from(angle * ANGLE_TO_SHORT)) & 0xFFFF) as u16
}

/// Brings an angle into (-180, 180], rounded toward zero to a 16-bit step.
///
/// Original: `AngleNormalize180` at 0x82526BF8
pub fn angle_normalize_180(angle: f32) -> f32 {
    let normalized = f32::from(angle_to_short(angle)) * SHORT_TO_ANGLE;
    if normalized > 180.0 {
        normalized - 360.0
    } else {
        normalized
    }
}

/// `angle1 - angle2` brought into [-180, 180] (not rounded to steps).
///
/// The difference is taken in single precision, then reduced with a
/// double-precision `fmod` by 360.
///
/// Original: `AngleSubtract` at 0x82526C58
pub fn angle_subtract(angle1: f32, angle2: f32) -> f32 {
    let mut a = (f64::from(angle1 - angle2) % 360.0) as f32;
    if a > 180.0 {
        a -= 360.0;
    }
    if a < -180.0 {
        a += 360.0;
    }
    a
}

/// `angle_normalize_180(angle1 - angle2)`.
///
/// Original: `AngleDelta` at 0x8252A008
pub fn angle_delta(angle1: f32, angle2: f32) -> f32 {
    angle_normalize_180(angle1 - angle2)
}

/// [`angle_subtract`] on each of the three angles.
///
/// Original: `AnglesSubtract` at 0x8252A068
pub fn angles_subtract(v1: &Vec3, v2: &Vec3) -> Vec3 {
    [
        angle_subtract(v1[0], v2[0]),
        angle_subtract(v1[1], v2[1]),
        angle_subtract(v1[2], v2[2]),
    ]
}

/// `angle` brought into [-180, 180] with a double-precision `fmod` when it
/// is above 180 (an angle inside is left as it is; not rounded to steps).
/// Below -180 the `fmod` keeps the sign, so the result stays below -180
/// (-190 stays -190, -600 becomes -240), as in the original.
///
/// Original: `AngleWrap180` at 0x820DA148 (multiplayer program)
pub fn angle_wrap_180(angle: f32) -> f32 {
    // Not `!(-180.0..=180.0).contains(..)`: a NaN must stay out of the `fmod`, as on the PowerPC.
    #[allow(clippy::manual_range_contains)]
    if angle < -180.0 || angle > 180.0 {
        (f64::from(angle + 180.0) % 360.0) as f32 - 180.0
    } else {
        angle
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn constants_have_the_games_bits() {
        assert_eq!(ANGLE_TO_SHORT.to_bits(), 0x4336_0B61);
        assert_eq!(SHORT_TO_ANGLE.to_bits(), 0x3BB4_0000);
    }

    #[test]
    fn normalizes_in_16_bit_steps() {
        assert_eq!(angle_normalize_180(190.0), -170.002_44);
        assert_eq!(angle_normalize_180(45.5), 45.499_878);
        assert_eq!(angle_subtract(10.0, 350.0), 20.0);
        assert_eq!(angle_wrap_180(190.0), -170.0);
        // fmod keeps the sign: below -180 it stays below (the original's quirk).
        assert_eq!(angle_wrap_180(-190.0), -190.0);
        assert_eq!(angle_wrap_180(-600.0), -240.0);
        assert_eq!(angle_wrap_180(45.5), 45.5);
    }
}
