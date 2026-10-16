//! The C runtime's `sin` and `cos`, as the Xbox 360 build of the game has them.
//!
//! The game's trigonometry (view angles, the level scripts' `sin`/`cos`,
//! effects) goes through these two double-precision functions. Each reduces
//! the angle by multiples of π in two parts (a "Cody-Waite" reduction, with
//! π split into a 34-bit head and a tail) and evaluates one odd polynomial
//! of degree 17 with fused multiply-adds. Ported operation for operation,
//! they give the console's results on every PC, which the platform's own
//! `sin` would not. From 2.2e8 on they give a NaN (with the sign bit set).

use crate::ppc::{fctid, fctidz, fsel};

/// π/2 (0x3FF921FB54442D18).
const HALF_PI: f64 = std::f64::consts::FRAC_PI_2;
/// 1/π (0x3FD45F306DC9C883).
const INV_PI: f64 = std::f64::consts::FRAC_1_PI;
/// π's head: its first 34 bits (0x400921FB54400000), so `n * PI_HEAD` is exact.
const PI_HEAD: f64 = 3.1415926534682512;
/// π minus `PI_HEAD` (0x3DE0B4611A600000).
const PI_TAIL: f64 = 1.2154201012607932e-10;
/// From here on the reduction loses every bit, and the result is NaN (2.2e8).
const RANGE_LIMIT: f64 = 220000000.0;
/// What the runtime returns out of range: a quiet NaN with the sign bit set.
const OUT_OF_RANGE: f64 = -f64::NAN; // 0xFFF8000000000000

/// The polynomial's coefficients for r³, r⁵ ... r¹⁷ (close to the Taylor series' ±1/n!).
const COEFFICIENTS: [f64; 8] = [
    -0.16666666666666666,    // r³: -1/6
    0.008333333333333165,    // r⁵: 1/120
    -0.0001984126984120184,  // r⁷
    2.7557319210152756e-06,  // r⁹
    -2.5052106798274583e-08, // r¹¹
    1.605893649037159e-10,   // r¹³
    -7.642917806891047e-13,  // r¹⁵
    2.7204790957888847e-15,  // r¹⁷
];

/// sin(r) for |r| <= π/2: r · (1 + z·(c3 + z·(c5 + ... + z·c17))), z = r², Horner with fmadd.
fn odd_polynomial(r: f64) -> f64 {
    let z = r * r;
    let mut p = COEFFICIENTS[7];
    for &c in COEFFICIENTS[..7].iter().rev() {
        p = p.mul_add(z, c);
    }
    p.mul_add(z, 1.0) * r
}

/// `x - n·π` in two fused steps (`fnmsub`), `n` being a whole number as a double.
fn reduce(x: f64, n: f64) -> f64 {
    let r = (-PI_HEAD).mul_add(n, x);
    (-PI_TAIL).mul_add(n, r)
}

/// Original: `sin` at 0x8234B538
pub fn sin(x: f64) -> f64 {
    let ax = x.abs();
    let sign = fsel(x, 1.0, -1.0);
    let n = fctid(INV_PI * ax) as f64;
    let mut s = odd_polynomial(reduce(ax, n));
    if fctidz(n) & 1 != 0 {
        s = -s;
    }
    let result = fsel(ax - RANGE_LIMIT, OUT_OF_RANGE, s * sign);
    if ax.to_bits() == 0 {
        x // ±0 comes back as it went in
    } else {
        result
    }
}

/// cos(x) = sin(|x| + π/2), with the shift folded into the reduction: `n` is
/// counted from |x| + π/2 and |x| is reduced by (n - ½)·π.
///
/// Original: `cos` at 0x8234B610
pub fn cos(x: f64) -> f64 {
    let ax = x.abs();
    let shifted = HALF_PI + ax;
    let n = fctid(INV_PI * shifted) as f64;
    let mut c = odd_polynomial(reduce(ax, n - 0.5));
    if fctidz(n) & 1 != 0 {
        c = -c;
    }
    let result = fsel(shifted - RANGE_LIMIT, OUT_OF_RANGE, c);
    if ax != 0.0 {
        result
    } else {
        1.0
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn constants_have_the_runtimes_bits() {
        assert_eq!(HALF_PI.to_bits(), 0x3FF9_21FB_5444_2D18);
        assert_eq!(INV_PI.to_bits(), 0x3FD4_5F30_6DC9_C883);
        assert_eq!(PI_HEAD.to_bits(), 0x4009_21FB_5440_0000);
        assert_eq!(PI_TAIL.to_bits(), 0x3DE0_B461_1A60_0000);
        assert_eq!(RANGE_LIMIT.to_bits(), 0x41AA_39DE_0000_0000);
        assert_eq!(OUT_OF_RANGE.to_bits(), 0xFFF8_0000_0000_0000);
        let expected: [u64; 8] = [
            0xBFC5_5555_5555_5555,
            0x3F81_1111_1111_10B0,
            0xBF2A_01A0_1A01_3E1A,
            0x3EC7_1DE3_A524_F063,
            0xBE5A_E645_4B5D_C0AB,
            0x3DE6_123C_686A_D430,
            0xBD6A_E420_DC08_499C,
            0x3CE8_80FF_6993_DF95,
        ];
        for (c, bits) in COEFFICIENTS.iter().zip(expected) {
            assert_eq!(c.to_bits(), bits);
        }
    }

    #[test]
    fn close_to_the_real_functions() {
        for i in -1000..1000 {
            let x = f64::from(i) * 0.0137;
            assert!((sin(x) - x.sin()).abs() < 1e-15, "sin {x}");
            assert!((cos(x) - x.cos()).abs() < 1e-15, "cos {x}");
        }
        assert_eq!(sin(-0.0).to_bits(), (-0.0f64).to_bits());
        assert_eq!(cos(0.0), 1.0);
        assert!(sin(3e8).is_nan());
    }
}
