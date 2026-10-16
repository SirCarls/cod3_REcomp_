//! The position update (message 0x23): what each console sends every other
//! console about its own players, on the 125/250/1000 ms timers
//! (`docs/MP_PROTOCOL.md` 2.4).
//!
//! An update is either *full* (every field) or a *delta* against the last
//! full update the sender committed as the baseline: eight "changed" bits,
//! then only the fields that changed. A full update goes out when asked
//! for, when the flags word changed, or when the player is 512 units or more
//! from the baseline's position, so the flags never travel in a delta. An
//! 8-bit counter tells the receiver which baseline a delta is against; the
//! sender increments it when it commits a new one.
//!
//! The message's first bits (the type, the player's slot and the "near"
//! bit) are the caller's; [`write_position_update`] writes the rest.
//! [`read_position_update`] reads from the "near" bit on, into the
//! receiving console's record of that player.

use crate::math::angles::{angle_wrap_180, Vec3};
use crate::net::bit_buffer::BitBuffer;
use crate::net::fields::{
    read_angles2, read_coord_delta, read_origin, read_velocity, write_angles2, write_byte,
    write_coord_delta, write_origin, write_velocity, SinCosLanes, COORD_DELTA_MAX,
};
use crate::ppc::fmadds;

/// The largest "use code" (`docs/MP_PROTOCOL.md` 2.4; 16 bits).
pub const USE_CODE_MAX: u32 = 0x8000;
/// The largest value of the client's +0x520 field (probably the stance; 5 bits).
pub const STANCE_MAX: u32 = 25;
/// A coordinate "moved" when it is more than this far from the baseline's.
const MOVE_THRESHOLD: f32 = 1.0;

/// What a position update says about one player, read from the game's
/// entity and client structures when the update is written.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct PlayerSnapshot {
    /// The entity's position (entity +0x150).
    pub origin: Vec3,
    /// Client +0x10.
    pub velocity: Vec3,
    /// Pitch, yaw, roll (client +0xCC); updates carry pitch and yaw.
    pub angles: Vec3,
    /// Client +0xA4; 8 bits go out.
    pub weapon: i32,
    /// Client +0x2C.
    pub flags: u32,
    /// What the player is using or doing (0x823DDB08; not ported yet), 0 to 0x8000.
    pub use_code: u32,
    /// Client +0x520, 0 to 25.
    pub stance: u32,
    /// Client +0x784; 3 (probably dead or spectating) sets the update's first bit.
    pub state: i32,
}

/// The last full update the sender committed (the `MPPlayer`'s +0xF0 and
/// +0x100 to +0x13F), which deltas are against.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct PositionBaseline {
    /// +0xF0: increments with each committed full update.
    pub counter: u8,
    /// +0x100.
    pub origin: Vec3,
    /// +0x110 (all three are stored, two compared).
    pub angles: Vec3,
    /// +0x120.
    pub velocity: Vec3,
    /// +0x130.
    pub use_code: u32,
    /// +0x134: one byte, so a weapon number from 128 up never matches it
    /// again and goes out in every delta, as in the original.
    pub weapon: i8,
    /// +0x138.
    pub flags: u32,
    /// +0x13C.
    pub stance: u32,
}

/// Writes a position update for `player` and returns whether it was a full
/// one. `force_full` asks for a full update; `commit` makes a full update
/// the new baseline (incrementing the counter). A player slot without a
/// client (`None`; slot +8 is -1) writes nothing.
///
/// Original: `MPPlayer::WritePositionUpdate` at 0x823F2E20
pub fn write_position_update(
    buffer: &mut BitBuffer,
    baseline: &mut PositionBaseline,
    player: Option<&PlayerSnapshot>,
    force_full: bool,
    commit: bool,
) -> bool {
    let Some(p) = player else {
        return false;
    };
    buffer.write_bool(p.state == 3);

    let [x, y, z] = p.origin;
    let [bx, by, bz] = baseline.origin;
    let (dx, dy, dz) = (x - bx, y - by, z - bz);
    let near_baseline = fmadds(dx, dx, fmadds(dz, dz, dy * dy)).sqrt() < COORD_DELTA_MAX;
    let xy_moved = dx.abs() > MOVE_THRESHOLD || dy.abs() > MOVE_THRESHOLD;
    let z_moved = dz.abs() > MOVE_THRESHOLD;
    let velocity_changed = p.velocity != baseline.velocity;
    let angles_changed = p.angles[0] != baseline.angles[0] || p.angles[1] != baseline.angles[1];
    let weapon_changed = i32::from(baseline.weapon) != p.weapon;
    let flags_changed = baseline.flags != p.flags;
    let use_changed = baseline.use_code != p.use_code;
    let stance_changed = baseline.stance != p.stance;

    let full = force_full || flags_changed || !near_baseline;
    buffer.write_bool(full);
    if full {
        if commit {
            baseline.counter = baseline.counter.wrapping_add(1);
        }
        write_byte(buffer, baseline.counter);
        write_origin(buffer, &p.origin);
        write_velocity(buffer, &p.velocity);
        write_angles2(buffer, &p.angles);
        write_byte(buffer, p.weapon as u8);
        buffer.write_u32(p.flags);
        buffer.write_ranged_u32(p.use_code, 0, USE_CODE_MAX, true);
        buffer.write_ranged_u32(p.stance, 0, STANCE_MAX, true);
        if commit {
            *baseline = PositionBaseline {
                counter: baseline.counter,
                origin: p.origin,
                angles: p.angles,
                velocity: p.velocity,
                use_code: p.use_code,
                weapon: p.weapon as i8,
                flags: p.flags,
                stance: p.stance,
            };
        }
        return true;
    }

    write_byte(buffer, baseline.counter);
    for changed in [
        xy_moved,
        z_moved,
        velocity_changed,
        angles_changed,
        weapon_changed,
        use_changed,
        flags_changed,
        stance_changed,
    ] {
        buffer.write_bool(changed);
    }
    if xy_moved {
        write_coord_delta(buffer, bx, x);
        write_coord_delta(buffer, by, y);
    }
    if z_moved {
        write_coord_delta(buffer, bz, z);
    }
    if velocity_changed {
        write_velocity(buffer, &p.velocity);
    }
    if angles_changed {
        write_angles2(buffer, &p.angles);
    }
    if weapon_changed {
        write_byte(buffer, p.weapon as u8);
    }
    if flags_changed {
        buffer.write_u32(p.flags);
    }
    if use_changed {
        buffer.write_ranged_u32(p.use_code, 0, USE_CODE_MAX, true);
    }
    if stance_changed {
        buffer.write_ranged_u32(p.stance, 0, STANCE_MAX, true);
    }
    false
}

/// The receiving console's record of a remote player (an `MPPlayer`; the
/// fields a position update changes).
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct RemotePlayer {
    /// +0x48: the game time of the last update read (good or not).
    pub last_update_time: u32,
    /// +0x50.
    pub use_code: u32,
    /// +0x54: cleared by each good update.
    pub field_54: u32,
    /// +0x5C.
    pub pitch: f32,
    /// +0xA0: where the player is, as the update says.
    pub origin: Vec3,
    /// +0xB0.
    pub velocity: Vec3,
    /// +0xC0: yaw, pitch, 0.
    pub view: Vec3,
    /// +0xF0 and +0x100 to +0x13F: the last full update, which deltas are against.
    pub baseline: PositionBaseline,
    /// +0x1E4: when the next update is due, in ms (125 when "near", else 250).
    pub update_interval: u16,
    /// +0x280: the view is to be reset at the next good update.
    pub view_reset_pending: bool,
    /// +0x210: the pitch at the reset.
    pub view_reset_pitch: f32,
    /// +0x218: the yaw at the reset, wrapped into [-180, 180] (stored as
    /// the float's bits in the original's 32-bit field).
    pub view_reset_yaw: f32,
}

/// What the receiving console knows of the player's entity when the update arrives.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum LocalEntity {
    /// The `MPPlayer` has no client index (+8 is negative).
    NoClient,
    /// There is a client index but no entity or client for it.
    Missing,
    /// The client's +0x784 state (3: probably dead or spectating).
    State(i32),
}

/// What [`read_position_update`] did.
#[derive(Debug, Clone, Copy, PartialEq)]
pub enum PositionRead {
    /// Dropped before any change but the baseline: the player is in state 3
    /// here and the update doesn't say so, or a delta against another
    /// baseline than the receiver's.
    Ignored,
    /// A read failed; only the time was recorded (and a full update's
    /// baseline, which is stored whether or not its reads succeeded).
    Failed,
    /// Applied to the `MPPlayer`. With an entity, the game also applies
    /// these to it (`flags`, `stance`, `weapon` and more; not ported yet).
    Applied {
        /// The update's "state 3" bit.
        state3: bool,
        /// Client +0x2C.
        flags: u32,
        /// Client +0x520.
        stance: u32,
        /// Client +0xA4 (one byte, sign-extended).
        weapon: i8,
    },
}

/// The values one update carries, starting from the baseline's for a delta.
struct Fields {
    counter: u8,
    origin: Vec3,
    velocity: Vec3,
    angles: Vec3,
    weapon: i8,
    flags: u32,
    use_code: u32,
    stance: u32,
}

/// Reads a position update into `player` (the message's type and slot
/// were read by the caller). `ok` is the shared read status of
/// [`crate::net::fields`]; `now` is the game time the original records
/// (a global, 0x82C2826C).
///
/// Every part is read only when all before it were. A full update becomes
/// the receiver's baseline right away; a delta starts from the baseline's
/// values and reads the fields its bits say changed. Then the update is
/// dropped, or recorded as failed, or applied (see [`PositionRead`]).
///
/// Original: `MPPlayer::ReadPositionUpdate` at 0x823F78B0
pub fn read_position_update(
    buffer: &mut BitBuffer,
    player: &mut RemotePlayer,
    entity: LocalEntity,
    ok: &mut bool,
    now: u32,
    sin_cos: SinCosLanes,
) -> PositionRead {
    let (mut near, mut state3, mut full) = (false, false, false);
    let mut good =
        buffer.read_bool(&mut near) && buffer.read_bool(&mut state3) && buffer.read_bool(&mut full);
    let mut f = Fields {
        counter: 0,
        origin: [0.0; 3],
        velocity: [0.0; 3],
        angles: [0.0; 3],
        weapon: 0,
        flags: 0,
        use_code: 0,
        stance: 0,
    };
    if good && full {
        let mut counter = 0i8;
        good = buffer.read_i8(&mut counter);
        f.counter = counter as u8;
        good = good && read_origin(buffer, &mut f.origin, ok);
        good = good && read_velocity(buffer, &mut f.velocity, ok, sin_cos);
        good = good && read_angles2(buffer, &mut f.angles, ok);
        good = good && buffer.read_i8(&mut f.weapon);
        good = good && buffer.read_u32(&mut f.flags);
        good = good && buffer.read_ranged_u32(&mut f.use_code, 0, USE_CODE_MAX, true);
        good = good && buffer.read_ranged_u32(&mut f.stance, 0, STANCE_MAX, true);
        player.baseline = PositionBaseline {
            counter: f.counter,
            origin: f.origin,
            angles: f.angles,
            velocity: f.velocity,
            use_code: f.use_code,
            weapon: f.weapon,
            flags: f.flags,
            stance: f.stance,
        };
    } else if good {
        let mut counter = 0i8;
        good = buffer.read_i8(&mut counter);
        f.counter = counter as u8;
        // xy, z, velocity, angles, weapon, use code, flags, stance.
        let mut changed = [false; 8];
        for c in changed.iter_mut() {
            good = good && buffer.read_bool(c);
        }
        let [xy, z, velocity, angles, weapon, use_code, flags, stance] = changed;
        let b = &player.baseline;
        f.origin = b.origin;
        f.velocity = b.velocity;
        f.angles = [b.angles[0], b.angles[1], 0.0];
        f.weapon = b.weapon;
        f.flags = b.flags;
        f.use_code = b.use_code;
        f.stance = b.stance;
        let base = b.origin;
        if xy {
            good = good && read_coord_delta(buffer, base[0], &mut f.origin[0], ok);
            good = good && read_coord_delta(buffer, base[1], &mut f.origin[1], ok);
        }
        if z {
            good = good && read_coord_delta(buffer, base[2], &mut f.origin[2], ok);
        }
        if velocity {
            good = good && read_velocity(buffer, &mut f.velocity, ok, sin_cos);
        }
        if angles {
            good = good && read_angles2(buffer, &mut f.angles, ok);
        }
        if weapon {
            good = good && buffer.read_i8(&mut f.weapon);
        }
        if flags {
            good = good && buffer.read_u32(&mut f.flags);
        }
        if use_code {
            good = good && buffer.read_ranged_u32(&mut f.use_code, 0, USE_CODE_MAX, true);
        }
        if stance {
            good = good && buffer.read_ranged_u32(&mut f.stance, 0, STANCE_MAX, true);
        }
    }

    if entity == LocalEntity::State(3) && !state3 {
        return PositionRead::Ignored;
    }
    if !full && f.counter != player.baseline.counter {
        return PositionRead::Ignored;
    }
    if !good {
        player.last_update_time = now;
        return PositionRead::Failed;
    }
    player.update_interval = if near { 125 } else { 250 };
    player.origin = f.origin;
    player.velocity = f.velocity;
    player.view = [f.angles[1], f.angles[0], 0.0];
    if player.view_reset_pending {
        player.view_reset_pending = false;
        player.view_reset_yaw = angle_wrap_180(player.view[0]);
        player.view_reset_pitch = player.view[1];
    }
    player.pitch = f.angles[0];
    player.field_54 = 0;
    player.use_code = f.use_code;
    player.last_update_time = now;
    PositionRead::Applied {
        state3,
        flags: f.flags,
        stance: f.stance,
        weapon: f.weapon,
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    /// A stand-in for the game's VMX128 sine and cosine, close enough for a round trip.
    fn sin_cos(a: &[f32; 4]) -> [f32; 4] {
        [a[0].sin(), a[1].cos(), a[2].sin(), a[3].cos()]
    }

    fn empty_baseline() -> PositionBaseline {
        PositionBaseline {
            counter: 0,
            origin: [0.0; 3],
            angles: [0.0; 3],
            velocity: [0.0; 3],
            use_code: 0,
            weapon: 0,
            flags: 0,
            stance: 0,
        }
    }

    fn receiver() -> RemotePlayer {
        RemotePlayer {
            last_update_time: 0,
            use_code: 0,
            field_54: 7,
            pitch: 0.0,
            origin: [0.0; 3],
            velocity: [0.0; 3],
            view: [0.0; 3],
            baseline: empty_baseline(),
            update_interval: 0,
            view_reset_pending: false,
            view_reset_pitch: 0.0,
            view_reset_yaw: 0.0,
        }
    }

    /// The caller's part of the message (slot, "near"), then the update.
    fn message(
        sender: &mut PositionBaseline,
        player: &PlayerSnapshot,
        force_full: bool,
    ) -> (BitBuffer, bool) {
        let mut buffer = BitBuffer::new(false);
        buffer.write_bits(&[5], 8);
        buffer.write_bool(true);
        let full = write_position_update(&mut buffer, sender, Some(player), force_full, true);
        (buffer, full)
    }

    fn read(buffer: &mut BitBuffer, into: &mut RemotePlayer) -> PositionRead {
        let mut slot = [0u8];
        assert!(buffer.read_bits(&mut slot, 8));
        assert_eq!(slot[0], 5);
        let mut ok = true;
        read_position_update(buffer, into, LocalEntity::NoClient, &mut ok, 1234, sin_cos)
    }

    #[test]
    fn a_full_update_and_a_delta_come_back() {
        let mut player = PlayerSnapshot {
            origin: [100.0, -2000.0, 64.0],
            velocity: [0.0, 190.0, -12.0],
            angles: [10.0, 90.0, 0.0],
            weapon: 7,
            flags: 0x0010_0002,
            use_code: 0x21,
            stance: 3,
            state: 0,
        };
        let mut sender = empty_baseline();
        let mut receiver = receiver();

        let (mut buffer, full) = message(&mut sender, &player, true);
        assert!(full);
        let PositionRead::Applied {
            flags,
            stance,
            weapon,
            state3,
        } = read(&mut buffer, &mut receiver)
        else {
            panic!("not applied")
        };
        assert_eq!((flags, stance, weapon, state3), (0x0010_0002, 3, 7, false));
        assert_eq!(receiver.origin, player.origin);
        // The sender's baseline keeps the exact values, the receiver's the
        // values as the wire carried them: angles in 16-bit steps,
        // velocities rebuilt from a direction byte.
        assert_eq!(sender.angles[0], 10.0);
        assert_eq!(receiver.baseline.angles[0], 9.997_559);
        assert_eq!(receiver.baseline.counter, sender.counter);
        assert_eq!(receiver.baseline.origin, sender.origin);
        assert_eq!(
            (
                receiver.baseline.flags,
                receiver.baseline.use_code,
                receiver.baseline.stance
            ),
            (sender.flags, sender.use_code, sender.stance)
        );
        assert_eq!(receiver.update_interval, 125);
        assert_eq!(receiver.last_update_time, 1234);
        assert_eq!(receiver.field_54, 0);
        // Speeds are whole numbers, directions 1/255 turns: the velocity comes back close.
        assert!(
            (receiver.velocity[1] - 190.0).abs() < 1.0,
            "{:?}",
            receiver.velocity
        );
        assert_eq!(receiver.velocity[2], -12.0);
        assert_eq!(receiver.view[0], 90.0);

        // A small move: a delta with only the position.
        player.origin[0] += 30.0;
        let (mut buffer, full) = message(&mut sender, &player, false);
        assert!(!full);
        assert!(matches!(
            read(&mut buffer, &mut receiver),
            PositionRead::Applied { .. }
        ));
        assert_eq!(receiver.origin, player.origin);
        assert_eq!(buffer.read_position, buffer.written);

        // A delta against a baseline the receiver doesn't have is dropped.
        receiver.baseline.counter = receiver.baseline.counter.wrapping_add(1);
        let (mut buffer, _) = message(&mut sender, &player, false);
        assert_eq!(read(&mut buffer, &mut receiver), PositionRead::Ignored);
    }
}
