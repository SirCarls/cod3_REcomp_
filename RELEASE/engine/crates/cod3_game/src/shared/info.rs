//! Info strings: `\key\value\key\value` text, the game's dictionaries.
//!
//! Server info, user info, objectives (`state`, `str`, `org` ...) and other
//! configuration strings are kept this way and sent over the network as is,
//! so a port has to produce exactly the same bytes. Keys can't contain `\`,
//! `;` or `"`; values silently lose those characters.
//!
//! CoD3 differs from Quake III in a few places, kept here:
//! * the unchecked [`set_value_for_key`] has no length check at all: it
//!   appends whatever fits in memory (Quake III printed "Info string length
//!   exceeded" and stopped);
//! * the `_Big` functions check against 1024 bytes, not Quake III's 8192;
//! * looking a key up ignores case ([`stricmpn`](super::strings::stricmpn)),
//!   removing one doesn't.

use crate::cstr;
use crate::error::GameError;
use crate::shared::strings::{self, sprintf_into};

/// `MAX_INFO_STRING`: the longest info string the functions accept, plus its terminator.
pub const MAX_INFO_STRING: usize = 1024;

/// What `Info_SetValueForKey` keeps of a value: its first 1023 characters, minus `\ ; "`.
const MAX_VALUE_SCAN: usize = MAX_INFO_STRING - 1;

/// One `\key\value` pair found while walking an info string.
struct Pair {
    /// Where the pair starts, its leading `\` included.
    start: usize,
    key: std::ops::Range<usize>,
    value: std::ops::Range<usize>,
}

/// Walks the pairs like the game's loops do. Stops at a key without a `\`
/// after it (a trailing `\key`), which the game treats as the end.
fn next_pair(info: &[u8], mut i: usize) -> Option<Pair> {
    let start = i;
    if cstr::at(info, i) == b'\\' {
        i += 1;
    }
    let key_start = i;
    while cstr::at(info, i) != b'\\' {
        if cstr::at(info, i) == 0 {
            return None;
        }
        i += 1;
    }
    let key = key_start..i;
    i += 1;
    let value_start = i;
    while !matches!(cstr::at(info, i), b'\\' | 0) {
        i += 1;
    }
    Some(Pair {
        start,
        key,
        value: value_start..i,
    })
}

/// The value of `key` (case ignored), or an empty string when it's missing.
///
/// The original copies the value into one of two static buffers it takes
/// turns with; the port returns the value's place in `info` instead.
/// (A NULL `info` or `key` gives an empty string in the original.)
///
/// Original: `Info_ValueForKey` at 0x82457570
pub fn value_for_key<'a>(info: &'a [u8], key: &[u8]) -> Result<&'a [u8], GameError> {
    let info = &info[..cstr::len(info)];
    if info.len() >= MAX_INFO_STRING {
        return Err(GameError::drop("Info_ValueForKey: oversize infostring"));
    }
    let mut i = 0;
    while let Some(pair) = next_pair(info, i) {
        if strings::stricmpn(key, &info[pair.key.clone()], strings::STRICMP_LIMIT) == 0 {
            return Ok(&info[pair.value]);
        }
        if cstr::at(info, pair.value.end) == 0 {
            break;
        }
        i = pair.value.end;
    }
    Ok(b"")
}

fn remove_key_checked(info: &mut [u8], key: &[u8], oversize: &str) -> Result<(), GameError> {
    if cstr::len(info) >= MAX_INFO_STRING {
        return Err(GameError::drop(oversize));
    }
    let key = &key[..cstr::len(key)];
    if key.contains(&b'\\') {
        return Ok(());
    }
    let mut i = 0;
    while let Some(pair) = next_pair(info, i) {
        if &info[pair.key.clone()] == key {
            // strcpy(start, rest): the rest of the string moves down over the pair.
            let rest = pair.value.end;
            let rest_len = cstr::len(&info[rest..]);
            info.copy_within(rest..rest + rest_len, pair.start);
            info[pair.start + rest_len] = 0;
            return Ok(());
        }
        if cstr::at(info, pair.value.end) == 0 {
            break;
        }
        i = pair.value.end;
    }
    Ok(())
}

/// Removes `key` (exact case) from the info string in `info`.
/// A key containing `\` is ignored.
///
/// Original: `Info_RemoveKey` at 0x82453338
pub fn remove_key(info: &mut [u8], key: &[u8]) -> Result<(), GameError> {
    remove_key_checked(info, key, "Info_RemoveKey: oversize infostring")
}

/// The same as [`remove_key`] in CoD3 (also limited to 1024 bytes); only its
/// error message differs.
///
/// Original: `Info_RemoveKey_Big` at 0x824531B8
pub fn remove_key_big(info: &mut [u8], key: &[u8]) -> Result<(), GameError> {
    remove_key_checked(info, key, "Info_RemoveKey_Big: oversize infostring")
}

/// The value as stored: the first 1023 characters, without `\`, `;` and `"`.
fn clean_value(value: &[u8]) -> Vec<u8> {
    value
        .iter()
        .take(MAX_VALUE_SCAN)
        .take_while(|&&c| c != 0)
        .filter(|&&c| !matches!(c, b'\\' | b';' | b'"'))
        .copied()
        .collect()
}

fn check_key(key: &[u8], value: &[u8]) -> Result<(), GameError> {
    let shown = |s: &[u8]| String::from_utf8_lossy(s).into_owned();
    for (c, what) in [(b'\\', "a \\"), (b';', "a semicolon"), (b'"', "a \"")] {
        if key.contains(&c) {
            return Err(GameError::drop(format!(
                "Can't use keys with {what}\nkey: '{}'\nvalue: '{}'",
                shown(key),
                shown(value)
            )));
        }
    }
    Ok(())
}

/// `\key\value` as `Com_sprintf` makes it in a 1024-byte buffer.
fn format_pair(key: &[u8], clean: &[u8]) -> Vec<u8> {
    let mut text = Vec::with_capacity(key.len() + clean.len() + 2);
    text.push(b'\\');
    text.extend_from_slice(key);
    text.push(b'\\');
    text.extend_from_slice(clean);
    let mut buffer = vec![0u8; MAX_INFO_STRING];
    sprintf_into(&mut buffer, MAX_INFO_STRING, &text);
    cstr::text(&buffer).to_vec()
}

/// Sets `key` to `value`: removes the old pair and appends `\key\value`
/// (an empty value only removes it).
///
/// No length check: the original appends past the end of an info string
/// that is already full. Here the pair is cut at the end of `info`, and
/// that is reported as an error the original doesn't have.
///
/// Original: `Info_SetValueForKey` at 0x82457408
pub fn set_value_for_key(info: &mut [u8], key: &[u8], value: &[u8]) -> Result<(), GameError> {
    let key = &key[..cstr::len(key)];
    let value = &value[..cstr::len(value)];
    let clean = clean_value(value);
    check_key(key, value)?;
    remove_key(info, key)?;
    if clean.is_empty() {
        return Ok(());
    }
    if !cstr::append(info, &format_pair(key, &clean)) {
        return Err(GameError::fatal(
            "Info_SetValueForKey: the info string ran past its buffer",
        ));
    }
    Ok(())
}

/// [`set_value_for_key`] with CoD3's checks: the info string must be under
/// 1024 bytes before, and at most 1024 (plus the terminator) after.
///
/// Original: `Info_SetValueForKey_Big` at 0x824571F8
pub fn set_value_for_key_big(info: &mut [u8], key: &[u8], value: &[u8]) -> Result<(), GameError> {
    if cstr::len(info) >= MAX_INFO_STRING {
        return Err(GameError::drop("Info_SetValueForKey: oversize infostring"));
    }
    let key = &key[..cstr::len(key)];
    let value = &value[..cstr::len(value)];
    let clean = clean_value(value);
    check_key(key, value)?;
    remove_key_big(info, key)?;
    if clean.is_empty() {
        return Ok(());
    }
    let pair = format_pair(key, &clean);
    if pair.len() + cstr::len(info) > MAX_INFO_STRING {
        return Err(GameError::drop("BIG Info string length exceeded"));
    }
    if !cstr::append(info, &pair) {
        return Err(GameError::fatal(
            "Info_SetValueForKey_Big: the info string ran past its buffer",
        ));
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    fn buffer(text: &[u8]) -> Vec<u8> {
        let mut b = vec![0u8; 2048];
        b[..text.len()].copy_from_slice(text);
        b
    }

    #[test]
    fn looks_up_ignoring_case() {
        let info = b"\\name\\Price\\Team\\allies";
        assert_eq!(value_for_key(info, b"team").unwrap(), b"allies");
        assert_eq!(value_for_key(info, b"rank").unwrap(), b"");
        assert_eq!(value_for_key(b"a\\1", b"A").unwrap(), b"1");
    }

    #[test]
    fn sets_and_removes() {
        let mut info = buffer(b"\\a\\1\\b\\2");
        set_value_for_key(&mut info, b"a", b"x;y").unwrap();
        assert_eq!(cstr::text(&info), b"\\b\\2\\a\\xy");
        remove_key(&mut info, b"b").unwrap();
        assert_eq!(cstr::text(&info), b"\\a\\xy");
        assert!(set_value_for_key(&mut info, b"bad;key", b"1").is_err());
    }
}
