//! The bodies of the two big server messages: the game state a client gets
//! when it connects or the map changes, and the snapshots it gets after
//! that, many times a second. Both are written into a [`Msg`] that
//! [`netchan::send_server_message`](super::netchan::send_server_message)
//! then compresses, scrambles and sends.
//!
//! The functions take plain inputs (the server's config strings, entity
//! states as their bytes) so the server keeps its own data structures.
//!
//! # Message types
//!
//! The server's message types are raw bytes. Call of Duty 2 renumbered them
//! from 0; United Offensive keeps Quake III's numbers ([`ServerOps`]).

use super::delta::{self, FieldTables};
use super::msg::{Msg, StringTooLong};
use crate::Game;

/// The server message types (`svc_ops_e`) of one game.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct ServerOps {
    pub nop: u8,
    pub game_state: u8,
    pub config_string: u8,
    pub baseline: u8,
    pub server_command: u8,
    pub download: u8,
    pub snapshot: u8,
    pub eof: u8,
}

/// `game`'s server message types.
pub fn server_ops(game: Game) -> ServerOps {
    match game {
        // Call of Duty 2 starts at svc_nop = 0 (no svc_bad).
        Game::CallOfDuty2 => ServerOps {
            nop: 0,
            game_state: 1,
            config_string: 2,
            baseline: 3,
            server_command: 4,
            download: 5,
            snapshot: 6,
            eof: 7,
        },
        // Quake III's numbers (svc_bad = 0).
        Game::UnitedOffensive => ServerOps {
            nop: 1,
            game_state: 2,
            config_string: 3,
            baseline: 4,
            server_command: 5,
            download: 6,
            snapshot: 7,
            eof: 8,
        },
    }
}

/// The size of the buffer the server writes a game state into: Call of
/// Duty 2 0x20000 bytes, United Offensive 0x8000.
pub fn game_state_buffer_size(game: Game) -> usize {
    match game {
        Game::CallOfDuty2 => 0x20000,
        Game::UnitedOffensive => 0x8000,
    }
}

/// The number of config strings (`MAX_CONFIGSTRINGS`).
pub const MAX_CONFIG_STRINGS: usize = 2048;

/// The server's reliable commands a client hasn't acknowledged yet.
#[derive(Debug, Clone, Copy)]
pub struct ServerCommands<'a> {
    /// The last command the client acknowledged.
    pub acknowledged: i32,
    /// The last command queued.
    pub sequence: i32,
    /// The command texts, a ring indexed by sequence number
    /// ([`max_reliable_commands`](super::netchan::max_reliable_commands)
    /// slots).
    pub texts: &'a [&'a [u8]],
}

/// Writes every reliable command after the acknowledged one: the message
/// type, the sequence number, the text. Returns the first text that was too
/// long (sent as an empty string, as the original does after printing a
/// warning).
///
/// Original: `SV_UpdateServerCommandsToClient` at 0x0045D9F0 (CoD2MP_s.exe 1.3)
/// Original: `SV_UpdateServerCommandsToClient` at 0x00464580 (CoDUOMP.exe 1.51)
pub fn write_server_commands(
    msg: &mut Msg,
    commands: &ServerCommands,
) -> Result<(), StringTooLong> {
    let ops = server_ops(msg.game);
    let mut result = Ok(());
    let slots = commands.texts.len().max(1);
    let mut i = commands.acknowledged.wrapping_add(1);
    while i <= commands.sequence {
        msg.write_byte(ops.server_command);
        msg.write_long(i);
        let text = commands
            .texts
            .get(i as u32 as usize % slots)
            .copied()
            .unwrap_or(b"");
        if let Err(e) = msg.write_string(text) {
            result = result.and(Err(e));
        }
        i = i.wrapping_add(1);
    }
    result
}

/// What a game state carries.
#[derive(Debug, Clone, Copy)]
pub struct GameState<'a> {
    /// The last client command the server executed (the message's first long).
    pub last_client_command: i32,
    /// Reliable commands still to deliver.
    pub commands: ServerCommands<'a>,
    /// Every config string by index; empty ones are not sent. At most
    /// [`MAX_CONFIG_STRINGS`].
    pub config_strings: &'a [&'a [u8]],
    /// Entity baselines (entity states); one whose number is 0 is not sent.
    pub baselines: &'a [&'a [u8]],
    /// The client's own number.
    pub client_num: i32,
    /// The checksum feed (keys the client's commands).
    pub checksum_feed: i32,
}

/// Bytes of an entity state the baselines are coded against (all zero):
/// at least the larger game's entity state (United Offensive: 0xF4).
const ZERO_ENTITY: [u8; 0x100] = [0; 0x100];

/// Writes a game state: the last client command, pending reliable commands,
/// the game state marker and reliable sequence, every non-empty config
/// string (index and big string), every baseline (as a change from an
/// all-zero entity, forced), then the end marker, the client number, the
/// checksum feed and a second end marker. Returns the first config string or
/// command that was too long (sent empty).
///
/// Original: `SV_SendClientGameState` at 0x004549A0 (CoD2MP_s.exe 1.3; the message part)
/// Original: `SV_SendClientGameState` at 0x0045A8F0 (CoDUOMP.exe 1.51; the message part)
pub fn write_game_state(
    msg: &mut Msg,
    tables: &FieldTables,
    state: &GameState,
) -> Result<(), StringTooLong> {
    let ops = server_ops(msg.game);
    msg.write_long(state.last_client_command);
    let mut result = write_server_commands(msg, &state.commands);
    msg.write_byte(ops.game_state);
    msg.write_long(state.commands.sequence);
    for (index, text) in state
        .config_strings
        .iter()
        .enumerate()
        .take(MAX_CONFIG_STRINGS)
    {
        if text.first().map_or(true, |&c| c == 0) {
            continue;
        }
        msg.write_byte(ops.config_string);
        msg.write_short(index as i16);
        if let Err(e) = msg.write_big_string(text) {
            result = result.and(Err(e));
        }
    }
    for baseline in state.baselines {
        if delta::word_at(baseline, 0) == 0 {
            continue;
        }
        msg.write_byte(ops.baseline);
        delta::write_delta_entity(msg, tables, &ZERO_ENTITY, Some(baseline), true);
    }
    msg.write_byte(ops.eof);
    msg.write_long(state.client_num);
    msg.write_long(state.checksum_feed);
    msg.write_byte(ops.eof);
    result
}

/// The parts of a client (`client_t`) sending it a game state reads and
/// changes.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub struct GameStateClient {
    /// `client->state` (0: a free slot); 3 (primed: loading the map) after.
    pub state: i32,
    /// The connection still has fragments of a message to send.
    pub unsent_fragments: bool,
    /// Cleared (Quake III's `pureAuthentic`): the client's pure checksums
    /// count again only once it sends them for this game state.
    pub pure_authentic: i32,
    /// Set to the outgoing sequence: the message the game state leaves in.
    pub gamestate_message_num: i32,
    /// The connection's outgoing sequence (read only).
    pub outgoing_sequence: i32,
    /// The last reliable command sent (`reliableSent`): set to the last one
    /// queued, as the game state carries them all.
    pub reliable_sent: i32,
}

/// The connection a game state goes out on (the server's netchan).
pub trait GameStateLink {
    /// Sends the connection's next fragment (`Netchan_TransmitNextFragment`);
    /// returns whether more remain.
    fn transmit_next_fragment(&mut self) -> bool;
    /// Sends the finished message (`SV_SendMessageToClient`: compressed,
    /// scrambled, sent); returns whether it was too big for one packet and
    /// left fragments to send.
    fn send_message(&mut self, msg: &Msg) -> bool;
}

/// Sends a client its game state. First the rest of a message still going
/// out in fragments (while the slot is in use), then the client is primed
/// (state 3, `pure_authentic` cleared, `gamestate_message_num` = the
/// outgoing sequence), the game state is written ([`write_game_state`])
/// into `buffer` (at most [`game_state_buffer_size`] bytes of it: a longer
/// game state ends overflowed, cut where it stopped fitting) and sent.
/// United Offensive then sends the first fragment at once when the message
/// left some and `timescale` (the `timescale` setting's integer) is 0; Call
/// of Duty 2 leaves them for the next frames. Returns the first config
/// string or command that was too long (sent empty after a warning).
///
/// Original: `SV_SendClientGameState` at 0x004549A0 (CoD2MP_s.exe 1.3; client on the stack)
/// Original: `SV_SendClientGameState` at 0x0045A8F0 (CoDUOMP.exe 1.51; client on the stack)
pub fn send_client_game_state(
    game: Game,
    tables: &FieldTables,
    client: &mut GameStateClient,
    state: &GameState,
    timescale: i32,
    buffer: &mut [u8],
    link: &mut dyn GameStateLink,
) -> Result<(), StringTooLong> {
    while client.state != 0 && client.unsent_fragments {
        client.unsent_fragments = link.transmit_next_fragment();
    }
    client.state = 3;
    client.pure_authentic = 0;
    client.gamestate_message_num = client.outgoing_sequence;
    let size = game_state_buffer_size(game).min(buffer.len());
    let mut msg = Msg::new(game, &mut buffer[..size]);
    let result = write_game_state(&mut msg, tables, state);
    client.reliable_sent = state.commands.sequence;
    if link.send_message(&msg) {
        client.unsent_fragments = true;
    }
    if game == Game::UnitedOffensive
        && timescale == 0
        && client.state != 0
        && client.unsent_fragments
    {
        client.unsent_fragments = link.transmit_next_fragment();
    }
    result
}

/// The number that ends an entity list (`ENTITYNUM_NONE`).
pub const ENTITY_LIST_END: i32 = 1023;

/// Writes the changes between two sorted entity lists: a changed entity as
/// a change from its old state, a new one as a change from its baseline
/// (forced), a gone one as removed; then the end number.
///
/// Original: `SV_EmitPacketEntities` at 0x0045D450 (CoD2MP_s.exe 1.3)
/// Original: `SV_EmitPacketEntities` at 0x00464000 (CoDUOMP.exe 1.51)
pub fn write_packet_entities<'a>(
    msg: &mut Msg,
    tables: &FieldTables,
    from: &[&[u8]],
    to: &[&[u8]],
    baseline: &dyn Fn(i32) -> &'a [u8],
) {
    let (mut old, mut new) = (0, 0);
    loop {
        let new_number = to.get(new).map(|e| delta::word_at(e, 0));
        let old_number = from.get(old).map(|e| delta::word_at(e, 0));
        match (old_number, new_number) {
            (None, None) => break,
            (Some(o), Some(n)) if o == n => {
                delta::write_delta_entity(msg, tables, from[old], Some(to[new]), false);
                old += 1;
                new += 1;
            }
            (o, Some(n)) if o.map_or(true, |o| n < o) => {
                delta::write_delta_entity(msg, tables, baseline(n), Some(to[new]), true);
                new += 1;
            }
            _ => {
                delta::write_delta_entity(msg, tables, from[old], None, true);
                old += 1;
            }
        }
    }
    msg.write_bits(ENTITY_LIST_END, delta::ENTITY_NUMBER_BITS);
}

/// The same for clients: each change starts with a 1 bit, a 0 bit ends the list.
///
/// Original: `SV_EmitPacketClients` at 0x0045D5C0 (CoD2MP_s.exe 1.3)
/// Original: `SV_EmitPacketClients` at 0x00464170 (CoDUOMP.exe 1.51)
pub fn write_packet_clients(msg: &mut Msg, tables: &FieldTables, from: &[&[u8]], to: &[&[u8]]) {
    let (mut old, mut new) = (0, 0);
    loop {
        let new_number = to.get(new).map(|c| delta::word_at(c, 0));
        let old_number = from.get(old).map(|c| delta::word_at(c, 0));
        match (old_number, new_number) {
            (None, None) => break,
            (Some(o), Some(n)) if o == n => {
                delta::write_delta_client(msg, tables, Some(from[old]), Some(to[new]), false);
                old += 1;
                new += 1;
            }
            (o, Some(n)) if o.map_or(true, |o| n < o) => {
                delta::write_delta_client(msg, tables, None, Some(to[new]), true);
                new += 1;
            }
            _ => {
                delta::write_delta_client(msg, tables, Some(from[old]), None, true);
                old += 1;
            }
        }
    }
    msg.write_bit0();
}

/// One of a client's snapshot frames as the server keeps it
/// (`clientSnapshot_t`): the player state and where the frame's entities
/// and clients are in the server's rings.
#[derive(Clone, Copy)]
pub struct SnapshotFrame<'a> {
    pub player: &'a [u8],
    pub num_entities: i32,
    pub num_clients: i32,
    pub first_entity: i32,
    pub first_client: i32,
}

/// The server's rings of snapshot entities and clients
/// (`svs.snapshotEntities`, `svs.snapshotClients`; each frame's lists are
/// runs in them) and the entities' baselines.
#[derive(Clone, Copy)]
pub struct SnapshotRings<'a> {
    pub entities: &'a [&'a [u8]],
    pub clients: &'a [&'a [u8]],
    /// `svs.nextSnapshotEntities`: where the next frame's entities go; a
    /// frame whose entities start more than the ring's size before it has
    /// been overwritten.
    pub next_entities: i32,
    pub baseline: &'a dyn Fn(i32) -> &'a [u8],
}

/// The client's side of a snapshot (`client_t`'s fields the writer reads).
pub struct SnapshotClient<'a> {
    /// `client->state` (4: in the game; 1: a zombie, just dropped).
    pub state: i32,
    /// Whether the client counts as in the game for the snapshot's flags:
    /// set while it is (state 4), cleared in any state but 1, kept in state
    /// 1 (the originals' word at +4).
    pub active: bool,
    /// The last message the client acknowledged (`deltaMessage`; 0 or less:
    /// none).
    pub delta_message: i32,
    /// The rate held this snapshot back (`rateDelayed`).
    pub rate_delayed: bool,
    /// The connection's outgoing sequence: this snapshot's frame is
    /// `frames[outgoing & 31]`.
    pub outgoing_sequence: i32,
    /// The client's 32 frames (`PACKET_BACKUP`).
    pub frames: &'a [SnapshotFrame<'a>],
}

/// Why a snapshot went out whole although the client had acknowledged one
/// (the originals print it when `developer` is set).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum DeltaRefused {
    /// The acknowledged message is 29 or more back ("Delta request from out
    /// of date packet").
    OutOfDatePacket,
    /// Its entities have been overwritten in the ring ("Delta request from
    /// out of date entities").
    OutOfDateEntities,
}

/// Writes a client's snapshot: the frame for this message, as a change from
/// the last frame the client acknowledged when that one is usable (the
/// client is in the game, the frame is less than 29 messages back and its
/// entities are still in the ring; otherwise whole), with the server's time,
/// how far back that frame is (0: none), the flags (the server's map-restart
/// bit; 1 for a rate-delayed client; 2 for one not in the game), then the
/// player state, the entities, the clients and `padding` no-op bytes
/// (`sv_padPackets`). A message the client acknowledged ahead of this one
/// counts as usable (a negative distance), as in the originals.
///
/// Original: `SV_WriteSnapshotToClient` at 0x0045D7C0 (CoD2MP_s.exe 1.3; client ecx, msg eax)
/// Original: `SV_WriteSnapshotToClient` at 0x00464390 (CoDUOMP.exe 1.51; client eax, msg esi)
pub fn write_snapshot_to_client(
    msg: &mut Msg,
    tables: &FieldTables,
    client: &mut SnapshotClient,
    rings: &SnapshotRings,
    time: i32,
    snap_flag_server_bit: i32,
    padding: i32,
) -> Option<DeltaRefused> {
    const PACKET_BACKUP: i32 = 32;
    let ops = server_ops(msg.game);
    let slot = |sequence: i32| (sequence & (PACKET_BACKUP - 1)) as usize;
    let frame = client.frames[slot(client.outgoing_sequence)];
    let mut refused = None;
    let mut old: Option<SnapshotFrame> = None;
    let mut last_frame = 0;
    if client.delta_message > 0 && client.state == 4 {
        let back = client.outgoing_sequence.wrapping_sub(client.delta_message);
        if back >= PACKET_BACKUP - 3 {
            refused = Some(DeltaRefused::OutOfDatePacket);
        } else {
            let candidate = client.frames[slot(client.delta_message)];
            let oldest = rings
                .next_entities
                .wrapping_sub(rings.entities.len() as i32);
            if candidate.first_entity < oldest {
                refused = Some(DeltaRefused::OutOfDateEntities);
            } else {
                old = Some(candidate);
                last_frame = back;
            }
        }
    }
    msg.write_byte(ops.snapshot);
    msg.write_long(time);
    msg.write_byte(last_frame as u8);
    let mut flags = snap_flag_server_bit;
    if client.rate_delayed {
        flags |= 1;
    }
    match client.state {
        4 => client.active = true,
        1 => {}
        _ => client.active = false,
    }
    if !client.active {
        flags |= 2;
    }
    msg.write_byte(flags as u8);
    delta::write_delta_playerstate(msg, tables, old.map(|o| o.player), frame.player);
    // A frame's run in a ring (the originals take each index modulo the
    // ring's size as they go).
    fn run<'r>(ring: &[&'r [u8]], first: i32, count: i32) -> Vec<&'r [u8]> {
        let size = ring.len().max(1) as i32;
        (0..count.max(0))
            .filter_map(|k| {
                ring.get(first.wrapping_add(k).rem_euclid(size) as usize)
                    .copied()
            })
            .collect()
    }
    let (from_entities, from_clients) = match old {
        Some(o) => (
            run(rings.entities, o.first_entity, o.num_entities),
            run(rings.clients, o.first_client, o.num_clients),
        ),
        None => (Vec::new(), Vec::new()),
    };
    let to_entities = run(rings.entities, frame.first_entity, frame.num_entities);
    let to_clients = run(rings.clients, frame.first_client, frame.num_clients);
    write_packet_entities(msg, tables, &from_entities, &to_entities, rings.baseline);
    write_packet_clients(msg, tables, &from_clients, &to_clients);
    for _ in 0..padding.max(0) {
        msg.write_byte(ops.nop);
    }
    refused
}
