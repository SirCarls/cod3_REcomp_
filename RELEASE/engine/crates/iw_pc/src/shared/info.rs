//! Info strings: `\key\value\key\value` text, the games' dictionaries
//! (server info, user info, system info, config strings), sent over the
//! network as they are, so a port has to produce exactly the same bytes.
//! Keys can't contain `\`, `;` or `"`; values silently lose them.
//!
//! The two PC games share the code; their limits, and Call of Duty 3's:
//!
//! | | CoD2 1.3 | UO 1.51 | CoD3 (360) |
//! |---|---|---|---|
//! | `Info_ValueForKey` accepts | under 8192 | under 8192 | under 1024 |
//! | `Info_RemoveKey` / `_Big` accept | 1024 / 8192 | 1024 / 8192 | 1024 / 1024 |
//! | `Info_SetValueForKey`: refusals | printed, string unchanged | printed | `Com_Error` |
//! | `Info_SetValueForKey_Big`: longest result | **1024** (the bug below) | 8192 | 1024 |
//!
//! Looking a key up ignores case, removing one doesn't (all three games).
//!
//! # The engine layer
//!
//! The remake's info strings without these limits are `cod3_game`'s
//! [`InfoString`](cod3_game::engine::info::InfoString): within the PC
//! games' limits it gives the same text as the functions here (this
//! module's tests compare them on random strings), and past them it keeps
//! working. What a Rust client or server sends to an original PC copy must
//! still fit these limits.

use std::fmt;
use std::ops::Range;

use crate::crt;
use crate::cstr;
use crate::shared::strings::{stricmpn, STRICMP_LIMIT_UO};
use crate::{Game, GameError};

/// `MAX_INFO_STRING`: what `Info_RemoveKey` and `Info_SetValueForKey` accept, terminator included.
pub const MAX_INFO_STRING: usize = 1024;
/// `BIG_INFO_STRING`: what `Info_ValueForKey` and the `_Big` functions accept.
pub const BIG_INFO_STRING: usize = 8192;

/// One `\key\value` pair found while walking an info string.
struct Pair {
    /// Where the pair starts, its leading `\` included.
    start: usize,
    key: Range<usize>,
    value: Range<usize>,
}

/// The pair from `i` on, walked like the originals do (one leading `\`
/// skipped). A key with no `\` after it ends the walk.
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

/// The value of `key`, ignoring case (the games' `Q_stricmpn`, which folds
/// to upper case: for equality that is the same as ignoring ASCII case),
/// or an empty string when it's missing. A NULL `info` or `key` gives an
/// empty string in the original.
///
/// The original copies the value into one of two static 8192-byte buffers
/// it takes turns with; the port returns the value's place in `info`.
///
/// Original: `Info_ValueForKey` at 0x0044AA90 (CoD2MP_s.exe 1.3)
/// Original: `Info_ValueForKey` at 0x0044FB70 (CoDUOMP.exe 1.51)
pub fn value_for_key<'a>(info: &'a [u8], key: &[u8]) -> Result<&'a [u8], GameError> {
    let info = cstr::text(info);
    if info.len() >= BIG_INFO_STRING {
        return Err(GameError::drop("Info_ValueForKey: oversize infostring"));
    }
    let mut i = 0;
    while let Some(pair) = next_pair(info, i) {
        // Call of Duty 2 passes INT_MAX, United Offensive 99999: keys this
        // short never reach either limit.
        if stricmpn(key, &info[pair.key.clone()], STRICMP_LIMIT_UO) == 0 {
            return Ok(&info[pair.value]);
        }
        if cstr::at(info, pair.value.end) == 0 {
            break;
        }
        i = pair.value.end;
    }
    Ok(b"")
}

/// Removes the first pair whose key is exactly `key`: the rest of the
/// string moves down over it (`strcpy`). A key containing `\` is ignored.
fn remove_pair(info: &mut [u8], key: &[u8]) {
    let key = cstr::text(key);
    if key.contains(&b'\\') {
        return;
    }
    let mut i = 0;
    while let Some(pair) = next_pair(info, i) {
        if &info[pair.key.clone()] == key {
            let rest = pair.value.end;
            let rest_len = cstr::len(&info[rest..]);
            info.copy_within(rest..rest + rest_len, pair.start);
            info[pair.start + rest_len] = 0;
            return;
        }
        if cstr::at(info, pair.value.end) == 0 {
            return;
        }
        i = pair.value.end;
    }
}

/// Removes `key` (exact case) from the info string in `info`; a key
/// containing `\` is ignored, a string of 1024 bytes or more is the
/// game's error. The same in Call of Duty 3: this is `cod3_game`'s port,
/// checked against both PC games.
///
/// Original: `Info_RemoveKey` at 0x0044ABE0 (CoD2MP_s.exe 1.3)
/// Original: `Info_RemoveKey` at 0x0044FCE0 (CoDUOMP.exe 1.51)
pub use cod3_game::shared::info::remove_key;

/// [`remove_key`] for strings up to 8192 bytes (Call of Duty 3's `_Big`
/// stops at 1024 like the normal one).
///
/// Original: `Info_RemoveKey_Big` at 0x0044ACE0 (CoD2MP_s.exe 1.3)
/// Original: `Info_RemoveKey_Big` at 0x0044FE00 (CoDUOMP.exe 1.51)
pub fn remove_key_big(info: &mut [u8], key: &[u8]) -> Result<(), GameError> {
    if cstr::len(info) >= BIG_INFO_STRING {
        return Err(GameError::drop("Info_RemoveKey_Big: oversize infostring"));
    }
    remove_pair(info, key);
    Ok(())
}

/// Whether a string can go into an info string as a value: no `"` and no `;`.
///
/// Original: `Info_Validate` at 0x0044ADE0 (CoD2MP_s.exe 1.3)
/// Original: `Info_Validate` at 0x0044FF20 (CoDUOMP.exe 1.51)
pub fn validate(s: &[u8]) -> bool {
    let s = cstr::text(s);
    !s.contains(&b'"') && !s.contains(&b';')
}

/// Why `Info_SetValueForKey` left the string as it was. The originals
/// print a message (`Com_Printf`, not an error) and return.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Refusal {
    /// The info string was already too long.
    Oversize,
    KeyWithBackslash,
    KeyWithSemicolon,
    KeyWithQuote,
    /// `\key\value` didn't fit the formatting buffer.
    PairTooLong,
    /// The info string with the new pair would be too long (after the old
    /// pair was already removed).
    StringTooLong,
    /// [`StringTooLong`](Self::StringTooLong) in United Offensive's `_Big`.
    BigStringTooLong,
    /// The caller's array is smaller than the game's (the original would
    /// write past it). Not a refusal of the original's.
    NoRoom,
}

impl Refusal {
    /// The format of the message the original prints, without its leading
    /// 0x15 (the games' "localized text" marker).
    pub fn message_format(self, game: Game) -> &'static str {
        match (self, game) {
            (Refusal::Oversize, _) => "Info_SetValueForKey: oversize infostring",
            (Refusal::KeyWithBackslash, _) => "Can't use keys with a \\\nkey: '%s'\nvalue: '%s'",
            (Refusal::KeyWithSemicolon, _) => {
                "Can't use keys with a semicolon\nkey: '%s'\nvalue: '%s'"
            }
            (Refusal::KeyWithQuote, _) => "Can't use keys with a \"\nkey: '%s'\nvalue: '%s'",
            (Refusal::PairTooLong, Game::CallOfDuty2) => {
                "Info buffer length exceeded, not including key/value pair in response\n"
            }
            (Refusal::PairTooLong, Game::UnitedOffensive) => {
                "Server info buffer length exceeded, not including key/value pair in response\n"
            }
            (Refusal::StringTooLong, _) => {
                "Info string length exceeded\nkey: '%s'\nvalue: '%s'\nInfo string:\n%s\n"
            }
            (Refusal::BigStringTooLong, _) => {
                "BIG Info string length exceeded\nkey: '%s'\nvalue: '%s'\nInfo string:\n%s\n"
            }
            (Refusal::NoRoom, _) => "",
        }
    }
}

impl fmt::Display for Refusal {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        let text = self.message_format(Game::CallOfDuty2);
        write!(f, "{}", text.lines().next().unwrap_or("no room"))
    }
}

impl std::error::Error for Refusal {}

/// The sizes one variant of `Info_SetValueForKey` works with.
struct SetLimits {
    /// The string must be shorter than this to start with.
    accepted: usize,
    /// How much of the value is looked at (the rest is dropped).
    value_scan: usize,
    /// The buffer `\key\value` is formatted into (`Com_Sprintf`'s size).
    pair_buffer: usize,
    /// The longest the string may become.
    longest: usize,
    too_long: Refusal,
}

const SET_LIMITS: SetLimits = SetLimits {
    accepted: MAX_INFO_STRING,
    value_scan: MAX_INFO_STRING - 1,
    pair_buffer: MAX_INFO_STRING,
    longest: MAX_INFO_STRING,
    too_long: Refusal::StringTooLong,
};

fn set_value(info: &mut [u8], key: &[u8], value: &[u8], limits: &SetLimits) -> Result<(), Refusal> {
    let key = cstr::text(key);
    let value = cstr::text(value);
    if cstr::len(info) >= limits.accepted {
        return Err(Refusal::Oversize);
    }
    let clean: Vec<u8> = value
        .iter()
        .take(limits.value_scan)
        .filter(|&&c| !matches!(c, b'\\' | b';' | b'"'))
        .copied()
        .collect();
    for (c, refusal) in [
        (b'\\', Refusal::KeyWithBackslash),
        (b';', Refusal::KeyWithSemicolon),
        (b'"', Refusal::KeyWithQuote),
    ] {
        if key.contains(&c) {
            return Err(refusal);
        }
    }
    remove_pair(info, key);
    if clean.is_empty() {
        return Ok(());
    }
    let mut text = Vec::with_capacity(key.len() + clean.len() + 2);
    text.push(b'\\');
    text.extend_from_slice(key);
    text.push(b'\\');
    text.extend_from_slice(&clean);
    let mut formatted = vec![0u8; limits.pair_buffer];
    if crt::sprintf(&mut formatted, limits.pair_buffer, &text) <= 0 {
        return Err(Refusal::PairTooLong);
    }
    let pair = cstr::text(&formatted);
    if pair.len() + cstr::len(info) > limits.longest {
        return Err(limits.too_long);
    }
    if cstr::append(info, pair) {
        Ok(())
    } else {
        Err(Refusal::NoRoom)
    }
}

/// Sets `key` to `value`: removes the old pair (exact case), then appends
/// `\key\value`; an empty value only removes it. The value loses `\`, `;`
/// and `"`, and anything past its first 1023 characters.
///
/// Refusals leave the string unchanged, except the last two
/// ([`Refusal::PairTooLong`], [`Refusal::StringTooLong`]), which come after
/// the old pair is gone. A `\key\value` of exactly 1024 characters is cut
/// to 1023 by the formatting and still goes in if it fits.
///
/// Original: `Info_SetValueForKey` at 0x0044AE10 (CoD2MP_s.exe 1.3)
/// Original: `Info_SetValueForKey` at 0x0044FF50 (CoDUOMP.exe 1.51)
pub fn set_value_for_key(info: &mut [u8], key: &[u8], value: &[u8]) -> Result<(), Refusal> {
    set_value(info, key, value, &SET_LIMITS)
}

/// [`set_value_for_key`] for strings up to 8192 bytes: values keep 8191
/// characters and the pair is formatted in 8192 bytes. But Call of Duty 2's
/// final check still uses 1024 (a bug: its `_Big` strings can't grow past
/// 1024 bytes this way), where United Offensive's uses 8192 with its own
/// message ([`Refusal::BigStringTooLong`]). Both print the normal
/// function's message for an oversize string.
///
/// Original: `Info_SetValueForKey_Big` at 0x0044AFE0 (CoD2MP_s.exe 1.3)
/// Original: `Info_SetValueForKey_Big` at 0x00450180 (CoDUOMP.exe 1.51)
pub fn set_value_for_key_big(
    game: Game,
    info: &mut [u8],
    key: &[u8],
    value: &[u8],
) -> Result<(), Refusal> {
    let limits = match game {
        Game::CallOfDuty2 => SetLimits {
            accepted: BIG_INFO_STRING,
            value_scan: BIG_INFO_STRING - 1,
            pair_buffer: BIG_INFO_STRING,
            longest: MAX_INFO_STRING,
            too_long: Refusal::StringTooLong,
        },
        Game::UnitedOffensive => SetLimits {
            accepted: BIG_INFO_STRING,
            value_scan: BIG_INFO_STRING - 1,
            pair_buffer: BIG_INFO_STRING,
            longest: BIG_INFO_STRING,
            too_long: Refusal::BigStringTooLong,
        },
    };
    set_value(info, key, value, &limits)
}

#[cfg(test)]
mod tests {
    use super::*;
    use cod3_game::engine::info::InfoString;

    fn buffer(text: &[u8], size: usize) -> Vec<u8> {
        let mut b = vec![0u8; size];
        b[..text.len()].copy_from_slice(text);
        b
    }

    #[test]
    fn looks_up_sets_and_removes() {
        let info = b"\\name\\Price\\Team\\allies";
        assert_eq!(value_for_key(info, b"team").unwrap(), b"allies");
        assert_eq!(value_for_key(info, b"rank").unwrap(), b"");
        let mut b = buffer(b"\\a\\1\\b\\2", 1025);
        set_value_for_key(&mut b, b"a", b"x;y").unwrap();
        assert_eq!(cstr::text(&b), b"\\b\\2\\a\\xy");
        assert_eq!(
            set_value_for_key(&mut b, b"bad;key", b"1"),
            Err(Refusal::KeyWithSemicolon)
        );
        remove_key_big(&mut b, b"b").unwrap();
        assert_eq!(cstr::text(&b), b"\\a\\xy");
    }

    #[test]
    fn cod2_big_strings_stop_at_1024() {
        let long = vec![b'v'; 1100];
        let mut b = buffer(b"", BIG_INFO_STRING + 1);
        assert_eq!(
            set_value_for_key_big(Game::CallOfDuty2, &mut b, b"k", &long),
            Err(Refusal::StringTooLong)
        );
        set_value_for_key_big(Game::UnitedOffensive, &mut b, b"k", &long).unwrap();
        assert_eq!(cstr::len(&b), 1100 + 3);
    }

    /// A small deterministic generator (xorshift32) for the comparisons below.
    struct Random(u32);

    impl Random {
        fn below(&mut self, n: u32) -> u32 {
            self.0 ^= self.0 << 13;
            self.0 ^= self.0 >> 17;
            self.0 ^= self.0 << 5;
            self.0 % n.max(1)
        }

        fn text(&mut self, alphabet: &[u8], max: u32) -> Vec<u8> {
            (0..self.below(max + 1))
                .map(|_| alphabet[self.below(alphabet.len() as u32) as usize])
                .collect()
        }
    }

    /// The engine layer's info string gives the same text as the PC games'
    /// functions on everything those accept.
    #[test]
    fn the_engine_info_string_matches_within_the_limits() {
        let mut r = Random(0x1234_5678);
        let keys = b"abcABC_";
        let values = b"xyzXYZ01 ;\\\"";
        for _ in 0..20_000 {
            let mut original = buffer(b"", MAX_INFO_STRING + 1);
            let mut engine = InfoString::new();
            for _ in 0..r.below(12) {
                let key = r.text(keys, 4);
                let value = r.text(values, 40);
                match r.below(3) {
                    0 => {
                        remove_key(&mut original, &key).unwrap();
                        engine.remove(&key);
                    }
                    _ => {
                        if set_value_for_key(&mut original, &key, &value).is_ok() {
                            engine.set(&key, &value).unwrap();
                        } else {
                            // Refused: the original is unchanged, or lost the old pair
                            // when the new one didn't fit; follow it.
                            engine = InfoString::from_text(cstr::text(&original));
                        }
                    }
                }
                assert_eq!(engine.as_bytes(), cstr::text(&original));
                let probe = r.text(keys, 4);
                assert_eq!(
                    engine.get(&probe),
                    value_for_key(&original, &probe).unwrap()
                );
            }
        }
    }
}
