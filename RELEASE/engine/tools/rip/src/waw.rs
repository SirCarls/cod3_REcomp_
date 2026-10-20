//! World at War fastfiles (PC and Xbox 360).

use crate::paths::safe_join;
use ff_t4::layout::Layout;
use std::collections::BTreeMap;
use std::path::{Path, PathBuf};

/// Reading a zone holds the file, the zone and its memory blocks (a large
/// map is a few hundred MB), so `waw-zone` reads at most this many at once.
const ZONE_WORKERS: usize = 2;

/// Lists a WaW fastfile (PC or Xbox 360) and writes its raw files (GSC/CSC scripts) to
/// `<out_dir>/rawfiles/`. With a zone layout, a PC zone is read asset by asset;
/// otherwise (and on Xbox 360) its raw files are found by their inline layout.
pub fn fastfile(
    file: &Path,
    out_dir: &Path,
    layout: Option<&Layout>,
) -> Result<(), Box<dyn std::error::Error>> {
    let named = |e: &dyn std::fmt::Display| format!("{}: {e}", file.display());
    let bytes = std::fs::read(file).map_err(|e| named(&e))?;
    let zone = ff_t4::Zone::from_fastfile(&bytes).map_err(|e| named(&e))?;
    println!(
        "{}: {} zone, {} bytes, {} script strings, {} assets",
        file.display(),
        zone.platform.name(),
        zone.data.len(),
        zone.script_strings.len(),
        zone.assets.len()
    );
    for (t, n) in zone.census() {
        let name = zone.type_name(t);
        println!("  {name:>16} ({t:2}): {n}");
    }
    // PC zones are read asset by asset when a layout is given; otherwise raw
    // files are found by their inline layout.
    let read = layout.map(|layout| zone.read_assets(layout));
    let raw = match read {
        Some(Ok(loaded)) => {
            println!("  {}", summary(&loaded));
            for p in loaded.problems.iter().take(5) {
                println!("    {p}");
            }
            loaded.raw_files()
        }
        None | Some(Err(ff_t4::Error::NotPc)) => zone.scan_raw_files(),
        Some(Err(e)) => return Err(named(&e).into()),
    };
    let dir = out_dir.join("rawfiles");
    for rf in &raw {
        let Some(path) = safe_join(&dir, &rf.name) else {
            eprintln!("  skipped unsafe name {:?}", rf.name);
            continue;
        };
        if let Some(parent) = path.parent() {
            std::fs::create_dir_all(parent)?;
        }
        std::fs::write(&path, &rf.data)?;
    }
    println!("  wrote {} raw files to {}", raw.len(), dir.display());
    Ok(())
}

/// "every asset read exactly: 3174 (localize 2615, material 191, ...)" or
/// "N problems reading the assets".
fn summary(loaded: &ff_t4::LoadedZone) -> String {
    let mut counts: BTreeMap<u32, usize> = BTreeMap::new();
    for a in &loaded.assets {
        *counts.entry(a.type_number).or_insert(0) += 1;
    }
    let mut by_count: Vec<(u32, usize)> = counts.into_iter().collect();
    by_count.sort_by(|a, b| b.1.cmp(&a.1).then(a.0.cmp(&b.0)));
    let list: Vec<String> = by_count
        .iter()
        .map(|(t, n)| {
            let name = ff_t4::AssetType::from_u32(*t)
                .map(|t| t.name())
                .unwrap_or("?");
            format!("{name} {n}")
        })
        .collect();
    let head = if loaded.problems.is_empty() {
        "every asset read exactly".to_string()
    } else {
        format!("{} problems reading the assets", loaded.problems.len())
    };
    format!("{head}: {} ({})", loaded.assets.len(), list.join(", "))
}

/// What reading one fastfile found, for `waw-zone`.
enum ZoneResult {
    Read(ff_t4::LoadedZone),
    /// Not a World at War PC fastfile (Xbox 360 zones are not read asset by asset yet).
    Skipped(String),
}

fn read_zone(file: &Path, layout: &Layout) -> Result<ZoneResult, String> {
    let bytes = std::fs::read(file).map_err(|e| e.to_string())?;
    let zone = match ff_t4::Zone::from_fastfile(&bytes) {
        Ok(zone) => zone,
        Err(e @ ff_t4::Error::OtherGame(_)) => return Ok(ZoneResult::Skipped(e.to_string())),
        Err(e) => return Err(e.to_string()),
    };
    drop(bytes);
    match zone.read_assets(layout) {
        Ok(loaded) => Ok(ZoneResult::Read(loaded)),
        Err(ff_t4::Error::NotPc) => Ok(ZoneResult::Skipped(
            "an Xbox 360 zone (read asset by asset only on PC so far)".into(),
        )),
        Err(e) => Err(e.to_string()),
    }
}

/// `rip waw-zone <file.ff | folder> --layout <file> [--jobs N]`: reads every
/// asset of each World at War PC fastfile the way the game's loader does,
/// following the zone layout, and checks it (the stream ends at the zone's
/// end, every block at the size the header gives, every reference lands on
/// something loaded before). True when every file read exactly.
pub fn zones(
    input: &Path,
    layout: Option<&Layout>,
    workers: usize,
) -> Result<bool, Box<dyn std::error::Error>> {
    let Some(layout) = layout else {
        return Err(
            "waw-zone needs the zone layout: --layout <file> (or RIP_WAW_LAYOUT); \
                    World at War PC's is not published, see docs/formats/waw.md"
                .into(),
        );
    };
    let files: Vec<PathBuf> = if input.is_dir() {
        let found = crate::jobs::find_files(input, "ff")?;
        if found.is_empty() {
            return Err(format!("no .ff files under {}", input.display()).into());
        }
        found
    } else {
        vec![input.to_path_buf()]
    };
    let (mut exact, mut with_problems, mut failed, mut skipped) = (0usize, 0usize, 0usize, 0usize);
    let mut assets = 0usize;
    crate::jobs::for_each_parallel(
        &files,
        workers.min(ZONE_WORKERS),
        |file| read_zone(file, layout),
        |file, result| match result {
            Ok(ZoneResult::Read(loaded)) => {
                println!("{}: {}", file.display(), summary(&loaded));
                for p in loaded.problems.iter().take(5) {
                    println!("    {p}");
                }
                assets += loaded.assets.len();
                if loaded.problems.is_empty() {
                    exact += 1;
                } else {
                    with_problems += 1;
                }
            }
            Ok(ZoneResult::Skipped(why)) => {
                println!("{}: skipped: {why}", file.display());
                skipped += 1;
            }
            Err(e) => {
                eprintln!("error: {}: {e}", file.display());
                failed += 1;
            }
        },
    );
    if files.len() > 1 {
        println!(
            "{} files: {exact} read exactly ({assets} assets), {with_problems} with problems, \
             {failed} unreadable, {skipped} skipped",
            files.len()
        );
    }
    Ok(with_problems == 0 && failed == 0)
}
