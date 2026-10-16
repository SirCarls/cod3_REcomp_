//! The pieces of the Microsoft C runtime the games link in (statically) that
//! ports depend on, with the exact behavior of that runtime.
//!
//! Both executables carry the same runtime code (`tolower` is the same
//! machine code at 0x0057B5B6 in `CoD2MP_s.exe` and 0x0056D1FD in
//! `CoDUOMP.exe`), with per-thread locale data. The games never select a
//! locale, so it stays "C".

/// `tolower` in the "C" locale: only `A`-`Z` change. The argument is an
/// `int`, and the games pass `movsx` bytes, so 0x80 and up arrive negative
/// and come back unchanged.
#[inline]
pub fn tolower(c: i32) -> i32 {
    if (b'A' as i32..=b'Z' as i32).contains(&c) {
        c + 32
    } else {
        c
    }
}

/// `toupper` in the "C" locale: only `a`-`z` change.
#[inline]
pub fn toupper(c: i32) -> i32 {
    if (b'a' as i32..=b'z' as i32).contains(&c) {
        c - 32
    } else {
        c
    }
}

/// `strncpy(dest, src, count)` with `count = dest.len()`: copies `src` up to
/// its terminator, and fills the rest of the `count` bytes with zeros. A
/// `src` of `count` characters or more leaves no terminator.
pub fn strncpy(dest: &mut [u8], src: &[u8]) {
    let n = crate::cstr::len(src).min(dest.len());
    dest[..n].copy_from_slice(&src[..n]);
    dest[n..].fill(0);
}

/// What `Com_sprintf(dest, size, ...)` leaves for a formatted text, and what
/// it returns.
///
/// Both games' `Com_sprintf` is `_vsnprintf(dest, size, ...)` followed by
/// `dest[size - 1] = 0`, and returns `_vsnprintf`'s result. This runtime's
/// `_vsnprintf` writes at most `size` characters and a terminator only if
/// there is room, and returns the length, or -1 when the text didn't fit
/// (a text of exactly `size` characters fits: no terminator, its length).
/// So the last byte is always zeroed, even after a short text.
///
/// `size` is at most `dest.len()` and at least 1.
pub fn sprintf(dest: &mut [u8], size: usize, formatted: &[u8]) -> i32 {
    cod3_game::shared::strings::sprintf_into(dest, size, formatted);
    if formatted.len() > size {
        -1
    } else {
        formatted.len() as i32
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn case_only_changes_ascii_letters() {
        assert_eq!(tolower(b'Q' as i32), b'q' as i32);
        assert_eq!(tolower(-61), -61);
        assert_eq!(toupper(b'q' as i32), b'Q' as i32);
        assert_eq!(toupper(b'{' as i32), b'{' as i32);
    }

    #[test]
    fn strncpy_pads_and_may_not_terminate() {
        let mut d = [0xAAu8; 6];
        strncpy(&mut d, b"ab");
        assert_eq!(&d, b"ab\0\0\0\0");
        strncpy(&mut d, b"abcdefgh");
        assert_eq!(&d, b"abcdef");
    }

    #[test]
    fn sprintf_cuts_and_reports() {
        let mut d = [0x55u8; 8];
        assert_eq!(sprintf(&mut d, 4, b"abcd"), 4);
        assert_eq!(&d[..5], b"abc\0\x55");
        assert_eq!(sprintf(&mut d, 4, b"abcde"), -1);
        assert_eq!(sprintf(&mut d, 4, b"a"), 1);
        assert_eq!(&d[..4], b"a\0c\0");
    }
}
