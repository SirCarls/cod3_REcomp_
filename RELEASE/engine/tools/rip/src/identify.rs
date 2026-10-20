//! `rip identify`: which game each fastfile (`.ff`) or archive (`.cod`) comes
//! from, read from its header (the `fastfile` and `kapf` crates), and with
//! `--open` what its container holds: the zone's size and blocks (checked
//! against the zone header), World at War's asset count (on PC, given a zone
//! layout with `--layout`, every asset read and checked, see
//! `ff_t4::Zone::read_assets`), an archive's sections and chunks.

use std::collections::BTreeMap;
use std::io::Read;
use std::path::{Path, PathBuf};

use ff_t4::layout::Layout;

type Result<T> = std::result::Result<T, Box<dyn std::error::Error>>;

/// Opening a file holds it and its zone in memory (a zone is a few times the
/// file), so `--open` runs at most this many files at once.
const OPEN_WORKERS: usize = 4;

/// What one file is.
struct Found {
    /// The game and platform, for the summary ("" when not known).
    game: String,
    /// The line printed for the file.
    detail: String,
}

fn not_known(detail: String) -> Found {
    Found {
        game: String::new(),
        detail,
    }
}

/// The first bytes of a file: enough for either header.
fn read_head(file: &Path) -> Result<Vec<u8>> {
    let mut head = Vec::with_capacity(fastfile::IDENTIFY_SIZE);
    std::fs::File::open(file)?
        .take(fastfile::IDENTIFY_SIZE as u64)
        .read_to_end(&mut head)?;
    Ok(head)
}

fn describe(file: &Path, open: bool, layout: Option<&Layout>) -> Result<Found> {
    let bytes = if open {
        std::fs::read(file)?
    } else {
        read_head(file)?
    };
    if bytes.starts_with(kapf::MAGIC) {
        Ok(archive(&bytes, open))
    } else {
        Ok(fastfile_found(&bytes, open, layout))
    }
}

fn fastfile_found(bytes: &[u8], open: bool, layout: Option<&Layout>) -> Found {
    let head = &bytes[..bytes.len().min(fastfile::IDENTIFY_SIZE)];
    let Some(identity) = fastfile::identify(head) else {
        let why = fastfile::read(head).err().map(|e| e.to_string());
        return not_known(why.unwrap_or_else(|| "not a fastfile".into()));
    };
    let game = format!("{} ({})", identity.game.name(), identity.platform.name());
    if !open {
        return Found {
            game,
            detail: identity.to_string(),
        };
    }
    match fastfile::read(bytes) {
        // Reading can tell more than the header (which console it is).
        Ok(zone) => Found {
            game: format!(
                "{} ({})",
                zone.identity.game.name(),
                zone.identity.platform.name()
            ),
            detail: zone_detail(zone, layout),
        },
        Err(e) => Found {
            game,
            detail: e.to_string(),
        },
    }
}

/// "<identity>: zone N bytes, B blocks[, size check][, World at War's counts]".
fn zone_detail(zone: fastfile::Zone, layout: Option<&Layout>) -> String {
    let identity = zone.identity;
    let mut detail = format!(
        "{identity}: zone {} bytes, {} blocks",
        zone.data.len(),
        zone.header.block_sizes.len()
    );
    if !zone.size_matches() {
        detail += &format!(
            ", but its header gives {} bytes after the header (the layout may be wrong \
             for this file)",
            zone.header.size
        );
    }
    let platform = match identity.platform {
        fastfile::Platform::Pc => Some(ff_t4::Platform::Pc),
        fastfile::Platform::Xbox360 => Some(ff_t4::Platform::Xbox360),
        _ => None,
    };
    if let (fastfile::Game::WorldAtWar, Some(platform)) = (identity.game, platform) {
        match ff_t4::Zone::from_zone(zone.data, platform) {
            Ok(t4) => {
                detail += &format!(
                    ", {} script strings, {} assets",
                    t4.script_strings.len(),
                    t4.assets.len()
                );
                // PC zones are read asset by asset, the way the game's loader
                // does, when a layout is given.
                let Some(layout) = layout else {
                    return detail;
                };
                match t4.read_assets(layout) {
                    Ok(loaded) if loaded.problems.is_empty() => {
                        detail += &format!(
                            ", every asset read exactly ({} with the ones inside)",
                            loaded.assets.len()
                        )
                    }
                    Ok(loaded) => {
                        detail += &format!(
                            ", {} problems reading the assets: {}",
                            loaded.problems.len(),
                            loaded.problems[0]
                        )
                    }
                    Err(ff_t4::Error::NotPc) => {}
                    Err(e) => detail += &format!(", assets unreadable: {e}"),
                }
            }
            Err(e) => detail += &format!(", asset list unreadable: {e}"),
        }
    }
    detail
}

fn archive(bytes: &[u8], open: bool) -> Found {
    let Some(version) = bytes.get(4..8) else {
        return not_known("a .cod archive cut short".into());
    };
    let version = u32::from_le_bytes([version[0], version[1], version[2], version[3]]);
    let Some(game) = kapf::Game::from_version(version) else {
        return not_known(format!(
            "a .cod archive (KAPF) of a version no game here uses ({version:#x})"
        ));
    };
    let label = format!("{} ({})", game.name(), game.platform());
    let mut detail = format!("{label} archive, version {version:#x}");
    if open {
        match kapf::Archive::parse(bytes) {
            Ok(parsed) => {
                let names: Vec<&str> = parsed
                    .sections
                    .iter()
                    .map(|s| {
                        if s.name.is_empty() {
                            "main"
                        } else {
                            s.name.as_str()
                        }
                    })
                    .collect();
                let chunks: usize = parsed.sections.iter().map(|s| s.chunks.len()).sum();
                detail += &format!(
                    ": {} sections ({}), {chunks} chunks",
                    names.len(),
                    names.join(", ")
                );
                // Every chunk lies inside the file when the table is read
                // with the right layout.
                let outside = parsed
                    .sections
                    .iter()
                    .flat_map(|s| &s.chunks)
                    .filter(|c| {
                        let stored = if c.is_packed() {
                            c.stored_size
                        } else {
                            c.data_size
                        };
                        (c.file_offset as usize).saturating_add(stored as usize) > bytes.len()
                    })
                    .count();
                if outside > 0 {
                    detail += &format!(
                        ", {outside} of them outside the file (the layout may be wrong for it)"
                    );
                }
            }
            Err(e) => detail += &format!(": {e}"),
        }
    }
    Found {
        game: label,
        detail,
    }
}

/// `rip identify <file | folder> [--open] [--layout <file>]`. Prints one line
/// per file and a count per game; false when some file isn't one we know.
pub fn identify(input: &Path, open: bool, layout: Option<&Layout>, workers: usize) -> Result<bool> {
    let files: Vec<PathBuf> = if input.is_dir() {
        let mut found = crate::jobs::find_files(input, "ff")?;
        found.extend(crate::jobs::find_files(input, "cod")?);
        found.sort();
        if found.is_empty() {
            return Err(format!("no .ff or .cod files under {}", input.display()).into());
        }
        found
    } else {
        vec![input.to_path_buf()]
    };
    let workers = if open {
        workers.min(OPEN_WORKERS)
    } else {
        workers
    };
    let mut per_game: BTreeMap<String, usize> = BTreeMap::new();
    let mut unknown = 0usize;
    crate::jobs::for_each_parallel(
        &files,
        workers,
        |file| describe(file, open, layout).map_err(|e| e.to_string()),
        |file, result| match result {
            Ok(found) => {
                println!("{}: {}", file.display(), found.detail);
                if found.game.is_empty() {
                    unknown += 1;
                } else {
                    *per_game.entry(found.game).or_insert(0) += 1;
                }
            }
            Err(e) => {
                unknown += 1;
                eprintln!("error: {}: {e}", file.display());
            }
        },
    );
    if files.len() > 1 {
        println!("{} files:", files.len());
        for (game, count) in &per_game {
            println!("  {count:5}  {game}");
        }
        if unknown > 0 {
            println!("  {unknown:5}  not known");
        }
    }
    Ok(unknown == 0)
}
