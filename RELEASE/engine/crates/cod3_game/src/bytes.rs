//! Byte loops that look at eight bytes at a time.
//!
//! Text in the game is short, but the engine has no length limits, so the
//! loops every string goes through (finding a terminator or a separator,
//! comparing names ignoring case) treat a `u64` as eight bytes at once
//! ("SWAR": SIMD within a register). Plain safe Rust, the same on every CPU,
//! and the same results as a byte-by-byte loop (the tests compare them).

/// Eight copies of a byte.
const fn repeat(byte: u8) -> u64 {
    u64::from_ne_bytes([byte; 8])
}

const ONES: u64 = repeat(0x01);
const HIGH_BITS: u64 = repeat(0x80);

/// Eight bytes from `chunk`, the first one in the lowest bits.
#[inline]
fn word(chunk: &[u8]) -> u64 {
    let mut bytes = [0u8; 8];
    bytes.copy_from_slice(chunk);
    u64::from_le_bytes(bytes)
}

/// The high bit of each byte of `word` that is zero (only the lowest one is
/// reliable: a zero byte can mark the byte above it too, never one below).
#[inline]
fn zero_bytes(word: u64) -> u64 {
    word.wrapping_sub(ONES) & !word & HIGH_BITS
}

/// The high bit of each byte of `word` that is zero, exactly (unlike
/// [`zero_bytes`], every bit is reliable, so all of them can be used).
#[inline]
fn exact_zero_bytes(word: u64) -> u64 {
    let seven_bits = repeat(0x7F);
    // A byte's high bit ends up clear only when its low seven bits were 0
    // (adding 0x7F carries into the high bit) and its high bit was 0.
    !(((word & seven_bits) + seven_bits) | word | seven_bits)
}

/// Where `byte` appears in a text, one place after another, eight bytes at
/// a time. [`find`] skips to the next word with a match, and that word
/// becomes a mask with a bit per match, taken lowest first: long gaps go
/// at `find`'s speed, and matches close together (an info string's
/// separators) don't need a search each.
pub struct Positions<'a> {
    text: &'a [u8],
    byte: u8,
    pattern: u64,
    /// Where the current word starts.
    word_start: usize,
    /// The matches in the current word not yet returned (bit 8·i + 7 for byte i).
    matches: u64,
}

impl<'a> Positions<'a> {
    /// The positions of `byte` in `text`, from `from` on.
    pub fn new(byte: u8, text: &'a [u8], from: usize) -> Self {
        let pattern = repeat(byte);
        let mut positions = Positions {
            text,
            byte,
            pattern,
            word_start: from,
            matches: 0,
        };
        positions.matches = positions.matches_at(from);
        positions
    }

    /// The match mask of the (up to) eight bytes at `start`; past the end
    /// of the text it reads bytes that never match.
    #[inline]
    fn matches_at(&self, start: usize) -> u64 {
        let rest = self.text.get(start..).unwrap_or(&[]);
        let word = match rest.get(..8) {
            Some(chunk) => word(chunk),
            None => {
                let mut bytes = self.pattern.to_le_bytes().map(|b| !b);
                bytes[..rest.len()].copy_from_slice(rest);
                u64::from_le_bytes(bytes)
            }
        };
        exact_zero_bytes(word ^ self.pattern)
    }
}

impl Iterator for Positions<'_> {
    type Item = usize;

    #[inline]
    fn next(&mut self) -> Option<usize> {
        if self.matches == 0 {
            let from = self.word_start + 8;
            let gap = find(self.byte, self.text.get(from..)?)?;
            self.word_start = from + gap; // the next match opens the next word
            self.matches = self.matches_at(self.word_start);
        }
        let position = self.word_start + self.matches.trailing_zeros() as usize / 8;
        self.matches &= self.matches - 1; // clear the lowest
        Some(position)
    }
}

/// Where `byte` first appears in `text`, if it does.
#[inline]
pub fn find(byte: u8, text: &[u8]) -> Option<usize> {
    let pattern = repeat(byte);
    let mut words = text.chunks_exact(8);
    let mut offset = 0;
    for chunk in &mut words {
        let found = zero_bytes(word(chunk) ^ pattern);
        if found != 0 {
            return Some(offset + found.trailing_zeros() as usize / 8);
        }
        offset += 8;
    }
    words
        .remainder()
        .iter()
        .position(|&c| c == byte)
        .map(|i| offset + i)
}

/// Lower-cases the ASCII letters `A`-`Z` among the eight bytes of `word`
/// (bytes 0x80 and up stay as they are, like `to_ascii_lowercase`).
#[inline]
fn lowercase_word(word: u64) -> u64 {
    let seven_bits = word & repeat(0x7F);
    // Adding these sets a byte's high bit when it is >= 'A' or >= 'Z' + 1;
    // seven-bit bytes can't carry into their neighbors.
    let at_least_a = seven_bits + repeat(0x80 - b'A');
    let past_z = seven_bits + repeat(0x80 - b'Z' - 1);
    let ascii = !word;
    let upper = at_least_a & !past_z & ascii & HIGH_BITS;
    word | (upper >> 2) // 0x80 >> 2 is 0x20, the case bit
}

/// Where `a` and `b` first differ ignoring ASCII case, comparing up to the
/// shorter length; `None` when one is a prefix of the other.
#[inline]
pub fn first_difference_ignoring_case(a: &[u8], b: &[u8]) -> Option<usize> {
    let shared = a.len().min(b.len());
    let (a, b) = (&a[..shared], &b[..shared]);
    // 32 bytes per step, with one test per step: long names compare at
    // memory speed, and the step that differs is searched afterwards.
    const BLOCK: usize = 32;
    let mut offset = 0;
    for (x, y) in a.chunks_exact(BLOCK).zip(b.chunks_exact(BLOCK)) {
        let mut differences = 0;
        for (x, y) in x.chunks_exact(8).zip(y.chunks_exact(8)) {
            differences |= lowercase_word(word(x)) ^ lowercase_word(word(y));
        }
        if differences != 0 {
            break;
        }
        offset += BLOCK;
    }
    for (x, y) in a[offset..].chunks_exact(8).zip(b[offset..].chunks_exact(8)) {
        let difference = lowercase_word(word(x)) ^ lowercase_word(word(y));
        if difference != 0 {
            return Some(offset + difference.trailing_zeros() as usize / 8);
        }
        offset += 8;
    }
    a[offset..]
        .iter()
        .zip(&b[offset..])
        .position(|(x, y)| !x.eq_ignore_ascii_case(y))
        .map(|i| offset + i)
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::test_random::Random;

    fn text(r: &mut Random, max: u32) -> Vec<u8> {
        let alphabet = [
            b'a', b'Z', b'@', b'[', b'`', b'{', b'\\', 0, 0x80, 0xC1, 0xDA, 0xFF,
        ];
        (0..r.below(max + 1))
            .map(|_| alphabet[r.below(alphabet.len() as u32) as usize])
            .collect()
    }

    #[test]
    fn find_matches_a_byte_loop() {
        let mut r = Random::new(17);
        for _ in 0..50_000 {
            let t = text(&mut r, 40);
            for byte in [b'\\', 0, 0x80, b'a'] {
                assert_eq!(find(byte, &t), t.iter().position(|&c| c == byte), "{t:?}");
            }
        }
    }

    #[test]
    fn positions_match_a_byte_loop() {
        let mut r = Random::new(31);
        for _ in 0..50_000 {
            let t = text(&mut r, 40);
            let from = r.below(t.len() as u32 + 3) as usize;
            for byte in [b'\\', 0, 0xFF, b'a'] {
                let expected: Vec<usize> = (from..t.len()).filter(|&i| t[i] == byte).collect();
                let got: Vec<usize> = Positions::new(byte, &t, from).collect();
                assert_eq!(got, expected, "{t:?} from {from}");
            }
        }
    }

    #[test]
    fn exact_zero_bytes_marks_every_zero() {
        for high in 0..=255u8 {
            for low in 0..=255u8 {
                let bytes = [low, high, 0, 1, 0x80, low, 0, high];
                let marks = exact_zero_bytes(u64::from_le_bytes(bytes)).to_le_bytes();
                assert_eq!(marks, bytes.map(|c| if c == 0 { 0x80 } else { 0 }));
            }
        }
    }

    #[test]
    fn lowercase_word_matches_the_standard_library() {
        for high in 0..=255u8 {
            for low in 0..=255u8 {
                let bytes = [low, high, b'A', b'z', 0x7F, 0xC1, low ^ 0x80, high ^ 0x20];
                let lowered = lowercase_word(u64::from_le_bytes(bytes)).to_le_bytes();
                assert_eq!(lowered, bytes.map(|c| c.to_ascii_lowercase()));
            }
        }
    }

    #[test]
    fn first_difference_matches_a_byte_loop() {
        let mut r = Random::new(19);
        for _ in 0..50_000 {
            let a = text(&mut r, 90);
            let mut b = a.to_ascii_uppercase();
            if !b.is_empty() && r.below(2) == 0 {
                let i = r.below(b.len() as u32) as usize;
                b[i] = b[i].wrapping_add(1);
            }
            b.truncate(r.below(b.len() as u32 + 1) as usize);
            let expected = a
                .iter()
                .zip(&b)
                .position(|(x, y)| !x.eq_ignore_ascii_case(y));
            assert_eq!(
                first_difference_ignoring_case(&a, &b),
                expected,
                "{a:?} {b:?}"
            );
        }
    }
}
