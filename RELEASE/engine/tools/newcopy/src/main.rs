//! `newcopy`: makes a new copy of Call of Duty, United Offensive or Call of
//! Duty 2 for the Rust engine from the player's own install (Steam, a CB
//! copy, a disc install, any folder). The copy holds only the game's data
//! archives and version text; no executable is ever read, copied or run.
//! See docs/NEW_COPY.md.

#![forbid(unsafe_code)]

use game_sources::copy::{FileAction, Progress};
use game_sources::{
    check_copy, copies_in, make_copy, steam, Found, GameId, MakeError, MakeRequest, SourceKind,
    SteamScan,
};
use std::path::{Path, PathBuf};
use std::process::ExitCode;

/// Prints a line to standard output, ignoring errors: if the output is
/// closed early (`newcopy find | head`), the work carries on instead of
/// stopping halfway.
macro_rules! say {
    ($($arg:tt)*) => {{
        use std::io::Write as _;
        let _ = writeln!(std::io::stdout(), $($arg)*);
    }};
}

const TOOL: &str = "newcopy";
const VERSION: &str = env!("CARGO_PKG_VERSION");

const USAGE: &str = "\
newcopy: makes a new copy of Call of Duty, United Offensive or Call of Duty 2
for the Rust engine from your own install. Only the game's data archives
(.pk3 / .iwd) and version text go in; no program file is read or copied.

Usage:
  newcopy find  [--from <dir>]... [--steam <dir>]
  newcopy make  <cod|coduo|cod2> --out <dir> [--from <dir>] [--steam <dir>]
                [--copy] [--language <lang>] [--allow-missing]
  newcopy check <dir>
  newcopy --help | --version

Commands:
  find    List every install found: game, version, language, where it is,
          and which archives are missing.
  make    Make (or bring up to date) the copy in <out>/<game>/.
  check   Check a copy made earlier against its newcopy.toml.

Options:
  --from <dir>       Use the install in this folder (a CB copy, a disc install,
                     a copy on another drive) instead of looking in Steam.
  --steam <dir>      Look for Steam games in this folder instead of the usual
                     places (the environment variable GAME_SOURCES_STEAM works too).
  --out <dir>        Where the copy goes; it ends up in <dir>/<game>/.
  --copy             Copy every file, even where a hard link would do.
  --language <lang>  The language to take when the install has several
                     (english, french, german, ...).
  --allow-missing    Make the copy even when some archives are missing.

Games: cod = Call of Duty (2003), coduo = Call of Duty: United Offensive
(its copy also holds Call of Duty's archives, which it needs), cod2 = Call of Duty 2.

Nothing is ever written into the game's own folder.";

/// What went wrong, and the exit code for it.
enum Failure {
    /// The command line is wrong (exit code 2).
    Usage(String),
    /// The command failed (exit code 1).
    Failed(String),
}

#[derive(Debug, Default, PartialEq, Eq)]
struct Options {
    command: String,
    positional: Vec<String>,
    from: Vec<PathBuf>,
    steam: Option<PathBuf>,
    out: Option<PathBuf>,
    copy: bool,
    language: Option<String>,
    allow_missing: bool,
    help: bool,
    version: bool,
}

fn parse_args(args: &[String]) -> Result<Options, String> {
    let mut options = Options::default();
    let mut i = 0;
    while i < args.len() {
        let arg = &args[i];
        i += 1;
        let (name, inline_value) = match arg.split_once('=') {
            Some((name, value)) if name.starts_with("--") => (name, Some(value.to_string())),
            _ => (arg.as_str(), None),
        };
        let mut value = |what: &str| -> Result<String, String> {
            if let Some(v) = inline_value.clone() {
                return Ok(v);
            }
            match args.get(i) {
                Some(v) if !v.starts_with("--") => {
                    i += 1;
                    Ok(v.clone())
                }
                _ => Err(format!("{name} needs {what}")),
            }
        };
        match name {
            "-h" | "--help" => options.help = true,
            "-V" | "--version" => options.version = true,
            "--from" => options.from.push(PathBuf::from(value("a folder")?)),
            "--steam" => {
                if options.steam.is_some() {
                    return Err("--steam can be given once".into());
                }
                options.steam = Some(PathBuf::from(value("a folder")?));
            }
            "--out" => {
                if options.out.is_some() {
                    return Err("--out can be given once".into());
                }
                options.out = Some(PathBuf::from(value("a folder")?));
            }
            "--language" => options.language = Some(value("a language")?),
            "--copy" => options.copy = true,
            "--allow-missing" => options.allow_missing = true,
            _ if name.starts_with('-') && name.len() > 1 => {
                return Err(format!("unknown option {name}"))
            }
            _ if options.command.is_empty() => options.command = arg.clone(),
            _ => options.positional.push(arg.clone()),
        }
    }
    Ok(options)
}

fn main() -> ExitCode {
    let args: Vec<String> = std::env::args().skip(1).collect();
    let result = parse_args(&args).map_err(Failure::Usage).and_then(run);
    match result {
        Ok(()) => ExitCode::SUCCESS,
        Err(Failure::Usage(message)) => {
            eprintln!("newcopy: {message}\nRun `newcopy --help` for how to use it.");
            ExitCode::from(2)
        }
        Err(Failure::Failed(message)) => {
            eprintln!("newcopy: {message}");
            ExitCode::from(1)
        }
    }
}

fn run(options: Options) -> Result<(), Failure> {
    if options.version {
        say!("{TOOL} {VERSION}");
        return Ok(());
    }
    if options.help || options.command.is_empty() || options.command == "help" {
        say!("{USAGE}");
        return Ok(());
    }
    match options.command.as_str() {
        "find" => {
            if !options.positional.is_empty() {
                return Err(Failure::Usage(format!(
                    "find takes no {} (use --from <dir> for a folder)",
                    options.positional[0]
                )));
            }
            reject(
                &options,
                &["--out", "--copy", "--language", "--allow-missing"],
            )?;
            find(&options)
        }
        "make" => make(&options),
        "check" => {
            reject(
                &options,
                &[
                    "--from",
                    "--steam",
                    "--out",
                    "--copy",
                    "--language",
                    "--allow-missing",
                ],
            )?;
            match options.positional.as_slice() {
                [dir] => check(Path::new(dir)),
                _ => Err(Failure::Usage(
                    "check needs one folder: newcopy check <dir>".into(),
                )),
            }
        }
        other => Err(Failure::Usage(format!("unknown command {other}"))),
    }
}

/// Refuses options that mean nothing to this command.
fn reject(options: &Options, names: &[&str]) -> Result<(), Failure> {
    let given = [
        ("--from", !options.from.is_empty()),
        ("--steam", options.steam.is_some()),
        ("--out", options.out.is_some()),
        ("--copy", options.copy),
        ("--language", options.language.is_some()),
        ("--allow-missing", options.allow_missing),
    ];
    for (name, is_given) in given {
        if is_given && names.contains(&name) {
            return Err(Failure::Usage(format!(
                "{name} doesn't apply to {}",
                options.command
            )));
        }
    }
    Ok(())
}

/// Bytes as a short decimal size: `170.2 MB`.
fn size_text(bytes: u64) -> String {
    const UNITS: [&str; 4] = ["KB", "MB", "GB", "TB"];
    if bytes < 1000 {
        return format!("{bytes} bytes");
    }
    let mut value = bytes as f64 / 1000.0;
    let mut unit = 0;
    while value >= 1000.0 && unit < UNITS.len() - 1 {
        value /= 1000.0;
        unit += 1;
    }
    format!("{value:.1} {}", UNITS[unit])
}

/// A list, cut after `limit` items.
fn short_list(items: &[String], limit: usize) -> String {
    if items.len() <= limit {
        return items.join(", ");
    }
    format!(
        "{}, and {} more",
        items[..limit].join(", "),
        items.len() - limit
    )
}

fn scan_steam(steam_dir: Option<&Path>) -> Result<SteamScan, Failure> {
    if let Some(dir) = steam_dir {
        if !dir.is_dir() {
            return Err(Failure::Failed(format!(
                "--steam {}: not a folder",
                dir.display()
            )));
        }
    }
    let roots = steam::find_roots(steam_dir);
    Ok(steam::scan(&roots))
}

fn print_steam_summary(scan: &SteamScan) {
    if scan.roots.is_empty() {
        say!("Steam: not found in the usual places (use --steam <dir> if it is elsewhere).");
    } else {
        say!("Steam:");
        for root in &scan.roots {
            say!("  {}  (found by {})", root.path.display(), root.found_by);
        }
        for library in &scan.libraries {
            if !scan.roots.iter().any(|r| &r.path == library) {
                say!("  library {}", library.display());
            }
        }
    }
}

/// The Proton builds Steam has (Linux). Not needed by the Rust engine; they
/// run the original Windows games, such as a CB copy for cross-play tests.
fn print_proton_builds(scan: &SteamScan) {
    if scan.proton_builds.is_empty() {
        return;
    }
    say!("\nProton builds (to run the original Windows games on Linux, for example a CB copy");
    say!("for cross-play tests; the Rust engine doesn't need one; newcopy never runs them):");
    let width = scan
        .proton_builds
        .iter()
        .map(|b| b.name.len())
        .max()
        .unwrap_or(0);
    for build in &scan.proton_builds {
        let kind = match build.kind {
            steam::ProtonKind::Valve => "Valve ",
            steam::ProtonKind::Custom => "custom",
        };
        say!("  {:<width$}  {kind}  {}", build.name, build.path.display());
    }
}

/// Where an install came from, in words.
fn source_text(found: &Found) -> String {
    let apps = |ids: &[u32]| {
        ids.iter()
            .map(|id| id.to_string())
            .collect::<Vec<_>>()
            .join(" and ")
    };
    match (found.source_kind, found.steam_app_id) {
        (SourceKind::Steam, Some(id)) => format!("Steam (app {id})"),
        (SourceKind::Steam, None) if !found.steam_folder_apps.is_empty() => format!(
            "Steam, in the folder of app {} (app {} itself isn't installed)",
            apps(&found.steam_folder_apps),
            found.install.game.game().steam_app_id
        ),
        (SourceKind::Steam, None) => {
            "Steam library (found by its folder name; no app manifest)".to_string()
        }
        (SourceKind::Folder, _) => "folder".to_string(),
    }
}

/// Whether United Offensive came from Steam without Call of Duty's own app
/// and lacks archives of Call of Duty's `main/`: Steam shares only Call of
/// Duty's base content with United Offensive, not its language archives.
fn needs_cod_from_steam(found: &Found, missing: &[String]) -> bool {
    found.install.game == GameId::CodUo
        && found.source_kind == SourceKind::Steam
        && !found
            .steam_folder_apps
            .contains(&GameId::Cod.game().steam_app_id)
        && missing.iter().any(|m| m.starts_with("main/"))
}

/// A hint for archives that are missing for a known reason.
fn missing_hint(found: &Found, missing: &[String]) {
    let cod_app = GameId::Cod.game().steam_app_id;
    if needs_cod_from_steam(found, missing) {
        say!(
            "  hint      United Offensive uses Call of Duty's archives in main/; install Call of \
             Duty (Steam app {cod_app}) too, in the same Steam library"
        );
    }
}

fn describe(found: &Found) {
    let install = &found.install;
    let game = install.game.game();
    say!();
    say!("{} ({})", game.name, game.key);
    let version = install.version.as_deref().unwrap_or("unknown");
    say!(
        "  version   {version} (the engine targets {})",
        game.target_version
    );
    let languages = if install.languages.is_empty() {
        "none found".to_string()
    } else {
        install.languages.join(", ")
    };
    say!("  language  {languages}");
    say!("  source    {}", source_text(found));
    say!("  folder    {}", install.root.display());
    if let Some(prefix) = &found.proton_prefix {
        say!(
            "  proton    {} (information only; never read)",
            prefix.display()
        );
    }
    let total: u64 = install.archives.iter().map(|a| a.size).sum();
    let missing = install.missing_any();
    let missing_text = if missing.is_empty() {
        "none missing".to_string()
    } else {
        format!("{} missing", missing.len())
    };
    say!(
        "  archives  {} found, {}, {missing_text}",
        install.archives.len(),
        size_text(total)
    );
    if !missing.is_empty() {
        say!("  missing   {}", short_list(&missing, 6));
        missing_hint(found, &missing);
    }
    let unexpected: Vec<String> = install
        .unexpected()
        .iter()
        .map(|a| a.copy_path.clone())
        .collect();
    if !unexpected.is_empty() {
        say!(
            "  extra     {} (not in the {} set; copied too)",
            short_list(&unexpected, 6),
            game.target_version
        );
    }
    if !install.executables_seen.is_empty() {
        say!(
            "  programs  {} (recognized by name only; never read or copied)",
            install.executables_seen.join(", ")
        );
    }
    for warning in &install.warnings {
        say!("  note      {warning}");
    }
}

fn find(options: &Options) -> Result<(), Failure> {
    let scan = scan_steam(options.steam.as_deref())?;
    print_steam_summary(&scan);
    let mut all = Found::from_steam(&scan);
    for dir in &options.from {
        if !dir.is_dir() {
            return Err(Failure::Failed(format!(
                "--from {}: not a folder",
                dir.display()
            )));
        }
        let found = Found::from_folder(dir);
        if found.is_empty() {
            say!(
                "\n{}: no Call of Duty, United Offensive or Call of Duty 2 install recognized",
                dir.display()
            );
        }
        all.extend(found);
    }
    for found in &all {
        describe(found);
    }
    print_proton_builds(&scan);
    if !scan.warnings.is_empty() {
        say!();
        for warning in &scan.warnings {
            say!("Note: {warning}");
        }
    }
    if all.is_empty() {
        say!("\nNo installs found. Point at a game folder with: newcopy find --from <dir>");
    }
    Ok(())
}

/// The install of `game` to copy from: the `--from` folder, or the one
/// Steam has.
fn pick_source(game: GameId, options: &Options) -> Result<Found, Failure> {
    let name = game.game().name;
    if options.from.len() > 1 {
        return Err(Failure::Usage("make takes one --from folder".into()));
    }
    if let Some(dir) = options.from.first() {
        if options.steam.is_some() {
            return Err(Failure::Usage(
                "give either --from or --steam, not both".into(),
            ));
        }
        if !dir.is_dir() {
            return Err(Failure::Failed(format!(
                "--from {}: not a folder",
                dir.display()
            )));
        }
        return Found::from_folder(dir)
            .into_iter()
            .find(|f| f.install.game == game)
            .ok_or_else(|| {
                let g = game.game();
                Failure::Failed(format!(
                    "no {name} install recognized in {} (looked for its {}/ folder of archives \
                     and for {})",
                    dir.display(),
                    g.identifying_folder.name,
                    g.executables
                        .iter()
                        .filter(|e| e.ends_with(".exe"))
                        .copied()
                        .collect::<Vec<_>>()
                        .join(" or ")
                ))
            });
    }
    let scan = scan_steam(options.steam.as_deref())?;
    let mut matches: Vec<Found> = Found::from_steam(&scan)
        .into_iter()
        .filter(|f| f.install.game == game)
        .collect();
    match matches.len() {
        0 => {
            let searched = if scan.roots.is_empty() {
                "Steam wasn't found".to_string()
            } else {
                format!(
                    "searched {}",
                    scan.libraries
                        .iter()
                        .map(|l| l.display().to_string())
                        .collect::<Vec<_>>()
                        .join(", ")
                )
            };
            Err(Failure::Failed(format!(
                "{name} wasn't found in Steam ({searched}). Point at an install with --from <dir>, \
                 or at Steam with --steam <dir>"
            )))
        }
        1 => Ok(matches.remove(0)),
        _ => Err(Failure::Failed(format!(
            "{name} is in several places; pick one with --from <dir>: {}",
            matches
                .iter()
                .map(|f| f.install.root.display().to_string())
                .collect::<Vec<_>>()
                .join(", ")
        ))),
    }
}

fn make(options: &Options) -> Result<(), Failure> {
    let key = match options.positional.as_slice() {
        [key] => key,
        [] => {
            return Err(Failure::Usage(
                "make needs a game: newcopy make <cod|coduo|cod2> --out <dir>".into(),
            ))
        }
        _ => return Err(Failure::Usage("make takes one game".into())),
    };
    let game = GameId::from_key(key)
        .ok_or_else(|| Failure::Usage(format!("unknown game {key}; use cod, coduo or cod2")))?;
    let out = options
        .out
        .clone()
        .ok_or_else(|| Failure::Usage("make needs --out <dir>".into()))?;
    let found = pick_source(game, options)?;
    let install = &found.install;

    say!(
        "Making a new copy of {} ({}), version {}",
        game.game().name,
        game.key(),
        install.version.as_deref().unwrap_or("unknown")
    );
    say!("  from    {}", install.root.display());
    say!("  source  {}", source_text(&found));
    say!("  to      {}", out.join(game.key()).display());
    if options.allow_missing {
        let missing = install.missing(
            options
                .language
                .as_deref()
                .or(install.languages.first().map(String::as_str)),
        );
        if !missing.is_empty() {
            say!(
                "  warning: {} archive(s) missing, copying anyway: {}",
                missing.len(),
                short_list(&missing, 6)
            );
        }
    }

    let request = MakeRequest {
        install,
        source_kind: found.source_kind,
        steam_app_id: found.steam_app_id,
        out,
        language: options.language.clone(),
        force_copy: options.copy,
        allow_missing: options.allow_missing,
        tool: TOOL,
        tool_version: VERSION,
    };
    let mut on_progress = |event: Progress<'_>| match event {
        Progress::CheckingArchives(count) => {
            say!("Checking {count} archives are whole ZIP files...");
        }
        Progress::FileDone { index, total, file } => {
            let width = total.to_string().len();
            say!(
                "  [{index:>width$}/{total}] {:<9} {} ({})",
                file.action.as_str(),
                file.path,
                size_text(file.size)
            );
        }
    };
    let result = make_copy(&request, &mut on_progress).map_err(|e| {
        let mut message = e.to_string();
        if let MakeError::Missing(missing) = &e {
            if needs_cod_from_steam(&found, missing) {
                message.push_str(&format!(
                    "\nUnited Offensive uses Call of Duty's archives in main/; install Call of \
                         Duty (Steam app {}) too, in the same Steam library",
                    GameId::Cod.game().steam_app_id
                ));
            }
        }
        Failure::Failed(message)
    })?;

    let count = |action: FileAction| result.files.iter().filter(|f| f.action == action).count();
    let total: u64 = result.files.iter().map(|f| f.size).sum();
    say!(
        "Done: {} files, {} ({} linked, {} copied, {} unchanged), language {}.",
        result.files.len(),
        size_text(total),
        count(FileAction::Linked),
        count(FileAction::Copied),
        count(FileAction::Unchanged),
        result.language.as_deref().unwrap_or("none")
    );
    if !result.removed.is_empty() {
        say!(
            "Removed {} file(s) of the earlier copy: {}",
            result.removed.len(),
            short_list(&result.removed, 6)
        );
    }
    if result.files.iter().any(|f| f.hard_link) {
        say!(
            "Hard-linked files share the disk space of the originals. If you'd rather have an \
             independent copy, run again with --copy."
        );
    }
    say!("Manifest: {}", result.manifest_path.display());
    Ok(())
}

fn check(dir: &Path) -> Result<(), Failure> {
    if !dir.is_dir() {
        return Err(Failure::Failed(format!("{}: not a folder", dir.display())));
    }
    let copies = copies_in(dir);
    if copies.is_empty() {
        return Err(Failure::Failed(format!(
            "no newcopy.toml in {} or in its cod, coduo or cod2 folders",
            dir.display()
        )));
    }
    let mut all_good = true;
    for copy_dir in copies {
        say!("Checking {} (every byte is read)...", copy_dir.display());
        let report = check_copy(&copy_dir, &mut |_| {}).map_err(Failure::Failed)?;
        let manifest = &report.manifest;
        say!(
            "  {} {}, {}, made {} from {} ({})",
            manifest.game_name,
            manifest.version.as_deref().unwrap_or("(version unknown)"),
            manifest.language.as_deref().unwrap_or("no language"),
            manifest.made,
            manifest.source_path,
            manifest.source_kind
        );
        if report.problems.is_empty() {
            say!("  OK: all {} files match.", manifest.files.len());
        } else {
            all_good = false;
            say!(
                "  {} problem(s) (run `newcopy make` again to repair):",
                report.problems.len()
            );
            for problem in &report.problems {
                say!("    {problem}");
            }
        }
        if !report.unlisted.is_empty() {
            say!(
                "  Not in the manifest (left alone): {}",
                short_list(&report.unlisted, 6)
            );
        }
    }
    if all_good {
        Ok(())
    } else {
        Err(Failure::Failed(
            "the copy doesn't match its manifest".into(),
        ))
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn args(list: &[&str]) -> Vec<String> {
        list.iter().map(|s| s.to_string()).collect()
    }

    #[test]
    fn parses_make() {
        let options = parse_args(&args(&[
            "make",
            "cod2",
            "--out",
            "copies",
            "--from=game dir",
            "--copy",
            "--language",
            "english",
        ]))
        .unwrap();
        assert_eq!(options.command, "make");
        assert_eq!(options.positional, vec!["cod2"]);
        assert_eq!(options.out, Some(PathBuf::from("copies")));
        assert_eq!(options.from, vec![PathBuf::from("game dir")]);
        assert!(options.copy);
        assert_eq!(options.language.as_deref(), Some("english"));
    }

    #[test]
    fn bad_arguments() {
        assert!(parse_args(&args(&["make", "--out"])).is_err());
        assert!(parse_args(&args(&["make", "--out", "--copy"])).is_err());
        assert!(parse_args(&args(&["find", "--frm", "x"])).is_err());
        assert!(parse_args(&args(&["make", "--out", "a", "--out", "b"])).is_err());
    }

    #[test]
    fn sizes() {
        assert_eq!(size_text(999), "999 bytes");
        assert_eq!(size_text(170_200_000), "170.2 MB");
        assert_eq!(size_text(2_100_000_000), "2.1 GB");
    }
}
