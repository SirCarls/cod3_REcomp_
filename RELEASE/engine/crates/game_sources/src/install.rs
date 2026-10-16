//! Recognizes the games in a folder: which archives are there, which are
//! missing, the version and the languages.
//!
//! Only the folder listings, the archives' sizes and the small version text
//! files are read. Executables are recognized by name in the listing and
//! never opened.

use crate::games::{ArchiveKind, ArchiveName, DataFolder, GameId, IndexStyle};
use crate::paths::{child_ignoring_case, path_ignoring_case};
use crate::vdf::read_text_limited;
use std::collections::BTreeSet;
use std::fs;
use std::path::{Path, PathBuf};

/// Largest text file read or copied (`version.inf`, `localization.txt`).
pub const MAX_TEXT_FILE_BYTES: u64 = 1 << 20;

/// One game found in a folder.
#[derive(Debug, Clone)]
pub struct GameInstall {
    pub game: GameId,
    /// The install folder (as given or found; not resolved).
    pub root: PathBuf,
    /// `ExtVersion` from the game's `version.inf`, if present.
    pub version: Option<String>,
    /// Languages of the localized archives, lowercase, sorted.
    pub languages: Vec<String>,
    /// Every archive of this game found, sorted by its path in the copy.
    pub archives: Vec<FoundArchive>,
    /// Text files found that the copy takes along.
    pub text_files: Vec<FoundFile>,
    /// Executable names seen in the folder listing (never opened).
    pub executables_seen: Vec<String>,
    /// Oddities worth telling the player (such as two names that differ only by case).
    pub warnings: Vec<String>,
}

/// An archive found in an install.
#[derive(Debug, Clone)]
pub struct FoundArchive {
    pub source: PathBuf,
    /// Path in the copy: lowercase folder and file name, `/`-separated.
    pub copy_path: String,
    pub folder: &'static DataFolder,
    pub name: ArchiveName,
    pub size: u64,
}

/// A small text file found in an install.
#[derive(Debug, Clone)]
pub struct FoundFile {
    pub source: PathBuf,
    pub copy_path: String,
    pub size: u64,
}

impl GameInstall {
    /// The archives a copy in `language` takes: every base and map pack
    /// archive, and the localized archives of that language only.
    pub fn archives_for(&self, language: Option<&str>) -> Vec<&FoundArchive> {
        self.archives
            .iter()
            .filter(|a| match &a.name.language {
                Some(l) => Some(l.as_str()) == language,
                None => true,
            })
            .collect()
    }

    fn has(&self, folder: &DataFolder, rule: usize, language: Option<&str>, index: &str) -> bool {
        self.archives.iter().any(|a| {
            std::ptr::eq(a.folder, folder)
                && a.name.rule == rule
                && a.name.language.as_deref() == language
                && a.name.index == index
        })
    }

    /// Required archives missing for a copy in `language` (paths as in the
    /// copy). With no language, localized archives are reported as one
    /// "any language" line per family.
    pub fn missing(&self, language: Option<&str>) -> Vec<String> {
        let mut missing = Vec::new();
        for folder in self.game.game().data_folders {
            for (rule_index, rule) in folder.rules.iter().enumerate() {
                if !rule.required {
                    continue;
                }
                if rule.kind != ArchiveKind::Localized {
                    for index in rule.expected {
                        if !self.has(folder, rule_index, None, index) {
                            missing.push(format!(
                                "{}/{}",
                                folder.name,
                                rule.file_name(None, index)
                            ));
                        }
                    }
                    continue;
                }
                let Some(language) = language else {
                    missing.push(format!(
                        "{}/{} (any language)",
                        folder.name,
                        rule.file_name(Some("<language>"), rule.expected[0])
                    ));
                    continue;
                };
                for index in expected_localized(self, folder, rule_index, language) {
                    if !self.has(folder, rule_index, Some(language), &index) {
                        missing.push(format!(
                            "{}/{}",
                            folder.name,
                            rule.file_name(Some(language), &index)
                        ));
                    }
                }
            }
        }
        missing
    }

    /// Missing archives for every language found (or for "any language"
    /// when none is), without repeats.
    pub fn missing_any(&self) -> Vec<String> {
        if self.languages.is_empty() {
            return self.missing(None);
        }
        let mut all = BTreeSet::new();
        for language in &self.languages {
            all.extend(self.missing(Some(language)));
        }
        all.into_iter().collect()
    }

    /// Archives found that the last official patch doesn't have (a newer or
    /// unofficial archive with an official-looking name). They are copied
    /// too; this is for information.
    pub fn unexpected(&self) -> Vec<&FoundArchive> {
        self.archives
            .iter()
            .filter(|a| {
                let rule = &a.folder.rules[a.name.rule];
                match a.name.language.as_deref() {
                    Some("english") | None => !rule.expected.contains(&a.name.index.as_str()),
                    Some(_) => false,
                }
            })
            .collect()
    }

    /// Total size of the archives a copy in `language` takes.
    pub fn archive_bytes(&self, language: Option<&str>) -> u64 {
        self.archives_for(language).iter().map(|a| a.size).sum()
    }
}

/// The numbers a localized family should have in `language`: the known
/// English set, or for other languages every number from the first up to
/// the highest one found (so gaps show).
fn expected_localized(
    install: &GameInstall,
    folder: &DataFolder,
    rule_index: usize,
    language: &str,
) -> Vec<String> {
    let rule = &folder.rules[rule_index];
    if language == "english" {
        return rule.expected.iter().map(|s| s.to_string()).collect();
    }
    let highest = install
        .archives
        .iter()
        .filter(|a| {
            std::ptr::eq(a.folder, folder)
                && a.name.rule == rule_index
                && a.name.language.as_deref() == Some(language)
        })
        .filter_map(|a| a.name.index.parse::<u32>().ok())
        .max();
    let width = match rule.index {
        IndexStyle::TwoDigits => 2,
        IndexStyle::Digit | IndexStyle::DigitOrLetter => 1,
    };
    match highest {
        Some(highest) => (0..=highest).map(|i| format!("{i:0width$}")).collect(),
        None => vec![rule.expected[0].to_string()],
    }
}

/// Reads `ExtVersion=` from a `version.inf` (an INI file).
pub fn parse_version_inf(text: &str) -> Option<String> {
    text.lines().find_map(|line| {
        let (key, value) = line.split_once('=')?;
        if key.trim().eq_ignore_ascii_case("ExtVersion") {
            let value = value.trim();
            (!value.is_empty()).then(|| value.to_string())
        } else {
            None
        }
    })
}

/// Names of the regular files in `dir` (symlinks to files included), sorted.
fn file_names(dir: &Path) -> Vec<String> {
    let Ok(entries) = fs::read_dir(dir) else {
        return Vec::new();
    };
    let mut names: Vec<String> = entries
        .filter_map(Result::ok)
        .filter_map(|e| e.file_name().into_string().ok())
        .collect();
    names.sort();
    names
}

/// Recognizes every supported game in `dir`.
pub fn recognize(dir: &Path) -> Vec<GameInstall> {
    recognize_with(dir, &[])
}

/// Like [`recognize`], but always reports the games in `expected` (for
/// example the ones Steam says are installed there), even with nothing found.
pub fn recognize_with(dir: &Path, expected: &[GameId]) -> Vec<GameInstall> {
    let root_names = file_names(dir);
    let mut found = Vec::new();
    for id in GameId::ALL {
        let game = id.game();
        let executables_seen: Vec<String> = root_names
            .iter()
            .filter(|n| game.executables.iter().any(|e| e.eq_ignore_ascii_case(n)))
            .cloned()
            .collect();
        let mut install = GameInstall {
            game: id,
            root: dir.to_path_buf(),
            version: None,
            languages: Vec::new(),
            archives: Vec::new(),
            text_files: Vec::new(),
            executables_seen,
            warnings: Vec::new(),
        };
        let mut identified = false;
        for &folder in game.data_folders {
            let Some(folder_path) = child_ignoring_case(dir, folder.name) else {
                continue;
            };
            let mut seen_lowercase = BTreeSet::new();
            for name in file_names(&folder_path) {
                let Some(matched) = folder.match_archive(&name) else {
                    continue;
                };
                let source = folder_path.join(&name);
                // Follows symlinks: a linked archive counts if it leads to a file.
                let Ok(metadata) = fs::metadata(&source) else {
                    continue;
                };
                if !metadata.is_file() {
                    continue;
                }
                let lower = name.to_ascii_lowercase();
                if !seen_lowercase.insert(lower.clone()) {
                    install.warnings.push(format!(
                        "{}/{name} differs from another file only by case; it is skipped",
                        folder.name
                    ));
                    continue;
                }
                if std::ptr::eq(folder, game.identifying_folder) {
                    identified = true;
                }
                install.archives.push(FoundArchive {
                    source,
                    copy_path: format!("{}/{lower}", folder.name),
                    folder,
                    name: matched,
                    size: metadata.len(),
                });
            }
        }
        if !identified && install.executables_seen.is_empty() && !expected.contains(&id) {
            continue;
        }
        install
            .archives
            .sort_by(|a, b| a.copy_path.cmp(&b.copy_path));
        let languages: BTreeSet<String> = install
            .archives
            .iter()
            .filter_map(|a| a.name.language.clone())
            .collect();
        install.languages = languages.into_iter().collect();
        if let Some(path) = path_ignoring_case(dir, game.version_file) {
            if let Ok(text) = read_text_limited(&path, MAX_TEXT_FILE_BYTES) {
                install.version = parse_version_inf(&text);
            }
        }
        for relative in game.text_files {
            let Some(source) = path_ignoring_case(dir, relative) else {
                continue;
            };
            let Ok(metadata) = fs::metadata(&source) else {
                continue;
            };
            if metadata.is_file() && metadata.len() <= MAX_TEXT_FILE_BYTES {
                install.text_files.push(FoundFile {
                    source,
                    copy_path: relative.to_ascii_lowercase(),
                    size: metadata.len(),
                });
            }
        }
        found.push(install);
    }
    found
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::testkit::{fake_archive, fake_install, write_file, TempDir};

    fn by_game(installs: &[GameInstall], id: GameId) -> &GameInstall {
        installs.iter().find(|i| i.game == id).unwrap()
    }

    #[test]
    fn cod_and_uo_in_one_folder() {
        let tmp = TempDir::new("recognize").unwrap();
        fake_install(tmp.path(), "coduo").unwrap();
        let installs = recognize(tmp.path());
        let ids: Vec<GameId> = installs.iter().map(|i| i.game).collect();
        assert_eq!(ids, vec![GameId::Cod, GameId::CodUo]);

        let cod = by_game(&installs, GameId::Cod);
        assert_eq!(cod.version.as_deref(), Some("1.5"));
        assert_eq!(cod.languages, vec!["english"]);
        assert_eq!(cod.archives.len(), 18);
        assert!(cod.missing_any().is_empty(), "{:?}", cod.missing_any());
        assert_eq!(cod.executables_seen, vec!["CoDMP.exe", "CoDSP.exe"]);
        assert_eq!(cod.archives[0].copy_path, "main/localized_english_pak0.pk3");

        let uo = by_game(&installs, GameId::CodUo);
        assert_eq!(uo.version.as_deref(), Some("1.51b"));
        assert_eq!(uo.archives.len(), 18 + 12);
        assert!(uo.missing(Some("english")).is_empty());
        assert!(uo.unexpected().is_empty());
        let texts: Vec<&str> = uo.text_files.iter().map(|t| t.copy_path.as_str()).collect();
        assert_eq!(
            texts,
            vec!["version.inf", "localization.txt", "uo/version.inf"]
        );
        // No executable or DLL is ever an archive or a text file to copy.
        for archive in &uo.archives {
            assert!(archive.copy_path.ends_with(".pk3"));
        }
    }

    #[test]
    fn cod2_with_mixed_case_and_missing_archives() {
        let tmp = TempDir::new("recognize").unwrap();
        for i in [0, 1, 2, 14] {
            let name = format!("IW_{i:02}.IWD");
            write_file(tmp.path(), &format!("MAIN/{name}"), &fake_archive(&name)).unwrap();
        }
        write_file(tmp.path(), "MAIN/localized_german_iw00.iwd", b"x").unwrap();
        write_file(tmp.path(), "MAIN/localized_german_iw02.iwd", b"x").unwrap();
        write_file(tmp.path(), "Version.INF", b"ExtVersion = 1.0 \n").unwrap();
        let installs = recognize(tmp.path());
        assert_eq!(installs.len(), 1);
        let cod2 = &installs[0];
        assert_eq!(cod2.game, GameId::Cod2);
        assert_eq!(cod2.version.as_deref(), Some("1.0"));
        assert_eq!(cod2.languages, vec!["german"]);
        assert_eq!(cod2.archives[0].copy_path, "main/iw_00.iwd");
        let missing = cod2.missing(Some("german"));
        assert!(missing.contains(&"main/iw_03.iwd".to_string()));
        assert!(missing.contains(&"main/iw_15.iwd".to_string()));
        assert!(!missing.contains(&"main/iw_14.iwd".to_string()));
        // German is checked for gaps: 01 is missing between 00 and 02.
        assert!(missing.contains(&"main/localized_german_iw01.iwd".to_string()));
        assert!(!missing.contains(&"main/localized_german_iw03.iwd".to_string()));
        // English isn't there at all.
        assert!(cod2
            .missing(Some("english"))
            .contains(&"main/localized_english_iw00.iwd".to_string()));
    }

    #[test]
    fn executables_alone_identify_a_game() {
        // Like the staged folder holding executables but no archives.
        let tmp = TempDir::new("recognize").unwrap();
        write_file(tmp.path(), "CoDUOMP.exe", b"stub").unwrap();
        let installs = recognize(tmp.path());
        assert_eq!(installs.len(), 1);
        let uo = &installs[0];
        assert_eq!(uo.game, GameId::CodUo);
        assert!(uo.archives.is_empty());
        let missing = uo.missing_any();
        assert!(missing.contains(&"uo/pakuo00.pk3".to_string()));
        assert!(missing.contains(&"main/pak0.pk3".to_string()));
        assert!(missing
            .iter()
            .any(|m| m == "uo/localized_<language>_pakuo00.pk3 (any language)"));
    }

    #[test]
    fn empty_folder_has_no_games_unless_expected() {
        let tmp = TempDir::new("recognize").unwrap();
        assert!(recognize(tmp.path()).is_empty());
        let installs = recognize_with(tmp.path(), &[GameId::Cod2]);
        assert_eq!(installs.len(), 1);
        assert_eq!(installs[0].missing(None).len(), 16 + 1);
    }

    #[test]
    fn unexpected_archives_are_reported() {
        let tmp = TempDir::new("recognize").unwrap();
        fake_install(tmp.path(), "cod2").unwrap();
        write_file(tmp.path(), "main/iw_16.iwd", &fake_archive("iw_16")).unwrap();
        let installs = recognize(tmp.path());
        let unexpected: Vec<&str> = installs[0]
            .unexpected()
            .iter()
            .map(|a| a.copy_path.as_str())
            .collect();
        assert_eq!(unexpected, vec!["main/iw_16.iwd"]);
    }

    #[test]
    fn version_inf_parsing() {
        assert_eq!(
            parse_version_inf("[Version Info]\r\nExtVersion=1.51b\r\nIntVersion=2.3\r\n"),
            Some("1.51b".into())
        );
        assert_eq!(parse_version_inf("IntVersion=2.3"), None);
        assert_eq!(parse_version_inf("ExtVersion=\n"), None);
    }
}
