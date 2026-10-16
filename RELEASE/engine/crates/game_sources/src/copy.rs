//! Makes the new copy, `<out>/<game>/<data folder>/<archive>`, and checks a
//! copy made earlier.
//!
//! Only names on the allow-list get in: the game's archives (matched by the
//! rules in [`crate::games`]) and its small version text files. Every
//! archive is checked to be a whole ZIP file before anything is written.
//! Files are hard-linked when the copy is on the same drive (instant, no
//! extra space) and copied otherwise, or always with `force_copy`.
//!
//! Safety:
//! - nothing is ever written inside the install the copy is made from; the
//!   output may not be inside it, nor it inside the output;
//! - each file is written under a temporary name and renamed into place, so
//!   an interrupted run leaves no half-written archive;
//! - running again only redoes what changed (same size and SHA-256 = kept);
//! - only files the previous manifest listed are ever removed.

use crate::games::{is_plain_file_name, GameId};
use crate::install::GameInstall;
use crate::manifest::{utc_now, Manifest, ManifestFile, FORMAT, MANIFEST_NAME, MAX_MANIFEST_BYTES};
use crate::paths::{canonical, is_within, resolve};
use crate::sha256::{self, hash_file, Sha256, BLOCK_BYTES};
use crate::vdf::read_text_limited;
use crate::zip::{self, ZipError};
use std::fmt;
use std::fs::{self, File};
use std::io::{self, Read, Write};
use std::path::{Path, PathBuf};

/// Ending of the temporary name a file is written under.
pub const TEMP_SUFFIX: &str = ".newcopy-partial";

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum SourceKind {
    Steam,
    Folder,
}

impl SourceKind {
    pub fn as_str(self) -> &'static str {
        match self {
            SourceKind::Steam => "steam",
            SourceKind::Folder => "folder",
        }
    }
}

/// What to copy and where.
#[derive(Debug, Clone)]
pub struct MakeRequest<'a> {
    pub install: &'a GameInstall,
    pub source_kind: SourceKind,
    pub steam_app_id: Option<u32>,
    /// The output folder; the copy goes into `<out>/<game key>/`.
    pub out: PathBuf,
    /// Language to take (lowercase); `None` = the only one there is.
    pub language: Option<String>,
    /// Copy every file even where a hard link would do.
    pub force_copy: bool,
    /// Make the copy even when required archives are missing.
    pub allow_missing: bool,
    /// Name and version of the program making the copy, for the manifest.
    pub tool: &'a str,
    pub tool_version: &'a str,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum FileAction {
    /// Already in the copy with the same size and SHA-256.
    Unchanged,
    Linked,
    Copied,
}

impl FileAction {
    pub fn as_str(self) -> &'static str {
        match self {
            FileAction::Unchanged => "unchanged",
            FileAction::Linked => "linked",
            FileAction::Copied => "copied",
        }
    }
}

/// One file of the copy.
#[derive(Debug, Clone)]
pub struct MadeFile {
    pub path: String,
    pub size: u64,
    pub sha256: [u8; 32],
    pub action: FileAction,
    /// Whether the file in the copy is a hard link to the original.
    pub hard_link: bool,
}

#[derive(Debug, Clone)]
pub struct MakeResult {
    /// `<out>/<game key>`.
    pub copy_dir: PathBuf,
    pub language: Option<String>,
    pub files: Vec<MadeFile>,
    /// Files of an earlier copy no longer wanted (another language), removed.
    pub removed: Vec<String>,
    pub manifest_path: PathBuf,
}

/// Progress reports while a copy is made.
#[derive(Debug)]
pub enum Progress<'a> {
    /// About to check this many archives.
    CheckingArchives(usize),
    /// A file is done (`index` counts from 1).
    FileDone {
        index: usize,
        total: usize,
        file: &'a MadeFile,
    },
}

#[derive(Debug)]
pub enum MakeError {
    Io {
        path: PathBuf,
        error: io::Error,
    },
    /// None of the game's archives are in the folder.
    NoArchives {
        game: &'static str,
        folders: Vec<&'static str>,
    },
    /// No localized archives at all.
    NoLanguage,
    LanguageNotFound {
        wanted: String,
        found: Vec<String>,
    },
    SeveralLanguages(Vec<String>),
    Missing(Vec<String>),
    OutputInsideSource {
        out: PathBuf,
        source: PathBuf,
    },
    SourceInsideOutput {
        out: PathBuf,
        source: PathBuf,
    },
    BadArchives(Vec<(String, ZipError)>),
    /// A file changed size while it was copied.
    SourceChanged(PathBuf),
}

impl fmt::Display for MakeError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            MakeError::Io { path, error } => write!(f, "{}: {error}", path.display()),
            MakeError::NoArchives { game, folders } => write!(
                f,
                "none of {game}'s archives are in this folder (looked in {}); is it the \
                 game's install folder?",
                folders
                    .iter()
                    .map(|name| format!("{name}/"))
                    .collect::<Vec<_>>()
                    .join(" and ")
            ),
            MakeError::NoLanguage => write!(
                f,
                "no localized archives (localized_<language>_...) were found, so the game has \
                 no text or speech"
            ),
            MakeError::LanguageNotFound { wanted, found } => write!(
                f,
                "no archives for the language \"{wanted}\"; this install has: {}",
                list_or_none(found)
            ),
            MakeError::SeveralLanguages(found) => write!(
                f,
                "this install has several languages ({}); pick one with --language",
                found.join(", ")
            ),
            MakeError::Missing(missing) => write!(
                f,
                "{} required archive(s) are missing: {}",
                missing.len(),
                missing.join(", ")
            ),
            MakeError::OutputInsideSource { out, source } => write!(
                f,
                "the output folder {} is inside the game folder {}; nothing is ever written \
                 into the game's own folder, pick another output",
                out.display(),
                source.display()
            ),
            MakeError::SourceInsideOutput { out, source } => write!(
                f,
                "the game folder {} is inside the output folder {}; pick another output",
                source.display(),
                out.display()
            ),
            MakeError::BadArchives(bad) => {
                write!(f, "{} archive(s) are damaged:", bad.len())?;
                for (path, error) in bad {
                    write!(f, "\n  {path}: {error}")?;
                }
                write!(
                    f,
                    "\nRepair the install (in Steam: Properties > Installed Files > Verify \
                     integrity of game files) and run again"
                )
            }
            MakeError::SourceChanged(path) => write!(
                f,
                "{} changed while it was being copied (is the game updating?); run again",
                path.display()
            ),
        }
    }
}

impl std::error::Error for MakeError {}

fn list_or_none(items: &[String]) -> String {
    if items.is_empty() {
        "none".into()
    } else {
        items.join(", ")
    }
}

fn io_error(path: &Path) -> impl FnOnce(io::Error) -> MakeError + '_ {
    move |error| MakeError::Io {
        path: path.to_path_buf(),
        error,
    }
}

/// The language a copy takes: `wanted` if the install has it, else the
/// only one there is.
pub fn choose_language(
    install: &GameInstall,
    wanted: Option<&str>,
) -> Result<Option<String>, MakeError> {
    let found = &install.languages;
    match wanted {
        Some(wanted) => {
            let wanted = wanted.to_ascii_lowercase();
            if found.contains(&wanted) {
                Ok(Some(wanted))
            } else {
                Err(MakeError::LanguageNotFound {
                    wanted,
                    found: found.clone(),
                })
            }
        }
        None => match found.len() {
            0 => Err(MakeError::NoLanguage),
            1 => Ok(Some(found[0].clone())),
            _ => Err(MakeError::SeveralLanguages(found.clone())),
        },
    }
}

/// A `/`-separated path inside the copy, checked to hold only plain names.
fn copy_relative_path(relative: &str) -> Option<PathBuf> {
    let mut path = PathBuf::new();
    for part in relative.split('/') {
        if !is_plain_file_name(part) {
            return None;
        }
        path.push(part);
    }
    (!path.as_os_str().is_empty()).then_some(path)
}

/// Whether `relative` is a name the copy of `game` can hold: one of its
/// text files, or an archive name of one of its data folders.
fn is_copy_name(game: GameId, relative: &str) -> bool {
    let game = game.game();
    if game
        .text_files
        .iter()
        .any(|t| t.eq_ignore_ascii_case(relative))
    {
        return true;
    }
    match relative.split_once('/') {
        Some((folder, name)) => game
            .data_folders
            .iter()
            .any(|f| f.name == folder && f.match_archive(name).is_some()),
        None => false,
    }
}

/// Whether two files are the same file on disk (a hard link). Only known on
/// Unix; elsewhere the contents are compared instead.
fn same_file(a: &fs::Metadata, b: &fs::Metadata) -> bool {
    #[cfg(unix)]
    {
        use std::os::unix::fs::MetadataExt;
        a.dev() == b.dev() && a.ino() == b.ino()
    }
    #[cfg(not(unix))]
    {
        let _ = (a, b);
        false
    }
}

struct Planned {
    source: PathBuf,
    relative: String,
    is_archive: bool,
}

/// Makes (or brings up to date) the copy described by `request`.
pub fn make_copy(
    request: &MakeRequest<'_>,
    progress: &mut dyn FnMut(Progress<'_>),
) -> Result<MakeResult, MakeError> {
    let install = request.install;
    let game = install.game.game();
    if install.archives.is_empty() {
        return Err(MakeError::NoArchives {
            game: game.name,
            folders: game.data_folders.iter().map(|f| f.name).collect(),
        });
    }
    let language = match choose_language(install, request.language.as_deref()) {
        Ok(language) => language,
        // No language at all: report it with everything else that's missing.
        Err(MakeError::NoLanguage) if request.allow_missing => None,
        Err(MakeError::NoLanguage) => return Err(MakeError::Missing(install.missing(None))),
        Err(e) => return Err(e),
    };
    let missing = install.missing(language.as_deref());
    if !missing.is_empty() && !request.allow_missing {
        return Err(MakeError::Missing(missing));
    }

    // Where things are, resolved, so the inside/outside checks can't be
    // fooled by symlinks or `..`.
    let source_root = canonical(&install.root).map_err(io_error(&install.root))?;
    let out = resolve(&request.out).map_err(io_error(&request.out))?;
    if is_within(&out, &source_root) {
        return Err(MakeError::OutputInsideSource {
            out,
            source: source_root,
        });
    }
    if is_within(&source_root, &out) {
        return Err(MakeError::SourceInsideOutput {
            out,
            source: source_root,
        });
    }

    let mut plan: Vec<Planned> = Vec::new();
    for archive in install.archives_for(language.as_deref()) {
        plan.push(Planned {
            source: archive.source.clone(),
            relative: archive.copy_path.clone(),
            is_archive: true,
        });
    }
    for text in &install.text_files {
        plan.push(Planned {
            source: text.source.clone(),
            relative: text.copy_path.clone(),
            is_archive: false,
        });
    }
    plan.sort_by(|a, b| a.relative.cmp(&b.relative));

    // Check every archive before writing anything.
    let archive_count = plan.iter().filter(|p| p.is_archive).count();
    progress(Progress::CheckingArchives(archive_count));
    let mut bad = Vec::new();
    for planned in plan.iter().filter(|p| p.is_archive) {
        if let Err(error) = zip::check_file(&planned.source) {
            bad.push((planned.relative.clone(), error));
        }
    }
    if !bad.is_empty() {
        return Err(MakeError::BadArchives(bad));
    }

    let copy_dir = out.join(game.key);
    fs::create_dir_all(&copy_dir).map_err(io_error(&copy_dir))?;
    for folder in game.data_folders {
        let dir = copy_dir.join(folder.name);
        fs::create_dir_all(&dir).map_err(io_error(&dir))?;
        // A data folder that is a symlink into the game folder would let
        // writes land there: check where it really is.
        let real = canonical(&dir).map_err(io_error(&dir))?;
        if is_within(&real, &source_root) {
            return Err(MakeError::OutputInsideSource {
                out: real,
                source: source_root,
            });
        }
    }
    let real_copy_dir = canonical(&copy_dir).map_err(io_error(&copy_dir))?;
    if is_within(&real_copy_dir, &source_root) {
        return Err(MakeError::OutputInsideSource {
            out: real_copy_dir,
            source: source_root,
        });
    }

    remove_partial_files(&copy_dir, install.game);
    let manifest_path = copy_dir.join(MANIFEST_NAME);
    let previous = read_manifest(&manifest_path).ok();

    let total = plan.len();
    let mut files = Vec::with_capacity(total);
    for (n, planned) in plan.iter().enumerate() {
        let relative = copy_relative_path(&planned.relative).ok_or_else(|| MakeError::Io {
            path: PathBuf::from(&planned.relative),
            error: io::Error::new(io::ErrorKind::InvalidInput, "not a plain relative path"),
        })?;
        let dest = copy_dir.join(relative);
        let made = place_file(
            &planned.source,
            &dest,
            planned.is_archive && !request.force_copy,
            request.force_copy,
        )?;
        let file = MadeFile {
            path: planned.relative.clone(),
            size: made.0,
            sha256: made.1,
            action: made.2,
            hard_link: made.3,
        };
        progress(Progress::FileDone {
            index: n + 1,
            total,
            file: &file,
        });
        files.push(file);
    }

    // Files an earlier run made that this run doesn't want (for example
    // another language) are removed: only names the old manifest lists,
    // and only names this tool could have made.
    let mut removed = Vec::new();
    if let Some(previous) = &previous {
        for old in &previous.files {
            if files.iter().any(|f| f.path == old.path) || !is_copy_name(install.game, &old.path) {
                continue;
            }
            if let Some(relative) = copy_relative_path(&old.path) {
                let path = copy_dir.join(relative);
                if path.is_file() && fs::remove_file(&path).is_ok() {
                    removed.push(old.path.clone());
                }
            }
        }
    }

    let manifest = Manifest {
        format: FORMAT,
        tool: request.tool.to_string(),
        tool_version: request.tool_version.to_string(),
        made: utc_now(),
        game: game.key.to_string(),
        game_name: game.name.to_string(),
        version: install.version.clone(),
        language: language.clone(),
        source_kind: request.source_kind.as_str().to_string(),
        source_path: source_root.to_string_lossy().into_owned(),
        steam_app_id: request.steam_app_id,
        files: files
            .iter()
            .map(|f| ManifestFile {
                path: f.path.clone(),
                size: f.size,
                sha256: sha256::to_hex(&f.sha256),
                how: if f.hard_link { "hard link" } else { "copy" }.to_string(),
            })
            .collect(),
    };
    write_atomically(&manifest_path, manifest.to_toml().as_bytes())?;

    Ok(MakeResult {
        copy_dir,
        language,
        files,
        removed,
        manifest_path,
    })
}

/// Removes temporary files an interrupted run left in the copy's folders.
fn remove_partial_files(copy_dir: &Path, game: GameId) {
    let mut dirs = vec![copy_dir.to_path_buf()];
    dirs.extend(
        game.game()
            .data_folders
            .iter()
            .map(|f| copy_dir.join(f.name)),
    );
    for dir in dirs {
        let Ok(entries) = fs::read_dir(&dir) else {
            continue;
        };
        for entry in entries.filter_map(Result::ok) {
            let is_partial = entry
                .file_name()
                .to_str()
                .is_some_and(|n| n.ends_with(TEMP_SUFFIX));
            if is_partial && entry.file_type().is_ok_and(|t| t.is_file()) {
                let _ = fs::remove_file(entry.path());
            }
        }
    }
}

fn temp_path(dest: &Path) -> PathBuf {
    let mut name = dest.file_name().unwrap_or_default().to_os_string();
    name.push(TEMP_SUFFIX);
    dest.with_file_name(name)
}

/// Puts `source` at `dest`. Returns size, SHA-256, what was done, and
/// whether `dest` is a hard link to `source`.
fn place_file(
    source: &Path,
    dest: &Path,
    link_allowed: bool,
    force_copy: bool,
) -> Result<(u64, [u8; 32], FileAction, bool), MakeError> {
    let source_meta = fs::metadata(source).map_err(io_error(source))?;
    if let Ok(dest_meta) = fs::symlink_metadata(dest) {
        if dest_meta.is_file() && dest_meta.len() == source_meta.len() {
            let linked = same_file(&source_meta, &dest_meta);
            if linked && !force_copy {
                let (size, sum) = hash_file(source).map_err(io_error(source))?;
                return Ok((size, sum, FileAction::Unchanged, true));
            }
            if !linked {
                let (size, sum) = hash_file(source).map_err(io_error(source))?;
                let (_, dest_sum) = hash_file(dest).map_err(io_error(dest))?;
                if sum == dest_sum {
                    return Ok((size, sum, FileAction::Unchanged, false));
                }
            }
        }
    }

    let temp = temp_path(dest);
    let _ = fs::remove_file(&temp);
    if link_allowed && fs::hard_link(source, &temp).is_ok() {
        let (size, sum) = hash_file(&temp).map_err(io_error(&temp))?;
        fs::rename(&temp, dest).map_err(io_error(dest))?;
        return Ok((size, sum, FileAction::Linked, true));
    }
    // Different drive, a file system without hard links, or --copy.
    let result = copy_and_hash(source, &temp, source_meta.len());
    match result {
        Ok((size, sum)) => {
            fs::rename(&temp, dest).map_err(io_error(dest))?;
            Ok((size, sum, FileAction::Copied, false))
        }
        Err(e) => {
            let _ = fs::remove_file(&temp);
            Err(e)
        }
    }
}

/// Copies `source` to `temp` in 1 MiB blocks, hashing as it goes, and
/// flushes it to disk.
fn copy_and_hash(
    source: &Path,
    temp: &Path,
    expected_size: u64,
) -> Result<(u64, [u8; 32]), MakeError> {
    let mut input = File::open(source).map_err(io_error(source))?;
    let mut output = File::create(temp).map_err(io_error(temp))?;
    let mut hasher = Sha256::new();
    let mut block = vec![0u8; BLOCK_BYTES];
    let mut total = 0u64;
    loop {
        let n = match input.read(&mut block) {
            Ok(0) => break,
            Ok(n) => n,
            Err(e) if e.kind() == io::ErrorKind::Interrupted => continue,
            Err(e) => return Err(io_error(source)(e)),
        };
        hasher.update(&block[..n]);
        output.write_all(&block[..n]).map_err(io_error(temp))?;
        total += n as u64;
    }
    output.sync_all().map_err(io_error(temp))?;
    if total != expected_size {
        return Err(MakeError::SourceChanged(source.to_path_buf()));
    }
    Ok((total, hasher.finish()))
}

/// Writes a small file under a temporary name, then renames it into place.
fn write_atomically(path: &Path, contents: &[u8]) -> Result<(), MakeError> {
    let temp = temp_path(path);
    let mut file = File::create(&temp).map_err(io_error(&temp))?;
    file.write_all(contents).map_err(io_error(&temp))?;
    file.sync_all().map_err(io_error(&temp))?;
    drop(file);
    fs::rename(&temp, path).map_err(io_error(path))
}

/// Reads a copy's `newcopy.toml`.
pub fn read_manifest(path: &Path) -> Result<Manifest, String> {
    let text = read_text_limited(path, MAX_MANIFEST_BYTES)
        .map_err(|e| format!("{}: {e}", path.display()))?;
    Manifest::parse(&text).map_err(|e| format!("{}: {e}", path.display()))
}

/// The result of checking one game's copy.
#[derive(Debug, Clone)]
pub struct CheckReport {
    pub copy_dir: PathBuf,
    pub manifest: Manifest,
    /// Files that are missing, changed or damaged. Empty = the copy is good.
    pub problems: Vec<String>,
    /// Files in the copy's data folders the manifest doesn't list.
    pub unlisted: Vec<String>,
}

/// The copies to check in `dir`: `dir` itself if it holds a manifest, else
/// each `<dir>/<game key>` that does.
pub fn copies_in(dir: &Path) -> Vec<PathBuf> {
    if dir.join(MANIFEST_NAME).is_file() {
        return vec![dir.to_path_buf()];
    }
    GameId::ALL
        .into_iter()
        .map(|id| dir.join(id.key()))
        .filter(|d| d.join(MANIFEST_NAME).is_file())
        .collect()
}

/// Checks the copy in `copy_dir` against its manifest: every file there,
/// same size, same SHA-256, archives whole. `progress` is told each file's
/// path before it is hashed.
pub fn check_copy(copy_dir: &Path, progress: &mut dyn FnMut(&str)) -> Result<CheckReport, String> {
    let manifest = read_manifest(&copy_dir.join(MANIFEST_NAME))?;
    let mut problems = Vec::new();
    for file in &manifest.files {
        progress(&file.path);
        let Some(relative) = copy_relative_path(&file.path) else {
            problems.push(format!("{}: not a plain path inside the copy", file.path));
            continue;
        };
        let path = copy_dir.join(relative);
        let size = match fs::metadata(&path) {
            Ok(meta) if meta.is_file() => meta.len(),
            Ok(_) => {
                problems.push(format!("{}: not a file", file.path));
                continue;
            }
            Err(_) => {
                problems.push(format!("{}: missing", file.path));
                continue;
            }
        };
        if size != file.size {
            problems.push(format!(
                "{}: {size} bytes, the manifest says {}",
                file.path, file.size
            ));
            continue;
        }
        match hash_file(&path) {
            Ok((_, sum)) => {
                if sha256::from_hex(&file.sha256) != Some(sum) {
                    problems.push(format!("{}: contents changed (SHA-256 differs)", file.path));
                    continue;
                }
            }
            Err(e) => {
                problems.push(format!("{}: {e}", file.path));
                continue;
            }
        }
        let lower = file.path.to_ascii_lowercase();
        if lower.ends_with(".pk3") || lower.ends_with(".iwd") {
            if let Err(e) = zip::check_file(&path) {
                problems.push(format!("{}: {e}", file.path));
            }
        }
    }

    let mut unlisted = Vec::new();
    let game = GameId::from_key(&manifest.game);
    let mut folders = vec![String::new()];
    if let Some(game) = game {
        folders.extend(game.game().data_folders.iter().map(|f| f.name.to_string()));
    }
    for folder in folders {
        let dir = if folder.is_empty() {
            copy_dir.to_path_buf()
        } else {
            copy_dir.join(&folder)
        };
        let Ok(entries) = fs::read_dir(&dir) else {
            continue;
        };
        let mut names: Vec<String> = entries
            .filter_map(Result::ok)
            .filter(|e| e.file_type().is_ok_and(|t| !t.is_dir()))
            .filter_map(|e| e.file_name().into_string().ok())
            .collect();
        names.sort();
        for name in names {
            let relative = if folder.is_empty() {
                name
            } else {
                format!("{folder}/{name}")
            };
            if relative != MANIFEST_NAME && !manifest.files.iter().any(|f| f.path == relative) {
                unlisted.push(relative);
            }
        }
    }
    Ok(CheckReport {
        copy_dir: copy_dir.to_path_buf(),
        manifest,
        problems,
        unlisted,
    })
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::install::recognize;
    use crate::testkit::{
        fake_archive, fake_install, stored_zip_with_comment, write_file, TempDir,
    };

    fn request<'a>(install: &'a GameInstall, out: &Path) -> MakeRequest<'a> {
        MakeRequest {
            install,
            source_kind: SourceKind::Folder,
            steam_app_id: None,
            out: out.to_path_buf(),
            language: None,
            force_copy: false,
            allow_missing: false,
            tool: "test",
            tool_version: "0",
        }
    }

    fn install_of(dir: &Path, id: GameId) -> GameInstall {
        recognize(dir).into_iter().find(|i| i.game == id).unwrap()
    }

    fn all_files(dir: &Path) -> Vec<String> {
        let mut found = Vec::new();
        let mut stack = vec![dir.to_path_buf()];
        while let Some(d) = stack.pop() {
            for entry in fs::read_dir(&d).unwrap().filter_map(Result::ok) {
                let path = entry.path();
                if path.is_dir() {
                    stack.push(path);
                } else {
                    let rel = path.strip_prefix(dir).unwrap().to_string_lossy();
                    found.push(rel.replace('\\', "/"));
                }
            }
        }
        found.sort();
        found
    }

    #[test]
    fn makes_a_uo_copy_with_only_archives_and_text() {
        let tmp = TempDir::new("make").unwrap();
        let source = tmp.path().join("Call of Duty");
        fake_install(&source, "coduo").unwrap();
        let out = tmp.path().join("copies");
        let uo = install_of(&source, GameId::CodUo);
        let result = make_copy(&request(&uo, &out), &mut |_| {}).unwrap();
        assert_eq!(result.language.as_deref(), Some("english"));
        assert_eq!(result.files.len(), 30 + 3);

        let files = all_files(&out.join("coduo"));
        assert_eq!(files.len(), 30 + 3 + 1, "{files:?}");
        assert!(files.contains(&"main/pak0.pk3".to_string()));
        assert!(files.contains(&"main/localized_english_pak5.pk3".to_string()));
        assert!(files.contains(&"uo/uomappack00.pk3".to_string()));
        assert!(files.contains(&"uo/version.inf".to_string()));
        assert!(files.contains(&"newcopy.toml".to_string()));
        for file in &files {
            let lower = file.to_ascii_lowercase();
            assert!(
                !lower.ends_with(".exe") && !lower.ends_with(".dll"),
                "{file} must never be copied"
            );
        }
        // Same drive: hard links (where the file system allows them).
        assert!(result
            .files
            .iter()
            .filter(|f| f.path.ends_with(".pk3"))
            .all(|f| f.action == FileAction::Linked || f.action == FileAction::Copied));
        // Text files are always real copies.
        assert!(result
            .files
            .iter()
            .filter(|f| f.path.ends_with(".inf"))
            .all(|f| f.action == FileAction::Copied));

        let report = check_copy(&out.join("coduo"), &mut |_| {}).unwrap();
        assert!(report.problems.is_empty(), "{:?}", report.problems);
        assert!(report.unlisted.is_empty(), "{:?}", report.unlisted);
        assert_eq!(report.manifest.game, "coduo");
        assert_eq!(report.manifest.version.as_deref(), Some("1.51b"));
        assert_eq!(copies_in(&out), vec![out.join("coduo")]);
    }

    #[test]
    fn rerun_is_incremental_and_repairs_damage() {
        let tmp = TempDir::new("make").unwrap();
        let source = tmp.path().join("cod2");
        fake_install(&source, "cod2").unwrap();
        let out = tmp.path().join("out");
        let cod2 = install_of(&source, GameId::Cod2);
        let mut req = request(&cod2, &out);
        req.force_copy = true;
        let first = make_copy(&req, &mut |_| {}).unwrap();
        assert!(first.files.iter().all(|f| f.action == FileAction::Copied));

        let second = make_copy(&req, &mut |_| {}).unwrap();
        assert!(second
            .files
            .iter()
            .all(|f| f.action == FileAction::Unchanged));

        // Damage one archive in the copy and leave a partial file behind,
        // as an interrupted run would.
        let damaged = out.join("cod2/main/iw_03.iwd");
        let mut bytes = fs::read(&damaged).unwrap();
        let last = bytes.len() - 1;
        bytes[last] ^= 0xFF;
        fs::write(&damaged, &bytes).unwrap();
        write_file(&out, &format!("cod2/main/iw_04.iwd{TEMP_SUFFIX}"), b"half").unwrap();
        let report = check_copy(&out.join("cod2"), &mut |_| {}).unwrap();
        assert_eq!(report.problems.len(), 1, "{:?}", report.problems);
        assert!(report.problems[0].starts_with("main/iw_03.iwd"));
        assert_eq!(report.unlisted.len(), 1);

        let third = make_copy(&req, &mut |_| {}).unwrap();
        let redone: Vec<&str> = third
            .files
            .iter()
            .filter(|f| f.action != FileAction::Unchanged)
            .map(|f| f.path.as_str())
            .collect();
        assert_eq!(redone, vec!["main/iw_03.iwd"]);
        let report = check_copy(&out.join("cod2"), &mut |_| {}).unwrap();
        assert!(report.problems.is_empty() && report.unlisted.is_empty());
    }

    #[test]
    fn output_inside_source_is_refused() {
        let tmp = TempDir::new("make").unwrap();
        fake_install(tmp.path(), "cod").unwrap();
        let cod = install_of(tmp.path(), GameId::Cod);
        let error = make_copy(
            &request(&cod, &tmp.path().join("Main/newcopy")),
            &mut |_| {},
        )
        .unwrap_err();
        assert!(
            matches!(error, MakeError::OutputInsideSource { .. }),
            "{error}"
        );
        let error = make_copy(&request(&cod, tmp.path()), &mut |_| {}).unwrap_err();
        assert!(matches!(error, MakeError::OutputInsideSource { .. }));
        // Nothing was written into the source.
        assert!(!tmp.path().join("Main/newcopy").exists());
        assert!(!tmp.path().join("cod").exists());

        let inner = tmp.path().join("inner");
        fake_install(&inner, "cod").unwrap();
        let cod = install_of(&inner, GameId::Cod);
        let error = make_copy(&request(&cod, tmp.path()), &mut |_| {}).unwrap_err();
        assert!(
            matches!(error, MakeError::SourceInsideOutput { .. }),
            "{error}"
        );
    }

    #[cfg(unix)]
    #[test]
    fn symlinked_output_into_source_is_refused() {
        let tmp = TempDir::new("make").unwrap();
        let source = tmp.path().join("game");
        fake_install(&source, "cod2").unwrap();
        let out = tmp.path().join("out");
        fs::create_dir_all(out.join("cod2")).unwrap();
        std::os::unix::fs::symlink(source.join("main"), out.join("cod2/main")).unwrap();
        let cod2 = install_of(&source, GameId::Cod2);
        let error = make_copy(&request(&cod2, &out), &mut |_| {}).unwrap_err();
        assert!(
            matches!(error, MakeError::OutputInsideSource { .. }),
            "{error}"
        );
    }

    #[test]
    fn damaged_or_missing_archives_stop_the_copy() {
        let tmp = TempDir::new("make").unwrap();
        let source = tmp.path().join("game");
        fake_install(&source, "cod2").unwrap();
        // Cut an archive short, as a failed download would.
        let truncated = source.join("main/iw_07.iwd");
        let bytes = fs::read(&truncated).unwrap();
        fs::write(&truncated, &bytes[..bytes.len() / 2]).unwrap();
        let out = tmp.path().join("out");
        let cod2 = install_of(&source, GameId::Cod2);
        let error = make_copy(&request(&cod2, &out), &mut |_| {}).unwrap_err();
        match &error {
            MakeError::BadArchives(bad) => {
                assert_eq!(bad.len(), 1);
                assert_eq!(bad[0].0, "main/iw_07.iwd");
            }
            other => panic!("{other}"),
        }
        assert!(
            !out.join("cod2").exists(),
            "nothing written before the check"
        );

        fs::remove_file(&truncated).unwrap();
        let cod2 = install_of(&source, GameId::Cod2);
        let error = make_copy(&request(&cod2, &out), &mut |_| {}).unwrap_err();
        assert!(matches!(&error, MakeError::Missing(m) if m == &["main/iw_07.iwd".to_string()]));
        let mut req = request(&cod2, &out);
        req.allow_missing = true;
        assert_eq!(make_copy(&req, &mut |_| {}).unwrap().files.len(), 27 + 2);
    }

    #[test]
    fn archive_with_comment_is_accepted() {
        let tmp = TempDir::new("make").unwrap();
        let source = tmp.path().join("game");
        fake_install(&source, "cod2").unwrap();
        let with_comment = stored_zip_with_comment(&[("a.txt", b"a")], b"archive comment");
        write_file(&source, "main/iw_00.iwd", &with_comment).unwrap();
        let cod2 = install_of(&source, GameId::Cod2);
        let result = make_copy(&request(&cod2, &tmp.path().join("out")), &mut |_| {}).unwrap();
        assert_eq!(result.files.len(), 30);
    }

    #[test]
    fn language_choice_and_switch() {
        let tmp = TempDir::new("make").unwrap();
        let source = tmp.path().join("game");
        fake_install(&source, "cod2").unwrap();
        for i in 0..=3 {
            let name = format!("localized_french_iw{i:02}.iwd");
            write_file(&source, &format!("main/{name}"), &fake_archive(&name)).unwrap();
        }
        let out = tmp.path().join("out");
        let cod2 = install_of(&source, GameId::Cod2);
        assert_eq!(cod2.languages, vec!["english", "french"]);
        let error = make_copy(&request(&cod2, &out), &mut |_| {}).unwrap_err();
        assert!(matches!(error, MakeError::SeveralLanguages(_)));
        let mut req = request(&cod2, &out);
        req.language = Some("German".into());
        assert!(matches!(
            make_copy(&req, &mut |_| {}).unwrap_err(),
            MakeError::LanguageNotFound { .. }
        ));

        req.language = Some("English".into());
        let english = make_copy(&req, &mut |_| {}).unwrap();
        assert_eq!(english.files.len(), 16 + 12 + 2);
        req.language = Some("french".into());
        let french = make_copy(&req, &mut |_| {}).unwrap();
        assert_eq!(french.files.len(), 16 + 4 + 2);
        assert_eq!(french.removed.len(), 12);
        assert!(!out.join("cod2/main/localized_english_iw00.iwd").exists());
        let report = check_copy(&out.join("cod2"), &mut |_| {}).unwrap();
        assert!(report.problems.is_empty() && report.unlisted.is_empty());
        assert_eq!(report.manifest.language.as_deref(), Some("french"));
    }

    #[test]
    fn folder_without_archives_is_refused_plainly() {
        let tmp = TempDir::new("make").unwrap();
        write_file(tmp.path(), "game/CoD2MP_s.exe", b"stub").unwrap();
        let cod2 = install_of(&tmp.path().join("game"), GameId::Cod2);
        let error = make_copy(&request(&cod2, &tmp.path().join("out")), &mut |_| {}).unwrap_err();
        assert!(matches!(error, MakeError::NoArchives { .. }), "{error}");
        assert!(error.to_string().contains("looked in main/"));
        assert!(!tmp.path().join("out").exists());
    }

    #[test]
    fn only_copy_names_are_ever_removed() {
        assert!(is_copy_name(GameId::Cod2, "main/iw_00.iwd"));
        assert!(is_copy_name(GameId::Cod2, "version.inf"));
        assert!(is_copy_name(GameId::CodUo, "uo/version.inf"));
        assert!(is_copy_name(GameId::CodUo, "main/pak0.pk3"));
        for other in [
            "main/notes.txt",
            "docs/iw_00.iwd",
            "main/sub/iw_00.iwd",
            "uo/pakuo00.pk3",
            "CoD2MP_s.exe",
            "main",
        ] {
            assert!(!is_copy_name(GameId::Cod2, other), "{other}");
        }

        // A doctored manifest naming another file is not obeyed.
        let tmp = TempDir::new("make").unwrap();
        let source = tmp.path().join("game");
        fake_install(&source, "cod2").unwrap();
        let out = tmp.path().join("out");
        let cod2 = install_of(&source, GameId::Cod2);
        make_copy(&request(&cod2, &out), &mut |_| {}).unwrap();
        let keep = write_file(&out, "cod2/main/notes.txt", b"mine").unwrap();
        let manifest_path = out.join("cod2").join(MANIFEST_NAME);
        let mut manifest = read_manifest(&manifest_path).unwrap();
        manifest.files.push(ManifestFile {
            path: "main/notes.txt".into(),
            size: 4,
            sha256: "00".repeat(32),
            how: "copy".into(),
        });
        fs::write(&manifest_path, manifest.to_toml()).unwrap();
        let result = make_copy(&request(&cod2, &out), &mut |_| {}).unwrap();
        assert!(result.removed.is_empty());
        assert!(keep.exists());
    }

    #[test]
    fn relative_paths_are_plain() {
        assert!(copy_relative_path("main/pak0.pk3").is_some());
        for bad in [
            "",
            "../x",
            "main/../x",
            "/etc/passwd",
            "main//x",
            "C:/x",
            "a\\b",
        ] {
            assert!(copy_relative_path(bad).is_none(), "{bad}");
        }
    }
}
