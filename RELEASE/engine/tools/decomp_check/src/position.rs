//! Requests for `cod3_game::net::position` (the position update, message 0x23).
//!
//! The `MPPlayer` comes as its first 0x140 bytes (`b:`, big-endian, as the
//! guest keeps them): +8 the client's index (-1: none), +0xF0 the counter,
//! +0x100 to +0x13F the baseline. What the update reads from the player's
//! entity and client comes last, as the checker's own packing (see
//! `tools/decomp/specs/net_position.py`): origin, velocity, angles (9
//! floats), weapon, flags, use code, stance, state (5 words).

use cod3_game::net::fields::{self, SinCosLanes};
use cod3_game::net::position::{
    read_position_update, write_position_update, LocalEntity, PlayerSnapshot, PositionBaseline,
    RemotePlayer,
};

use cod3_game::net::view::{read_view_update, write_view_update, ViewSource};

use crate::bit_buffer::{ok, on_buffer, on_buffer_at};
use crate::{floats, store_floats, Answer, Args, Ret};

/// What the checks put in place of the game's VMX128 sine and cosine
/// (0x820B3920), on both sides: lane i times [0.5, -0.25, 2, 1], exact in
/// any arithmetic (`tools/decomp/specs/net_position.py`).
pub(crate) const SIN_COS_STAND_IN: SinCosLanes = |a| [a[0] * 0.5, a[1] * -0.25, a[2] * 2.0, a[3]];

fn word(bytes: &[u8], at: usize) -> u32 {
    u32::from_be_bytes([bytes[at], bytes[at + 1], bytes[at + 2], bytes[at + 3]])
}

fn baseline(player: &[u8]) -> PositionBaseline {
    PositionBaseline {
        counter: player[0xF0],
        origin: floats(&player[0x100..]),
        angles: floats(&player[0x110..]),
        velocity: floats(&player[0x120..]),
        use_code: word(player, 0x130),
        weapon: player[0x134] as i8,
        flags: word(player, 0x138),
        stance: word(player, 0x13C),
    }
}

fn store_baseline(player: &mut [u8], b: &PositionBaseline) {
    player[0xF0] = b.counter;
    store_floats(&mut player[0x100..], &b.origin);
    store_floats(&mut player[0x110..], &b.angles);
    store_floats(&mut player[0x120..], &b.velocity);
    player[0x130..0x134].copy_from_slice(&b.use_code.to_be_bytes());
    player[0x134] = b.weapon as u8;
    player[0x138..0x13C].copy_from_slice(&b.flags.to_be_bytes());
    player[0x13C..0x140].copy_from_slice(&b.stance.to_be_bytes());
}

fn remote(player: &[u8]) -> RemotePlayer {
    RemotePlayer {
        last_update_time: word(player, 0x48),
        use_code: word(player, 0x50),
        field_54: word(player, 0x54),
        pitch: floats::<1>(&player[0x5C..])[0],
        origin: floats(&player[0xA0..]),
        velocity: floats(&player[0xB0..]),
        view: floats(&player[0xC0..]),
        baseline: baseline(player),
        update_interval: u16::from_be_bytes([player[0x1E4], player[0x1E5]]),
        view_reset_pending: player[0x280] != 0,
        view_reset_pitch: floats::<1>(&player[0x210..])[0],
        view_reset_yaw: floats::<1>(&player[0x218..])[0],
    }
}

fn store_remote(player: &mut [u8], r: &RemotePlayer) {
    player[0x48..0x4C].copy_from_slice(&r.last_update_time.to_be_bytes());
    player[0x50..0x54].copy_from_slice(&r.use_code.to_be_bytes());
    player[0x54..0x58].copy_from_slice(&r.field_54.to_be_bytes());
    store_floats(&mut player[0x5C..], &[r.pitch]);
    store_floats(&mut player[0xA0..], &r.origin);
    store_floats(&mut player[0xB0..], &r.velocity);
    store_floats(&mut player[0xC0..], &r.view);
    store_baseline(player, &r.baseline);
    player[0x1E4..0x1E6].copy_from_slice(&r.update_interval.to_be_bytes());
    player[0x280] = u8::from(r.view_reset_pending);
    store_floats(&mut player[0x210..], &[r.view_reset_pitch]);
    store_floats(&mut player[0x218..], &[r.view_reset_yaw]);
}

fn snapshot(bytes: &[u8]) -> PlayerSnapshot {
    PlayerSnapshot {
        origin: floats(bytes),
        velocity: floats(&bytes[12..]),
        angles: floats(&bytes[24..]),
        weapon: word(bytes, 36) as i32,
        flags: word(bytes, 40),
        use_code: word(bytes, 44),
        stance: word(bytes, 48),
        state: word(bytes, 52) as i32,
    }
}

pub fn dispatch(name: &str, args: &mut Args) -> Option<Answer> {
    Some(match name {
        // Arguments: the MPPlayer, the buffer, force_full, commit, the snapshot.
        "MPPlayer::WritePositionUpdate" => {
            let player = args.buffer(0).expect("an MPPlayer").clone();
            let has_client = word(&player, 8) as i32 != -1;
            let current = snapshot(args.bytes(4));
            let (force_full, commit) = (args.int(2) as u8 != 0, args.int(3) as u8 != 0);
            let mut base = baseline(&player);
            let full = on_buffer_at(args, 1, |b, _| {
                write_position_update(
                    b,
                    &mut base,
                    has_client.then_some(&current),
                    force_full,
                    commit,
                )
            });
            store_baseline(args.buffer(0).expect("an MPPlayer"), &base);
            ok(full)
        }
        // Arguments: the MPPlayer, the buffer, the read status, the time, the entity's state.
        "MPPlayer::ReadPositionUpdate" => {
            let player = args.buffer(0).expect("an MPPlayer").clone();
            let mut record = remote(&player);
            let entity = if (word(&player, 8) as i32) < 0 {
                LocalEntity::NoClient
            } else {
                LocalEntity::State(word(args.bytes(4), 0) as i32)
            };
            let mut status = args.buffer(2).expect("the status")[0] != 0;
            let now = args.global_u32(3);
            on_buffer_at(args, 1, |b, _| {
                read_position_update(b, &mut record, entity, &mut status, now, SIN_COS_STAND_IN)
            });
            args.buffer(2).expect("the status")[0] = u8::from(status);
            store_remote(args.buffer(0).expect("an MPPlayer"), &record);
            Ok(Ret::Int(1))
        }
        // Arguments: the MPPlayer, the buffer, the checker's packing of a
        // ViewSource (tools/decomp/specs/net_view.py).
        "MPPlayer::WriteViewUpdate" => {
            let s = args.bytes(2).to_vec();
            let fault = s[33]; // after the turret index (32)
            let source = ViewSource {
                client_index: word(&s, 0) as i32,
                frozen: s[4] != 0,
                angles: floats(&s[5..]),
                view_counts: s[17] != 0,
                turret_limits: floats(&s[18..]),
                vehicle_seat_flags: (fault == 0).then_some([s[26] != 0, s[27] != 0]),
                weapon: word(&s, 28) as i32,
            };
            on_buffer_at(args, 1, |b, _| write_view_update(b, &source));
            Ok(Ret::Void)
        }
        // Arguments: the MPPlayer, the buffer, the read status, the time, the client's +0xAE0.
        "MPPlayer::ReadViewUpdate" => {
            let player = args.buffer(0).expect("an MPPlayer").clone();
            let mut record = remote(&player);
            let frozen = args.bytes(4)[0] != 0;
            let mut status = args.buffer(2).expect("the status")[0] != 0;
            let now = args.global_u32(3);
            on_buffer_at(args, 1, |b, _| {
                read_view_update(b, &mut record, frozen, &mut status, now)
            });
            args.buffer(2).expect("the status")[0] = u8::from(status);
            store_remote(args.buffer(0).expect("an MPPlayer"), &record);
            Ok(Ret::Int(1))
        }
        "MSGBB_ReadVelocity" => on_buffer(args, |b, a| {
            let mut out: [f32; 3] = floats(a.buffer(1).expect("an output"));
            let mut status = a.buffer(2).expect("the status")[0] != 0;
            let result = fields::read_velocity(b, &mut out, &mut status, SIN_COS_STAND_IN);
            store_floats(a.buffer(1).expect("an output"), &out);
            a.buffer(2).expect("the status")[0] = u8::from(status);
            ok(result)
        }),
        _ => return None,
    })
}
