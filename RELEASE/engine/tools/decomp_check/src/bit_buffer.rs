//! Requests for `cod3_game::net::bit_buffer` (Demonware's bit buffer).
//!
//! The buffer travels as a `b:` argument holding its whole state (see
//! `tools/decomp/specs/net_bit_buffer.py`): type-checked, read-failed,
//! write position, highest bit written, read position (big-endian u32s),
//! then the bytes. It comes back in the reply in the same form.

use cod3_game::net::bit_buffer::{high_bit_number, BitBuffer};

use crate::{Answer, Args, Ret};

const HEADER: usize = 14;

fn word(state: &[u8], at: usize) -> u32 {
    u32::from_be_bytes([state[at], state[at + 1], state[at + 2], state[at + 3]])
}

fn decode(state: &[u8]) -> BitBuffer {
    BitBuffer {
        type_checked: state[0] != 0,
        read_failed: state[1] != 0,
        write_position: word(state, 2),
        written: word(state, 6),
        read_position: word(state, 10),
        data: state[HEADER..].to_vec(),
    }
}

fn encode(buffer: &BitBuffer) -> Vec<u8> {
    let mut out = vec![u8::from(buffer.type_checked), u8::from(buffer.read_failed)];
    for value in [buffer.write_position, buffer.written, buffer.read_position] {
        out.extend_from_slice(&value.to_be_bytes());
    }
    out.extend_from_slice(&buffer.data);
    out
}

/// Runs `f` on the buffer in argument 0 and puts its new state back.
pub(crate) fn on_buffer<R>(args: &mut Args, f: impl FnOnce(&mut BitBuffer, &mut Args) -> R) -> R {
    on_buffer_at(args, 0, f)
}

/// [`on_buffer`] for a buffer in argument `i`.
pub(crate) fn on_buffer_at<R>(
    args: &mut Args,
    i: usize,
    f: impl FnOnce(&mut BitBuffer, &mut Args) -> R,
) -> R {
    let state = args.buffer(i).expect("a bit buffer").clone();
    let mut buffer = decode(&state);
    let result = f(&mut buffer, args);
    *args.buffer(i).expect("a bit buffer") = encode(&buffer);
    result
}

/// A 4-byte output argument, as the guest keeps it (big-endian).
fn out_word(args: &mut Args, i: usize) -> u32 {
    word(args.buffer(i).expect("an output"), 0)
}

fn set_out_word(args: &mut Args, i: usize, value: u32) {
    args.buffer(i).expect("an output")[..4].copy_from_slice(&value.to_be_bytes());
}

pub(crate) fn ok(result: bool) -> Answer {
    Ok(Ret::Int(result.into()))
}

pub fn dispatch(name: &str, args: &mut Args) -> Option<Answer> {
    let int = |args: &Args, i| args.int(i);
    Some(match name {
        "bdBitOperations::highBitNumber" => {
            Ok(Ret::Int(high_bit_number(args.int(0) as u32).into()))
        }
        "bdBitBuffer::writeBits" => on_buffer(args, |b, a| {
            let source = a.bytes(1).to_vec();
            b.write_bits(&source, a.int(2) as u32);
            Ok(Ret::Void)
        }),
        "bdBitBuffer::readBits" => on_buffer(args, |b, a| {
            let bits = a.int(2) as u32;
            let destination = a.buffer(1).expect("a destination");
            ok(b.read_bits(destination, bits))
        }),
        "bdBitBuffer::writeDataType" => on_buffer(args, |b, a| {
            b.write_data_type(int(a, 1) as u32);
            Ok(Ret::Void)
        }),
        "bdBitBuffer::readDataType" => {
            on_buffer(args, |b, a| ok(b.read_data_type(int(a, 1) as u32)))
        }
        "bdBitBuffer::writeBool" => on_buffer(args, |b, a| {
            b.write_bool(int(a, 1) as u8 != 0);
            Ok(Ret::Void)
        }),
        "bdBitBuffer::writeInt16" => on_buffer(args, |b, a| {
            b.write_i16(int(a, 1) as i16);
            Ok(Ret::Void)
        }),
        "bdBitBuffer::writeUInt16" => on_buffer(args, |b, a| {
            b.write_u16(int(a, 1) as u16);
            Ok(Ret::Void)
        }),
        "bdBitBuffer::writeInt32" => on_buffer(args, |b, a| {
            b.write_i32(int(a, 1) as i32);
            Ok(Ret::Void)
        }),
        "bdBitBuffer::writeUInt32" => on_buffer(args, |b, a| {
            b.write_u32(int(a, 1) as u32);
            Ok(Ret::Void)
        }),
        "bdBitBuffer::writeUInt64" => on_buffer(args, |b, a| {
            b.write_u64(int(a, 1) as u64);
            Ok(Ret::Void)
        }),
        "bdBitBuffer::writeFloat32" => on_buffer(args, |b, a| {
            b.write_f32(a.f32(1));
            Ok(Ret::Void)
        }),
        "bdBitBuffer::readBool" => on_buffer(args, |b, a| {
            let out = &mut a.buffer(1).expect("an output")[0];
            let mut value = *out != 0;
            let result = b.read_bool(&mut value);
            if result {
                *out = u8::from(value);
            }
            ok(result)
        }),
        "bdBitBuffer::readUInt8" => on_buffer(args, |b, a| {
            let out = &mut a.buffer(1).expect("an output")[0];
            ok(b.read_u8(out))
        }),
        "bdBitBuffer::readUInt32" => on_buffer(args, |b, a| {
            let mut value = out_word(a, 1);
            let result = b.read_u32(&mut value);
            set_out_word(a, 1, value);
            ok(result)
        }),
        "bdBitBuffer::readInt32" => on_buffer(args, |b, a| {
            let mut value = out_word(a, 1) as i32;
            let result = b.read_i32(&mut value);
            set_out_word(a, 1, value as u32);
            ok(result)
        }),
        "bdBitBuffer::readFloat32" => on_buffer(args, |b, a| {
            let mut value = f32::from_bits(out_word(a, 1));
            let result = b.read_f32(&mut value);
            set_out_word(a, 1, value.to_bits());
            ok(result)
        }),
        "bdBitBuffer::writeRangedUInt32" => on_buffer(args, |b, a| {
            let (v, lo, hi) = (int(a, 1) as u32, int(a, 2) as u32, int(a, 3) as u32);
            b.write_ranged_u32(v, lo, hi, int(a, 4) as u8 != 0);
            Ok(Ret::Void)
        }),
        "bdBitBuffer::writeRangedInt32" => on_buffer(args, |b, a| {
            b.write_ranged_i32(int(a, 1) as i32, int(a, 2) as i32, int(a, 3) as i32);
            Ok(Ret::Void)
        }),
        "bdBitBuffer::writeRangedFloat32" => on_buffer(args, |b, a| {
            b.write_ranged_f32(a.f32(1), a.f32(2), a.f32(3), a.f32(4));
            Ok(Ret::Void)
        }),
        "bdBitBuffer::readRangedUInt32" => on_buffer(args, |b, a| {
            let mut value = out_word(a, 1);
            let (lo, hi) = (int(a, 2) as u32, int(a, 3) as u32);
            let result = b.read_ranged_u32(&mut value, lo, hi, int(a, 4) as u8 != 0);
            set_out_word(a, 1, value);
            ok(result)
        }),
        "bdBitBuffer::readRangedInt32" => on_buffer(args, |b, a| {
            let mut value = out_word(a, 1) as i32;
            let result = b.read_ranged_i32(&mut value, int(a, 2) as i32, int(a, 3) as i32);
            set_out_word(a, 1, value as u32);
            ok(result)
        }),
        "bdBitBuffer::readRangedFloat32" => on_buffer(args, |b, a| {
            let mut value = f32::from_bits(out_word(a, 1));
            let result = b.read_ranged_f32(&mut value, a.f32(2), a.f32(3), a.f32(4));
            set_out_word(a, 1, value.to_bits());
            ok(result)
        }),
        _ => return None,
    })
}
