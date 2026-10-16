//! Finds Steam and the games it installed, on Windows, Linux and macOS.
//!
//! Steam keeps a list of its library folders in
//! `steamapps/libraryfolders.vdf`, and one `steamapps/appmanifest_<id>.acf`
//! per installed app naming its folder under `steamapps/common/`. Both are
//! text files read with [`crate::vdf`]. Nothing else of Steam's is read.
//!
//! On Linux a game installed through Proton lives in the same
//! `steamapps/common/<folder>` as on Windows, so it is found the same way.
//! Its Wine prefix (`steamapps/compatdata/<id>/pfx`) is reported for
//! information only and never looked inside, and so are the Proton builds
//! Steam has (Valve's, and custom ones such as GE-Proton or Proton-CachyOS
//! in `compatibilitytools.d`): the original Windows games need one to run on
//! Linux (a CB copy, for cross-play tests against the native Rust build);
//! the Rust build needs none.
//!
//! On Windows the Steam folder comes from the registry, through Windows' own
//! `reg.exe` (the only program this crate ever starts). Only Steam's install
//! path values are queried.

use crate::games::{is_plain_file_name, GameId};
use crate::install::{recognize_with, GameInstall};
use crate::paths::{canonical, child_ignoring_case};
use crate::vdf::{self, Block, Value};
use std::ffi::OsString;
use std::path::{Path, PathBuf};
use std::process::Command;

/// Environment variable naming the Steam folder(s) to use instead of
/// looking for them (several are separated like `PATH`).
pub const STEAM_ENV: &str = "GAME_SOURCES_STEAM";

/// A Steam folder (the Steam install, or any library folder).
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct SteamRoot {
    pub path: PathBuf,
    /// How it was found, for messages: `--steam`, `$HOME/.steam/steam`, ...
    pub found_by: String,
}

/// A folder in a Steam library that holds one or more of the games.
#[derive(Debug, Clone)]
pub struct SteamInstall {
    /// `steamapps/common/<installdir>`.
    pub path: PathBuf,
    /// The library folder it is in.
    pub library: PathBuf,
    /// Apps whose manifest names this folder (United Offensive shares Call
    /// of Duty's folder, so there can be two). Empty when the folder was
    /// found by its usual name without a manifest.
    pub app_ids: Vec<u32>,
    /// Proton/Wine prefixes of those apps that exist (Linux; information only).
    pub proton_prefixes: Vec<PathBuf>,
    /// The games recognized in the folder.
    pub games: Vec<GameInstall>,
}

/// Who made a Proton build.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ProtonKind {
    /// Valve's, installed by Steam as an app (`steamapps/common/Proton 9.0`).
    Valve,
    /// A custom build in a `compatibilitytools.d` folder (GE-Proton,
    /// Proton-CachyOS, ...), installed by hand or by a system package.
    Custom,
}

/// A Proton build found on this machine (Linux; information only: it is
/// never started or looked inside beyond its folder listing).
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ProtonBuild {
    /// Its folder name: `GE-Proton10-25`, `proton-cachyos-10.0`, `Proton 9.0`.
    pub name: String,
    pub path: PathBuf,
    pub kind: ProtonKind,
}

/// System-wide folders Linux packages put custom Proton builds in.
pub const SYSTEM_COMPAT_TOOL_DIRS: [&str; 2] = [
    "/usr/share/steam/compatibilitytools.d",
    "/usr/local/share/steam/compatibilitytools.d",
];

/// Everything Steam-related that was found.
#[derive(Debug, Clone, Default)]
pub struct SteamScan {
    pub roots: Vec<SteamRoot>,
    pub libraries: Vec<PathBuf>,
    pub installs: Vec<SteamInstall>,
    /// Proton builds, sorted by name (empty on Windows).
    pub proton_builds: Vec<ProtonBuild>,
    /// Problems worth telling the player (unreadable files, missing folders).
    pub warnings: Vec<String>,
}

/// Steam folders worth checking on `os` (`std::env::consts::OS`), given a
/// way to read environment variables. Registry values are not included.
pub fn candidate_roots(os: &str, env: &dyn Fn(&str) -> Option<OsString>) -> Vec<SteamRoot> {
    let mut roots = Vec::new();
    let mut add = |path: PathBuf, found_by: &str| {
        roots.push(SteamRoot {
            path,
            found_by: found_by.to_string(),
        })
    };
    let home = env("HOME").filter(|h| !h.is_empty()).map(PathBuf::from);
    match os {
        "windows" => {
            for (var, fallback) in [
                ("ProgramFiles(x86)", r"C:\Program Files (x86)"),
                ("ProgramFiles", r"C:\Program Files"),
            ] {
                match env(var).filter(|v| !v.is_empty()) {
                    Some(dir) => add(PathBuf::from(dir).join("Steam"), &format!("%{var}%\\Steam")),
                    None => add(PathBuf::from(fallback).join("Steam"), fallback),
                }
            }
        }
        "macos" => {
            if let Some(home) = &home {
                add(
                    home.join("Library/Application Support/Steam"),
                    "$HOME/Library/Application Support/Steam",
                );
            }
        }
        _ => {
            if let Some(home) = &home {
                add(home.join(".steam/steam"), "$HOME/.steam/steam");
                add(home.join(".steam/root"), "$HOME/.steam/root");
                // Debian and Ubuntu's steam-installer package.
                add(
                    home.join(".steam/debian-installation"),
                    "$HOME/.steam/debian-installation",
                );
            }
            match env("XDG_DATA_HOME").filter(|v| !v.is_empty()) {
                Some(data) => add(PathBuf::from(data).join("Steam"), "$XDG_DATA_HOME/Steam"),
                None => {
                    if let Some(home) = &home {
                        add(home.join(".local/share/Steam"), "$HOME/.local/share/Steam");
                    }
                }
            }
            if let Some(home) = &home {
                let flatpak = home.join(".var/app/com.valvesoftware.Steam");
                add(flatpak.join(".local/share/Steam"), "Flatpak Steam");
                add(flatpak.join("data/Steam"), "Flatpak Steam");
                add(
                    home.join("snap/steam/common/.local/share/Steam"),
                    "Snap Steam",
                );
            }
        }
    }
    roots
}

/// Reads one value from `reg query` output, which looks like:
///
/// ```text
/// HKEY_CURRENT_USER\Software\Valve\Steam
///     SteamPath    REG_SZ    c:/program files (x86)/steam
/// ```
pub fn parse_reg_query(output: &str, value_name: &str) -> Option<String> {
    output.lines().find_map(|line| {
        let line = line.trim();
        let rest = line.get(value_name.len()..)?;
        if !line[..value_name.len()].eq_ignore_ascii_case(value_name)
            || !rest.starts_with(char::is_whitespace)
        {
            return None;
        }
        let rest = rest.trim_start();
        let after_type = rest.strip_prefix("REG_")?;
        let type_end = after_type.find(char::is_whitespace)?;
        let value_type = &after_type[..type_end];
        if value_type != "SZ" && value_type != "EXPAND_SZ" {
            return None;
        }
        let value = after_type[type_end..].trim();
        (!value.is_empty()).then(|| value.to_string())
    })
}

/// Steam's folder from the Windows registry. Returns nothing elsewhere or
/// when `reg.exe` fails.
fn registry_roots() -> Vec<SteamRoot> {
    if std::env::consts::OS != "windows" {
        return Vec::new();
    }
    // Windows' own reg.exe by full path, so a program of the same name
    // earlier in PATH is never started instead.
    let reg = std::env::var_os("SystemRoot")
        .map(|root| PathBuf::from(root).join("System32").join("reg.exe"))
        .filter(|path| path.is_file())
        .unwrap_or_else(|| PathBuf::from("reg.exe"));
    let queries = [
        (r"HKCU\Software\Valve\Steam", "SteamPath"),
        (r"HKLM\SOFTWARE\WOW6432Node\Valve\Steam", "InstallPath"),
        (r"HKLM\SOFTWARE\Valve\Steam", "InstallPath"),
    ];
    let mut roots = Vec::new();
    for (key, value) in queries {
        let Ok(output) = Command::new(&reg)
            .args(["query", key, "/v", value])
            .output()
        else {
            continue;
        };
        if !output.status.success() {
            continue;
        }
        let text = String::from_utf8_lossy(&output.stdout);
        if let Some(path) = parse_reg_query(&text, value) {
            roots.push(SteamRoot {
                path: PathBuf::from(path),
                found_by: format!("registry {key} {value}"),
            });
        }
    }
    roots
}

/// The Steam folders to search: `explicit` (from `--steam`) if given, else
/// those named by [`STEAM_ENV`], else the usual places for this system.
/// Only folders that exist are kept, resolved and without repeats.
pub fn find_roots(explicit: Option<&Path>) -> Vec<SteamRoot> {
    let mut candidates = Vec::new();
    if let Some(path) = explicit {
        candidates.push(SteamRoot {
            path: path.to_path_buf(),
            found_by: "--steam".into(),
        });
    } else if let Some(list) = std::env::var_os(STEAM_ENV).filter(|v| !v.is_empty()) {
        for path in std::env::split_paths(&list) {
            candidates.push(SteamRoot {
                path,
                found_by: STEAM_ENV.into(),
            });
        }
    } else {
        candidates.extend(registry_roots());
        candidates.extend(candidate_roots(std::env::consts::OS, &|name| {
            std::env::var_os(name)
        }));
    }
    let mut roots: Vec<SteamRoot> = Vec::new();
    for candidate in candidates {
        let Ok(path) = canonical(&candidate.path) else {
            continue;
        };
        if path.is_dir() && !roots.iter().any(|r| r.path == path) {
            roots.push(SteamRoot {
                path,
                found_by: candidate.found_by,
            });
        }
    }
    roots
}

/// Library paths listed in a parsed `libraryfolders.vdf`. Handles the
/// current format (`"0" { "path" "..." }`) and the old one (`"1" "..."`).
pub fn parse_library_folders(file: &Block) -> Vec<String> {
    let Some(folders) = file.get_block("libraryfolders") else {
        return Vec::new();
    };
    folders
        .entries
        .iter()
        .filter(|(key, _)| !key.is_empty() && key.bytes().all(|b| b.is_ascii_digit()))
        .filter_map(|(_, value)| match value {
            Value::String(path) => Some(path.clone()),
            Value::Block(block) => block.get_str("path").map(str::to_string),
        })
        .filter(|path| !path.is_empty())
        .collect()
}

/// What an app manifest says.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct AppManifest {
    pub app_id: u32,
    pub name: Option<String>,
    /// Folder name under `steamapps/common/`.
    pub install_dir: String,
}

/// Reads a parsed `appmanifest_<id>.acf`. The folder name must be a plain
/// name (no separators or `..`), since it is joined to a path.
pub fn parse_app_manifest(file: &Block) -> Result<AppManifest, String> {
    let state = file
        .get_block("AppState")
        .ok_or("no AppState block".to_string())?;
    let app_id = state
        .get_str("appid")
        .and_then(|id| id.trim().parse::<u32>().ok())
        .ok_or("no app id".to_string())?;
    let install_dir = state
        .get_str("installdir")
        .ok_or("no installdir".to_string())?
        .to_string();
    if !is_plain_file_name(&install_dir) {
        return Err(format!(
            "installdir {install_dir:?} is not a plain folder name"
        ));
    }
    Ok(AppManifest {
        app_id,
        name: state.get_str("name").map(str::to_string),
        install_dir,
    })
}

/// The library folders of a Steam folder: the folder itself, then those in
/// its `libraryfolders.vdf` files. Only existing folders, resolved.
pub fn library_folders(root: &Path, warnings: &mut Vec<String>) -> Vec<PathBuf> {
    let mut libraries = vec![root.to_path_buf()];
    let lists = [
        child_ignoring_case(root, "steamapps")
            .and_then(|dir| child_ignoring_case(&dir, "libraryfolders.vdf")),
        child_ignoring_case(root, "config")
            .and_then(|dir| child_ignoring_case(&dir, "libraryfolders.vdf")),
    ];
    for list in lists.into_iter().flatten() {
        match vdf::parse_file(&list) {
            Ok(file) => {
                for path in parse_library_folders(&file) {
                    match canonical(Path::new(&path)) {
                        Ok(path) if path.is_dir() => libraries.push(path),
                        _ => warnings.push(format!(
                            "{} lists the library {path}, which isn't there (a drive not connected?)",
                            list.display()
                        )),
                    }
                }
            }
            Err(e) => warnings.push(format!("{} could not be read: {e}", list.display())),
        }
    }
    let mut unique = Vec::new();
    for library in libraries {
        if !unique.contains(&library) {
            unique.push(library);
        }
    }
    unique
}

/// Searches every Steam folder in `roots` for the supported games.
pub fn scan(roots: &[SteamRoot]) -> SteamScan {
    let mut scan = SteamScan {
        roots: roots.to_vec(),
        ..SteamScan::default()
    };
    for root in roots {
        for library in library_folders(&root.path, &mut scan.warnings) {
            if !scan.libraries.contains(&library) {
                scan.libraries.push(library);
            }
        }
    }
    for library in scan.libraries.clone() {
        scan_library(&library, &mut scan);
    }
    let system_dirs: Vec<PathBuf> = if cfg!(target_os = "linux") {
        SYSTEM_COMPAT_TOOL_DIRS.iter().map(PathBuf::from).collect()
    } else {
        Vec::new()
    };
    scan.proton_builds = proton_builds(&scan.roots, &scan.libraries, &system_dirs);
    // Both copies of libraryfolders.vdf may name the same missing library.
    let mut seen = Vec::new();
    scan.warnings.retain(|w| {
        let new = !seen.contains(w);
        if new {
            seen.push(w.clone());
        }
        new
    });
    scan
}

fn scan_library(library: &Path, scan: &mut SteamScan) {
    let Some(steamapps) = child_ignoring_case(library, "steamapps") else {
        return;
    };
    let common = child_ignoring_case(&steamapps, "common");
    // Folder → app ids naming it. A Vec keeps the order stable.
    let mut folders: Vec<(PathBuf, Vec<u32>)> = Vec::new();
    let mut add_folder = |folder: PathBuf, app_id: Option<u32>| {
        let Ok(folder) = canonical(&folder) else {
            return;
        };
        match folders.iter_mut().find(|(f, _)| *f == folder) {
            Some((_, ids)) => {
                if let Some(id) = app_id {
                    if !ids.contains(&id) {
                        ids.push(id);
                    }
                }
            }
            None => folders.push((folder, app_id.into_iter().collect())),
        }
    };
    for id in GameId::ALL {
        let game = id.game();
        let manifest_name = format!("appmanifest_{}.acf", game.steam_app_id);
        if let Some(manifest_path) = child_ignoring_case(&steamapps, &manifest_name) {
            let manifest = vdf::parse_file(&manifest_path).and_then(|f| parse_app_manifest(&f));
            match manifest {
                Ok(manifest) if manifest.app_id == game.steam_app_id => {
                    let folder = common
                        .as_ref()
                        .and_then(|c| child_ignoring_case(c, &manifest.install_dir));
                    match folder {
                        Some(folder) => add_folder(folder, Some(game.steam_app_id)),
                        None => scan.warnings.push(format!(
                            "Steam lists {} (app {}) in {}, but its folder \"{}\" isn't there \
                             (still downloading?)",
                            game.name,
                            game.steam_app_id,
                            library.display(),
                            manifest.install_dir
                        )),
                    }
                    continue;
                }
                Ok(manifest) => scan.warnings.push(format!(
                    "{} names app {}, not {}",
                    manifest_path.display(),
                    manifest.app_id,
                    game.steam_app_id
                )),
                Err(e) => scan.warnings.push(format!(
                    "{} could not be read: {e}",
                    manifest_path.display()
                )),
            }
        }
        // No usable manifest: a library copied from elsewhere may still hold
        // the folder under its usual name.
        if let Some(common) = &common {
            for dir in game.steam_install_dirs {
                if let Some(folder) = child_ignoring_case(common, dir) {
                    add_folder(folder, None);
                }
            }
        }
    }
    for (folder, app_ids) in folders {
        if scan.installs.iter().any(|i| i.path == folder) {
            continue;
        }
        let expected: Vec<GameId> = GameId::ALL
            .into_iter()
            .filter(|id| app_ids.contains(&id.game().steam_app_id))
            .collect();
        let games = recognize_with(&folder, &expected);
        if games.is_empty() {
            continue;
        }
        let compatdata = child_ignoring_case(&steamapps, "compatdata");
        let proton_prefixes = app_ids
            .iter()
            .filter_map(|id| {
                let prefix = compatdata.as_ref()?.join(id.to_string()).join("pfx");
                prefix.is_dir().then_some(prefix)
            })
            .collect();
        scan.installs.push(SteamInstall {
            path: folder,
            library: library.to_path_buf(),
            app_ids,
            proton_prefixes,
            games,
        });
    }
}

/// The Proton builds in Steam's folders: custom ones in
/// `<steam>/compatibilitytools.d/` and in `extra_tool_dirs` (system
/// packages), Valve's in `<library>/steamapps/common/Proton*`. A folder
/// counts when it holds Proton's `proton` launcher script or a
/// `compatibilitytool.vdf`; only the folder listing is read.
pub fn proton_builds(
    roots: &[SteamRoot],
    libraries: &[PathBuf],
    extra_tool_dirs: &[PathBuf],
) -> Vec<ProtonBuild> {
    let mut found: Vec<ProtonBuild> = Vec::new();
    let mut add = |dir: &Path, kind: ProtonKind| {
        let looks_like_proton = |d: &Path| {
            child_ignoring_case(d, "proton").is_some_and(|p| p.is_file())
                || child_ignoring_case(d, "compatibilitytool.vdf").is_some_and(|p| p.is_file())
        };
        if !dir.is_dir() || !looks_like_proton(dir) {
            return;
        }
        let Ok(path) = canonical(dir) else {
            return;
        };
        if found.iter().any(|b| b.path == path) {
            return;
        }
        let name = dir
            .file_name()
            .map(|n| n.to_string_lossy().into_owned())
            .unwrap_or_default();
        found.push(ProtonBuild { name, path, kind });
    };
    let custom_dirs = roots
        .iter()
        .filter_map(|r| child_ignoring_case(&r.path, "compatibilitytools.d"))
        .chain(extra_tool_dirs.iter().cloned());
    for tools in custom_dirs {
        for dir in sorted_subfolders(&tools) {
            add(&dir, ProtonKind::Custom);
        }
    }
    for library in libraries {
        let Some(common) = child_ignoring_case(library, "steamapps")
            .and_then(|s| child_ignoring_case(&s, "common"))
        else {
            continue;
        };
        for dir in sorted_subfolders(&common) {
            let is_valve_proton = dir.file_name().is_some_and(|n| {
                n.to_string_lossy()
                    .to_ascii_lowercase()
                    .starts_with("proton")
            });
            if is_valve_proton {
                add(&dir, ProtonKind::Valve);
            }
        }
    }
    found.sort_by(|a, b| {
        a.name
            .to_ascii_lowercase()
            .cmp(&b.name.to_ascii_lowercase())
    });
    found
}

/// The folders directly inside `dir`, by name (none if it can't be read).
fn sorted_subfolders(dir: &Path) -> Vec<PathBuf> {
    let Ok(entries) = std::fs::read_dir(dir) else {
        return Vec::new();
    };
    let mut dirs: Vec<PathBuf> = entries
        .filter_map(|e| e.ok())
        .map(|e| e.path())
        .filter(|p| p.is_dir())
        .collect();
    dirs.sort();
    dirs
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::testkit::{fake_install, write_file, TempDir};
    use std::fs;

    fn no_env(_: &str) -> Option<OsString> {
        None
    }

    #[test]
    fn linux_candidates() {
        let env = |name: &str| match name {
            "HOME" => Some(OsString::from("/home/user")),
            _ => None,
        };
        let roots = candidate_roots("linux", &env);
        let paths: Vec<String> = roots
            .iter()
            .map(|r| r.path.to_string_lossy().replace('\\', "/"))
            .collect();
        for expected in [
            "/home/user/.steam/steam",
            "/home/user/.steam/root",
            "/home/user/.local/share/Steam",
            "/home/user/.var/app/com.valvesoftware.Steam/.local/share/Steam",
            "/home/user/.var/app/com.valvesoftware.Steam/data/Steam",
            "/home/user/snap/steam/common/.local/share/Steam",
        ] {
            assert!(
                paths.iter().any(|p| p == expected),
                "{expected} in {paths:?}"
            );
        }
        let with_xdg = |name: &str| match name {
            "HOME" => Some(OsString::from("/home/user")),
            "XDG_DATA_HOME" => Some(OsString::from("/data")),
            _ => None,
        };
        let roots = candidate_roots("linux", &with_xdg);
        assert!(roots
            .iter()
            .any(|r| r.path == Path::new("/data").join("Steam")));
        assert!(!roots
            .iter()
            .any(|r| r.path.ends_with(".local/share/Steam")
                && r.found_by == "$HOME/.local/share/Steam"));
        assert!(candidate_roots("linux", &no_env).is_empty());
    }

    #[test]
    fn windows_and_mac_candidates() {
        let roots = candidate_roots("windows", &no_env);
        assert_eq!(roots.len(), 2);
        assert!(roots[0]
            .path
            .to_string_lossy()
            .contains("Program Files (x86)"));
        let env = |name: &str| match name {
            "ProgramFiles(x86)" => Some(OsString::from(r"E:\Apps32")),
            _ => None,
        };
        assert!(candidate_roots("windows", &env)[0]
            .path
            .to_string_lossy()
            .starts_with(r"E:\Apps32"));
        let env = |name: &str| (name == "HOME").then(|| OsString::from("/Users/user"));
        let roots = candidate_roots("macos", &env);
        assert!(roots[0].path.ends_with("Library/Application Support/Steam"));
    }

    #[test]
    fn reg_query_output() {
        let hkcu = "\r\nHKEY_CURRENT_USER\\Software\\Valve\\Steam\r\n    SteamPath    REG_SZ    c:/program files (x86)/steam\r\n\r\n";
        assert_eq!(
            parse_reg_query(hkcu, "SteamPath").as_deref(),
            Some("c:/program files (x86)/steam")
        );
        let hklm = "HKEY_LOCAL_MACHINE\\SOFTWARE\\WOW6432Node\\Valve\\Steam\n    InstallPath    REG_SZ    D:\\Games\\Steam\n";
        assert_eq!(
            parse_reg_query(hklm, "installpath").as_deref(),
            Some("D:\\Games\\Steam")
        );
        // A different value that starts the same, a DWORD, and an error.
        assert_eq!(
            parse_reg_query("    SteamPathX    REG_SZ    x\n", "SteamPath"),
            None
        );
        assert_eq!(
            parse_reg_query("    SteamPath    REG_DWORD    0x1\n", "SteamPath"),
            None
        );
        assert_eq!(
            parse_reg_query(
                "ERROR: The system was unable to find the specified registry key or value.",
                "SteamPath"
            ),
            None
        );
    }

    #[test]
    fn library_folders_new_and_old_formats() {
        let new = vdf::parse(
            "\"libraryfolders\"\n{\n\t\"0\"\n\t{\n\t\t\"path\"\t\t\"C:\\\\Program Files (x86)\\\\Steam\"\n\t\t\"apps\" { \"2620\" \"1\" }\n\t}\n\t\"1\"\n\t{\n\t\t\"path\"\t\t\"D:\\\\SteamLibrary\"\n\t}\n}\n",
        )
        .unwrap();
        assert_eq!(
            parse_library_folders(&new),
            vec![r"C:\Program Files (x86)\Steam", r"D:\SteamLibrary"]
        );
        let old = vdf::parse(
            "\"LibraryFolders\"\n{\n\t\"TimeNextStatsReport\"\t\t\"1234\"\n\t\"ContentStatsID\"\t\t\"-5\"\n\t\"1\"\t\t\"E:\\\\Games\\\\Steam\"\n\t\"2\"\t\t\"/srv/steam\"\n}\n",
        )
        .unwrap();
        assert_eq!(
            parse_library_folders(&old),
            vec![r"E:\Games\Steam", "/srv/steam"]
        );
    }

    #[test]
    fn app_manifest() {
        let file = vdf::parse(
            "\"AppState\"\n{\n\t\"appid\"\t\t\"2630\"\n\t\"name\"\t\t\"Call of Duty 2\"\n\t\"installdir\"\t\t\"Call of Duty 2\"\n}\n",
        )
        .unwrap();
        let manifest = parse_app_manifest(&file).unwrap();
        assert_eq!(manifest.app_id, 2630);
        assert_eq!(manifest.install_dir, "Call of Duty 2");
        for bad in ["..", "a/b", "..\\\\x", ""] {
            let text = format!("\"AppState\" {{ \"appid\" \"2630\" \"installdir\" \"{bad}\" }}");
            assert!(
                parse_app_manifest(&vdf::parse(&text).unwrap()).is_err(),
                "{bad}"
            );
        }
    }

    /// A Steam folder with a second library, like a real setup: CoD + UO in
    /// the main library (two manifests, one folder), CoD2 in the other one
    /// with an old-style `SteamApps` folder and a Proton prefix.
    #[test]
    fn scans_fake_steam() {
        let tmp = TempDir::new("steam").unwrap();
        let steam = tmp.path().join("Steam");
        let other = tmp.path().join("Library Two");
        fake_install(&steam.join("steamapps/common/Call of Duty"), "coduo").unwrap();
        fake_install(&other.join("SteamApps/common/Call of Duty 2"), "cod2").unwrap();
        fs::create_dir_all(other.join("SteamApps/compatdata/2630/pfx")).unwrap();
        let other_escaped = other.to_string_lossy().replace('\\', "\\\\");
        write_file(
            &steam,
            "steamapps/libraryfolders.vdf",
            format!(
                "\"libraryfolders\"\n{{\n\t\"0\"\n\t{{\n\t\t\"path\"\t\t\"{}\"\n\t}}\n\t\"1\"\n\t{{\n\t\t\"path\"\t\t\"{other_escaped}\"\n\t}}\n\t\"2\"\n\t{{\n\t\t\"path\"\t\t\"{}\"\n\t}}\n}}\n",
                steam.to_string_lossy().replace('\\', "\\\\"),
                tmp.path().join("unplugged drive").to_string_lossy().replace('\\', "\\\\"),
            )
            .as_bytes(),
        )
        .unwrap();
        for (id, dir) in [(2620, "Call of Duty"), (2640, "Call of Duty")] {
            write_file(
                &steam,
                &format!("steamapps/appmanifest_{id}.acf"),
                format!("\"AppState\"\n{{\n\t\"appid\"\t\t\"{id}\"\n\t\"installdir\"\t\t\"{dir}\"\n}}\n")
                    .as_bytes(),
            )
            .unwrap();
        }
        write_file(
            &other,
            "SteamApps/appmanifest_2630.acf",
            b"\"AppState\" { \"appid\" \"2630\" \"installdir\" \"call of duty 2\" }",
        )
        .unwrap();

        let roots = vec![SteamRoot {
            path: canonical(&steam).unwrap(),
            found_by: "test".into(),
        }];
        let scan = scan(&roots);
        assert_eq!(scan.libraries.len(), 2, "{:?}", scan.libraries);
        assert_eq!(scan.installs.len(), 2);
        assert!(scan.warnings.iter().any(|w| w.contains("unplugged drive")));

        let cod = &scan.installs[0];
        assert_eq!(cod.app_ids, vec![2620, 2640]);
        let games: Vec<GameId> = cod.games.iter().map(|g| g.game).collect();
        assert_eq!(games, vec![GameId::Cod, GameId::CodUo]);
        assert!(cod.proton_prefixes.is_empty());

        let cod2 = &scan.installs[1];
        assert_eq!(cod2.app_ids, vec![2630]);
        assert_eq!(cod2.games.len(), 1);
        assert_eq!(cod2.games[0].version.as_deref(), Some("1.3"));
        assert!(cod2.games[0].missing_any().is_empty());
        assert_eq!(cod2.proton_prefixes.len(), 1);
    }

    #[test]
    fn finds_proton_builds() {
        let tmp = TempDir::new("steam").unwrap();
        let steam = tmp.path().join("Steam");
        let system = tmp.path().join("usr-share-steam/compatibilitytools.d");
        write_file(
            &steam,
            "compatibilitytools.d/GE-Proton10-25/proton",
            b"#!/usr/bin/env python3\n",
        )
        .unwrap();
        write_file(
            &steam,
            "compatibilitytools.d/GE-Proton10-25/compatibilitytool.vdf",
            b"\"compatibilitytools\" {}",
        )
        .unwrap();
        // Not a Proton build: an empty folder and a stray file.
        fs::create_dir_all(steam.join("compatibilitytools.d/empty")).unwrap();
        write_file(&steam, "compatibilitytools.d/readme.txt", b"x").unwrap();
        write_file(&system, "proton-cachyos-10.0/proton", b"x").unwrap();
        write_file(&steam, "steamapps/common/Proton 9.0/proton", b"x").unwrap();
        write_file(
            &steam,
            "steamapps/common/Proton - Experimental/proton",
            b"x",
        )
        .unwrap();
        // A game folder whose name doesn't start with "Proton" is not one, whatever it holds.
        write_file(&steam, "steamapps/common/Call of Duty 2/proton", b"x").unwrap();

        let roots = vec![SteamRoot {
            path: canonical(&steam).unwrap(),
            found_by: "test".into(),
        }];
        let libraries = vec![canonical(&steam).unwrap()];
        let builds = proton_builds(&roots, &libraries, std::slice::from_ref(&system));
        let names: Vec<(&str, ProtonKind)> =
            builds.iter().map(|b| (b.name.as_str(), b.kind)).collect();
        assert_eq!(
            names,
            vec![
                ("GE-Proton10-25", ProtonKind::Custom),
                ("Proton - Experimental", ProtonKind::Valve),
                ("Proton 9.0", ProtonKind::Valve),
                ("proton-cachyos-10.0", ProtonKind::Custom),
            ]
        );
        // The same folder reached twice (root and library are one folder) is listed once.
        let twice = proton_builds(&[roots[0].clone(), roots[0].clone()], &libraries, &[]);
        assert_eq!(twice.len(), 3);
    }

    #[test]
    fn broken_manifest_falls_back_to_the_usual_folder() {
        let tmp = TempDir::new("steam").unwrap();
        fake_install(&tmp.path().join("steamapps/common/Call of Duty 2"), "cod2").unwrap();
        write_file(
            tmp.path(),
            "steamapps/appmanifest_2630.acf",
            b"\"AppState\" {",
        )
        .unwrap();
        write_file(tmp.path(), "steamapps/libraryfolders.vdf", b"}}}").unwrap();
        let roots = vec![SteamRoot {
            path: canonical(tmp.path()).unwrap(),
            found_by: "test".into(),
        }];
        let scan = scan(&roots);
        assert_eq!(scan.warnings.len(), 2, "{:?}", scan.warnings);
        assert_eq!(scan.installs.len(), 1);
        assert!(scan.installs[0].app_ids.is_empty());
        assert_eq!(scan.installs[0].games[0].game, GameId::Cod2);
    }

    #[test]
    fn explicit_root_must_exist() {
        let tmp = TempDir::new("steam").unwrap();
        assert!(find_roots(Some(&tmp.path().join("nope"))).is_empty());
        let roots = find_roots(Some(tmp.path()));
        assert_eq!(roots.len(), 1);
        assert_eq!(roots[0].found_by, "--steam");
    }
}
