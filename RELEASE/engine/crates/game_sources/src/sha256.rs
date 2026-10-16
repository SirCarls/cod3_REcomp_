//! SHA-256 (FIPS 180-4, section 6.2), streaming.
//!
//! The new copy's manifest records each file's SHA-256 so `newcopy check`
//! can prove later that nothing changed. Archives are up to about 170 MB,
//! so files are hashed in 1 MiB blocks and never read whole.

use std::fs::File;
use std::io::{self, Read};
use std::path::Path;

/// Bytes read per block when hashing a file.
pub const BLOCK_BYTES: usize = 1 << 20;

/// First 32 bits of the fractional parts of the cube roots of the first 64
/// primes (FIPS 180-4, 4.2.2).
const ROUND_CONSTANTS: [u32; 64] = [
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
];

/// First 32 bits of the fractional parts of the square roots of the first 8
/// primes (FIPS 180-4, 5.3.3).
const INITIAL_STATE: [u32; 8] = [
    0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19,
];

/// A running SHA-256 hash: feed it bytes with [`Sha256::update`], then take
/// the 32-byte digest with [`Sha256::finish`].
#[derive(Clone)]
pub struct Sha256 {
    state: [u32; 8],
    /// Bytes waiting for a full 64-byte block.
    pending: [u8; 64],
    pending_len: usize,
    /// Message length so far, in bytes.
    total_len: u64,
}

impl Default for Sha256 {
    fn default() -> Self {
        Self::new()
    }
}

impl Sha256 {
    pub fn new() -> Self {
        Sha256 {
            state: INITIAL_STATE,
            pending: [0; 64],
            pending_len: 0,
            total_len: 0,
        }
    }

    pub fn update(&mut self, mut data: &[u8]) {
        self.total_len = self.total_len.wrapping_add(data.len() as u64);
        if self.pending_len > 0 {
            let take = (64 - self.pending_len).min(data.len());
            self.pending[self.pending_len..self.pending_len + take].copy_from_slice(&data[..take]);
            self.pending_len += take;
            data = &data[take..];
            if self.pending_len < 64 {
                return;
            }
            let block = self.pending;
            compress(&mut self.state, &block);
            self.pending_len = 0;
        }
        let mut blocks = data.chunks_exact(64);
        for block in &mut blocks {
            compress(&mut self.state, block);
        }
        let rest = blocks.remainder();
        self.pending[..rest.len()].copy_from_slice(rest);
        self.pending_len = rest.len();
    }

    pub fn finish(mut self) -> [u8; 32] {
        // Padding (5.1.1): a 1 bit, zeros, then the length in bits as a
        // big-endian 64-bit number, so the total is a multiple of 64 bytes.
        let bit_len = self.total_len.wrapping_mul(8);
        let mut tail = [0u8; 128];
        tail[..self.pending_len].copy_from_slice(&self.pending[..self.pending_len]);
        tail[self.pending_len] = 0x80;
        let tail_len = if self.pending_len < 56 { 64 } else { 128 };
        tail[tail_len - 8..tail_len].copy_from_slice(&bit_len.to_be_bytes());
        for block in tail[..tail_len].chunks_exact(64) {
            compress(&mut self.state, block);
        }
        let mut digest = [0u8; 32];
        for (out, word) in digest.chunks_exact_mut(4).zip(self.state) {
            out.copy_from_slice(&word.to_be_bytes());
        }
        digest
    }
}

/// One 64-byte block through the compression function (6.2.2).
fn compress(state: &mut [u32; 8], block: &[u8]) {
    let mut schedule = [0u32; 64];
    for (word, bytes) in schedule.iter_mut().zip(block.chunks_exact(4)) {
        *word = u32::from_be_bytes([bytes[0], bytes[1], bytes[2], bytes[3]]);
    }
    for t in 16..64 {
        let s0 = schedule[t - 15].rotate_right(7)
            ^ schedule[t - 15].rotate_right(18)
            ^ (schedule[t - 15] >> 3);
        let s1 = schedule[t - 2].rotate_right(17)
            ^ schedule[t - 2].rotate_right(19)
            ^ (schedule[t - 2] >> 10);
        schedule[t] = schedule[t - 16]
            .wrapping_add(s0)
            .wrapping_add(schedule[t - 7])
            .wrapping_add(s1);
    }
    let [mut a, mut b, mut c, mut d, mut e, mut f, mut g, mut h] = *state;
    for t in 0..64 {
        let sum1 = e.rotate_right(6) ^ e.rotate_right(11) ^ e.rotate_right(25);
        let choose = (e & f) ^ (!e & g);
        let temp1 = h
            .wrapping_add(sum1)
            .wrapping_add(choose)
            .wrapping_add(ROUND_CONSTANTS[t])
            .wrapping_add(schedule[t]);
        let sum0 = a.rotate_right(2) ^ a.rotate_right(13) ^ a.rotate_right(22);
        let majority = (a & b) ^ (a & c) ^ (b & c);
        let temp2 = sum0.wrapping_add(majority);
        h = g;
        g = f;
        f = e;
        e = d.wrapping_add(temp1);
        d = c;
        c = b;
        b = a;
        a = temp1.wrapping_add(temp2);
    }
    for (word, add) in state.iter_mut().zip([a, b, c, d, e, f, g, h]) {
        *word = word.wrapping_add(add);
    }
}

/// SHA-256 of a byte slice.
pub fn digest(data: &[u8]) -> [u8; 32] {
    let mut hasher = Sha256::new();
    hasher.update(data);
    hasher.finish()
}

/// Lowercase hex, as written in the manifest.
pub fn to_hex(digest: &[u8; 32]) -> String {
    const DIGITS: &[u8; 16] = b"0123456789abcdef";
    let mut text = String::with_capacity(64);
    for byte in digest {
        text.push(DIGITS[usize::from(byte >> 4)] as char);
        text.push(DIGITS[usize::from(byte & 15)] as char);
    }
    text
}

/// Reads hex written by [`to_hex`] (either case); `None` if it isn't 64 hex digits.
pub fn from_hex(text: &str) -> Option<[u8; 32]> {
    let bytes = text.as_bytes();
    if bytes.len() != 64 {
        return None;
    }
    let mut digest = [0u8; 32];
    for (out, pair) in digest.iter_mut().zip(bytes.chunks_exact(2)) {
        let high = (pair[0] as char).to_digit(16)?;
        let low = (pair[1] as char).to_digit(16)?;
        *out = (high * 16 + low) as u8;
    }
    Some(digest)
}

/// Hashes everything a reader gives, in 1 MiB blocks. Returns the number of
/// bytes read and the digest.
pub fn hash_reader<R: Read>(mut reader: R) -> io::Result<(u64, [u8; 32])> {
    let mut hasher = Sha256::new();
    let mut block = vec![0u8; BLOCK_BYTES];
    let mut total = 0u64;
    loop {
        let n = match reader.read(&mut block) {
            Ok(0) => break,
            Ok(n) => n,
            Err(e) if e.kind() == io::ErrorKind::Interrupted => continue,
            Err(e) => return Err(e),
        };
        hasher.update(&block[..n]);
        total += n as u64;
    }
    Ok((total, hasher.finish()))
}

/// Hashes a file in 1 MiB blocks. Returns its size and digest.
pub fn hash_file(path: &Path) -> io::Result<(u64, [u8; 32])> {
    hash_reader(File::open(path)?)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn hex(data: &[u8]) -> String {
        to_hex(&digest(data))
    }

    // FIPS 180-4 examples (NIST "SHA256.pdf" and "SHA2_Additional.pdf").
    #[test]
    fn nist_vectors() {
        assert_eq!(
            hex(b""),
            "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"
        );
        assert_eq!(
            hex(b"abc"),
            "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"
        );
        assert_eq!(
            hex(b"abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"),
            "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"
        );
        assert_eq!(
            hex(b"abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijklmnoijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu"),
            "cf5b16a778af8380036ce59e7b0492370b249b11e8f07a51afac45037afee9d1"
        );
        assert_eq!(
            hex(&vec![b'a'; 1_000_000]),
            "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"
        );
        // One byte 0xbd and 55/56/64 bytes of zeros: the padding edge cases.
        assert_eq!(
            hex(&[0xbd]),
            "68325720aabd7c82f30f554b313d0570c95accbb7dc4b5aae11204c08ffe732b"
        );
        assert_eq!(
            hex(&[0u8; 55]),
            "02779466cdec163811d078815c633f21901413081449002f24aa3e80f0b88ef7"
        );
        assert_eq!(
            hex(&[0u8; 56]),
            "d4817aa5497628e7c77e6b606107042bbba3130888c5f47a375e6179be789fbb"
        );
        assert_eq!(
            hex(&[0u8; 64]),
            "f5a5fd42d16a20302798ef6ed309979b43003d2320d9f0e8ea9831a92759fb4b"
        );
    }

    #[test]
    fn split_updates_match_one_update() {
        let data: Vec<u8> = (0..1000u32).map(|i| (i * 7 + 3) as u8).collect();
        let whole = digest(&data);
        for split in [1, 3, 63, 64, 65, 127, 500] {
            let mut hasher = Sha256::new();
            for piece in data.chunks(split) {
                hasher.update(piece);
            }
            assert_eq!(hasher.finish(), whole, "pieces of {split}");
        }
    }

    #[test]
    fn reader_hash_counts_bytes() {
        let data = vec![7u8; BLOCK_BYTES + 5];
        let (len, sum) = hash_reader(&data[..]).unwrap();
        assert_eq!(len, data.len() as u64);
        assert_eq!(sum, digest(&data));
    }

    #[test]
    fn hex_round_trip() {
        let sum = digest(b"abc");
        assert_eq!(from_hex(&to_hex(&sum)), Some(sum));
        assert_eq!(from_hex(&to_hex(&sum).to_uppercase()), Some(sum));
        assert_eq!(from_hex("abc"), None);
        assert_eq!(from_hex(&"g".repeat(64)), None);
    }
}
