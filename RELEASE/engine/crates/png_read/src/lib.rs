//! Reads PNG images with 8 bits per channel (grayscale, RGB, gray + alpha or
//! RGBA; not interlaced) as RGBA8, which is what upscalers such as waifu2x
//! write. Other PNGs are refused with `Error::Unsupported`.
//!
//! Follows the PNG specification (W3C, Portable Network Graphics, see
//! docs/SOURCES.md): signature, IHDR, the IDAT chunks joined into one zlib
//! stream, scanline filters 0-4. zlib is decoded by the workspace's own
//! `inflate` crate, whose Adler-32 check covers the pixel data, so chunk
//! CRCs are not checked. Ancillary chunks (gamma, color profiles, `tRNS`
//! color keys) are ignored.

#![forbid(unsafe_code)]

use std::fmt;

const SIGNATURE: [u8; 8] = [0x89, b'P', b'N', b'G', b'\r', b'\n', 0x1A, b'\n'];
/// Signature, then the IHDR chunk's length, type and 13 data bytes.
const HEADER_BYTES: usize = 8 + 8 + 13;
/// Largest width or height accepted: twice the Xbox 360's 8192-texel limit.
/// It also stops a damaged header from asking for gigabytes.
pub const MAX_SIDE: u32 = 16384;

const COLOR_GRAY: u8 = 0;
const COLOR_RGB: u8 = 2;
const COLOR_GRAY_ALPHA: u8 = 4;
const COLOR_RGBA: u8 = 6;

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Error {
    NotPng,
    Truncated,
    /// A chunk that must come first, or be present, is missing.
    MissingChunk(&'static str),
    BadSize {
        width: u32,
        height: u32,
    },
    Unsupported {
        bit_depth: u8,
        color_type: u8,
    },
    Interlaced,
    Zlib(inflate::Error),
    BadFilter(u8),
    /// The decompressed data is shorter than the image needs.
    ShortData {
        need: usize,
        have: usize,
    },
}

impl fmt::Display for Error {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Error::NotPng => write!(f, "not a PNG file"),
            Error::Truncated => write!(f, "PNG file is truncated"),
            Error::MissingChunk(kind) => write!(f, "PNG has no {kind} chunk where expected"),
            Error::BadSize { width, height } => {
                write!(f, "PNG size {width} x {height} is outside 1-{MAX_SIDE}")
            }
            Error::Unsupported {
                bit_depth,
                color_type,
            } => write!(
                f,
                "PNG with {bit_depth}-bit samples and color type {color_type} is not supported \
                 (8-bit gray, RGB, gray + alpha and RGBA are)"
            ),
            Error::Interlaced => write!(f, "interlaced PNG is not supported"),
            Error::Zlib(e) => write!(f, "PNG image data: zlib error {e}"),
            Error::BadFilter(t) => write!(f, "PNG scanline filter {t} is not 0-4"),
            Error::ShortData { need, have } => {
                write!(f, "PNG image data is {have} bytes, needs {need}")
            }
        }
    }
}

impl std::error::Error for Error {}

/// A decoded image.
#[derive(Debug, Clone)]
pub struct Image {
    pub width: u32,
    pub height: u32,
    /// `width * height * 4` bytes, rows top to bottom.
    pub rgba: Vec<u8>,
    /// True if the file has an alpha channel (else alpha is 255 everywhere).
    pub has_alpha: bool,
}

/// The fields of IHDR this reader uses.
struct Header {
    width: u32,
    height: u32,
    color_type: u8,
}

fn be_u32(bytes: &[u8]) -> u32 {
    u32::from_be_bytes([bytes[0], bytes[1], bytes[2], bytes[3]])
}

fn parse_header(bytes: &[u8]) -> Result<Header, Error> {
    if bytes.len() < SIGNATURE.len() || bytes[..8] != SIGNATURE {
        return Err(Error::NotPng);
    }
    if bytes.len() < HEADER_BYTES {
        return Err(Error::Truncated);
    }
    if &bytes[12..16] != b"IHDR" || be_u32(&bytes[8..12]) != 13 {
        return Err(Error::MissingChunk("IHDR"));
    }
    let ihdr = &bytes[16..29];
    let (width, height) = (be_u32(&ihdr[0..4]), be_u32(&ihdr[4..8]));
    let (bit_depth, color_type, interlace) = (ihdr[8], ihdr[9], ihdr[12]);
    if !(1..=MAX_SIDE).contains(&width) || !(1..=MAX_SIDE).contains(&height) {
        return Err(Error::BadSize { width, height });
    }
    let known_color = [COLOR_GRAY, COLOR_RGB, COLOR_GRAY_ALPHA, COLOR_RGBA].contains(&color_type);
    if bit_depth != 8 || !known_color {
        return Err(Error::Unsupported {
            bit_depth,
            color_type,
        });
    }
    if interlace != 0 {
        return Err(Error::Interlaced);
    }
    Ok(Header {
        width,
        height,
        color_type,
    })
}

/// The width and height from the start of a PNG file (the first 29 bytes
/// are enough), without decoding it. Fails like `decode` for PNGs it can't
/// read.
pub fn dimensions(bytes: &[u8]) -> Result<(u32, u32), Error> {
    parse_header(bytes).map(|h| (h.width, h.height))
}

/// The Paeth predictor (PNG specification, section 9.4).
fn paeth(left: u8, above: u8, upper_left: u8) -> u8 {
    let p = left as i16 + above as i16 - upper_left as i16;
    let (pa, pb, pc) = (
        (p - left as i16).abs(),
        (p - above as i16).abs(),
        (p - upper_left as i16).abs(),
    );
    if pa <= pb && pa <= pc {
        left
    } else if pb <= pc {
        above
    } else {
        upper_left
    }
}

/// Undoes the scanline filters: `data` holds `height` lines of a filter
/// byte plus `row_bytes` bytes; returns the plain rows joined. `bpp` is the
/// bytes per pixel, the distance to the "left" byte.
fn unfilter(data: &[u8], row_bytes: usize, height: usize, bpp: usize) -> Result<Vec<u8>, Error> {
    let mut out = vec![0u8; row_bytes * height];
    // The row above the first one counts as zeros.
    let zeros = vec![0u8; row_bytes];
    for y in 0..height {
        let line = &data[y * (row_bytes + 1)..(y + 1) * (row_bytes + 1)];
        let (filter, src) = (line[0], &line[1..]);
        let (before, rest) = out.split_at_mut(y * row_bytes);
        let row = &mut rest[..row_bytes];
        let above = if y == 0 {
            &zeros[..]
        } else {
            &before[(y - 1) * row_bytes..]
        };
        // The first pixel has no left neighbor (zero); every filter is
        // written as "first pixel, then the rest" so the loops need no
        // per-byte checks.
        let first = bpp.min(row_bytes);
        match filter {
            0 => row.copy_from_slice(src),
            1 => {
                row[..first].copy_from_slice(&src[..first]);
                for x in first..row_bytes {
                    row[x] = src[x].wrapping_add(row[x - bpp]);
                }
            }
            2 => {
                for ((r, &s), &a) in row.iter_mut().zip(src).zip(above) {
                    *r = s.wrapping_add(a);
                }
            }
            3 => {
                for x in 0..first {
                    row[x] = src[x].wrapping_add(above[x] / 2);
                }
                for x in first..row_bytes {
                    let average = (row[x - bpp] as u16 + above[x] as u16) / 2;
                    row[x] = src[x].wrapping_add(average as u8);
                }
            }
            4 => {
                for x in 0..first {
                    // Left and upper-left are zero, so Paeth picks above.
                    row[x] = src[x].wrapping_add(above[x]);
                }
                for x in first..row_bytes {
                    let predicted = paeth(row[x - bpp], above[x], above[x - bpp]);
                    row[x] = src[x].wrapping_add(predicted);
                }
            }
            other => return Err(Error::BadFilter(other)),
        }
    }
    Ok(out)
}

/// Decodes a whole PNG file.
pub fn decode(bytes: &[u8]) -> Result<Image, Error> {
    let header = parse_header(bytes)?;
    let channels = match header.color_type {
        COLOR_GRAY => 1,
        COLOR_GRAY_ALPHA => 2,
        COLOR_RGB => 3,
        _ => 4,
    };
    let (width, height) = (header.width as usize, header.height as usize);
    let row_bytes = width * channels;

    // Join the IDAT chunks; stop at IEND.
    let mut zlib = Vec::new();
    let mut at = SIGNATURE.len();
    loop {
        let chunk_header = bytes.get(at..at + 8).ok_or(Error::Truncated)?;
        let length = be_u32(chunk_header) as usize;
        let kind = &chunk_header[4..8];
        let data = bytes.get(at + 8..at + 8 + length).ok_or(Error::Truncated)?;
        match kind {
            b"IDAT" => zlib.extend_from_slice(data),
            b"IEND" => break,
            _ => {}
        }
        at += 8 + length + 4; // length, type, data, CRC
    }
    if zlib.is_empty() {
        return Err(Error::MissingChunk("IDAT"));
    }
    let need = (row_bytes + 1) * height;
    let raw = inflate::inflate_zlib(&zlib, need).map_err(Error::Zlib)?;
    if raw.len() < need {
        return Err(Error::ShortData {
            need,
            have: raw.len(),
        });
    }
    let pixels = unfilter(&raw, row_bytes, height, channels)?;
    let rgba = if channels == 4 {
        pixels
    } else {
        let mut rgba = vec![0u8; width * height * 4];
        for (out, p) in rgba.chunks_exact_mut(4).zip(pixels.chunks_exact(channels)) {
            out.copy_from_slice(&match channels {
                3 => [p[0], p[1], p[2], 255],
                2 => [p[0], p[0], p[0], p[1]],
                _ => [p[0], p[0], p[0], 255],
            });
        }
        rgba
    };
    Ok(Image {
        width: header.width,
        height: header.height,
        rgba,
        has_alpha: channels == 2 || channels == 4,
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    /// A zlib stream of stored (uncompressed) blocks, so the tests can build
    /// PNGs with any filter without a compressor.
    fn zlib_stored(data: &[u8]) -> Vec<u8> {
        let mut out = vec![0x78, 0x01];
        let chunks: Vec<&[u8]> = data.chunks(65535).collect();
        for (i, chunk) in chunks.iter().enumerate() {
            out.push((i + 1 == chunks.len()) as u8); // BFINAL, BTYPE 00
            let len = chunk.len() as u16;
            out.extend_from_slice(&len.to_le_bytes());
            out.extend_from_slice(&(!len).to_le_bytes());
            out.extend_from_slice(chunk);
        }
        let (mut a, mut b) = (1u32, 0u32);
        for &x in data {
            a = (a + x as u32) % 65521;
            b = (b + a) % 65521;
        }
        out.extend_from_slice(&((b << 16) | a).to_be_bytes());
        out
    }

    fn chunk(out: &mut Vec<u8>, kind: &[u8; 4], data: &[u8]) {
        out.extend_from_slice(&(data.len() as u32).to_be_bytes());
        out.extend_from_slice(kind);
        out.extend_from_slice(data);
        out.extend_from_slice(&[0; 4]); // CRC, not checked
    }

    /// A PNG of `pixels` (`channels` per pixel) with row `y` filtered by
    /// filter `y % 5`, the IDAT split in two.
    fn png_with_every_filter(w: usize, h: usize, channels: usize, pixels: &[u8]) -> Vec<u8> {
        let row_bytes = w * channels;
        let mut raw = Vec::new();
        for y in 0..h {
            let row = &pixels[y * row_bytes..(y + 1) * row_bytes];
            let above = |x: usize| {
                if y == 0 {
                    0
                } else {
                    pixels[(y - 1) * row_bytes + x]
                }
            };
            let left = |x: usize| if x >= channels { row[x - channels] } else { 0 };
            let upper_left = |x: usize| {
                if x >= channels {
                    above(x - channels)
                } else {
                    0
                }
            };
            let filter = (y % 5) as u8;
            raw.push(filter);
            for (x, &value) in row.iter().enumerate() {
                let predicted = match filter {
                    0 => 0,
                    1 => left(x),
                    2 => above(x),
                    3 => ((left(x) as u16 + above(x) as u16) / 2) as u8,
                    _ => paeth(left(x), above(x), upper_left(x)),
                };
                raw.push(value.wrapping_sub(predicted));
            }
        }
        let color_type = match channels {
            1 => COLOR_GRAY,
            2 => COLOR_GRAY_ALPHA,
            3 => COLOR_RGB,
            _ => COLOR_RGBA,
        };
        let mut ihdr = Vec::new();
        ihdr.extend_from_slice(&(w as u32).to_be_bytes());
        ihdr.extend_from_slice(&(h as u32).to_be_bytes());
        ihdr.extend_from_slice(&[8, color_type, 0, 0, 0]);
        let zlib = zlib_stored(&raw);
        let (first, second) = zlib.split_at(zlib.len() / 2);
        let mut png = SIGNATURE.to_vec();
        chunk(&mut png, b"IHDR", &ihdr);
        chunk(&mut png, b"gAMA", &[0, 0, 0xB1, 0x8F]);
        chunk(&mut png, b"IDAT", first);
        chunk(&mut png, b"IDAT", second);
        chunk(&mut png, b"IEND", &[]);
        png
    }

    fn test_pixels(count: usize) -> Vec<u8> {
        (0..count).map(|i| ((i * 37) ^ (i >> 3)) as u8).collect()
    }

    #[test]
    fn every_filter_and_color_type() {
        let (w, h) = (7, 11);
        for channels in 1..=4 {
            let pixels = test_pixels(w * h * channels);
            let image = decode(&png_with_every_filter(w, h, channels, &pixels)).unwrap();
            assert_eq!((image.width, image.height), (w as u32, h as u32));
            assert_eq!(image.has_alpha, channels % 2 == 0);
            for (i, p) in image.rgba.chunks_exact(4).enumerate() {
                let src = &pixels[i * channels..(i + 1) * channels];
                let expected = match channels {
                    1 => [src[0], src[0], src[0], 255],
                    2 => [src[0], src[0], src[0], src[1]],
                    3 => [src[0], src[1], src[2], 255],
                    _ => [src[0], src[1], src[2], src[3]],
                };
                assert_eq!(p, expected, "channels {channels}, pixel {i}");
            }
        }
    }

    #[test]
    fn reads_what_png_write_writes() {
        let (w, h) = (33, 17);
        let rgba = test_pixels(w * h * 4);
        let png = png_write::encode_rgba(w as u32, h as u32, &rgba);
        assert_eq!(dimensions(&png).unwrap(), (w as u32, h as u32));
        let image = decode(&png).unwrap();
        assert_eq!(image.rgba, rgba);
    }

    #[test]
    fn refuses_what_it_cannot_read() {
        let mut png = png_with_every_filter(2, 2, 3, &test_pixels(12));
        assert_eq!(decode(b"GIF89a").unwrap_err(), Error::NotPng);
        assert_eq!(decode(&png[..20]).unwrap_err(), Error::Truncated);
        png[24] = 16; // bit depth
        assert!(matches!(
            decode(&png).unwrap_err(),
            Error::Unsupported { bit_depth: 16, .. }
        ));
        png[24] = 8;
        png[28] = 1; // Adam7
        assert_eq!(dimensions(&png).unwrap_err(), Error::Interlaced);
    }
}
