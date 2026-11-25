//! `rip waw-images`: the textures of World at War Xbox 360 fastfiles as DDS
//! and PNG, with the disc's higher-resolution `highmip/` levels where a
//! texture has one.
//!
//! The image records and where their pixels are come from
//! `ff_t4::Zone::xbox_images` (docs/formats/waw.md). Each texture is
//! untiled with the `xenos` crate, as Call of Duty 3's are.
//!
//! **highmip.** A texture whose record says "streamed" has a file
//! `highmip/<name>.hi` on the disc: its top level at twice the width and
//! height, tiled the same way and in the same format, with no header (the
//! file is exactly that level's tiled size: a 512 x 512 DXT1 texture in the
//! zone has a 524,288-byte `.hi`, 1024 x 1024 DXT1). That is the
//! resolution the console shows up close, so it is the one written; the
//! zone's own top level is written beside it as `<name>.zone.png`.

use crate::hd::{keep_original, KeyLine, Kind, KEYS_FILE, NO_FILE};
use crate::output::write_if_changed;
use crate::paths::sanitize;
use std::path::{Path, PathBuf};

/// Reading a zone holds it whole (a level is a few hundred MB), so at most
/// this many are read at once.
const ZONE_WORKERS: usize = 2;
/// Tiled surfaces are padded to 32 texels in each direction.
const TILE: u32 = 32;

#[derive(Default)]
struct Counts {
    written: usize,
    highmip: usize,
    no_pixels: usize,
    not_2d: usize,
    unsupported: usize,
}

/// Every `.ff` under a folder, or one file.
fn fastfiles(input: &Path) -> Result<Vec<PathBuf>, Box<dyn std::error::Error>> {
    if input.is_dir() {
        let files = crate::jobs::find_files(input, "ff")?;
        if files.is_empty() {
            return Err(format!("no .ff files under {}", input.display()).into());
        }
        Ok(files)
    } else {
        Ok(vec![input.to_path_buf()])
    }
}

/// `rip waw-images <file.ff | folder> <out_dir> [--highmip <dir>]`. Without
/// `--highmip`, the `highmip` folder beside each fastfile is used if there
/// is one.
pub fn images(
    input: &Path,
    out_dir: &Path,
    highmip: Option<&Path>,
    workers: usize,
) -> Result<bool, Box<dyn std::error::Error>> {
    let files = fastfiles(input)?;
    let failures = crate::jobs::run_all(&files, workers.min(ZONE_WORKERS), |file| {
        let beside = file.parent().map(|p| p.join("highmip"));
        let hi = highmip
            .map(Path::to_path_buf)
            .or_else(|| beside.filter(|p| p.is_dir()));
        one_fastfile(file, out_dir, hi.as_deref())
    });
    Ok(failures == 0)
}

/// One readable 2D texture of a zone: its name, its top level untiled, and
/// the disc's highmip level of it when there is one.
struct Texture<'a> {
    /// The record's name, or `unnamed_<record offset>`.
    name: String,
    /// `name` sanitized, made unique within the zone.
    base: String,
    fetch: &'a xenos::FetchConstant,
    linear: &'a [u8],
    hi: Option<&'a (xenos::FetchConstant, Vec<u8>)>,
}

/// Reads one fastfile and calls `each` for every 2D texture whose top level
/// can be untiled. `each` returns false for a texture whose format it can't
/// write; that counts as unsupported. Returns the counts, the number of
/// image records and the reader's problems, for `report`.
fn each_texture(
    file: &Path,
    highmip: Option<&Path>,
    mut each: impl FnMut(&Texture) -> std::io::Result<bool>,
) -> Result<(Counts, usize, Vec<String>), Box<dyn std::error::Error>> {
    let named = |e: &dyn std::fmt::Display| format!("{}: {e}", file.display());
    let bytes = std::fs::read(file).map_err(|e| named(&e))?;
    let zone = ff_t4::Zone::from_fastfile(&bytes).map_err(|e| named(&e))?;
    drop(bytes);
    let found = zone.xbox_images().map_err(|e| named(&e))?;

    let mut counts = Counts::default();
    let mut used = std::collections::HashSet::new();
    for image in &found.images {
        let Some(pixels) = image.pixels.clone() else {
            counts.no_pixels += 1;
            continue;
        };
        let fetch = xenos::FetchConstant::parse(image.fetch);
        if fetch.dimension != 1 {
            counts.not_2d += 1; // cube maps and volumes: not converted yet
            continue;
        }
        let name = image
            .name
            .clone()
            .unwrap_or_else(|| format!("unnamed_{:x}", image.record_at));
        // Two records can carry one name; keep both.
        let mut base = sanitize(&name);
        if !used.insert(base.clone()) {
            base = format!("{base}__{:x}", image.record_at);
            used.insert(base.clone());
        }
        let Ok(linear) = xenos::base_level_linear(&fetch, &zone.data[pixels]) else {
            counts.unsupported += 1;
            continue;
        };
        let hi = image
            .streamed
            .then_some(image.name.as_deref())
            .flatten()
            .and_then(|name| {
                highmip.map(|dir| dir.join(format!("{}.hi", name.to_ascii_lowercase())))
            })
            .and_then(|path| std::fs::read(path).ok())
            .and_then(|data| highmip_level(&fetch, &data));
        let texture = Texture {
            name,
            base,
            fetch: &fetch,
            linear: &linear,
            hi: hi.as_ref(),
        };
        if !each(&texture)? {
            counts.unsupported += 1;
            continue;
        }
        if hi.is_some() {
            counts.highmip += 1;
        }
        counts.written += 1;
    }
    let problems = found.problems.iter().map(|p| p.to_string()).collect();
    Ok((counts, found.images.len(), problems))
}

/// The summary line of one fastfile.
fn report(file: &Path, images: usize, counts: &Counts, to: &Path, problems: &[String]) -> String {
    let mut report = format!(
        "{}: {} images, {} written ({} at highmip resolution) to {}",
        file.display(),
        images,
        counts.written,
        counts.highmip,
        to.display()
    );
    for (n, what) in [
        (counts.not_2d, "cube or volume (not converted yet)"),
        (counts.unsupported, "in a format not handled yet"),
        (counts.no_pixels, "without pixels"),
    ] {
        if n > 0 {
            report.push_str(&format!("; {n} {what}"));
        }
    }
    report.push('\n');
    for problem in problems {
        report.push_str(&format!("  {problem}\n"));
    }
    report
}

fn one_fastfile(
    file: &Path,
    out_dir: &Path,
    highmip: Option<&Path>,
) -> Result<String, Box<dyn std::error::Error>> {
    let stem = file
        .file_stem()
        .map_or("zone".into(), |s| s.to_string_lossy().into_owned());
    let dir = out_dir.join("images").join(sanitize(&stem));
    std::fs::create_dir_all(&dir)?;
    let (counts, images, problems) = each_texture(file, highmip, |t| {
        let zone_suffix = if t.hi.is_some() { ".zone" } else { "" };
        if !write_texture(&dir, &format!("{}{zone_suffix}", t.base), t.fetch, t.linear)? {
            return Ok(false);
        }
        if let Some((hi_fetch, hi_linear)) = t.hi {
            write_texture(&dir, &t.base, hi_fetch, hi_linear)?;
        }
        Ok(true)
    })?;
    Ok(report(file, images, &counts, &dir, &problems))
}

/// `rip waw-hd-textures <file.ff | folder> <out_dir> [--highmip <dir>]`:
/// the textures the HD pack can replace, as `rip captured-textures` writes
/// them: `<out_dir>/textures/<file>.png` and `textures/hd_keys.tsv`, for the
/// setup tool's upscaler and `cod3-hd-pack`. No playing is needed: every
/// texture of every fastfile given is listed.
///
/// Each texture's key is `xenos::hd_key` of its top level, which is what
/// the game's texture cache hashes. A streamed texture gets two keys, the
/// zone's level and the highmip level (twice the size), because which one
/// is in memory when the game first draws with it depends on how far the
/// streamer has got; each key has its own picture. The same picture in
/// several zones is one key, listed once. Files are named
/// `<name>__<key>` so that two pictures under one name never share a file
/// and a rerun names them the same whatever order the zones finish in.
///
/// What a texture holds is guessed as `rip captured-textures` does (no
/// material is read yet): DXN is a normal map, everything else a color
/// picture, and `hd::keep_original` (lightmaps, small lookup tables) is
/// left as the game has it.
pub fn hd_textures(
    input: &Path,
    out_dir: &Path,
    highmip: Option<&Path>,
    workers: usize,
) -> Result<bool, Box<dyn std::error::Error>> {
    let files = fastfiles(input)?;
    let textures = out_dir.join("textures");
    std::fs::create_dir_all(&textures)?;
    let lines = std::sync::Mutex::new(std::collections::BTreeMap::<u64, KeyLine>::new());
    let failures = crate::jobs::run_all(&files, workers.min(ZONE_WORKERS), |file| {
        let beside = file.parent().map(|p| p.join("highmip"));
        let hi = highmip
            .map(Path::to_path_buf)
            .or_else(|| beside.filter(|p| p.is_dir()));
        let (counts, images, problems) = each_texture(file, hi.as_deref(), |t| {
            if !xenos::hd_replaceable(t.fetch) {
                return Ok(false);
            }
            let mut levels = vec![(t.fetch, t.linear)];
            if let Some((hi_fetch, hi_linear)) = t.hi {
                levels.push((hi_fetch, hi_linear.as_slice()));
            }
            for (fetch, linear) in levels {
                let key = xenos::hd_key(linear);
                // Another zone may have listed this picture already.
                if lines
                    .lock()
                    .unwrap_or_else(|e| e.into_inner())
                    .contains_key(&key)
                {
                    continue;
                }
                let line = key_line(&t.name, key, fetch);
                if line.file != NO_FILE {
                    let Ok(rgba) = xenos::rgba8(fetch, linear) else {
                        return Ok(false);
                    };
                    let png = png_write::encode_rgba(fetch.width, fetch.height, &rgba);
                    write_if_changed(&textures.join(format!("{}.png", line.file)), &png)?;
                }
                lines
                    .lock()
                    .unwrap_or_else(|e| e.into_inner())
                    .entry(key)
                    .or_insert(line);
            }
            Ok(true)
        })?;
        Ok(report(file, images, &counts, &textures, &problems))
    });
    let lines = lines.into_inner().unwrap_or_else(|e| e.into_inner());
    let mut sorted: Vec<&KeyLine> = lines.values().collect();
    sorted.sort();
    let mut text = String::from("# key\twidth\theight\txenos_format\tname\tfile\tkind\n");
    for line in &sorted {
        text.push_str(&line.tsv(&format!(
            "{}\t{}\t{}",
            line.name,
            line.file,
            line.kind.label()
        )));
    }
    write_if_changed(&textures.join(KEYS_FILE), text.as_bytes())?;
    let pictures = sorted.iter().filter(|l| l.file != NO_FILE).count();
    println!(
        "{KEYS_FILE}: {} texture keys, {pictures} pictures for the HD pack, {} left as the game \
         has them",
        sorted.len(),
        sorted.len() - pictures
    );
    Ok(failures == 0)
}

/// The hd_keys.tsv line of one texture level (see `hd_textures`).
fn key_line(name: &str, key: u64, fetch: &xenos::FetchConstant) -> KeyLine {
    let name = sanitize(name);
    let (kind, file) = if keep_original(&name, fetch.width, fetch.height) {
        (Kind::Unknown, NO_FILE.to_string())
    } else if fetch.format == xenos::Format::Dxn {
        (Kind::Normal, format!("{name}__{key:016x}"))
    } else {
        (Kind::Diffuse, format!("{name}__{key:016x}"))
    };
    KeyLine {
        name,
        key,
        width: fetch.width,
        height: fetch.height,
        format: fetch.format.to_xenos(),
        file,
        kind,
        name_hash: 0,
    }
}

/// The fetch constant of the `.hi` level (twice the zone's top level) and
/// the level untiled, if the file is exactly that level's tiled size.
fn highmip_level(
    fetch: &xenos::FetchConstant,
    data: &[u8],
) -> Option<(xenos::FetchConstant, Vec<u8>)> {
    let (block, bytes_per_block) = fetch.format.block()?;
    let (width, height) = (fetch.width * 2, fetch.height * 2);
    let padded = |side: u32| side.div_ceil(block).next_multiple_of(TILE) as usize;
    if data.len() != padded(width) * padded(height) * bytes_per_block as usize {
        return None;
    }
    let hi = xenos::FetchConstant {
        width,
        height,
        pitch: width.next_multiple_of(TILE * block),
        packed_mips: false,
        mip_offset: 0,
        ..fetch.clone()
    };
    let linear = xenos::base_level_linear(&hi, data).ok()?;
    Some((hi, linear))
}

/// `<base>.dds` and `<base>.png`; false if the format can't be written.
fn write_texture(
    dir: &Path,
    base: &str,
    fetch: &xenos::FetchConstant,
    linear: &[u8],
) -> std::io::Result<bool> {
    let (Ok(dds), Ok(rgba)) = (xenos::dds_file(fetch, linear), xenos::rgba8(fetch, linear)) else {
        return Ok(false);
    };
    write_if_changed(&dir.join(format!("{base}.dds")), &dds)?;
    let png = png_write::encode_rgba(fetch.width, fetch.height, &rgba);
    write_if_changed(&dir.join(format!("{base}.png")), &png)?;
    Ok(true)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn dxt1(width: u32, height: u32) -> xenos::FetchConstant {
        xenos::FetchConstant::parse([
            0x8000_0002 | ((width / 32) << 22),
            0x52,
            (width - 1) | ((height - 1) << 13),
            0,
            0,
            1 << 9,
        ])
    }

    #[test]
    fn highmip_is_twice_the_size_and_exactly_its_tiled_bytes() {
        let fetch = dxt1(512, 512);
        // 1024 x 1024 DXT1: 256 x 256 blocks of 8 bytes.
        let (hi, linear) = highmip_level(&fetch, &vec![0u8; 524_288]).unwrap();
        assert_eq!((hi.width, hi.height, hi.pitch), (1024, 1024, 1024));
        assert_eq!(linear.len(), 524_288);
        assert!(highmip_level(&fetch, &vec![0u8; 524_287]).is_none());
        // 1024 x 512 in the zone: a 2048 x 1024 level.
        let wide = dxt1(1024, 512);
        assert!(highmip_level(&wide, &vec![0u8; 1_048_576]).is_some());
    }

    #[test]
    fn small_sides_are_padded_to_a_tile() {
        // 64 x 32 DXT1 in the zone: the .hi is 128 x 64 (32 x 16 blocks),
        // stored as 32 x 32 blocks.
        let fetch = dxt1(64, 32);
        assert!(highmip_level(&fetch, &vec![0u8; 32 * 32 * 8]).is_some());
        assert!(highmip_level(&fetch, &vec![0u8; 32 * 16 * 8]).is_none());
    }

    #[test]
    fn key_lines_name_each_picture_by_its_key_and_keep_lookup_tables() {
        let line = key_line("~ber1_wall_c", 0xabc, &dxt1(256, 128));
        assert_eq!(line.file, format!("{}__0000000000000abc", line.name));
        assert_eq!(
            (line.width, line.height, line.kind),
            (256, 128, Kind::Diffuse)
        );
        // A side of 8 texels or less is a lookup table: left as it is.
        let small = key_line("ramp", 1, &dxt1(64, 8));
        assert_eq!((small.file.as_str(), small.kind), (NO_FILE, Kind::Unknown));
        let lightmap = key_line("lm0001", 2, &dxt1(512, 512));
        assert_eq!(lightmap.file, NO_FILE);
    }
}
