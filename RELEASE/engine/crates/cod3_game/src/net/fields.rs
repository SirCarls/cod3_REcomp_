//! Message fields in the multiplayer program: how positions, angles,
//! directions and velocities go into a [`BitBuffer`] (the functions the
//! symbol map calls `MSGBB_*`, after Quake III's `MSG_Write*`/`MSG_Read*`).
//!
//! | Field | Bits | Encoding |
//! |---|---|---|
//! | position | 3 × 14 | each coordinate a ranged float in [-8191, 8191], precision 1 (rounded to nearest) |
//! | integer position | 3 × 14 | each coordinate truncated, a ranged int in the same range |
//! | coordinate delta | 11 | `value - base`, a ranged float in [-512, 512], precision 1 |
//! | angle | 16 | `(int)(angle * 65536/360)`, Quake III's `ANGLE2SHORT` |
//! | direction | 8 | the nearest of Quake's 162 unit normals (`DirToByte`) |
//! | float vector | 3 × 32 | the floats' bits |
//! | velocity | 1, then 8 + 12 if set, then 12 | "moves horizontally"; the direction of movement in 1/255 turns and the horizontal speed in [-1024, 1024]; the vertical speed in [-1024, 1024] |
//!
//! Game messages aren't type-checked, so only the bits above go out (a
//! type-checked buffer adds each value's tag, as [`BitBuffer`] says).
//!
//! The original passes the buffer as a `bdReference<bdBitBuffer>` by value
//! and drops that reference at the end (0x820DB148); here it is `&mut BitBuffer`.
//!
//! **The read status.** The readers share one status between calls, a
//! global byte (0x82B793E6, the `ok` argument here): whether the last reads
//! got everything. Most readers start it over, and stop reading at the
//! first failure; [`read_coord_delta`] skips its read when it is already
//! false. A value whose read failed is what the original's stack holds: the
//! one read before it in the same call, or 0 for the first. On the console
//! that 0 is whatever the stack held before; the port's 0 is what a fresh
//! stack (and the checker) gives.

use crate::math::angles::{angle_to_short, Vec3, SHORT_TO_ANGLE};
use crate::math::yaw::vector_to_yaw;
use crate::net::bit_buffer::{BitBuffer, DataType};
use crate::ppc::{fctiwz, fmadds};

/// The largest coordinate a position can carry (0x45FFF800). The smallest
/// is its negative, a global the program sets when it starts (0x82C9DD00).
pub const COORD_MAX: f32 = 8191.0;
/// The largest change a coordinate delta can carry.
pub const COORD_DELTA_MAX: f32 = 512.0;
/// The largest speed a velocity can carry, horizontally or vertically.
pub const SPEED_MAX: f32 = 1024.0;
/// How many unit normals a direction byte can name.
pub const BYTE_DIRECTION_COUNT: usize = 162;
/// 1/360 as the game stores it (0x3B360B61).
const TURNS_PER_DEGREE: f32 = 1.0 / 360.0;
/// 1/255 as the game stores it (0x3B808081): a direction byte's step, in turns.
const TURNS_PER_DIRECTION_STEP: f32 = 1.0 / 255.0;
/// π/180 as the game stores it (0x3C8EFA35).
const RADIANS_PER_DEGREE: f32 = std::f32::consts::PI / 180.0;

/// The game's four-lane sine and cosine (0x820B3920): `lanes(&[a; 4])`
/// gives sin(a) in lane 0 and cos(a) in lane 1 (it adds 3π/2 to lanes 0
/// and 2 and evaluates one series on all four). It is made of the Xbox
/// 360's VMX128 vector instructions, which the checker's emulator can't run
/// yet, so it isn't ported: callers pass it in, and the checks replace it
/// on both sides with the same stand-in.
pub type SinCosLanes = fn(&[f32; 4]) -> [f32; 4];

/// Quake's 162 unit normals (`bytedirs`), which a direction byte indexes.
///
/// The table isn't part of this crate: it is read from the player's own
/// copy of the game (the multiplayer program keeps it at 0x825FC698, as
/// 162 × 3 big-endian floats).
pub type ByteDirections = [Vec3; BYTE_DIRECTION_COUNT];

/// The table from the game's bytes (162 × 3 big-endian floats, 1,944 bytes).
pub fn byte_directions_from_be_bytes(bytes: &[u8; BYTE_DIRECTION_COUNT * 12]) -> ByteDirections {
    let mut table = [[0.0; 3]; BYTE_DIRECTION_COUNT];
    for (i, normal) in table.iter_mut().enumerate() {
        for (j, c) in normal.iter_mut().enumerate() {
            let at = i * 12 + j * 4;
            *c = f32::from_be_bytes([bytes[at], bytes[at + 1], bytes[at + 2], bytes[at + 3]]);
        }
    }
    table
}

// --- writers -------------------------------------------------------------

/// One angle in 16 bits.
///
/// Original: `MSGBB_WriteAngle` at 0x823D7540
pub fn write_angle(buffer: &mut BitBuffer, angle: f32) {
    buffer.write_i16(angle_to_short(angle) as i16);
}

/// Pitch and yaw (`angles[0]`, `angles[1]`), 16 bits each.
///
/// Original: `MSGBB_WriteAngles2` at 0x823D7768 (an identical copy at 0x823D7698)
pub fn write_angles2(buffer: &mut BitBuffer, angles: &Vec3) {
    write_angle(buffer, angles[0]);
    write_angle(buffer, angles[1]);
}

/// All three angles, 16 bits each.
///
/// Original: `MSGBB_WriteAngles3` at 0x823D7A60 (an identical copy at 0x823D7B78)
pub fn write_angles3(buffer: &mut BitBuffer, angles: &Vec3) {
    for &angle in angles {
        write_angle(buffer, angle);
    }
}

/// Three floats, 32 bits each.
///
/// Original: `MSGBB_WriteVector` at 0x823D78F0
pub fn write_vector(buffer: &mut BitBuffer, v: &Vec3) {
    for &c in v {
        buffer.write_f32(c);
    }
}

/// The index of the normal nearest to `dir` (the largest dot product; 0
/// when none is above 0, or for no direction at all).
///
/// Original: `DirToByte` at 0x82545998
pub fn dir_to_byte(dir: Option<&Vec3>, table: &ByteDirections) -> u8 {
    let Some(dir) = dir else {
        return 0;
    };
    let mut best = 0;
    let mut best_dot = 0.0;
    for (i, normal) in table.iter().enumerate() {
        let dot = fmadds(
            normal[2],
            dir[2],
            fmadds(normal[0], dir[0], normal[1] * dir[1]),
        );
        if dot > best_dot {
            best_dot = dot;
            best = i as u8;
        }
    }
    best
}

/// A direction as one byte: the nearest of the 162 normals.
///
/// Original: `MSGBB_WriteDir` at 0x823D7D20 (an identical copy at 0x823D7D88)
pub fn write_dir(buffer: &mut BitBuffer, dir: &Vec3, table: &ByteDirections) {
    write_byte(buffer, dir_to_byte(Some(dir), table));
}

/// Eight bits with the `Int8` tag (the original's inline `writeInt8`).
pub(crate) fn write_byte(buffer: &mut BitBuffer, byte: u8) {
    buffer.write_data_type(DataType::Int8 as u32);
    buffer.write_bits(&[byte], 8);
}

/// A position: each coordinate a ranged float in [-8191, 8191] at precision 1.
///
/// Original: `MSGBB_WriteOrigin` at 0x823D8228 (an identical copy at 0x823D80C8)
pub fn write_origin(buffer: &mut BitBuffer, origin: &Vec3) {
    for &c in origin {
        buffer.write_ranged_f32(c, -COORD_MAX, COORD_MAX, 1.0);
    }
}

/// A position with each coordinate truncated to a whole number (a ranged
/// int in [-8191, 8191]).
///
/// Original: `MSGBB_WriteOriginInt` at 0x823D7F30
pub fn write_origin_int(buffer: &mut BitBuffer, origin: &Vec3) {
    let lo = fctiwz(f64::from(-COORD_MAX));
    for &c in origin {
        buffer.write_ranged_i32(fctiwz(f64::from(c)), lo, COORD_MAX as i32);
    }
}

/// How far a coordinate moved from `base`, in [-512, 512] at precision 1.
///
/// Original: `MSGBB_WriteCoordDelta` at 0x823D81D0
pub fn write_coord_delta(buffer: &mut BitBuffer, base: f32, value: f32) {
    buffer.write_ranged_f32(value - base, -COORD_DELTA_MAX, COORD_DELTA_MAX, 1.0);
}

/// A velocity: whether it moves horizontally; if it does, the direction in
/// 1/255 turns (from the yaw rounded to 16 bits first) and the horizontal
/// speed, truncated to a whole number and capped at 1024; then the
/// vertical speed. The speeds are ranged floats in [-1024, 1024], precision 1.
///
/// Original: `MSGBB_WriteVelocity` at 0x823F20C0
pub fn write_velocity(buffer: &mut BitBuffer, velocity: &Vec3) {
    let (vx, vy) = (velocity[0], velocity[1]);
    let speed = fmadds(vy, vy, vx * vx).sqrt();
    let mut dir = [vx, vy, 0.0];
    if speed != 0.0 {
        let inverse = 1.0 / speed;
        dir = [inverse * vx, inverse * vy, inverse * 0.0];
    }
    let mut whole_speed = fctiwz(f64::from(speed));
    if whole_speed as f32 > SPEED_MAX {
        whole_speed = SPEED_MAX as i32;
    }
    if vx == 0.0 && vy == 0.0 {
        buffer.write_bool(false);
    } else {
        buffer.write_bool(true);
        let yaw = f32::from(angle_to_short(vector_to_yaw(&dir))) * SHORT_TO_ANGLE;
        let turns = yaw * TURNS_PER_DEGREE;
        write_byte(buffer, fctiwz(f64::from(turns * 255.0)) as u8);
        buffer.write_ranged_f32(whole_speed as f32, -SPEED_MAX, SPEED_MAX, 1.0);
    }
    buffer.write_ranged_f32(velocity[2], -SPEED_MAX, SPEED_MAX, 1.0);
}

// --- readers -------------------------------------------------------------

/// Reads three values with `read`, stopping at the first failure; each
/// value is what `temp` holds after its read, as in the original.
fn read_three<T: Copy>(
    buffer: &mut BitBuffer,
    out: &mut Vec3,
    ok: &mut bool,
    mut temp: T,
    mut read: impl FnMut(&mut BitBuffer, &mut T) -> bool,
    to_float: impl Fn(T) -> f32,
) -> bool {
    let mut good = true;
    for c in out.iter_mut() {
        good = good && read(buffer, &mut temp);
        *ok = good;
        *c = to_float(temp);
    }
    good
}

/// Pitch and yaw from 16 bits each; `out[2]` becomes 0.
///
/// Original: `MSGBB_ReadAngles2` at 0x823D75C8
pub fn read_angles2(buffer: &mut BitBuffer, out: &mut Vec3, ok: &mut bool) -> bool {
    let mut short = 0i16;
    let first = buffer.read_i16(&mut short);
    *ok = first;
    out[0] = f32::from(short) * SHORT_TO_ANGLE;
    let second = first && buffer.read_i16(&mut short);
    *ok = second;
    out[1] = f32::from(short) * SHORT_TO_ANGLE;
    out[2] = 0.0;
    second
}

/// All three angles from 16 bits each.
///
/// Original: `MSGBB_ReadAngles3` at 0x823D7950
pub fn read_angles3(buffer: &mut BitBuffer, out: &mut Vec3, ok: &mut bool) -> bool {
    read_three(
        buffer,
        out,
        ok,
        0i16,
        |b, s| b.read_i16(s),
        |s| f32::from(s) * SHORT_TO_ANGLE,
    )
}

/// Three floats from 32 bits each.
///
/// Original: `MSGBB_ReadVector` at 0x823D7838
pub fn read_vector(buffer: &mut BitBuffer, out: &mut Vec3, ok: &mut bool) -> bool {
    read_three(buffer, out, ok, 0.0f32, |b, v| b.read_f32(v), |v| v)
}

/// A direction from one byte; a byte past the table gives a zero vector.
/// (This one leaves the read status alone.)
///
/// Original: `MSGBB_ReadDir` at 0x823D7C90
pub fn read_dir(buffer: &mut BitBuffer, out: &mut Vec3, table: &ByteDirections) -> bool {
    let mut byte = 0i8;
    let read = buffer.read_i8(&mut byte);
    *out = table.get(byte as u8 as usize).copied().unwrap_or([0.0; 3]);
    read
}

/// A position: three ranged floats in [-8191, 8191] at precision 1, read
/// straight into `out`.
///
/// Original: `MSGBB_ReadOrigin` at 0x823D7FF8
pub fn read_origin(buffer: &mut BitBuffer, out: &mut Vec3, ok: &mut bool) -> bool {
    let mut good = true;
    for c in out.iter_mut() {
        good = good && buffer.read_ranged_f32(c, -COORD_MAX, COORD_MAX, 1.0);
        *ok = good;
    }
    good
}

/// A position of whole numbers (ranged ints in [-8191, 8191]). Before the
/// first read the original's temporary holds the range's low end, so that
/// is what a failed first read gives.
///
/// Original: `MSGBB_ReadOriginInt` at 0x823D7DF0
pub fn read_origin_int(buffer: &mut BitBuffer, out: &mut Vec3, ok: &mut bool) -> bool {
    let lo = fctiwz(f64::from(-COORD_MAX));
    let hi = COORD_MAX as i32;
    read_three(
        buffer,
        out,
        ok,
        lo,
        |b, v| b.read_ranged_i32(v, lo, hi),
        |v| v as f32,
    )
}

/// A velocity, as [`write_velocity`] puts it: zero unless it moves
/// horizontally; then the direction byte becomes a unit vector (through
/// `sin_cos`) scaled by the horizontal speed; the vertical speed goes in
/// `out[2]`. Each part is read only when the ones before it were.
///
/// Original: `MSGBB_ReadVelocity` at 0x823EF2B0
pub fn read_velocity(
    buffer: &mut BitBuffer,
    out: &mut Vec3,
    ok: &mut bool,
    sin_cos: SinCosLanes,
) -> bool {
    let mut moving = false;
    let mut good = buffer.read_bool(&mut moving);
    *ok = good;
    if moving {
        let mut byte = 0i8;
        good = good && buffer.read_i8(&mut byte);
        *ok = good;
        let angle = f32::from(byte) * TURNS_PER_DIRECTION_STEP * 360.0 * RADIANS_PER_DEGREE;
        let lanes = sin_cos(&[angle; 4]);
        *out = [lanes[1], lanes[0], 0.0];
        if good {
            let mut speed = 0.0;
            good = buffer.read_ranged_f32(&mut speed, -SPEED_MAX, SPEED_MAX, 1.0);
            if good {
                out[0] *= speed;
                out[1] *= speed;
                out[2] *= speed;
            }
        }
        *ok = good;
    } else {
        *out = [0.0; 3];
    }
    if good {
        let mut vertical = 0.0;
        good = buffer.read_ranged_f32(&mut vertical, -SPEED_MAX, SPEED_MAX, 1.0);
        if good {
            out[2] = vertical;
        }
    }
    *ok = good;
    good
}

/// A coordinate delta added to `base`: `out` becomes the value read plus
/// `base`. When the status is already false nothing is read, and `out`
/// still gets `base` added.
///
/// Original: `MSGBB_ReadCoordDelta` at 0x823D8140
pub fn read_coord_delta(buffer: &mut BitBuffer, base: f32, out: &mut f32, ok: &mut bool) -> bool {
    let read = *ok && buffer.read_ranged_f32(out, -COORD_DELTA_MAX, COORD_DELTA_MAX, 1.0);
    *out += base;
    *ok = read;
    read
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn constants_have_the_games_bits() {
        assert_eq!(COORD_MAX.to_bits(), 0x45FF_F800);
        assert_eq!(TURNS_PER_DEGREE.to_bits(), 0x3B36_0B61);
        assert_eq!(TURNS_PER_DIRECTION_STEP.to_bits(), 0x3B80_8081);
        assert_eq!(RADIANS_PER_DEGREE.to_bits(), 0x3C8E_FA35);
    }

    #[test]
    fn fields_come_back() {
        let mut table = [[0.0; 3]; BYTE_DIRECTION_COUNT];
        table[7] = [0.0, 0.0, 1.0];
        table[9] = [1.0, 0.0, 0.0];
        let mut buffer = BitBuffer::new(false);
        write_origin(&mut buffer, &[100.4, -2000.6, 8191.0]);
        write_angles2(&mut buffer, &[45.0, 90.0, 0.0]);
        write_dir(&mut buffer, &[0.9, 0.1, 0.0], &table);
        write_coord_delta(&mut buffer, 10.0, 25.0);
        // 42 + 32 + 8 + 11 bits after the header bit.
        assert_eq!(buffer.written, 1 + 42 + 32 + 8 + 11);

        let mut ok = true;
        let mut origin = [0.0; 3];
        assert!(read_origin(&mut buffer, &mut origin, &mut ok));
        assert_eq!(origin, [100.0, -2001.0, 8191.0]);
        let mut angles = [9.0; 3];
        assert!(read_angles2(&mut buffer, &mut angles, &mut ok));
        assert_eq!(angles, [45.0, 90.0, 0.0]);
        let mut dir = [0.0; 3];
        assert!(read_dir(&mut buffer, &mut dir, &table));
        assert_eq!(dir, [1.0, 0.0, 0.0]);
        let mut coord = 0.0;
        assert!(read_coord_delta(&mut buffer, 10.0, &mut coord, &mut ok));
        assert_eq!(coord, 25.0);
        // Past the end: the status goes false, and the next delta reads
        // nothing but still adds its base.
        assert!(!read_coord_delta(&mut buffer, 1.0, &mut coord, &mut ok));
        assert!(!ok);
        let mut next = 5.0;
        assert!(!read_coord_delta(&mut buffer, 2.0, &mut next, &mut ok));
        assert_eq!(next, 7.0);
    }
}
