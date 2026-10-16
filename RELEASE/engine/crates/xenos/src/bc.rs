//! Block-compressed texture decoding (DXT1/3/5 = BC1/2/3, DXN = BC5) to
//! RGBA8, so textures can be written as PNG for glTF viewers and Unreal.
//!
//! Follows the block layouts in Microsoft's "Texture Block Compression in
//! Direct3D 11" documentation (see docs/SOURCES.md). The input is the linear,
//! already byte-swapped data from `base_level_linear`, which is in the
//! standard little-endian block layout.

/// Pixels per block side.
const BLOCK: usize = 4;

/// A 5-bit channel to 8 bits, replicating the top bits into the low ones.
pub(crate) fn expand5(v: u16) -> u8 {
    let v = (v & 31) as u8;
    (v << 3) | (v >> 2)
}

/// A 6-bit channel to 8 bits, replicating the top bits into the low ones.
pub(crate) fn expand6(v: u16) -> u8 {
    let v = (v & 63) as u8;
    (v << 2) | (v >> 4)
}

/// RGB565 to RGB888.
fn rgb565(c: u16) -> [u8; 3] {
    [expand5(c >> 11), expand6(c >> 5), expand5(c)]
}

/// `a` moved `num / den` of the way to `b`, rounded to nearest.
pub(crate) fn lerp(a: u8, b: u8, num: u32, den: u32) -> u8 {
    ((a as u32 * (den - num) + b as u32 * num + den / 2) / den) as u8
}

/// The four palette entries of a BC1 color block with endpoints `c0`, `c1`:
/// with `four_color`, c0, c1 and two colors between them; otherwise c0, c1,
/// their average and transparent black. Shared with the encoder, so it
/// minimizes exactly the error this decoder shows.
pub(crate) fn color_palette(c0: u16, c1: u16, four_color: bool) -> [[u8; 4]; 4] {
    let (p0, p1) = (rgb565(c0), rgb565(c1));
    let mix = |num, den| {
        [
            lerp(p0[0], p1[0], num, den),
            lerp(p0[1], p1[1], num, den),
            lerp(p0[2], p1[2], num, den),
            255,
        ]
    };
    if four_color {
        [
            [p0[0], p0[1], p0[2], 255],
            [p1[0], p1[1], p1[2], 255],
            mix(1, 3),
            mix(2, 3),
        ]
    } else {
        [
            [p0[0], p0[1], p0[2], 255],
            [p1[0], p1[1], p1[2], 255],
            mix(1, 2),
            [0, 0, 0, 0],
        ]
    }
}

/// The 16 RGBA pixels of a BC1 color block (8 bytes). With `force_four`, the
/// three-color-plus-transparent mode is never used (BC2/BC3 color blocks).
fn color_block(b: &[u8], force_four: bool) -> [[u8; 4]; 16] {
    let c0 = u16::from_le_bytes([b[0], b[1]]);
    let c1 = u16::from_le_bytes([b[2], b[3]]);
    let palette = color_palette(c0, c1, c0 > c1 || force_four);
    let bits = u32::from_le_bytes([b[4], b[5], b[6], b[7]]);
    std::array::from_fn(|i| palette[((bits >> (2 * i)) & 3) as usize])
}

/// The eight palette entries of a BC4-style block: with `a0 > a1`, the two
/// endpoints and six values between them; otherwise the endpoints, four
/// values between them, 0 and 255. Shared with the encoder.
pub(crate) fn interpolated_palette(a0: u8, a1: u8) -> [u8; 8] {
    if a0 > a1 {
        std::array::from_fn(|i| match i {
            0 => a0,
            1 => a1,
            k => lerp(a0, a1, k as u32 - 1, 7),
        })
    } else {
        std::array::from_fn(|i| match i {
            0 => a0,
            1 => a1,
            6 => 0,
            7 => 255,
            k => lerp(a0, a1, k as u32 - 1, 5),
        })
    }
}

/// The 16 values of a BC4-style interpolated block (8 bytes): two endpoints
/// and 3-bit indices. Used for BC3 alpha and both BC5 channels.
fn interpolated_block(b: &[u8]) -> [u8; 16] {
    let palette = interpolated_palette(b[0], b[1]);
    let mut bits = 0u64;
    for (k, &byte) in b[2..8].iter().enumerate() {
        bits |= (byte as u64) << (8 * k);
    }
    std::array::from_fn(|i| palette[((bits >> (3 * i)) & 7) as usize])
}

/// BC2 explicit alpha: 4 bits per pixel, scaled to 8 bits.
fn explicit_alpha(b: &[u8]) -> [u8; 16] {
    std::array::from_fn(|i| {
        let nibble = (b[i / 2] >> (4 * (i % 2))) & 15;
        nibble * 17
    })
}

/// Which block format to decode (or encode, see `bc_encode`).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum BlockFormat {
    Bc1,
    Bc2,
    Bc3,
    /// Two-channel normal map; blue is rebuilt as sqrt(1 - x² - y²).
    Bc5,
}

impl BlockFormat {
    pub fn block_bytes(self) -> usize {
        match self {
            BlockFormat::Bc1 => 8,
            _ => 16,
        }
    }
}

/// Decodes `width` x `height` pixels of linear block data to RGBA8.
/// Returns `None` if `data` is too short.
pub fn decode(format: BlockFormat, width: usize, height: usize, data: &[u8]) -> Option<Vec<u8>> {
    let blocks_wide = width.div_ceil(BLOCK);
    let blocks_high = height.div_ceil(BLOCK);
    let block_bytes = format.block_bytes();
    if data.len() < blocks_wide * blocks_high * block_bytes {
        return None;
    }
    let mut rgba = vec![0u8; width * height * 4];
    for by in 0..blocks_high {
        for bx in 0..blocks_wide {
            let at = (by * blocks_wide + bx) * block_bytes;
            let b = &data[at..at + block_bytes];
            let pixels: [[u8; 4]; 16] = match format {
                BlockFormat::Bc1 => color_block(b, false),
                BlockFormat::Bc2 => {
                    let alpha = explicit_alpha(&b[..8]);
                    let mut px = color_block(&b[8..], true);
                    px.iter_mut().zip(alpha).for_each(|(p, a)| p[3] = a);
                    px
                }
                BlockFormat::Bc3 => {
                    let alpha = interpolated_block(&b[..8]);
                    let mut px = color_block(&b[8..], true);
                    px.iter_mut().zip(alpha).for_each(|(p, a)| p[3] = a);
                    px
                }
                BlockFormat::Bc5 => {
                    let red = interpolated_block(&b[..8]);
                    let green = interpolated_block(&b[8..]);
                    std::array::from_fn(|i| {
                        let x = red[i] as f32 / 127.5 - 1.0;
                        let y = green[i] as f32 / 127.5 - 1.0;
                        let z = (1.0 - x * x - y * y).max(0.0).sqrt();
                        [red[i], green[i], ((z + 1.0) * 127.5) as u8, 255]
                    })
                }
            };
            for (i, p) in pixels.iter().enumerate() {
                let (x, y) = (bx * BLOCK + i % BLOCK, by * BLOCK + i / BLOCK);
                if x < width && y < height {
                    let o = (y * width + x) * 4;
                    rgba[o..o + 4].copy_from_slice(p);
                }
            }
        }
    }
    Some(rgba)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn bc1_solid_red() {
        // c0 = pure red (0xF800), c1 = black, every index 0.
        let block = [0x00, 0xF8, 0x00, 0x00, 0, 0, 0, 0];
        let rgba = decode(BlockFormat::Bc1, 4, 4, &block).unwrap();
        assert_eq!(&rgba[..4], &[255, 0, 0, 255]);
        assert_eq!(&rgba[60..64], &[255, 0, 0, 255]);
    }

    #[test]
    fn bc1_transparent_mode() {
        // c0 <= c1 selects the three-color mode; index 3 is transparent black.
        let block = [0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF];
        let rgba = decode(BlockFormat::Bc1, 4, 4, &block).unwrap();
        assert_eq!(&rgba[..4], &[0, 0, 0, 0]);
    }

    #[test]
    fn bc3_alpha_endpoints() {
        // Alpha 255 / 0 with every index 1 (= a1 = 0); color white.
        let mut block = [0u8; 16];
        block[0] = 255;
        block[1] = 0;
        // 3-bit indices all 1: bit pattern 001 repeated.
        let mut bits = 0u64;
        for i in 0..16 {
            bits |= 1 << (3 * i);
        }
        block[2..8].copy_from_slice(&bits.to_le_bytes()[..6]);
        block[8..10].copy_from_slice(&0xFFFFu16.to_le_bytes());
        let rgba = decode(BlockFormat::Bc3, 4, 4, &block).unwrap();
        assert_eq!(&rgba[..4], &[255, 255, 255, 0]);
    }
}
