//! Info strings (`\key\value\key\value`) for the engine, without the
//! console's 1024-byte limit.
//!
//! [`InfoString`] produces the same text as the console's functions
//! ([`shared::info`](crate::shared::info)) for every
//! string those accept, so the two stay interchangeable; it just never
//! runs out of room. Values keep all their characters (the console keeps
//! 1023); `\`, `;` and `"` are still dropped from values and refused in keys,
//! since they would break the format.
//!
//! # Speed
//!
//! A lookup walks the text like the game does, but allocates nothing, stops
//! at the first match and finds each `\` eight bytes at a time
//! ([`bytes::find`]). Past the console's 1024 bytes, an index of the keys
//! present (`KeyIndex`) answers "missing" without walking, so adding a new
//! key costs the same however big the string is, and building one key at a
//! time grows linearly instead of with the square of the size.

use std::collections::hash_map::RandomState;
use std::collections::HashMap;
use std::fmt;
use std::hash::{BuildHasher, BuildHasherDefault, Hasher};
use std::ops::Range;

use crate::bytes;
use crate::engine::strings::equal_ignore_case;

/// Between keys and values.
const SEPARATOR: u8 = b'\\';

/// The characters a key can't have and a value loses.
fn is_reserved(c: u8) -> bool {
    matches!(c, b'\\' | b';' | b'"')
}

/// Up to this size the text is just walked; past it a [`KeyIndex`] is kept.
/// (The console's limit: anything the console can hold stays a plain walk.)
const INDEX_FROM: usize = 1024;

/// A key the format can't hold.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct InvalidKey(pub Vec<u8>);

impl fmt::Display for InvalidKey {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(
            f,
            "an info string key can't contain \\, ; or \": {:?}",
            String::from_utf8_lossy(&self.0)
        )
    }
}

impl std::error::Error for InvalidKey {}

/// An info string, kept as its text so it goes over the network and into
/// config strings exactly as the game would write it.
#[derive(Clone, Default)]
pub struct InfoString {
    text: Vec<u8>,
    /// Kept while the text is longer than [`INDEX_FROM`] and ends cleanly
    /// (see [`ends_cleanly`]).
    index: Option<KeyIndex>,
}

/// One `\key\value` pair's place in the text.
struct Pair {
    /// Where the pair starts: its leading `\` (the first pair may have none).
    start: usize,
    key: Range<usize>,
    value: Range<usize>,
}

/// The pairs in the order the game walks them. The walk ends with a value
/// that runs to the end of the text, or at a key with no `\\` after it (a
/// dangling key, which the game ignores).
///
/// Every pair after the first starts at the `\\` that ended the value before
/// it, so the walk is just the text's separators in order: key end, value
/// end, key end, value end ... ([`bytes::Positions`] finds them eight bytes
/// at a time).
struct Pairs<'a> {
    text: &'a [u8],
    separators: bytes::Positions<'a>,
    /// Where the next pair starts and where its key starts; `None` once the walk has ended.
    next: Option<(usize, usize)>,
}

impl Iterator for Pairs<'_> {
    type Item = Pair;

    fn next(&mut self) -> Option<Pair> {
        let (start, key_start) = self.next.take()?;
        let key_end = self.separators.next()?; // none: a dangling key
        let value_end = self.separators.next().unwrap_or(self.text.len());
        if value_end < self.text.len() {
            self.next = Some((value_end, value_end + 1));
        }
        Some(Pair {
            start,
            key: key_start..key_end,
            value: key_end + 1..value_end,
        })
    }
}

fn pairs(text: &[u8]) -> Pairs<'_> {
    let mut separators = bytes::Positions::new(SEPARATOR, text, 0);
    // A leading `\\` only opens the first key.
    let key_start = if text.first() == Some(&SEPARATOR) {
        separators.next();
        1
    } else {
        0
    };
    Pairs {
        text,
        separators,
        next: Some((0, key_start)),
    }
}

/// Whether the walk of `text` ends with a value running to its end (or the
/// text is empty). Only then does appending `\key\value` add exactly that
/// pair, and removing a pair leave the others as they were; a dangling key
/// at the end would take the appended key as its value instead.
fn ends_cleanly(text: &[u8]) -> bool {
    pairs(text).last().map_or(0, |pair| pair.value.end) == text.len()
}

/// Which keys an info string has: how many pairs there are per key
/// (ignoring case), counted by a hash of the key.
///
/// It only answers "missing" for sure. A key it counts is still looked up
/// in the text, so two keys with the same hash cost a walk, never a wrong
/// answer. The hash has a random key per index, so nobody can choose keys
/// that collide on purpose to slow a server down.
#[derive(Clone)]
struct KeyIndex {
    hasher: RandomState,
    counts: HashMap<u64, u32, BuildHasherDefault<AlreadyHashed>>,
}

/// The counts' keys are already random hashes, so the map uses them as they are.
#[derive(Default)]
struct AlreadyHashed(u64);

impl Hasher for AlreadyHashed {
    fn finish(&self) -> u64 {
        self.0
    }

    fn write_u64(&mut self, hash: u64) {
        self.0 = hash;
    }

    fn write(&mut self, bytes: &[u8]) {
        // Not used by a map with `u64` keys; folds the bytes in for completeness.
        for &byte in bytes {
            self.0 = self.0.rotate_left(8) ^ u64::from(byte);
        }
    }
}

impl KeyIndex {
    /// The index of `text`, if its walk ends cleanly.
    fn build(text: &[u8]) -> Option<KeyIndex> {
        if !ends_cleanly(text) {
            return None;
        }
        let mut index = KeyIndex {
            hasher: RandomState::new(),
            counts: HashMap::default(),
        };
        for pair in pairs(text) {
            index.add(&text[pair.key]);
        }
        Some(index)
    }

    /// The hash of the lower-cased key, lower-cased in pieces on the stack.
    fn hash(&self, key: &[u8]) -> u64 {
        let mut hasher = self.hasher.build_hasher();
        let mut piece = [0u8; 32];
        for chunk in key.chunks(piece.len()) {
            let lower = &mut piece[..chunk.len()];
            lower.copy_from_slice(chunk);
            lower.make_ascii_lowercase();
            hasher.write(lower);
        }
        hasher.finish()
    }

    fn may_contain(&self, key: &[u8]) -> bool {
        self.counts.contains_key(&self.hash(key))
    }

    fn add(&mut self, key: &[u8]) {
        *self.counts.entry(self.hash(key)).or_insert(0) += 1;
    }

    fn remove(&mut self, key: &[u8]) {
        let hash = self.hash(key);
        if let Some(count) = self.counts.get_mut(&hash) {
            *count -= 1;
            if *count == 0 {
                self.counts.remove(&hash);
            }
        }
    }
}

impl InfoString {
    pub fn new() -> Self {
        Self::default()
    }

    /// Takes existing text as it is (from the network or a config string).
    pub fn from_text(text: &[u8]) -> Self {
        let index = if text.len() > INDEX_FROM {
            KeyIndex::build(text)
        } else {
            None
        };
        InfoString {
            text: text.to_vec(),
            index,
        }
    }

    pub fn as_bytes(&self) -> &[u8] {
        &self.text
    }

    /// Whether an Xbox 360 or recomp peer can take this string: under the
    /// console's 1024 bytes. Bigger ones make the console drop the
    /// connection ("oversize infostring"), so anything sent to such a peer
    /// has to pass this first.
    pub fn fits_console(&self) -> bool {
        self.text.len() < crate::shared::info::MAX_INFO_STRING
    }

    /// False when the index says the key (ignoring case) isn't there.
    fn may_contain(&self, key: &[u8]) -> bool {
        match &self.index {
            Some(index) => index.may_contain(key),
            None => true,
        }
    }

    /// The value of `key`, ignoring ASCII case; empty when missing.
    pub fn get(&self, key: &[u8]) -> &[u8] {
        if !self.may_contain(key) {
            return &[];
        }
        pairs(&self.text)
            .find(|pair| equal_ignore_case(&self.text[pair.key.clone()], key))
            .map_or(&[], |pair| &self.text[pair.value])
    }

    /// Removes the first pair whose key is exactly `key` (case counts, as in the game).
    pub fn remove(&mut self, key: &[u8]) {
        if key.contains(&SEPARATOR) || !self.may_contain(key) {
            return;
        }
        let found = pairs(&self.text).find(|pair| &self.text[pair.key.clone()] == key);
        if let Some(pair) = found {
            self.text.drain(pair.start..pair.value.end);
            if let Some(index) = &mut self.index {
                index.remove(key);
            }
        }
    }

    /// Sets `key`: removes its old pair, then appends `\key\value` (an empty
    /// value only removes it). `\`, `;` and `"` are dropped from the value.
    pub fn set(&mut self, key: &[u8], value: &[u8]) -> Result<(), InvalidKey> {
        if key.iter().any(|&c| is_reserved(c)) {
            return Err(InvalidKey(key.to_vec()));
        }
        self.remove(key);
        let pair_start = self.text.len();
        self.text.reserve(key.len() + value.len() + 2);
        self.text.push(SEPARATOR);
        self.text.extend_from_slice(key);
        self.text.push(SEPARATOR);
        let value_start = self.text.len();
        for piece in value.split(|&c| is_reserved(c)) {
            self.text.extend_from_slice(piece);
        }
        if self.text.len() == value_start {
            self.text.truncate(pair_start); // nothing left of the value
            return Ok(());
        }
        match &mut self.index {
            Some(index) => index.add(key),
            None if self.text.len() > INDEX_FROM => self.index = KeyIndex::build(&self.text),
            None => {}
        }
        Ok(())
    }
}

/// Two info strings are equal when their text is.
impl PartialEq for InfoString {
    fn eq(&self, other: &Self) -> bool {
        self.text == other.text
    }
}

impl Eq for InfoString {}

impl fmt::Debug for InfoString {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_tuple("InfoString")
            .field(&String::from_utf8_lossy(&self.text))
            .finish()
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::cstr;
    use crate::shared::info as console;
    use crate::test_random::Random;

    fn word(r: &mut Random, max: u32) -> Vec<u8> {
        let alphabet = b"abcABC12_ .-";
        (0..r.below(max + 1))
            .map(|_| alphabet[r.below(alphabet.len() as u32) as usize])
            .collect()
    }

    #[test]
    fn same_text_as_the_console_within_its_limits() {
        let mut r = Random::new(11);
        for _ in 0..5_000 {
            let mut console_buffer = vec![0u8; 2048];
            let mut engine = InfoString::new();
            let mut keys: Vec<Vec<u8>> = Vec::new();
            for _ in 0..r.below(12) {
                let key = word(&mut r, 6);
                let key = if !keys.is_empty() && r.below(3) == 0 {
                    keys[r.below(keys.len() as u32) as usize].clone()
                } else {
                    key
                };
                keys.push(key.clone());
                let mut value = word(&mut r, 20);
                if r.below(10) == 0 {
                    value.push(b';');
                }
                let console_result = console::set_value_for_key(&mut console_buffer, &key, &value);
                let engine_result = engine.set(&key, &value);
                assert_eq!(console_result.is_ok(), engine_result.is_ok());
                if r.below(4) == 0 {
                    let k = keys[r.below(keys.len() as u32) as usize].clone();
                    console::remove_key(&mut console_buffer, &k).unwrap();
                    engine.remove(&k);
                }
                assert_eq!(engine.as_bytes(), cstr::text(&console_buffer));
                let probe = keys[r.below(keys.len() as u32) as usize].to_ascii_uppercase();
                assert_eq!(
                    engine.get(&probe),
                    console::value_for_key(cstr::text(&console_buffer), &probe).unwrap()
                );
            }
        }
    }

    /// The straightforward version (a list of every pair per call), as a reference.
    struct Reference(Vec<u8>);

    impl Reference {
        fn pairs(&self) -> Vec<Pair> {
            pairs(&self.0).collect()
        }

        fn get(&self, key: &[u8]) -> &[u8] {
            self.pairs()
                .into_iter()
                .find(|p| self.0[p.key.clone()].eq_ignore_ascii_case(key))
                .map_or(&[], |p| &self.0[p.value])
        }

        fn remove(&mut self, key: &[u8]) {
            if key.contains(&b'\\') {
                return;
            }
            if let Some(p) = self
                .pairs()
                .into_iter()
                .find(|p| &self.0[p.key.clone()] == key)
            {
                self.0.drain(p.start..p.value.end);
            }
        }

        fn set(&mut self, key: &[u8], value: &[u8]) {
            if key.iter().any(|&c| is_reserved(c)) {
                return;
            }
            self.remove(key);
            let clean: Vec<u8> = value.iter().copied().filter(|&c| !is_reserved(c)).collect();
            if !clean.is_empty() {
                self.0.push(b'\\');
                self.0.extend_from_slice(key);
                self.0.push(b'\\');
                self.0.extend_from_slice(&clean);
            }
        }
    }

    /// Big strings (with the index) and odd ones (dangling keys, no leading
    /// `\\`, empty keys and values) against the reference.
    #[test]
    fn same_as_the_reference_with_the_index() {
        let mut r = Random::new(23);
        let starts: [&[u8]; 6] = [
            b"",
            b"a\\1",
            b"\\a\\1\\b",
            b"\\a\\1\\",
            b"\\",
            b"\\\\x\\a\\",
        ];
        for round in 0..300 {
            let start = starts[round % starts.len()];
            let mut engine = InfoString::from_text(start);
            let mut reference = Reference(start.to_vec());
            for step in 0..400 {
                let key_number = r.below(if round % 2 == 0 { 60 } else { 400 });
                let mut key = format!("k{key_number}").into_bytes();
                if r.below(4) == 0 {
                    key.make_ascii_uppercase();
                }
                if r.below(50) == 0 {
                    key.clear();
                }
                match r.below(10) {
                    0..=5 => {
                        let mut value = word(&mut r, 12);
                        if r.below(8) == 0 {
                            value.push(b'\\');
                        }
                        assert!(engine.set(&key, &value).is_ok());
                        reference.set(&key, &value);
                    }
                    6 => {
                        engine.remove(&key);
                        reference.remove(&key);
                    }
                    7 if step % 97 == 0 => {
                        // Start over from the text, as if it came from the network.
                        engine = InfoString::from_text(&reference.0);
                    }
                    _ => {}
                }
                assert_eq!(engine.as_bytes(), &reference.0[..]);
                assert_eq!(
                    engine.get(&key),
                    reference.get(&key),
                    "{:?}",
                    String::from_utf8_lossy(&key)
                );
            }
        }
    }

    #[test]
    fn no_size_limit() {
        let mut info = InfoString::new();
        let long = vec![b'x'; 5_000];
        for i in 0..100 {
            info.set(format!("key{i}").as_bytes(), &long).unwrap();
        }
        assert_eq!(info.get(b"KEY99"), &long[..]);
        assert!(info.as_bytes().len() > 500_000);
        assert!(!info.fits_console());
        assert!(info.set(b"bad;key", b"1").is_err());
    }
}
