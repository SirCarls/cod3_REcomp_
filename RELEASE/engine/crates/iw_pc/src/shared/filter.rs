//! Wildcard filters for names and paths (`*`, `?`, `[a-z]`), used to match
//! file names and ban lists.
//!
//! United Offensive's `Com_Filter` is Quake III's: a `*` followed by text
//! looks for that text anywhere in the rest of the name. Call of Duty 2's
//! looks with a whole-string compare (`Q_strcmp` / `Q_stricmp`) at each
//! place, so the text only matches at the very end of the name: `*mp_*`
//! matches names *ending* in `mp_`, and `*.iwd` still works as expected.
//!
//! Quirks both keep: `[[` doesn't escape a bracket (the second `[` starts a
//! class); `]]` inside a class is a `]`; a `?` or a class can move past the
//! end of the name, and a class the filter doesn't close can move past the
//! end of the filter, where the originals read whatever memory follows (the
//! ports read zeros there, like the end of the string).

use crate::crt::toupper;
use crate::cstr;
use crate::shared::strings::{strcmp, stricmp};

/// How a `*` finds the text after it in the rest of the name.
#[derive(Clone, Copy)]
enum StarSearch {
    /// Anywhere, the first place it fits (United Offensive, Quake III).
    Anywhere,
    /// Only where the rest of the name equals the text (Call of Duty 2).
    WholeRest,
}

/// Two characters as the filter compares them: exactly, or after the C
/// runtime's `toupper` on the signed chars.
fn same(a: u8, b: u8, case_sensitive: bool) -> bool {
    if case_sensitive {
        a == b
    } else {
        toupper(a as i8 as i32) == toupper(b as i8 as i32)
    }
}

/// Whether `c` lies between `low` and `high` (signed chars, case folded
/// to upper unless `case_sensitive`).
fn in_range(c: u8, low: u8, high: u8, case_sensitive: bool) -> bool {
    let key = |x: u8| {
        if case_sensitive {
            x as i8 as i32
        } else {
            toupper(x as i8 as i32)
        }
    };
    key(c) >= key(low) && key(c) <= key(high)
}

/// Where `text` starts in `rest` (`Com_StringContains`), the way each game looks.
fn find(rest: &[u8], text: &[u8], case_sensitive: bool, search: StarSearch) -> Option<usize> {
    let last_start = rest.len().checked_sub(text.len())?;
    match search {
        StarSearch::Anywhere => (0..=last_start).find(|&i| {
            rest[i..i + text.len()]
                .iter()
                .zip(text)
                .all(|(&a, &b)| same(a, b, case_sensitive))
        }),
        StarSearch::WholeRest => {
            // The original tries every start with a whole-string compare;
            // only the start where both have the same length can be equal.
            let rest_from_there = &rest[last_start..];
            let equal = if case_sensitive {
                strcmp(rest_from_there, text) == 0
            } else {
                stricmp(rest_from_there, text) == 0
            };
            equal.then_some(last_start)
        }
    }
}

fn filter_with(filter: &[u8], name: &[u8], case_sensitive: bool, search: StarSearch) -> bool {
    let filter = cstr::text(filter);
    let name = cstr::text(name);
    let mut f = 0; // in the filter
    let mut n = 0; // in the name (may run past its end, where it reads zeros)
    loop {
        let c = cstr::at(filter, f);
        match c {
            0 => return true,
            b'*' => {
                f += 1;
                let start = f;
                while !matches!(cstr::at(filter, f), 0 | b'*' | b'?') {
                    f += 1;
                }
                let text = &filter[start..f];
                if text.is_empty() {
                    continue;
                }
                let rest = name.get(n..).unwrap_or(&[]);
                match find(rest, text, case_sensitive, search) {
                    Some(at) => n += at + text.len(),
                    None => return false,
                }
            }
            b'?' => {
                f += 1;
                n += 1;
            }
            b'[' => {
                f += 1;
                if cstr::at(filter, f) == b'[' {
                    continue; // the second '[' is looked at again: it starts a class
                }
                if !class_matches(filter, &mut f, cstr::at(name, n), case_sensitive) {
                    return false;
                }
                // Skip to the class's closing ']' (a "]]" doesn't close it), and past it.
                loop {
                    let c = cstr::at(filter, f);
                    if c == 0 || (c == b']' && cstr::at(filter, f + 1) != b']') {
                        break;
                    }
                    f += 1;
                }
                f += 1;
                n += 1;
            }
            _ => {
                if !same(c, cstr::at(name, n), case_sensitive) {
                    return false;
                }
                f += 1;
                n += 1;
            }
        }
    }
}

/// Walks a `[...]` class from `*f` (just after the `[`) until `c` matches
/// one of its characters or ranges, leaving `*f` after the matching part.
/// False when the class ends (or the filter does) without a match.
fn class_matches(filter: &[u8], f: &mut usize, c: u8, case_sensitive: bool) -> bool {
    loop {
        let first = cstr::at(filter, *f);
        if first == 0 || (first == b']' && cstr::at(filter, *f + 1) != b']') {
            return false;
        }
        let high = cstr::at(filter, *f + 2);
        let found = if cstr::at(filter, *f + 1) == b'-'
            && high != 0
            && (high != b']' || cstr::at(filter, *f + 3) == b']')
        {
            *f += 3;
            in_range(c, first, high, case_sensitive)
        } else {
            *f += 1;
            same(first, c, case_sensitive)
        };
        if found {
            return true;
        }
    }
}

/// United Offensive's `Com_Filter` (Quake III's): whether `name` matches
/// `filter`.
///
/// Original: `Com_Filter` at 0x004351B0 (CoDUOMP.exe 1.51)
pub fn filter(filter: &[u8], name: &[u8], case_sensitive: bool) -> bool {
    filter_with(filter, name, case_sensitive, StarSearch::Anywhere)
}

/// Call of Duty 2's `Com_Filter`: like [`filter`], but the text after a
/// `*` must be the whole rest of the name (see the module's notes).
///
/// Original: `Com_Filter` at 0x0042D660 (CoD2MP_s.exe 1.3)
pub fn filter_cod2(filter: &[u8], name: &[u8], case_sensitive: bool) -> bool {
    filter_with(filter, name, case_sensitive, StarSearch::WholeRest)
}

/// The most of a filter or name `Com_FilterPath` looks at.
pub const FILTER_PATH_KEEP: usize = 63;

/// A path as `Com_FilterPath` compares it: its first 63 characters, with
/// `\` and `:` as `/`.
fn path_for_filter(path: &[u8]) -> Vec<u8> {
    cstr::text(path)
        .iter()
        .take(FILTER_PATH_KEEP)
        .map(|&c| if c == b'\\' || c == b':' { b'/' } else { c })
        .collect()
}

/// [`filter`] on paths: both cut to 63 characters, `\` and `:` read as `/`.
///
/// Original: `Com_FilterPath` at 0x004353D0 (CoDUOMP.exe 1.51)
pub fn filter_path(filter_text: &[u8], name: &[u8], case_sensitive: bool) -> bool {
    filter(
        &path_for_filter(filter_text),
        &path_for_filter(name),
        case_sensitive,
    )
}

/// [`filter_cod2`] on paths, cut and converted like [`filter_path`].
///
/// Original: `Com_FilterPath` at 0x0042D860 (CoD2MP_s.exe 1.3)
pub fn filter_path_cod2(filter_text: &[u8], name: &[u8], case_sensitive: bool) -> bool {
    filter_cod2(
        &path_for_filter(filter_text),
        &path_for_filter(name),
        case_sensitive,
    )
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn wildcards() {
        assert!(filter(b"*mp_*", b"maps/mp_harbor.bsp", false));
        assert!(!filter_cod2(b"*mp_*", b"maps/mp_harbor.bsp", false));
        assert!(filter_cod2(b"*.IWD", b"main/iw_00.iwd", false));
        assert!(!filter_cod2(b"*.IWD", b"main/iw_00.iwd", true));
        assert!(filter(b"iw_0[0-4]?", b"iw_03x", true));
        assert!(!filter(b"iw_0[0-4]", b"iw_07", true));
        assert!(filter_path(b"main\\*", b"main/x", true));
    }
}
