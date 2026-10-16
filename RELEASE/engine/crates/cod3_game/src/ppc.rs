//! PowerPC arithmetic the game relies on, where Rust's own operators differ.
//!
//! Plain `f32` arithmetic (`+ - * /`, `sqrt`) already rounds like the
//! PowerPC's single-precision instructions (`fadds`, `fmuls`, `fdivs`,
//! `fsqrts`). What needs care:
//!
//! * float to integer conversions: `fctiwz` truncates and saturates, and
//!   turns NaN into `i32::MIN`, where Rust's `as i32` gives 0;
//! * fused multiply-add (`fmadds`, `fmsubs`, `fnmsubs`): one rounding, never
//!   `a * b + c`. [`fmadds`] is exact on every platform: the CPU's own
//!   instruction when the build enables it, otherwise a short exact method in
//!   `f64` (faster than the C library's `fmaf`, and the same bits whatever C
//!   library the player's system has).

/// `fctiwz`: float to a 32-bit integer, rounding toward zero.
///
/// Out-of-range values saturate to `i32::MIN` / `i32::MAX`; NaN becomes
/// `i32::MIN` (0x80000000), as the PowerPC Book I specifies.
#[inline]
pub fn fctiwz(value: f64) -> i32 {
    if value.is_nan() {
        i32::MIN
    } else {
        value as i32 // Rust truncates toward zero and saturates, like the PowerPC.
    }
}

/// `fctidz`: float to a 64-bit integer, rounding toward zero (NaN → `i64::MIN`).
#[inline]
pub fn fctidz(value: f64) -> i64 {
    if value.is_nan() {
        i64::MIN
    } else {
        value as i64
    }
}

/// `fctid` in the default rounding mode: float to a 64-bit integer, rounding to
/// nearest (ties to even), saturating, NaN → `i64::MIN`.
#[inline]
pub fn fctid(value: f64) -> i64 {
    if value.is_nan() {
        i64::MIN
    } else {
        value.round_ties_even() as i64
    }
}

/// `fsel`: `a >= 0.0 ? if_true : otherwise`. -0.0 counts as `>= 0`; NaN picks `otherwise`.
#[inline]
pub fn fsel(a: f64, if_true: f64, otherwise: f64) -> f64 {
    if a >= 0.0 {
        if_true
    } else {
        otherwise
    }
}

/// `fmadds`: `a * b + c` with a single rounding to `f32`.
#[inline]
pub fn fmadds(a: f32, b: f32, c: f32) -> f32 {
    // x86-64 has the instruction only from Haswell on (2013), so it is used
    // when the build targets such CPUs (`-C target-cpu=native` or
    // `x86-64-v3`); ARM64 always has it.
    if cfg!(any(target_feature = "fma", target_arch = "aarch64")) {
        a.mul_add(b, c)
    } else {
        fmadds_through_f64(a, b, c)
    }
}

/// `fmsubs`: `a * b - c` with a single rounding to `f32`.
#[inline]
pub fn fmsubs(a: f32, b: f32, c: f32) -> f32 {
    fmadds(a, b, -c)
}

/// `fnmsubs`: `-(a * b - c)`, that is `c - a * b`, with a single rounding.
#[inline]
pub fn fnmsubs(a: f32, b: f32, c: f32) -> f32 {
    -fmsubs(a, b, c)
}

/// `a * b + c` rounded once to `f32`, with `f64` arithmetic only.
///
/// The product of two `f32`s is exact in an `f64` (24 + 24 bits fit in 53).
/// The sum can't be exact in general, and rounding it to `f64` and then to
/// `f32` could round twice the wrong way. So the sum is rounded "to odd"
/// instead: when it isn't exact, the `f64` whose last bit is 1 is taken (of
/// the two around the exact sum). An `f64` rounded that way still rounds to
/// the right `f32`, since it carries 29 more bits than an `f32` needs
/// (Boldo and Melquiond, "When double rounding is odd", 2005). The rounding
/// error of the `f64` sum is found exactly with Knuth's two-sum.
#[inline]
fn fmadds_through_f64(a: f32, b: f32, c: f32) -> f32 {
    let product = f64::from(a) * f64::from(b);
    let addend = f64::from(c);
    let sum = product + addend;
    if !sum.is_finite() {
        return sum as f32; // infinities and NaN pass through
    }
    // Two-sum: `error` is exactly (product + addend) - sum.
    let addend_part = sum - product;
    let error = (product - (sum - addend_part)) + (addend - addend_part);
    let bits = sum.to_bits();
    if error == 0.0 || bits & 1 == 1 {
        return sum as f32; // exact, or already odd
    }
    // Inexact and even: step one unit toward the exact sum, which is odd.
    let away_from_zero = (error > 0.0) == (sum > 0.0);
    let odd = if away_from_zero { bits + 1 } else { bits - 1 };
    f64::from_bits(odd) as f32
}

#[cfg(test)]
mod fma_cases;

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn conversions_saturate_like_the_powerpc() {
        assert_eq!(fctiwz(f64::NAN), i32::MIN);
        assert_eq!(fctiwz(1e20), i32::MAX);
        assert_eq!(fctiwz(-1e20), i32::MIN);
        assert_eq!(fctiwz(-2.9), -2);
        assert_eq!(fctidz(f64::NAN), i64::MIN);
    }

    #[test]
    fn fused_multiply_add_rounds_once() {
        // 1 + 2^-24 can't be stored in an f32, but (1 + 2^-12)^2 - 1 keeps it when fused.
        let a = 1.0 + f32::EPSILON * 2048.0; // 1 + 2^-12
        assert_ne!(fmadds(a, a, -1.0), a * a - 1.0);
        assert_ne!(fmadds_through_f64(a, a, -1.0), a * a - 1.0);
    }

    /// Cases built so that rounding twice gives the wrong answer, with exact
    /// answers (`tools/decomp/fma_cases.py`): `fmadds` must be right on all of
    /// them on every machine, and rounding through `f64` must be wrong on all
    /// of them (so the cases stay sharp).
    #[test]
    fn fmadds_is_exact_on_hard_cases() {
        for &(a, b, c, expected) in &fma_cases::F32_CASES {
            let (a, b, c) = (f32::from_bits(a), f32::from_bits(b), f32::from_bits(c));
            assert_eq!(fmadds(a, b, c).to_bits(), expected, "{a:e} * {b:e} + {c:e}");
            assert_eq!(fmadds_through_f64(a, b, c).to_bits(), expected);
            let rounded_twice = (f64::from(a) * f64::from(b) + f64::from(c)) as f32;
            assert_ne!(rounded_twice.to_bits(), expected);
        }
    }

    /// The C runtime's `sin`/`cos` port (`math::crt`) uses `f64::mul_add`,
    /// which is the CPU's instruction when the build enables it and the C
    /// library's `fma` otherwise. If this fails, this platform's `fma` rounds
    /// twice (as some C libraries do, through the x87's 80-bit format), and
    /// players on it would compute other `sin`/`cos` bits than everyone else.
    #[test]
    fn the_platforms_f64_fused_multiply_add_is_exact() {
        for &(a, b, c, expected) in &fma_cases::F64_CASES {
            let (a, b, c) = (f64::from_bits(a), f64::from_bits(b), f64::from_bits(c));
            assert_eq!(a.mul_add(b, c).to_bits(), expected, "{a:e} * {b:e} + {c:e}");
        }
    }

    /// The `f64` method against the CPU's or glibc's `fmaf`, which round
    /// correctly (elsewhere `f32::mul_add` may be a C library call that
    /// doesn't, so this runs only where it's known to): any bit pattern
    /// (infinities, NaN, denormals), and products that nearly cancel the
    /// addend.
    #[test]
    #[cfg(any(
        target_feature = "fma",
        target_arch = "aarch64",
        all(target_os = "linux", target_env = "gnu")
    ))]
    fn fused_multiply_add_through_f64_is_exact() {
        let same = |x: f32, y: f32| x.to_bits() == y.to_bits() || (x.is_nan() && y.is_nan());
        let mut r = crate::test_random::Random::new(5);
        let mut bits = || r.next();
        for i in 0..1_000_000 {
            let (a, b, c) = if i % 2 == 0 {
                (
                    f32::from_bits(bits()),
                    f32::from_bits(bits()),
                    f32::from_bits(bits()),
                )
            } else {
                let a = f32::from_bits((bits() & 0x807F_FFFF) | 0x3F80_0000);
                let b = f32::from_bits((bits() & 0x007F_FFFF) | 0x3F80_0000);
                let near = (a * b).to_bits() ^ (bits() & 7) ^ ((bits() & 3) << 20);
                (a, b, -f32::from_bits(near))
            };
            let expected = a.mul_add(b, c);
            let got = fmadds_through_f64(a, b, c);
            assert!(
                same(got, expected),
                "{a:e} * {b:e} + {c:e}: {got:e}, expected {expected:e}"
            );
            assert!(same(fmadds(a, b, c), expected));
        }
        for (a, b, c) in [
            (0.0f32, -1.0, 0.0),
            (-0.0, 1.0, -0.0),
            (f32::MAX, 2.0, -f32::MAX),
            (f32::MIN_POSITIVE, 0.5, 0.0),
            (f32::INFINITY, 0.0, 1.0),
        ] {
            assert!(same(fmadds_through_f64(a, b, c), a.mul_add(b, c)));
        }
    }
}
