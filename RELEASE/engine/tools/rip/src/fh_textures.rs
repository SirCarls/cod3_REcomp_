//! Finest Hour (original Xbox) textures for the HD pack, read from the disc's
//! `COD.PAK`, keyed the way the port's renderer finds them.
//!
//! The game's textures are RenderWare texture dictionaries (TXD, chunk 0x16)
//! in Xbox form, stored inside the pack's entries; the pack keeps no names,
//! so the dictionaries are found by their chunk headers (see
//! docs/formats/fh.md). Each texture native (chunk 0x15) holds, after a
//! 92-byte header, the palette (256 colors for PAL8, 32 for PAL4) and the
//! swizzled level chain exactly as the game hands it to Direct3D.
//!
//! The key of a texture is the one `RELEASE/fh/src/fh_draw.c` computes when
//! it first draws with it: FNV-1a 64 over the level-0 texels as
//! `texture_pixels` writes them, one little-endian 32-bit color (bytes B, G,
//! R, A) per texel, row by row. That is why the decoding here follows
//! `texture_pixels` to the byte, including what it does not do: X8R8G8B8
//! gets alpha 0xFF, a palette's alpha is kept as stored. The runtime only
//! decodes P8 and 32-bit textures, so only those are listed.
//!
//! Files are named `<width>x<height>_<key as 16 hex digits>.png`, the name
//! the port looks up in the pack, so the upscaled folder is the pack itself.
//! The list beside them is `hd_keys.tsv` in the columns the setup tool's
//! upscaler reads (`hd.rs`'s: key, width, height, format, name, file, kind),
//! with the RenderWare raster format as the format and where the texture
//! was found as an eighth column. Every texture is listed as a color
//! picture (`diffuse`): no material is read, and the textures this reads
//! are palette and 32-bit color pictures.

// The list of every key goes beside the pictures under the name the setup
// tool's upscaler reads, as for the other games.
use crate::hd::KEYS_FILE;
use crate::output::write_if_changed;
use crate::paths::sanitize;
use std::collections::btree_map::Entry;
use std::collections::BTreeMap;
use std::fs::File;
use std::io::{Read, Seek, SeekFrom};
use std::path::{Path, PathBuf};
use std::sync::Mutex;

/// RenderWare chunk types this reader walks.
const CHUNK_STRUCT: u32 = 0x01;
const CHUNK_TEXTURE_NATIVE: u32 = 0x15;
const CHUNK_TEXDICTIONARY: u32 = 0x16;
/// RenderWare's platform id for Xbox texture natives.
const PLATFORM_XBOX: u32 = 5;
/// Bytes of a texture native's struct before its palette: platform, filter,
/// name[32], mask[32], raster format, has-alpha, width, height, depth,
/// levels, type, compression, size of the pixel data.
const NATIVE_HEADER: usize = 92;
/// The largest side `fh_draw.c` uploads (its MAX_TEXTURE_SIDE).
const MAX_SIDE: u32 = 4096;

/// RenderWare raster format bits (rwRASTERFORMAT*).
const RASTER_PAL8: u32 = 0x2000;
const RASTER_PAL4: u32 = 0x4000;
const RASTER_PIXEL_MASK: u32 = 0x0F00;
const RASTER_C8888: u32 = 0x0500;
const RASTER_C888: u32 = 0x0600;

/// The `Spark Pack` header: its version and entry count sit at 0x78, the
/// 16-byte entries (size, offset, name hash) start at 0x80.
const PAK_MAGIC: &[u8] = b"Spark Pack";
const PAK_COUNT_AT: u64 = 0x78;
const PAK_ENTRIES_AT: u64 = 0x80;

type Result<T> = std::result::Result<T, Box<dyn std::error::Error>>;

/// One decoded texture: its name in the dictionary, raster format, level-0
/// size and texels (B, G, R, A per texel).
#[derive(Debug)]
pub struct Texture {
    pub name: String,
    pub raster: u32,
    pub width: u32,
    pub height: u32,
    pub bgra: Vec<u8>,
}

/// Why a texture native was not decoded.
#[derive(Debug, PartialEq, Eq, PartialOrd, Ord, Clone, Copy)]
pub enum Skip {
    /// Compressed (DXT): the port draws those without a texture, so there
    /// is nothing to replace yet.
    Compressed,
    /// A raster format `texture_pixels` doesn't decode.
    Format,
    /// Not a power of two on each side, or larger than the port uploads.
    Size,
    /// The sizes in the header don't fit the chunk.
    Damaged,
}

/// FNV-1a 64 of `bytes`, as `fh_draw.c` hashes a texture's level 0.
pub fn fnv1a64(bytes: &[u8]) -> u64 {
    bytes.iter().fold(0xCBF2_9CE4_8422_2325, |hash, &b| {
        (hash ^ u64::from(b)).wrapping_mul(0x0000_0100_0000_01B3)
    })
}

/// `fh_draw.c`'s `swizzled_offset`: the bits of x and y interleaved, x
/// first, for as long as each side still has bits.
pub fn swizzled_offset(x: u32, y: u32, width: u32, height: u32) -> usize {
    let (mut offset, mut bit, mut side) = (0usize, 1usize, 1u32);
    while side < width || side < height {
        if side < width {
            if x & side != 0 {
                offset |= bit;
            }
            bit <<= 1;
        }
        if side < height {
            if y & side != 0 {
                offset |= bit;
            }
            bit <<= 1;
        }
        side <<= 1;
    }
    offset
}

fn u32_at(d: &[u8], at: usize) -> Option<u32> {
    d.get(at..at + 4)
        .map(|b| u32::from_le_bytes([b[0], b[1], b[2], b[3]]))
}

fn u16_at(d: &[u8], at: usize) -> Option<u16> {
    d.get(at..at + 2).map(|b| u16::from_le_bytes([b[0], b[1]]))
}

/// Decodes one texture native's struct (`st` is the struct's data, after
/// its chunk header).
pub fn decode_native(st: &[u8]) -> std::result::Result<Texture, Skip> {
    let field = |at| u32_at(st, at).ok_or(Skip::Damaged);
    if field(0)? != PLATFORM_XBOX {
        return Err(Skip::Format);
    }
    let name = st.get(8..40).ok_or(Skip::Damaged)?;
    let name = String::from_utf8_lossy(name.split(|&b| b == 0).next().unwrap_or(&[])).into_owned();
    let raster = field(72)?;
    let width = u32::from(u16_at(st, 80).ok_or(Skip::Damaged)?);
    let height = u32::from(u16_at(st, 82).ok_or(Skip::Damaged)?);
    let size = field(88)? as usize; // read first: it proves the bytes before it are there
    let depth = st[84];
    let compression = st[87];
    if compression != 0 {
        return Err(Skip::Compressed);
    }
    if !width.is_power_of_two()
        || !height.is_power_of_two()
        || width > MAX_SIDE
        || height > MAX_SIDE
    {
        return Err(Skip::Size);
    }
    let texels = (width * height) as usize;
    let palette_colors = if raster & RASTER_PAL8 != 0 {
        256
    } else if raster & RASTER_PAL4 != 0 {
        32 // the Xbox driver keeps PAL4 indices a byte each, with a 32-color palette
    } else {
        0
    };
    let pixels_at = NATIVE_HEADER + 4 * palette_colors;
    let pixels = st.get(pixels_at..pixels_at + size).ok_or(Skip::Damaged)?;
    let mut bgra = vec![0u8; texels * 4];
    if palette_colors != 0 {
        if depth != 8 || pixels.len() < texels {
            return Err(Skip::Damaged);
        }
        // texture_pixels: colors past the palette's own count read as 0.
        let mut colors = [[0u8; 4]; 256];
        for (i, c) in colors.iter_mut().take(palette_colors).enumerate() {
            c.copy_from_slice(&st[NATIVE_HEADER + 4 * i..NATIVE_HEADER + 4 * i + 4]);
        }
        for y in 0..height {
            for x in 0..width {
                let index = pixels[swizzled_offset(x, y, width, height)];
                let out = 4 * (y * width + x) as usize;
                bgra[out..out + 4].copy_from_slice(&colors[usize::from(index)]);
            }
        }
    } else {
        let pixel = raster & RASTER_PIXEL_MASK;
        if depth != 32 || (pixel != RASTER_C8888 && pixel != RASTER_C888) {
            return Err(Skip::Format);
        }
        if pixels.len() < texels * 4 {
            return Err(Skip::Damaged);
        }
        for y in 0..height {
            for x in 0..width {
                let at = 4 * swizzled_offset(x, y, width, height);
                let out = 4 * (y * width + x) as usize;
                bgra[out..out + 4].copy_from_slice(&pixels[at..at + 4]);
                if pixel == RASTER_C888 {
                    bgra[out + 3] = 0xFF; // D3D X8R8G8B8: texture_pixels forces alpha
                }
            }
        }
    }
    Ok(Texture {
        name,
        raster,
        width,
        height,
        bgra,
    })
}

/// Every texture native of every RenderWare texture dictionary in `data`.
/// Dictionaries are found by their header: chunk 0x16 whose first child is
/// a 4-byte struct of the same RenderWare version.
pub fn each_native(data: &[u8], mut each: impl FnMut(std::result::Result<Texture, Skip>)) -> usize {
    let mut dictionaries = 0;
    let mut at = 0;
    while at + 28 <= data.len() {
        let is_dictionary = u32_at(data, at) == Some(CHUNK_TEXDICTIONARY)
            && u32_at(data, at + 12) == Some(CHUNK_STRUCT)
            && u32_at(data, at + 16) == Some(4)
            && u32_at(data, at + 8) == u32_at(data, at + 20);
        if !is_dictionary {
            at += 4; // chunks start on 4-byte boundaries
            continue;
        }
        dictionaries += 1;
        let count = u16_at(data, at + 24).unwrap_or(0);
        let end = at
            .saturating_add(12)
            .saturating_add(u32_at(data, at + 4).unwrap_or(0) as usize)
            .min(data.len());
        let mut q = at + 28;
        for _ in 0..count {
            let (Some(kind), Some(size)) = (u32_at(data, q), u32_at(data, q + 4)) else {
                break;
            };
            let struct_size = u32_at(data, q + 16).unwrap_or(0) as usize;
            if kind != CHUNK_TEXTURE_NATIVE || u32_at(data, q + 12) != Some(CHUNK_STRUCT) {
                each(Err(Skip::Damaged));
                break;
            }
            match data.get(q + 24..q + 24 + struct_size) {
                Some(st) => each(decode_native(st)),
                None => each(Err(Skip::Damaged)),
            }
            q += 12 + size as usize;
        }
        at = end.max(at + 4);
        at += (4 - at % 4) % 4;
    }
    dictionaries
}

/// The byte ranges of `file` to search: each entry of a Spark pack, or the
/// whole file for anything else (a loose `.TXD`).
fn ranges(file: &mut File) -> Result<Vec<(u64, usize)>> {
    let length = file.metadata()?.len();
    let mut head = [0u8; 16];
    file.read_exact(&mut head).ok();
    if !head.starts_with(PAK_MAGIC) {
        return Ok(vec![(0, length as usize)]);
    }
    let mut word = [0u8; 4];
    file.seek(SeekFrom::Start(PAK_COUNT_AT + 4))?;
    file.read_exact(&mut word)?;
    let count = u64::from(u32::from_le_bytes(word));
    let mut table = vec![0u8; (count * 16) as usize];
    file.seek(SeekFrom::Start(PAK_ENTRIES_AT))?;
    file.read_exact(&mut table)?;
    let mut found = Vec::new();
    for entry in table.chunks_exact(16) {
        let size = u64::from(u32_at(entry, 0).unwrap_or(0));
        let offset = u64::from(u32_at(entry, 4).unwrap_or(0));
        if size == 0 || offset + size > length {
            return Err(format!("pack entry at {offset:#x} runs past the end of the file").into());
        }
        found.push((offset, size as usize));
    }
    Ok(found)
}

/// `COD.PAK` and any loose `.txd` under a folder, or the one file given.
fn inputs(input: &Path) -> Result<Vec<PathBuf>> {
    if !input.is_dir() {
        return Ok(vec![input.to_path_buf()]);
    }
    let mut files = crate::jobs::find_files(input, "pak")?;
    files.extend(crate::jobs::find_files(input, "txd")?);
    files.sort();
    if files.is_empty() {
        return Err(format!("no .pak or .txd files under {}", input.display()).into());
    }
    Ok(files)
}

/// One line of hd_keys.tsv. `origin` (input file, pack entry, texture's
/// place in it) picks which copy of a picture found several times is
/// listed, the first in the file, so the list is the same whatever order
/// threads finish in.
#[derive(Debug)]
struct KeyLine {
    name: String,
    raster: u32,
    source: String,
    origin: (usize, usize, usize),
}

/// The file name of a texture's picture, and the name the port looks up.
pub fn picture_name(width: u32, height: u32, key: u64) -> String {
    format!("{width}x{height}_{key:016x}")
}

/// The picture for the upscaler: RGBA as the game has it, made opaque when
/// every alpha is 0 (a palette that keeps no alpha), so the upscaler sees
/// the colors. Only the texels decide, so every copy of a picture gives the
/// same file.
fn png_of(texture: &Texture) -> Vec<u8> {
    let opaque = texture.bgra.chunks_exact(4).all(|c| c[3] == 0);
    let rgba: Vec<u8> = texture
        .bgra
        .chunks_exact(4)
        .flat_map(|c| [c[2], c[1], c[0], if opaque { 0xFF } else { c[3] }])
        .collect();
    png_write::encode_rgba(texture.width, texture.height, &rgba)
}

/// `rip fh-hd-textures <COD.PAK | disc folder> <out_dir>`: every texture the
/// port can replace, as `<out_dir>/textures/<w>x<h>_<key>.png` with
/// `textures/hd_keys.tsv` (see the module's text).
/// The same picture in several dictionaries is one key.
pub fn hd_textures(input: &Path, out_dir: &Path, workers: usize) -> Result<bool> {
    let textures = out_dir.join("textures");
    std::fs::create_dir_all(&textures)?;
    let lines = Mutex::new(BTreeMap::<(u32, u32, u64), KeyLine>::new());
    let skipped = Mutex::new(BTreeMap::<Skip, usize>::new());
    let mut failures = 0;
    for (file_index, file) in inputs(input)?.into_iter().enumerate() {
        let label = file
            .file_name()
            .map(|n| n.to_string_lossy().into_owned())
            .unwrap_or_default();
        let ranges = ranges(&mut File::open(&file)?)?;
        let indexed: Vec<(usize, (u64, usize))> = ranges.into_iter().enumerate().collect();
        let error = Mutex::new(None::<String>);
        crate::jobs::for_each_parallel(
            &indexed,
            workers,
            |&(entry, (offset, size))| -> std::io::Result<()> {
                let mut data = vec![0u8; size];
                let mut f = File::open(&file)?;
                f.seek(SeekFrom::Start(offset))?;
                f.read_exact(&mut data)?;
                let mut result = Ok(());
                let mut ordinal = 0;
                each_native(&data, |native| match native {
                    Err(skip) => {
                        *skipped
                            .lock()
                            .unwrap_or_else(|e| e.into_inner())
                            .entry(skip)
                            .or_default() += 1;
                    }
                    Ok(t) if result.is_ok() => {
                        ordinal += 1;
                        let key = fnv1a64(&t.bgra);
                        let line = KeyLine {
                            name: sanitize(&t.name),
                            raster: t.raster,
                            source: format!("{label}#{entry}"),
                            origin: (file_index, entry, ordinal),
                        };
                        let mut lines = lines.lock().unwrap_or_else(|e| e.into_inner());
                        match lines.entry((t.width, t.height, key)) {
                            Entry::Occupied(mut seen) => {
                                if line.origin < seen.get().origin {
                                    seen.insert(line);
                                }
                            }
                            Entry::Vacant(new) => {
                                new.insert(line);
                                drop(lines); // the picture is written without holding the list
                                let name = picture_name(t.width, t.height, key);
                                result = write_if_changed(
                                    &textures.join(format!("{name}.png")),
                                    &png_of(&t),
                                )
                                .map(|_| ());
                            }
                        }
                    }
                    Ok(_) => {}
                });
                result
            },
            |&(entry, _), result| {
                if let Err(e) = result {
                    *error.lock().unwrap_or_else(|e| e.into_inner()) =
                        Some(format!("entry {entry}: {e}"));
                }
            },
        );
        if let Some(e) = error.into_inner().unwrap_or_else(|e| e.into_inner()) {
            eprintln!("error: {}: {e}", file.display());
            failures += 1;
        }
    }
    let lines = lines.into_inner().unwrap_or_else(|e| e.into_inner());
    let mut text = String::from("# key\twidth\theight\traster_format\tname\tfile\tkind\tsource\n");
    for ((width, height, key), line) in &lines {
        text.push_str(&format!(
            "{key:016x}\t{width}\t{height}\t{:#06x}\t{}\t{}\tdiffuse\t{}\n",
            line.raster,
            line.name,
            picture_name(*width, *height, *key),
            line.source
        ));
    }
    write_if_changed(&textures.join(KEYS_FILE), text.as_bytes())?;
    let skipped = skipped.into_inner().unwrap_or_else(|e| e.into_inner());
    println!(
        "{KEYS_FILE}: {} pictures for the HD pack; not listed: {skipped:?}",
        lines.len()
    );
    Ok(failures == 0)
}

#[cfg(test)]
mod tests {
    use super::*;

    /// A texture native struct: Xbox, the given raster and size, then the
    /// palette and pixels.
    fn native(
        raster: u32,
        width: u16,
        height: u16,
        depth: u8,
        palette: &[u8],
        pixels: &[u8],
    ) -> Vec<u8> {
        let mut st = Vec::new();
        st.extend(PLATFORM_XBOX.to_le_bytes());
        st.extend(0u32.to_le_bytes());
        let mut name = [0u8; 32];
        name[..4].copy_from_slice(b"wall");
        st.extend(name);
        st.extend([0u8; 32]);
        st.extend(raster.to_le_bytes());
        st.extend(1u32.to_le_bytes());
        st.extend(width.to_le_bytes());
        st.extend(height.to_le_bytes());
        st.extend([depth, 1, 4, 0]);
        st.extend((pixels.len() as u32).to_le_bytes());
        st.extend(palette);
        st.extend(pixels);
        st
    }

    #[test]
    fn the_hash_is_fnv1a_64() {
        assert_eq!(fnv1a64(b""), 0xCBF2_9CE4_8422_2325);
        assert_eq!(fnv1a64(b"a"), 0xAF63_DC4C_8601_EC8C);
    }

    #[test]
    fn swizzle_interleaves_x_first_until_a_side_runs_out() {
        assert_eq!(swizzled_offset(1, 0, 4, 4), 1);
        assert_eq!(swizzled_offset(0, 1, 4, 4), 2);
        assert_eq!(swizzled_offset(2, 0, 4, 4), 4);
        // 4 wide, 2 high: past y's one bit, x's bits follow on.
        assert_eq!(swizzled_offset(2, 0, 4, 2), 4);
        assert_eq!(swizzled_offset(3, 1, 4, 2), 7);
    }

    #[test]
    fn a_pal4_texture_reads_32_colors_and_leaves_the_rest_black() {
        let mut palette = vec![0u8; 128];
        palette[4..8].copy_from_slice(&[1, 2, 3, 4]);
        let pixels = [1u8, 0, 40, 1]; // swizzled 2 x 2: (0,0) (1,0) (0,1) (1,1)
        let t = decode_native(&native(
            RASTER_PAL4 | RASTER_C888,
            2,
            2,
            8,
            &palette,
            &pixels,
        ))
        .unwrap();
        assert_eq!(t.bgra, [1, 2, 3, 4, 0, 0, 0, 0, 0, 0, 0, 0, 1, 2, 3, 4]);
    }

    #[test]
    fn x8r8g8b8_gets_opaque_alpha_in_its_key_and_a8r8g8b8_keeps_it() {
        let pixels = [9u8, 8, 7, 0];
        let x = decode_native(&native(RASTER_C888, 1, 1, 32, &[], &pixels)).unwrap();
        let a = decode_native(&native(RASTER_C8888, 1, 1, 32, &[], &pixels)).unwrap();
        assert_eq!(x.bgra, [9, 8, 7, 0xFF]);
        assert_eq!(a.bgra, [9, 8, 7, 0]);
    }

    #[test]
    fn compressed_and_odd_sizes_are_skipped() {
        let mut st = native(RASTER_C8888, 4, 4, 32, &[], &[0; 64]);
        st[87] = 0x0C;
        assert_eq!(decode_native(&st).unwrap_err(), Skip::Compressed);
        let st = native(RASTER_C8888, 3, 4, 32, &[], &[0; 48]);
        assert_eq!(decode_native(&st).unwrap_err(), Skip::Size);
    }

    #[test]
    fn dictionaries_are_found_by_their_header_anywhere() {
        let st = native(RASTER_C8888, 1, 1, 32, &[], &[1, 2, 3, 4]);
        let version = 0x1803_FFFFu32;
        let chunk = |kind: u32, body: &[u8]| {
            let mut c = Vec::new();
            c.extend(kind.to_le_bytes());
            c.extend((body.len() as u32).to_le_bytes());
            c.extend(version.to_le_bytes());
            c.extend(body);
            c
        };
        let texture = chunk(CHUNK_TEXTURE_NATIVE, &chunk(CHUNK_STRUCT, &st));
        let mut body = chunk(CHUNK_STRUCT, &[1, 0, 0, 0]);
        body.extend(&texture);
        let mut data = vec![0xAAu8; 8];
        data.extend(chunk(CHUNK_TEXDICTIONARY, &body));
        let mut found = Vec::new();
        assert_eq!(each_native(&data, |t| found.push(t.unwrap().bgra)), 1);
        assert_eq!(found, [vec![1, 2, 3, 4]]);
    }
}
