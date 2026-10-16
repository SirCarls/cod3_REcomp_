//! Xbox 360 (Xenos GPU) texture data: fetch constants, tiled layout, and
//! conversion to plain DDS files.
//!
//! A Call of Duty 3 texture object holds a D3D texture header whose last six
//! words are the GPU's texture fetch constant. The pixel data lives in the
//! archive's GPU area, tiled and byte-swapped the way the Xenos GPU reads it.
//!
//! Field layout of the fetch constant (the parts used here), as documented by
//! the Xenia emulator (`xenos.h`, `xe_gpu_texture_fetch_t`):
//!
//! ```text
//! dword0  bits 0-1 type (2 = texture), bits 22-30 pitch / 32, bit 31 tiled
//! dword1  bits 0-5 format, bits 6-7 endianness, bits 12-31 base address >> 12
//! dword2  bits 0-12 width - 1, bits 13-25 height - 1        (2D textures)
//! dword3  bits 1-12 swizzle: four 3-bit component sources (R, G, B, A)
//! dword5  bits 9-10 dimension, bit 11 packed mips, bits 12-31 mip address >> 12
//! ```
//!
//! The tiled address function is the well-known `XGAddress2DTiledOffset`
//! (the same arithmetic as Xenia's `texture_address::Tiled2D`).
//!
//! Packed mips: the levels 16 texels or smaller share one 32 x 32 tile. When
//! the texture's smaller side is already 16 or less (a 256 x 16 trim, a
//! 16 x 16 icon), the base level itself lies in that tile, 16 texels in
//! (`FetchConstant::packed_base_offset`, after Xenia's `GetPackedMipOffset`);
//! the first rows of the tile hold the smaller levels and padding.

#![forbid(unsafe_code)]

pub mod bc;
pub mod bc_encode;
pub mod xxh64;

use std::fmt;

/// The texture formats seen in Call of Duty 3 so far (Xenos format numbers).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Format {
    Dxt1,
    Dxt2_3,
    Dxt4_5,
    /// DXN / ATI2 (two-channel normal maps).
    Dxn,
    A8R8G8B8,
    Other(u32),
}

impl Format {
    pub fn from_xenos(n: u32) -> Self {
        match n {
            18 => Format::Dxt1,
            19 => Format::Dxt2_3,
            20 => Format::Dxt4_5,
            49 => Format::Dxn,
            6 => Format::A8R8G8B8,
            other => Format::Other(other),
        }
    }

    /// (block width in texels, bytes per block)
    pub fn block(self) -> Option<(u32, u32)> {
        match self {
            Format::Dxt1 => Some((4, 8)),
            Format::Dxt2_3 | Format::Dxt4_5 | Format::Dxn => Some((4, 16)),
            Format::A8R8G8B8 => Some((1, 4)),
            Format::Other(_) => None,
        }
    }

    /// The Xenos format number (`from_xenos` reversed).
    pub fn to_xenos(self) -> u32 {
        match self {
            Format::Dxt1 => 18,
            Format::Dxt2_3 => 19,
            Format::Dxt4_5 => 20,
            Format::Dxn => 49,
            Format::A8R8G8B8 => 6,
            Format::Other(n) => n,
        }
    }

    /// The block-compressed format of the data, or `None` for A8R8G8B8 and
    /// unknown formats.
    pub fn block_format(self) -> Option<bc::BlockFormat> {
        match self {
            Format::Dxt1 => Some(bc::BlockFormat::Bc1),
            Format::Dxt2_3 => Some(bc::BlockFormat::Bc2),
            Format::Dxt4_5 => Some(bc::BlockFormat::Bc3),
            Format::Dxn => Some(bc::BlockFormat::Bc5),
            Format::A8R8G8B8 | Format::Other(_) => None,
        }
    }

    fn dds_fourcc(self) -> Option<&'static [u8; 4]> {
        match self {
            Format::Dxt1 => Some(b"DXT1"),
            Format::Dxt2_3 => Some(b"DXT3"),
            Format::Dxt4_5 => Some(b"DXT5"),
            Format::Dxn => Some(b"ATI2"),
            _ => None,
        }
    }
}

/// How the GPU swaps bytes when it reads the data.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Endian {
    None,
    Swap8In16,
    Swap8In32,
    Swap16In32,
}

/// The swizzle of a 32-bit texture whose memory holds B, G, R, A after the
/// swap (Direct3D's A8R8G8B8): R from the third byte, G the second, B the
/// first, A the fourth. Every 8_8_8_8 texture of Call of Duty 3 uses it.
pub const SWIZZLE_BGRA_MEMORY: [u8; 4] = [2, 1, 0, 3];
/// The swizzle that takes the components as the format stores them.
pub const SWIZZLE_IDENTITY: [u8; 4] = [0, 1, 2, 3];
/// Levels this small, on the smaller side, are packed into one tile.
const PACKED_MIP_MAX_SIDE: u32 = 16;

#[derive(Debug, Clone)]
pub struct FetchConstant {
    pub tiled: bool,
    pub pitch: u32,
    pub format: Format,
    pub endian: Endian,
    pub width: u32,
    pub height: u32,
    /// Where each of R, G, B, A comes from: 0-3 the format's components in
    /// memory order, 4 constant 0, 5 constant 1.
    pub swizzle: [u8; 4],
    pub dimension: u32,
    pub packed_mips: bool,
    /// Offset of the mip chain from the base data (0 when there is none).
    pub mip_offset: u32,
}

impl FetchConstant {
    pub fn parse(words: [u32; 6]) -> Self {
        let swizzle_bits = (words[3] >> 1) & 0xFFF;
        FetchConstant {
            tiled: words[0] >> 31 == 1,
            pitch: ((words[0] >> 22) & 0x1FF) * 32,
            format: Format::from_xenos(words[1] & 0x3F),
            endian: match (words[1] >> 6) & 3 {
                0 => Endian::None,
                1 => Endian::Swap8In16,
                2 => Endian::Swap8In32,
                _ => Endian::Swap16In32,
            },
            width: (words[2] & 0x1FFF) + 1,
            height: ((words[2] >> 13) & 0x1FFF) + 1,
            swizzle: std::array::from_fn(|i| ((swizzle_bits >> (3 * i)) & 7) as u8),
            dimension: (words[5] >> 9) & 3,
            packed_mips: (words[5] >> 11) & 1 == 1,
            mip_offset: words[5] & 0xFFFF_F000,
        }
    }

    /// True when the swizzle is the one this crate's DDS and PNG files
    /// assume for the format: components in memory order for the block
    /// formats, B, G, R, A in memory for 8_8_8_8. A texture with another
    /// swizzle would get its channels mixed up, so it is left alone.
    pub fn has_expected_swizzle(&self) -> bool {
        match self.format {
            Format::A8R8G8B8 => self.swizzle == SWIZZLE_BGRA_MEMORY,
            Format::Dxt1 | Format::Dxt2_3 | Format::Dxt4_5 | Format::Dxn => {
                self.swizzle == SWIZZLE_IDENTITY
            }
            Format::Other(_) => false,
        }
    }

    /// True when the base level lies in the packed mip tile: packed mips and
    /// a smaller side of 16 texels or less.
    pub fn base_in_packed_tile(&self) -> bool {
        self.packed_mips && self.width.min(self.height) <= PACKED_MIP_MAX_SIDE
    }

    /// Where the base level starts inside its tile, in blocks: (0, 0) unless
    /// it is in the packed mip tile, where a texture wider than tall sits 16
    /// texels down and any other 16 texels to the right (Xenia's
    /// `GetPackedMipOffset` for level 0).
    pub fn packed_base_offset(&self) -> (u32, u32) {
        if !self.base_in_packed_tile() {
            return (0, 0);
        }
        let block = self.format.block().map_or(1, |(block, _)| block);
        let log2 = |side: u32| 32 - (side.max(1) - 1).leading_zeros(); // ceil(log2)
        if log2(self.width) > log2(self.height) {
            (0, PACKED_MIP_MAX_SIDE / block)
        } else {
            (PACKED_MIP_MAX_SIDE / block, 0)
        }
    }
}

#[derive(Debug)]
pub enum Error {
    UnsupportedFormat(Format),
    NotTwoD(u32),
    Truncated { need: usize, have: usize },
}

impl fmt::Display for Error {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Error::UnsupportedFormat(fmt_) => {
                write!(f, "texture format {fmt_:?} is not handled yet")
            }
            Error::NotTwoD(d) => write!(f, "texture dimension {d} is not 2D"),
            Error::Truncated { need, have } => {
                write!(f, "texture data is {have} bytes, needs {need}")
            }
        }
    }
}

impl std::error::Error for Error {}

/// Block index of block (x, y) in a tiled surface `width_blocks` wide, with
/// `bytes_per_block` bytes per block (XGAddress2DTiledOffset).
pub fn tiled_offset_2d(x: u32, y: u32, width_blocks: u32, bytes_per_block: u32) -> u32 {
    let aligned_width = (width_blocks + 31) & !31;
    let log_bpp = (bytes_per_block >> 2) + ((bytes_per_block >> 1) >> (bytes_per_block >> 2));
    let macro_ = ((x >> 5) + (y >> 5) * (aligned_width >> 5)) << (log_bpp + 7);
    let micro = ((x & 7) + ((y & 6) << 2)) << log_bpp;
    let offset =
        macro_ + ((micro & !15) << 1) + (micro & 15) + ((y & 8) << (3 + log_bpp)) + ((y & 1) << 4);
    // Note: in Rust `+` binds tighter than `<<`, so every shift is bracketed.
    let bank_pipe = ((((y & 8) >> 2) + (x >> 3)) & 3) << 6;
    (((offset & !511) << 3) + ((offset & 448) << 2) + (offset & 63) + ((y & 16) << 7) + bank_pipe)
        >> log_bpp
}

fn swap_bytes(data: &mut [u8], endian: Endian) {
    match endian {
        Endian::None => {}
        Endian::Swap8In16 => data.chunks_exact_mut(2).for_each(|c| c.swap(0, 1)),
        Endian::Swap8In32 => data.chunks_exact_mut(4).for_each(|c| c.reverse()),
        Endian::Swap16In32 => data.chunks_exact_mut(4).for_each(|c| {
            c.swap(0, 2);
            c.swap(1, 3);
        }),
    }
}

/// Untiles and byte-swaps the base level into linear rows of blocks.
pub fn base_level_linear(fetch: &FetchConstant, gpu_data: &[u8]) -> Result<Vec<u8>, Error> {
    if fetch.dimension != 1 {
        return Err(Error::NotTwoD(fetch.dimension));
    }
    let (block, bpb) = fetch
        .format
        .block()
        .ok_or(Error::UnsupportedFormat(fetch.format))?;
    let width_blocks = fetch.width.div_ceil(block);
    let height_blocks = fetch.height.div_ceil(block);
    let pitch_blocks = (fetch.pitch / block).max(width_blocks);
    let (x_offset, y_offset) = fetch.packed_base_offset();
    let bpb = bpb as usize;

    let mut out = vec![0u8; (width_blocks * height_blocks) as usize * bpb];
    for y in 0..height_blocks {
        for x in 0..width_blocks {
            let (tile_x, tile_y) = (x + x_offset, y + y_offset);
            let src_block = if fetch.tiled {
                tiled_offset_2d(tile_x, tile_y, pitch_blocks, bpb as u32)
            } else {
                tile_y * pitch_blocks + tile_x
            } as usize;
            let src = src_block * bpb;
            let chunk = gpu_data.get(src..src + bpb).ok_or(Error::Truncated {
                need: src + bpb,
                have: gpu_data.len(),
            })?;
            let dst = ((y * width_blocks + x) as usize) * bpb;
            out[dst..dst + bpb].copy_from_slice(chunk);
        }
    }
    swap_bytes(&mut out, fetch.endian);
    Ok(out)
}

/// True for the textures the HD texture pack can replace: 2D, one of the
/// five formats, with the swizzle the pack's files assume (the game's
/// texture cache can't see the swizzle, so a texture with an unusual one is
/// simply never listed).
pub fn hd_replaceable(fetch: &FetchConstant) -> bool {
    fetch.dimension == 1 && fetch.format.block().is_some() && fetch.has_expected_swizzle()
}

/// The key that names a texture in the HD texture pack: XXH64 (seed 0) of
/// its base level exactly as `base_level_linear` returns it (untiled, rows
/// of blocks packed tightly, byte-swapped). The game hashes the same bytes
/// from guest memory.
pub fn hd_key(linear: &[u8]) -> u64 {
    xxh64::xxh64(linear, 0)
}

/// Size of a DDS file's header, including the "DDS " magic.
pub const DDS_HEADER_BYTES: usize = 128;
/// The R, G, B, A channel masks of a 32-bit A8R8G8B8 DDS: bytes B, G, R, A.
const A8R8G8B8_MASKS: [u32; 4] = [0x00FF_0000, 0x0000_FF00, 0x0000_00FF, 0xFF00_0000];

/// A legacy DDS header for a `width` x `height` texture in `format` whose
/// first level is `top_level_bytes` long, with `mip_count` levels (0 writes
/// the plain one-level header).
fn dds_header(
    format: Format,
    width: u32,
    height: u32,
    top_level_bytes: u32,
    mip_count: u32,
) -> Result<Vec<u8>, Error> {
    let mut h = Vec::with_capacity(DDS_HEADER_BYTES);
    let le = |v: u32| v.to_le_bytes();
    const DDSD_CAPS_HEIGHT_WIDTH_PIXELFORMAT: u32 = 0x1 | 0x2 | 0x4 | 0x1000;
    const DDSD_PITCH: u32 = 0x8;
    const DDSD_MIPMAPCOUNT: u32 = 0x20000;
    const DDSD_LINEARSIZE: u32 = 0x80000;
    const DDSCAPS_COMPLEX: u32 = 0x8;
    const DDSCAPS_TEXTURE: u32 = 0x1000;
    const DDSCAPS_MIPMAP: u32 = 0x40_0000;
    let (mip_flag, mip_caps) = if mip_count > 0 {
        (DDSD_MIPMAPCOUNT, DDSCAPS_COMPLEX | DDSCAPS_MIPMAP)
    } else {
        (0, 0)
    };
    h.extend_from_slice(b"DDS ");
    h.extend_from_slice(&le(124));
    match format.dds_fourcc() {
        Some(fourcc) => {
            h.extend_from_slice(&le(DDSD_CAPS_HEIGHT_WIDTH_PIXELFORMAT
                | DDSD_LINEARSIZE
                | mip_flag));
            h.extend_from_slice(&le(height));
            h.extend_from_slice(&le(width));
            h.extend_from_slice(&le(top_level_bytes));
            h.extend_from_slice(&le(0)); // depth
            h.extend_from_slice(&le(mip_count));
            h.extend_from_slice(&[0u8; 4 * 11]); // reserved
            h.extend_from_slice(&le(32));
            h.extend_from_slice(&le(0x4)); // DDPF_FOURCC
            h.extend_from_slice(fourcc);
            h.extend_from_slice(&[0u8; 4 * 5]);
        }
        None if format == Format::A8R8G8B8 => {
            h.extend_from_slice(&le(DDSD_CAPS_HEIGHT_WIDTH_PIXELFORMAT
                | DDSD_PITCH
                | mip_flag));
            h.extend_from_slice(&le(height));
            h.extend_from_slice(&le(width));
            h.extend_from_slice(&le(width * 4));
            h.extend_from_slice(&le(0)); // depth
            h.extend_from_slice(&le(mip_count));
            h.extend_from_slice(&[0u8; 4 * 11]);
            h.extend_from_slice(&le(32));
            h.extend_from_slice(&le(0x41)); // DDPF_RGB | DDPF_ALPHAPIXELS
            h.extend_from_slice(&[0u8; 4]);
            h.extend_from_slice(&le(32));
            // After the swap the game's 8_8_8_8 textures hold B, G, R, A in
            // memory (Direct3D's A8R8G8B8, see SWIZZLE_BGRA_MEMORY): the
            // usual 32-bit DDS masks.
            h.extend_from_slice(&le(A8R8G8B8_MASKS[0]));
            h.extend_from_slice(&le(A8R8G8B8_MASKS[1]));
            h.extend_from_slice(&le(A8R8G8B8_MASKS[2]));
            h.extend_from_slice(&le(A8R8G8B8_MASKS[3]));
        }
        None => return Err(Error::UnsupportedFormat(format)),
    }
    h.extend_from_slice(&le(DDSCAPS_TEXTURE | mip_caps));
    h.extend_from_slice(&[0u8; 4 * 4]);
    debug_assert_eq!(h.len(), DDS_HEADER_BYTES);
    Ok(h)
}

/// The format, width and height of a DDS file this crate wrote, from its
/// first 128 bytes; `None` for anything else.
pub fn dds_info(header: &[u8]) -> Option<(Format, u32, u32)> {
    let word = |i: usize| -> Option<u32> {
        Some(u32::from_le_bytes(header.get(i..i + 4)?.try_into().ok()?))
    };
    if header.get(..4)? != b"DDS " || word(4)? != 124 {
        return None;
    }
    let (height, width) = (word(12)?, word(16)?);
    let pixel_flags = word(80)?;
    let format = if pixel_flags & 0x4 != 0 {
        // DDPF_FOURCC
        let fourcc = header.get(84..88)?;
        [Format::Dxt1, Format::Dxt2_3, Format::Dxt4_5, Format::Dxn]
            .into_iter()
            .find(|f| f.dds_fourcc().is_some_and(|c| c.as_slice() == fourcc))?
    } else if word(88)? == 32 && [word(92)?, word(96)?, word(100)?, word(104)?] == A8R8G8B8_MASKS {
        // 32-bit RGB with the B, G, R, A byte order dds_header writes.
        Format::A8R8G8B8
    } else {
        return None;
    };
    Some((format, width, height))
}

/// The header `dds_file_mipmapped` writes for `level_count` levels whose
/// first is `top_level_bytes` long, for checking an existing file.
pub fn dds_mipmapped_header(
    format: Format,
    width: u32,
    height: u32,
    top_level_bytes: usize,
    level_count: usize,
) -> Result<Vec<u8>, Error> {
    dds_header(
        format,
        width,
        height,
        top_level_bytes as u32,
        level_count.max(1) as u32,
    )
}

/// A DDS file (one mip level) holding `linear` in the fetch constant's format.
pub fn dds_file(fetch: &FetchConstant, linear: &[u8]) -> Result<Vec<u8>, Error> {
    let mut file = dds_header(
        fetch.format,
        fetch.width,
        fetch.height,
        linear.len() as u32,
        0,
    )?;
    file.extend_from_slice(linear);
    Ok(file)
}

/// A DDS file with a mip chain: `levels[0]` is the `width` x `height` level,
/// each next one half the size (at least 1), in `format`'s linear layout
/// (block rows for BC formats, B G R A bytes per texel for A8R8G8B8).
pub fn dds_file_mipmapped(
    format: Format,
    width: u32,
    height: u32,
    levels: &[Vec<u8>],
) -> Result<Vec<u8>, Error> {
    let top = levels.first().map_or(0, Vec::len);
    let mut file = dds_mipmapped_header(format, width, height, top, levels.len())?;
    for level in levels {
        file.extend_from_slice(level);
    }
    Ok(file)
}

/// Swaps the first and third byte of every texel: the game's 8_8_8_8 layout
/// (B, G, R, A in memory) to R, G, B, A, and back.
pub fn bgra_to_rgba(texels: &[u8]) -> Vec<u8> {
    texels
        .chunks_exact(4)
        .flat_map(|p| [p[2], p[1], p[0], p[3]])
        .collect()
}

/// The base level as RGBA8 pixels (`width * height * 4` bytes), from the
/// linear data `base_level_linear` returns.
pub fn rgba8(fetch: &FetchConstant, linear: &[u8]) -> Result<Vec<u8>, Error> {
    let (w, h) = (fetch.width as usize, fetch.height as usize);
    let block_format = match fetch.format.block_format() {
        Some(block_format) => block_format,
        None if fetch.format == Format::A8R8G8B8 => {
            let need = w * h * 4;
            let pixels = linear.get(..need).ok_or(Error::Truncated {
                need,
                have: linear.len(),
            })?;
            // After the swap the bytes are B, G, R, A (see dds_header).
            return Ok(bgra_to_rgba(pixels));
        }
        None => return Err(Error::UnsupportedFormat(fetch.format)),
    };
    bc::decode(block_format, w, h, linear).ok_or(Error::Truncated {
        need: w.div_ceil(4) * h.div_ceil(4) * block_format.block_bytes(),
        have: linear.len(),
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn fetch_constant_of_a_512_dxt3_texture() {
        // Words from saint_lo.cod, texture bgr$ra_bushline_c.
        let f = FetchConstant::parse([
            0x8400_0002,
            0x0000_0053,
            0x003f_e1ff,
            0x0000_0d10,
            0x0000_0240,
            0x0004_0a00,
        ]);
        assert!(f.tiled);
        assert_eq!((f.width, f.height, f.pitch), (512, 512, 512));
        assert_eq!(f.format, Format::Dxt2_3);
        assert_eq!(f.endian, Endian::Swap8In16);
        assert_eq!(f.swizzle, SWIZZLE_IDENTITY);
        assert!(f.has_expected_swizzle());
        assert_eq!(f.dimension, 1);
        assert_eq!(f.mip_offset, 0x40000);
        assert!(!f.base_in_packed_tile());
        assert_eq!(f.packed_base_offset(), (0, 0));

        // The fog ramp of saint_lo.cod: 256 x 8 8_8_8_8, swizzle Z Y X W
        // (B, G, R, A in memory), 8-in-32 swap, packed mips.
        let ramp = FetchConstant::parse([
            0x8200_0002,
            0x0000_0086,
            0x0000_e0ff,
            0x0000_0c14,
            0x0000_0200,
            0x0000_0a00,
        ]);
        assert_eq!((ramp.width, ramp.height), (256, 8));
        assert_eq!(ramp.format, Format::A8R8G8B8);
        assert_eq!(ramp.endian, Endian::Swap8In32);
        assert_eq!(ramp.swizzle, SWIZZLE_BGRA_MEMORY);
        assert!(ramp.has_expected_swizzle());
        assert!(ramp.packed_mips && ramp.base_in_packed_tile());
        // Wider than tall: the base level is 16 texel rows down.
        assert_eq!(ramp.packed_base_offset(), (0, 16));
    }

    #[test]
    fn packed_base_levels() {
        // weaponnameback (global.cod): 256 x 16 DXT5 with packed mips, the
        // base level 16 texels (4 block rows) down in the tile.
        let wide = FetchConstant::parse([
            0x8200_0002,
            0x0000_0054,
            0x0001_e0ff,
            0x0000_0d10,
            0x0000_0200,
            0x0000_0a00,
        ]);
        assert_eq!((wide.width, wide.height), (256, 16));
        assert_eq!(wide.packed_base_offset(), (0, 4));
        // Taller than wide, and square: 16 texels to the right.
        let mut tall = wide.clone();
        (tall.width, tall.height) = (16, 32);
        assert_eq!(tall.packed_base_offset(), (4, 0));
        (tall.width, tall.height) = (16, 16);
        assert_eq!(tall.packed_base_offset(), (4, 0));
        // A 17-texel side is not packed; without the packed flag nothing is.
        (tall.width, tall.height) = (17, 64);
        assert_eq!(tall.packed_base_offset(), (0, 0));
        (tall.width, tall.height) = (8, 8);
        tall.packed_mips = false;
        assert_eq!(tall.packed_base_offset(), (0, 0));

        // A 8 x 4 DXT1 texture with packed mips (two 4 x 4 blocks), tiled
        // with a 32-texel pitch: its blocks are read from block row 4.
        let mut tiny = wide;
        (tiny.width, tiny.height, tiny.pitch) = (8, 4, 32);
        tiny.format = Format::Dxt1;
        let mut gpu = vec![0u8; 32 * 32 * 8];
        for (i, block) in [0xA5u8, 0x5A].into_iter().enumerate() {
            let at = tiled_offset_2d(i as u32, 4, 8, 8) as usize * 8;
            gpu[at..at + 8].copy_from_slice(&[block; 8]);
        }
        let linear = base_level_linear(&tiny, &gpu).unwrap();
        assert_eq!(linear, [[0xA5u8; 8], [0x5A; 8]].concat());
        assert!(hd_replaceable(&tiny));
    }

    #[test]
    fn a8r8g8b8_channels() {
        // Memory B, G, R, A (after the swap) to R, G, B, A and back.
        let bgra = [10u8, 20, 30, 40, 50, 60, 70, 80];
        assert_eq!(bgra_to_rgba(&bgra), [30, 20, 10, 40, 70, 60, 50, 80]);
        assert_eq!(bgra_to_rgba(&bgra_to_rgba(&bgra)), bgra);
        let fetch = FetchConstant::parse([0x8000_0002, 0x0000_0086, 0x0000_0001, 0xc14, 0, 0x200]);
        assert_eq!((fetch.width, fetch.height), (2, 1));
        assert_eq!(rgba8(&fetch, &bgra).unwrap(), bgra_to_rgba(&bgra));
        // Another swizzle (A, B, G, R in memory, say) is not handled.
        let mut other = fetch;
        other.swizzle = [3, 2, 1, 0];
        assert!(!other.has_expected_swizzle() && !hd_replaceable(&other));
    }

    #[test]
    fn mipmapped_dds_header() {
        let levels = vec![
            vec![0u8; 16 * 8],
            vec![0u8; 16 * 2],
            vec![0u8; 16],
            vec![0u8; 16],
        ];
        let dds = dds_file_mipmapped(Format::Dxt4_5, 16, 8, &levels).unwrap();
        let word = |i: usize| u32::from_le_bytes(dds[i..i + 4].try_into().unwrap());
        assert_eq!(dds.len(), DDS_HEADER_BYTES + 16 * 12);
        assert_eq!(word(8) & 0x2_0000, 0x2_0000, "DDSD_MIPMAPCOUNT");
        assert_eq!((word(12), word(16), word(20), word(28)), (8, 16, 128, 4));
        assert_eq!(&dds[84..88], b"DXT5");
        assert_eq!(
            word(108),
            0x1000 | 0x40_0000 | 0x8,
            "texture, mipmap, complex"
        );
        assert_eq!(dds_info(&dds), Some((Format::Dxt4_5, 16, 8)));

        // The one-level file keeps its old header: no mip count or flags.
        let fetch = FetchConstant::parse([0x8000_0002, 0x0000_0046, 0x0000_e00f, 0, 0, 0x200]);
        assert_eq!(fetch.format, Format::A8R8G8B8);
        let one = dds_file(&fetch, &[0u8; 16 * 8 * 4]).unwrap();
        let word = |i: usize| u32::from_le_bytes(one[i..i + 4].try_into().unwrap());
        assert_eq!((word(8), word(28), word(108)), (0x100F, 0, 0x1000));
        assert_eq!(dds_info(&one), Some((Format::A8R8G8B8, 16, 8)));
    }

    #[test]
    fn hd_replaceable_textures() {
        // 2D DXT1 256 x 256, then with packed mips, then 16 x 16 with packed
        // mips (its base level in the packed tile, read from there).
        let mut words = [0x8000_0002, 0x0000_0052, 0x001f_e0ff, 0xd10, 0, 0x200];
        assert!(hd_replaceable(&FetchConstant::parse(words)));
        words[5] |= 1 << 11;
        assert!(hd_replaceable(&FetchConstant::parse(words)));
        words[2] = 0x0001_e00f;
        assert!(hd_replaceable(&FetchConstant::parse(words)));
        // A cube map (dimension 3), and a swizzle the files don't assume.
        let cube = [0x8000_0002, 0x0000_0052, 0x001f_e0ff, 0xd10, 0, 0x600];
        assert!(!hd_replaceable(&FetchConstant::parse(cube)));
        let swizzled = [0x8000_0002, 0x0000_0052, 0x001f_e0ff, 0xc14, 0, 0x200];
        assert!(!hd_replaceable(&FetchConstant::parse(swizzled)));
        assert_eq!(Format::from_xenos(49).to_xenos(), 49);
    }

    #[test]
    fn tiled_offsets_are_a_permutation() {
        // Every block of a 128 x 128 block surface lands on a distinct index.
        let mut seen = vec![false; 128 * 128];
        for y in 0..128 {
            for x in 0..128 {
                let i = tiled_offset_2d(x, y, 128, 16) as usize;
                assert!(!seen[i], "collision at {x},{y}");
                seen[i] = true;
            }
        }
    }
}
