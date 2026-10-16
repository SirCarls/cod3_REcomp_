//! The supported games: Steam app ids and folders, where each game keeps
//! its data archives, and which archive names belong to it.
//!
//! Only archive *names* are listed here (no game data). The expected sets
//! are those of the last official patch, which the Rust engine targets:
//! Call of Duty 1.5, United Offensive 1.51 and Call of Duty 2 1.3.
//!
//! Executable names are listed only to recognize an install by its folder
//! listing. They are never opened, read, hashed, copied or run.

use std::fmt;

#[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub enum GameId {
    /// Call of Duty (2003).
    Cod,
    /// Call of Duty: United Offensive (2004), the expansion. It needs Call of
    /// Duty's own archives too, so its copy holds both.
    CodUo,
    /// Call of Duty 2 (2005).
    Cod2,
}

impl GameId {
    pub const ALL: [GameId; 3] = [GameId::Cod, GameId::CodUo, GameId::Cod2];

    /// The short name used on the command line and as the copy's folder name.
    pub fn key(self) -> &'static str {
        self.game().key
    }

    pub fn from_key(key: &str) -> Option<GameId> {
        GameId::ALL
            .into_iter()
            .find(|id| id.key().eq_ignore_ascii_case(key))
    }

    pub fn game(self) -> &'static Game {
        match self {
            GameId::Cod => &COD,
            GameId::CodUo => &COD_UO,
            GameId::Cod2 => &COD2,
        }
    }
}

impl fmt::Display for GameId {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str(self.key())
    }
}

/// One supported game.
#[derive(Debug)]
pub struct Game {
    pub id: GameId,
    /// Command-line name and the copy's folder name: `cod`, `coduo`, `cod2`.
    pub key: &'static str,
    pub name: &'static str,
    /// The Steam store app (checked on SteamDB, see docs/SOURCES.md).
    pub steam_app_id: u32,
    /// Folder names under `steamapps/common/` Steam uses for this app.
    /// United Offensive installs into Call of Duty's folder.
    pub steam_install_dirs: &'static [&'static str],
    /// Data folders the copy holds, in load order.
    pub data_folders: &'static [&'static DataFolder],
    /// The data folder whose archives identify this game in an install.
    pub identifying_folder: &'static DataFolder,
    /// Executable names that also identify an install (names only).
    pub executables: &'static [&'static str],
    /// The text file holding this game's `ExtVersion=` line.
    pub version_file: &'static str,
    /// The version the Rust engine targets.
    pub target_version: &'static str,
    /// Small text files copied when present (paths relative to the install).
    pub text_files: &'static [&'static str],
}

/// A folder of data archives, and the rules for the names it may hold.
#[derive(Debug)]
pub struct DataFolder {
    /// The name in the copy: always lowercase. Installs spell it either way
    /// (Call of Duty uses `Main`), so it is looked up without regard to case.
    pub name: &'static str,
    pub rules: &'static [ArchiveRule],
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ArchiveKind {
    /// The game's main content.
    Base,
    /// Text and speech for one language: `localized_<language>_...`.
    Localized,
    /// Extra maps (United Offensive's map pack).
    MapPack,
}

/// How the number at the end of an archive's name is written.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum IndexStyle {
    /// One digit: `localized_english_pak3`.
    Digit,
    /// One digit or lowercase letter: `pak9`, `paka`, `pakb`.
    DigitOrLetter,
    /// Two digits: `iw_07`, `pakuo03`.
    TwoDigits,
}

impl IndexStyle {
    fn matches(self, index: &str) -> bool {
        let bytes = index.as_bytes();
        match self {
            IndexStyle::Digit => bytes.len() == 1 && bytes[0].is_ascii_digit(),
            IndexStyle::DigitOrLetter => {
                bytes.len() == 1 && (bytes[0].is_ascii_digit() || bytes[0].is_ascii_lowercase())
            }
            IndexStyle::TwoDigits => bytes.len() == 2 && bytes.iter().all(u8::is_ascii_digit),
        }
    }
}

/// One family of archive names, such as `iw_00.iwd` .. `iw_15.iwd`.
#[derive(Debug)]
pub struct ArchiveRule {
    pub kind: ArchiveKind,
    /// The name before the number (after `localized_<language>_` for a
    /// localized rule): `pak`, `pakuo`, `iw_`, `iw`, `uomappack`.
    pub stem: &'static str,
    pub index: IndexStyle,
    /// File extension, with the dot.
    pub extension: &'static str,
    /// Numbers present after the last official patch. For a localized rule
    /// these are the English set (the only one checked so far); other
    /// languages are checked for gaps instead.
    pub expected: &'static [&'static str],
    /// Whether the game can't run without this family.
    pub required: bool,
}

impl ArchiveRule {
    /// The file name for `index` (and `language`, for a localized rule).
    pub fn file_name(&self, language: Option<&str>, index: &str) -> String {
        match (self.kind, language) {
            (ArchiveKind::Localized, Some(language)) => format!(
                "localized_{language}_{}{index}{}",
                self.stem, self.extension
            ),
            _ => format!("{}{index}{}", self.stem, self.extension),
        }
    }
}

/// An archive name that matched a rule.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ArchiveName {
    pub kind: ArchiveKind,
    /// Index of the rule in its folder's `rules`.
    pub rule: usize,
    /// Lowercase language for a localized archive.
    pub language: Option<String>,
    pub index: String,
}

/// Is `name` a plain file name: no folder separators, not `.` or `..`, no
/// drive letter or control characters?
pub fn is_plain_file_name(name: &str) -> bool {
    !name.is_empty()
        && name != "."
        && name != ".."
        && !name.contains(['/', '\\', ':', '\0'])
        && !name.chars().any(char::is_control)
}

impl DataFolder {
    /// Matches a file name (any case) against this folder's rules.
    pub fn match_archive(&self, file_name: &str) -> Option<ArchiveName> {
        if !is_plain_file_name(file_name) || !file_name.is_ascii() {
            return None;
        }
        let lower = file_name.to_ascii_lowercase();
        for (rule_index, rule) in self.rules.iter().enumerate() {
            let Some(stem_and_index) = lower.strip_suffix(rule.extension) else {
                continue;
            };
            let (language, rest) = if rule.kind == ArchiveKind::Localized {
                let Some(after) = stem_and_index.strip_prefix("localized_") else {
                    continue;
                };
                let Some((language, rest)) = after.split_once('_') else {
                    continue;
                };
                if language.is_empty() || !language.bytes().all(|b| b.is_ascii_lowercase()) {
                    continue;
                }
                (Some(language.to_string()), rest)
            } else {
                (None, stem_and_index)
            };
            let Some(index) = rest.strip_prefix(rule.stem) else {
                continue;
            };
            if rule.index.matches(index) {
                return Some(ArchiveName {
                    kind: rule.kind,
                    rule: rule_index,
                    language,
                    index: index.to_string(),
                });
            }
        }
        None
    }
}

const DIGITS_0_TO_5: &[&str] = &["0", "1", "2", "3", "4", "5"];
const PAK_0_TO_B: &[&str] = &["0", "1", "2", "3", "4", "5", "6", "7", "8", "9", "a", "b"];
const TWO_DIGITS_0_TO_15: &[&str] = &[
    "00", "01", "02", "03", "04", "05", "06", "07", "08", "09", "10", "11", "12", "13", "14", "15",
];
const TWO_DIGITS_0_TO_11: &[&str] = &[
    "00", "01", "02", "03", "04", "05", "06", "07", "08", "09", "10", "11",
];
const TWO_DIGITS_0_TO_7: &[&str] = &["00", "01", "02", "03", "04", "05", "06", "07"];
const TWO_DIGITS_0_TO_2: &[&str] = &["00", "01", "02"];
const TWO_DIGITS_0: &[&str] = &["00"];

/// Call of Duty's `Main` folder, also loaded by United Offensive.
pub static COD_MAIN: DataFolder = DataFolder {
    name: "main",
    rules: &[
        ArchiveRule {
            kind: ArchiveKind::Base,
            stem: "pak",
            index: IndexStyle::DigitOrLetter,
            extension: ".pk3",
            expected: PAK_0_TO_B,
            required: true,
        },
        ArchiveRule {
            kind: ArchiveKind::Localized,
            stem: "pak",
            index: IndexStyle::Digit,
            extension: ".pk3",
            expected: DIGITS_0_TO_5,
            required: true,
        },
    ],
};

/// United Offensive's own `uo` folder.
pub static UO_FOLDER: DataFolder = DataFolder {
    name: "uo",
    rules: &[
        ArchiveRule {
            kind: ArchiveKind::Base,
            stem: "pakuo",
            index: IndexStyle::TwoDigits,
            extension: ".pk3",
            expected: TWO_DIGITS_0_TO_7,
            required: true,
        },
        ArchiveRule {
            kind: ArchiveKind::Localized,
            stem: "pakuo",
            index: IndexStyle::TwoDigits,
            extension: ".pk3",
            expected: TWO_DIGITS_0_TO_2,
            required: true,
        },
        ArchiveRule {
            kind: ArchiveKind::MapPack,
            stem: "uomappack",
            index: IndexStyle::TwoDigits,
            extension: ".pk3",
            expected: TWO_DIGITS_0,
            required: false,
        },
    ],
};

/// Call of Duty 2's `main` folder.
pub static COD2_MAIN: DataFolder = DataFolder {
    name: "main",
    rules: &[
        ArchiveRule {
            kind: ArchiveKind::Base,
            stem: "iw_",
            index: IndexStyle::TwoDigits,
            extension: ".iwd",
            expected: TWO_DIGITS_0_TO_15,
            required: true,
        },
        ArchiveRule {
            kind: ArchiveKind::Localized,
            stem: "iw",
            index: IndexStyle::TwoDigits,
            extension: ".iwd",
            expected: TWO_DIGITS_0_TO_11,
            required: true,
        },
    ],
};

pub static COD: Game = Game {
    id: GameId::Cod,
    key: "cod",
    name: "Call of Duty",
    steam_app_id: 2620,
    steam_install_dirs: &["Call of Duty"],
    data_folders: &[&COD_MAIN],
    identifying_folder: &COD_MAIN,
    executables: &[
        "CoDSP.exe",
        "CoDMP.exe",
        "Call of Duty SP.app",
        "Call of Duty MP.app",
    ],
    version_file: "version.inf",
    target_version: "1.5",
    text_files: &["version.inf", "localization.txt"],
};

pub static COD_UO: Game = Game {
    id: GameId::CodUo,
    key: "coduo",
    name: "Call of Duty: United Offensive",
    steam_app_id: 2640,
    steam_install_dirs: &["Call of Duty"],
    data_folders: &[&COD_MAIN, &UO_FOLDER],
    identifying_folder: &UO_FOLDER,
    executables: &[
        "CoDUOSP.exe",
        "CoDUOMP.exe",
        "CoD United Offensive SP.app",
        "CoD United Offensive MP.app",
    ],
    version_file: "uo/version.inf",
    target_version: "1.51",
    text_files: &["version.inf", "localization.txt", "uo/version.inf"],
};

pub static COD2: Game = Game {
    id: GameId::Cod2,
    key: "cod2",
    name: "Call of Duty 2",
    steam_app_id: 2630,
    steam_install_dirs: &["Call of Duty 2"],
    data_folders: &[&COD2_MAIN],
    identifying_folder: &COD2_MAIN,
    executables: &["CoD2SP_s.exe", "CoD2MP_s.exe", "Call of Duty 2.app"],
    version_file: "version.inf",
    target_version: "1.3",
    text_files: &["version.inf", "localization.txt"],
};

#[cfg(test)]
mod tests {
    use super::*;

    fn kind_of(folder: &DataFolder, name: &str) -> Option<(ArchiveKind, Option<String>, String)> {
        folder
            .match_archive(name)
            .map(|m| (m.kind, m.language, m.index))
    }

    #[test]
    fn keys_round_trip() {
        for id in GameId::ALL {
            assert_eq!(GameId::from_key(id.key()), Some(id));
            assert_eq!(id.game().id, id);
        }
        assert_eq!(GameId::from_key("CoDUO"), Some(GameId::CodUo));
        assert_eq!(GameId::from_key("cod4"), None);
    }

    #[test]
    fn cod_names() {
        assert_eq!(
            kind_of(&COD_MAIN, "pak0.pk3"),
            Some((ArchiveKind::Base, None, "0".into()))
        );
        assert_eq!(
            kind_of(&COD_MAIN, "PAKB.PK3"),
            Some((ArchiveKind::Base, None, "b".into()))
        );
        assert_eq!(
            kind_of(&COD_MAIN, "localized_english_pak5.pk3"),
            Some((ArchiveKind::Localized, Some("english".into()), "5".into()))
        );
        // Mods and maps dropped into Main are not the game's own archives.
        for other in [
            "zzz_mod.pk3",
            "pak_custom.pk3",
            "pak10.pk3",
            "mp_custommap.pk3",
            "localized_english_pak0_fix.pk3",
            "pak0.pk3.bak",
            "pak0.iwd",
            "localized__pak0.pk3",
            "game_mp_x86.dll",
        ] {
            assert_eq!(COD_MAIN.match_archive(other), None, "{other}");
        }
    }

    #[test]
    fn uo_names() {
        assert_eq!(
            kind_of(&UO_FOLDER, "pakuo07.pk3"),
            Some((ArchiveKind::Base, None, "07".into()))
        );
        assert_eq!(
            kind_of(&UO_FOLDER, "localized_english_pakuo02.pk3"),
            Some((ArchiveKind::Localized, Some("english".into()), "02".into()))
        );
        assert_eq!(
            kind_of(&UO_FOLDER, "uomappack00.pk3"),
            Some((ArchiveKind::MapPack, None, "00".into()))
        );
        assert_eq!(UO_FOLDER.match_archive("pak0.pk3"), None);
        assert_eq!(COD_MAIN.match_archive("pakuo00.pk3"), None);
        assert_eq!(
            COD_MAIN.match_archive("localized_english_pakuo00.pk3"),
            None
        );
    }

    #[test]
    fn cod2_names() {
        assert_eq!(
            kind_of(&COD2_MAIN, "iw_15.iwd"),
            Some((ArchiveKind::Base, None, "15".into()))
        );
        assert_eq!(
            kind_of(&COD2_MAIN, "localized_french_iw03.iwd"),
            Some((ArchiveKind::Localized, Some("french".into()), "03".into()))
        );
        assert_eq!(COD2_MAIN.match_archive("iw_1.iwd"), None);
        assert_eq!(COD2_MAIN.match_archive("iw_00.pk3"), None);
        assert_eq!(COD2_MAIN.match_archive("mss32.dll"), None);
    }

    #[test]
    fn rule_file_names() {
        let localized = &COD2_MAIN.rules[1];
        assert_eq!(
            localized.file_name(Some("english"), "07"),
            "localized_english_iw07.iwd"
        );
        assert_eq!(COD_MAIN.rules[0].file_name(None, "a"), "paka.pk3");
    }

    #[test]
    fn plain_file_names() {
        assert!(is_plain_file_name("pak0.pk3"));
        for bad in ["", ".", "..", "a/b", "a\\b", "c:x", "a\nb"] {
            assert!(!is_plain_file_name(bad), "{bad:?}");
        }
        assert_eq!(COD_MAIN.match_archive("../pak0.pk3"), None);
    }

    #[test]
    fn expected_sets_match_the_owner_installs() {
        // The owner's installs (CoD 1.5 + UO 1.51b, CoD2 1.3) hold exactly
        // these numbers.
        assert_eq!(COD_MAIN.rules[0].expected.len(), 12);
        assert_eq!(COD_MAIN.rules[1].expected.len(), 6);
        assert_eq!(UO_FOLDER.rules[0].expected.len(), 8);
        assert_eq!(UO_FOLDER.rules[1].expected.len(), 3);
        assert_eq!(COD2_MAIN.rules[0].expected.len(), 16);
        assert_eq!(COD2_MAIN.rules[1].expected.len(), 12);
    }
}
