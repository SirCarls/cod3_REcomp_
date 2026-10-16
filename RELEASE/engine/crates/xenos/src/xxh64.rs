//! XXH64, the 64-bit xxHash, written from Yann Collet's "xxHash fast digest
//! algorithm" specification
//! (<https://github.com/Cyan4973/xxHash/blob/dev/doc/xxhash_spec.md>).
//!
//! It names textures for the HD texture pack: the ripper hashes a texture's
//! untiled base level, and the recompiled game's texture cache hashes the
//! same bytes from guest memory with the reference xxHash library, so both
//! sides must produce exactly the reference values (the tests check them).

const PRIME_1: u64 = 0x9E37_79B1_85EB_CA87;
const PRIME_2: u64 = 0xC2B2_AE3D_27D4_EB4F;
const PRIME_3: u64 = 0x1656_67B1_9E37_79F9;
const PRIME_4: u64 = 0x85EB_CA77_C2B2_AE63;
const PRIME_5: u64 = 0x27D4_EB2F_1656_67C5;

/// Bytes consumed per round of the four parallel accumulators.
const STRIPE: usize = 32;

fn read_u64(bytes: &[u8]) -> u64 {
    u64::from_le_bytes(bytes[..8].try_into().expect("8 bytes"))
}

fn read_u32(bytes: &[u8]) -> u32 {
    u32::from_le_bytes(bytes[..4].try_into().expect("4 bytes"))
}

/// One accumulator step: mixes 8 input bytes into `accumulator`.
fn round(accumulator: u64, lane: u64) -> u64 {
    accumulator
        .wrapping_add(lane.wrapping_mul(PRIME_2))
        .rotate_left(31)
        .wrapping_mul(PRIME_1)
}

/// Folds one of the four accumulators into the converged hash.
fn merge_accumulator(hash: u64, accumulator: u64) -> u64 {
    (hash ^ round(0, accumulator))
        .wrapping_mul(PRIME_1)
        .wrapping_add(PRIME_4)
}

/// XXH64 of `input` with `seed` (the texture keys use seed 0).
pub fn xxh64(input: &[u8], seed: u64) -> u64 {
    let mut hash;
    let mut rest = input;
    if input.len() >= STRIPE {
        // Inputs of a stripe or more run four independent lanes, then merge.
        let mut acc = [
            seed.wrapping_add(PRIME_1).wrapping_add(PRIME_2),
            seed.wrapping_add(PRIME_2),
            seed,
            seed.wrapping_sub(PRIME_1),
        ];
        while rest.len() >= STRIPE {
            for (i, a) in acc.iter_mut().enumerate() {
                *a = round(*a, read_u64(&rest[i * 8..]));
            }
            rest = &rest[STRIPE..];
        }
        hash = acc[0]
            .rotate_left(1)
            .wrapping_add(acc[1].rotate_left(7))
            .wrapping_add(acc[2].rotate_left(12))
            .wrapping_add(acc[3].rotate_left(18));
        for a in acc {
            hash = merge_accumulator(hash, a);
        }
    } else {
        hash = seed.wrapping_add(PRIME_5);
    }
    hash = hash.wrapping_add(input.len() as u64);

    // The tail (under 32 bytes): 8 bytes, then 4, then single bytes.
    while rest.len() >= 8 {
        hash ^= round(0, read_u64(rest));
        hash = hash
            .rotate_left(27)
            .wrapping_mul(PRIME_1)
            .wrapping_add(PRIME_4);
        rest = &rest[8..];
    }
    if rest.len() >= 4 {
        hash ^= (read_u32(rest) as u64).wrapping_mul(PRIME_1);
        hash = hash
            .rotate_left(23)
            .wrapping_mul(PRIME_2)
            .wrapping_add(PRIME_3);
        rest = &rest[4..];
    }
    for &byte in rest {
        hash ^= (byte as u64).wrapping_mul(PRIME_5);
        hash = hash.rotate_left(11).wrapping_mul(PRIME_1);
    }

    // Final avalanche so every input bit affects every output bit.
    hash ^= hash >> 33;
    hash = hash.wrapping_mul(PRIME_2);
    hash ^= hash >> 29;
    hash = hash.wrapping_mul(PRIME_3);
    hash ^ (hash >> 32)
}

#[cfg(test)]
mod tests {
    use super::*;

    /// `count` bytes 0, step, 2 * step, ... (mod 256).
    fn ramp(count: usize, step: usize) -> Vec<u8> {
        (0..count).map(|i| (i * step) as u8).collect()
    }

    // Reference values from the xxHash library (XXH64, seed 0), which the
    // game side uses too.
    #[test]
    fn short_inputs() {
        assert_eq!(xxh64(b"", 0), 0xEF46_DB37_51D8_E999);
        assert_eq!(xxh64(b"a", 0), 0xD24E_C4F1_A98C_6E5B);
        assert_eq!(xxh64(b"abc", 0), 0x44BC_2CF5_AD77_0999);
        assert_eq!(xxh64(b"xxhash", 0), 0x32DD_3895_2C4B_C720);
    }

    #[test]
    fn stripes_and_unaligned_tails() {
        // 39 bytes: one stripe, then a 4-byte and a 3-byte tail.
        assert_eq!(
            xxh64(b"Nobody inspects the spammish repetition", 0),
            0xFBCE_A83C_8A37_8BF1
        );
        assert_eq!(xxh64(&ramp(32, 1), 0), 0xCBF5_9C51_16FF_32B4);
        assert_eq!(xxh64(&ramp(33, 1), 0), 0x0C53_5D1A_CAFB_8EAD);
        assert_eq!(xxh64(&ramp(37, 1), 0), 0xD93F_A2DF_EE5C_24C9);
        assert_eq!(xxh64(&ramp(64, 1), 0), 0xF7C6_7301_DB67_13F0);
        assert_eq!(xxh64(&ramp(101, 7), 0), 0x4231_2F4D_55D9_BD17);
        assert_eq!(xxh64(&ramp(1000, 31), 0), 0xB128_0F64_2812_6532);
    }

    #[test]
    fn hash_does_not_depend_on_slice_alignment() {
        let data = ramp(200, 13);
        let mut shifted = vec![0u8];
        shifted.extend_from_slice(&data);
        assert_eq!(xxh64(&data, 0), xxh64(&shifted[1..], 0));
    }
}
