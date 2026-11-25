//! `rip captured-textures`: the textures a game saved while it was played
//! (the SDK's texture capture, `texture_capture_path`; SDK patch 0321) turned
//! into the same `textures/` folder `cod3-all` makes: one PNG per texture and
//! `hd_keys.tsv`. Then the setup tool's upscaler and `cod3-hd-pack` make the
//! HD pack from it exactly as they do Call of Duty 3's.
//!
//! Why: no ripper reads World at War's, Call of Duty 2's or Call of Duty 4's
//! Xbox 360 textures yet (ff_t4 reads Xbox 360 zones only up to their asset
//! tables). Their games run on the same SDK, so the game itself can say which
//! textures it draws with and give their keys.
//!
//! Nothing from the capture folder is trusted: every line of `captured.tsv`
//! must name `<key>.dds` for its own key, the DDS header must be the plain
//! one-level header the capture writes for that format and size, the file
//! must be exactly as long as that level, and the level's XXH64 must be the
//! key. Anything else is skipped and counted.

use std::collections::BTreeMap;
use std::path::Path;

use xenos::{bc, Format};

use crate::hd::{KeyLine, Kind, KEYS_FILE, NO_FILE};
use crate::output::write_if_changed;
use crate::Result;

/// The list the capture writes next to its DDS files.
pub const LIST_FILE: &str = "captured.tsv";
/// Largest side the capture can hold (the Xbox 360's limit).
const MAX_SIDE: u32 = 8192;

/// One usable line of captured.tsv.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Captured {
    pub key: u64,
    pub width: u32,
    pub height: u32,
    pub format: Format,
}

/// Parses captured.tsv: `<key>\t<width>\t<height>\t<xenos_format>\t<file>`.
/// Malformed lines are skipped and counted, not fatal: the game appends to
/// the file, and a session killed mid-line leaves a short last line. A key
/// listed twice keeps its first line.
pub fn parse_list(text: &str) -> (Vec<Captured>, usize) {
    let mut seen = BTreeMap::new();
    let mut skipped = 0;
    for line in text.lines() {
        let line = line.trim_end_matches('\r');
        if line.is_empty() || line.starts_with('#') {
            continue;
        }
        match parse_line(line) {
            Some(c) => {
                seen.entry(c.key).or_insert(c);
            }
            None => skipped += 1,
        }
    }
    (seen.into_values().collect(), skipped)
}

fn parse_line(line: &str) -> Option<Captured> {
    let fields: Vec<&str> = line.split('\t').collect();
    let [key, width, height, format, file] = fields[..] else {
        return None;
    };
    if key.len() != 16 {
        return None;
    }
    let key = u64::from_str_radix(key, 16).ok()?;
    // The file is always named after its key: no other path is ever opened.
    if file != dds_name(key) {
        return None;
    }
    let side = |s: &str| s.parse::<u32>().ok().filter(|v| (1..=MAX_SIDE).contains(v));
    let (width, height) = (side(width)?, side(height)?);
    let format = Format::from_xenos(format.parse().ok()?);
    format.block()?;
    Some(Captured {
        key,
        width,
        height,
        format,
    })
}

/// `<key>.dds`, the name the capture gives a texture's file.
pub fn dds_name(key: u64) -> String {
    format!("{key:016x}.dds")
}

/// The bytes of a `width` x `height` level of `format` (rows of blocks).
fn level_bytes(format: Format, width: u32, height: u32) -> Option<usize> {
    let (block, bytes) = format.block()?;
    Some(width.div_ceil(block) as usize * height.div_ceil(block) as usize * bytes as usize)
}

/// What a captured texture holds, as far as can be told without the game's
/// materials: two-channel DXN textures are normal maps; a side of 8 texels or
/// less is a lookup table or a solid color, left as it is (hd::keep_original
/// does the same for Call of Duty 3); everything else is taken for a color
/// picture, so the HD pack upscales it.
pub fn kind_of(c: &Captured) -> (Kind, bool) {
    if c.width <= 8 || c.height <= 8 {
        return (Kind::Unknown, false);
    }
    if c.format == Format::Dxn {
        return (Kind::Normal, true);
    }
    (Kind::Diffuse, true)
}

/// The base level's RGBA8 pixels from one captured DDS file, after every
/// check in this module's description.
pub fn read_one(capture_dir: &Path, c: &Captured) -> std::result::Result<Vec<u8>, String> {
    let name = dds_name(c.key);
    let file = std::fs::read(capture_dir.join(&name)).map_err(|e| format!("{name}: {e}"))?;
    let need = level_bytes(c.format, c.width, c.height).ok_or(format!("{name}: bad format"))?;
    if file.len() != xenos::DDS_HEADER_BYTES + need {
        return Err(format!(
            "{name}: {} bytes, expected {}",
            file.len(),
            xenos::DDS_HEADER_BYTES + need
        ));
    }
    if xenos::dds_info(&file) != Some((c.format, c.width, c.height)) {
        return Err(format!("{name}: the header doesn't match captured.tsv"));
    }
    let level = &file[xenos::DDS_HEADER_BYTES..];
    if xenos::hd_key(level) != c.key {
        return Err(format!("{name}: the picture's hash isn't its key"));
    }
    let (w, h) = (c.width as usize, c.height as usize);
    match c.format.block_format() {
        Some(block_format) => bc::decode(block_format, w, h, level).ok_or(format!("{name}: short")),
        // After the GPU's swap the bytes are B, G, R, A (xenos::dds_header).
        None => Ok(xenos::bgra_to_rgba(level)),
    }
}

/// `rip captured-textures <capture dir> <out_dir>`: writes
/// `<out_dir>/textures/<key>.png` for each usable captured texture and
/// `<out_dir>/textures/hd_keys.tsv` for all of them. Files that didn't change
/// are left alone, so the upscaler doesn't redo them. Returns false if any
/// texture was skipped.
pub fn convert(capture_dir: &Path, out_dir: &Path) -> Result<bool> {
    let list_path = capture_dir.join(LIST_FILE);
    let text = std::fs::read_to_string(&list_path).map_err(|e| {
        format!(
            "{}: {e} (play the game with texture capture on first)",
            list_path.display()
        )
    })?;
    let (captured, skipped_lines) = parse_list(&text);
    let textures = out_dir.join("textures");
    std::fs::create_dir_all(&textures)?;
    let mut lines = Vec::new();
    let mut problems = Vec::new();
    let mut written = 0usize;
    for c in &captured {
        let (kind, wants_picture) = kind_of(c);
        let name = format!("{:016x}", c.key);
        let file = if wants_picture {
            match read_one(capture_dir, c) {
                Ok(rgba) => {
                    let png = png_write::encode_rgba(c.width, c.height, &rgba);
                    if write_if_changed(&textures.join(format!("{name}.png")), &png)? {
                        written += 1;
                    }
                    name.clone()
                }
                Err(problem) => {
                    problems.push(problem);
                    continue;
                }
            }
        } else {
            NO_FILE.to_string()
        };
        lines.push(KeyLine {
            name: name.clone(),
            key: c.key,
            width: c.width,
            height: c.height,
            format: c.format.to_xenos(),
            file,
            kind,
            name_hash: 0,
        });
    }
    lines.sort();
    let mut text = String::from("# key\twidth\theight\txenos_format\tname\tfile\tkind\n");
    for line in &lines {
        text.push_str(&line.tsv(&format!(
            "{}\t{}\t{}",
            line.name,
            line.file,
            line.kind.label()
        )));
    }
    write_if_changed(&textures.join(KEYS_FILE), text.as_bytes())?;
    for problem in problems.iter().take(10) {
        println!("skipped: {problem}");
    }
    println!(
        "{KEYS_FILE}: {} captured textures ({} pictures written, {} unchanged), {} skipped, {} \
         malformed list lines",
        lines.len(),
        written,
        lines.iter().filter(|l| l.file != NO_FILE).count() - written,
        problems.len(),
        skipped_lines
    );
    Ok(problems.is_empty() && skipped_lines == 0)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn temp_dir(tag: &str) -> std::path::PathBuf {
        let dir = std::env::temp_dir().join(format!("rip-capture-{tag}-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&dir);
        std::fs::create_dir_all(&dir).unwrap();
        dir
    }

    /// The header the SDK's capture writes (capture.cpp MakeDdsHeader), byte
    /// for byte, so a change on either side shows up here.
    fn sdk_header(format: Format, width: u32, height: u32, size: usize) -> Vec<u8> {
        let mut h = vec![0u8; 128];
        let mut put = |at: usize, v: u32| h[at..at + 4].copy_from_slice(&v.to_le_bytes());
        put(4, 124);
        let fourcc = match format {
            Format::Dxt1 => Some(*b"DXT1"),
            Format::Dxt2_3 => Some(*b"DXT3"),
            Format::Dxt4_5 => Some(*b"DXT5"),
            Format::Dxn => Some(*b"ATI2"),
            _ => None,
        };
        put(12, height);
        put(16, width);
        put(28, 1);
        put(76, 32);
        put(108, 0x1000);
        match fourcc {
            Some(code) => {
                put(8, 0x1 | 0x2 | 0x4 | 0x1000 | 0x80000);
                put(20, size as u32);
                put(80, 0x4);
                put(84, u32::from_le_bytes(code));
            }
            None => {
                put(8, 0x1 | 0x2 | 0x4 | 0x1000 | 0x8);
                put(20, width * 4);
                put(80, 0x41);
                put(88, 32);
                put(92, 0x00FF_0000);
                put(96, 0x0000_FF00);
                put(100, 0x0000_00FF);
                put(104, 0xFF00_0000);
            }
        }
        h[..4].copy_from_slice(b"DDS ");
        h
    }

    fn save(dir: &Path, format: Format, width: u32, height: u32, level: &[u8]) -> u64 {
        let key = xenos::hd_key(level);
        let mut file = sdk_header(format, width, height, level.len());
        file.extend_from_slice(level);
        std::fs::write(dir.join(dds_name(key)), file).unwrap();
        key
    }

    #[test]
    fn the_sdk_header_is_one_this_crate_reads() {
        for (format, w, h) in [
            (Format::Dxt1, 64, 32),
            (Format::Dxt2_3, 16, 16),
            (Format::Dxt4_5, 128, 64),
            (Format::Dxn, 32, 32),
            (Format::A8R8G8B8, 20, 12),
        ] {
            let header = sdk_header(format, w, h, level_bytes(format, w, h).unwrap());
            assert_eq!(xenos::dds_info(&header), Some((format, w, h)), "{format:?}");
        }
    }

    #[test]
    fn list_lines_are_checked_and_a_short_last_line_is_skipped() {
        let k = 0x0123_4567_89ab_cdefu64;
        let text = format!(
            "# comment\n{k:016x}\t64\t32\t18\t{}\n\
             {k:016x}\t64\t32\t18\t{}\n\
             0000000000000001\t64\t32\t18\t../x.dds\n\
             0000000000000002\t64\t32\t7\t0000000000000002.dds\n\
             0000000000000003\t0\t32\t18\t0000000000000003.dds\n\
             00000000000000",
            dds_name(k),
            dds_name(k)
        );
        let (list, skipped) = parse_list(&text);
        assert_eq!(
            list,
            vec![Captured {
                key: k,
                width: 64,
                height: 32,
                format: Format::Dxt1
            }]
        );
        assert_eq!(skipped, 4);
    }

    #[test]
    fn kinds_follow_the_format_and_size() {
        let c = |format, width, height| Captured {
            key: 1,
            width,
            height,
            format,
        };
        assert_eq!(kind_of(&c(Format::Dxn, 64, 64)), (Kind::Normal, true));
        assert_eq!(kind_of(&c(Format::Dxt1, 64, 64)), (Kind::Diffuse, true));
        assert_eq!(kind_of(&c(Format::Dxt4_5, 8, 256)), (Kind::Unknown, false));
    }

    #[test]
    fn captured_textures_become_pngs_and_key_lines() {
        let cap = temp_dir("in");
        let out = temp_dir("out");
        // A 4x4 A8R8G8B8 texture: B, G, R, A in memory, so the PNG gets R, G, B, A.
        let texels: Vec<u8> = (0..16).flat_map(|i| [i as u8, 2, 200, 255]).collect();
        let good = save(&cap, Format::A8R8G8B8, 4, 4, &texels);
        // Too small to upscale: listed, no picture.
        let tiny = save(&cap, Format::Dxt1, 4, 4, &[0u8; 8]);
        // A file whose content doesn't hash to its key.
        let bad_key = 0xdead_beef_u64;
        let mut bad = sdk_header(Format::A8R8G8B8, 16, 16, 1024);
        bad.extend_from_slice(&[7u8; 1024]);
        std::fs::write(cap.join(dds_name(bad_key)), bad).unwrap();
        let list = [(good, 4, 4, 6), (tiny, 4, 4, 18), (bad_key, 16, 16, 6)]
            .iter()
            .map(|(k, w, h, f)| format!("{k:016x}\t{w}\t{h}\t{f}\t{}\n", dds_name(*k)))
            .collect::<String>();
        std::fs::write(cap.join(LIST_FILE), list).unwrap();

        // The 4x4 picture: too small to be taken for a color picture, so a
        // larger one is needed to see a PNG written.
        let big: Vec<u8> = (0..16 * 16).flat_map(|i| [i as u8, 2, 200, 255]).collect();
        let big_key = save(&cap, Format::A8R8G8B8, 16, 16, &big);
        let mut list = std::fs::read_to_string(cap.join(LIST_FILE)).unwrap();
        list.push_str(&format!(
            "{big_key:016x}\t16\t16\t6\t{}\n",
            dds_name(big_key)
        ));
        std::fs::write(cap.join(LIST_FILE), list).unwrap();

        assert!(!convert(&cap, &out).unwrap(), "the bad file is reported");
        let keys = std::fs::read_to_string(out.join("textures").join(KEYS_FILE)).unwrap();
        let parsed = crate::hd::parse_keys(&keys).unwrap();
        assert_eq!(parsed.len(), 3, "{keys}");
        let line = |k: u64| parsed.iter().find(|l| l.key == k).unwrap();
        assert_eq!(line(tiny).file, NO_FILE);
        assert_eq!(line(good).file, NO_FILE);
        assert_eq!(line(big_key).file, format!("{big_key:016x}"));
        assert_eq!(line(big_key).kind, Kind::Diffuse);
        assert!(parsed.iter().all(|l| l.key != bad_key));
        let png = std::fs::read(out.join("textures").join(format!("{big_key:016x}.png"))).unwrap();
        let image = png_read::decode(&png).unwrap();
        assert_eq!((image.width, image.height), (16, 16));
        assert_eq!(&image.rgba[..4], &[200, 2, 0, 255]);

        // And the HD pack is made from it as from Call of Duty 3's rip: the
        // picture at 2x, packed under the captured key.
        let upscaled = temp_dir("up");
        let big2: Vec<u8> = image
            .rgba
            .chunks_exact(4 * 16)
            .flat_map(|row| {
                let wide: Vec<u8> = row.chunks_exact(4).flat_map(|p| [p, p].concat()).collect();
                [wide.clone(), wide].concat()
            })
            .collect();
        std::fs::write(
            upscaled.join(format!("{big_key:016x}.png")),
            png_write::encode_rgba(32, 32, &big2),
        )
        .unwrap();
        let pack = temp_dir("pack");
        let summary = crate::hd::pack(
            &out.join("textures"),
            &upscaled,
            &pack,
            &Kind::default_set(),
            1,
        )
        .unwrap();
        assert_eq!(summary.failed, 0);
        let index = std::fs::read_to_string(pack.join(crate::hd::INDEX_FILE)).unwrap();
        assert!(
            index.contains(&format!("{big_key:016x}\t16\t16\t6\t")),
            "{index}"
        );
        for dir in [&cap, &out, &upscaled, &pack] {
            let _ = std::fs::remove_dir_all(dir);
        }
    }
}
