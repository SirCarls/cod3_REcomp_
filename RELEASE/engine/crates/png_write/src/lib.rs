//! Writes 8-bit RGBA images as PNG files.
//!
//! Follows the PNG specification (W3C, Portable Network Graphics, see
//! docs/SOURCES.md): signature, IHDR, one IDAT holding the zlib-compressed
//! scanlines, IEND. Every scanline uses filter 0 (none) or 2 (up), whichever
//! has the smaller sum of absolute values (the spec's suggested heuristic).
//! Compression is done by `miniz_oxide`.

#![forbid(unsafe_code)]

/// PNG file signature.
const SIGNATURE: [u8; 8] = [0x89, b'P', b'N', b'G', b'\r', b'\n', 0x1A, b'\n'];
const BIT_DEPTH: u8 = 8;
const COLOR_TYPE_RGBA: u8 = 6;
const FILTER_NONE: u8 = 0;
const FILTER_UP: u8 = 2;
/// zlib level: 6 is the usual balance of speed and size.
const COMPRESSION_LEVEL: u8 = 6;

/// CRC-32 (ISO 3309, the polynomial PNG uses), bit by bit for clarity; the
/// chunks here are few, so speed doesn't matter.
fn crc32(parts: &[&[u8]]) -> u32 {
    let mut crc = 0xFFFF_FFFFu32;
    for part in parts {
        for &byte in *part {
            crc ^= byte as u32;
            for _ in 0..8 {
                crc = if crc & 1 != 0 {
                    (crc >> 1) ^ 0xEDB8_8320
                } else {
                    crc >> 1
                };
            }
        }
    }
    !crc
}

fn push_chunk(out: &mut Vec<u8>, kind: &[u8; 4], data: &[u8]) {
    out.extend_from_slice(&(data.len() as u32).to_be_bytes());
    out.extend_from_slice(kind);
    out.extend_from_slice(data);
    out.extend_from_slice(&crc32(&[kind, data]).to_be_bytes());
}

/// Encodes `rgba` (`width * height * 4` bytes, rows top to bottom) as a PNG.
///
/// Panics if `rgba` has the wrong length; callers build it from the same
/// width and height.
pub fn encode_rgba(width: u32, height: u32, rgba: &[u8]) -> Vec<u8> {
    let row_bytes = width as usize * 4;
    assert_eq!(rgba.len(), row_bytes * height as usize, "pixel data size");

    let mut scanlines = Vec::with_capacity((row_bytes + 1) * height as usize);
    let mut up = vec![0u8; row_bytes];
    for (y, row) in rgba.chunks_exact(row_bytes).enumerate() {
        let previous = if y == 0 {
            None
        } else {
            Some(&rgba[(y - 1) * row_bytes..y * row_bytes])
        };
        let cost = |bytes: &[u8]| {
            bytes
                .iter()
                .map(|&b| (b as i8).unsigned_abs() as u32)
                .sum::<u32>()
        };
        match previous {
            Some(prev) => {
                for ((u, &cur), &above) in up.iter_mut().zip(row).zip(prev) {
                    *u = cur.wrapping_sub(above);
                }
                if cost(&up) < cost(row) {
                    scanlines.push(FILTER_UP);
                    scanlines.extend_from_slice(&up);
                    continue;
                }
                scanlines.push(FILTER_NONE);
                scanlines.extend_from_slice(row);
            }
            None => {
                scanlines.push(FILTER_NONE);
                scanlines.extend_from_slice(row);
            }
        }
    }

    let mut header = Vec::with_capacity(13);
    header.extend_from_slice(&width.to_be_bytes());
    header.extend_from_slice(&height.to_be_bytes());
    // bit depth, color type, compression 0, filter method 0, no interlace
    header.extend_from_slice(&[BIT_DEPTH, COLOR_TYPE_RGBA, 0, 0, 0]);

    let compressed = miniz_oxide::deflate::compress_to_vec_zlib(&scanlines, COMPRESSION_LEVEL);
    let mut out = Vec::with_capacity(compressed.len() + 64);
    out.extend_from_slice(&SIGNATURE);
    push_chunk(&mut out, b"IHDR", &header);
    push_chunk(&mut out, b"IDAT", &compressed);
    push_chunk(&mut out, b"IEND", &[]);
    out
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn crc_of_iend() {
        // Every PNG ends with this chunk; its CRC is fixed.
        assert_eq!(crc32(&[b"IEND"]), 0xAE42_6082);
    }

    #[test]
    fn round_trip_scanlines() {
        let rgba: Vec<u8> = (0..2 * 3 * 4).map(|i| (i * 7) as u8).collect();
        let png = encode_rgba(2, 3, &rgba);
        assert_eq!(&png[..8], &SIGNATURE);
        // IDAT starts after the signature (8) and IHDR (4 + 4 + 13 + 4).
        let idat_len = u32::from_be_bytes(png[33..37].try_into().unwrap()) as usize;
        assert_eq!(&png[37..41], b"IDAT");
        let raw = miniz_oxide::inflate::decompress_to_vec_zlib(&png[41..41 + idat_len]).unwrap();
        // Undo the filters and compare.
        let mut rows: Vec<Vec<u8>> = Vec::new();
        for line in raw.chunks_exact(1 + 8) {
            let mut row = line[1..].to_vec();
            if line[0] == FILTER_UP {
                let prev = rows.last().unwrap();
                row.iter_mut()
                    .zip(prev)
                    .for_each(|(b, &p)| *b = b.wrapping_add(p));
            }
            rows.push(row);
        }
        assert_eq!(rows.concat(), rgba);
    }
}
