//! NUL-terminated strings in fixed-size byte buffers, the way the game keeps text.
//!
//! The game's strings are bytes, not UTF-8 (its text has 0x14/0x15 markers
//! and Latin-1 letters), so they are `&[u8]` here. Two forms appear:
//!
//! * a **string argument**: a `&[u8]` holding just the characters. Reading
//!   past its end gives 0, like reaching the terminator ([`at`]).
//! * a **string buffer**: a `&mut [u8]` the size of the game's array, holding
//!   the characters, a 0, then whatever was there before ([`len`], [`text`]).

/// The byte at `index`, or 0 past the end (where the original finds its terminator).
#[inline]
pub fn at(s: &[u8], index: usize) -> u8 {
    s.get(index).copied().unwrap_or(0)
}

/// The byte at `index` as the PowerPC's `lbz` + `extsb` see it: a signed char.
#[inline]
pub fn signed_at(s: &[u8], index: usize) -> i32 {
    at(s, index) as i8 as i32
}

/// The length of the string in a buffer: up to its first 0, or the whole buffer.
pub fn len(buffer: &[u8]) -> usize {
    crate::bytes::find(0, buffer).unwrap_or(buffer.len())
}

/// The characters of the string in a buffer.
pub fn text(buffer: &[u8]) -> &[u8] {
    &buffer[..len(buffer)]
}

/// Writes `s` and a terminator at `offset`, as far as the buffer goes.
///
/// Returns false when the buffer was too short (the original would have
/// written past its array; the checker never asks for that).
pub fn write_at(buffer: &mut [u8], offset: usize, s: &[u8]) -> bool {
    let room = buffer.len().saturating_sub(offset);
    let n = s.len().min(room);
    buffer[offset..offset + n].copy_from_slice(&s[..n]);
    if n < room {
        buffer[offset + n] = 0;
        n == s.len()
    } else {
        false
    }
}

/// `strcat`: appends `s` to the string in `buffer`.
pub fn append(buffer: &mut [u8], s: &[u8]) -> bool {
    let end = len(buffer);
    write_at(buffer, end, s)
}
