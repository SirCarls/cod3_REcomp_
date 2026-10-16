//! Finds the player's own Call of Duty (2003), Call of Duty: United
//! Offensive and Call of Duty 2 installs (from Steam on Windows, Linux or
//! macOS, or any folder) and makes a **new copy** of each for the Rust
//! engine.
//!
//! The new copy holds only the game's data archives (`.pk3` for Call of Duty
//! and United Offensive, `.iwd` for Call of Duty 2) and its small version
//! text files. The Rust engine replaces the game's program, so no
//! executable, DLL, PunkBuster or Miles file is ever read, copied, hashed or
//! run. Steam's DRM lives only in the executables, so it never comes into it.
//! No registry key other than Steam's install path is read, and no player
//! profile, CD key or config file is touched.
//!
//! - [`games`]: the supported games, their Steam apps and archive names;
//! - [`steam`]: finding Steam, its libraries and the games in them;
//! - [`install`]: recognizing the games in any folder;
//! - [`zip`]: checking an archive is a whole ZIP file without unpacking it;
//! - [`sha256`]: streaming SHA-256;
//! - [`copy`]: making and checking the copy; [`manifest`]: its `newcopy.toml`;
//! - [`vdf`]: Valve's KeyValues text format.
//!
//! How it works for players: `docs/NEW_COPY.md`.

#![forbid(unsafe_code)]

pub mod copy;
pub mod games;
pub mod install;
pub mod manifest;
pub mod paths;
pub mod sha256;
pub mod steam;
pub mod testkit;
pub mod vdf;
pub mod zip;

pub use copy::{check_copy, copies_in, make_copy, MakeError, MakeRequest, MakeResult, SourceKind};
pub use games::{Game, GameId};
pub use install::{recognize, GameInstall};
pub use steam::{SteamInstall, SteamRoot, SteamScan};

use std::path::Path;

/// One place a game was found.
#[derive(Debug, Clone)]
pub struct Found {
    pub install: GameInstall,
    pub source_kind: SourceKind,
    /// The Steam app this game was installed as, when Steam lists it.
    pub steam_app_id: Option<u32>,
    /// Every Steam app whose manifest names this folder. Call of Duty's
    /// archives can arrive with United Offensive (app 2640 shares Call of
    /// Duty's content depot) without app 2620 being installed.
    pub steam_folder_apps: Vec<u32>,
    /// Proton/Wine prefix of that app (Linux; information only).
    pub proton_prefix: Option<std::path::PathBuf>,
}

impl Found {
    /// Every game found by a Steam scan, one entry per game.
    pub fn from_steam(scan: &SteamScan) -> Vec<Found> {
        let mut found = Vec::new();
        for steam_install in &scan.installs {
            for install in &steam_install.games {
                let app_id = install.game.game().steam_app_id;
                let listed = steam_install.app_ids.contains(&app_id);
                let proton_prefix = steam_install
                    .proton_prefixes
                    .iter()
                    .find(|p| {
                        p.parent()
                            .and_then(Path::file_name)
                            .is_some_and(|n| n.to_str() == Some(&app_id.to_string()))
                    })
                    .cloned();
                found.push(Found {
                    install: install.clone(),
                    source_kind: SourceKind::Steam,
                    steam_app_id: listed.then_some(app_id),
                    steam_folder_apps: steam_install.app_ids.clone(),
                    proton_prefix,
                });
            }
        }
        found
    }

    /// Every game recognized in a folder the player named.
    pub fn from_folder(dir: &Path) -> Vec<Found> {
        recognize(dir)
            .into_iter()
            .map(|install| Found {
                install,
                source_kind: SourceKind::Folder,
                steam_app_id: None,
                steam_folder_apps: Vec::new(),
                proton_prefix: None,
            })
            .collect()
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::testkit::{fake_install, write_file, TempDir};
    use std::fs;

    /// Steam with only United Offensive (app 2640) installed: Call of
    /// Duty's base archives come with it, its language archives don't.
    #[test]
    fn united_offensive_alone_in_steam() {
        let tmp = TempDir::new("lib").unwrap();
        let folder = tmp.path().join("steamapps/common/Call of Duty");
        fake_install(&folder, "coduo").unwrap();
        for i in 0..=5 {
            fs::remove_file(folder.join(format!("Main/localized_english_pak{i}.pk3"))).unwrap();
        }
        write_file(
            tmp.path(),
            "steamapps/appmanifest_2640.acf",
            b"\"AppState\" { \"appid\" \"2640\" \"installdir\" \"Call of Duty\" }",
        )
        .unwrap();
        let roots = vec![SteamRoot {
            path: paths::canonical(tmp.path()).unwrap(),
            found_by: "test".into(),
        }];
        let found = Found::from_steam(&steam::scan(&roots));
        assert_eq!(found.len(), 2);
        let cod = &found[0];
        assert_eq!(cod.install.game, GameId::Cod);
        assert_eq!(cod.steam_app_id, None);
        assert_eq!(cod.steam_folder_apps, vec![2640]);
        let uo = &found[1];
        assert_eq!(uo.steam_app_id, Some(2640));
        let missing = uo.install.missing(Some("english"));
        assert_eq!(missing.len(), 6, "{missing:?}");
        assert!(missing
            .iter()
            .all(|m| m.starts_with("main/localized_english_pak")));
    }
}
