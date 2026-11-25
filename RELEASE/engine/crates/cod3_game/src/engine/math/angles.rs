//! Angles in degrees, at full precision.
//!
//! The console rounds a normalized angle toward zero to one of 65536 steps
//! (0.0055°), because Quake III sent angles over the network as 16-bit
//! numbers ([`math::angles`](crate::math::angles)). With
//! a high-resolution mouse those steps are coarser than the input, so the
//! engine keeps the full `f32`. Results stay within one step of the console's.
//!
//! For local use only (the view, menus, tools): the game rules that a 360
//! or recomp peer also runs use the console's angles
//! ([`math::angles`](crate::math::angles)), or the peers would disagree.

use crate::math::angles as console;

/// Brings an angle into (-180, 180], without rounding it to 16-bit steps.
pub fn normalize_180(angle: f32) -> f32 {
    // Under one turn, `angle % 360.0` is `angle` itself, so the slow
    // remainder (a library call) is only needed for bigger ones.
    let mut a = if angle.abs() < 360.0 {
        angle
    } else {
        angle % 360.0
    };
    if a > 180.0 {
        a -= 360.0;
    } else if a <= -180.0 {
        a += 360.0;
    }
    a
}

/// `angle1 - angle2` in [-180, 180] (the console's, which isn't rounded).
pub fn subtract(angle1: f32, angle2: f32) -> f32 {
    console::angle_subtract(angle1, angle2)
}

/// [`normalize_180`] of `angle1 - angle2`.
pub fn delta(angle1: f32, angle2: f32) -> f32 {
    normalize_180(angle1 - angle2)
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::test_random::Random;

    #[test]
    fn within_one_console_step() {
        let step = console::SHORT_TO_ANGLE;
        let mut r = Random::new(3);
        for _ in 0..100_000 {
            let a = (r.below(1_440_000) as f32 - 720_000.0) / 1000.0;
            let engine = normalize_180(a);
            let console = console::angle_normalize_180(a);
            assert!(engine > -180.0 && engine <= 180.0, "{a} -> {engine}");
            let mut difference = (engine - console).abs();
            if difference > 180.0 {
                difference = 360.0 - difference;
            }
            assert!(
                difference <= step * 1.01,
                "{a}: engine {engine}, console {console}"
            );
        }
        assert_eq!(normalize_180(190.0), -170.0);
        assert_eq!(normalize_180(-180.0), 180.0);
        assert_eq!(normalize_180(-0.0).to_bits(), (-0.0f32).to_bits());
        assert!(normalize_180(f32::NAN).is_nan());
    }
}
