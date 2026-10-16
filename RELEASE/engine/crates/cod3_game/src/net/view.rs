//! The view update (message 0x24): what a console sends instead of the
//! position update while its player is in a vehicle or on a turret, whose
//! position travels in the vehicle message (0x4C). It has no player slot:
//! the receiver knows the sender's connection.
//!
//! The bits: whether the view angles follow (1), the pitch and yaw (2 × 16,
//! when they do), the seat (a ranged int in [0, 3]: 2 bits) and the weapon (8).

use crate::math::angles::{angle_wrap_180, Vec3};
use crate::net::bit_buffer::BitBuffer;
use crate::net::fields::{read_angles2, write_angles2, write_byte};
use crate::net::position::RemotePlayer;
use crate::net::vehicle::seat_from_flags;

/// The largest seat number a view update can carry.
pub const SEAT_MAX: i32 = 3;

/// What `MPPlayer::WriteViewUpdate` reads from the game to decide what the
/// update says (the parts of the game it asks aren't ported yet; their
/// answers come in here).
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct ViewSource {
    /// The `MPPlayer`'s client index (+8); -1 writes nothing.
    pub client_index: i32,
    /// Client +0xAE0: when set, nothing is written.
    pub frozen: bool,
    /// Client +0xCC: pitch, yaw, roll.
    pub angles: Vec3,
    /// What 0x820B12D8 says of the player's entity (whether the view counts;
    /// not ported yet).
    pub view_counts: bool,
    /// The two limits of the client's turret entry (+0x114 and +0x118 of
    /// the 0x1F0-byte entry the client's +0xAF4 picks in the table at
    /// 0x82C77E30): no angles go out when the first is below the second.
    pub turret_limits: [f32; 2],
    /// The two seat flags (+0x215, +0x216 of the vehicle's +0x20C) of the
    /// vehicle the entity is linked to (+0x1B0), when that link is good and
    /// the player is in it (`MPPlayer` +0x40 is 1 and 0x823D9B50 agrees).
    pub vehicle_seat_flags: Option<[bool; 2]>,
    /// Client +0xA4; 8 bits go out.
    pub weapon: i32,
}

/// Writes the view update for a player; nothing for a slot without a
/// client or a frozen client. Returns whether it wrote one.
///
/// Original: `MPPlayer::WriteViewUpdate` at 0x823E6770
pub fn write_view_update(buffer: &mut BitBuffer, source: &ViewSource) -> bool {
    if source.client_index == -1 || source.frozen {
        return false;
    }
    let [first, second] = source.turret_limits;
    // Not `first >= second`: a NaN limit lets the angles out, as on the PowerPC.
    #[allow(clippy::neg_cmp_op_on_partial_ord)]
    let within_limits = !(first < second);
    if source.view_counts && within_limits {
        buffer.write_bool(true);
        write_angles2(buffer, &source.angles);
    } else {
        buffer.write_bool(false);
    }
    let seat = source.vehicle_seat_flags.map_or(0, seat_from_flags);
    buffer.write_ranged_i32(seat, 0, SEAT_MAX);
    write_byte(buffer, source.weapon as u8);
    true
}

/// What a view update carried.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct ViewRead {
    /// Pitch and yaw, when the update had them.
    pub angles: Option<[f32; 2]>,
    /// The seat, 0 to 3; the game gives it to the vehicle when the player
    /// is in one (`MPPlayer` +0x40 is 1; not ported yet).
    pub seat: i32,
    /// The weapon; the game switches the player to it when it changed and
    /// `MPPlayer` +0x40 is 2 to 5 (not ported yet).
    pub weapon: i8,
}

/// Reads a view update into `player`. `frozen` is the player's client's
/// +0xAE0 here; a frozen player takes nothing from the update. The pitch
/// and yaw go into the view (+0xC0, +0xC4), with the view reset of
/// [`RemotePlayer::view_reset_pending`]; the rest is returned for the game.
/// The time is recorded whatever happens. `ok` is the read status of
/// [`crate::net::fields`], `now` the game time (0x82C2826C).
///
/// Original: `MPPlayer::ReadViewUpdate` at 0x823F7638
pub fn read_view_update(
    buffer: &mut BitBuffer,
    player: &mut RemotePlayer,
    frozen: bool,
    ok: &mut bool,
    now: u32,
) -> Option<ViewRead> {
    let mut has_angles = false;
    let mut angles = [0.0; 3];
    let mut seat = 0;
    let mut weapon = 0i8;
    let mut good = buffer.read_bool(&mut has_angles);
    if good && has_angles {
        good = read_angles2(buffer, &mut angles, ok);
    }
    good = good && buffer.read_ranged_i32(&mut seat, 0, SEAT_MAX);
    good = good && buffer.read_i8(&mut weapon);
    player.last_update_time = now;
    if !good || frozen {
        return None;
    }
    if has_angles {
        player.view[1] = angles[0];
        player.view[0] = angles[1];
        if player.view_reset_pending {
            player.view_reset_pending = false;
            player.view_reset_yaw = angle_wrap_180(player.view[0]);
            player.view_reset_pitch = player.view[1];
        }
    }
    Some(ViewRead {
        angles: has_angles.then_some([angles[0], angles[1]]),
        seat,
        weapon,
    })
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::net::position::{PositionBaseline, RemotePlayer};

    #[test]
    fn a_view_update_comes_back() {
        let source = ViewSource {
            client_index: 2,
            frozen: false,
            angles: [-11.25, 270.0, 0.0],
            view_counts: true,
            turret_limits: [0.0, 0.0],
            vehicle_seat_flags: Some([false, true]),
            weapon: 9,
        };
        let mut buffer = BitBuffer::new(false);
        assert!(write_view_update(&mut buffer, &source));
        assert_eq!(buffer.written, 1 + 1 + 32 + 2 + 8);
        let mut player = RemotePlayer {
            last_update_time: 0,
            use_code: 0,
            field_54: 0,
            pitch: 0.0,
            origin: [0.0; 3],
            velocity: [0.0; 3],
            view: [0.0; 3],
            baseline: PositionBaseline {
                counter: 0,
                origin: [0.0; 3],
                angles: [0.0; 3],
                velocity: [0.0; 3],
                use_code: 0,
                weapon: 0,
                flags: 0,
                stance: 0,
            },
            update_interval: 0,
            view_reset_pending: false,
            view_reset_pitch: 0.0,
            view_reset_yaw: 0.0,
        };
        let mut ok = true;
        let read = read_view_update(&mut buffer, &mut player, false, &mut ok, 5).expect("read");
        // 270° goes out as the 16-bit step it is; read back it is -90°.
        assert_eq!(
            read,
            ViewRead {
                angles: Some([-11.25, -90.0]),
                seat: 1,
                weapon: 9
            }
        );
        assert_eq!(player.view, [-90.0, -11.25, 0.0]);
        assert_eq!(player.last_update_time, 5);
    }
}
