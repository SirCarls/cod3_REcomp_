//! String helpers shared by the whole game (Quake III's `q_shared.c` family).
//!
//! CoD3 keeps Quake III's names here but not always its behavior: its two
//! case-insensitive compares fold case in opposite directions and return
//! different things (see [`stricmpn`] and [`stricmp`]).

use crate::cstr;
use crate::error::GameError;

/// The `n` that `Q_stricmp` passes to its inlined `Q_stricmpn` (Quake III's 99999).
pub const STRICMP_LIMIT: i32 = 99_999;

fn fold_to_lower(c: i32) -> i32 {
    if (b'A' as i32..=b'Z' as i32).contains(&c) {
        c + (b'a' - b'A') as i32
    } else {
        c
    }
}

fn fold_to_upper(c: i32) -> i32 {
    if (b'a' as i32..=b'z' as i32).contains(&c) {
        c - (b'a' - b'A') as i32
    } else {
        c
    }
}

/// Compares at most `n` characters, ignoring ASCII case.
///
/// Returns 0 when equal, else the difference of the first differing
/// characters after folding `A`–`Z` to lower case, as signed chars (so
/// bytes 0x80 and up compare below `\0`). A negative `n` never runs out.
///
/// Original: `Q_stricmpn` at 0x820CDE10
pub fn stricmpn(s1: &[u8], s2: &[u8], n: i32) -> i32 {
    let mut remaining = n;
    let mut i = 0;
    loop {
        let mut c1 = cstr::signed_at(s1, i);
        let mut c2 = cstr::signed_at(s2, i);
        i += 1;
        let limit_reached = remaining == 0;
        remaining = remaining.wrapping_sub(1);
        if limit_reached {
            return 0;
        }
        if c1 != c2 {
            c1 = fold_to_lower(c1);
            c2 = fold_to_lower(c2);
            if c1 != c2 {
                return c1 - c2;
            }
        }
        if c1 == 0 {
            return 0;
        }
    }
}

/// Compares two strings ignoring ASCII case: 0 when equal, -1 otherwise.
///
/// Unlike Quake III's (and unlike [`stricmpn`]), CoD3's `Q_stricmp` folds to
/// upper case and never returns a positive number, so it can only test for
/// equality, not sort. It stops after [`STRICMP_LIMIT`] characters.
///
/// Original: `Q_stricmp` at 0x82453580
pub fn stricmp(s1: &[u8], s2: &[u8]) -> i32 {
    let mut remaining = STRICMP_LIMIT;
    let mut i = 0;
    loop {
        let mut c1 = cstr::signed_at(s1, i);
        let mut c2 = cstr::signed_at(s2, i);
        i += 1;
        let limit_reached = remaining == 0;
        remaining -= 1;
        if limit_reached {
            return 0;
        }
        if c1 != c2 {
            c1 = fold_to_upper(c1);
            c2 = fold_to_upper(c2);
            if c1 != c2 {
                return -1;
            }
        }
        if c1 == 0 {
            return 0;
        }
    }
}

/// Lower-cases the string in `buffer` in place (`A`–`Z` only: the game's
/// `tolower` leaves every other byte alone).
///
/// Original: `Q_strlwr` at 0x824515D8
pub fn strlwr(buffer: &mut [u8]) {
    let n = cstr::len(buffer);
    for c in &mut buffer[..n] {
        c.make_ascii_lowercase();
    }
}

/// The extension of a file name: the text after the last `.` of its last
/// path component (`/` or `\`), or an empty string.
///
/// Original: `COM_GetExtension` at 0x82524E50
pub fn get_extension(name: &[u8]) -> &[u8] {
    let name = &name[..cstr::len(name)];
    let mut extension: &[u8] = b"";
    for (i, &c) in name.iter().enumerate() {
        match c {
            b'.' => extension = &name[i + 1..],
            b'/' | b'\\' => extension = b"",
            _ => {}
        }
    }
    extension
}

/// The size `COM_DefaultExtension` formats into: its only caller's 128-byte buffer.
pub const DEFAULT_EXTENSION_BUFFER: usize = 128;

/// Adds `.cfg` to a config file name that has no extension.
///
/// The game's `COM_DefaultExtension` was specialized for its one caller,
/// `Cmd_Exec_f`: the size (128) and the extension (`.cfg`) are built in.
/// It looks back from the last character to the first `/` (not `\`); a `.`
/// on the way means the name already has an extension, except at the very
/// first character (`.cfg` alone becomes `..cfg`). Otherwise the name is
/// cut to 127 characters, `.cfg` added and the result cut to 127 again.
///
/// `path` is the caller's buffer; at least 128 bytes. An empty name makes
/// the original read before the buffer; here it just gets `.cfg`.
///
/// Original: `COM_DefaultExtension` at 0x82451900
pub fn default_extension_cfg(path: &mut [u8]) {
    let n = cstr::len(path);
    if n > 0 {
        let mut i = n - 1;
        if path[i] != b'/' {
            loop {
                if i == 0 {
                    break;
                }
                if path[i] == b'.' {
                    return;
                }
                i -= 1;
                if path[i] == b'/' {
                    break;
                }
            }
        }
    }
    let old: Vec<u8> = path[..n.min(DEFAULT_EXTENSION_BUFFER - 1)].to_vec();
    let mut formatted = old;
    formatted.extend_from_slice(b".cfg");
    sprintf_into(path, DEFAULT_EXTENSION_BUFFER, &formatted);
}

/// What `Com_sprintf(dest, size, ...)` leaves in `dest` for a formatted text.
///
/// The game's `Com_sprintf` (0x82451568) is `_vsnprintf` followed by
/// `dest[size - 1] = 0`: at most `size` characters are written, a terminator
/// follows only if there was room, and the last byte of the buffer is zeroed
/// in every case (even after a short string).
pub fn sprintf_into(dest: &mut [u8], size: usize, formatted: &[u8]) {
    let size = size.min(dest.len());
    if size == 0 {
        return;
    }
    let n = formatted.len().min(size);
    dest[..n].copy_from_slice(&formatted[..n]);
    if n < size {
        dest[n] = 0;
    }
    dest[size - 1] = 0;
}

/// Appends `src` to the string in `dest`, a buffer of `dest.len()` bytes.
///
/// Like `strncpy`, it fills the rest of the buffer with zeros, and the last
/// byte is always 0. A string already as long as the buffer is a fatal
/// error ("Q_strcat: already overflowed").
///
/// Original: `Q_strcat` at 0x824535F8
pub fn strcat(dest: &mut [u8], src: &[u8]) -> Result<(), GameError> {
    let size = dest.len();
    let start = cstr::len(dest);
    if start >= size {
        return Err(GameError::fatal("Q_strcat: already overflowed"));
    }
    let room = size - start - 1;
    let src = &src[..cstr::len(src)];
    for (k, slot) in dest[start..start + room].iter_mut().enumerate() {
        *slot = src.get(k).copied().unwrap_or(0);
    }
    dest[size - 1] = 0;
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn compares() {
        assert_eq!(stricmp(b"Hello", b"hELLO"), 0);
        assert_eq!(stricmp(b"abc", b"abd"), -1);
        assert_eq!(stricmp(b"abd", b"abc"), -1);
        assert_eq!(stricmpn(b"abd", b"ABC", 99), 1);
        assert_eq!(stricmpn(b"abc", b"abd", 2), 0);
        assert_eq!(stricmpn(b"\x80", b"", 5), -128);
    }

    #[test]
    fn extensions() {
        assert_eq!(get_extension(b"maps/a.b/c"), b"");
        assert_eq!(get_extension(b"x.tar.gz"), b"gz");
        let mut path = [0u8; 128];
        path[..4].copy_from_slice(b"auto");
        default_extension_cfg(&mut path);
        assert_eq!(cstr::text(&path), b"auto.cfg");
        let mut has = [0u8; 128];
        has[..5].copy_from_slice(b"a.txt");
        default_extension_cfg(&mut has);
        assert_eq!(cstr::text(&has), b"a.txt");
    }

    #[test]
    fn strcat_pads_and_terminates() {
        let mut d = *b"ab\0xxxxx";
        strcat(&mut d, b"cd").unwrap();
        assert_eq!(&d, b"abcd\0\0\0\0");
        let mut full = *b"abcd";
        assert!(strcat(&mut full, b"x").is_err());
    }
}
