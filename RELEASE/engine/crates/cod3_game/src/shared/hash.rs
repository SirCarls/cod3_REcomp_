//! The game's string hashes.
//!
//! * [`name_hash`]: names the level scripts and the engine trade instead of
//!   text: events (`notify`, `endon`), level functions, animations, tags.
//!   It's djb2 (`h = h * 33 + c`, from 0) over the text with letters
//!   lower-cased; bytes 0x80 and up count as negative numbers.
//! * [`cvar_hash`]: the dvar table's bucket (Quake III's `generateHashValue`).
//! * [`file_name_hash`]: the file system's bucket (Quake III's `FS_HashFileName`).

use crate::cstr;

/// A name and its hash, as the game keeps them: 32 bytes, the hash then up
/// to 27 characters (lower-cased) and a terminator.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct NameKey {
    pub hash: u32,
    pub text: [u8; NAME_KEY_TEXT],
}

/// The text part of a [`NameKey`]: 27 characters and the terminator.
pub const NAME_KEY_TEXT: usize = 28;

/// A byte as the game's `isalpha` + `tolower` see it: ASCII letters lowered, the rest as is.
fn fold(c: u8) -> u8 {
    c.to_ascii_lowercase()
}

/// A character as [`name_hash`] adds it: lower-cased, then as a signed char.
fn hash_letter(c: u8) -> u32 {
    fold(c) as i8 as i32 as u32
}

/// The hash and the stored (cut, lower-cased) text of a name.
///
/// Original: `NameHash_Init` at 0x820A0118
pub fn name_key(name: &[u8]) -> NameKey {
    let name = &name[..cstr::len(name)];
    let mut key = NameKey {
        hash: 0,
        text: [0; NAME_KEY_TEXT],
    };
    for (i, &c) in name.iter().enumerate() {
        let c = fold(c);
        key.hash = key.hash.wrapping_mul(33).wrapping_add(hash_letter(c));
        if i < NAME_KEY_TEXT - 1 {
            key.text[i] = c;
        }
    }
    key
}

impl NameKey {
    /// The 32 bytes the game stores (big-endian hash first).
    pub fn to_guest_bytes(&self) -> [u8; 32] {
        let mut out = [0u8; 32];
        out[..4].copy_from_slice(&self.hash.to_be_bytes());
        out[4..].copy_from_slice(&self.text);
        out
    }
}

/// The hash of a name (`h = h * 33 + c` over the lower-cased text). A missing
/// name (NULL in the original) hashes to 0, like an empty one.
///
/// Original: `NameHash` at 0x820CEBB8
pub fn name_hash(name: &[u8]) -> u32 {
    // Four characters per step: h·33⁴ + c0·33³ + c1·33² + c2·33 + c3 is what
    // four single steps add up to (modulo 2³²), with one multiply in the
    // chain from one step to the next instead of four. The text ends at the
    // first 0, so a group that has one finishes a character at a time.
    let step = |hash: u32, c: u8| hash.wrapping_mul(33).wrapping_add(hash_letter(c));
    let finish = |hash: u32, rest: &[u8]| {
        rest.iter()
            .take_while(|&&c| c != 0)
            .fold(hash, |h, &c| step(h, c))
    };
    let mut groups = name.chunks_exact(4);
    let mut hash = 0u32;
    for group in &mut groups {
        if group.contains(&0) {
            return finish(hash, group);
        }
        hash = hash
            .wrapping_mul(33 * 33 * 33 * 33)
            .wrapping_add(hash_letter(group[0]).wrapping_mul(33 * 33 * 33))
            .wrapping_add(hash_letter(group[1]).wrapping_mul(33 * 33))
            .wrapping_add(hash_letter(group[2]).wrapping_mul(33))
            .wrapping_add(hash_letter(group[3]));
    }
    finish(hash, groups.remainder())
}

/// The 256 buckets of the dvar table.
pub const CVAR_HASH_SIZE: u32 = 256;

/// Sum of each lower-cased character times (its position + 119), cut to 8 bits.
///
/// (A NULL name is a fatal error in the original: "null name in generateHashValue".)
///
/// Original: `generateHashValue` at 0x82526148
pub fn cvar_hash(name: &[u8]) -> u32 {
    let mut hash: i32 = 0;
    for (i, &c) in name[..cstr::len(name)].iter().enumerate() {
        let letter = fold(c) as i8 as i32;
        hash = hash.wrapping_add(letter.wrapping_mul(i as i32 + 119));
    }
    hash as u32 & (CVAR_HASH_SIZE - 1)
}

/// Like [`cvar_hash`] but stops at the first `.` and counts `\` as `/`, then
/// folds the high bits down (`h ^ h >> 10 ^ h >> 20`) and masks to `hash_size`
/// (a power of two).
///
/// Original: `FS_HashFileName` at 0x82525038
pub fn file_name_hash(name: &[u8], hash_size: u32) -> u32 {
    let mut hash: i32 = 0;
    for (i, &c) in name[..cstr::len(name)].iter().enumerate() {
        let mut letter = fold(c);
        if letter == b'.' {
            break;
        }
        if letter == b'\\' {
            letter = b'/';
        }
        hash = hash.wrapping_add((letter as i8 as i32).wrapping_mul(i as i32 + 119));
    }
    let folded = hash ^ (hash >> 10) ^ (hash >> 20);
    folded as u32 & hash_size.wrapping_sub(1)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn hashes() {
        assert_eq!(name_hash(b""), 0);
        assert_eq!(name_hash(b"End"), name_hash(b"end"));
        assert_eq!(name_hash(b"ab"), 97 * 33 + 98);
        let key = name_key(b"a_very_long_name_with_more_than_27_chars");
        assert_eq!(&key.text[..27], b"a_very_long_name_with_more_");
        assert_eq!(key.text[27], 0);
        assert_eq!(cvar_hash(b"A"), (97 * 119) & 255);
    }

    #[test]
    fn four_at_a_time_is_the_same_hash() {
        let mut r = crate::test_random::Random::new(29);
        for _ in 0..20_000 {
            let text: Vec<u8> = (0..r.below(40))
                .map(|_| if r.below(20) == 0 { 0 } else { r.next() as u8 })
                .collect();
            assert_eq!(name_hash(&text), name_key(&text).hash, "{text:?}");
        }
    }
}
