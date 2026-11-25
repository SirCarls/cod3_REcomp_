//! The images of a World at War Xbox 360 zone, found by their own records
//! (the Xbox 360 zone isn't read asset by asset yet; see `lib.rs`).
//!
//! Found by us in the retail disc's zones (2026-11-24, `code_post_gfx`,
//! `ui`, `ber1`, `mp_seelow`; docs/formats/waw.md has the method). The
//! reference tools read Xbox 360 zones only as a raw dump, so nothing here
//! comes from them.
//!
//! An image is a 40-byte record, big-endian like the rest of the zone:
//!
//! ```text
//!  0 u32 map type        3 = 2D, 4 = 3D, 5 = cube
//!  4 ptr texture         always -2: the D3D texture follows (with a slot)
//!  8 u8  semantic        2 colour, 5 normal, 8 specular, ...; 3 bytes padding
//! 12 u32 card memory     bytes of pixel data, mips included (a multiple of 4 KB)
//! 16 u16 width, height, depth
//! 22 u8  category
//! 23 u8  pixels at end   1: the pixels are in the zone's last block (see below)
//! 24 ptr pixels          -1
//! 28 u32 (unknown)
//! 32 u16 stream slot, u8 streamed (1: highmip/<name>.hi holds a bigger top level), u8 0
//! 36 ptr name            -1: the name follows; anything else refers to a string read before
//! ```
//!
//! Then come, in this order: the name (when -1), the pixels (when they are
//! not at the end), a 16-byte load record (u8 levels, u8 flags, u16 width,
//! height, depth, u32 D3D format, u32 -1) and the 52-byte D3D texture
//! header. The header is stored little-endian; its last six words are the
//! GPU's texture fetch constant, which the `xenos` crate reads.
//!
//! **Pixels at the end.** Most textures keep their pixels in block 2
//! (LARGE_RUNTIME), which the loader reads after everything else: the last
//! `blockSize[2]` bytes of the stream hold every such image's pixels, one
//! after another in the order of their records. The sizes adding up to the
//! block exactly is the check that every record was found; when they don't,
//! no image is given pixels from that block.

use crate::{read_u32, Error, Platform, Zone, PTR_FOLLOWING, PTR_INSERT};
use std::ops::Range;

const RECORD: usize = 40;
const LOAD_RECORD: usize = 16;
const D3D_HEADER: usize = 52;
/// The stream block whose data comes last (LARGE_RUNTIME).
const END_BLOCK: usize = 2;
/// The block after it (PHYSICAL_RUNTIME): never used by the disc's zones,
/// and where it would go is unknown, so a zone using it is refused.
const AFTER_END_BLOCK: usize = 3;
/// The first words of every D3D texture header (common = texture, one reference).
const D3D_MAGIC: [u8; 8] = [3, 0, 0, 0, 1, 0, 0, 0];
const MAX_NAME: usize = 256;

#[derive(Debug, Clone)]
pub struct XboxImage {
    /// Where the record starts in the zone.
    pub record_at: usize,
    /// None when the record refers to a name read earlier.
    pub name: Option<String>,
    pub map_type: u32,
    pub semantic: u8,
    pub category: u8,
    pub card_memory: u32,
    pub width: u16,
    pub height: u16,
    pub depth: u16,
    pub levels: u8,
    /// The D3D format of the load record (0x1A200152 DXT1, ...).
    pub d3d_format: u32,
    /// The fetch constant, for `xenos::FetchConstant::parse`.
    pub fetch: [u32; 6],
    /// `highmip/<name>.hi` holds this texture's top level at twice the size.
    pub streamed: bool,
    pub pixels_at_end: bool,
    /// The pixel data in `Zone::data`; None for end-block pixels when the
    /// block's sizes don't add up.
    pub pixels: Option<Range<usize>>,
}

#[derive(Debug, Default)]
pub struct XboxImages {
    pub images: Vec<XboxImage>,
    /// Why some pixels were left out (empty when everything adds up).
    pub problems: Vec<String>,
}

fn be32(d: &[u8], at: usize) -> u32 {
    read_u32(d[at..at + 4].try_into().unwrap(), true)
}

fn be16(d: &[u8], at: usize) -> u16 {
    u16::from_be_bytes([d[at], d[at + 1]])
}

/// The record at `i`, if one starts there: the image and where its trailer ends.
fn record_at(d: &[u8], i: usize) -> Option<(XboxImage, usize)> {
    if i + RECORD > d.len() {
        return None;
    }
    let map_type = be32(d, i);
    let pixels_at_end = d[i + 23];
    if !(1..=5).contains(&map_type)
        || be32(d, i + 4) != PTR_INSERT
        || d[i + 9..i + 12] != [0, 0, 0]
        || pixels_at_end > 1
        || be32(d, i + 24) != PTR_FOLLOWING
    {
        return None;
    }
    let card_memory = be32(d, i + 12);
    let (width, height, depth) = (be16(d, i + 16), be16(d, i + 18), be16(d, i + 20));
    if card_memory == 0 || width == 0 || height == 0 || depth == 0 {
        return None;
    }
    let mut p = i + RECORD;
    let name = if be32(d, i + 36) == PTR_FOLLOWING {
        let rest = &d[p..d.len().min(p + MAX_NAME)];
        let n = rest.iter().position(|&b| b == 0)?;
        if n == 0 || !rest[..n].iter().all(|b| (0x20..0x7F).contains(b)) {
            return None;
        }
        p += n + 1;
        Some(String::from_utf8_lossy(&rest[..n]).into_owned())
    } else {
        None
    };
    let pixels = if pixels_at_end == 0 {
        let range = p..p.checked_add(card_memory as usize)?;
        p = range.end;
        Some(range)
    } else {
        None
    };
    // The load record and the D3D header must follow, and agree on the size.
    if p + LOAD_RECORD + D3D_HEADER > d.len()
        || be16(d, p + 2) != width
        || be16(d, p + 4) != height
        || be16(d, p + 6) != depth
        || be32(d, p + 12) != PTR_FOLLOWING
        || d[p + LOAD_RECORD..p + LOAD_RECORD + 8] != D3D_MAGIC
    {
        return None;
    }
    let header = p + LOAD_RECORD;
    let fetch = std::array::from_fn(|k| {
        let at = header + 28 + 4 * k;
        read_u32(d[at..at + 4].try_into().unwrap(), false)
    });
    let image = XboxImage {
        record_at: i,
        name,
        map_type,
        semantic: d[i + 8],
        category: d[i + 22],
        card_memory,
        width,
        height,
        depth,
        levels: d[p],
        d3d_format: be32(d, p + 8),
        fetch,
        streamed: d[i + 34] == 1,
        pixels_at_end: pixels_at_end == 1,
        pixels,
    };
    Some((image, header + D3D_HEADER))
}

impl Zone {
    /// Every image record of an Xbox 360 zone, with where its pixels are.
    pub fn xbox_images(&self) -> Result<XboxImages, Error> {
        if self.platform != Platform::Xbox360 {
            return Err(Error::NotXbox360);
        }
        let d = &self.data;
        let mut out = XboxImages::default();
        let mut i = self.assets_offset;
        while i + RECORD <= d.len() {
            // Records sit at their block's alignment, not the stream's, so
            // every byte is a candidate; the -2 texture pointer is a cheap
            // first test.
            if d[i + 4] != 0xFF {
                i += 1;
                continue;
            }
            match record_at(d, i) {
                Some((image, next)) => {
                    out.images.push(image);
                    i = next;
                }
                None => i += 1,
            }
        }
        self.place_end_block(&mut out);
        Ok(out)
    }

    /// Gives the end-block images their pixels, in record order, if the
    /// sizes add up to the block exactly.
    fn place_end_block(&self, out: &mut XboxImages) {
        let block = self.header.block_size[END_BLOCK] as usize;
        let wanted: usize = out
            .images
            .iter()
            .filter(|im| im.pixels_at_end)
            .map(|im| im.card_memory as usize)
            .sum();
        if wanted == 0 && block == 0 {
            return;
        }
        if self.header.block_size[AFTER_END_BLOCK] != 0 {
            out.problems.push(format!(
                "block {AFTER_END_BLOCK} is used ({} bytes): where it is in the stream is unknown, so the \
                 end-block pixels are left out",
                self.header.block_size[AFTER_END_BLOCK]
            ));
            return;
        }
        if wanted != block || block > self.data.len() {
            out.problems.push(format!(
                "the end-block images need {wanted} bytes and the block holds {block}: an image record \
                 wasn't found, so their pixels are left out"
            ));
            return;
        }
        let mut at = self.data.len() - block;
        for im in out.images.iter_mut().filter(|im| im.pixels_at_end) {
            let end = at + im.card_memory as usize;
            im.pixels = Some(at..end);
            at = end;
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    /// One image: record, name, [pixels], load record, D3D header.
    fn image(name: &str, at_end: bool, card_memory: u32, w: u16, h: u16) -> Vec<u8> {
        let mut v = Vec::new();
        v.extend_from_slice(&3u32.to_be_bytes());
        v.extend_from_slice(&PTR_INSERT.to_be_bytes());
        v.extend_from_slice(&[2, 0, 0, 0]);
        v.extend_from_slice(&card_memory.to_be_bytes());
        for x in [w, h, 1] {
            v.extend_from_slice(&x.to_be_bytes());
        }
        v.extend_from_slice(&[3, at_end as u8]);
        v.extend_from_slice(&PTR_FOLLOWING.to_be_bytes());
        v.extend_from_slice(&0x100u32.to_be_bytes());
        v.extend_from_slice(&[0xFF, 0xFF, 1, 0]);
        v.extend_from_slice(&PTR_FOLLOWING.to_be_bytes());
        v.extend_from_slice(name.as_bytes());
        v.push(0);
        if !at_end {
            v.extend(vec![0xABu8; card_memory as usize]);
        }
        v.extend_from_slice(&[1, 0x10]);
        for x in [w, h, 1] {
            v.extend_from_slice(&x.to_be_bytes());
        }
        v.extend_from_slice(&0x1A20_0152u32.to_be_bytes());
        v.extend_from_slice(&PTR_FOLLOWING.to_be_bytes());
        v.extend_from_slice(&D3D_MAGIC);
        v.extend([0u8; 20]);
        // fetch constant, little-endian: 2D, DXT1, w x h
        let words = [
            0x8000_0002u32,
            0x52,
            (w as u32 - 1) | ((h as u32 - 1) << 13),
            0,
            0,
            1 << 9,
        ];
        for word in words {
            v.extend_from_slice(&word.to_le_bytes());
        }
        v
    }

    fn zone(end_block: u32, body: &[u8], tail: &[u8]) -> Zone {
        let mut z = Vec::new();
        for v in [0u32, 0, 0, 0, end_block, 0, 0, 0, 0] {
            z.extend_from_slice(&v.to_be_bytes());
        }
        z.extend_from_slice(&[0; 16]); // no script strings, no assets
        z.extend_from_slice(&[0x55; 7]); // something before, unaligned
        z.extend_from_slice(body);
        z.extend_from_slice(tail);
        Zone::from_zone(z, Platform::Xbox360).unwrap()
    }

    #[test]
    fn finds_inline_and_end_block_images() {
        let mut body = image("inline_c", false, 0x1000, 8, 8);
        body.extend_from_slice(&[0x11; 5]);
        body.extend(image("~big_s-rgb~0123", true, 0x2000, 64, 32));
        body.extend(image("second_n", true, 0x1000, 16, 16));
        let mut tail = vec![1u8; 0x2000];
        tail.extend(vec![2u8; 0x1000]);
        let z = zone(0x3000, &body, &tail);
        let found = z.xbox_images().unwrap();
        assert!(found.problems.is_empty(), "{:?}", found.problems);
        let names: Vec<_> = found
            .images
            .iter()
            .map(|i| i.name.clone().unwrap())
            .collect();
        assert_eq!(names, ["inline_c", "~big_s-rgb~0123", "second_n"]);
        let first = &found.images[0];
        assert!(z.data[first.pixels.clone().unwrap()]
            .iter()
            .all(|&b| b == 0xAB));
        let big = found.images[1].pixels.clone().unwrap();
        assert_eq!(big.len(), 0x2000);
        assert!(z.data[big].iter().all(|&b| b == 1));
        assert!(z.data[found.images[2].pixels.clone().unwrap()]
            .iter()
            .all(|&b| b == 2));
        assert_eq!(found.images[1].fetch[2], 63 | (31 << 13));
        assert_eq!((found.images[1].width, found.images[1].height), (64, 32));
        assert!(found.images[1].streamed);
    }

    #[test]
    fn sizes_that_dont_add_up_leave_the_end_block_alone() {
        let body = image("a_c", true, 0x1000, 8, 8);
        let z = zone(0x2000, &body, &[0; 0x2000]);
        let found = z.xbox_images().unwrap();
        assert_eq!(found.images.len(), 1);
        assert!(found.images[0].pixels.is_none());
        assert_eq!(found.problems.len(), 1);
    }

    #[test]
    fn a_name_read_before_is_a_reference() {
        let mut body = image("x", false, 0x1000, 4, 4);
        // Point the name at something read before: no inline name follows.
        body[36..40].copy_from_slice(&0x8362_0B8Du32.to_be_bytes());
        body.drain(40..42);
        let z = zone(0, &body, &[]);
        let found = z.xbox_images().unwrap();
        assert_eq!(found.images.len(), 1);
        assert_eq!(found.images[0].name, None);
    }

    #[test]
    fn pc_zones_are_refused() {
        let mut z = vec![0u8; 52];
        z.extend_from_slice(&image("a", false, 0x1000, 4, 4));
        let pc = Zone::from_zone(z, Platform::Pc).unwrap();
        assert!(matches!(pc.xbox_images(), Err(Error::NotXbox360)));
    }
}
