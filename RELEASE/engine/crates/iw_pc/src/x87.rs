//! The x87 FPU at Call of Duty 2's precision: 24-bit significands, the x87's
//! own exponent range.
//!
//! Call of Duty 2 sets the x87 to 24-bit precision every frame, so each
//! arithmetic result in a register is rounded to a `float`'s 24 bits, but a
//! register keeps the x87's wide exponent: `3e38 * 3e38` is a finite number
//! there, and only storing it to a `float` makes it infinite. [`F24`] is
//! such a register value: an `f64` (whose exponent covers everything these
//! ports compute) rounded to 24 significant bits after every operation.
//! [`F24::store`] is `fstp dword ptr`.
//!
//! Rounding an exact result once to 24 bits is what the x87 does. The `f64`
//! result of `+ - * /` and square roots of two 24-bit values rounds once
//! more first; for these five operations that second rounding never changes
//! the 24-bit result (53 >= 2 * 24 + 2). Products and sums also carry their
//! exact error (an FMA, a two-sum), so they are exact for any `f64`
//! operands, such as a 32-bit integer from `fild`.
//!
//! NaNs follow the x87's rules (as the checker's emulator implements them):
//! of two NaNs the one with the larger significand, of two with the same
//! significand the positive one; an invalid operation on numbers (`inf -
//! inf`, `0 * inf`, the root of a negative number) gives the x87's default
//! NaN, negative ("real indefinite"). Loads make every NaN quiet.

use std::ops::{Add, Div, Mul, Neg, Sub};

/// An x87 register value at 24-bit precision.
#[derive(Debug, Clone, Copy, PartialEq, PartialOrd)]
pub struct F24(pub f64);

/// `hi + lo` (an exact pair, `|lo|` below half of `hi`'s last bit) rounded
/// to 24 significant bits, ties to even, keeping `hi`'s exponent range.
fn round24(hi: f64, lo: f64) -> f64 {
    if !hi.is_finite() || hi == 0.0 {
        return hi;
    }
    const DROP: u32 = 52 - 23;
    let bits = hi.to_bits();
    let low = bits & ((1u64 << DROP) - 1);
    let half = 1u64 << (DROP - 1);
    let kept = bits >> DROP;
    let up = match low.cmp(&half) {
        std::cmp::Ordering::Greater => true,
        std::cmp::Ordering::Less => false,
        // Exactly halfway in `hi`'s bits: the exact value's rest decides,
        // else ties to even.
        std::cmp::Ordering::Equal => {
            if lo == 0.0 || lo.is_nan() {
                kept & 1 == 1
            } else {
                (lo > 0.0) == (hi > 0.0)
            }
        }
    };
    // (A carry out of the significand moves into the exponent: still right.)
    f64::from_bits((kept + u64::from(up)) << DROP)
}

/// The x87's default NaN ("real indefinite"): negative, quiet.
fn indefinite() -> f64 {
    f64::from_bits(0xFFF8_0000_0000_0000)
}

/// The NaN an operation on `a` and `b` gives when either is one; `None`
/// when neither is.
pub(crate) fn propagate(a: f64, b: f64) -> Option<f64> {
    let quiet = |x: f64| f64::from_bits(x.to_bits() | 0x0008_0000_0000_0000);
    match (a.is_nan(), b.is_nan()) {
        (false, false) => None,
        (true, false) => Some(quiet(a)),
        (false, true) => Some(quiet(b)),
        (true, true) => {
            let (qa, qb) = (quiet(a), quiet(b));
            let mantissa = |x: f64| x.to_bits() & 0x000F_FFFF_FFFF_FFFF;
            Some(match mantissa(qa).cmp(&mantissa(qb)) {
                std::cmp::Ordering::Greater => qa,
                std::cmp::Ordering::Less => qb,
                std::cmp::Ordering::Equal => {
                    if qa.is_sign_negative() {
                        qb
                    } else {
                        qa
                    }
                }
            })
        }
    }
}

/// An operation's result: the NaN rules, else `exact(a, b)`.
fn op(a: f64, b: f64, exact: impl FnOnce(f64, f64) -> f64) -> f64 {
    if let Some(nan) = propagate(a, b) {
        return nan;
    }
    let r = exact(a, b);
    if r.is_nan() {
        indefinite()
    } else {
        r
    }
}

impl F24 {
    /// `fld dword ptr`: a `float` loaded as it is.
    pub fn load(x: f32) -> F24 {
        F24(f64::from(x))
    }

    /// `fild dword ptr`: an integer, exact (the next operation rounds).
    pub fn int(x: i32) -> F24 {
        F24(f64::from(x))
    }

    /// `fld qword ptr`: a `double` constant, as it is.
    pub fn double(x: f64) -> F24 {
        F24(x)
    }

    /// `fstp dword ptr`: rounded to a `float`, with its range (too big
    /// becomes infinite, too small a denormal or zero).
    pub fn store(self) -> f32 {
        self.0 as f32
    }

    /// `fsqrt` (and the C runtime's `sqrt` on a `float`, whose result is
    /// stored as one).
    pub fn sqrt(self) -> F24 {
        F24(op(self.0, 0.0, |a, _| round24(a.sqrt(), 0.0)))
    }

    /// `fabs`: the sign cleared (a NaN's too).
    pub fn abs(self) -> F24 {
        F24(self.0.abs())
    }

    /// `fistp dword ptr` in the default rounding (to nearest, ties to even);
    /// NaN and anything outside the 32-bit range give `i32::MIN` (the
    /// x87's "integer indefinite").
    pub fn round_to_int(self) -> i32 {
        let r = self.0.round_ties_even();
        if r.is_nan() || !(-2_147_483_648.0..=2_147_483_647.0).contains(&r) {
            i32::MIN
        } else {
            r as i32
        }
    }

    pub fn is_nan(self) -> bool {
        self.0.is_nan()
    }
}

impl From<f32> for F24 {
    fn from(x: f32) -> F24 {
        F24::load(x)
    }
}

impl Add for F24 {
    type Output = F24;
    fn add(self, rhs: F24) -> F24 {
        F24(op(self.0, rhs.0, |a, b| {
            let s = a + b;
            if !s.is_finite() {
                return s;
            }
            let bb = s - a;
            let err = (a - (s - bb)) + (b - bb);
            round24(s, err)
        }))
    }
}

impl Sub for F24 {
    type Output = F24;
    fn sub(self, rhs: F24) -> F24 {
        // (Not `self + -rhs`: a NaN keeps its own sign.)
        F24(op(self.0, rhs.0, |a, b| {
            let s = a - b;
            if !s.is_finite() {
                return s;
            }
            let (a, b) = (a, -b);
            let bb = s - a;
            let err = (a - (s - bb)) + (b - bb);
            round24(s, err)
        }))
    }
}

impl Neg for F24 {
    type Output = F24;
    fn neg(self) -> F24 {
        F24(-self.0)
    }
}

impl Mul for F24 {
    type Output = F24;
    fn mul(self, rhs: F24) -> F24 {
        F24(op(self.0, rhs.0, |a, b| {
            let p = a * b;
            if !p.is_finite() || p == 0.0 {
                return p;
            }
            round24(p, a.mul_add(b, -p))
        }))
    }
}

impl Div for F24 {
    type Output = F24;
    fn div(self, rhs: F24) -> F24 {
        F24(op(self.0, rhs.0, |a, b| round24(a / b, 0.0)))
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn rounds_like_floats_but_keeps_the_range() {
        let a = F24::load(1.0) / F24::load(3.0);
        assert_eq!(a.store(), 1.0f32 / 3.0);
        let big = F24::load(3e38) * F24::load(3e38);
        assert!(big.0.is_finite());
        assert_eq!(big.store(), f32::INFINITY);
        assert_eq!((big.sqrt()).store(), 3e38);
    }

    #[test]
    fn nans_follow_the_x87() {
        let pos = F24::load(f32::from_bits(0x7FC0_0000));
        let neg = F24::load(f32::from_bits(0xFFC0_0000));
        assert_eq!((neg * pos).store().to_bits(), 0x7FC0_0000);
        assert_eq!((pos + neg).store().to_bits(), 0x7FC0_0000);
        let big = F24::load(f32::from_bits(0x7FC0_0001));
        assert_eq!((neg * big).store().to_bits(), 0x7FC0_0001);
        let inf = F24::load(f32::INFINITY);
        assert_eq!((inf - inf).store().to_bits(), 0xFFC0_0000);
        assert_eq!((neg - neg).store().to_bits(), 0xFFC0_0000);
        assert_eq!(F24::load(-1.0).sqrt().store().to_bits(), 0xFFC0_0000);
    }

    #[test]
    fn integer_products_round_once() {
        // 16777217 (2^24 + 1) times 1: the x87 rounds it to 2^24 once.
        assert_eq!((F24::int(16_777_217) * F24::load(1.0)).0, 16_777_216.0);
        // A tie decided by the exact rest: 2^24 + 1 + 2^-30 rounds up.
        let x = F24(16_777_217.0) + F24(2f64.powi(-30));
        assert_eq!(x.0, 16_777_218.0);
    }
}
