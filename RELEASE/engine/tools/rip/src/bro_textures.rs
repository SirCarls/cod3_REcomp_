//! Big Red One (original Xbox) textures for the HD pack, keyed the way the
//! port's texture loader finds them (`bro/src/ngl/ngl_texture_load.c`).
//!
//! Big Red One's textures are ordinary DDS files: a 128-byte header, then
//! the levels row by row (not swizzled). The loader keys a texture on the
//! name the game gives it (a 32-byte NGL name: a 4-byte hash, then up to 28
//! bytes of lower-case name) and the pack is looked up by:
//!
//! - `namekey`, FNV-1a 64 of that lower-case name (up to its first NUL);
//! - `filekey`, FNV-1a 64 of the whole DDS file as read, header included.
//!
//! A picture is `<namekey>_<filekey>.png` (16 lowercase hex digits each).
//! The port tries that name first, then `<namekey>.png`; this writes only
//! the first, which can't give a texture another file's picture (every
//! mission's loading map is loaded under the one name `texload`).
//!
//! The DDS files come from a folder: the disc's loose `textures\` tree, or
//! the port's texture dump (`BRO_TEX_DUMP_DIR`), which saves each texture
//! as `<name>__<filekey>.dds`. A dumped file carries its real name; a loose
//! file's name is a best guess, its stem in lower case cut to 28 bytes
//! (true for most callers, not all), and its filekey is exact either way.

use crate::hd::KEYS_FILE;
use crate::output::write_if_changed;
use std::collections::BTreeMap;
use std::path::Path;

/// The longest name an NGL name holds (32 bytes less its 4-byte hash).
const NAME_BYTES: usize = 28;
/// Where the DDS header's fields are, counted from the file's start (after
/// the 4-byte "DDS " magic).
const HEADER_BYTES: usize = 128;
const HEIGHT_AT: usize = 12;
const WIDTH_AT: usize = 16;
const PIXEL_FLAGS_AT: usize = 80;
const FOURCC_AT: usize = 84;
const BIT_COUNT_AT: usize = 88;
const MASKS_AT: usize = 92; // red, green, blue, alpha
const CAPS2_AT: usize = 112;
/// DDS pixel format flags and caps.
const DDPF_ALPHAPIXELS: u32 = 0x1;
const DDPF_ALPHA: u32 = 0x2;
const DDPF_FOURCC: u32 = 0x4;
const DDPF_PALETTEINDEXED8: u32 = 0x20;
const DDPF_LUMINANCE: u32 = 0x2_0000;
const DDSCAPS2_CUBEMAP: u32 = 0x200;

type Result<T> = std::result::Result<T, Box<dyn std::error::Error>>;

/// FNV-1a 64, the hash the port's loader uses for names and file bytes.
pub fn fnv1a64(bytes: &[u8]) -> u64 {
    bytes.iter().fold(0xCBF2_9CE4_8422_2325, |hash, &b| {
        (hash ^ u64::from(b)).wrapping_mul(0x0000_0100_0000_01B3)
    })
}

fn u32_at(d: &[u8], at: usize) -> Option<u32> {
    d.get(at..at + 4)
        .map(|b| u32::from_le_bytes([b[0], b[1], b[2], b[3]]))
}

/// Why a file was not listed.
#[derive(Debug, PartialEq, Eq, PartialOrd, Ord, Clone, Copy)]
pub enum Skip {
    /// Not a DDS file, or shorter than its header says.
    NotDds,
    /// A cube map: the port doesn't decode those yet.
    Cube,
    /// A pixel format this doesn't decode (P8 DDS files).
    Format,
}

/// Level 0 of a DDS file as RGBA8, with its size and a short format label.
pub fn decode_dds(file: &[u8]) -> std::result::Result<(u32, u32, &'static str, Vec<u8>), Skip> {
    if !file.starts_with(b"DDS ") || file.len() < HEADER_BYTES {
        return Err(Skip::NotDds);
    }
    let field = |at| u32_at(file, at).ok_or(Skip::NotDds);
    let (height, width) = (field(HEIGHT_AT)?, field(WIDTH_AT)?);
    if width == 0 || height == 0 || width > 8192 || height > 8192 {
        return Err(Skip::NotDds);
    }
    if field(CAPS2_AT)? & DDSCAPS2_CUBEMAP != 0 {
        return Err(Skip::Cube);
    }
    let flags = field(PIXEL_FLAGS_AT)?;
    let data = &file[HEADER_BYTES..];
    let (w, h) = (width as usize, height as usize);
    if flags & DDPF_FOURCC != 0 {
        let (format, label) = match &file[FOURCC_AT..FOURCC_AT + 4] {
            b"DXT1" => (xenos::bc::BlockFormat::Bc1, "DXT1"),
            b"DXT2" | b"DXT3" => (xenos::bc::BlockFormat::Bc2, "DXT3"),
            b"DXT4" | b"DXT5" => (xenos::bc::BlockFormat::Bc3, "DXT5"),
            _ => return Err(Skip::Format),
        };
        let rgba = xenos::bc::decode(format, w, h, data).ok_or(Skip::NotDds)?;
        return Ok((width, height, label, rgba));
    }
    if flags & DDPF_PALETTEINDEXED8 != 0 {
        return Err(Skip::Format);
    }
    let bits = field(BIT_COUNT_AT)? as usize;
    if !matches!(bits, 8 | 16 | 24 | 32) {
        return Err(Skip::Format);
    }
    let masks = [
        field(MASKS_AT)?,
        field(MASKS_AT + 4)?,
        field(MASKS_AT + 8)?,
        field(MASKS_AT + 12)?,
    ];
    let has_alpha = flags & (DDPF_ALPHAPIXELS | DDPF_ALPHA) != 0 && masks[3] != 0;
    let bytes = bits / 8;
    if data.len() < w * h * bytes {
        return Err(Skip::NotDds);
    }
    let label = match (flags & DDPF_LUMINANCE != 0, flags & DDPF_ALPHA != 0, bits) {
        (true, _, 8) if has_alpha => "A4L4",
        (true, _, _) => "L8",
        (_, true, _) => "A8",
        (_, _, 32) if has_alpha => "A8R8G8B8",
        (_, _, 32) => "X8R8G8B8",
        (_, _, 16) if masks[3] == 0x8000 => "A1R5G5B5",
        (_, _, 16) if masks[3] == 0xF000 => "A4R4G4B4",
        (_, _, 16) => "R5G6B5",
        _ => "RGB",
    };
    let mut rgba = vec![0u8; w * h * 4];
    for (texel, out) in data.chunks_exact(bytes).zip(rgba.chunks_exact_mut(4)) {
        let mut word = [0u8; 4];
        word[..bytes].copy_from_slice(texel);
        let value = u32::from_le_bytes(word);
        let channel = |mask: u32| -> u8 {
            if mask == 0 {
                return 0;
            }
            let max = mask >> mask.trailing_zeros();
            (((value & mask) >> mask.trailing_zeros()) * 255 / max) as u8
        };
        let (r, g, b) = if flags & DDPF_LUMINANCE != 0 {
            let l = channel(masks[0]);
            (l, l, l)
        } else if flags & DDPF_ALPHA != 0 {
            (255, 255, 255) // an alpha-only texture: white, shaped by its alpha
        } else {
            (channel(masks[0]), channel(masks[1]), channel(masks[2]))
        };
        let a = if has_alpha { channel(masks[3]) } else { 255 };
        out.copy_from_slice(&[r, g, b, a]);
    }
    Ok((width, height, label, rgba))
}

/// The texture's NGL name and whether it is certain: a dumped
/// `<name>__<filekey>.dds` names it (when the filekey is this file's), a
/// loose file is guessed from its stem.
pub fn texture_name(stem: &str, filekey: u64) -> (String, bool) {
    if let Some((name, key)) = stem.rsplit_once("__") {
        if u64::from_str_radix(key, 16) == Ok(filekey) && key.len() == 16 {
            return (name.to_string(), true);
        }
    }
    let mut name = stem.to_lowercase();
    while name.len() > NAME_BYTES {
        name.pop();
    }
    (name, false)
}

/// The file name of a picture in the pack (without `.png`).
pub fn picture_name(namekey: u64, filekey: u64) -> String {
    format!("{namekey:016x}_{filekey:016x}")
}

/// `rip bro-hd-textures <folder> <out_dir>`: every DDS file under the folder
/// (the disc's `textures` tree, or the port's texture dump) as
/// `<out_dir>/textures/<namekey>_<filekey>.png`, with `hd_keys.tsv` in the
/// columns the setup tool's upscaler reads (namekey, width, height,
/// format, name, file, kind) and three more: filekey, whether the name is
/// certain (`dump`) or guessed from the file name (`stem`), and the file.
pub fn hd_textures(input: &Path, out_dir: &Path, workers: usize) -> Result<bool> {
    let files = crate::jobs::find_files(input, "dds")?;
    if files.is_empty() {
        return Err(format!("no .dds files under {}", input.display()).into());
    }
    let textures = out_dir.join("textures");
    std::fs::create_dir_all(&textures)?;
    let lines = std::sync::Mutex::new(BTreeMap::<String, String>::new());
    let mut skipped = BTreeMap::<Skip, usize>::new();
    let mut failures = 0;
    crate::jobs::for_each_parallel(
        &files,
        workers,
        |file| -> std::io::Result<std::result::Result<(), Skip>> {
            let bytes = std::fs::read(file)?;
            let filekey = fnv1a64(&bytes);
            let (width, height, label, rgba) = match decode_dds(&bytes) {
                Ok(level) => level,
                Err(skip) => return Ok(Err(skip)),
            };
            let stem = file.file_stem().unwrap_or_default().to_string_lossy();
            let (name, certain) = texture_name(&stem, filekey);
            let namekey = fnv1a64(name.as_bytes());
            let picture = picture_name(namekey, filekey);
            let png = png_write::encode_rgba(width, height, &rgba);
            write_if_changed(&textures.join(format!("{picture}.png")), &png)?;
            let shown = file
                .strip_prefix(input)
                .unwrap_or(file)
                .display()
                .to_string();
            let line = format!(
                "{namekey:016x}\t{width}\t{height}\t{label}\t{name}\t{picture}\tdiffuse\t\
                 {filekey:016x}\t{}\t{}\n",
                if certain { "dump" } else { "stem" },
                shown.replace('\\', "/")
            );
            lines
                .lock()
                .unwrap_or_else(|e| e.into_inner())
                .entry(picture)
                .or_insert(line);
            Ok(Ok(()))
        },
        |file, result| match result {
            Ok(Ok(())) => {}
            Ok(Err(skip)) => *skipped.entry(skip).or_default() += 1,
            Err(e) => {
                failures += 1;
                eprintln!("error: {}: {e}", file.display());
            }
        },
    );
    let lines = lines.into_inner().unwrap_or_else(|e| e.into_inner());
    let mut text = String::from(
        "# namekey\twidth\theight\tformat\tname\tfile\tkind\tfilekey\tname_from\tsource\n",
    );
    lines.values().for_each(|line| text.push_str(line));
    write_if_changed(&textures.join(KEYS_FILE), text.as_bytes())?;
    println!(
        "{KEYS_FILE}: {} pictures for the HD pack from {} files; not listed: {skipped:?}",
        lines.len(),
        files.len()
    );
    Ok(failures == 0)
}

#[cfg(test)]
mod tests {
    use super::*;

    /// A DDS file with the given pixel format fields and data.
    fn dds(
        width: u32,
        height: u32,
        flags: u32,
        fourcc: &[u8; 4],
        bits: u32,
        masks: [u32; 4],
        data: &[u8],
    ) -> Vec<u8> {
        let mut f = vec![0u8; HEADER_BYTES];
        f[..4].copy_from_slice(b"DDS ");
        f[HEIGHT_AT..HEIGHT_AT + 4].copy_from_slice(&height.to_le_bytes());
        f[WIDTH_AT..WIDTH_AT + 4].copy_from_slice(&width.to_le_bytes());
        f[PIXEL_FLAGS_AT..PIXEL_FLAGS_AT + 4].copy_from_slice(&flags.to_le_bytes());
        f[FOURCC_AT..FOURCC_AT + 4].copy_from_slice(fourcc);
        f[BIT_COUNT_AT..BIT_COUNT_AT + 4].copy_from_slice(&bits.to_le_bytes());
        for (i, m) in masks.iter().enumerate() {
            f[MASKS_AT + 4 * i..MASKS_AT + 4 * i + 4].copy_from_slice(&m.to_le_bytes());
        }
        f.extend(data);
        f
    }

    #[test]
    fn a8r8g8b8_and_r5g6b5_decode_to_rgba() {
        let argb = [0x00FF_0000, 0x0000_FF00, 0x0000_00FF, 0xFF00_0000];
        let f = dds(
            1,
            1,
            DDPF_ALPHAPIXELS,
            b"\0\0\0\0",
            32,
            argb,
            &[3, 2, 1, 0x80],
        );
        assert_eq!(
            decode_dds(&f).unwrap(),
            (1, 1, "A8R8G8B8", vec![1, 2, 3, 0x80])
        );
        let f = dds(
            1,
            1,
            0,
            b"\0\0\0\0",
            16,
            [0xF800, 0x07E0, 0x001F, 0],
            &0xF800u16.to_le_bytes(),
        );
        assert_eq!(decode_dds(&f).unwrap().3, [255, 0, 0, 255]);
    }

    #[test]
    fn luminance_and_alpha_only_textures() {
        let f = dds(
            1,
            1,
            DDPF_LUMINANCE | DDPF_ALPHAPIXELS,
            b"\0\0\0\0",
            8,
            [0x0F, 0, 0, 0xF0],
            &[0x3F],
        );
        assert_eq!(
            decode_dds(&f).unwrap(),
            (1, 1, "A4L4", vec![255, 255, 255, 0x33])
        );
        let f = dds(1, 1, DDPF_ALPHA, b"\0\0\0\0", 8, [0, 0, 0, 0xFF], &[7]);
        assert_eq!(decode_dds(&f).unwrap().3, [255, 255, 255, 7]);
    }

    #[test]
    fn cube_maps_and_palettes_are_skipped() {
        let mut f = dds(4, 4, DDPF_FOURCC, b"DXT1", 0, [0; 4], &[0; 8]);
        assert_eq!(decode_dds(&f).unwrap().2, "DXT1");
        f[CAPS2_AT..CAPS2_AT + 4].copy_from_slice(&DDSCAPS2_CUBEMAP.to_le_bytes());
        assert_eq!(decode_dds(&f).unwrap_err(), Skip::Cube);
        let f = dds(1, 1, DDPF_PALETTEINDEXED8, b"\0\0\0\0", 8, [0; 4], &[0]);
        assert_eq!(decode_dds(&f).unwrap_err(), Skip::Format);
    }

    #[test]
    fn a_dumped_file_names_itself_and_a_loose_one_is_guessed() {
        let key = 0x0123_4567_89ab_cdefu64;
        assert_eq!(
            texture_name("texload__0123456789abcdef", key),
            ("texload".into(), true)
        );
        // Another file's key: the whole stem is the guess.
        assert!(!texture_name("texload__0123456789abcdee", key).1);
        let (name, certain) = texture_name("Sicily", key);
        assert_eq!((name.as_str(), certain), ("sicily", false));
        assert_eq!(texture_name(&"x".repeat(40), key).0.len(), NAME_BYTES);
        assert_eq!(picture_name(1, 2), "0000000000000001_0000000000000002");
    }
}
