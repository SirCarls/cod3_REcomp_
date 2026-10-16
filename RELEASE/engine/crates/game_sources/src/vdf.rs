//! Reads Valve's KeyValues text format (`.vdf`, `.acf`), which Steam uses
//! for `libraryfolders.vdf` and the `appmanifest_<id>.acf` files.
//!
//! A file is a list of `key value` pairs, where a value is a string or a
//! `{ ... }` block holding more pairs:
//!
//! ```text
//! "AppState"
//! {
//!     "appid"      "2630"
//!     "installdir" "Call of Duty 2"
//! }
//! ```
//!
//! Strings are quoted (with `\\`, `\"`, `\n`, `\t` escapes; Windows paths
//! appear as `"D:\\SteamLibrary"`) or bare words. `//` starts a comment.
//! A condition such as `[$WIN32]` after a value is skipped. These files are
//! on the player's disk and may be damaged: a bad file gives a
//! [`ParseError`], never a panic.

use std::fmt;
use std::fs;
use std::io::Read;
use std::path::Path;

/// Largest file read. Steam's own files are a few kilobytes.
pub const MAX_FILE_BYTES: u64 = 16 << 20;
/// Deepest nesting of blocks accepted.
pub const MAX_DEPTH: usize = 64;

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Value {
    String(String),
    Block(Block),
}

/// A list of key-value pairs, in file order. Keys are matched without
/// regard to case, as Steam does.
#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct Block {
    pub entries: Vec<(String, Value)>,
}

impl Block {
    /// The first value under `key`.
    pub fn get(&self, key: &str) -> Option<&Value> {
        self.entries
            .iter()
            .find(|(k, _)| k.eq_ignore_ascii_case(key))
            .map(|(_, v)| v)
    }

    /// The first string value under `key`.
    pub fn get_str(&self, key: &str) -> Option<&str> {
        self.entries.iter().find_map(|(k, v)| match v {
            Value::String(s) if k.eq_ignore_ascii_case(key) => Some(s.as_str()),
            _ => None,
        })
    }

    /// The first block under `key`.
    pub fn get_block(&self, key: &str) -> Option<&Block> {
        self.entries.iter().find_map(|(k, v)| match v {
            Value::Block(b) if k.eq_ignore_ascii_case(key) => Some(b),
            _ => None,
        })
    }
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ParseError {
    /// 1-based line of the problem.
    pub line: usize,
    pub message: String,
}

impl fmt::Display for ParseError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "line {}: {}", self.line, self.message)
    }
}

impl std::error::Error for ParseError {}

#[derive(Debug, Clone, PartialEq, Eq)]
enum Token {
    Open,
    Close,
    Text(String),
    /// `[$WIN32]` and similar platform conditions.
    Condition,
}

struct Lexer<'a> {
    chars: std::iter::Peekable<std::str::Chars<'a>>,
    line: usize,
}

impl<'a> Lexer<'a> {
    fn new(text: &'a str) -> Self {
        let text = text.strip_prefix('\u{FEFF}').unwrap_or(text);
        Lexer {
            chars: text.chars().peekable(),
            line: 1,
        }
    }

    fn error(&self, message: impl Into<String>) -> ParseError {
        ParseError {
            line: self.line,
            message: message.into(),
        }
    }

    fn next_token(&mut self) -> Result<Option<Token>, ParseError> {
        loop {
            match self.chars.peek().copied() {
                None => return Ok(None),
                Some('\n') => {
                    self.line += 1;
                    self.chars.next();
                }
                Some(c) if c.is_whitespace() => {
                    self.chars.next();
                }
                Some('/') => {
                    self.chars.next();
                    if self.chars.peek() == Some(&'/') {
                        while let Some(&c) = self.chars.peek() {
                            if c == '\n' {
                                break;
                            }
                            self.chars.next();
                        }
                    } else {
                        // A lone slash starts a bare word.
                        return Ok(Some(Token::Text(self.bare_word("/"))));
                    }
                }
                Some('{') => {
                    self.chars.next();
                    return Ok(Some(Token::Open));
                }
                Some('}') => {
                    self.chars.next();
                    return Ok(Some(Token::Close));
                }
                Some('"') => {
                    self.chars.next();
                    return self.quoted().map(|s| Some(Token::Text(s)));
                }
                Some('[') => {
                    let word = self.bare_word("");
                    if word.ends_with(']') {
                        return Ok(Some(Token::Condition));
                    }
                    return Ok(Some(Token::Text(word)));
                }
                Some(_) => return Ok(Some(Token::Text(self.bare_word("")))),
            }
        }
    }

    fn quoted(&mut self) -> Result<String, ParseError> {
        let start_line = self.line;
        let mut text = String::new();
        loop {
            match self.chars.next() {
                None => {
                    return Err(ParseError {
                        line: start_line,
                        message: "a quoted string is never closed".into(),
                    })
                }
                Some('"') => return Ok(text),
                Some('\\') => match self.chars.next() {
                    Some('\\') => text.push('\\'),
                    Some('"') => text.push('"'),
                    Some('n') => text.push('\n'),
                    Some('t') => text.push('\t'),
                    // Anything else keeps its backslash, as Steam does.
                    Some(other) => {
                        if other == '\n' {
                            self.line += 1;
                        }
                        text.push('\\');
                        text.push(other);
                    }
                    None => {
                        return Err(ParseError {
                            line: start_line,
                            message: "a quoted string is never closed".into(),
                        })
                    }
                },
                Some(c) => {
                    if c == '\n' {
                        self.line += 1;
                    }
                    text.push(c);
                }
            }
        }
    }

    fn bare_word(&mut self, start: &str) -> String {
        let mut word = String::from(start);
        while let Some(&c) = self.chars.peek() {
            if c.is_whitespace() || c == '{' || c == '}' || c == '"' {
                break;
            }
            word.push(c);
            self.chars.next();
        }
        word
    }
}

/// Parses a whole file's text. The result is the top level: usually one
/// pair such as `"libraryfolders" { ... }`.
pub fn parse(text: &str) -> Result<Block, ParseError> {
    let mut lexer = Lexer::new(text);
    let block = parse_block(&mut lexer, 0)?;
    Ok(block)
}

/// Reads and parses a file (at most [`MAX_FILE_BYTES`]).
pub fn parse_file(path: &Path) -> Result<Block, String> {
    let text = read_text_limited(path, MAX_FILE_BYTES).map_err(|e| e.to_string())?;
    parse(&text).map_err(|e| e.to_string())
}

/// Reads a text file of at most `limit` bytes; invalid UTF-8 is replaced.
pub(crate) fn read_text_limited(path: &Path, limit: u64) -> std::io::Result<String> {
    let file = fs::File::open(path)?;
    let mut bytes = Vec::new();
    file.take(limit + 1).read_to_end(&mut bytes)?;
    if bytes.len() as u64 > limit {
        return Err(std::io::Error::new(
            std::io::ErrorKind::InvalidData,
            format!("larger than {limit} bytes"),
        ));
    }
    Ok(String::from_utf8_lossy(&bytes).into_owned())
}

fn parse_block(lexer: &mut Lexer<'_>, depth: usize) -> Result<Block, ParseError> {
    if depth > MAX_DEPTH {
        return Err(lexer.error(format!("blocks nested deeper than {MAX_DEPTH}")));
    }
    let mut block = Block::default();
    loop {
        let key = match lexer.next_token()? {
            None if depth == 0 => return Ok(block),
            None => return Err(lexer.error("the file ends inside a { block }")),
            Some(Token::Close) if depth > 0 => return Ok(block),
            Some(Token::Close) => return Err(lexer.error("a } with no { before it")),
            Some(Token::Open) => return Err(lexer.error("a { where a key should be")),
            Some(Token::Condition) => continue,
            Some(Token::Text(key)) => key,
        };
        let value = loop {
            match lexer.next_token()? {
                None => return Err(lexer.error(format!("the key \"{key}\" has no value"))),
                Some(Token::Text(text)) => break Value::String(text),
                Some(Token::Open) => break Value::Block(parse_block(lexer, depth + 1)?),
                Some(Token::Close) => {
                    return Err(lexer.error(format!("the key \"{key}\" has no value")))
                }
                Some(Token::Condition) => continue,
            }
        };
        block.entries.push((key, value));
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn nested_blocks_and_escapes() {
        let text = "\u{FEFF}\"libraryfolders\"\r\n{\r\n\t\"0\"\r\n\t{\r\n\t\t\"path\"\t\t\"D:\\\\Steam Library\"\r\n\t\t\"label\"\t\t\"say \\\"hi\\\"\"\r\n\t\t\"apps\"\r\n\t\t{\r\n\t\t\t\"2630\"\t\t\"123\"\r\n\t\t}\r\n\t}\r\n}\r\n";
        let root = parse(text).unwrap();
        let folders = root.get_block("LibraryFolders").unwrap();
        let first = folders.get_block("0").unwrap();
        assert_eq!(first.get_str("path"), Some("D:\\Steam Library"));
        assert_eq!(first.get_str("label"), Some("say \"hi\""));
        assert_eq!(
            first.get_block("apps").unwrap().get_str("2630"),
            Some("123")
        );
    }

    #[test]
    fn comments_bare_words_and_conditions() {
        let text =
            "// a comment\nAppState { appid 2620 // trailing\n \"x\" \"1\" [$WIN32]\n \"y\" { } }";
        let root = parse(text).unwrap();
        let app = root.get_block("appstate").unwrap();
        assert_eq!(app.get_str("appid"), Some("2620"));
        assert_eq!(app.get_str("x"), Some("1"));
        assert!(app.get_block("y").unwrap().entries.is_empty());
    }

    #[test]
    fn unknown_escape_keeps_backslash() {
        let root = parse(r#""p" "C:\Games\x""#).unwrap();
        assert_eq!(root.get_str("p"), Some(r"C:\Games\x"));
    }

    #[test]
    fn malformed_files_are_errors() {
        for text in [
            "\"a\" { \"b\" \"c\"",  // never closed
            "\"a\"",                // key with no value
            "}",                    // stray close
            "{ }",                  // block with no key
            "\"a\" \"unterminated", // string never closed
            "\"a\" { \"b\" }",      // key with no value inside
            "\"a\" \"b\\",          // escape at end of file
        ] {
            assert!(parse(text).is_err(), "{text:?} should fail");
        }
        let error = parse("\"a\"\n{\n\"b\"\n}").unwrap_err();
        assert_eq!(error.line, 4);
    }

    #[test]
    fn deep_nesting_is_refused() {
        let text = "\"k\" {".repeat(MAX_DEPTH + 2);
        assert!(parse(&text).is_err());
    }

    #[test]
    fn empty_file_is_empty_block() {
        assert!(parse("").unwrap().entries.is_empty());
        assert!(parse("  // nothing\n").unwrap().entries.is_empty());
    }
}
