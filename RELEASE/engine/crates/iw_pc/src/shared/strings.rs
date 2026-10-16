//! String and file-name helpers (Quake III's `q_shared.c` family).
//!
//! Most are the same machine code in both games (United Offensive's copy
//! and Call of Duty 2's differ only in which registers the callers use).
//! What differs from Quake III, and from Call of Duty 3's PowerPC versions:
//!
//! * the compares fold case **to upper** and return **only -1, 0 or 1**
//!   ([`stricmpn`], [`strncmp`]); Quake III returns -1/1 after folding to
//!   lower, Call of Duty 3's `Q_stricmpn` returns the difference after
//!   folding to lower, and its `Q_stricmp` only 0 or -1;
//! * color codes are `^` and a digit only ([`clean_str`]); Quake III takes
//!   any character after the `^`;
//! * there are no NULL or size checks in [`strncpyz`];
//! * the two games strip extensions differently ([`strip_extension`],
//!   [`strip_extension_at_first_dot`]).

use crate::cstr;

/// The `n` Call of Duty 2's `Q_stricmp` gives `Q_stricmpn` (`INT_MAX`).
pub const STRICMP_LIMIT_COD2: i32 = i32::MAX;
/// The `n` United Offensive's `Q_stricmp` gives `Q_stricmpn` (Quake III's 99999).
pub const STRICMP_LIMIT_UO: i32 = 99_999;

/// What `Q_stricmpn` and friends return for two different characters:
/// 1 when the first is greater or equal (as signed chars), else -1.
#[inline]
fn order(c1: i32, c2: i32) -> i32 {
    if c1 >= c2 {
        1
    } else {
        -1
    }
}

/// Compares at most `n` characters, ignoring ASCII case: 0 when equal, else
/// 1 or -1 by the first different characters folded to **upper** case, as
/// signed chars (bytes 0x80 and up sort below `\0`). A negative `n` never
/// runs out. (Call of Duty 2 calls it `I_strnicmp`.)
///
/// Folding to upper case sorts `[ \ ] ^ _ `` ` `` after the letters, where
/// Quake III and Call of Duty 3 (which fold to lower case) sort them before.
///
/// Original: `Q_stricmpn` at 0x0044A670 (CoD2MP_s.exe 1.3)
/// Original: `Q_stricmpn` at 0x0044F830 (CoDUOMP.exe 1.51)
pub fn stricmpn(s1: &[u8], s2: &[u8], n: i32) -> i32 {
    let mut remaining = n;
    let mut i = 0;
    loop {
        let mut c1 = cstr::signed_at(s1, i);
        let mut c2 = cstr::signed_at(s2, i);
        i += 1;
        if remaining == 0 {
            return 0;
        }
        remaining = remaining.wrapping_sub(1);
        if c1 != c2 {
            c1 = crate::crt::toupper(c1);
            c2 = crate::crt::toupper(c2);
            if c1 != c2 {
                return order(c1, c2);
            }
        }
        if c1 == 0 {
            return 0;
        }
    }
}

/// Compares at most `n` characters exactly: 0, or 1 / -1 by the first
/// different characters as signed chars. (Call of Duty 2: `I_strncmp`.)
///
/// Original: `Q_strncmp` at 0x0044A6D0 (CoD2MP_s.exe 1.3)
/// Original: `Q_strncmp` at 0x0044F890 (CoDUOMP.exe 1.51)
pub fn strncmp(s1: &[u8], s2: &[u8], n: i32) -> i32 {
    let mut remaining = n;
    let mut i = 0;
    loop {
        let c1 = cstr::signed_at(s1, i);
        let c2 = cstr::signed_at(s2, i);
        i += 1;
        if remaining == 0 {
            return 0;
        }
        remaining = remaining.wrapping_sub(1);
        if c1 != c2 {
            return order(c1, c2);
        }
        if c1 == 0 {
            return 0;
        }
    }
}

/// Call of Duty 2's `Q_stricmp` (`I_stricmp`): [`stricmpn`] with no limit
/// that matters (`INT_MAX`). Most callers have it inlined; this copy is the
/// one passed as a function pointer (to `Com_StringContains`).
///
/// Original: `Q_stricmp` at 0x0044A710 (CoD2MP_s.exe 1.3)
pub fn stricmp(s1: &[u8], s2: &[u8]) -> i32 {
    stricmpn(s1, s2, STRICMP_LIMIT_COD2)
}

/// Call of Duty 2's `Q_strcmp` (`I_strcmp`): [`strncmp`] with no limit
/// that matters (`INT_MAX`).
///
/// Original: `Q_strcmp` at 0x0044A730 (CoD2MP_s.exe 1.3)
pub fn strcmp(s1: &[u8], s2: &[u8]) -> i32 {
    strncmp(s1, s2, STRICMP_LIMIT_COD2)
}

/// United Offensive's `Q_stricmp`: -1 when either string is missing (a
/// NULL pointer), else [`stricmpn`] limited to 99999 characters.
///
/// Original: `Q_stricmp` at 0x0044F8D0 (CoDUOMP.exe 1.51)
pub fn stricmp_nullable(s1: Option<&[u8]>, s2: Option<&[u8]>) -> i32 {
    match (s1, s2) {
        (Some(s1), Some(s2)) => stricmpn(s1, s2, STRICMP_LIMIT_UO),
        _ => -1,
    }
}

/// An unused second `Q_stricmp` in United Offensive (no caller, no
/// pointer to it): 0 or -1 only, case folded to upper, 99999 characters at
/// most. It is exactly Call of Duty 3's `Q_stricmp`, which descends from
/// it: this is `cod3_game`'s port, checked against both games.
///
/// Original: `sub_0044FAA0` at 0x0044FAA0 (CoDUOMP.exe 1.51)
pub use cod3_game::shared::strings::stricmp as stricmp_equal_or_minus_one;

/// Copies `src` into `dest` (the game's array of `dest.len()` bytes): at
/// most `dest.len() - 1` characters, the rest of the array zero-filled like
/// `strncpy`, and the last byte always 0.
///
/// Unlike Quake III's, there are no checks: a NULL pointer or a size below
/// 1 crashes the original. `dest` must hold at least one byte.
///
/// Original: `Q_strncpyz` at 0x0044A650 (CoD2MP_s.exe 1.3)
/// Original: `Q_strncpyz` at 0x0044F810 (CoDUOMP.exe 1.51)
pub fn strncpyz(dest: &mut [u8], src: &[u8]) {
    let last = dest.len() - 1;
    crate::crt::strncpy(&mut dest[..last], src);
    dest[last] = 0;
}

/// Lower-cases `A`-`Z` in place (nothing else changes). United Offensive
/// calls the C runtime's `tolower` for each byte, Call of Duty 2 compares
/// inline; the result is the same, and the same as Call of Duty 3's: this
/// is `cod3_game`'s port, checked against both PC games.
///
/// Original: `Q_strlwr` at 0x0044A7F0 (CoD2MP_s.exe 1.3)
/// Original: `Q_strlwr` at 0x0044F8F0 (CoDUOMP.exe 1.51)
pub use cod3_game::shared::strings::strlwr;

/// Upper-cases `a`-`z` in place (nothing else changes).
///
/// Original: `Q_strupr` at 0x0044A820 (CoD2MP_s.exe 1.3)
/// Original: `Q_strupr` at 0x0044F920 (CoDUOMP.exe 1.51)
pub fn strupr(buffer: &mut [u8]) {
    let n = cstr::len(buffer);
    buffer[..n].make_ascii_uppercase();
}

/// Appends `src` to the string in `dest` (an array of `dest.len()` bytes),
/// zero-filling the rest like `strncpy`; the last byte is always 0. A
/// string already as long as the array is a fatal error. Call of Duty 2's
/// message says `I_strncat` where United Offensive's and Call of Duty 3's
/// say `Q_strcat`; otherwise the three are the same: this is `cod3_game`'s
/// port, checked against both PC games.
///
/// Original: `Q_strcat` at 0x0044A850 (CoD2MP_s.exe 1.3)
/// Original: `Q_strcat` at 0x0044F950 (CoDUOMP.exe 1.51)
pub use cod3_game::shared::strings::strcat;

/// A color code: `^` and a digit (`^0`-`^9`). Quake III accepted any
/// character but `^` and the terminator after the `^`.
#[inline]
fn is_color_code(s: &[u8], i: usize) -> bool {
    cstr::at(s, i) == b'^' && cstr::at(s, i + 1).is_ascii_digit()
}

/// The length of a string as printed: color codes don't count.
/// (Call of Duty 2 calls it `I_DrawStrlen`.)
///
/// Original: `Q_PrintStrlen` at 0x0044A8A0 (CoD2MP_s.exe 1.3)
/// Original: `Q_PrintStrlen` at 0x0044F9A0 (CoDUOMP.exe 1.51)
pub fn print_strlen(s: &[u8]) -> i32 {
    let mut count = 0;
    let mut i = 0;
    while cstr::at(s, i) != 0 {
        if is_color_code(s, i) {
            i += 2;
        } else {
            count += 1;
            i += 1;
        }
    }
    count
}

/// Removes color codes and every byte outside printable ASCII (0x20-0x7E)
/// from the string in `buffer`, in place.
///
/// Original: `Q_CleanStr` at 0x0044A8D0 (CoD2MP_s.exe 1.3)
/// Original: `Q_CleanStr` at 0x0044F9D0 (CoDUOMP.exe 1.51)
pub fn clean_str(buffer: &mut [u8]) {
    let n = cstr::len(buffer);
    let mut kept = 0;
    let mut i = 0;
    while i < n {
        let c = buffer[i];
        if is_color_code(buffer, i) {
            i += 1; // the digit goes with the step below
        } else if (0x20..=0x7E).contains(&c) {
            buffer[kept] = c;
            kept += 1;
        }
        i += 1;
    }
    buffer[kept] = 0;
}

/// The file name after the last `/` (a `\` doesn't count).
///
/// Original: `COM_SkipPath` at 0x00449F60 (CoD2MP_s.exe 1.3)
/// Original: `COM_SkipPath` at 0x0044F310 (CoDUOMP.exe 1.51)
pub fn skip_path(path: &[u8]) -> &[u8] {
    let path = cstr::text(path);
    match path.iter().rposition(|&c| c == b'/') {
        Some(slash) => &path[slash + 1..],
        None => path,
    }
}

/// Where the extension of the last path component starts: its last `.`,
/// or the end of the name when there is none (`/` and `\` end a component).
fn extension_start(name: &[u8]) -> usize {
    let mut dot = None;
    for (i, &c) in name.iter().enumerate() {
        match c {
            b'.' => dot = Some(i),
            b'/' | b'\\' => dot = None,
            _ => {}
        }
    }
    dot.unwrap_or(name.len())
}

/// The extension of a file name **with its dot** (`.iwd`), or an empty
/// string. Call of Duty 2's own name; Call of Duty 3's `COM_GetExtension`
/// returns the text after the dot instead.
///
/// Original: `Com_GetExtensionSubString` at 0x0044A040 (CoD2MP_s.exe 1.3)
pub fn extension_with_dot(name: &[u8]) -> &[u8] {
    let name = cstr::text(name);
    &name[extension_start(name)..]
}

/// Call of Duty 2's `COM_StripExtension`: the name without the extension
/// of its last component (a `.` in a folder name stays).
///
/// The original copies this part into its `out` argument, then a
/// terminator (it may be the same array: the copy moves forward); the port
/// returns it.
///
/// Original: `COM_StripExtension` at 0x0044A070 (CoD2MP_s.exe 1.3)
pub fn strip_extension(name: &[u8]) -> &[u8] {
    let name = cstr::text(name);
    &name[..extension_start(name)]
}

/// United Offensive's `COM_StripExtension` (Quake III 1.32's): the name up
/// to its **first** `.`, wherever it is (`maps/mp.v2/x.bsp` gives `maps/mp`).
/// Returned like [`strip_extension`].
///
/// Original: `COM_StripExtension` at 0x0044F330 (CoDUOMP.exe 1.51)
pub fn strip_extension_at_first_dot(name: &[u8]) -> &[u8] {
    let name = cstr::text(name);
    match name.iter().position(|&c| c == b'.') {
        Some(dot) => &name[..dot],
        None => name,
    }
}

/// The text `COM_DefaultExtension` keeps of the old name: its first 63 characters.
pub const DEFAULT_EXTENSION_KEEP: usize = 63;

/// Adds `extension` to the name in `path` (the caller's array; its first
/// `max_size` bytes are the original's `maxSize`) if the name has none.
///
/// It looks back from the last character to the first `/` (not `\`); a
/// `.` on the way means the name has an extension, except at the very
/// first character. Otherwise the name is cut to 63 characters, the
/// extension added, and the result written like `Com_sprintf(path,
/// maxSize, "%s%s")` ([`crate::crt::sprintf`]). Call of Duty 3's version
/// is the same steps specialized for `.cfg` in a 128-byte array, keeping
/// 127 characters.
///
/// An empty name makes the original read before the array; here it just
/// gets the extension. `max_size` is at least 1 and at most `path.len()`.
///
/// Original: `COM_DefaultExtension` at 0x0044A100 (CoD2MP_s.exe 1.3)
/// Original: `COM_DefaultExtension` at 0x0044F3A0 (CoDUOMP.exe 1.51)
pub fn default_extension(path: &mut [u8], max_size: usize, extension: &[u8]) {
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
    let mut formatted = Vec::with_capacity(DEFAULT_EXTENSION_KEEP + extension.len());
    formatted.extend_from_slice(&path[..n.min(DEFAULT_EXTENSION_KEEP)]);
    formatted.extend_from_slice(cstr::text(extension));
    crate::crt::sprintf(path, max_size, &formatted);
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn compares_fold_to_upper_and_return_a_sign() {
        assert_eq!(stricmpn(b"abc", b"ABD", 99), -1);
        assert_eq!(stricmpn(b"_", b"a", 5), 1); // '_' (0x5F) is above 'A' (0x41)
        assert_eq!(stricmpn(b"abc", b"abd", 2), 0);
        assert_eq!(stricmpn(b"\x80", b"", 5), -1);
        assert_eq!(strncmp(b"a", b"A", 1), 1);
        assert_eq!(stricmp_nullable(None, Some(b"x")), -1);
        assert_eq!(stricmp(b"Hello", b"hELLO"), 0);
    }

    #[test]
    fn copies_and_cleans() {
        let mut d = [0x55u8; 6];
        strncpyz(&mut d, b"abcdefgh");
        assert_eq!(&d, b"abcde\0");
        let mut s = *b"^1Hi^^2 \x80there^\0";
        clean_str(&mut s);
        // "^^2": the first '^' is followed by '^', so it stays; "^2" is a color code.
        assert_eq!(cstr::text(&s), b"Hi^ there^");
        assert_eq!(print_strlen(b"^1Hi^7!"), 3);
    }

    #[test]
    fn file_names() {
        assert_eq!(skip_path(b"a/b\\c/d.e"), b"d.e");
        assert_eq!(extension_with_dot(b"x.tar.gz"), b".gz");
        assert_eq!(extension_with_dot(b"maps.v2/x"), b"");
        assert_eq!(strip_extension(b"maps.v2/x.bsp"), b"maps.v2/x");
        assert_eq!(strip_extension_at_first_dot(b"maps.v2/x.bsp"), b"maps");
        let mut path = [0u8; 16];
        path[..4].copy_from_slice(b"auto");
        default_extension(&mut path, 16, b".cfg");
        assert_eq!(cstr::text(&path), b"auto.cfg");
        default_extension(&mut path, 16, b".txt");
        assert_eq!(cstr::text(&path), b"auto.cfg");
    }
}
