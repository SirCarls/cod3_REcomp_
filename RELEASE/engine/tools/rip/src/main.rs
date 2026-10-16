//! rip: pulls assets out of the user's own CoD3 and World at War files into
//! open formats for the engine (and for the Unreal 5 export).
//!
//! ```text
//! rip waw-ff <file.ff> <out_dir>          list a WaW fastfile (PC or Xbox 360), write its raw files
//! rip cod3-split <file.cod> <out_dir>     split a CoD3 archive into its resources
//! rip cod3-chunks <file.cod> <out_dir>    write each unpacked chunk whole (research)
//! rip cod3-tex <file.cod> <out_dir>       textures as DDS + PNG
//! rip cod3-mesh <file.cod> <out_dir>      meshes as glTF with materials
//! rip cod3-check <file.cod|folder>        check the archive layout rules
//! rip cod3-all <folder> <out_dir>         textures, meshes and materials of every
//!                                         .cod file under the folder
//! rip cod3-hd-pack <textures> <upscaled> <out_dir> [--kinds diffuse,normal,...|all]
//!                                         the HD texture pack: upscaled PNGs (any
//!                                         scale) as DDS in the game's formats,
//!                                         with index.tsv; color maps unless
//!                                         --kinds says more
//! rip cod3-user-pack <textures> <png dir> <out_dir> [--list <insignia list>]
//!                                         the user texture pack: the player's own
//!                                         PNGs (named like the ripped ones) as DDS,
//!                                         with index.tsv, checked (see `user`)
//! rip cod3-check-pack <pack dir>          check a texture pack with the game's rules
//! rip cod3-locate <file.cod|folder> <name|key>...
//!                                         where textures are in the archives
//! rip version                             version and output layout number
//! ```
//!
//! `cod3-tex` and `cod3-all` also write `textures/hd_keys.tsv`, the key of
//! every texture for the HD texture pack (see `hd`). `cod3-check`,
//! `cod3-all` and `cod3-hd-pack` use every CPU core; `--jobs N` limits them.
//! Output files whose content didn't change are not rewritten.
//! Paths can be given in Linux or Windows form; nothing is hard-coded.

#![forbid(unsafe_code)]

mod cod3;
mod hd;
mod jobs;
mod output;
mod pack_check;
mod paths;
mod user;
mod waw;

use std::collections::BTreeSet;
use std::path::{Path, PathBuf};
use std::process::ExitCode;
use std::sync::Mutex;

const USAGE: &str = "usage:
  rip waw-ff <file.ff> <out_dir>
  rip cod3-split <file.cod> <out_dir>
  rip cod3-chunks <file.cod> <out_dir>
  rip cod3-tex <file.cod> <out_dir>
  rip cod3-mesh <file.cod> <out_dir>
  rip cod3-check <file.cod | folder> [--jobs N]
  rip cod3-all <folder> <out_dir> [--jobs N]
  rip cod3-hd-pack <textures dir> <upscaled dir> <out_dir> [--kinds diffuse,normal,...|all] [--jobs N]
  rip cod3-user-pack <textures dir> <png dir> <out_dir> [--list <insignia list>] [--jobs N]
  rip cod3-check-pack <pack dir>
  rip cod3-locate <file.cod | folder> <name | key>... [--jobs N]
  rip version";

/// Bumped whenever the output folders or file formats change, so the setup
/// tool knows whether a prebuilt rip fits (c3w/rippers.py RIP_OUTPUT_VERSION).
/// 2: textures/hd_keys.tsv, the cod3-hd-pack command, unchanged files kept.
/// 3: cod3-hd-pack takes upscaled PNGs of any scale (not only 2x).
/// 4: hd_keys.tsv names the PNG of each key's own picture (other pictures
///    under a name are saved as `<name>__<key>.png`), lightmaps and lookup
///    tables are left out of the HD pack.
/// 5: the cod3-user-pack, cod3-check-pack and cod3-locate commands (the
///    user texture pack; the setup tool needs them).
/// 6: hd_keys.tsv has a `kind` column (what each texture holds, from the
///    materials); the pack takes color maps only unless told otherwise;
///    textures with a side of 16 texels or less are read from the right
///    place (the packed mip tile) and 8_8_8_8 pictures get their channels
///    in the right order.
const OUTPUT_LAYOUT: u32 = 6;

type Result<T> = std::result::Result<T, Box<dyn std::error::Error>>;

/// Splits `--jobs N` out of the arguments.
fn take_jobs(args: &mut Vec<String>) -> Result<Option<usize>> {
    let Some(value) = take_option(args, "--jobs")? else {
        return Ok(None);
    };
    Ok(Some(value.parse()?))
}

/// Splits `<option> <value>` out of the arguments.
fn take_option(args: &mut Vec<String>, option: &str) -> Result<Option<String>> {
    let Some(i) = args.iter().position(|a| a == option) else {
        return Ok(None);
    };
    let value = args
        .get(i + 1)
        .ok_or_else(|| format!("{option} needs a value"))?
        .clone();
    args.drain(i..i + 2);
    Ok(Some(value))
}

/// `rip cod3-locate`: the wanted textures' places in every archive.
fn cod3_locate(input: &Path, wanted: &[String], workers: usize) -> Result<bool> {
    let (root, files) = cod_files(input)?;
    let wanted = cod3::Wanted::from_args(wanted);
    println!(
        "# archive\tsection\tchunk\tchunk's place in the file\tTEX object in the unpacked \
         chunk\tpixel data in the unpacked chunk\tname\tsize\tformat\tkey"
    );
    let failures = jobs::run_all(&files, workers, |file| {
        cod3::locate(file, &paths::archive_label(root.as_deref(), file), &wanted)
    });
    Ok(failures == 0)
}

/// One file, or every `.cod` file under a folder.
fn cod_files(input: &Path) -> Result<(Option<PathBuf>, Vec<PathBuf>)> {
    if input.is_dir() {
        let files = jobs::find_files(input, "cod")?;
        if files.is_empty() {
            return Err(format!("no .cod files under {}", input.display()).into());
        }
        Ok((Some(input.to_path_buf()), files))
    } else {
        Ok((None, vec![input.to_path_buf()]))
    }
}

fn cod3_check(input: &Path, workers: usize) -> Result<bool> {
    let (_, files) = cod_files(input)?;
    let all_ok = std::sync::atomic::AtomicBool::new(true);
    let failures = jobs::run_all(&files, workers, |file| {
        let (report, ok) = cod3::check(file)?;
        if !ok {
            all_ok.store(false, std::sync::atomic::Ordering::Relaxed);
        }
        Ok(report)
    });
    println!(
        "checked {} archives: {}",
        files.len(),
        if failures == 0 && all_ok.into_inner() {
            "all rules hold"
        } else {
            "some rules don't hold (see above)"
        }
    );
    Ok(failures == 0)
}

fn cod3_all(input: &Path, out_dir: &Path, workers: usize) -> Result<bool> {
    let (root, files) = cod_files(input)?;
    println!("{} archives, {workers} worker threads", files.len());
    let claims = jobs::Claims::default();
    let keys = hd::KeyList::default();
    let used_textures = Mutex::new(BTreeSet::new());
    let failures = jobs::run_all(&files, workers, |file| {
        let label = paths::archive_label(root.as_deref(), file);
        let mut report = cod3::textures(file, out_dir, &claims, &keys)?;
        report.push_str(&cod3::meshes(file, &label, out_dir, &used_textures)?);
        Ok(report)
    });
    print!("{}", keys.write(&out_dir.join("textures"))?);
    let missing: Vec<String> = used_textures
        .into_inner()
        .unwrap_or_else(|e| e.into_inner())
        .into_iter()
        .filter(|t| !claims.contains(t))
        .collect();
    let written_count = files.len() - failures;
    println!("done: {written_count} archives ripped, {failures} failed");
    if !missing.is_empty() {
        println!(
            "{} textures are used by materials but were not found in any archive",
            missing.len()
        );
    }
    Ok(failures == 0)
}

fn run(mut args: Vec<String>) -> Result<bool> {
    let jobs = take_jobs(&mut args)?;
    let list = take_option(&mut args, "--list")?;
    let kinds = match take_option(&mut args, "--kinds")? {
        Some(text) => hd::Kind::parse_list(&text)?,
        None => hd::Kind::default_set(),
    };
    let workers = jobs::worker_count(jobs);
    let path = |i: usize| Path::new(&args[i]);
    match (args.get(1).map(String::as_str), args.len()) {
        (Some("waw-ff"), 4) => waw::fastfile(path(2), path(3)).map(|_| true),
        (Some("cod3-split"), 4) => cod3::split(path(2), path(3)).map(|_| true),
        (Some("cod3-chunks"), 4) => cod3::chunks(path(2), path(3)).map(|_| true),
        (Some("cod3-tex"), 4) => {
            let keys = hd::KeyList::default();
            print!(
                "{}",
                cod3::textures(path(2), path(3), &jobs::Claims::default(), &keys)?
            );
            print!("{}", keys.write(&path(3).join("textures"))?);
            Ok(true)
        }
        (Some("cod3-mesh"), 4) => {
            let label = paths::archive_label(None, path(2));
            let used = Mutex::new(BTreeSet::new());
            print!("{}", cod3::meshes(path(2), &label, path(3), &used)?);
            Ok(true)
        }
        (Some("cod3-check"), 3) => cod3_check(path(2), workers),
        (Some("cod3-all"), 4) => cod3_all(path(2), path(3), workers),
        (Some("cod3-hd-pack"), 5) => {
            hd::pack(path(2), path(3), path(4), &kinds, workers).map(|summary| summary.failed == 0)
        }
        (Some("cod3-user-pack"), 5) => user::pack_user(
            path(2),
            path(3),
            path(4),
            list.as_deref().map(Path::new),
            workers,
        )
        .map(|_| true),
        (Some("cod3-check-pack"), 3) => {
            pack_check::check_pack(path(2)).map(|summary| summary.bad_files == 0)
        }
        (Some("cod3-locate"), n) if n >= 4 => cod3_locate(path(2), &args[3..], workers),
        (Some("version"), 2) => {
            println!(
                "rip {}, output layout {OUTPUT_LAYOUT}",
                env!("CARGO_PKG_VERSION")
            );
            Ok(true)
        }
        _ => {
            eprintln!("{USAGE}");
            Err("unknown command or wrong number of arguments".into())
        }
    }
}

fn main() -> ExitCode {
    match run(std::env::args().collect()) {
        Ok(true) => ExitCode::SUCCESS,
        Ok(false) => ExitCode::FAILURE,
        Err(e) => {
            eprintln!("error: {e}");
            ExitCode::FAILURE
        }
    }
}
