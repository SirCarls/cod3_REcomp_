//! The level scripts' random numbers: their own linear congruential
//! generator (the C runtime's `rand` constants), separate from the game's.

/// The generator's state, a 32-bit seed (a global in the original, at 0x825F2E94).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct ScriptRandom {
    pub seed: u32,
}

/// The seed the executable starts with.
pub const INITIAL_SEED: u32 = 0x89AB_CDEF;
/// `seed = seed * 214013 + 2531011` (Microsoft's `rand`).
const MULTIPLIER: u32 = 214_013;
const INCREMENT: u32 = 2_531_011;
/// Each step gives 15 bits: 0..32767.
const STEP_BITS: u32 = 15;
/// 1 / 32768 (0x38000000).
const STEP_SCALE: f32 = 1.0 / 32768.0;

impl Default for ScriptRandom {
    fn default() -> Self {
        ScriptRandom { seed: INITIAL_SEED }
    }
}

impl ScriptRandom {
    /// Advances the generator and returns its next 15 bits.
    fn next(&mut self) -> u32 {
        self.seed = self.seed.wrapping_mul(MULTIPLIER).wrapping_add(INCREMENT);
        self.seed >> 17
    }

    /// `randomint(n)`: 0 to n - 1 (`(value * n) >> 15`; negative n gives 0 down to n + 1).
    ///
    /// Original: `Broc_RandomInt` at 0x820B2508 (level slot 0x140)
    pub fn int(&mut self, n: i32) -> i32 {
        (self.next() as i32).wrapping_mul(n) >> STEP_BITS
    }

    /// `randomfloat(max)`: 0 to just under `max`.
    ///
    /// Original: `Broc_RandomFloat` at 0x820B2538 (level slot 0x144)
    pub fn float(&mut self, max: f32) -> f32 {
        (self.next() as f32 * max) * STEP_SCALE
    }

    /// `randomintrange(min, max)`: `min` to `max - 1`; reversed bounds count down from `max`.
    ///
    /// Original: `Broc_RandomIntRange` at 0x820B2588 (level slot 0x148)
    pub fn int_range(&mut self, min: i32, max: i32) -> i32 {
        let value = self.next() as i32;
        if max > min {
            (value.wrapping_mul(max.wrapping_sub(min)) >> STEP_BITS).wrapping_add(min)
        } else {
            (value.wrapping_mul(min.wrapping_sub(max)) >> STEP_BITS).wrapping_add(max)
        }
    }

    /// `randomfloatrange(min, max)`: from the smaller bound, as in [`int_range`](Self::int_range).
    ///
    /// Original: `Broc_RandomFloatRange` at 0x820B25E0 (level slot 0x14C)
    pub fn float_range(&mut self, min: f32, max: f32) -> f32 {
        let (low, span) = if max > min {
            (min, max - min)
        } else {
            (max, min - max)
        };
        let value = self.next() as f32;
        crate::ppc::fmadds(value * span, STEP_SCALE, low)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn known_sequence() {
        let mut r = ScriptRandom::default();
        let a = r.int(100);
        assert!((0..100).contains(&a));
        let mut same = ScriptRandom::default();
        assert_eq!(same.int(100), a);
        assert!((5..9).contains(&r.int_range(9, 5)));
    }
}
