//! Demonware's bit buffer (`bdBitBuffer`): how the multiplayer program packs
//! the messages consoles send each other.
//!
//! A message is a type byte and one of these. Bits go in least significant
//! first: bit `n` of the stream is bit `n % 8` of byte `n / 8`, and a value
//! of several bytes goes in little-endian, so an N-bit value is its N low
//! bits in order. The first bit of every buffer is a header: whether the
//! buffer is *type-checked*. A type-checked buffer puts a 5-bit type tag
//! ([`DataType`]) before every value, and the range before every ranged
//! value; game messages are not type-checked, so they carry neither.
//!
//! Ranged values take only the bits their range needs:
//! [`high_bit_number`]`(hi - lo) + 1`. A ranged float is quantized to steps
//! of `precision`, rounding to nearest. Out-of-range values are clamped
//! (the original also logs an assertion, which changes nothing).
//!
//! The game's object: +0 vtable, +4 reference count, +8 the bytes (a
//! `bdFastArray<unsigned char>`: data, capacity, size), +0x14 write position
//! in bits, +0x18 one past the highest bit written, +0x1C read position,
//! +0x20 "a read failed", +0x21 type-checked.

use crate::ppc::{fctidz, fmadds};

/// The 5-bit type tags of a type-checked buffer (the ones the game uses).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[repr(u32)]
pub enum DataType {
    Bool = 1,
    Int8 = 2,
    UInt8 = 3,
    Int16 = 5,
    UInt16 = 6,
    Int32 = 7,
    UInt32 = 8,
    UInt64 = 10,
    RangedInt32 = 11,
    RangedUInt32 = 12,
    Float32 = 13,
    RangedFloat32 = 15,
    Blob = 19,
}

/// The largest number of steps a ranged float can have; a wider range or a
/// finer precision sends the plain 32-bit float instead.
const RANGED_FLOAT_LIMIT: f32 = 4_294_967_296.0;

/// The index of the highest set bit (`floor(log2(value))`); 0 for 0 (the
/// original asserts that it isn't 0, then returns 0).
///
/// Original: `bdBitOperations::highBitNumber` at 0x821B1A78
pub fn high_bit_number(value: u32) -> u32 {
    if value == 0 {
        0
    } else {
        31 - value.leading_zeros()
    }
}

/// How many bits a range needs.
fn range_bits(span: u32) -> u32 {
    if span == 0 {
        0
    } else {
        high_bit_number(span) + 1
    }
}

/// A Demonware bit buffer.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct BitBuffer {
    /// The bytes written so far (the array's size is `data.len()`).
    pub data: Vec<u8>,
    /// Where the next write goes, in bits.
    pub write_position: u32,
    /// One past the highest bit ever written: where reading stops.
    pub written: u32,
    /// Where the next read starts, in bits.
    pub read_position: u32,
    /// Set by any read that ran out of data; never cleared.
    pub read_failed: bool,
    /// Every value carries a type tag (see the module's documentation).
    pub type_checked: bool,
}

impl BitBuffer {
    /// An empty buffer with its header bit written; reading starts after it.
    /// (`capacity_bits` only sized the game's allocation.)
    ///
    /// Original: `bdBitBuffer::ctor` at 0x821AFCE8
    pub fn new(type_checked: bool) -> Self {
        let mut buffer = BitBuffer {
            data: Vec::new(),
            write_position: 0,
            written: 0,
            read_position: 0,
            read_failed: false,
            type_checked,
        };
        buffer.write_bits(&[if type_checked { 0xFF } else { 0 }], 1);
        buffer.read_position = 1;
        buffer
    }

    /// Appends the `bits` low bits of `source` (byte 0 first, each byte's
    /// low bit first). `source` must hold at least `bits` bits.
    ///
    /// Original: `bdBitBuffer::writeBits` at 0x821AFB08
    pub fn write_bits(&mut self, source: &[u8], bits: u32) {
        // The last byte the write touches is added, zeroed, when the array is
        // shorter; the bytes before it are written whole below.
        let last = (self.write_position.wrapping_add(bits).wrapping_sub(1) >> 3) as usize;
        if last >= self.data.len() {
            self.data.resize(last + 1, 0);
        }
        let mut remaining = bits;
        while remaining > 0 {
            let bit_offset = self.write_position & 7;
            let chunk = remaining.min(8 - bit_offset);
            // The bits of the destination byte this chunk doesn't cover.
            let keep = ((0xFFu32 << (chunk + bit_offset)) | (0xFFu32 >> (8 - bit_offset))) as u8;
            let done = bits - remaining;
            let index = (done >> 3) as usize;
            let shift = done & 7;
            let low = u32::from(source[index]);
            let high = if (bits - 1) >> 3 > index as u32 {
                u32::from(source[index + 1])
            } else {
                0
            };
            let value = ((high << (8 - shift)) & 0xFF) | (low >> shift);
            let byte = &mut self.data[(self.write_position >> 3) as usize];
            *byte = (*byte & keep) | ((value << bit_offset) as u8 & !keep);
            self.write_position += chunk;
            self.written = self.written.max(self.write_position);
            remaining -= chunk;
        }
    }

    /// Reads `bits` bits into `destination` (8 per byte, the last byte's low
    /// bits). Past what was written it fails, sets `read_failed` and leaves
    /// everything else as it is.
    ///
    /// Original: `bdBitBuffer::readBits` at 0x821AEF80
    pub fn read_bits(&mut self, destination: &mut [u8], bits: u32) -> bool {
        if bits == 0 {
            return true;
        }
        if self.read_position.wrapping_add(bits) > self.written {
            self.read_failed = true;
            return false;
        }
        let mut index = (self.read_position >> 3) as usize;
        let mut remaining = bits;
        let mut out = 0;
        loop {
            if index >= self.data.len() {
                self.read_failed = true;
                return false;
            }
            let chunk = remaining.min(8);
            let low = self.data[index];
            index += 1;
            let bit_offset = self.read_position & 7;
            let mask = (0xFFu32 >> (8 - chunk)) as u8;
            destination[out] = if bit_offset + chunk > 8 {
                if index >= self.data.len() {
                    self.read_failed = true;
                    return false;
                }
                let high = (u32::from(self.data[index]) << (8 - bit_offset)) as u8;
                (high | (low >> bit_offset)) & mask
            } else {
                (low >> bit_offset) & mask
            };
            out += 1;
            remaining -= chunk;
            self.read_position += chunk;
            if remaining == 0 {
                return true;
            }
        }
    }

    /// A type tag, in a type-checked buffer only.
    ///
    /// Original: `bdBitBuffer::writeDataType` at 0x821B0068
    pub fn write_data_type(&mut self, data_type: u32) {
        if self.type_checked {
            self.write_ranged_u32(data_type, 0, 31, false);
        }
    }

    /// Reads a type tag (a type-checked buffer only) and checks it is
    /// `expected`; true when it is, or when the buffer has no tags.
    ///
    /// Original: `bdBitBuffer::readDataType` at 0x821AF0B8
    pub fn read_data_type(&mut self, expected: u32) -> bool {
        if !self.type_checked {
            return true;
        }
        let mut found = 0;
        self.read_ranged_u32(&mut found, 0, 31, false) && found == expected
    }

    /// One bit: 1 when `value` isn't 0.
    ///
    /// Original: `bdBitBuffer::writeBool` at 0x820D9770
    pub fn write_bool(&mut self, value: bool) {
        self.write_data_type(DataType::Bool as u32);
        self.write_bits(&[if value { 0xFF } else { 0 }], 1);
    }

    /// A 16-bit value (type `Int16`).
    ///
    /// Original: `bdBitBuffer::writeInt16` at 0x820D97D8
    pub fn write_i16(&mut self, value: i16) {
        self.write_data_type(DataType::Int16 as u32);
        self.write_bits(&value.to_le_bytes(), 16);
    }

    /// A 16-bit value (type `UInt16`).
    ///
    /// Original: `bdBitBuffer::writeUInt16` at 0x820D9830
    pub fn write_u16(&mut self, value: u16) {
        self.write_data_type(DataType::UInt16 as u32);
        self.write_bits(&value.to_le_bytes(), 16);
    }

    /// A 32-bit value (type `Int32`).
    ///
    /// Original: `bdBitBuffer::writeInt32` at 0x820D9888
    pub fn write_i32(&mut self, value: i32) {
        self.write_data_type(DataType::Int32 as u32);
        self.write_bits(&value.to_le_bytes(), 32);
    }

    /// A 32-bit value (type `UInt32`).
    ///
    /// Original: `bdBitBuffer::writeUInt32` at 0x820D98F0
    pub fn write_u32(&mut self, value: u32) {
        self.write_data_type(DataType::UInt32 as u32);
        self.write_bits(&value.to_le_bytes(), 32);
    }

    /// A 64-bit value (type 10).
    ///
    /// Original: `bdBitBuffer::writeUInt64` at 0x820D9958
    pub fn write_u64(&mut self, value: u64) {
        self.write_data_type(DataType::UInt64 as u32);
        self.write_bits(&value.to_le_bytes(), 64);
    }

    /// A float's 32 bits.
    ///
    /// Original: `bdBitBuffer::writeFloat32` at 0x820D99E0
    pub fn write_f32(&mut self, value: f32) {
        self.write_data_type(DataType::Float32 as u32);
        self.write_bits(&value.to_bits().to_le_bytes(), 32);
    }

    /// One bit into `out` (true when it is 1); `out` stays as it was when the read fails.
    ///
    /// Original: `bdBitBuffer::readBool` at 0x820D9A48
    pub fn read_bool(&mut self, out: &mut bool) -> bool {
        if !self.read_data_type(DataType::Bool as u32) {
            return false;
        }
        let mut bit = [0u8];
        if !self.read_bits(&mut bit, 1) {
            return false;
        }
        *out = bit[0] != 0;
        true
    }

    /// Eight bits into `out` (written by the read even when it fails half way).
    ///
    /// Original: `bdBitBuffer::readUInt8` at 0x820D9B18
    pub fn read_u8(&mut self, out: &mut u8) -> bool {
        self.read_data_type(DataType::UInt8 as u32) && self.read_bits(std::slice::from_mut(out), 8)
    }

    /// Eight bits into `out` (type `Int8`; written by the read even when it fails half way).
    ///
    /// Original: `bdBitBuffer::readInt8` at 0x820D9B88
    pub fn read_i8(&mut self, out: &mut i8) -> bool {
        let mut byte = *out as u8;
        let ok = self.read_data_type(DataType::Int8 as u32)
            && self.read_bits(std::slice::from_mut(&mut byte), 8);
        *out = byte as i8;
        ok
    }

    /// 16 bits into `out`; `out` stays as it was when the read fails.
    ///
    /// Original: `bdBitBuffer::readInt16` at 0x820D9BF8
    pub fn read_i16(&mut self, out: &mut i16) -> bool {
        let mut raw = [0u8; 2];
        if !self.read_data_type(DataType::Int16 as u32) || !self.read_bits(&mut raw, 16) {
            return false;
        }
        *out = i16::from_le_bytes(raw);
        true
    }

    /// 16 bits into `out`; `out` stays as it was when the read fails.
    ///
    /// Original: `bdBitBuffer::readUInt16` at 0x820D9C80
    pub fn read_u16(&mut self, out: &mut u16) -> bool {
        let mut raw = [0u8; 2];
        if !self.read_data_type(DataType::UInt16 as u32) || !self.read_bits(&mut raw, 16) {
            return false;
        }
        *out = u16::from_le_bytes(raw);
        true
    }

    /// 64 bits into `out`; `out` stays as it was when the read fails.
    ///
    /// Original: `bdBitBuffer::readUInt64` at 0x820D9E38
    pub fn read_u64(&mut self, out: &mut u64) -> bool {
        let mut raw = [0u8; 8];
        if !self.read_data_type(DataType::UInt64 as u32) || !self.read_bits(&mut raw, 64) {
            return false;
        }
        *out = u64::from_le_bytes(raw);
        true
    }

    /// 32 bits into `out`; `out` stays as it was when the read fails.
    ///
    /// Original: `bdBitBuffer::readUInt32` at 0x820D9DA0
    pub fn read_u32(&mut self, out: &mut u32) -> bool {
        self.read_32(DataType::UInt32, out)
    }

    /// 32 bits into `out`; `out` stays as it was when the read fails.
    ///
    /// Original: `bdBitBuffer::readInt32` at 0x820D9D08
    pub fn read_i32(&mut self, out: &mut i32) -> bool {
        let mut bits = *out as u32;
        let ok = self.read_32(DataType::Int32, &mut bits);
        *out = bits as i32;
        ok
    }

    /// A float's 32 bits into `out`; `out` stays as it was when the read fails.
    ///
    /// Original: `bdBitBuffer::readFloat32` at 0x820D9EF0
    pub fn read_f32(&mut self, out: &mut f32) -> bool {
        let mut bits = out.to_bits();
        let ok = self.read_32(DataType::Float32, &mut bits);
        *out = f32::from_bits(bits);
        ok
    }

    fn read_32(&mut self, data_type: DataType, out: &mut u32) -> bool {
        let mut raw = [0u8; 4];
        if !self.read_data_type(data_type as u32) || !self.read_bits(&mut raw, 32) {
            return false;
        }
        *out = u32::from_le_bytes(raw);
        true
    }

    /// `value` clamped to `lo..=hi`, as `value - lo` in the range's bits.
    /// With `tagged` set, a type-checked buffer also gets the tag and both
    /// ends of the range (the original's fourth argument).
    ///
    /// Original: `bdBitBuffer::writeRangedUInt32` at 0x821AFF20
    pub fn write_ranged_u32(&mut self, value: u32, lo: u32, hi: u32, tagged: bool) {
        if tagged {
            if self.type_checked {
                self.write_ranged_u32(DataType::RangedUInt32 as u32, 0, 31, false);
            }
            if self.type_checked {
                self.write_u32(lo);
                self.write_u32(hi);
            }
        }
        let bits = range_bits(hi.wrapping_sub(lo));
        let clamped = if value > hi {
            hi
        } else if value < lo {
            lo
        } else {
            value
        };
        self.write_bits(&clamped.wrapping_sub(lo).to_le_bytes(), bits);
    }

    /// The signed version of [`write_ranged_u32`](Self::write_ranged_u32);
    /// a type-checked buffer always gets the tag and the range.
    ///
    /// Original: `bdBitBuffer::writeRangedInt32` at 0x821B0088
    pub fn write_ranged_i32(&mut self, value: i32, lo: i32, hi: i32) {
        if self.type_checked {
            self.write_ranged_u32(DataType::RangedInt32 as u32, 0, 31, false);
        }
        if self.type_checked {
            self.write_i32(lo);
            self.write_i32(hi);
        }
        let bits = range_bits(hi.wrapping_sub(lo) as u32);
        let clamped = if value > hi {
            hi
        } else if value < lo {
            lo
        } else {
            value
        };
        self.write_bits(&(clamped.wrapping_sub(lo) as u32).to_le_bytes(), bits);
    }

    /// `value` clamped to `lo..=hi` and quantized to steps of `precision`:
    /// `trunc((value - lo) / precision + 0.5)` in as many bits as the
    /// number of steps needs. With more than 2³² steps the plain float goes
    /// out instead (the original logs a warning). All in single precision,
    /// as the original computes it: `1 / precision` once, then multiplies.
    ///
    /// Original: `bdBitBuffer::writeRangedFloat32` at 0x821B01C8
    // `!(a <= b)` on purpose: NaN takes the same branch as on the PowerPC.
    #[allow(clippy::neg_cmp_op_on_partial_ord)]
    pub fn write_ranged_f32(&mut self, value: f32, lo: f32, hi: f32, precision: f32) {
        if self.type_checked {
            self.write_ranged_u32(DataType::RangedFloat32 as u32, 0, 31, false);
        }
        if self.type_checked {
            self.write_f32(lo);
            self.write_f32(hi);
            self.write_f32(precision);
        }
        let inverse = 1.0 / precision.abs();
        let steps = (hi - lo) * inverse;
        if !(steps <= RANGED_FLOAT_LIMIT) {
            self.write_f32(value); // also when the count is NaN
            return;
        }
        let bits = range_bits(fctidz(f64::from(steps)) as u32);
        let clamped = if value > hi {
            hi
        } else if value >= lo {
            value
        } else {
            lo // also NaN
        };
        let mut step = fmadds(clamped - lo, inverse, 0.5);
        if !(step <= steps) {
            step = steps;
        }
        self.write_bits(&(fctidz(f64::from(step)) as u32).to_le_bytes(), bits);
    }

    /// Reads what [`write_ranged_u32`](Self::write_ranged_u32) wrote, into
    /// `out` (clamped to the range: a range that isn't a power of two minus
    /// one has bit patterns past `hi`). `out` stays as it was when the read
    /// fails. A tagged read that finds another range only logs it.
    ///
    /// Original: `bdBitBuffer::readRangedUInt32` at 0x821AF1E0
    pub fn read_ranged_u32(&mut self, out: &mut u32, lo: u32, hi: u32, tagged: bool) -> bool {
        if tagged {
            let mut ok = self.read_data_type(DataType::RangedUInt32 as u32);
            if self.type_checked {
                let (mut read_lo, mut read_hi) = (0, 0);
                ok = ok && self.read_u32(&mut read_lo) && self.read_u32(&mut read_hi);
            }
            if !ok {
                return false;
            }
        }
        let mut raw = [0u8; 4];
        if !self.read_bits(&mut raw, range_bits(hi.wrapping_sub(lo))) {
            return false;
        }
        let value = u32::from_le_bytes(raw).wrapping_add(lo);
        *out = if value > hi {
            hi
        } else if value < lo {
            lo
        } else {
            value
        };
        true
    }

    /// The signed version of [`read_ranged_u32`](Self::read_ranged_u32),
    /// for [`write_ranged_i32`](Self::write_ranged_i32).
    ///
    /// Original: `bdBitBuffer::readRangedInt32` at 0x821AF430
    pub fn read_ranged_i32(&mut self, out: &mut i32, lo: i32, hi: i32) -> bool {
        let mut ok = self.read_data_type(DataType::RangedInt32 as u32);
        if self.type_checked {
            let (mut read_lo, mut read_hi) = (0, 0);
            ok = ok && self.read_i32(&mut read_lo) && self.read_i32(&mut read_hi);
        }
        if !ok {
            return false;
        }
        let mut raw = [0u8; 4];
        if !self.read_bits(&mut raw, range_bits(hi.wrapping_sub(lo) as u32)) {
            return false;
        }
        let value = (u32::from_le_bytes(raw) as i32).wrapping_add(lo);
        *out = if value > hi {
            hi
        } else if value < lo {
            lo
        } else {
            value
        };
        true
    }

    /// Reads what [`write_ranged_f32`](Self::write_ranged_f32) wrote:
    /// `step * precision + lo` (fused), clamped to the range; `out` stays as
    /// it was when the read fails.
    ///
    /// Note: the reader counts the steps as `(hi - lo) / precision` (one
    /// division) and the writer as `(hi - lo) * (1 / precision)`; the two can
    /// round differently, and for a range where that changes the number of
    /// bits the console's reader and writer disagree. Kept as the console does it.
    ///
    /// Original: `bdBitBuffer::readRangedFloat32` at 0x821AF670
    // `!(a <= b)` on purpose: NaN takes the same branch as on the PowerPC.
    #[allow(clippy::neg_cmp_op_on_partial_ord)]
    pub fn read_ranged_f32(&mut self, out: &mut f32, lo: f32, hi: f32, precision: f32) -> bool {
        let mut ok = self.read_data_type(DataType::RangedFloat32 as u32);
        if self.type_checked {
            let (mut read_lo, mut read_hi, mut read_precision) = (0.0, 0.0, 0.0);
            ok = ok
                && self.read_f32(&mut read_lo)
                && self.read_f32(&mut read_hi)
                && self.read_f32(&mut read_precision);
        }
        if !ok {
            return false;
        }
        let step_size = precision.abs();
        let steps = (hi - lo) / step_size;
        let value = if !(steps <= RANGED_FLOAT_LIMIT) {
            let mut value = *out;
            if !self.read_f32(&mut value) {
                return false;
            }
            value
        } else {
            let mut raw = [0u8; 4];
            let bits = range_bits(fctidz(f64::from(steps)) as u32);
            if !self.read_bits(&mut raw, bits) {
                return false;
            }
            fmadds(u32::from_le_bytes(raw) as f32, step_size, lo)
        };
        *out = if !(value <= hi) {
            hi // also NaN
        } else if !(value >= lo) {
            lo
        } else {
            value
        };
        true
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn bits_go_in_low_bit_first() {
        let mut b = BitBuffer::new(false);
        b.write_bits(&0x1ABCu32.to_le_bytes(), 13);
        b.write_ranged_u32(300, 0, 1000, true);
        // Header bit 0, then 13 bits of 0x1ABC, then 300 in 10 bits (checked
        // against the original in the emulator).
        assert_eq!(b.data, [0x78, 0x35, 0x4B]);
        assert_eq!((b.write_position, b.written, b.read_position), (24, 24, 1));
        let mut raw = [0u8; 2];
        assert!(b.read_bits(&mut raw, 13));
        assert_eq!(u16::from_le_bytes(raw), 0x1ABC);
        let mut value = 0;
        assert!(b.read_ranged_u32(&mut value, 0, 1000, true));
        assert_eq!(value, 300);
        assert!(!b.read_bits(&mut raw, 1));
        assert!(b.read_failed);
    }

    #[test]
    fn ranged_floats_round_trip_to_a_step() {
        let mut b = BitBuffer::new(false);
        for v in [-8191.0f32, -0.4, 0.6, 1234.5, 8191.0, 9000.0] {
            b.write_ranged_f32(v, -8191.0, 8191.0, 1.0);
        }
        for v in [-8191.0f32, 0.0, 1.0, 1235.0, 8191.0, 8191.0] {
            let mut out = 0.0;
            assert!(b.read_ranged_f32(&mut out, -8191.0, 8191.0, 1.0));
            assert_eq!(out, v);
        }
        assert_eq!(b.written, 1 + 6 * 14);
    }
}
