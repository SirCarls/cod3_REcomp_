//! The block checksums both games use for their archives: MD4 (RFC 1320,
//! see docs/SOURCES.md) of a block, its four words folded into one by XOR.
//! A pure server and its clients compare archives by these
//! (docs/PC_MULTIPLAYER.md, section 8): an archive's checksum is that of
//! the list of its files' CRC-32s, its pure checksum the same with the
//! game's checksum feed first.

/// MD4 of `data`: the four state words after the last block (the digest's
/// bytes, read as little-endian words).
pub fn md4(data: &[u8]) -> [u32; 4] {
    let mut state: [u32; 4] = [0x6745_2301, 0xEFCD_AB89, 0x98BA_DCFE, 0x1032_5476];
    let mut message = data.to_vec();
    let bits = (data.len() as u64).wrapping_mul(8);
    message.push(0x80);
    while message.len() % 64 != 56 {
        message.push(0);
    }
    message.extend_from_slice(&bits.to_le_bytes());
    for block in message.chunks_exact(64) {
        let mut x = [0u32; 16];
        for (i, word) in x.iter_mut().enumerate() {
            *word = u32::from_le_bytes([
                block[4 * i],
                block[4 * i + 1],
                block[4 * i + 2],
                block[4 * i + 3],
            ]);
        }
        let [mut a, mut b, mut c, mut d] = state;
        let f = |x: u32, y: u32, z: u32| (x & y) | (!x & z);
        let g = |x: u32, y: u32, z: u32| (x & y) | (x & z) | (y & z);
        let h = |x: u32, y: u32, z: u32| x ^ y ^ z;
        for &k in &[0usize, 4, 8, 12] {
            a = a.wrapping_add(f(b, c, d)).wrapping_add(x[k]).rotate_left(3);
            d = d
                .wrapping_add(f(a, b, c))
                .wrapping_add(x[k + 1])
                .rotate_left(7);
            c = c
                .wrapping_add(f(d, a, b))
                .wrapping_add(x[k + 2])
                .rotate_left(11);
            b = b
                .wrapping_add(f(c, d, a))
                .wrapping_add(x[k + 3])
                .rotate_left(19);
        }
        const ROUND2: u32 = 0x5A82_7999;
        for &k in &[0usize, 1, 2, 3] {
            a = a
                .wrapping_add(g(b, c, d))
                .wrapping_add(x[k])
                .wrapping_add(ROUND2)
                .rotate_left(3);
            d = d
                .wrapping_add(g(a, b, c))
                .wrapping_add(x[k + 4])
                .wrapping_add(ROUND2)
                .rotate_left(5);
            c = c
                .wrapping_add(g(d, a, b))
                .wrapping_add(x[k + 8])
                .wrapping_add(ROUND2)
                .rotate_left(9);
            b = b
                .wrapping_add(g(c, d, a))
                .wrapping_add(x[k + 12])
                .wrapping_add(ROUND2)
                .rotate_left(13);
        }
        const ROUND3: u32 = 0x6ED9_EBA1;
        for &k in &[0usize, 2, 1, 3] {
            a = a
                .wrapping_add(h(b, c, d))
                .wrapping_add(x[k])
                .wrapping_add(ROUND3)
                .rotate_left(3);
            d = d
                .wrapping_add(h(a, b, c))
                .wrapping_add(x[k + 8])
                .wrapping_add(ROUND3)
                .rotate_left(9);
            c = c
                .wrapping_add(h(d, a, b))
                .wrapping_add(x[k + 4])
                .wrapping_add(ROUND3)
                .rotate_left(11);
            b = b
                .wrapping_add(h(c, d, a))
                .wrapping_add(x[k + 12])
                .wrapping_add(ROUND3)
                .rotate_left(15);
        }
        state = [
            state[0].wrapping_add(a),
            state[1].wrapping_add(b),
            state[2].wrapping_add(c),
            state[3].wrapping_add(d),
        ];
    }
    state
}

/// MD4 of `data`, its four words XORed together.
///
/// Original: `Com_BlockChecksum` at 0x00443A70 (CoD2MP_s.exe 1.3)
/// Original: `Com_BlockChecksum` at 0x00449470 (CoDUOMP.exe 1.51)
pub fn block_checksum(data: &[u8]) -> i32 {
    let [a, b, c, d] = md4(data);
    (a ^ b ^ c ^ d) as i32
}

/// [`block_checksum`] of `key` (4 bytes, little-endian) followed by `data`.
///
/// Original: `Com_BlockChecksumKey` at 0x00443AE0 (CoD2MP_s.exe 1.3)
/// Original: `Com_BlockChecksumKey` at 0x004494F0 (CoDUOMP.exe 1.51)
pub fn block_checksum_key(data: &[u8], key: i32) -> i32 {
    let mut keyed = key.to_le_bytes().to_vec();
    keyed.extend_from_slice(data);
    block_checksum(&keyed)
}

/// An archive's checksum and pure checksum, as the games' `FS_LoadZipFile`
/// makes them: from the CRC-32 of each file in the archive with any bytes
/// (in the central directory's order), as little-endian words; the pure
/// checksum keyed with the game's checksum feed (`fs_checksumFeed`, the
/// server's number for the map).
///
/// (The end of `FS_LoadZipFile`: 0x00423922 in CoD2MP_s.exe 1.3, 0x0042ED8E
/// in CoDUOMP.exe 1.51; checked through the two functions it calls.)
pub fn archive_checksums(crcs: &[u32], feed: i32) -> (i32, i32) {
    let bytes: Vec<u8> = crcs.iter().flat_map(|c| c.to_le_bytes()).collect();
    (block_checksum(&bytes), block_checksum_key(&bytes, feed))
}

#[cfg(test)]
mod tests {
    use super::*;

    fn hex(state: [u32; 4]) -> String {
        state
            .iter()
            .flat_map(|w| w.to_le_bytes())
            .map(|b| format!("{b:02x}"))
            .collect()
    }

    #[test]
    fn md4_matches_rfc_1320() {
        // The test suite in RFC 1320, appendix A.5.
        assert_eq!(hex(md4(b"")), "31d6cfe0d16ae931b73c59d7e0c089c0");
        assert_eq!(hex(md4(b"a")), "bde52cb31de33e46245e05fbdbd6fb24");
        assert_eq!(hex(md4(b"abc")), "a448017aaf21d8525fc10ae87aa6729d");
        assert_eq!(
            hex(md4(b"message digest")),
            "d9130a8164549fe818874806e1c7014b"
        );
        assert_eq!(
            hex(md4(
                b"12345678901234567890123456789012345678901234567890123456789012345678901234567890"
            )),
            "e33b4ddc9c38f2199c3e7b164fcc0536"
        );
    }
}
