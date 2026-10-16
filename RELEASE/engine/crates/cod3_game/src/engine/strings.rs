//! Text helpers for the engine, without the console's limits.
//!
//! Same results as the console's functions ([`shared::strings`](crate::shared::strings))
//! wherever those work, but on strings of any length and without fixed
//! buffers: there's nothing to overflow, so there are no overflow errors.

use std::cmp::Ordering;

use crate::bytes;

/// Whether two names are equal ignoring ASCII case (the game's `Q_stricmp`
/// test, which only knows `A`-`Z`), at any length. Eight bytes at a time.
pub fn equal_ignore_case(a: &[u8], b: &[u8]) -> bool {
    a.len() == b.len() && bytes::first_difference_ignoring_case(a, b).is_none()
}

/// Orders two names ignoring ASCII case, like the game's `Q_stricmpn`:
/// letters fold to lower case, and bytes 0x80 and up sort before the rest
/// (the game compares them as signed chars). Any length, eight bytes at a time.
pub fn compare_ignore_case(a: &[u8], b: &[u8]) -> Ordering {
    // Past its end a string has its terminator, 0: a shorter string that is
    // a prefix of the other compares its 0 with the other's next byte.
    let at = |s: &[u8], i: usize| s.get(i).map_or(0, |&c| c.to_ascii_lowercase() as i8);
    let i = bytes::first_difference_ignoring_case(a, b).unwrap_or(a.len().min(b.len()));
    at(a, i).cmp(&at(b, i))
}

/// The extension of a file name: the text after the last `.` of its last
/// path component (`/` or `\`), or an empty string.
pub fn extension(name: &[u8]) -> &[u8] {
    crate::shared::strings::get_extension(name)
}

/// `path` with `extension` (".cfg") added when its last part has none.
///
/// The console's version (for `exec`) works in a 128-byte buffer and cuts
/// longer names; this one doesn't. Like the console, it looks back to the
/// last `/` only, and a `.` at the very start doesn't count.
pub fn with_default_extension(path: &[u8], extension: &[u8]) -> Vec<u8> {
    let has_extension = path.len() > 1
        && path[1..]
            .iter()
            .rev()
            .take_while(|&&c| c != b'/')
            .any(|&c| c == b'.')
        && path.last() != Some(&b'/');
    let mut out = path.to_vec();
    if !has_extension {
        out.extend_from_slice(extension);
    }
    out
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::cstr;
    use crate::shared::strings as console;
    use crate::test_random::Random;

    fn text(r: &mut Random, max: usize, alphabet: &[u8]) -> Vec<u8> {
        (0..r.below(max as u32 + 1))
            .map(|_| alphabet[r.below(alphabet.len() as u32) as usize])
            .collect()
    }

    #[test]
    fn same_as_the_console_within_its_limits() {
        let mut r = Random::new(7);
        let letters: Vec<u8> = (b'A'..=b'Z')
            .chain(b'a'..=b'z')
            .chain([b'.', b'/', 0x80, 0xE9])
            .collect();
        for _ in 0..20_000 {
            let a = text(&mut r, 12, &letters);
            let b = if r.below(2) == 0 {
                a.to_ascii_uppercase()
            } else {
                text(&mut r, 12, &letters)
            };
            assert_eq!(
                equal_ignore_case(&a, &b),
                console::stricmp(&a, &b) == 0,
                "{a:?} {b:?}"
            );
            assert_eq!(
                compare_ignore_case(&a, &b),
                console::stricmpn(&a, &b, -1).cmp(&0),
                "{a:?} {b:?}"
            );
            let (long_a, long_b) = (
                [&a[..], &[b'x'; 21], &a].concat(),
                [&b[..], &[b'X'; 21], &b].concat(),
            );
            assert_eq!(
                compare_ignore_case(&long_a, &long_b),
                console::stricmpn(&long_a, &long_b, -1).cmp(&0),
                "{long_a:?} {long_b:?}"
            );
            assert_eq!(
                equal_ignore_case(&long_a, &long_b),
                console::stricmp(&long_a, &long_b) == 0
            );
            if !a.is_empty() && a.len() < 120 {
                let mut buffer = [0u8; 160];
                buffer[..a.len()].copy_from_slice(&a);
                console::default_extension_cfg(&mut buffer);
                assert_eq!(
                    with_default_extension(&a, b".cfg"),
                    cstr::text(&buffer),
                    "{a:?}"
                );
            }
        }
    }

    #[test]
    fn no_length_limit() {
        let long = vec![b'p'; 500];
        assert_eq!(with_default_extension(&long, b".cfg").len(), 504);
        assert!(equal_ignore_case(
            &vec![b'a'; 200_000],
            &vec![b'A'; 200_000]
        ));
    }
}
