//! The games' string hashes: the same three in both PC games, and the first
//! two the same as Call of Duty 3's.
//!
//! All add each character times its position plus 119 (Quake III's), with
//! characters as signed chars.

use crate::cstr;

/// The file system's bucket for a file name: lower-cased characters (`.`
/// ends the name, `\` counts as `/`), then `h ^ h >> 10 ^ h >> 20` masked to
/// `hash_size` (a power of two). The same as Call of Duty 3's: this is
/// `cod3_game`'s port, checked against both PC games.
///
/// Original: `FS_HashFileName` at 0x00421AA0 (CoD2MP_s.exe 1.3)
/// Original: `FS_HashFileName` at 0x0042C990 (CoDUOMP.exe 1.51)
pub use cod3_game::shared::hash::file_name_hash;

/// The dvar table's bucket (256 of them): lower-cased characters, the sum
/// cut to 8 bits. A NULL name is the game's error ("null name in
/// generateHashValue"); a Rust name can't be NULL. The same as Call of
/// Duty 3's: this is `cod3_game`'s port, checked against both PC games.
/// (Call of Duty 2 calls it `Dvar_GenerateHashValue`.)
///
/// Original: `generateHashValue` at 0x00435A70 (CoD2MP_s.exe 1.3)
/// Original: `generateHashValue` at 0x0043D860 (CoDUOMP.exe 1.51)
pub use cod3_game::shared::hash::cvar_hash;

/// The hash of up to `max_len` characters, case kept, folded
/// (`h ^ h >> 10 ^ h >> 20`) but not masked. A `max_len` of 0 or less
/// hashes nothing (0).
///
/// Original: `Com_HashKey` at 0x0042D8F0 (CoD2MP_s.exe 1.3)
/// Original: `Com_HashKey` at 0x00435480 (CoDUOMP.exe 1.51)
pub fn hash_key(s: &[u8], max_len: i32) -> i32 {
    let mut hash: i32 = 0;
    let count = usize::try_from(max_len).unwrap_or(0);
    for (i, &c) in cstr::text(s).iter().take(count).enumerate() {
        hash = hash.wrapping_add((c as i8 as i32).wrapping_mul(i as i32 + 119));
    }
    hash ^ (hash >> 10) ^ (hash >> 20)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn hash_key_counts_signed_characters() {
        assert_eq!(hash_key(b"", 10), 0);
        assert_eq!(hash_key(b"A", 0), 0);
        assert_eq!(hash_key(b"A", 1), (65 * 119) ^ ((65 * 119) >> 10));
        assert_eq!(hash_key(b"\xFF", 4), -119 ^ (-119 >> 10) ^ (-119 >> 20));
    }
}
