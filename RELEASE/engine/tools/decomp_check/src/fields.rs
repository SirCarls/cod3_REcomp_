//! Requests for `cod3_game::net::fields` (the multiplayer program's message
//! fields) and the bit buffer readers they use, and for `math::yaw`.
//!
//! Argument 0 is the bit buffer's state, as in `bit_buffer.rs`. The read
//! status (a global byte) comes as a 1-byte buffer. The normals table that
//! direction bytes index comes last, as the game's 1,944 bytes (the checker
//! reads it from the player's copy of the program).

use cod3_game::math::yaw;
use cod3_game::net::fields::{self, byte_directions_from_be_bytes, ByteDirections};

use crate::bit_buffer::{ok, on_buffer};
use crate::{floats, store_floats, Answer, Args, Ret};

fn table(args: &Args, i: usize) -> ByteDirections {
    let bytes = args
        .bytes(i)
        .try_into()
        .expect("the normals table is 1,944 bytes");
    byte_directions_from_be_bytes(bytes)
}

/// A reader of three floats: out in argument 1, the status in argument 2.
fn read_vec3(
    args: &mut Args,
    read: fn(&mut cod3_game::net::bit_buffer::BitBuffer, &mut [f32; 3], &mut bool) -> bool,
) -> Answer {
    on_buffer(args, |b, a| {
        let mut out: [f32; 3] = floats(a.buffer(1).expect("an output"));
        let mut status = a.buffer(2).expect("the status")[0] != 0;
        let result = read(b, &mut out, &mut status);
        store_floats(a.buffer(1).expect("an output"), &out);
        a.buffer(2).expect("the status")[0] = u8::from(status);
        ok(result)
    })
}

pub fn dispatch(name: &str, args: &mut Args) -> Option<Answer> {
    Some(match name {
        "bdBitBuffer::readInt8" => on_buffer(args, |b, a| {
            let out = &mut a.buffer(1).expect("an output")[0];
            let mut value = *out as i8;
            let result = b.read_i8(&mut value);
            *out = value as u8;
            ok(result)
        }),
        "bdBitBuffer::readInt16" => on_buffer(args, |b, a| {
            let out = a.buffer(1).expect("an output");
            let mut value = i16::from_be_bytes([out[0], out[1]]);
            let result = b.read_i16(&mut value);
            out[..2].copy_from_slice(&value.to_be_bytes());
            ok(result)
        }),
        "bdBitBuffer::readUInt16" => on_buffer(args, |b, a| {
            let out = a.buffer(1).expect("an output");
            let mut value = u16::from_be_bytes([out[0], out[1]]);
            let result = b.read_u16(&mut value);
            out[..2].copy_from_slice(&value.to_be_bytes());
            ok(result)
        }),
        "bdBitBuffer::readUInt64" => on_buffer(args, |b, a| {
            let out = a.buffer(1).expect("an output");
            let mut value = u64::from_be_bytes(out[..8].try_into().expect("8 bytes"));
            let result = b.read_u64(&mut value);
            out[..8].copy_from_slice(&value.to_be_bytes());
            ok(result)
        }),
        "vectoyaw" => Ok(Ret::F32(yaw::vector_to_yaw(&args.floats(0)))),
        "vectoyaw_signed" => Ok(Ret::F32(yaw::vector_to_yaw_signed(&args.floats(0)))),
        "DirToByte" => {
            let dir: Option<[f32; 3]> = match args.bytes(0) {
                [] => None,
                bytes => Some(floats(bytes)),
            };
            let index = fields::dir_to_byte(dir.as_ref(), &table(args, 1));
            Ok(Ret::Int(index.into()))
        }
        "MSGBB_WriteAngle" => on_buffer(args, |b, a| {
            fields::write_angle(b, a.f32(1));
            Ok(Ret::Void)
        }),
        "MSGBB_WriteAngles2" | "MSGBB_WriteAngles2_copy" => on_buffer(args, |b, a| {
            fields::write_angles2(b, &a.floats(1));
            Ok(Ret::Void)
        }),
        "MSGBB_WriteAngles3" | "MSGBB_WriteAngles3_copy" => on_buffer(args, |b, a| {
            fields::write_angles3(b, &a.floats(1));
            Ok(Ret::Void)
        }),
        "MSGBB_WriteVector" => on_buffer(args, |b, a| {
            fields::write_vector(b, &a.floats(1));
            Ok(Ret::Void)
        }),
        "MSGBB_WriteDir" | "MSGBB_WriteDir_copy" => on_buffer(args, |b, a| {
            fields::write_dir(b, &a.floats(1), &table(a, 2));
            Ok(Ret::Void)
        }),
        "MSGBB_WriteOrigin" | "MSGBB_WriteOrigin_copy" => on_buffer(args, |b, a| {
            fields::write_origin(b, &a.floats(1));
            Ok(Ret::Void)
        }),
        "MSGBB_WriteOriginInt" => on_buffer(args, |b, a| {
            fields::write_origin_int(b, &a.floats(1));
            Ok(Ret::Void)
        }),
        "MSGBB_WriteCoordDelta" => on_buffer(args, |b, a| {
            fields::write_coord_delta(b, a.f32(1), a.f32(2));
            Ok(Ret::Void)
        }),
        "MSGBB_WriteVelocity" => on_buffer(args, |b, a| {
            fields::write_velocity(b, &a.floats(1));
            Ok(Ret::Void)
        }),
        "MSGBB_ReadAngles2" => read_vec3(args, fields::read_angles2),
        "MSGBB_ReadAngles3" => read_vec3(args, fields::read_angles3),
        "MSGBB_ReadVector" => read_vec3(args, fields::read_vector),
        "MSGBB_ReadOrigin" => read_vec3(args, fields::read_origin),
        "MSGBB_ReadOriginInt" => read_vec3(args, fields::read_origin_int),
        "MSGBB_ReadDir" => on_buffer(args, |b, a| {
            let normals = table(a, 2);
            let mut out: [f32; 3] = floats(a.buffer(1).expect("an output"));
            let result = fields::read_dir(b, &mut out, &normals);
            store_floats(a.buffer(1).expect("an output"), &out);
            ok(result)
        }),
        "MSGBB_ReadCoordDelta" => on_buffer(args, |b, a| {
            let base = a.f32(1);
            let [mut out] = floats::<1>(a.buffer(2).expect("an output"));
            let mut status = a.buffer(3).expect("the status")[0] != 0;
            let result = fields::read_coord_delta(b, base, &mut out, &mut status);
            store_floats(a.buffer(2).expect("an output"), &[out]);
            a.buffer(3).expect("the status")[0] = u8::from(status);
            ok(result)
        }),
        _ => return None,
    })
}
