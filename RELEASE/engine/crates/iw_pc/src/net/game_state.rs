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

/// What a snapshot carries.
#[derive(Clone, Copy)]
pub struct Snapshot<'a> {
    /// The server's time.
    pub server_time: i32,
    /// How many snapshots back the one this is a change from is (0: none).
    pub delta_num: u8,
    /// Flags: the server's map-restart bit, 1 for a rate-delayed client, 2
    /// for a client not in the game yet.
    pub snap_flags: u8,
    /// The player state the client has (`None`: send it whole).
    pub player_from: Option<&'a [u8]>,
    /// The player state now.
    pub player_to: &'a [u8],
    /// The entities the client has and the ones now, each sorted by number.
    pub entities_from: &'a [&'a [u8]],
    pub entities_to: &'a [&'a [u8]],
    /// The baseline of an entity number (for entities the client doesn't have).
    pub baseline: &'a dyn Fn(i32) -> &'a [u8],
    /// The clients the client has and the ones now, each sorted by number.
    pub clients_from: &'a [&'a [u8]],
    pub clients_to: &'a [&'a [u8]],
    /// Padding: this many no-op bytes at the end (`sv_padPackets`).
    pub padding: usize,
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

/// Writes a snapshot: its message type, the server time, the delta number,
/// the flags, the player state, the entity and client lists, and the padding.
///
/// Original: `SV_WriteSnapshotToClient` at 0x0045D7C0 (CoD2MP_s.exe 1.3; the message part)
/// Original: `SV_WriteSnapshotToClient` at 0x00464390 (CoDUOMP.exe 1.51; the message part)
pub fn write_snapshot(msg: &mut Msg, tables: &FieldTables, snapshot: &Snapshot) {
    let ops = server_ops(msg.game);
    msg.write_byte(ops.snapshot);
    msg.write_long(snapshot.server_time);
    msg.write_byte(snapshot.delta_num);
    msg.write_byte(snapshot.snap_flags);
    delta::write_delta_playerstate(msg, tables, snapshot.player_from, snapshot.player_to);
    write_packet_entities(
        msg,
        tables,
        snapshot.entities_from,
        snapshot.entities_to,
        snapshot.baseline,
    );
    write_packet_clients(msg, tables, snapshot.clients_from, snapshot.clients_to);
    for _ in 0..snapshot.padding {
        msg.write_byte(ops.nop);
    }
}
