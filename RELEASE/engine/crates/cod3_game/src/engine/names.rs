//! Names the engine and the level scripts trade by hash.
//!
//! The hash is the console's ([`name_hash`]): it has to match, because the
//! level scripts and the ripped data refer to events, functions and tags by
//! it. What the console limits is only the copy of the text it keeps beside
//! the hash (27 characters); [`Name`] keeps all of it.

pub use crate::shared::hash::name_hash;

/// A name with its hash; the text is kept in full, lower-cased like the game's copy.
#[derive(Debug, Clone, PartialEq, Eq, Hash)]
pub struct Name {
    pub hash: u32,
    pub text: Vec<u8>,
}

impl Name {
    pub fn new(text: &[u8]) -> Self {
        // The hash lower-cases anyway, so hashing the lower-cased copy is the same.
        let text = text.to_ascii_lowercase();
        Name {
            hash: name_hash(&text),
            text,
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::shared::hash::name_key;

    #[test]
    fn same_hash_and_text_as_the_console_without_the_cut() {
        for text in [
            &b"end"[..],
            b"Tag_Flash",
            b"chambois_drones::drone_combat_cover_with_a_long_name",
        ] {
            let name = Name::new(text);
            let key = name_key(text);
            assert_eq!(name.hash, key.hash);
            let kept = key.text.iter().position(|&c| c == 0).unwrap();
            assert_eq!(&name.text[..kept], &key.text[..kept]);
            assert_eq!(name.text.len(), text.len());
        }
    }
}
