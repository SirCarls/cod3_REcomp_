//! The copy's manifest, `newcopy.toml`: where the copy came from and every
//! file in it with its size and SHA-256.
//!
//! It is plain TOML (a small subset: strings, integers, an integer list and
//! `[[file]]` tables), written and read here by hand so the crate needs no
//! dependencies.

use std::fmt::Write as _;
use std::time::{SystemTime, UNIX_EPOCH};

/// File name of the manifest inside `<out>/<game>/`.
pub const MANIFEST_NAME: &str = "newcopy.toml";
/// Version of the manifest's layout.
pub const FORMAT: u32 = 1;
/// Largest manifest read.
pub const MAX_MANIFEST_BYTES: u64 = 4 << 20;

#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct Manifest {
    pub format: u32,
    pub tool: String,
    pub tool_version: String,
    /// When the copy was made, UTC, ISO 8601.
    pub made: String,
    /// Game key: `cod`, `coduo`, `cod2`.
    pub game: String,
    pub game_name: String,
    pub version: Option<String>,
    pub language: Option<String>,
    /// `steam` or `folder`.
    pub source_kind: String,
    pub source_path: String,
    pub steam_app_id: Option<u32>,
    pub files: Vec<ManifestFile>,
}

#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct ManifestFile {
    /// Path inside the copy, `/`-separated: `main/iw_00.iwd`.
    pub path: String,
    pub size: u64,
    /// Lowercase hex.
    pub sha256: String,
    /// `hard link` or `copy`.
    pub how: String,
}

/// A TOML basic string, quoted and escaped.
fn quote(text: &str) -> String {
    let mut out = String::with_capacity(text.len() + 2);
    out.push('"');
    for c in text.chars() {
        match c {
            '"' => out.push_str("\\\""),
            '\\' => out.push_str("\\\\"),
            '\n' => out.push_str("\\n"),
            '\t' => out.push_str("\\t"),
            '\r' => out.push_str("\\r"),
            c if c.is_control() => {
                let _ = write!(out, "\\u{:04X}", c as u32);
            }
            c => out.push(c),
        }
    }
    out.push('"');
    out
}

impl Manifest {
    pub fn to_toml(&self) -> String {
        let mut out = String::new();
        out.push_str(&format!(
            "# A new copy of {} for the Rust engine, made by {}.\n",
            self.game_name, self.tool
        ));
        out.push_str(
            "# It holds only the game's data archives and version text: no executable.\n\
             # `newcopy check <this folder>` checks every file below against its SHA-256.\n\n",
        );
        let mut line = |key: &str, value: String| {
            out.push_str(key);
            out.push_str(" = ");
            out.push_str(&value);
            out.push('\n');
        };
        line("format", self.format.to_string());
        line("tool", quote(&self.tool));
        line("tool_version", quote(&self.tool_version));
        line("made", quote(&self.made));
        line("game", quote(&self.game));
        line("game_name", quote(&self.game_name));
        if let Some(version) = &self.version {
            line("version", quote(version));
        }
        if let Some(language) = &self.language {
            line("language", quote(language));
        }
        line("source_kind", quote(&self.source_kind));
        line("source_path", quote(&self.source_path));
        if let Some(id) = self.steam_app_id {
            line("steam_app_id", id.to_string());
        }
        for file in &self.files {
            out.push_str("\n[[file]]\n");
            out.push_str(&format!("path = {}\n", quote(&file.path)));
            out.push_str(&format!("size = {}\n", file.size));
            out.push_str(&format!("sha256 = {}\n", quote(&file.sha256)));
            out.push_str(&format!("how = {}\n", quote(&file.how)));
        }
        out
    }

    /// Reads a manifest written by [`Manifest::to_toml`]. Unknown keys are
    /// skipped, so a newer tool's manifest still reads.
    pub fn parse(text: &str) -> Result<Manifest, String> {
        let mut manifest = Manifest::default();
        let mut current_file: Option<ManifestFile> = None;
        for (number, raw) in text.lines().enumerate() {
            let line_no = number + 1;
            let line = raw.trim();
            if line.is_empty() || line.starts_with('#') {
                continue;
            }
            if line == "[[file]]" {
                if let Some(file) = current_file.take() {
                    manifest.files.push(file);
                }
                current_file = Some(ManifestFile::default());
                continue;
            }
            if line.starts_with('[') {
                return Err(format!("line {line_no}: unknown table {line}"));
            }
            let (key, rest) = line
                .split_once('=')
                .ok_or_else(|| format!("line {line_no}: expected key = value"))?;
            let key = key.trim();
            let value = parse_value(rest.trim()).map_err(|e| format!("line {line_no}: {e}"))?;
            let as_string = |v: &TomlValue| match v {
                TomlValue::String(s) => Ok(s.clone()),
                TomlValue::Integer(_) => Err(format!("line {line_no}: {key} should be a string")),
            };
            let as_integer = |v: &TomlValue| match v {
                TomlValue::Integer(n) => Ok(*n),
                TomlValue::String(_) => Err(format!("line {line_no}: {key} should be a number")),
            };
            if let Some(file) = current_file.as_mut() {
                match key {
                    "path" => file.path = as_string(&value)?,
                    "size" => file.size = as_integer(&value)?,
                    "sha256" => file.sha256 = as_string(&value)?,
                    "how" => file.how = as_string(&value)?,
                    _ => {}
                }
                continue;
            }
            match key {
                "format" => {
                    manifest.format = u32::try_from(as_integer(&value)?)
                        .map_err(|_| format!("line {line_no}: format out of range"))?
                }
                "tool" => manifest.tool = as_string(&value)?,
                "tool_version" => manifest.tool_version = as_string(&value)?,
                "made" => manifest.made = as_string(&value)?,
                "game" => manifest.game = as_string(&value)?,
                "game_name" => manifest.game_name = as_string(&value)?,
                "version" => manifest.version = Some(as_string(&value)?),
                "language" => manifest.language = Some(as_string(&value)?),
                "source_kind" => manifest.source_kind = as_string(&value)?,
                "source_path" => manifest.source_path = as_string(&value)?,
                "steam_app_id" => {
                    manifest.steam_app_id = Some(
                        u32::try_from(as_integer(&value)?)
                            .map_err(|_| format!("line {line_no}: app id out of range"))?,
                    )
                }
                _ => {}
            }
        }
        if let Some(file) = current_file.take() {
            manifest.files.push(file);
        }
        if manifest.format == 0 || manifest.game.is_empty() {
            return Err("not a newcopy manifest (no format or game)".into());
        }
        if manifest.format > FORMAT {
            return Err(format!(
                "manifest format {} is newer than this tool understands ({FORMAT})",
                manifest.format
            ));
        }
        for file in &manifest.files {
            if file.path.is_empty() || file.sha256.is_empty() {
                return Err("a [[file]] entry has no path or sha256".into());
            }
        }
        Ok(manifest)
    }
}

#[derive(Debug, Clone, PartialEq, Eq)]
enum TomlValue {
    String(String),
    Integer(u64),
}

/// One value and an optional `# comment` after it.
fn parse_value(text: &str) -> Result<TomlValue, String> {
    let (value, rest) = if let Some(body) = text.strip_prefix('"') {
        let mut value = String::new();
        let mut chars = body.char_indices();
        let end = loop {
            let Some((at, c)) = chars.next() else {
                return Err("a string is never closed".into());
            };
            match c {
                '"' => break at + 1,
                '\\' => {
                    let (_, escape) = chars.next().ok_or("a string is never closed")?;
                    match escape {
                        '"' => value.push('"'),
                        '\\' => value.push('\\'),
                        'n' => value.push('\n'),
                        't' => value.push('\t'),
                        'r' => value.push('\r'),
                        'b' => value.push('\u{8}'),
                        'f' => value.push('\u{c}'),
                        'u' | 'U' => {
                            let digits = if escape == 'u' { 4 } else { 8 };
                            let mut code = 0u32;
                            for _ in 0..digits {
                                let (_, d) = chars.next().ok_or("a string is never closed")?;
                                code = code * 16 + d.to_digit(16).ok_or("bad \\u escape")?;
                            }
                            value.push(char::from_u32(code).ok_or("bad \\u escape")?);
                        }
                        other => return Err(format!("unknown escape \\{other}")),
                    }
                }
                c => value.push(c),
            }
        };
        (TomlValue::String(value), &body[end..])
    } else {
        let end = text
            .find(|c: char| c.is_whitespace() || c == '#')
            .unwrap_or(text.len());
        let digits = text[..end].replace('_', "");
        let number = digits
            .parse::<u64>()
            .map_err(|_| format!("expected a string or a number, found {}", &text[..end]))?;
        (TomlValue::Integer(number), &text[end..])
    };
    let rest = rest.trim_start();
    if !rest.is_empty() && !rest.starts_with('#') {
        return Err(format!("unexpected text after the value: {rest}"));
    }
    Ok(value)
}

/// Now, UTC, as `YYYY-MM-DDTHH:MM:SSZ`.
pub fn utc_now() -> String {
    let seconds = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map(|d| d.as_secs())
        .unwrap_or(0);
    utc_timestamp(seconds)
}

/// Seconds since 1970 as `YYYY-MM-DDTHH:MM:SSZ` (proleptic Gregorian).
pub fn utc_timestamp(seconds: u64) -> String {
    let days = (seconds / 86_400) as i64;
    let in_day = seconds % 86_400;
    // Days to civil date (Howard Hinnant's algorithm, public domain).
    let z = days + 719_468;
    let era = z.div_euclid(146_097);
    let day_of_era = z.rem_euclid(146_097);
    let year_of_era =
        (day_of_era - day_of_era / 1460 + day_of_era / 36_524 - day_of_era / 146_096) / 365;
    let day_of_year = day_of_era - (365 * year_of_era + year_of_era / 4 - year_of_era / 100);
    let month_index = (5 * day_of_year + 2) / 153;
    let day = day_of_year - (153 * month_index + 2) / 5 + 1;
    let month = if month_index < 10 {
        month_index + 3
    } else {
        month_index - 9
    };
    let year = year_of_era + era * 400 + i64::from(month <= 2);
    format!(
        "{year:04}-{month:02}-{day:02}T{:02}:{:02}:{:02}Z",
        in_day / 3600,
        in_day / 60 % 60,
        in_day % 60
    )
}

#[cfg(test)]
mod tests {
    use super::*;

    fn sample() -> Manifest {
        Manifest {
            format: FORMAT,
            tool: "newcopy".into(),
            tool_version: "0.1.0".into(),
            made: utc_timestamp(0),
            game: "cod2".into(),
            game_name: "Call of Duty 2".into(),
            version: Some("1.3".into()),
            language: Some("english".into()),
            source_kind: "folder".into(),
            source_path: r#"D:\Games\Call of Duty 2 "CB""#.into(),
            steam_app_id: Some(2630),
            files: vec![
                ManifestFile {
                    path: "main/iw_00.iwd".into(),
                    size: 170_000_000,
                    sha256: "ab".repeat(32),
                    how: "hard link".into(),
                },
                ManifestFile {
                    path: "version.inf".into(),
                    size: 141,
                    sha256: "cd".repeat(32),
                    how: "copy".into(),
                },
            ],
        }
    }

    #[test]
    fn round_trip() {
        let manifest = sample();
        let text = manifest.to_toml();
        assert!(text.contains("source_path = \"D:\\\\Games\\\\Call of Duty 2 \\\"CB\\\"\""));
        assert_eq!(Manifest::parse(&text).unwrap(), manifest);
    }

    #[test]
    fn optional_fields_and_comments() {
        let mut manifest = sample();
        manifest.version = None;
        manifest.language = None;
        manifest.steam_app_id = None;
        manifest.source_path = "tab\there\u{1}".into();
        let text = manifest.to_toml() + "future_key = 7 # added later\n";
        assert_eq!(Manifest::parse(&text).unwrap(), manifest);
    }

    #[test]
    fn bad_manifests() {
        assert!(Manifest::parse("").is_err());
        assert!(Manifest::parse("format = 1\n").is_err());
        assert!(Manifest::parse("format = 1\ngame = \"cod\n").is_err());
        assert!(Manifest::parse("format = 1\ngame = 5\n").is_err());
        assert!(Manifest::parse("format = 99\ngame = \"cod\"\n").is_err());
        assert!(Manifest::parse("format = 1\ngame = \"cod\"\n[[file]]\nsize = 1\n").is_err());
        assert!(Manifest::parse("format = 1\ngame = \"cod\" x\n").is_err());
        assert!(Manifest::parse("format = 1\ngame = \"cod\"\n[other]\n").is_err());
    }

    #[test]
    fn timestamps() {
        assert_eq!(utc_timestamp(0), "1970-01-01T00:00:00Z");
        assert_eq!(utc_timestamp(951_782_400), "2000-02-29T00:00:00Z");
        assert_eq!(utc_timestamp(1_791_861_296), "2026-10-13T03:14:56Z");
        assert_eq!(utc_timestamp(4_102_444_799), "2099-12-31T23:59:59Z");
    }
}
