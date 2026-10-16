//! Requests for `cod3_game::net::vehicle` (the vehicle update, message 0x4C).
//!
//! The writer's sources come in the checker's packing
//! (`tools/decomp/specs/net_vehicle.py`). The reader's entry (0x70 bytes)
//! and entity (0x310 bytes) come as the guest keeps them.

use cod3_game::net::vehicle::{
    motion_state, read_vehicle_update, seat_from_flags, write_vehicle_update, VehicleEntity,
    VehicleEntry, VehicleUpdate,
};

use crate::bit_buffer::on_buffer_at;
use crate::position::SIN_COS_STAND_IN;
use crate::{floats, store_floats, Answer, Args, Ret};

fn word(bytes: &[u8], at: usize) -> u32 {
    u32::from_be_bytes([bytes[at], bytes[at + 1], bytes[at + 2], bytes[at + 3]])
}

pub fn dispatch(name: &str, args: &mut Args) -> Option<Answer> {
    Some(match name {
        // Arguments: the entry, the buffer, the packed sources.
        "MPPeer::WriteVehicleUpdate" => {
            let s = args.bytes(2).to_vec();
            let update = VehicleUpdate {
                origin: floats(&s),
                angles: floats(&s[12..]),
                velocity: floats(&s[24..]),
                second_velocity: floats(&s[36..]),
                controls: [0.0; 4],
                motion: motion_state(
                    s[48] != 0,
                    s[49] != 0,
                    u16::from_be_bytes([s[50], s[51]]) == 1,
                    floats::<1>(&s[52..])[0],
                ),
                seat: seat_from_flags([s[56] != 0, s[57] != 0]),
                field_308: word(&s, 58) as i32,
            };
            on_buffer_at(args, 1, |b, _| write_vehicle_update(b, &update));
            Ok(Ret::Void)
        }
        // Arguments: the entry, the buffer, (unused), the entity, the read
        // status, then whether there is an entity and whether it refuses.
        "MPPeer::ReadVehicleUpdate" => {
            let e = args.buffer(0).expect("an entry").clone();
            let mut entry = VehicleEntry {
                origin: floats(&e[0x20..]),
                velocity: floats(&e[0x30..]),
                second_velocity: floats(&e[0x40..]),
                angles: floats(&e[0x50..]),
                motion: word(&e, 0x60) as i32,
                seat: word(&e, 0x64) as i32,
            };
            let info = args.bytes(5).to_vec();
            let entity_bytes = args.buffer(3).expect("an entity").clone();
            let mut entity = VehicleEntity {
                refuses_updates: info[1] != 0,
                field_308: word(&entity_bytes, 0x308) as i32,
                has_physics: false,
            };
            let mut status = args.buffer(4).expect("the status")[0] != 0;
            on_buffer_at(args, 1, |b, _| {
                read_vehicle_update(
                    b,
                    &mut entry,
                    (info[0] != 0).then_some(&mut entity),
                    &mut status,
                    SIN_COS_STAND_IN,
                )
            });
            args.buffer(4).expect("the status")[0] = u8::from(status);
            let e = args.buffer(0).expect("an entry");
            store_floats(&mut e[0x20..], &entry.origin);
            store_floats(&mut e[0x30..], &entry.velocity);
            store_floats(&mut e[0x40..], &entry.second_velocity);
            store_floats(&mut e[0x50..], &entry.angles);
            e[0x60..0x64].copy_from_slice(&entry.motion.to_be_bytes());
            e[0x64..0x68].copy_from_slice(&entry.seat.to_be_bytes());
            let ent = args.buffer(3).expect("an entity");
            ent[0x308..0x30C].copy_from_slice(&entity.field_308.to_be_bytes());
            Ok(Ret::Int(1))
        }
        _ => return None,
    })
}
