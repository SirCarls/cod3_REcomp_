//! The user texture pack: the player's own pictures for chosen textures (the
//! insignia listed in the setup tool's `insignia_textures.txt`, or any other
//! texture), packed for the game in the HD pack's layout:
//!
//! ```text
//! rip cod3-user-pack <textures dir> <png dir> <out dir> [--list insignia_textures.txt]
//! ```
//!
//! Input: `<png dir>/<file>.png`, named like the ripped picture it replaces
//! (`<textures dir>/<file>.png`, the `file` column of hd_keys.tsv): `<name>`
//! for most textures, `<name>__<key>` for another picture saved under the
//! same name. A PNG replaces exactly the texture whose ripped picture has its
//! name, never another texture's (the key decides, see `hd`). The rules
//! (docs/TEXTURE_REPLACEMENT.md tells players):
//!
//! - PNG with 8 bits per channel: RGBA, RGB, gray or gray + alpha. Not a
//!   palette (indexed) or 16-bit PNG, not interlaced, not animated (APNG).
//!   At most `MAX_USER_PNG_BYTES`.
//! - The original's aspect ratio, at least the original's size (any scale,
//!   not necessarily whole), at most 8192 a side, and for the DXT and ATI2
//!   formats both sides multiples of 4 (the GPU's block size).
//! - Alpha: a replacement is never more see-through than the original. For
//!   an opaque original the PNG's alpha is ignored (opaque). For an original
//!   with transparent parts, each texel keeps at least the original's alpha
//!   (resampled to the PNG's size); a PNG without alpha takes the original's.
//!   The game checks this again when it loads the pack.
//!
//! Output: `<out>/<file>.dds` (full mip chain, the original's format) and
//! `<out>/index.tsv`; DDS files the index no longer lists are removed, and
//! with no usable PNG the index is removed too, so there is no pack. The
//! files are checked with the game's own rules (`pack_check`) before the
//! command reports success.

use crate::hd::{self, KeyLine};
use crate::jobs;
use crate::output::write_if_changed;
use crate::pack_check;
use std::collections::{BTreeMap, BTreeSet};
use std::path::{Path, PathBuf};
use xenos::Format;

type Result<T> = std::result::Result<T, Box<dyn std::error::Error>>;

/// Largest PNG file read (an 8192 x 8192 RGBA picture is 256 MB unpacked;
/// PNG files are smaller).
pub const MAX_USER_PNG_BYTES: u64 = 256 << 20;
/// Problems printed by name before only counting the rest.
const PROBLEMS_SHOWN: usize = 20;

const COLOR_GRAY: u8 = 0;
const COLOR_RGB: u8 = 2;
const COLOR_PALETTE: u8 = 3;
const COLOR_GRAY_ALPHA: u8 = 4;
const COLOR_RGBA: u8 = 6;

/// Checks a PNG file's structure before decoding it: 8 bits per channel,
/// a color type with direct colors, not interlaced, not animated (an `acTL`
/// chunk before the image data). Returns the width and height. The messages
/// say what to change, for players.
pub fn check_png(bytes: &[u8]) -> std::result::Result<(u32, u32), String> {
    const SIGNATURE: &[u8] = b"\x89PNG\r\n\x1a\n";
    if bytes.len() < 8 || &bytes[..8] != SIGNATURE {
        return Err("not a PNG file".into());
    }
    if bytes.len() < 33 || &bytes[12..16] != b"IHDR" {
        return Err("a damaged PNG (no image header)".into());
    }
    let be32 =
        |at: usize| u32::from_be_bytes([bytes[at], bytes[at + 1], bytes[at + 2], bytes[at + 3]]);
    let (width, height) = (be32(16), be32(20));
    let (bit_depth, color_type, interlace) = (bytes[24], bytes[25], bytes[28]);
    if color_type == COLOR_PALETTE {
        return Err("a palette (indexed-color) PNG: save it as RGBA, 8 bits per channel".into());
    }
    if bit_depth != 8 {
        return Err(format!(
            "a {bit_depth}-bit PNG: save it with 8 bits per channel (RGBA 8-bit)"
        ));
    }
    if ![COLOR_GRAY, COLOR_RGB, COLOR_GRAY_ALPHA, COLOR_RGBA].contains(&color_type) {
        return Err(format!("an unknown PNG color type ({color_type})"));
    }
    if interlace != 0 {
        return Err("an interlaced PNG: save it without interlacing".into());
    }
    // The chunks up to the image data: an animated PNG has acTL there.
    let mut at = 8usize;
    while let Some(head) = bytes.get(at..at + 8) {
        let length = u32::from_be_bytes([head[0], head[1], head[2], head[3]]) as usize;
        match &head[4..8] {
            b"acTL" => return Err("an animated PNG: save a single still picture".into()),
            b"IDAT" | b"IEND" => break,
            _ => {}
        }
        at = at.saturating_add(12).saturating_add(length);
    }
    // The workspace's reader agrees (and checks the size limits).
    png_read::dimensions(bytes).map_err(|e| e.to_string())?;
    Ok((width, height))
}

/// Sizes a replacement for an `original` texture in `format` may have, as
/// "WxH (scale)" for whole scales up to 4x, for messages.
fn allowed_sizes(format: Format, original: (u32, u32)) -> String {
    let block = format.block().map_or(1, |(b, _)| b);
    let sizes: Vec<String> = (1..=4u32)
        .filter_map(|s| {
            let (w, h) = (original.0 * s, original.1 * s);
            (w % block == 0 && h % block == 0 && w.max(h) <= hd::MAX_HD_SIDE)
                .then(|| format!("{w}x{h} ({s}x)"))
        })
        .collect();
    if sizes.is_empty() {
        "none at a whole scale; see docs/TEXTURE_REPLACEMENT.md".into()
    } else {
        sizes.join(", ")
    }
}

/// Checks a PNG's size for an `original` texture in `format`: at least the
/// original's, the same aspect ratio (to within a texel of rounding), at most
/// 8192 a side, and multiples of 4 for the block formats.
pub fn check_size(
    format: Format,
    original: (u32, u32),
    png: (u32, u32),
) -> std::result::Result<(), String> {
    let use_instead = || format!("use {}", allowed_sizes(format, original));
    if png.0.max(png.1) > hd::MAX_HD_SIDE {
        return Err(format!(
            "{}x{} is larger than {} a side: {}",
            png.0,
            png.1,
            hd::MAX_HD_SIDE,
            use_instead()
        ));
    }
    if !hd::is_scaled_copy(original, png) {
        return Err(format!(
            "{}x{} is not the original {}x{} scaled up (another aspect ratio, or smaller): {}",
            png.0,
            png.1,
            original.0,
            original.1,
            use_instead()
        ));
    }
    if format.block().is_some_and(|(block, _)| block == 4) && (png.0 % 4 != 0 || png.1 % 4 != 0) {
        return Err(format!(
            "{}x{}: this texture is compressed in 4x4 blocks, so both sides must be multiples of \
             4: {}",
            png.0,
            png.1,
            use_instead()
        ));
    }
    Ok(())
}

/// Applies the alpha rules to a `size` RGBA picture (see the module
/// documentation). `original` is the ripped original (`original_size`).
/// Returns how many texels' alpha changed.
pub fn clamp_alpha(
    rgba: &mut [u8],
    size: (u32, u32),
    png_has_alpha: bool,
    original: &[u8],
    original_size: (u32, u32),
) -> usize {
    let opaque = original.chunks_exact(4).all(|t| t[3] == 255);
    let mut changed = 0;
    if opaque {
        for texel in rgba.chunks_exact_mut(4) {
            if texel[3] != 255 {
                texel[3] = 255;
                changed += 1;
            }
        }
        return changed;
    }
    // The original's alpha at the PNG's size.
    let scaled = hd::resize(original.to_vec(), original_size, size);
    for (texel, reference) in rgba.chunks_exact_mut(4).zip(scaled.chunks_exact(4)) {
        let alpha = if png_has_alpha {
            texel[3].max(reference[3])
        } else {
            reference[3]
        };
        if alpha != texel[3] {
            texel[3] = alpha;
            changed += 1;
        }
    }
    changed
}

/// The names (the first column) of an insignia list: tab-separated lines,
/// `#` comments.
pub fn parse_list(text: &str) -> Vec<String> {
    text.lines()
        .map(|l| l.trim_end_matches('\r'))
        .filter(|l| !l.trim().is_empty() && !l.starts_with('#'))
        .filter_map(|l| l.split('\t').next())
        .map(|name| name.trim().to_string())
        .filter(|name| !name.is_empty())
        .collect()
}

/// One packed texture.
struct Packed {
    keys: Vec<KeyLine>,
    scale: u32,
    /// Texels whose alpha the rules changed.
    alpha_changed: usize,
    opaque_original: bool,
}

/// Builds `<out>/<file>.dds` from a user PNG.
fn pack_one(
    file: &str,
    keys: &[KeyLine],
    png_path: &Path,
    textures: &Path,
    out: &Path,
    budget: &hd::TexelBudget,
) -> std::result::Result<Packed, String> {
    let first = &keys[0];
    if keys.iter().any(|k| {
        (k.key, k.format, k.width, k.height) != (first.key, first.format, first.width, first.height)
    }) {
        return Err(
            "hd_keys.tsv gives this picture more than one texture: rip the game again".into(),
        );
    }
    let format = Format::from_xenos(first.format);
    let original_size = (first.width, first.height);
    let meta = std::fs::metadata(png_path).map_err(|e| e.to_string())?;
    if meta.len() > MAX_USER_PNG_BYTES {
        return Err(format!(
            "the file is {} MB, more than {} MB",
            meta.len() >> 20,
            MAX_USER_PNG_BYTES >> 20
        ));
    }
    // The size first, from the header alone.
    let head = hd::read_prefix(png_path, 64 * 1024).map_err(|e| e.to_string())?;
    let png_size = check_png(&head)?;
    check_size(format, original_size, png_size)?;
    let _lease = budget.take(png_size.0 as u64 * png_size.1 as u64);
    let bytes = std::fs::read(png_path).map_err(|e| e.to_string())?;
    check_png(&bytes)?;
    let image = png_read::decode(&bytes).map_err(|e| e.to_string())?;
    // The original, for its alpha.
    let original_path = textures.join(format!("{file}.png"));
    let original = std::fs::read(&original_path)
        .ok()
        .and_then(|b| png_read::decode(&b).ok())
        .filter(|o| (o.width, o.height) == original_size)
        .ok_or_else(|| {
            format!(
                "the ripped original {} is missing or not {}x{}: rip the game again",
                original_path.display(),
                original_size.0,
                original_size.1
            )
        })?;
    let mut rgba = image.rgba;
    let opaque_original =
        format == Format::Dxn || original.rgba.chunks_exact(4).all(|t| t[3] == 255);
    let alpha_changed = if format == Format::Dxn {
        0 // red and green only
    } else {
        clamp_alpha(
            &mut rgba,
            png_size,
            image.has_alpha,
            &original.rgba,
            original_size,
        )
    };
    let dds = hd::build_dds(format, png_size.0, png_size.1, rgba).map_err(|e| e.to_string())?;
    write_if_changed(&out.join(format!("{file}.dds")), &dds).map_err(|e| e.to_string())?;
    Ok(Packed {
        keys: vec![first.clone()],
        scale: hd::scale_hundredths(original_size, png_size),
        alpha_changed,
        opaque_original,
    })
}

/// What `pack_user` did.
#[derive(Debug, Default, Clone, PartialEq, Eq)]
pub struct UserSummary {
    pub packed: usize,
    pub refused: usize,
    pub index_keys: usize,
}

/// `rip cod3-user-pack`: see the module documentation.
pub fn pack_user(
    textures: &Path,
    png_dir: &Path,
    out: &Path,
    list: Option<&Path>,
    workers: usize,
) -> Result<UserSummary> {
    let keys_path = textures.join(hd::KEYS_FILE);
    let text = std::fs::read_to_string(&keys_path).map_err(|e| {
        format!(
            "{}: {e} (the textures must come from a rip with output layout 4 or later)",
            keys_path.display()
        )
    })?;
    let lines = hd::parse_keys(&text)?;
    // The pictures that can be replaced, and the names left as they are.
    let mut by_file: BTreeMap<String, Vec<KeyLine>> = BTreeMap::new();
    let mut kept_names = BTreeSet::new();
    for line in lines {
        if line.file == hd::NO_FILE || hd::keep_original(&line.name, line.width, line.height) {
            kept_names.insert(line.name.clone());
        } else {
            by_file.entry(line.file.clone()).or_default().push(line);
        }
    }

    // The PNGs: regular files directly in the folder.
    let mut refused: Vec<String> = Vec::new();
    let mut work: Vec<(String, PathBuf)> = Vec::new();
    let entries = std::fs::read_dir(png_dir).map_err(|e| format!("{}: {e}", png_dir.display()))?;
    for entry in entries.flatten() {
        let path = entry.path();
        let is_png = path
            .extension()
            .is_some_and(|e| e.eq_ignore_ascii_case("png"));
        if !is_png {
            continue;
        }
        let shown = entry.file_name().to_string_lossy().into_owned();
        if !std::fs::symlink_metadata(&path).is_ok_and(|m| m.is_file()) {
            refused.push(format!("{shown}: not a plain file (a folder or a link)"));
            continue;
        }
        let Some(stem) = path.file_stem().and_then(|s| s.to_str()) else {
            refused.push(format!("{shown}: the name isn't valid text"));
            continue;
        };
        if by_file.contains_key(stem) {
            work.push((stem.to_string(), path.clone()));
        } else if kept_names.contains(stem) {
            refused.push(format!(
                "{shown}: this texture holds lighting or lookup data and is always left as the \
                 game has it"
            ));
        } else {
            refused.push(format!(
                "{shown}: no ripped picture has this name (it must be a name from \
                 data/cod3/textures, case and all, as in insignia_textures.txt)"
            ));
        }
    }
    work.sort();
    std::fs::create_dir_all(out)?;
    println!("{} user textures, {workers} worker threads", work.len());

    let budget = hd::TexelBudget::new();
    let mut packed: BTreeMap<String, Packed> = BTreeMap::new();
    let total = work.len();
    let mut finished = 0usize;
    jobs::for_each_parallel(
        &work,
        workers,
        |(file, path)| pack_one(file, &by_file[file], path, textures, out, &budget),
        |(file, _), result| {
            finished += 1;
            println!("{finished}/{total} textures");
            match result {
                Ok(p) => {
                    packed.insert(file.clone(), p);
                }
                Err(why) => refused.push(format!("{file}.png: {why}")),
            }
        },
    );

    // index.tsv, in the HD pack's format.
    let mut index_lines: Vec<&KeyLine> = packed.values().flat_map(|p| p.keys.iter()).collect();
    index_lines.sort();
    let mut index = String::from(hd::INDEX_HEADER);
    let mut listed = BTreeSet::new();
    let mut seen = BTreeSet::new();
    for line in &index_lines {
        let dds = format!("{}.dds", line.file);
        if !pack_check::is_plain_pack_file(&dds) {
            return Err(format!("{dds} can't be named in an index").into());
        }
        if seen.insert(line.key) {
            index.push_str(&line.tsv(&dds));
            listed.insert(dds);
        }
    }
    let index_path = out.join(hd::INDEX_FILE);
    if packed.is_empty() {
        // No pack: the game then has none to read.
        if index_path.exists() {
            std::fs::remove_file(&index_path)?;
        }
    } else {
        write_if_changed(&index_path, index.as_bytes())?;
    }
    let removed = hd::remove_unlisted(out, &listed);

    // The output must pass the game's checks.
    let (checked, malformed) = pack_check::parse_index(&index);
    if !malformed.is_empty() {
        return Err(format!("the index written has malformed lines {malformed:?}").into());
    }
    for line in &checked {
        pack_check::check_dds(out, line)
            .map_err(|why| format!("{} doesn't pass the game's checks: {why}", line.file))?;
    }

    refused.sort();
    for line in refused.iter().take(PROBLEMS_SHOWN) {
        println!("skipped: {line}");
    }
    if refused.len() > PROBLEMS_SHOWN {
        println!("skipped: ... and {} more", refused.len() - PROBLEMS_SHOWN);
    }
    for (file, p) in &packed {
        let alpha = match (p.alpha_changed, p.opaque_original) {
            (0, _) => String::new(),
            (n, true) => format!(", alpha ignored (the original is opaque; {n} texels)"),
            (n, false) => format!(
                ", {n} texels kept as opaque as the original (a replacement can't be more \
                 see-through)"
            ),
        };
        println!("packed {file}.dds ({}{alpha})", hd::scale_label(p.scale));
    }
    if let Some(list) = list {
        let names = parse_list(&std::fs::read_to_string(list)?);
        let missing: Vec<&String> = names.iter().filter(|n| !packed.contains_key(*n)).collect();
        println!(
            "insignia list: {} of {} replaced{}",
            names.len() - missing.len(),
            names.len(),
            if missing.is_empty() {
                String::new()
            } else {
                format!(
                    "; the game's own picture stays for {}",
                    missing
                        .iter()
                        .map(|s| s.as_str())
                        .collect::<Vec<_>>()
                        .join(", ")
                )
            }
        );
    }
    println!(
        "done: {} packed, {} skipped; {} keys in {}{}",
        packed.len(),
        refused.len(),
        seen.len(),
        if packed.is_empty() {
            "no index (no user pack)".to_string()
        } else {
            index_path.display().to_string()
        },
        if removed > 0 {
            format!("; {removed} old DDS files removed")
        } else {
            String::new()
        }
    );
    Ok(UserSummary {
        packed: packed.len(),
        refused: refused.len(),
        index_keys: seen.len(),
    })
}

#[cfg(test)]
mod tests {
    use super::*;
    use xenos::bc::{self, BlockFormat};

    fn png(w: u32, h: u32, rgba: &[u8]) -> Vec<u8> {
        png_write::encode_rgba(w, h, rgba)
    }

    /// A PNG with IHDR's bit depth, color type or interlace byte changed.
    fn with_ihdr(mut bytes: Vec<u8>, at: usize, value: u8) -> Vec<u8> {
        bytes[24 + at] = value;
        bytes
    }

    #[test]
    fn png_files_must_be_plain_8_bit_stills() {
        let good = png(4, 4, &[9; 64]);
        assert_eq!(check_png(&good), Ok((4, 4)));
        assert!(check_png(b"GIF89a").unwrap_err().contains("not a PNG"));
        assert!(check_png(&good[..20]).unwrap_err().contains("damaged"));
        let palette = check_png(&with_ihdr(good.clone(), 1, 3)).unwrap_err();
        assert!(palette.contains("palette"), "{palette}");
        let deep = check_png(&with_ihdr(good.clone(), 0, 16)).unwrap_err();
        assert!(deep.contains("16-bit"), "{deep}");
        let interlaced = check_png(&with_ihdr(good.clone(), 4, 1)).unwrap_err();
        assert!(interlaced.contains("interlaced"), "{interlaced}");
        // An acTL chunk right after IHDR makes it an animated PNG.
        let mut animated = good[..33].to_vec();
        animated.extend_from_slice(&[0, 0, 0, 8]);
        animated.extend_from_slice(b"acTL");
        animated.extend_from_slice(&[0, 0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0]);
        animated.extend_from_slice(&good[33..]);
        assert!(check_png(&animated).unwrap_err().contains("animated"));
        // A damaged chunk length can't make the scan read out of bounds.
        let mut odd = good.clone();
        odd[33..37].copy_from_slice(&u32::MAX.to_be_bytes());
        let _ = check_png(&odd);
    }

    #[test]
    fn sizes_keep_the_aspect_ratio_and_the_blocks() {
        let flag = (512, 256);
        assert!(check_size(Format::Dxt1, flag, (512, 256)).is_ok());
        assert!(check_size(Format::Dxt1, flag, (1024, 512)).is_ok());
        assert!(check_size(Format::Dxt1, flag, (1536, 768)).is_ok()); // 3x
        assert!(check_size(Format::Dxt1, flag, (8192, 4096)).is_ok());
        let wrong = check_size(Format::Dxt1, flag, (1024, 1024)).unwrap_err();
        assert!(
            wrong.contains("aspect ratio") && wrong.contains("1024x512 (2x)"),
            "{wrong}"
        );
        assert!(check_size(Format::Dxt1, flag, (256, 128)).is_err()); // smaller
        assert!(check_size(Format::Dxt1, flag, (16384, 8192))
            .unwrap_err()
            .contains("8192"));
        // 1.5x of 30x10 is 45x15: fine for 32-bit, not for blocks of 4.
        assert!(check_size(Format::A8R8G8B8, (30, 10), (45, 15)).is_ok());
        let blocks = check_size(Format::Dxt4_5, (30, 10), (45, 15)).unwrap_err();
        assert!(blocks.contains("multiples of 4"), "{blocks}");
        assert_eq!(
            allowed_sizes(Format::Dxt1, (256, 512)),
            "256x512 (1x), 512x1024 (2x), 768x1536 (3x), 1024x2048 (4x)"
        );
        assert_eq!(
            allowed_sizes(Format::Dxt1, (30, 10)),
            "60x20 (2x), 120x40 (4x)"
        );
    }

    #[test]
    fn replacements_are_never_more_see_through() {
        // An opaque original: the PNG's alpha is ignored.
        let opaque = [10u8, 20, 30, 255].repeat(4);
        let mut picture = [1u8, 2, 3, 0].repeat(16);
        assert_eq!(clamp_alpha(&mut picture, (4, 4), true, &opaque, (2, 2)), 16);
        assert!(picture.chunks_exact(4).all(|t| t == [1, 2, 3, 255]));
        // An original with a transparent half (left) and an opaque half.
        let original: Vec<u8> = (0..4)
            .flat_map(|i| [0, 0, 0, if i % 2 == 0 { 0 } else { 255 }])
            .collect();
        // The PNG tries to clear everything: the opaque half stays opaque.
        let mut picture = [5u8, 5, 5, 0].repeat(4);
        clamp_alpha(&mut picture, (2, 2), true, &original, (2, 2));
        let alphas: Vec<u8> = picture.chunks_exact(4).map(|t| t[3]).collect();
        assert_eq!(alphas, vec![0, 255, 0, 255]);
        // It may fill the transparent half.
        let mut picture = [5u8, 5, 5, 255].repeat(4);
        assert_eq!(
            clamp_alpha(&mut picture, (2, 2), true, &original, (2, 2)),
            0
        );
        // A PNG without alpha takes the original's.
        let mut picture = [5u8, 5, 5, 255].repeat(4);
        clamp_alpha(&mut picture, (2, 2), false, &original, (2, 2));
        let alphas: Vec<u8> = picture.chunks_exact(4).map(|t| t[3]).collect();
        assert_eq!(alphas, vec![0, 255, 0, 255]);
    }

    #[test]
    fn lists_are_names_in_the_first_column() {
        let list = "# comment\n\ncloth_masked$of_flag_german\te26a0a1331259fd8\t512\t256\r\nx\n";
        assert_eq!(parse_list(list), vec!["cloth_masked$of_flag_german", "x"]);
    }

    #[test]
    fn packs_user_pictures_for_their_own_keys_only() {
        let root = std::env::temp_dir().join(format!("rip-user-pack-test-{}", std::process::id()));
        let (textures, pngs, out) = (root.join("t"), root.join("user"), root.join("pack"));
        for dir in [&textures, &pngs] {
            std::fs::create_dir_all(dir).unwrap();
        }
        // (Textures of 8 texels or fewer a side are lookup tables, left as
        // they are, so these are at least 16 a side.)
        let keys = "# key\twidth\theight\txenos_format\tname\tfile\tkind\n\
                    00000000000000a1\t32\t16\t18\tflag\tflag\tdiffuse\n\
                    00000000000000a2\t32\t16\t18\tflag\tflag__00000000000000a2\tdiffuse\n\
                    00000000000000b1\t16\t16\t18\tmasked\tmasked\tdiffuse\n\
                    00000000000000c1\t16\t16\t18\tlm0001\t-\tother\n\
                    00000000000000d1\t16\t16\t20\tposter\tposter\tunknown\n";
        std::fs::write(textures.join(hd::KEYS_FILE), keys).unwrap();
        let write = |dir: &Path, name: &str, w: u32, h: u32, rgba: &[u8]| {
            std::fs::write(dir.join(format!("{name}.png")), png(w, h, rgba)).unwrap();
        };
        // Originals: an opaque flag, a masked texture with a transparent left
        // half, a poster.
        write(&textures, "flag", 32, 16, &[200, 0, 0, 255].repeat(512));
        write(
            &textures,
            "flag__00000000000000a2",
            32,
            16,
            &[0, 200, 0, 255].repeat(512),
        );
        let masked: Vec<u8> = (0..256)
            .flat_map(|i| [9, 9, 9, if i % 16 < 8 { 0 } else { 255 }])
            .collect();
        write(&textures, "masked", 16, 16, &masked);
        write(&textures, "poster", 16, 16, &[1, 2, 3, 255].repeat(256));
        // User pictures: the flag at 2x with a hole it may not have, the
        // masked texture fully transparent, a wrong aspect ratio, sides not a
        // multiple of 4, a lightmap, an unknown name.
        write(&pngs, "flag", 64, 32, &[0, 0, 200, 0].repeat(2048));
        write(&pngs, "masked", 32, 32, &[50, 50, 50, 0].repeat(1024));
        write(&pngs, "poster", 32, 16, &[7; 2048]);
        write(&pngs, "flag__00000000000000a2", 36, 18, &[0; 2592]);
        write(&pngs, "lm0001", 16, 16, &[0; 1024]);
        write(&pngs, "nothing", 16, 16, &[0; 1024]);
        std::fs::write(pngs.join("notes.txt"), "not a picture").unwrap();

        let summary = pack_user(&textures, &pngs, &out, None, 2).unwrap();
        assert_eq!(summary.packed, 2);
        assert_eq!(summary.refused, 4);
        let index = std::fs::read_to_string(out.join(hd::INDEX_FILE)).unwrap();
        // Each picture serves its own key only: not the second flag picture.
        assert_eq!(
            index,
            "# key\twidth\theight\txenos_format\tfile\n\
             00000000000000a1\t32\t16\t18\tflag.dds\n\
             00000000000000b1\t16\t16\t18\tmasked.dds\n"
        );
        // The flag is opaque like its original.
        let dds = std::fs::read(out.join("flag.dds")).unwrap();
        assert_eq!(xenos::dds_info(&dds), Some((Format::Dxt1, 64, 32)));
        let top = bc::decode(BlockFormat::Bc1, 64, 32, &dds[128..]).unwrap();
        assert!(top.chunks_exact(4).all(|t| t[3] == 255));
        // The masked texture keeps the original's opaque half.
        let dds = std::fs::read(out.join("masked.dds")).unwrap();
        let top = bc::decode(BlockFormat::Bc1, 32, 32, &dds[128..]).unwrap();
        let right_half_opaque = top
            .chunks_exact(4)
            .enumerate()
            .filter(|(i, _)| i % 32 >= 17)
            .all(|(_, t)| t[3] == 255);
        let left_half_clear = top
            .chunks_exact(4)
            .enumerate()
            .filter(|(i, _)| i % 32 < 14)
            .all(|(_, t)| t[3] == 0);
        assert!(right_half_opaque && left_half_clear);
        assert!(pack_check::check_pack(&out).unwrap().bad_files == 0);

        // Without usable pictures, the pack goes away (no index, no DDS).
        for name in ["flag", "masked"] {
            std::fs::remove_file(pngs.join(format!("{name}.png"))).unwrap();
        }
        let summary = pack_user(&textures, &pngs, &out, None, 1).unwrap();
        assert_eq!(summary.packed, 0);
        assert!(!out.join(hd::INDEX_FILE).exists());
        assert!(!out.join("flag.dds").exists());
        std::fs::remove_dir_all(&root).unwrap();
    }
}
