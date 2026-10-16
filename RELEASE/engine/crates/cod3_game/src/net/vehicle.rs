//! The vehicle update (message 0x4C): what a console sends about the
//! vehicles it runs (one entry of the list at `MPPeer` +0x59B0 each, on the
//! 1000 and 250 ms passes), including where they are while its players
//! drive them.
//!
//! The caller writes the entry's index first (a ranged int in [0, 20]:
//! 5 bits) and, after this, whether this console's player drives it, with
//! that player's view update ([`crate::net::view`]). In between, in bit order:
//!
//! | Field | Bits |
//! |---|---|
//! | position | 3 × 14 |
//! | velocity, then a second velocity (probably angular) | 2 × (1, then 8 + 12 if moving, then 12) |
//! | angles | 3 × 16 |
//! | four controls (probably steering, throttle, brake and a fourth): [-1, 1] at 0.05, [-1, 1] at 0.1, [0, 1] at 0.1, [0, 1] at 0.1 | 6 + 5 + 4 + 4 |
//! | motion state, 0 to 3 | 2 |
//! | seat, 0 to 3 | 2 |
//! | the entity's +0x308, -1 to 0x1004 (probably an entity number) | 13 |

use crate::math::angles::Vec3;
use crate::net::bit_buffer::BitBuffer;
use crate::net::fields::{
    read_angles3, read_origin, read_velocity, write_angles3, write_origin, write_velocity,
    SinCosLanes,
};

/// The four controls' ranges and steps: (low, high, precision).
pub const CONTROL_RANGES: [(f32, f32, f32); 4] = [
    (-1.0, 1.0, 0.05),
    (-1.0, 1.0, 0.1),
    (0.0, 1.0, 0.1),
    (0.0, 1.0, 0.1),
];
/// The motion state's and the seat's largest value.
pub const STATE_MAX: i32 = 3;
/// The range of the entity's +0x308 field.
pub const FIELD_308_RANGE: (i32, i32) = (-1, 0x1004);
/// The second control reads back as 0 inside (-0.001, 0.001) (0x3A83126F).
const CONTROL_DEAD_ZONE: f32 = 0.001;

/// What a vehicle update carries.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct VehicleUpdate {
    pub origin: Vec3,
    pub velocity: Vec3,
    pub second_velocity: Vec3,
    pub angles: Vec3,
    pub controls: [f32; 4],
    pub motion: i32,
    pub seat: i32,
    pub field_308: i32,
}

/// The seat a player has in a vehicle from its two flags (+0x215, +0x216
/// of the vehicle's +0x20C): 2 for the first, else 1 for the second, else 0.
pub fn seat_from_flags(flags: [bool; 2]) -> i32 {
    match flags {
        [true, _] => 2,
        [false, true] => 1,
        _ => 0,
    }
}

/// A vehicle's motion state as the writer works it out from its info
/// (the entity's +0x20C): 2 when +0x1F9 is set; else 1 when +0x1FA is set,
/// or when its kind (the table at 0x82B2DC90, by its +0x178) is 1 and its
/// +0x3D8 is above 0.5; else 0.
pub fn motion_state(flag_1f9: bool, flag_1fa: bool, kind_is_1: bool, value_3d8: f32) -> i32 {
    if flag_1f9 {
        2
    } else if flag_1fa || (kind_is_1 && value_3d8 > 0.5) {
        1
    } else {
        0
    }
}

/// Writes the update's fields (the parts between the caller's index and
/// "driven" bit). The original gathers the values from the vehicle's
/// entity, or from its physics body when it has one (with VMX128 loads);
/// here they come in as a [`VehicleUpdate`]. Without a physics body the
/// controls are all 0.
///
/// Original: `MPPeer::WriteVehicleUpdate` at 0x823F2288
pub fn write_vehicle_update(buffer: &mut BitBuffer, update: &VehicleUpdate) {
    write_origin(buffer, &update.origin);
    write_velocity(buffer, &update.velocity);
    write_velocity(buffer, &update.second_velocity);
    write_angles3(buffer, &update.angles);
    for (&value, &(lo, hi, precision)) in update.controls.iter().zip(&CONTROL_RANGES) {
        buffer.write_ranged_f32(value, lo, hi, precision);
    }
    buffer.write_ranged_i32(update.motion, 0, STATE_MAX);
    buffer.write_ranged_i32(update.seat, 0, STATE_MAX);
    buffer.write_ranged_i32(update.field_308, FIELD_308_RANGE.0, FIELD_308_RANGE.1);
}

/// The receiving console's entry for a vehicle (an entry of the list at
/// `MPPeer` +0x59B0; the fields an update changes).
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct VehicleEntry {
    /// +0x20.
    pub origin: Vec3,
    /// +0x30.
    pub velocity: Vec3,
    /// +0x40.
    pub second_velocity: Vec3,
    /// +0x50: the angles in the order yaw, pitch, roll.
    pub angles: Vec3,
    /// +0x60.
    pub motion: i32,
    /// +0x64.
    pub seat: i32,
}

/// The receiving console's vehicle entity, when the entry has one (+4).
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct VehicleEntity {
    /// What 0x823EA990 says (not ported yet): when true the update is refused.
    pub refuses_updates: bool,
    /// +0x308.
    pub field_308: i32,
    /// The vehicle info (+0x20C) has a physics body (+0x518).
    pub has_physics: bool,
}

/// What [`read_vehicle_update`] did.
#[derive(Debug, Clone, Copy, PartialEq)]
pub enum VehicleRead {
    /// A read failed: nothing changed.
    Failed,
    /// The entity refused it (0x823EA990): nothing changed.
    Refused,
    /// The entry took the position, velocities and angles.
    Applied,
    /// The motion state, seat and +0x308 were stored; the rest goes to the
    /// vehicle's physics body, which the game's physics code takes (with
    /// VMX128 stores; not ported yet).
    ForPhysics(VehicleUpdate),
}

/// Reads a vehicle update into `entry` (the caller read the index). `ok` is
/// the read status of [`crate::net::fields`]. In both the applied and the
/// physics case the original then tells the game the entry changed (0x823DD170).
///
/// Original: `MPPeer::ReadVehicleUpdate` at 0x823F6048
pub fn read_vehicle_update(
    buffer: &mut BitBuffer,
    entry: &mut VehicleEntry,
    entity: Option<&mut VehicleEntity>,
    ok: &mut bool,
    sin_cos: SinCosLanes,
) -> VehicleRead {
    let mut u = VehicleUpdate {
        origin: [0.0; 3],
        velocity: [0.0; 3],
        second_velocity: [0.0; 3],
        angles: [0.0; 3],
        controls: [0.0; 4],
        motion: 0,
        seat: 0,
        field_308: 0,
    };
    let mut good = read_origin(buffer, &mut u.origin, ok);
    good = good && read_velocity(buffer, &mut u.velocity, ok, sin_cos);
    good = good && read_velocity(buffer, &mut u.second_velocity, ok, sin_cos);
    good = good && read_angles3(buffer, &mut u.angles, ok);
    for (value, &(lo, hi, precision)) in u.controls.iter_mut().zip(&CONTROL_RANGES) {
        good = good && buffer.read_ranged_f32(value, lo, hi, precision);
    }
    good = good && buffer.read_ranged_i32(&mut u.motion, 0, STATE_MAX);
    good = good && buffer.read_ranged_i32(&mut u.seat, 0, STATE_MAX);
    good = good && buffer.read_ranged_i32(&mut u.field_308, FIELD_308_RANGE.0, FIELD_308_RANGE.1);
    if u.controls[1] < CONTROL_DEAD_ZONE && u.controls[1] > -CONTROL_DEAD_ZONE {
        u.controls[1] = 0.0;
    }
    if !good {
        return VehicleRead::Failed;
    }
    if entity.as_ref().is_some_and(|e| e.refuses_updates) {
        return VehicleRead::Refused;
    }
    entry.motion = u.motion;
    entry.seat = u.seat;
    if let Some(e) = entity {
        e.field_308 = u.field_308;
        if e.has_physics {
            return VehicleRead::ForPhysics(u);
        }
    }
    entry.origin = u.origin;
    entry.velocity = u.velocity;
    entry.second_velocity = u.second_velocity;
    entry.angles = [u.angles[1], u.angles[0], u.angles[2]];
    VehicleRead::Applied
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn constants_have_the_games_bits() {
        assert_eq!(CONTROL_DEAD_ZONE.to_bits(), 0x3A83_126F);
        assert_eq!(CONTROL_RANGES[0].2.to_bits(), 0x3D4C_CCCD);
        assert_eq!(CONTROL_RANGES[1].2.to_bits(), 0x3DCC_CCCD);
    }

    #[test]
    fn a_vehicle_update_comes_back() {
        let sent = VehicleUpdate {
            origin: [1000.0, -50.0, 12.0],
            velocity: [0.0, 0.0, -3.0],
            second_velocity: [0.0; 3],
            angles: [0.0, 90.0, 0.0],
            controls: [0.5, -1.0, 0.3, 0.0],
            motion: 2,
            seat: 1,
            field_308: 77,
        };
        let mut buffer = BitBuffer::new(false);
        write_vehicle_update(&mut buffer, &sent);
        assert_eq!(
            buffer.written,
            1 + 42 + 13 + 13 + 48 + 6 + 5 + 4 + 4 + 2 + 2 + 13
        );
        let mut entry = VehicleEntry {
            origin: [0.0; 3],
            velocity: [0.0; 3],
            second_velocity: [0.0; 3],
            angles: [0.0; 3],
            motion: 0,
            seat: 0,
        };
        let mut ok = true;
        let read = read_vehicle_update(&mut buffer, &mut entry, None, &mut ok, |_| [0.0; 4]);
        assert_eq!(read, VehicleRead::Applied);
        assert_eq!(entry.origin, sent.origin);
        assert_eq!(entry.velocity, sent.velocity);
        assert_eq!(entry.angles, [90.0, 0.0, 0.0]);
        assert_eq!((entry.motion, entry.seat), (2, 1));
    }
}
