//! Dependency-free DEFLATE (RFC 1951) and zlib (RFC 1950) decoder.
//!
//! The rippers must build without crates.io, so this replaces miniz_oxide /
//! flate2 for reading World at War fastfiles (one zlib stream after the
//! 12-byte header) and `.iwd` (zip) members.

#![forbid(unsafe_code)]

use std::fmt;

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Error {
    UnexpectedEof,
    BadZlibHeader,
    BadBlockType,
    BadStoredLength,
    BadHuffmanTable,
    BadSymbol,
    BadDistance,
    BadChecksum,
}

impl fmt::Display for Error {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "{self:?}")
    }
}

impl std::error::Error for Error {}

struct BitReader<'a> {
    data: &'a [u8],
    pos: usize,
    /// Bits read ahead from `data`, least significant bit first (DEFLATE bit order).
    bit_buffer: u64,
    /// How many bits of `bit_buffer` are valid.
    bit_count: u32,
}

impl<'a> BitReader<'a> {
    fn new(data: &'a [u8]) -> Self {
        Self {
            data,
            pos: 0,
            bit_buffer: 0,
            bit_count: 0,
        }
    }

    #[inline]
    fn refill(&mut self, n: u32) -> Result<(), Error> {
        while self.bit_count < n {
            if self.pos >= self.data.len() {
                return Err(Error::UnexpectedEof);
            }
            self.bit_buffer |= (self.data[self.pos] as u64) << self.bit_count;
            self.pos += 1;
            self.bit_count += 8;
        }
        Ok(())
    }

    #[inline]
    /// Reads `n` bits (0..=32) as an unsigned value.
    fn bits(&mut self, n: u32) -> Result<u32, Error> {
        if n == 0 {
            return Ok(0);
        }
        self.refill(n)?;
        let v = (self.bit_buffer & ((1u64 << n) - 1)) as u32;
        self.bit_buffer >>= n;
        self.bit_count -= n;
        Ok(v)
    }

    fn align_byte(&mut self) {
        let drop = self.bit_count % 8;
        self.bit_buffer >>= drop;
        self.bit_count -= drop;
    }

    /// Byte position of the next unread byte (after `align_byte`).
    fn byte_pos(&self) -> usize {
        self.pos - (self.bit_count / 8) as usize
    }
}

/// Canonical Huffman decoder (counts per length + symbols sorted by code).
struct Huffman {
    counts: [u16; 16],
    symbols: Vec<u16>,
}

impl Huffman {
    fn new(lengths: &[u8]) -> Result<Self, Error> {
        let mut counts = [0u16; 16];
        for &l in lengths {
            counts[l as usize] += 1;
        }
        counts[0] = 0;
        // Over-subscribed sets are invalid; incomplete ones are allowed (single-code trees).
        let mut left: i32 = 1;
        for &count in &counts[1..16] {
            left <<= 1;
            left -= count as i32;
            if left < 0 {
                return Err(Error::BadHuffmanTable);
            }
        }
        let mut offs = [0u16; 16];
        for len in 1..15 {
            offs[len + 1] = offs[len] + counts[len];
        }
        let mut symbols = vec![0u16; lengths.len()];
        for (sym, &l) in lengths.iter().enumerate() {
            if l != 0 {
                symbols[offs[l as usize] as usize] = sym as u16;
                offs[l as usize] += 1;
            }
        }
        Ok(Self { counts, symbols })
    }

    fn decode(&self, reader: &mut BitReader) -> Result<u16, Error> {
        let mut code: i32 = 0;
        let mut first: i32 = 0;
        let mut index: i32 = 0;
        for len in 1..16 {
            code |= reader.bits(1)? as i32;
            let count = self.counts[len] as i32;
            if code - count < first {
                return Ok(self.symbols[(index + (code - first)) as usize]);
            }
            index += count;
            first += count;
            first <<= 1;
            code <<= 1;
        }
        Err(Error::BadSymbol)
    }
}

const LEN_BASE: [u16; 29] = [
    3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131,
    163, 195, 227, 258,
];
const LEN_EXTRA: [u8; 29] = [
    0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0,
];
const DIST_BASE: [u16; 30] = [
    1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537,
    2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577,
];
const DIST_EXTRA: [u8; 30] = [
    0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13,
    13,
];

/// Decodes one Huffman-coded block (fixed or dynamic) until the end-of-block symbol.
fn codes(
    reader: &mut BitReader,
    out: &mut Vec<u8>,
    lit: &Huffman,
    dist: &Huffman,
) -> Result<(), Error> {
    loop {
        let sym = lit.decode(reader)?;
        match sym {
            0..=255 => out.push(sym as u8),
            256 => return Ok(()),
            257..=285 => {
                let i = (sym - 257) as usize;
                let len = LEN_BASE[i] as usize + reader.bits(LEN_EXTRA[i] as u32)? as usize;
                let ds = dist.decode(reader)? as usize;
                if ds >= 30 {
                    return Err(Error::BadDistance);
                }
                let d = DIST_BASE[ds] as usize + reader.bits(DIST_EXTRA[ds] as u32)? as usize;
                if d > out.len() {
                    return Err(Error::BadDistance);
                }
                let start = out.len() - d;
                if d >= len {
                    out.extend_from_within(start..start + len);
                } else {
                    for k in 0..len {
                        let b = out[start + k];
                        out.push(b);
                    }
                }
            }
            _ => return Err(Error::BadSymbol),
        }
    }
}

/// The fixed literal/length and distance codes of block type 1 (RFC 1951 3.2.6).
fn fixed_tables() -> (Huffman, Huffman) {
    let mut l = [0u8; 288];
    l[..144].fill(8);
    l[144..256].fill(9);
    l[256..280].fill(7);
    l[280..].fill(8);
    let d = [5u8; 30];
    (Huffman::new(&l).unwrap(), Huffman::new(&d).unwrap())
}

/// Reads the code-length code, then the literal/length and distance code lengths of a
/// block type 2 header (RFC 1951 3.2.7).
fn dynamic_tables(reader: &mut BitReader) -> Result<(Huffman, Huffman), Error> {
    const ORDER: [usize; 19] = [
        16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15,
    ];
    let nlen = reader.bits(5)? as usize + 257;
    let ndist = reader.bits(5)? as usize + 1;
    let ncode = reader.bits(4)? as usize + 4;
    if nlen > 286 || ndist > 30 {
        return Err(Error::BadHuffmanTable);
    }
    let mut cl = [0u8; 19];
    for &o in ORDER.iter().take(ncode) {
        cl[o] = reader.bits(3)? as u8;
    }
    let clh = Huffman::new(&cl)?;
    let mut lengths = vec![0u8; nlen + ndist];
    let mut i = 0;
    while i < nlen + ndist {
        let sym = clh.decode(reader)?;
        match sym {
            0..=15 => {
                lengths[i] = sym as u8;
                i += 1;
            }
            16 => {
                if i == 0 {
                    return Err(Error::BadHuffmanTable);
                }
                let prev = lengths[i - 1];
                let n = 3 + reader.bits(2)? as usize;
                if i + n > lengths.len() {
                    return Err(Error::BadHuffmanTable);
                }
                lengths[i..i + n].fill(prev);
                i += n;
            }
            17 | 18 => {
                let n = if sym == 17 {
                    3 + reader.bits(3)? as usize
                } else {
                    11 + reader.bits(7)? as usize
                };
                if i + n > lengths.len() {
                    return Err(Error::BadHuffmanTable);
                }
                i += n; // already zero
            }
            _ => return Err(Error::BadHuffmanTable),
        }
    }
    if lengths[256] == 0 {
        return Err(Error::BadHuffmanTable);
    }
    Ok((
        Huffman::new(&lengths[..nlen])?,
        Huffman::new(&lengths[nlen..])?,
    ))
}

/// Decodes a raw DEFLATE stream. Returns the data and the number of input
/// bytes consumed.
pub fn inflate_raw(data: &[u8], size_hint: usize) -> Result<(Vec<u8>, usize), Error> {
    let mut out = Vec::with_capacity(size_hint);
    let mut reader = BitReader::new(data);
    let mut fixed: Option<(Huffman, Huffman)> = None;
    loop {
        let last = reader.bits(1)? == 1;
        match reader.bits(2)? {
            0 => {
                reader.align_byte();
                let p = reader.byte_pos();
                // Discard any whole bytes still buffered, then read LEN/NLEN directly.
                reader.bit_buffer = 0;
                reader.bit_count = 0;
                reader.pos = p;
                if p + 4 > data.len() {
                    return Err(Error::UnexpectedEof);
                }
                let len = u16::from_le_bytes([data[p], data[p + 1]]) as usize;
                let nlen = u16::from_le_bytes([data[p + 2], data[p + 3]]) as usize;
                if len != (!nlen & 0xFFFF) {
                    return Err(Error::BadStoredLength);
                }
                let s = p + 4;
                if s + len > data.len() {
                    return Err(Error::UnexpectedEof);
                }
                out.extend_from_slice(&data[s..s + len]);
                reader.pos = s + len;
            }
            1 => {
                let (l, d) = fixed.get_or_insert_with(fixed_tables);
                codes(&mut reader, &mut out, l, d)?;
            }
            2 => {
                let (l, d) = dynamic_tables(&mut reader)?;
                codes(&mut reader, &mut out, &l, &d)?;
            }
            _ => return Err(Error::BadBlockType),
        }
        if last {
            break;
        }
    }
    reader.align_byte();
    Ok((out, reader.byte_pos()))
}

fn adler32(data: &[u8]) -> u32 {
    let (mut a, mut b) = (1u32, 0u32);
    for chunk in data.chunks(5552) {
        for &x in chunk {
            a += x as u32;
            b += a;
        }
        a %= 65521;
        b %= 65521;
    }
    (b << 16) | a
}

/// Decodes a zlib stream (2-byte header, DEFLATE data, Adler-32).
pub fn inflate_zlib(data: &[u8], size_hint: usize) -> Result<Vec<u8>, Error> {
    if data.len() < 6 {
        return Err(Error::UnexpectedEof);
    }
    let (cmf, flg) = (data[0], data[1]);
    if cmf & 0x0F != 8 || ((cmf as u16) << 8 | flg as u16) % 31 != 0 || flg & 0x20 != 0 {
        return Err(Error::BadZlibHeader);
    }
    let (out, used) = inflate_raw(&data[2..], size_hint)?;
    let p = 2 + used;
    if p + 4 <= data.len() {
        let want = u32::from_be_bytes([data[p], data[p + 1], data[p + 2], data[p + 3]]);
        if want != adler32(&out) {
            return Err(Error::BadChecksum);
        }
    }
    Ok(out)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn stored_and_fixed() {
        // zlib.compress(b"hello hello hello", 9)
        let z = [
            0x78, 0xda, 0xcb, 0x48, 0xcd, 0xc9, 0xc9, 0x57, 0xc8, 0x40, 0x90, 0x00, 0x3a, 0x2e,
            0x06, 0x7d,
        ];
        assert_eq!(inflate_zlib(&z, 0).unwrap(), b"hello hello hello");
        // zlib.compress(b"abc", 0) -> stored block
        let s = [
            0x78, 0x01, 0x01, 0x03, 0x00, 0xfc, 0xff, 0x61, 0x62, 0x63, 0x02, 0x4d, 0x01, 0x27,
        ];
        assert_eq!(inflate_zlib(&s, 0).unwrap(), b"abc");
    }
}
