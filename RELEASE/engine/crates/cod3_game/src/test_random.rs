//! A small deterministic generator for this crate's tests (xorshift32).

pub struct Random(u32);

impl Random {
    pub fn new(seed: u32) -> Self {
        Random(seed.max(1))
    }

    pub fn next(&mut self) -> u32 {
        self.0 ^= self.0 << 13;
        self.0 ^= self.0 >> 17;
        self.0 ^= self.0 << 5;
        self.0
    }

    /// 0 to `n - 1` (0 when `n` is 0).
    pub fn below(&mut self, n: u32) -> u32 {
        if n == 0 {
            0
        } else {
            self.next() % n
        }
    }
}
