//! The client's side of the network protocol: what a client keeps of the
//! server's messages and how it reads them, so that a Rust client can join
//! a server of the original PC games (a CB or Steam copy's dedicated server).
//!
//! [`ClientState`] holds what the original keeps for it in the client's `cl`
//! (the game state's config strings, the entity baselines, the last 32
//! snapshots and the rings their entities and clients are kept in) and in
//! `clc`/`cls` (the server's command sequence and commands, the checksum
//! feed, the client number). The parsers:
//!
//! * [`parse_server_message`]: one message from the server, after the
//!   netchan: decompressed, then its parts in order.
//! * [`parse_game_state`]: the game state (config strings, baselines).
//! * [`parse_snapshot`], with [`parse_packet_entities`] and
//!   [`parse_packet_clients`]: a snapshot, as a change from an earlier one.
//! * [`parse_command_string`]: one reliable command from the server.
//!
//! Around them, [`packet_event`]: a packet from the network, through the
//! channel, unscrambled and parsed (`CL_PacketEvent`); and
//! [`connectionless_packet`], what the client does with the server's
//! packets without a connection (the challenge, the connect answer ...).
//!
//! And the other way: [`write_packet`], the client's message to the server
//! (its reliable commands and its latest player commands), coded, scrambled
//! and handed to the channel, as `CL_WritePacket` does.
//!
//! Each is checked against the original with the client's globals as they
//! are in the game (`tools/decomp/x86/specs/net_client.py`).
//!
//! What the originals do around these and the ports leave to the caller:
//! the console (`Con_Close`), the system info and file system restart,
//! authorization and downloads after a game state, the debug output
//! (`cl_shownet`, `cl_shownuments`).
//!
//! Not ported yet: United Offensive's handling of a server with
//! `sv_onlyVisibleClients` set (the client's flag that `challengeResponse`
//! carries): it traces through the map for each player entity it carries
//! over (`CM_BoxTrace`) and flags the ones it can't see. The ports read
//! snapshots as the original does with the flag off.

pub mod connect;
pub mod connection;
pub mod download;

use crate::net::delta::{self, FieldTables};
use crate::net::game_state::ENTITY_LIST_END;
use crate::net::huffman::MessageTree;
use crate::net::msg::{Msg, BIG_INFO_STRING, MAX_STRING_CHARS};
use crate::net::netchan::{self, NetAddress, NetSource, Netchan, Packet};
use crate::shared::hash::hash_key;
use crate::{Game, GameError};

/// Config strings (`MAX_CONFIGSTRINGS`).
pub const MAX_CONFIG_STRINGS: usize = 2048;
/// Snapshots kept (`PACKET_BACKUP`).
pub const PACKET_BACKUP: usize = 32;
/// Entities and clients kept for snapshots (`MAX_PARSE_ENTITIES`, `MAX_PARSE_CLIENTS`).
pub const MAX_PARSE_ENTITIES: usize = 2048;
/// Entity numbers (`MAX_GENTITIES`).
pub const MAX_ENTITIES: usize = 1024;
/// A snapshot's entities or clients may come from one at most this many
/// entries back in the ring (`MAX_PARSE_ENTITIES - 128`).
pub const MAX_RING_DISTANCE: i32 = 1920;
/// An entity or client number that ends a list.
const LIST_END: i32 = 99999;
/// A client state's size (`clientState_t`).
pub const CLIENT_STATE_SIZE: usize = 0x5C;
/// Player commands kept (`CMD_BACKUP`).
pub const CMD_BACKUP: usize = 128;
/// The most player commands one packet carries (`MAX_PACKET_USERCMDS`).
pub const MAX_PACKET_USERCMDS: i32 = 32;

/// An entity state's size (`entityState_t`): Call of Duty 2 0xF0, United
/// Offensive 0xF4.
pub fn entity_state_size(game: Game) -> usize {
    match game {
        Game::CallOfDuty2 => 0xF0,
        Game::UnitedOffensive => 0xF4,
    }
}

/// The room for config string text (`MAX_GAMESTATE_CHARS`): Call of Duty 2
/// 0x20000, United Offensive 0x5000.
pub fn max_game_state_chars(game: Game) -> usize {
    match game {
        Game::CallOfDuty2 => 0x20000,
        Game::UnitedOffensive => 0x5000,
    }
}

/// Reliable commands kept (`MAX_RELIABLE_COMMANDS`): 128 (Call of Duty 2), 64 (United Offensive).
pub fn max_reliable_commands(game: Game) -> usize {
    crate::net::netchan::max_reliable_commands(game)
}

/// The server's message types (`svc_ops_e`), as [`crate::net::game_state::server_ops`].
fn ops(game: Game) -> crate::net::game_state::ServerOps {
    crate::net::game_state::server_ops(game)
}

/// The game state: every config string's text, one after the other
/// (`gameState_t`). Offset 0 (where unset strings point) is the empty string.
#[derive(Clone)]
pub struct GameStateStrings {
    /// Where each config string starts in `data`.
    pub offsets: Vec<i32>,
    /// The texts, each with its terminator.
    pub data: Vec<u8>,
    /// The bytes of `data` in use.
    pub data_count: i32,
}

impl GameStateStrings {
    fn new(game: Game) -> GameStateStrings {
        GameStateStrings {
            offsets: vec![0; MAX_CONFIG_STRINGS],
            data: vec![0; max_game_state_chars(game)],
            data_count: 0,
        }
    }

    /// Config string `index`'s text (empty when unset or out of range).
    pub fn get(&self, index: usize) -> &[u8] {
        let Some(&at) = self.offsets.get(index) else {
            return b"";
        };
        let rest = self.data.get(at.max(0) as usize..).unwrap_or(b"");
        &rest[..crate::cstr::len(rest)]
    }
}

/// One snapshot as the client keeps it (`clSnapshot_t`).
#[derive(Clone)]
pub struct Snapshot {
    /// Complete: the client has the snapshot it was a change from.
    pub valid: i32,
    pub snap_flags: i32,
    pub server_time: i32,
    /// The server message it came in.
    pub message_num: i32,
    /// The message of the snapshot it changes (-1: none).
    pub delta_num: i32,
    pub ping: i32,
    /// The word after `ping` (`cmdNum`; the parsers only copy it).
    pub cmd_num: i32,
    /// The player's own state (`playerState_t`).
    pub player_state: Vec<u8>,
    pub num_entities: i32,
    pub num_clients: i32,
    /// Where its entities start in the entity ring.
    pub parse_entities_num: i32,
    /// Where its clients start in the client ring.
    pub parse_clients_num: i32,
    /// The last server command when it arrived.
    pub server_command_num: i32,
}

impl Snapshot {
    /// An empty snapshot of `game`.
    pub fn new(game: Game) -> Snapshot {
        Snapshot {
            valid: 0,
            snap_flags: 0,
            server_time: 0,
            message_num: 0,
            delta_num: 0,
            ping: 0,
            cmd_num: 0,
            player_state: vec![0; delta::player_state_size(game)],
            num_entities: 0,
            num_clients: 0,
            parse_entities_num: 0,
            parse_clients_num: 0,
            server_command_num: 0,
        }
    }

    fn clear(&mut self) {
        self.valid = 0;
        self.snap_flags = 0;
        self.server_time = 0;
        self.message_num = 0;
        self.delta_num = 0;
        self.ping = 0;
        self.cmd_num = 0;
        self.player_state.fill(0);
        self.num_entities = 0;
        self.num_clients = 0;
        self.parse_entities_num = 0;
        self.parse_clients_num = 0;
        self.server_command_num = 0;
    }

    fn copy_from(&mut self, other: &Snapshot) {
        self.valid = other.valid;
        self.snap_flags = other.snap_flags;
        self.server_time = other.server_time;
        self.message_num = other.message_num;
        self.delta_num = other.delta_num;
        self.ping = other.ping;
        self.cmd_num = other.cmd_num;
        self.player_state.copy_from_slice(&other.player_state);
        self.num_entities = other.num_entities;
        self.num_clients = other.num_clients;
        self.parse_entities_num = other.parse_entities_num;
        self.parse_clients_num = other.parse_clients_num;
        self.server_command_num = other.server_command_num;
    }
}

/// When a command the client sent left (`outPacket_t`): its ping is
/// measured against the snapshot that answers it.
#[derive(Debug, Clone, Copy, Default)]
pub struct OutPacket {
    pub cmd_number: i32,
    pub server_time: i32,
    pub realtime: i32,
}

/// What a client keeps of the server's messages.
#[derive(Clone)]
pub struct ClientState {
    pub game: Game,
    /// Frames in a row without a message from the server for longer than
    /// the time-out ([`check_timeout`]; `cl.timeoutcount`).
    pub timeout_count: i32,
    // --- the original's `cl` (cleared by a new game state) ---
    /// The latest complete snapshot.
    pub snap: Snapshot,
    /// The server time of the snapshot before it.
    pub old_frame_server_time: i32,
    /// A new snapshot arrived (the client game reads it, then clears this).
    pub new_snapshots: i32,
    /// The server time the client game runs at (`cl.serverTime`), which its
    /// player commands carry: set by [`set_cgame_time`].
    pub server_time: i32,
    /// The last [`Self::server_time`]: the time never runs backwards.
    pub old_server_time: i32,
    /// The latest snapshot's server time when [`set_cgame_time`] last ran
    /// (Quake III's `cl.oldFrameServerTime`).
    pub frame_server_time: i32,
    /// The server's time minus the client's (`cl.serverTimeDelta`), kept in
    /// step with the snapshots by [`adjust_time_delta`].
    pub server_time_delta: i32,
    /// The client game's time got near the latest snapshot's
    /// (`cl.extrapolatedSnapshot`): the delta then drifts back.
    pub extrapolated_snapshot: i32,
    pub game_state: GameStateStrings,
    /// The next free entry of the entity ring (counts up, used modulo the size).
    pub parse_entities_num: i32,
    /// The next free entry of the client ring.
    pub parse_clients_num: i32,
    /// The client's last 32 commands' send times.
    pub out_packets: [OutPacket; PACKET_BACKUP],
    /// The last 32 snapshots, by message number modulo 32.
    pub snapshots: Vec<Snapshot>,
    /// Entity baselines (`entityState_t` each), by number.
    pub baselines: Vec<u8>,
    /// The entity ring (`entityState_t` each).
    pub parse_entities: Vec<u8>,
    /// The client ring (`clientState_t` each).
    pub parse_clients: Vec<u8>,
    /// The server's id from the system info (`sv_serverid`): its low byte
    /// starts each of the client's messages.
    pub server_id: i32,
    /// The client's last [`CMD_BACKUP`] player commands
    /// ([`delta::usercmd_size`] bytes each), by number modulo 128.
    pub cmds: Vec<u8>,
    /// The number of the latest player command.
    pub cmd_number: i32,
    /// The weapon the client game picked for the player's commands
    /// (`cl.cgameUserCmdValue`; its low byte goes in each command).
    pub user_cmd_weapon: i32,
    /// Call of Duty 2's second value from the client game (the off-hand
    /// item, at 0x0098FDCC; its low byte goes in each command).
    pub user_cmd_offhand: i32,
    /// Angles the client game adds to the view angles in each command
    /// (Call of Duty 2 at 0x0098FDE0, United Offensive at 0x04963790).
    pub cgame_angles: [f32; 3],
    /// Where the player looks (`cl.viewangles`), in degrees.
    pub view_angles: [f32; 3],
    // --- `clc` and `cls`: the connection (not cleared by a game state) ---
    pub client_num: i32,
    pub checksum_feed: i32,
    /// The challenge the server handed out (it keys the scrambling).
    pub challenge: i32,
    /// The last reliable command the client queued.
    pub reliable_sequence: i32,
    /// The last of them the server acknowledged.
    pub reliable_acknowledge: i32,
    /// The client's reliable commands, a ring of [`max_reliable_commands`]
    /// texts of 1024 bytes.
    pub reliable_commands: Vec<u8>,
    /// `cls.realtime` when the last packet went out.
    pub last_packet_sent_time: i32,
    /// `cls.realtime` when the last packet from the server came in.
    pub last_packet_time: i32,
    /// A demo is playing (nothing is sent).
    pub demo_playing: i32,
    /// A demo is being recorded.
    pub demo_recording: i32,
    /// The server being joined (`clc.serverAddress`).
    pub server_address: NetAddress,
    /// `cls.realtime` when the last `getchallenge` or `connect` went out
    /// (-99999: send the next one at once).
    pub connect_time: i32,
    /// The server's last message for the player (`print`, a cd key
    /// request): 256 bytes, the text and zeros.
    pub server_message: Vec<u8>,
    /// United Offensive: the server sends only the players the client can
    /// see (the third word of `challengeResponse`).
    pub only_visible_clients: i32,
    /// The client's `net_qport`: the number its packets carry so the server
    /// knows it behind a router that changes its port.
    pub qport: i32,
    /// The last server message received.
    pub server_message_sequence: i32,
    /// The last reliable command from the server.
    pub server_command_sequence: i32,
    /// The server's reliable commands, a ring of
    /// [`max_reliable_commands`] texts of 1024 bytes.
    pub server_commands: Vec<u8>,
    /// The last server command the client game took
    /// ([`get_server_command`]; `clc.lastExecutedServerCommand`).
    pub last_executed_server_command: i32,
    pub connect_packet_count: i32,
    /// Waiting for a full snapshot to start a demo recording.
    pub demo_waiting: i32,
    /// A demo's first frame was skipped (`clc.firstDemoFrameSkipped`).
    pub first_demo_frame_skipped: i32,
    /// `timedemo`: the demo plays as fast as it can (`clc.timeDemo`; Call of
    /// Duty 2's, United Offensive reads its `timedemo` cvar).
    pub time_demo: i32,
    /// The first snapshot's server time (`clc.timeDemoBaseTime`).
    pub time_demo_base_time: i32,
    /// The connection to the server (`clc.netchan`).
    pub chan: Netchan,
    /// The connection state (`cls.state`): see [`write_packet`].
    pub connection_state: i32,
    /// The server's name as the player gave it (`cls.servername`, 256
    /// bytes): a server called `localhost` may go back in time
    /// ([`set_cgame_time`]).
    pub server_name: Vec<u8>,
    /// Milliseconds since the client started (`cls.realtime`).
    pub realtime: i32,
    /// United Offensive's second clock (at 0x04AD3E80), which its client
    /// game's clock runs on ([`set_cgame_time`]): the frames' simulated
    /// milliseconds, the same as [`Self::realtime`]'s but in `cl_avidemo`'s
    /// fixed steps while it records. Call of Duty 2's client game runs on
    /// `realtime`.
    pub sim_time: i32,
}

/// Bytes of one server command in [`ClientState::server_commands`].
pub const SERVER_COMMAND_SIZE: usize = MAX_STRING_CHARS;
/// Bytes of [`ClientState::server_message`].
pub const SERVER_MESSAGE_SIZE: usize = 256;
/// Bytes of [`ClientState::server_name`].
pub const SERVER_NAME_SIZE: usize = 256;

impl ClientState {
    /// A client of `game` before any message.
    pub fn new(game: Game) -> ClientState {
        let entity = entity_state_size(game);
        ClientState {
            game,
            timeout_count: 0,
            snap: Snapshot::new(game),
            old_frame_server_time: 0,
            new_snapshots: 0,
            server_time: 0,
            old_server_time: 0,
            frame_server_time: 0,
            server_time_delta: 0,
            extrapolated_snapshot: 0,
            game_state: GameStateStrings::new(game),
            parse_entities_num: 0,
            parse_clients_num: 0,
            out_packets: [OutPacket::default(); PACKET_BACKUP],
            snapshots: (0..PACKET_BACKUP).map(|_| Snapshot::new(game)).collect(),
            baselines: vec![0; MAX_ENTITIES * entity],
            parse_entities: vec![0; MAX_PARSE_ENTITIES * entity],
            parse_clients: vec![0; MAX_PARSE_ENTITIES * CLIENT_STATE_SIZE],
            server_id: 0,
            cmds: vec![0; CMD_BACKUP * delta::usercmd_size(game)],
            cmd_number: 0,
            user_cmd_weapon: 0,
            user_cmd_offhand: 0,
            cgame_angles: [0.0; 3],
            view_angles: [0.0; 3],
            client_num: 0,
            checksum_feed: 0,
            challenge: 0,
            reliable_sequence: 0,
            reliable_acknowledge: 0,
            reliable_commands: vec![0; max_reliable_commands(game) * SERVER_COMMAND_SIZE],
            last_packet_sent_time: 0,
            last_packet_time: 0,
            demo_playing: 0,
            demo_recording: 0,
            server_address: NetAddress::default(),
            connect_time: 0,
            server_message: vec![0; SERVER_MESSAGE_SIZE],
            only_visible_clients: 0,
            qport: 0,
            server_message_sequence: 0,
            server_command_sequence: 0,
            server_commands: vec![0; max_reliable_commands(game) * SERVER_COMMAND_SIZE],
            last_executed_server_command: 0,
            connect_packet_count: 0,
            demo_waiting: 0,
            first_demo_frame_skipped: 0,
            time_demo: 0,
            time_demo_base_time: 0,
            chan: Netchan::new(game, NetSource::Client, NetAddress::default(), 0),
            connection_state: 0,
            server_name: vec![0; SERVER_NAME_SIZE],
            realtime: 0,
            sim_time: 0,
        }
    }

    /// Clears what the original's `CL_ClearState` clears: everything of `cl`
    /// (the connection's fields stay).
    pub fn clear(&mut self) {
        self.timeout_count = 0;
        self.snap.clear();
        self.old_frame_server_time = 0;
        self.new_snapshots = 0;
        self.server_time = 0;
        self.old_server_time = 0;
        self.frame_server_time = 0;
        self.server_time_delta = 0;
        self.extrapolated_snapshot = 0;
        self.game_state.offsets.fill(0);
        self.game_state.data.fill(0);
        self.game_state.data_count = 0;
        self.parse_entities_num = 0;
        self.parse_clients_num = 0;
        self.out_packets = [OutPacket::default(); PACKET_BACKUP];
        for s in &mut self.snapshots {
            s.clear();
        }
        self.baselines.fill(0);
        self.parse_entities.fill(0);
        self.parse_clients.fill(0);
        self.server_id = 0;
        self.cmds.fill(0);
        self.cmd_number = 0;
        self.user_cmd_weapon = 0;
        self.user_cmd_offhand = 0;
        self.cgame_angles = [0.0; 3];
        self.view_angles = [0.0; 3];
    }

    /// Server command `sequence`'s text (from the ring).
    pub fn server_command(&self, sequence: i32) -> &[u8] {
        let slot = sequence as u32 as usize % max_reliable_commands(self.game);
        let text = &self.server_commands[slot * SERVER_COMMAND_SIZE..][..SERVER_COMMAND_SIZE];
        &text[..crate::cstr::len(text)]
    }

    fn entity_range(&self, ring_index: i32) -> std::ops::Range<usize> {
        let size = entity_state_size(self.game);
        let at = (ring_index as u32 as usize & (MAX_PARSE_ENTITIES - 1)) * size;
        at..at + size
    }

    fn client_range(ring_index: i32) -> std::ops::Range<usize> {
        let at = (ring_index as u32 as usize & (MAX_PARSE_ENTITIES - 1)) * CLIENT_STATE_SIZE;
        at..at + CLIENT_STATE_SIZE
    }
}

/// Reads one reliable command (its sequence number and text) and keeps it
/// when it is newer than the last one: copied into its slot of the ring,
/// at most 1023 characters, the rest of the slot zeroed.
///
/// Original: `CL_ParseCommandString` at 0x00414380 (CoD2MP_s.exe 1.3; CoD2x calls it `CL_ServerCommand`)
/// Original: `CL_ParseCommandString` at 0x004194A0 (CoDUOMP.exe 1.51)
pub fn parse_command_string(msg: &mut Msg, cl: &mut ClientState) {
    let sequence = msg.read_long();
    let mut text = [0u8; MAX_STRING_CHARS];
    let s = msg.read_string(&mut text);
    if cl.server_command_sequence < sequence {
        cl.server_command_sequence = sequence;
        let slot = sequence as u32 as usize % max_reliable_commands(cl.game);
        let dest = &mut cl.server_commands[slot * SERVER_COMMAND_SIZE..][..SERVER_COMMAND_SIZE];
        crate::crt::strncpy(&mut dest[..SERVER_COMMAND_SIZE - 1], s);
        dest[SERVER_COMMAND_SIZE - 1] = 0;
    }
}

/// Reads a game state into `cl`: the client's `cl` is cleared, then the
/// server command sequence, every config string and baseline, the end
/// marker, the client number and the checksum feed. Each config string's
/// text goes after the others in [`GameStateStrings`]; a baseline is read as
/// a change from an all-zero entity.
///
/// Errors (all `ERR_DROP`, as the originals): a config string index outside
/// 0-2047, more text than [`max_game_state_chars`], a baseline number
/// outside 0-1023, any other byte where a part's type should be (the message
/// running out included).
///
/// The originals then apply the system info, restart the file system,
/// authorize and start downloads: the caller's part (see the module).
/// The originals also close the console first (`Con_Close`).
///
/// Original: `CL_ParseGamestate` at 0x00413BD0 (CoD2MP_s.exe 1.3; the message part)
/// Original: `CL_ParseGamestate` at 0x00418D10 (CoDUOMP.exe 1.51; the message part)
pub fn parse_game_state(
    msg: &mut Msg,
    tables: &FieldTables,
    cl: &mut ClientState,
) -> Result<(), GameError> {
    let game = cl.game;
    let ops = ops(game);
    cl.connect_packet_count = 0;
    cl.clear();
    cl.server_command_sequence = msg.read_long();
    cl.game_state.data_count = 1;
    let bad_command = || {
        match game {
        Game::CallOfDuty2 => GameError::drop(
            "CL_ParseGamestate: bad command byte (GameState may exceed MAX_GAMESTATE_CHARS, or packet may exceed MAX_MSGLEN)",
        ),
        Game::UnitedOffensive => GameError::drop("CL_ParseGamestate: bad command byte"),
    }
    };
    let entity = entity_state_size(game);
    let zeros = vec![0u8; entity];
    loop {
        let op = msg.read_byte();
        if op == i32::from(ops.eof) {
            break;
        }
        if op == i32::from(ops.config_string) {
            let index = msg.read_short();
            if !(0..MAX_CONFIG_STRINGS as i32).contains(&index) {
                return Err(GameError::drop("configstring > MAX_CONFIGSTRINGS"));
            }
            let mut text = [0u8; BIG_INFO_STRING];
            let s = msg.read_big_string(&mut text);
            let gs = &mut cl.game_state;
            let start = gs.data_count;
            let end = start as i64 + s.len() as i64 + 1;
            if end > max_game_state_chars(game) as i64 {
                return Err(GameError::drop("MAX_GAMESTATE_CHARS exceeded"));
            }
            gs.offsets[index as usize] = start;
            let at = start as usize;
            gs.data[at..at + s.len()].copy_from_slice(s);
            gs.data[at + s.len()] = 0;
            gs.data_count = end as i32;
        } else if op == i32::from(ops.baseline) {
            let number = msg.read_bits(delta::ENTITY_NUMBER_BITS);
            if !(0..MAX_ENTITIES as i32).contains(&number) {
                return Err(GameError::drop(format!(
                    "Baseline number out of range: {number}"
                )));
            }
            let at = number as usize * entity;
            let baseline = &mut cl.baselines[at..at + entity];
            delta::read_delta_entity(msg, tables, &zeros, baseline, number);
        } else {
            return Err(bad_command());
        }
    }
    cl.client_num = msg.read_long();
    cl.checksum_feed = msg.read_long();
    Ok(())
}

/// Reads a snapshot's entities: each one the message names is read as a
/// change from its state in `old` (the snapshot this one changes, `None`
/// when there is none) or, when `old` doesn't have it, from its baseline;
/// the entities of `old` the message doesn't name are carried over
/// unchanged. They go into the entity ring from `cl.parse_entities_num`,
/// sorted by number, and `new` records where they start and how many.
///
/// `old` is an index into `cl.snapshots`. The ring entries `old` refers to
/// must not be the ones being written (`parse_snapshot` checks it: at most
/// [`MAX_RING_DISTANCE`] back).
///
/// Errors: the message read past its end (`ERR_DROP`).
///
/// Original: `CL_ParsePacketEntities` at 0x00412FB0 (CoD2MP_s.exe 1.3)
/// Original: `CL_ParsePacketEntities` at 0x00418150 (CoDUOMP.exe 1.51)
pub fn parse_packet_entities(
    msg: &mut Msg,
    tables: &FieldTables,
    cl: &mut ClientState,
    old: Option<usize>,
    new: &mut Snapshot,
) -> Result<(), GameError> {
    let game = cl.game;
    let cod2 = game == Game::CallOfDuty2;
    new.parse_entities_num = cl.parse_entities_num;
    new.num_entities = 0;
    let (old_count, old_start) = match old {
        Some(i) => (
            cl.snapshots[i].num_entities,
            cl.snapshots[i].parse_entities_num,
        ),
        None => (0, 0),
    };
    let size = entity_state_size(game);
    let mut old_index = 0;
    // The old entity being compared: its ring index and its number.
    let number_at =
        |cl: &ClientState, ring: i32| delta::word_at(&cl.parse_entities[cl.entity_range(ring)], 0);
    let mut old_ring = old_start;
    let mut old_num = if old.is_some() && old_count >= 1 {
        number_at(cl, old_ring)
    } else {
        LIST_END
    };
    let mut old_state = vec![0u8; size];
    let next_old = |cl: &ClientState, old_index: i32, old_ring: &mut i32| -> i32 {
        if old_index < old_count {
            *old_ring = old_start.wrapping_add(old_index);
            number_at(cl, *old_ring)
        } else {
            LIST_END
        }
    };
    let end_error = || GameError::drop("CL_ParsePacketEntities: end of message");
    // Carries old entity `ring` over unchanged.
    let copy_unchanged = |cl: &mut ClientState, ring: i32, new: &mut Snapshot| {
        let from = cl.entity_range(ring);
        let to = cl.entity_range(cl.parse_entities_num);
        cl.parse_entities.copy_within(from, to.start);
        cl.parse_entities_num = cl.parse_entities_num.wrapping_add(1);
        new.num_entities += 1;
    };
    let mut new_num = if cod2 && msg.overflowed {
        ENTITY_LIST_END
    } else {
        msg.read_bits(delta::ENTITY_NUMBER_BITS)
    };
    while new_num != ENTITY_LIST_END {
        if msg.cursize < msg.readcount {
            return Err(end_error());
        }
        while old_num < new_num && !(cod2 && msg.overflowed) {
            copy_unchanged(cl, old_ring, new);
            old_index += 1;
            old_num = next_old(cl, old_index, &mut old_ring);
        }
        let to = cl.entity_range(cl.parse_entities_num);
        let from_range = if old_num == new_num {
            Some(cl.entity_range(old_ring))
        } else {
            None
        };
        match &from_range {
            Some(r) => old_state.copy_from_slice(&cl.parse_entities[r.clone()]),
            None if new_num >= 0 => {
                let at = new_num as usize * size;
                old_state.copy_from_slice(&cl.baselines[at..at + size]);
            }
            // Call of Duty 2's number of a message that ran out: -1, whose
            // "baseline" is what precedes the baselines in the game's `cl`.
            None => old_state.copy_from_slice(&snapshot_tail(cl, PACKET_BACKUP - 1, size)),
        }
        let removed =
            delta::read_delta_entity(msg, tables, &old_state, &mut cl.parse_entities[to], new_num);
        if !removed {
            cl.parse_entities_num = cl.parse_entities_num.wrapping_add(1);
            new.num_entities += 1;
        }
        if from_range.is_some() {
            old_index += 1;
            old_num = next_old(cl, old_index, &mut old_ring);
        }
        new_num = if cod2 && msg.overflowed {
            ENTITY_LIST_END
        } else {
            msg.read_bits(delta::ENTITY_NUMBER_BITS)
        };
    }
    // The old entities after the last one the message names.
    while old_num != LIST_END && !(cod2 && msg.overflowed) {
        copy_unchanged(cl, old_ring, new);
        old_index += 1;
        old_num = next_old(cl, old_index, &mut old_ring);
    }
    Ok(())
}

/// The last `len` bytes of snapshot `index` as the game lays it out
/// (`clSnapshot_t`: seven numbers, the player state, five numbers).
fn snapshot_tail(cl: &ClientState, index: usize, len: usize) -> Vec<u8> {
    let s = &cl.snapshots[index];
    let mut bytes = Vec::with_capacity(0x30 + s.player_state.len());
    for v in [
        s.valid,
        s.snap_flags,
        s.server_time,
        s.message_num,
        s.delta_num,
        s.ping,
        s.cmd_num,
    ] {
        bytes.extend_from_slice(&v.to_le_bytes());
    }
    bytes.extend_from_slice(&s.player_state);
    for v in [
        s.num_entities,
        s.num_clients,
        s.parse_entities_num,
        s.parse_clients_num,
        s.server_command_num,
    ] {
        bytes.extend_from_slice(&v.to_le_bytes());
    }
    bytes[bytes.len() - len..].to_vec()
}

/// Reads a snapshot's clients (every player's name, team and the like) the
/// same way as [`parse_packet_entities`]: each the message names (a 1 bit,
/// then its number) as a change from its state in `old`, or from an empty
/// client; the rest of `old`'s carried over. Into the client ring from
/// `cl.parse_clients_num`.
///
/// Errors: the message read past its end (`ERR_DROP`).
///
/// Original: `CL_ParsePacketClients` at 0x00413310 (CoD2MP_s.exe 1.3)
/// Original: `CL_ParsePacketClients` at 0x004184E0 (CoDUOMP.exe 1.51)
pub fn parse_packet_clients(
    msg: &mut Msg,
    tables: &FieldTables,
    cl: &mut ClientState,
    old: Option<usize>,
    new: &mut Snapshot,
) -> Result<(), GameError> {
    let cod2 = cl.game == Game::CallOfDuty2;
    new.parse_clients_num = cl.parse_clients_num;
    new.num_clients = 0;
    let (old_count, old_start) = match old {
        Some(i) => (
            cl.snapshots[i].num_clients,
            cl.snapshots[i].parse_clients_num,
        ),
        None => (0, 0),
    };
    let number_at = |cl: &ClientState, ring: i32| {
        delta::word_at(&cl.parse_clients[ClientState::client_range(ring)], 0)
    };
    let mut old_index = 0;
    let mut old_ring = old_start;
    let mut old_num = if old.is_some() && old_count >= 1 {
        number_at(cl, old_ring)
    } else {
        LIST_END
    };
    let next_old = |cl: &ClientState, old_index: i32, old_ring: &mut i32| -> i32 {
        if old_index < old_count {
            *old_ring = old_start.wrapping_add(old_index);
            number_at(cl, *old_ring)
        } else {
            LIST_END
        }
    };
    let copy_unchanged = |cl: &mut ClientState, ring: i32, new: &mut Snapshot| {
        let from = ClientState::client_range(ring);
        let to = ClientState::client_range(cl.parse_clients_num);
        cl.parse_clients.copy_within(from, to.start);
        cl.parse_clients_num = cl.parse_clients_num.wrapping_add(1);
        new.num_clients += 1;
    };
    let mut old_state = [0u8; CLIENT_STATE_SIZE];
    loop {
        if cod2 && msg.overflowed {
            break;
        }
        // One more client? Call of Duty 2 reads the bit by hand: a message
        // that has run out marks itself overflowed and goes on as if the bit
        // were 1, reading a client numbered -1 (then the list ends).
        if msg.read_bit() == 0 {
            break;
        }
        let new_num = msg.read_bits(delta::CLIENT_NUMBER_BITS);
        if msg.cursize < msg.readcount {
            return Err(GameError::drop("CL_ParsePacketClients: end of message"));
        }
        while old_num < new_num {
            copy_unchanged(cl, old_ring, new);
            old_index += 1;
            old_num = next_old(cl, old_index, &mut old_ring);
        }
        let to = ClientState::client_range(cl.parse_clients_num);
        let delta_from_old = old_num == new_num;
        let from = if delta_from_old {
            old_state.copy_from_slice(&cl.parse_clients[ClientState::client_range(old_ring)]);
            Some(&old_state[..])
        } else {
            None
        };
        let removed =
            delta::read_delta_client(msg, tables, from, &mut cl.parse_clients[to], new_num);
        if !removed {
            cl.parse_clients_num = cl.parse_clients_num.wrapping_add(1);
            new.num_clients += 1;
        }
        if delta_from_old {
            old_index += 1;
            old_num = next_old(cl, old_index, &mut old_ring);
        }
    }
    while old_num != LIST_END && !(cod2 && msg.overflowed) {
        copy_unchanged(cl, old_ring, new);
        old_index += 1;
        old_num = next_old(cl, old_index, &mut old_ring);
    }
    Ok(())
}

/// Reads a snapshot (after its message type): the server time, which
/// snapshot it changes and the flags, then the player's state, the entities
/// and the clients, each as a change from that snapshot. A complete one (the
/// client still has the snapshot it changes, recent enough) becomes
/// `cl.snap`, with the ping of the command it answers, and is kept in
/// `cl.snapshots`; the older snapshots it skipped are marked incomplete.
///
/// Call of Duty 2 drops a snapshot whose message ran out; United Offensive
/// keeps it.
///
/// Original: `CL_ParseSnapshot` at 0x004136C0 (CoD2MP_s.exe 1.3)
/// Original: `CL_ParseSnapshot` at 0x00418870 (CoDUOMP.exe 1.51)
pub fn parse_snapshot(
    msg: &mut Msg,
    tables: &FieldTables,
    cl: &mut ClientState,
) -> Result<(), GameError> {
    let game = cl.game;
    let mut new = Snapshot::new(game);
    new.server_command_num = cl.server_command_sequence;
    new.server_time = msg.read_long();
    new.message_num = cl.server_message_sequence;
    let delta_byte = msg.read_byte();
    new.delta_num = if delta_byte == 0 {
        -1
    } else {
        new.message_num.wrapping_sub(delta_byte)
    };
    new.snap_flags = msg.read_byte();
    let old = if new.delta_num < 1 {
        cl.demo_waiting = 0;
        new.valid = 1;
        None
    } else {
        let i = (new.delta_num as u32 as usize) % PACKET_BACKUP;
        let o = &cl.snapshots[i];
        let complete = o.valid != 0
            && o.message_num == new.delta_num
            && cl.parse_entities_num.wrapping_sub(o.parse_entities_num) <= MAX_RING_DISTANCE
            && cl.parse_clients_num.wrapping_sub(o.parse_clients_num) <= MAX_RING_DISTANCE;
        if complete {
            new.valid = 1;
        }
        // The originals keep reading from the old snapshot even when it's
        // incomplete (the result is then thrown away).
        Some(i)
    };
    {
        let from = old.map(|i| cl.snapshots[i].player_state.clone());
        delta::read_delta_playerstate(msg, tables, from.as_deref(), &mut new.player_state);
    }
    parse_packet_entities(msg, tables, cl, old, &mut new)?;
    parse_packet_clients(msg, tables, cl, old, &mut new)?;
    if game == Game::CallOfDuty2 && msg.overflowed {
        return Ok(());
    }
    if new.valid == 0 {
        return Ok(());
    }
    // Snapshots skipped since the last one can't be deltas any more.
    let mut skipped = cl.snap.message_num.wrapping_add(1);
    if new.message_num.wrapping_sub(skipped) >= PACKET_BACKUP as i32 {
        skipped = new.message_num.wrapping_sub(PACKET_BACKUP as i32 - 1);
    }
    while skipped < new.message_num {
        cl.snapshots[skipped as u32 as usize % PACKET_BACKUP].valid = 0;
        skipped = skipped.wrapping_add(1);
    }
    cl.old_frame_server_time = cl.snap.server_time;
    cl.snap.copy_from(&new);
    cl.snap.ping = 999;
    let command_time = delta::word_at(&cl.snap.player_state, 0);
    let mut packet = cl.chan.outgoing_sequence;
    for _ in 0..PACKET_BACKUP {
        packet = packet.wrapping_sub(1);
        let p = cl.out_packets[packet as u32 as usize % PACKET_BACKUP];
        if p.server_time <= command_time {
            cl.snap.ping = cl.realtime.wrapping_sub(p.realtime);
            break;
        }
    }
    let slot = cl.snap.message_num as u32 as usize % PACKET_BACKUP;
    let snap = cl.snap.clone();
    cl.snapshots[slot].copy_from(&snap);
    cl.new_snapshots = 1;
    Ok(())
}

/// What the caller does with the parts of a server message the ports leave
/// to it.
/// Each returns whether to go on reading the message.
pub trait ServerMessageParts {
    /// After a game state: apply the system info (config string 1), restart
    /// the file system, authorize, start downloads (`CL_SystemInfoChanged`
    /// and what follows it in `CL_ParseGamestate`).
    fn game_state_parsed(&mut self, cl: &mut ClientState) -> bool;
    /// A download block (`svc_download`), to read from `msg` (`CL_ParseDownload`).
    fn download(&mut self, msg: &mut Msg, cl: &mut ClientState) -> bool;
}

/// Reads one message from the server (`msg` after the netchan header and
/// the acknowledge: what follows is Huffman-coded): decompressed with the
/// message tree into a buffer of the game's largest message, then each part
/// by its type until the end marker. Running out of data before it, or an
/// unknown type, is the original's error ("Illegible server message").
/// Call of Duty 2 also stops, without an error, after a part that left the
/// message overflowed (it read past the end, or a structure's field count
/// was past its table). Returns whether it got to the end (false: one of
/// `parts` stopped it).
///
/// Original: `CL_ParseServerMessage` at 0x004143E0 (CoD2MP_s.exe 1.3)
/// Original: `CL_ParseServerMessage` at 0x00419500 (CoDUOMP.exe 1.51)
pub fn parse_server_message(
    msg: &mut Msg,
    tree: &MessageTree,
    tables: &FieldTables,
    cl: &mut ClientState,
    parts: &mut dyn ServerMessageParts,
) -> Result<bool, GameError> {
    let game = cl.game;
    let ops = ops(game);
    let size = crate::net::netchan::max_message_length(game);
    let mut buffer = vec![0u8; size];
    let start = msg.readcount.min(msg.data.len());
    let end = msg.cursize.min(msg.data.len()).max(start);
    let count = tree.read_bits_compress(&msg.data[start..end], &mut buffer);
    let mut m = Msg::new(game, &mut buffer);
    m.cursize = count;
    loop {
        if game == Game::UnitedOffensive && m.cursize < m.readcount {
            return Err(GameError::drop(
                "CL_ParseServerMessage: read past end of server message",
            ));
        }
        let op = if m.readcount < m.cursize {
            // (A message can decode to more than the buffer holds: zeros past it.)
            let op = i32::from(m.data.get(m.readcount).copied().unwrap_or(0));
            m.readcount += 1;
            if op == i32::from(ops.eof) {
                return Ok(true);
            }
            op
        } else {
            -1
        };
        if op == i32::from(ops.nop) {
            continue;
        }
        if op == i32::from(ops.game_state) {
            parse_game_state(&mut m, tables, cl)?;
            if !parts.game_state_parsed(cl) {
                return Ok(false);
            }
        } else if op == i32::from(ops.server_command) {
            parse_command_string(&mut m, cl);
        } else if op == i32::from(ops.download) {
            if !parts.download(&mut m, cl) {
                return Ok(false);
            }
        } else if op == i32::from(ops.snapshot) {
            parse_snapshot(&mut m, tables, cl)?;
        } else {
            return Err(GameError::drop(format!(
                "CL_ParseServerMessage: Illegible server message {op}"
            )));
        }
        // Call of Duty 2 stops at a part that read past its message's end
        // or found a field count past its table.
        if game == Game::CallOfDuty2 && m.overflowed {
            return Ok(true);
        }
    }
}

/// The client's message types (`clc_ops_e`): 3 bits in Call of Duty 2, 2 in
/// United Offensive.
const CLC_MOVE: i32 = 0;
const CLC_MOVE_NO_DELTA: i32 = 1;
const CLC_CLIENT_COMMAND: i32 = 2;
const CLC_EOF: i32 = 3;

fn clc_bits(game: Game) -> u32 {
    match game {
        Game::CallOfDuty2 => 3,
        Game::UnitedOffensive => 2,
    }
}

/// The client's settings [`write_packet`] reads.
#[derive(Debug, Clone, Copy, Default)]
pub struct SendSettings {
    /// `cl_packetdup`: how many earlier packets' player commands to send
    /// again (0-5). United Offensive sets the cvar back into that range
    /// first (Call of Duty 2's dvar can't leave it).
    pub packet_dup: i32,
    /// `cl_nodelta`: never code the snapshot acknowledged as a delta base.
    pub no_delta: bool,
}

/// Whether the original sends nothing in this connection state (`cls.state`):
/// Call of Duty 2 in states 1 and 2, United Offensive in 7 and 8. (The two
/// number their states differently: a challenge is taken in state 3 / 1,
/// the connect answer in 4 / 2, which leaves the client in 5 / 3, connected,
/// as `CL_ConnectionlessPacket` shows.)
pub fn sends_nothing_in(game: Game, state: i32) -> bool {
    match game {
        Game::CallOfDuty2 => state == 1 || state == 2,
        Game::UnitedOffensive => state == 7 || state == 8,
    }
}

/// Builds and sends the client's message to the server (nothing while a
/// demo plays or in [`sends_nothing_in`] states):
///
/// 1. raw: the server id's low byte, the last server message and the last
///    server command received;
/// 2. each reliable command the server hasn't acknowledged
///    (`clc_clientCommand`, its number, its text);
/// 3. the player commands made since the packet `packet_dup + 1` packets
///    back (at most [`MAX_PACKET_USERCMDS`]; none: this part is left out):
///    `clc_move` when the last snapshot can serve the server as a delta base
///    (`clc_moveNoDelta` otherwise), their count, each coded against the one
///    before it (the first against [`delta::set_default_usercmd`] of the
///    latest snapshot's player state), keyed with the checksum feed, the
///    last server message and the hash of the last server command's text;
/// 4. `clc_EOF`.
///
/// Bytes 9 on are Huffman-coded and scrambled ([`netchan::client_encode_key`],
/// the last server command's text); the packet is recorded in
/// `out_packets` (for the ping), and the channel sends it, in fragments if
/// it has to: `send` gets each packet and the address it goes to.
///
/// The originals' debug output (`cl_showSend`, "MAX_PACKET_USERCMDS") is
/// left out.
///
/// Original: `CL_WritePacket` at 0x00408E80 (CoD2MP_s.exe 1.3)
/// Original: `CL_WritePacket` at 0x0040CC80 (CoDUOMP.exe 1.51)
pub fn write_packet(
    cl: &mut ClientState,
    tree: &MessageTree,
    settings: &SendSettings,
    send: &mut dyn FnMut(&NetAddress, &[u8]),
) -> Result<(), GameError> {
    let game = cl.game;
    if cl.demo_playing != 0 || sends_nothing_in(game, cl.connection_state) {
        return Ok(());
    }
    let size = delta::usercmd_size(game);
    let mut previous = vec![0u8; size];
    delta::set_default_usercmd(game, &cl.snap.player_state, &mut previous);
    let mut buffer = vec![0u8; netchan::max_message_length(game)];
    let mut msg = Msg::new(game, &mut buffer);
    let bits = clc_bits(game);
    msg.write_byte(cl.server_id as u8);
    msg.write_long(cl.server_message_sequence);
    msg.write_long(cl.server_command_sequence);
    let ring = max_reliable_commands(game);
    let mut i = cl.reliable_acknowledge.wrapping_add(1);
    while i <= cl.reliable_sequence {
        msg.write_bits(CLC_CLIENT_COMMAND, bits);
        msg.write_long(i);
        let slot = (i as u32 as usize % ring) * SERVER_COMMAND_SIZE;
        let text = &cl.reliable_commands[slot..slot + SERVER_COMMAND_SIZE];
        // (A slot holds at most 1023 characters: never too long to write.)
        let _ = msg.write_string(&text[..crate::cstr::len(text)]);
        i = i.wrapping_add(1);
    }
    let packet_dup = match game {
        Game::CallOfDuty2 => settings.packet_dup,
        Game::UnitedOffensive => settings.packet_dup.clamp(0, 5),
    };
    let back = cl
        .chan
        .outgoing_sequence
        .wrapping_sub(packet_dup)
        .wrapping_sub(1);
    let old_packet = cl.out_packets[(back & 31) as usize];
    let count = cl
        .cmd_number
        .wrapping_sub(old_packet.cmd_number)
        .min(MAX_PACKET_USERCMDS);
    if count >= 1 {
        let delta_base = !settings.no_delta
            && cl.snap.valid != 0
            && cl.demo_waiting == 0
            && cl.server_message_sequence == cl.snap.message_num;
        msg.write_bits(
            if delta_base {
                CLC_MOVE
            } else {
                CLC_MOVE_NO_DELTA
            },
            bits,
        );
        msg.write_byte(count as u8);
        let text = cl.server_command(cl.server_command_sequence);
        let key = cl.checksum_feed ^ cl.server_message_sequence ^ hash_key(text, 32);
        for k in 0..count {
            let number = cl
                .cmd_number
                .wrapping_sub(count)
                .wrapping_add(1)
                .wrapping_add(k);
            let at = (number as u32 as usize % CMD_BACKUP) * size;
            let cmd = &cl.cmds[at..at + size];
            delta::write_delta_usercmd_key(&mut msg, key, &previous, cmd);
            previous.copy_from_slice(cmd);
        }
    }
    msg.write_bits(CLC_EOF, bits);
    let length = msg.cursize;
    // The packet: the 9 header bytes as they are, the rest coded.
    let mut data = vec![0u8; 9 + 4 * length + 64];
    data[..9].copy_from_slice(&buffer[..9]);
    let coded = tree.write_bits_compress(&buffer[9..length], &mut data[9..]);
    let slot = (cl.chan.outgoing_sequence & 31) as usize;
    cl.out_packets[slot] = OutPacket {
        cmd_number: cl.cmd_number,
        server_time: delta::word_at(&previous, 0),
        realtime: cl.realtime,
    };
    cl.last_packet_sent_time = cl.realtime;
    let key = netchan::client_encode_key(cl.challenge, cl.server_id, cl.server_message_sequence);
    let text = cl.server_command(cl.server_command_sequence).to_vec();
    netchan::scramble(&mut data[9..9 + coded], key, &text);
    let mut packet = Packet::default();
    cl.chan.transmit(&data[..9 + coded], &mut packet)?;
    send(&cl.chan.remote_address, packet.as_bytes());
    while cl.chan.unsent_fragments {
        cl.chan.transmit_next_fragment(&mut packet);
        send(&cl.chan.remote_address, packet.as_bytes());
    }
    Ok(())
}

/// What [`packet_event`] did with a packet.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum PacketOutcome {
    /// A packet without a connection (it starts with four 0xFF bytes): the
    /// caller's connectionless handling takes it (`CL_ConnectionlessPacket`).
    Connectionless,
    /// The client isn't connected yet: ignored.
    NotConnected,
    /// Shorter than its 4-byte sequence number ("%s: Runt packet").
    Runt,
    /// Not from the server's address ("%s:sequenced packet without connection").
    NotFromServer,
    /// The channel dropped it: late, a duplicate, or a fragment that doesn't
    /// finish its message ([`Netchan::process`]).
    Dropped,
    /// It acknowledges a reliable command older than the ring holds: the
    /// acknowledge jumps to the last command sent and the packet is dropped.
    TooOld,
    /// Read: the message was parsed ([`parse_server_message`]), as far as
    /// `parts` let it go. `demo`: the original now writes the message into
    /// the demo being recorded (`CL_WriteDemoMessage`, the caller's part).
    Read { demo: bool },
    /// Call of Duty 2: the message turned out unreadable (the packet's
    /// message was overflowed after parsing, "ignoring illegible message"):
    /// the last message and acknowledge numbers are put back.
    Illegible,
}

impl PacketOutcome {
    /// Call of Duty 2's `CL_PacketEvent` result: true for a runt packet and
    /// for a message read.
    pub fn cod2_result(self) -> bool {
        matches!(self, PacketOutcome::Runt | PacketOutcome::Read { .. })
    }
}

/// The connection state from which a client takes sequenced packets (its
/// `cls.state` past this): connected, Call of Duty 2 5, United Offensive 3.
fn connected_state(game: Game) -> i32 {
    match game {
        Game::CallOfDuty2 => 5,
        Game::UnitedOffensive => 3,
    }
}

/// A packet from the network (`msg` holds it as it arrived, from `from`):
/// a connectionless one goes back to the caller; a connected client takes
/// one from the server's address through the channel, notes the message's
/// number (its first 4 bytes) and the reliable command it acknowledges,
/// unscrambles the rest ([`netchan::client_decode_key`], the acknowledged
/// command's text) and parses it ([`parse_server_message`]; when one of
/// `parts` stops the message, nothing after it is done).
///
/// Original: `CL_PacketEvent` at 0x0040F250 (CoD2MP_s.exe 1.3; msg in eax, from on the stack)
/// Original: `CL_PacketEvent` at 0x004131B0 (CoDUOMP.exe 1.51; msg in ecx, from on the stack)
pub fn packet_event(
    cl: &mut ClientState,
    from: &NetAddress,
    msg: &mut Msg,
    tree: &MessageTree,
    tables: &FieldTables,
    parts: &mut dyn ServerMessageParts,
) -> Result<PacketOutcome, GameError> {
    let game = cl.game;
    if msg.cursize >= 4 && msg.data.get(..4) == Some(&[0xFF; 4][..]) {
        return Ok(PacketOutcome::Connectionless);
    }
    if cl.connection_state < connected_state(game) {
        return Ok(PacketOutcome::NotConnected);
    }
    if msg.cursize < 4 {
        return Ok(PacketOutcome::Runt);
    }
    if !crate::net::oob::same_address(from, &cl.chan.remote_address) {
        return Ok(PacketOutcome::NotFromServer);
    }
    cl.last_packet_time = cl.realtime;
    if !cl.chan.process(msg) {
        return Ok(PacketOutcome::Dropped);
    }
    let saved = (cl.server_message_sequence, cl.reliable_acknowledge);
    cl.server_message_sequence = delta::word_at(msg.data, 0);
    let acknowledge = msg.read_long();
    let ring = max_reliable_commands(game);
    if acknowledge < cl.reliable_sequence.wrapping_sub(ring as i32) {
        cl.reliable_acknowledge = cl.reliable_sequence;
        return Ok(PacketOutcome::TooOld);
    }
    cl.reliable_acknowledge = acknowledge;
    let slot = (acknowledge as u32 as usize % ring) * SERVER_COMMAND_SIZE;
    let text = cl.reliable_commands[slot..slot + SERVER_COMMAND_SIZE].to_vec();
    let key = netchan::client_decode_key(cl.challenge, cl.server_message_sequence);
    let start = msg.readcount.min(msg.data.len());
    let end = msg.cursize.min(msg.data.len()).max(start);
    netchan::scramble(&mut msg.data[start..end], key, &text);
    if !parse_server_message(msg, tree, tables, cl, parts)? {
        // One of `parts` stopped the message: nothing after it happens.
        return Ok(PacketOutcome::Read { demo: false });
    }
    if game == Game::CallOfDuty2 && msg.overflowed {
        (cl.server_message_sequence, cl.reliable_acknowledge) = saved;
        return Ok(PacketOutcome::Illegible);
    }
    Ok(PacketOutcome::Read {
        demo: cl.demo_recording != 0 && cl.demo_waiting == 0,
    })
}

/// What [`connectionless_packet`] did with a packet without a connection.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum ClientPacket {
    /// Text starting with `pb_` (any case): PunkBuster's own packets, for
    /// PunkBuster when it runs (a Rust client leaves them).
    PunkBuster,
    /// Call of Duty 2's `v`: voice chat data (`CL_VoicePacket`, the caller's).
    Voice,
    /// `challengeResponse` while connecting: the challenge and the server's
    /// address are kept, and the client goes on to send `connect`.
    ChallengeTaken,
    /// `challengeResponse` at any other time ("Unwanted challenge response
    /// received.  Ignored.").
    ChallengeUnwanted,
    /// `connectResponse` while sending `connect`, from the server: the
    /// channel is set up and the client is connected.
    Connected,
    /// `connectResponse` once connected ("Dup connect received.  Ignored.").
    DuplicateConnect,
    /// `connectResponse` before the challenge ("... while not connecting.  Ignored.").
    NotConnecting,
    /// `connectResponse` from another address ("connectResponse from a
    /// different address.  Ignored.").
    ConnectFromElsewhere,
    /// `infoResponse`: a server's answer to `getinfo` (`CL_ServerInfoPacket`, the caller's).
    Info,
    /// `statusResponse` (`CL_ServerStatusResponse`, the caller's).
    Status,
    /// `disconnect` (`CL_ServerDisconnect`, the caller's).
    Disconnect,
    /// `echo`: the second word goes back to the sender (sent).
    Echo,
    /// `keyAuthorize`: ignored.
    KeyAuthorize,
    /// United Offensive's `motd`: the message of the day (`CL_MotdPacket`, the caller's).
    Motd,
    /// `print`: the text (the message's next string) is kept in
    /// [`ClientState::server_message`] (255 characters) and printed.
    Print,
    /// `error` from the server while connecting or connected: the original
    /// drops the connection with this text (`Com_Error`, `ERR_DROP`; United
    /// Offensive looks the text up in its localized strings first). The
    /// caller's part.
    ServerError(Vec<u8>),
    /// `error` from anyone else, or while not connecting: ignored.
    ErrorIgnored,
    /// `updateResponse` (`CL_UpdateInfoPacket`, the caller's).
    Update,
    /// Starting `getserversResponse`: a master server's list (`CL_ServersResponsePacket`, the caller's).
    Servers,
    /// Starting `needcdkey`: [`ClientState::server_message`] becomes
    /// `EXE_AWAITINGCDKEYAUTH`; the original then shows it and disconnects
    /// (the caller's part).
    NeedCdKey,
    /// `loadingnewmap` from the server: the map's name (the message's next
    /// line; Call of Duty 2 keeps 63 characters), the line after it read
    /// and dropped; the original then sets up for it
    /// (`CL_SetupForNewServerMap`, the caller's) and, in Call of Duty 2,
    /// goes back to the connected state (5).
    NewMap(Vec<u8>),
    /// `loadingnewmap` from anyone else: ignored.
    NewMapIgnored,
    /// Anything else: ignored.
    Unknown,
}

impl ClientPacket {
    /// Call of Duty 2's `CL_ConnectionlessPacket` result: false for the
    /// packets it ignores as unwanted, duplicate, misplaced or unknown.
    pub fn cod2_result(&self) -> bool {
        !matches!(
            self,
            ClientPacket::ChallengeUnwanted
                | ClientPacket::DuplicateConnect
                | ClientPacket::NotConnecting
                | ClientPacket::ConnectFromElsewhere
                | ClientPacket::ErrorIgnored
                | ClientPacket::Unknown
        )
    }
}

/// The connection states a client goes through while joining (`cls.state`;
/// the two games number them differently).
fn joining_states(game: Game) -> (i32, i32, i32) {
    // (sending getchallenge, sending connect, connected)
    match game {
        Game::CallOfDuty2 => (3, 4, 5),
        Game::UnitedOffensive => (1, 2, 3),
    }
}

/// Whether `command` starts with `prefix`'s `n` characters, case and all
/// (the originals' inlined `strncmp`).
fn starts_exactly(command: &[u8], prefix: &[u8], n: usize) -> bool {
    crate::shared::strings::strncmp(command, prefix, n as i32) == 0
}

/// What a client does with a packet without a connection from `from`
/// (`msg` holds it, the four 0xFF bytes included): reads its first line and
/// splits it into words, then by the first word (compared without case):
/// see [`ClientPacket`]. The packets that change the client:
///
/// * `challengeResponse <challenge>` (United Offensive: then the
///   only-visible-clients flag) while connecting (state 3 / 1): the
///   challenge and `from` are kept, the connect packets start over (count
///   0, time -99999), the state moves on (4 / 2).
/// * `connectResponse` while sending `connect`, from the server's address
///   (same base address): the channel is set up to it with the client's
///   qport ([`Netchan::setup`]), the state is connected (5 / 3), the last
///   packet time is now, the last send time -9999.
/// * `print`, `needcdkey`: [`ClientState::server_message`].
/// * `echo`: `send` gets the answer (to `from`).
///
/// Before its words, a packet of fewer than 4 bytes is marked overflowed
/// (Call of Duty 2); reading starts after the four bytes.
///
/// Original: `CL_ConnectionlessPacket` at 0x0040E750 (CoD2MP_s.exe 1.3; from, msg and a word on the stack)
/// Original: `CL_ConnectionlessPacket` at 0x00412880 (CoDUOMP.exe 1.51; from, msg and a word on the stack)
pub fn connectionless_packet(
    cl: &mut ClientState,
    from: &NetAddress,
    msg: &mut Msg,
    send: &mut dyn FnMut(&NetAddress, &[u8]),
) -> ClientPacket {
    use crate::net::oob::{self, Tokens};
    let game = cl.game;
    let cod2 = game == Game::CallOfDuty2;
    // MSG_BeginReading, then the -1 (read by hand).
    if cod2 {
        msg.overflowed = false;
    }
    msg.readcount = 0;
    msg.bit = 0;
    if msg.cursize < 4 {
        if cod2 {
            msg.overflowed = true;
        }
    } else {
        msg.readcount = 4;
    }
    if oob::is_punkbuster(msg.data) {
        return ClientPacket::PunkBuster;
    }
    let mut line_buffer = [0u8; MAX_STRING_CHARS];
    let line = msg.read_string_line(&mut line_buffer).to_vec();
    let mut tokens = Tokens::default();
    tokens.tokenize(game, &line, 0);
    let command = tokens.argv(0).to_vec();
    let limit = if cod2 {
        crate::shared::strings::STRICMP_LIMIT_COD2
    } else {
        crate::shared::strings::STRICMP_LIMIT_UO
    };
    let is = |name: &[u8]| crate::shared::strings::stricmpn(name, &command, limit) == 0;
    let (connecting, challenging, connected) = joining_states(game);
    if cod2 && is(b"v") {
        return ClientPacket::Voice;
    }
    if is(b"challengeResponse") {
        if cl.connection_state != connecting {
            return ClientPacket::ChallengeUnwanted;
        }
        cl.challenge = oob::atol(tokens.argv(1));
        if !cod2 {
            cl.only_visible_clients = if tokens.argc() < 3 {
                0
            } else {
                oob::atol(tokens.argv(2))
            };
        }
        cl.connection_state = challenging;
        cl.connect_packet_count = 0;
        cl.connect_time = -99999;
        cl.server_address = *from;
        return ClientPacket::ChallengeTaken;
    }
    if is(b"connectResponse") {
        if cl.connection_state >= connected {
            return ClientPacket::DuplicateConnect;
        }
        if cl.connection_state != challenging {
            return ClientPacket::NotConnecting;
        }
        if !oob::same_base_address(game, from, &cl.server_address) {
            return ClientPacket::ConnectFromElsewhere;
        }
        // (The originals also note an update server's answer here, when
        // they asked one: not done.)
        cl.chan.setup(NetSource::Client, *from, cl.qport);
        cl.connection_state = connected;
        cl.last_packet_time = cl.realtime;
        cl.last_packet_sent_time = -9999;
        return ClientPacket::Connected;
    }
    if is(b"infoResponse") {
        return ClientPacket::Info;
    }
    if is(b"statusResponse") {
        return ClientPacket::Status;
    }
    if is(b"disconnect") {
        return ClientPacket::Disconnect;
    }
    if is(b"echo") {
        let mut packet = vec![0u8; MAX_STRING_CHARS + 8];
        if let Some(n) = oob::out_of_band_print(game, tokens.argv(1), &mut packet) {
            send(from, &packet[..n]);
        }
        return ClientPacket::Echo;
    }
    if is(b"keyAuthorize") {
        return ClientPacket::KeyAuthorize;
    }
    if !cod2 && is(b"motd") {
        return ClientPacket::Motd;
    }
    if is(b"print") {
        let mut text = [0u8; BIG_INFO_STRING];
        let s = msg.read_big_string(&mut text);
        crate::crt::strncpy(&mut cl.server_message[..SERVER_MESSAGE_SIZE - 1], s);
        cl.server_message[SERVER_MESSAGE_SIZE - 1] = 0;
        return ClientPacket::Print;
    }
    if is(b"error") {
        if cl.connection_state != 0 && oob::same_base_address(game, from, &cl.server_address) {
            let mut text = [0u8; BIG_INFO_STRING];
            return ClientPacket::ServerError(msg.read_big_string(&mut text).to_vec());
        }
        return ClientPacket::ErrorIgnored;
    }
    if is(b"updateResponse") {
        return ClientPacket::Update;
    }
    if starts_exactly(&command, b"getserversResponse", 18) {
        return ClientPacket::Servers;
    }
    if starts_exactly(&command, b"needcdkey", 9) {
        crate::crt::strncpy(
            &mut cl.server_message[..SERVER_MESSAGE_SIZE - 1],
            b"EXE_AWAITINGCDKEYAUTH",
        );
        cl.server_message[SERVER_MESSAGE_SIZE - 1] = 0;
        return ClientPacket::NeedCdKey;
    }
    if is(b"loadingnewmap") {
        if !oob::same_base_address(game, from, &cl.server_address) {
            return ClientPacket::NewMapIgnored;
        }
        let mut first = [0u8; MAX_STRING_CHARS];
        let map = msg.read_string_line(&mut first);
        // (Call of Duty 2 keeps 63 characters of it.)
        let map = map[..if cod2 { map.len().min(63) } else { map.len() }].to_vec();
        let mut second = [0u8; MAX_STRING_CHARS];
        msg.read_string_line(&mut second);
        return ClientPacket::NewMap(map);
    }
    ClientPacket::Unknown
}

/// What [`check_for_resend`] did.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Resend {
    /// Nothing to send (a demo plays, the client isn't joining, or the last
    /// packet went out less than [`resend_time`] ago).
    Nothing,
    /// `getchallenge` went to the server. `authorize`: the original first
    /// asks the authorize server about the player's key
    /// (`CL_RequestAuthorization`, the caller's): the server isn't on the
    /// local network, or `net_lanauthorize` is set.
    Challenge { authorize: bool },
    /// `connect` went to the server, with the player's settings. (The
    /// original then marks its settings as sent, clearing the userinfo bit
    /// of its changed-settings flags, `cvar_modifiedFlags &= ~2`: the
    /// caller's.)
    Connect,
}

/// How long a client waits before sending `getchallenge` or `connect`
/// again, in milliseconds: Call of Duty 2 2000, United Offensive 3000.
pub fn resend_time(game: Game) -> i32 {
    match game {
        Game::CallOfDuty2 => 2000,
        Game::UnitedOffensive => 3000,
    }
}

/// What [`check_for_resend`] needs besides the client's state.
#[derive(Debug, Clone, Copy)]
pub struct ResendSettings<'a> {
    /// The player's settings as an info string (the games' userinfo
    /// dvars/cvars: name, rate, ...).
    pub userinfo: &'a [u8],
    /// Call of Duty 2's `getchallenge` carries a hash of the player's key
    /// (`CL_BuildMd5StrFromCDKey`, not ported: a LAN server only keeps it).
    pub key_hash: &'a [u8],
    /// `net_lanauthorize`.
    pub lan_authorize: bool,
    /// The machine's own IPv4 addresses ([`crate::net::oob::is_lan_address`]).
    pub local_addresses: &'a [[u8; 4]],
}

/// The buffer the originals build the `connect` text in.
const CONNECT_TEXT_BUFFER: usize = 1024;

/// While joining, sends the next `getchallenge` or `connect` to the server
/// ([`ClientState::server_address`]) every [`resend_time`]: counted in
/// `connect_packet_count`, timed in `connect_time`.
///
/// * Sending `getchallenge` (state 3 / 1): Call of Duty 2's text is
///   `getchallenge 0 "<key hash>"`, United Offensive's `getchallenge`.
/// * Sending `connect` (state 4 / 2): `connect "<userinfo>"`, the player's
///   settings (1023 characters) with `protocol`, `challenge` and `qport`
///   set (United Offensive: `protocol`, `qport`, `challenge`), as data (United
///   Offensive's is then compressed from byte 12, [`crate::net::oob::out_of_band_data`]).
///
/// The text can be longer than the 1024-byte buffer the originals build it
/// in (settings of more than 1014 characters). United Offensive's then
/// starts over: from byte 1024 on it repeats its start (`connect "...`),
/// which the port does too. Call of Duty 2's stays whole, but with settings
/// of 1023 characters or more its copy runs past the end of its stack frame
/// (into the saved frame pointer); the port sends it whole.
///
/// The qport is [`ClientState::qport`]: Call of Duty 2 reads a copy it
/// took from `net_qport` when the connection started, United Offensive the
/// `net_qport` cvar's value. When PunkBuster's client library is loaded,
/// the originals also hand it each packet's text first (kind 1
/// `getchallenge`, kind 2 the `connect` text); PunkBuster isn't part of the
/// port, so nothing does here.
///
/// Original: `CL_CheckForResend` at 0x0040E100 (CoD2MP_s.exe 1.3)
/// Original: `CL_CheckForResend` at 0x00411E60 (CoDUOMP.exe 1.51)
pub fn check_for_resend(
    cl: &mut ClientState,
    settings: &ResendSettings,
    send: &mut dyn FnMut(&NetAddress, &[u8]),
) -> Resend {
    use crate::net::oob;
    let game = cl.game;
    let cod2 = game == Game::CallOfDuty2;
    let (connecting, challenging, _) = joining_states(game);
    if cl.demo_playing != 0
        || (cl.connection_state != connecting && cl.connection_state != challenging)
        || cl.realtime.wrapping_sub(cl.connect_time) < resend_time(game)
    {
        return Resend::Nothing;
    }
    cl.connect_packet_count = cl.connect_packet_count.wrapping_add(1);
    cl.connect_time = cl.realtime;
    let server = cl.server_address;
    if cl.connection_state == connecting {
        let authorize =
            settings.lan_authorize || !oob::is_lan_address(&server, settings.local_addresses);
        let text = if cod2 {
            let mut t = b"getchallenge 0 \"".to_vec();
            t.extend_from_slice(&settings.key_hash[..crate::cstr::len(settings.key_hash)]);
            t.push(b'"');
            t
        } else {
            b"getchallenge".to_vec()
        };
        let mut packet = vec![0u8; text.len() + 8];
        if let Some(n) = oob::out_of_band_print(game, &text, &mut packet) {
            send(&server, &packet[..n]);
        }
        return Resend::Challenge { authorize };
    }
    let mut info = [0u8; crate::net::msg::MAX_STRING_CHARS];
    crate::crt::strncpy(&mut info[..MAX_STRING_CHARS - 1], settings.userinfo);
    info[MAX_STRING_CHARS - 1] = 0;
    let protocol = oob::protocol_version(game).to_string();
    let challenge = cl.challenge.to_string();
    let qport = cl.qport.to_string();
    let keys: [(&[u8], &[u8]); 3] = if cod2 {
        [
            (b"protocol", protocol.as_bytes()),
            (b"challenge", challenge.as_bytes()),
            (b"qport", qport.as_bytes()),
        ]
    } else {
        [
            (b"protocol", protocol.as_bytes()),
            (b"qport", qport.as_bytes()),
            (b"challenge", challenge.as_bytes()),
        ]
    };
    for (key, value) in keys {
        // (A setting too long for the string is refused with a warning, as the original prints.)
        let _ = crate::shared::info::set_value_for_key(&mut info, key, value);
    }
    let mut data = b"connect \"".to_vec();
    data.extend_from_slice(&info[..crate::cstr::len(&info)]);
    data.push(b'"');
    if !cod2 {
        // United Offensive builds the text in a 1024-byte buffer right in
        // front of the one it copies it to (a forward copy): past 1024
        // bytes, the text starts over.
        for k in CONNECT_TEXT_BUFFER..data.len() {
            data[k] = data[k - CONNECT_TEXT_BUFFER];
        }
    }
    let mut packet = vec![0u8; 2 * data.len() + 64];
    if let Some(n) = oob::out_of_band_data(game, &data, &mut packet) {
        let mut length = n;
        if !cod2 {
            let mut msg = Msg::new(game, &mut packet);
            msg.cursize = n;
            crate::net::huffman::compress_adaptive(&mut msg, 12);
            length = msg.cursize;
        }
        send(&server, &packet[..length]);
    }
    Resend::Connect
}

// --- the system info ------------------------------------------------------------------

/// The config string holding the server's system info (`CS_SYSTEMINFO`).
pub const SYSTEM_INFO: usize = 1;

/// What the server's system info asks of the rest of the client. The
/// originals do it themselves, through their console variables and file
/// system; the port hands it to the caller.
#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct SystemInfo {
    /// `sv_cheats` reads as 0 (`atol`): the originals put every
    /// cheat-protected setting back to its default (`Dvar_SetCheatState`;
    /// United Offensive's `Cvar_SetCheatState`).
    pub cheats_off: bool,
    /// The archives the server has loaded, for a pure server's file
    /// checks, as numbers and names: Call of Duty 2's `sv_iwds` and
    /// `sv_iwdNames` (`FS_PureServerSetLoadedIwds`), United Offensive's
    /// `sv_paks` and `sv_pakNames` (`FS_PureServerSetLoadedPaks`).
    pub loaded_archives: (Vec<u8>, Vec<u8>),
    /// The archives the server's game uses (a client without them
    /// downloads them): `sv_referencedIwds` and `sv_referencedIwdNames`
    /// (United Offensive: `sv_referencedPaks`, `sv_referencedPakNames`;
    /// `FS_ServerSetReferencedIwds` / `Paks`).
    pub referenced_archives: (Vec<u8>, Vec<u8>),
    /// Every `\key\value` pair of the system info, in order (a last key
    /// without a value has an empty one), which the client takes as its own
    /// settings: Call of Duty 2 sets an existing dvar or creates a string
    /// dvar with the system-info flag (`Dvar_FindVar`, then
    /// `Dvar_SetFromString` or `Dvar_RegisterString(key, value, 0x4000)`),
    /// United Offensive `Cvar_Set2(key, value, force)`. None of them when
    /// the client runs the server itself (`sv_running`).
    pub settings: Vec<(Vec<u8>, Vec<u8>)>,
}

/// The server's system info ([`SYSTEM_INFO`]) changed: its id (`atol` of
/// `sv_serverid`) becomes [`ClientState::server_id`]. While a demo plays,
/// that is all (`None`); otherwise the rest is for the caller
/// ([`SystemInfo`]).
///
/// The originals end by setting `cl_connectedToPureServer` from the
/// client's own `sv_pure` (after taking the settings): the caller's. United
/// Offensive also clears a 256-byte table its snapshot parsers keep for
/// `sv_onlyVisibleClients` (at 0x04957F60), which the port doesn't model
/// (see [`parse_packet_entities`]).
///
/// Errors: a system info of 8192 characters or more (`Info_ValueForKey`'s
/// "oversize infostring").
///
/// Original: `CL_SystemInfoChanged` at 0x004139E0 (CoD2MP_s.exe 1.3)
/// Original: `CL_SystemInfoChanged` at 0x00418B80 (CoDUOMP.exe 1.51)
pub fn system_info_changed(
    cl: &mut ClientState,
    server_running: bool,
) -> Result<Option<SystemInfo>, GameError> {
    use crate::net::oob::atol;
    use crate::shared::info::value_for_key;
    let info = cl.game_state.get(SYSTEM_INFO).to_vec();
    cl.server_id = atol(value_for_key(&info, b"sv_serverid")?);
    if cl.demo_playing != 0 {
        return Ok(None);
    }
    let cod2 = cl.game == Game::CallOfDuty2;
    let value = |key: &[u8]| value_for_key(&info, key).map(<[u8]>::to_vec);
    let (loaded, loaded_names, referenced, referenced_names): (&[u8], &[u8], &[u8], &[u8]) = if cod2
    {
        (
            b"sv_iwds",
            b"sv_iwdNames",
            b"sv_referencedIwds",
            b"sv_referencedIwdNames",
        )
    } else {
        (
            b"sv_paks",
            b"sv_pakNames",
            b"sv_referencedPaks",
            b"sv_referencedPakNames",
        )
    };
    let mut out = SystemInfo {
        cheats_off: atol(&value(b"sv_cheats")?) == 0,
        loaded_archives: (value(loaded)?, value(loaded_names)?),
        referenced_archives: (value(referenced)?, value(referenced_names)?),
        settings: Vec::new(),
    };
    if !server_running {
        // Info_NextPair, inlined: one leading `\` skipped, the key up to the
        // next `\` (or the end: an empty value), the value up to the next.
        let at = |i: usize| crate::cstr::at(&info, i);
        let mut i = 0;
        loop {
            if at(i) == b'\\' {
                i += 1;
            }
            let start = i;
            while !matches!(at(i), b'\\' | 0) {
                i += 1;
            }
            let key = info[start..i].to_vec();
            let mut value = Vec::new();
            if at(i) == b'\\' {
                i += 1;
                let start = i;
                while !matches!(at(i), b'\\' | 0) {
                    i += 1;
                }
                value = info[start..i].to_vec();
            }
            if key.is_empty() {
                break;
            }
            out.settings.push((key, value));
        }
    }
    Ok(Some(out))
}

// --- the client game's clock ----------------------------------------------------------

/// The connection states after joining: loading the map, primed (loaded,
/// waiting for the first snapshot), active (Call of Duty 2: 6, 7, 8;
/// United Offensive: 4, 5, 6).
pub fn playing_states(game: Game) -> (i32, i32, i32) {
    match game {
        Game::CallOfDuty2 => (6, 7, 8),
        Game::UnitedOffensive => (4, 5, 6),
    }
}

/// A snapshot's flag: the server doesn't have the player in the game yet
/// (`SNAPFLAG_NOT_ACTIVE`).
pub const SNAPFLAG_NOT_ACTIVE: i32 = 2;

/// The client's first snapshot in the game (not flagged
/// [`SNAPFLAG_NOT_ACTIVE`]) makes it active: the clock starts from the
/// snapshot's time (`server_time_delta`, `old_server_time`,
/// `time_demo_base_time`). Returns whether it did; the originals then run
/// the player's `activeAction` setting once and clear it (the caller's).
///
/// Original: `CL_FirstSnapshot` at 0x00402EC0 (CoD2MP_s.exe 1.3)
/// Original: `CL_FirstSnapshot` at 0x004056A0 (CoDUOMP.exe 1.51)
pub fn first_snapshot(cl: &mut ClientState) -> bool {
    if cl.snap.snap_flags & SNAPFLAG_NOT_ACTIVE != 0 {
        return false;
    }
    cl.connection_state = playing_states(cl.game).2;
    cl.server_time_delta = cl.snap.server_time.wrapping_sub(clock_now(cl));
    cl.old_server_time = cl.snap.server_time;
    cl.time_demo_base_time = cl.snap.server_time;
    true
}

/// The client time the client game's clock runs on: Call of Duty 2's
/// [`ClientState::realtime`], United Offensive's [`ClientState::sim_time`].
fn clock_now(cl: &ClientState) -> i32 {
    match cl.game {
        Game::CallOfDuty2 => cl.realtime,
        Game::UnitedOffensive => cl.sim_time,
    }
}

/// The settings the client's clock reads.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct ClockSettings {
    /// `cl_paused`, `sv_paused` and `sv_running` (Call of Duty 2's
    /// `com_sv_running`): with all three set (a paused local game) the
    /// clock stands still.
    pub client_paused: bool,
    pub server_paused: bool,
    pub server_running: bool,
    /// `cl_freezeDemo`: a demo's time stands still.
    pub freeze_demo: bool,
    /// United Offensive's `cl_timeNudge`, milliseconds of latency added for
    /// smoothness (-30 to 30 used); Call of Duty 2 has none.
    pub time_nudge: i32,
    /// `timescale`: the slow drift of [`adjust_time_delta`] only runs at 1
    /// (United Offensive: at 0 or 1).
    pub timescale: f32,
    /// United Offensive's `timedemo` cvar (Call of Duty 2 keeps it in
    /// [`ClientState::time_demo`]).
    pub time_demo: bool,
}

impl Default for ClockSettings {
    fn default() -> ClockSettings {
        ClockSettings {
            client_paused: false,
            server_paused: false,
            server_running: false,
            freeze_demo: false,
            time_nudge: 0,
            timescale: 1.0,
            time_demo: false,
        }
    }
}

/// Keeps [`ClientState::server_time_delta`] in step after a new snapshot
/// (and clears `new_snapshots`); nothing while a demo plays. The new delta
/// is the snapshot's time minus the client's; off by more than 500 ms, the
/// clock jumps to it (`server_time` and `old_server_time` too); by more
/// than 100, it moves halfway; otherwise it drifts a millisecond at a time
/// (while `timescale` is 1): back 2 after the clock got near the latest
/// snapshot ([`ClientState::extrapolated_snapshot`]), else forward.
///
/// The originals differ: Call of Duty 2 takes the time between the last two
/// snapshots ([`ClientState::old_frame_server_time`], up to 500 ms) and 5
/// ms off the new delta and out of how far off it is, and drifts toward
/// the new delta (a millisecond either way, none when equal); United
/// Offensive takes that time out of how far off a late clock is (down to
/// 0; with no time between them, the clock is first set to the snapshot's)
/// and drifts forward always, as Quake III does.
///
/// Original: `CL_AdjustTimeDelta` at 0x00402D70 (CoD2MP_s.exe 1.3)
/// Original: `CL_AdjustTimeDelta` at 0x00405560 (CoDUOMP.exe 1.51)
pub fn adjust_time_delta(cl: &mut ClientState, settings: &ClockSettings) {
    cl.new_snapshots = 0;
    if cl.demo_playing != 0 {
        return;
    }
    let snap_time = cl.snap.server_time;
    let between = snap_time.wrapping_sub(cl.old_frame_server_time);
    let cod2 = cl.game == Game::CallOfDuty2;
    let now = clock_now(cl);
    let new_delta = if cod2 {
        snap_time
            .wrapping_sub(now)
            .wrapping_sub(between)
            .wrapping_sub(5)
    } else {
        snap_time.wrapping_sub(now)
    };
    let mut off = new_delta.wrapping_sub(cl.server_time_delta);
    let mut drift = false;
    if off <= 0 {
        off = off.wrapping_neg();
    } else if between as u32 <= 500 {
        off = off.wrapping_sub(between);
        if cod2 {
            drift = off < 0;
        } else {
            off = off.max(0);
            if between == 0 {
                cl.server_time_delta = new_delta;
                cl.old_server_time = snap_time;
                cl.server_time = snap_time;
            }
        }
    }
    if !drift && off > 500 {
        cl.server_time_delta = new_delta;
        cl.old_server_time = snap_time;
        cl.server_time = snap_time;
    } else if !drift && off > 100 {
        cl.server_time_delta = cl.server_time_delta.wrapping_add(new_delta) >> 1;
    } else {
        let at_normal_speed = if cod2 {
            settings.timescale == 1.0
        } else {
            settings.timescale == 0.0 || settings.timescale.to_bits() == 1.0f32.to_bits()
        };
        if !at_normal_speed {
            return;
        }
        if cl.extrapolated_snapshot != 0 {
            cl.extrapolated_snapshot = 0;
            cl.server_time_delta = cl.server_time_delta.wrapping_sub(2);
        } else if !cod2 {
            cl.server_time_delta = cl.server_time_delta.wrapping_add(1);
        } else if new_delta > cl.server_time_delta {
            cl.server_time_delta += 1;
        } else if new_delta < cl.server_time_delta {
            cl.server_time_delta -= 1;
        }
    }
}

/// What the client's clock leaves to the caller: the `activeAction`
/// setting, demo playback.
pub trait ClockParts {
    /// The first snapshot made the client active ([`first_snapshot`]): the
    /// originals run the player's `activeAction` setting once and clear it.
    fn became_active(&mut self, cl: &mut ClientState);
    /// The next message of the demo playing (`CL_ReadDemoMessage`).
    /// Returns whether to go on.
    fn read_demo_message(&mut self, cl: &mut ClientState) -> bool;
    /// `timedemo`'s frame timing (Call of Duty 2 0x00402F90, United
    /// Offensive 0x00405770). Returns whether to go on.
    fn time_demo_frame(&mut self, cl: &mut ClientState) -> bool;
}

/// The client game's clock, once a frame: while primed, the first new
/// snapshot makes the client active ([`first_snapshot`]); while active, the
/// server time the client runs at ([`ClientState::server_time`]) is the
/// client's time (United Offensive's second clock,
/// [`ClientState::sim_time`]) plus the delta (United Offensive: less `cl_timeNudge`,
/// -30 to 30), never earlier than before; near the latest snapshot's time
/// (5 ms) the snapshot is extrapolated; a new snapshot adjusts the delta
/// ([`adjust_time_delta`]). A demo reads messages until it is ahead again.
/// Nothing in the other states.
///
/// Errors: active without a valid snapshot; a snapshot earlier than the
/// last one, except from a server named `localhost` (the clock then starts
/// again from it).
///
/// Original: `CL_SetCGameTime` at 0x00403080 (CoD2MP_s.exe 1.3)
/// Original: `CL_SetCGameTime` at 0x00405840 (CoDUOMP.exe 1.51)
pub fn set_cgame_time(
    cl: &mut ClientState,
    settings: &ClockSettings,
    parts: &mut dyn ClockParts,
) -> Result<(), GameError> {
    let game = cl.game;
    let cod2 = game == Game::CallOfDuty2;
    let (_, primed, active) = playing_states(game);
    let first = |cl: &mut ClientState, parts: &mut dyn ClockParts| {
        if first_snapshot(cl) {
            parts.became_active(cl);
        }
    };
    if cl.connection_state != active {
        if cl.connection_state != primed {
            return Ok(());
        }
        if cl.demo_playing != 0 {
            if cl.first_demo_frame_skipped == 0 {
                cl.first_demo_frame_skipped = 1;
                return Ok(());
            }
            if !parts.read_demo_message(cl) {
                return Ok(());
            }
        }
        if cl.new_snapshots != 0 {
            cl.new_snapshots = 0;
            first(cl, parts);
        }
        if cl.connection_state != active {
            return Ok(());
        }
    }
    if cl.snap.valid == 0 {
        return Err(GameError::drop("CL_SetCGameTime: !cl.snap.valid"));
    }
    if settings.client_paused && settings.server_paused && settings.server_running {
        return Ok(());
    }
    if cl.snap.server_time < cl.frame_server_time {
        let limit = if cod2 {
            i32::MAX
        } else {
            crate::shared::strings::STRICMP_LIMIT_UO
        };
        if crate::shared::strings::stricmpn(&cl.server_name, b"localhost", limit) == 0 {
            first(cl, parts);
        } else {
            return Err(GameError::drop(
                "cl.snap.serverTime < cl.oldFrameServerTime",
            ));
        }
    }
    let snap_time = cl.snap.server_time;
    cl.frame_server_time = snap_time;
    if !(cl.demo_playing != 0 && settings.freeze_demo) {
        let nudge = if cod2 {
            0
        } else {
            settings.time_nudge.clamp(-30, 30)
        };
        let now = clock_now(cl).wrapping_add(cl.server_time_delta);
        cl.server_time = now.wrapping_sub(nudge).max(cl.old_server_time);
        cl.old_server_time = cl.server_time;
        if now >= snap_time.wrapping_sub(5) {
            cl.extrapolated_snapshot = 1;
        }
    }
    if cl.new_snapshots != 0 {
        adjust_time_delta(cl, settings);
    }
    if cl.demo_playing == 0 {
        return Ok(());
    }
    let time_demo = if cod2 {
        cl.time_demo != 0
    } else {
        settings.time_demo
    };
    if time_demo && !parts.time_demo_frame(cl) {
        return Ok(());
    }
    while cl.server_time >= cl.snap.server_time {
        if !parts.read_demo_message(cl) || cl.connection_state != active {
            return Ok(());
        }
    }
    Ok(())
}

// --- the player's commands ------------------------------------------------------------

/// The last of a player command's fields, after the input's: the weapon
/// (Call of Duty 2: and the off-hand item) the client game picked, the
/// server time ([`ClientState::server_time`], at most 5 s past the latest
/// snapshot's), and the angles (view plus the client game's, in 16-bit
/// steps, as the game's x87 computes them).
///
/// Original: `CL_FinishMove` at 0x00408A90 (CoD2MP_s.exe 1.3; cmd in eax)
/// Original: `CL_FinishMove` at 0x0040C8A0 (CoDUOMP.exe 1.51; cmd in eax)
pub fn finish_move(cl: &ClientState, cmd: &mut [u8]) {
    use crate::net::msg::{ftol2, ANGLE16_SCALE};
    let cod2 = cl.game == Game::CallOfDuty2;
    if cod2 {
        cmd[8] = cl.user_cmd_weapon as u8;
        cmd[9] = cl.user_cmd_offhand as u8;
    } else {
        cmd[6] = cl.user_cmd_weapon as u8;
    }
    let snap_time = cl.snap.server_time;
    let mut time = cl.server_time;
    if time.wrapping_sub(snap_time) > 5000 {
        time = snap_time.wrapping_add(5000);
    }
    delta::set_word(cmd, 0, time);
    let angles = if cod2 { 0xC } else { 8 };
    for i in 0..3 {
        let (a, b) = (cl.cgame_angles[i], cl.view_angles[i]);
        let product = if cod2 {
            f64::from((a + b) * ANGLE16_SCALE)
        } else {
            (f64::from(a) + f64::from(b)) * f64::from(ANGLE16_SCALE)
        };
        delta::set_word(
            cmd,
            (angles + 4 * i) as u32,
            (ftol2(product) as i32) & 0xFFFF,
        );
    }
}

/// United Offensive's input timing (`cl_input.c`'s statics): the
/// milliseconds of the frame (`frame_msec`, at most 200) and the frame time
/// it was taken from.
#[derive(Debug, Clone, Copy, Default, PartialEq, Eq)]
pub struct InputTiming {
    pub frame_msec: i32,
    pub old_frame_time: i32,
}

/// Once loaded (primed or active), each frame adds a player command:
/// [`ClientState::cmd_number`] counts up and the command (`create`, the
/// caller's `CL_CreateCmd`: the input, then [`finish_move`]) goes in the
/// ring. United Offensive first times the frame from `frame_time`
/// (`com_frameTime`) into `timing`, which its input reads.
///
/// Original: `CL_CreateNewCommands` at 0x00408BB0 (CoD2MP_s.exe 1.3)
/// Original: `CL_CreateNewCommands` at 0x0040CAD0 (CoDUOMP.exe 1.51)
pub fn create_new_commands(
    cl: &mut ClientState,
    frame_time: i32,
    timing: &mut InputTiming,
    create: &mut dyn FnMut(&mut ClientState, &InputTiming) -> Vec<u8>,
) {
    let game = cl.game;
    if cl.connection_state < playing_states(game).1 {
        return;
    }
    if game == Game::UnitedOffensive {
        let msec = frame_time.wrapping_sub(timing.old_frame_time);
        timing.frame_msec = if msec as u32 > 200 { 200 } else { msec };
        timing.old_frame_time = frame_time;
    }
    cl.cmd_number = cl.cmd_number.wrapping_add(1);
    let cmd = create(cl, timing);
    let size = delta::usercmd_size(game);
    let at = (cl.cmd_number as u32 as usize % CMD_BACKUP) * size;
    cl.cmds[at..at + size].copy_from_slice(&cmd[..size]);
}

/// Whether to send the client's next message now. Never while a demo plays
/// (or in Call of Duty 2's states 1 and 2, United Offensive's 7 and 8);
/// while downloading, at most every 50 ms; before loading, once a second;
/// to a loopback or LAN server every frame; otherwise at most
/// `cl_maxpackets` a second, timed from the last packet's send time.
/// United Offensive first keeps `max_packets` within 15 to 100 (setting
/// the cvar: the caller's, through `max_packets`).
///
/// Original: `CL_ReadyToSendPacket` at 0x00408C10 (CoD2MP_s.exe 1.3)
/// Original: `CL_ReadyToSendPacket` at 0x0040CB60 (CoDUOMP.exe 1.51)
pub fn ready_to_send_packet(
    cl: &ClientState,
    downloading: bool,
    local_addresses: &[[u8; 4]],
    max_packets: &mut i32,
) -> bool {
    use crate::net::oob;
    let game = cl.game;
    if cl.demo_playing != 0 || sends_nothing_in(game, cl.connection_state) {
        return false;
    }
    let since = cl.realtime.wrapping_sub(cl.last_packet_sent_time);
    if downloading && since < 50 {
        return false;
    }
    let (_, primed, active) = playing_states(game);
    if cl.connection_state != active
        && cl.connection_state != primed
        && !downloading
        && since < 1000
    {
        return false;
    }
    let to = &cl.chan.remote_address;
    if to.kind == netchan::AddressType::Loopback || oob::is_lan_address(to, local_addresses) {
        return true;
    }
    if game == Game::UnitedOffensive {
        *max_packets = (*max_packets).clamp(15, 100);
    }
    let old = cl.out_packets
        [(cl.chan.outgoing_sequence.wrapping_sub(1) as u32 as usize) & (PACKET_BACKUP - 1)];
    // (Call of Duty 2's dvar keeps cl_maxpackets above 0; the original divides by it.)
    let interval = 1000i32.wrapping_div((*max_packets).max(1));
    cl.realtime.wrapping_sub(old.realtime) >= interval
}

/// Queues a command for the server (sent with every packet until the
/// server acknowledges it; [`write_packet`]): the next slot of the ring,
/// copied by [`crate::net::msg::write_reliable_command_to_buffer`], whose
/// warning (a command cut to 1023 characters, or an empty one) is the inner
/// `Err`. More than the ring's size unacknowledged is a dropping error
/// before anything changes; exactly the ring's size is not, and overwrites
/// the oldest unacknowledged command (the original's comparison).
///
/// Original: `CL_AddReliableCommand` at 0x0040C000 (CoD2MP_s.exe 1.3)
/// Original: `CL_AddReliableCommand` at 0x0040F930 (CoDUOMP.exe 1.51)
pub fn add_reliable_command(
    cl: &mut ClientState,
    command: &[u8],
) -> Result<Result<(), crate::net::msg::ReliableCommandWarning>, GameError> {
    let ring = max_reliable_commands(cl.game);
    if cl.reliable_sequence.wrapping_sub(cl.reliable_acknowledge) > ring as i32 {
        return Err(GameError::drop("EXE_ERR_CLIENT_CMD_OVERFLOW"));
    }
    cl.reliable_sequence = cl.reliable_sequence.wrapping_add(1);
    let slot = (cl.reliable_sequence as u32 as usize % ring) * SERVER_COMMAND_SIZE;
    let out = &mut cl.reliable_commands[slot..slot + SERVER_COMMAND_SIZE];
    Ok(crate::net::msg::write_reliable_command_to_buffer(
        cl.game, command, out,
    ))
}

/// Tells a pure server which archives the client uses: `cp ` and the list
/// [`crate::fs::referenced_pure_checksums`] makes (cut to 1020 characters,
/// the command being at most 1023), queued with [`add_reliable_command`].
/// The original spells the command `Va ` and adds 13 and 15 to its first
/// two characters, so that the text `cp ` isn't in the executable. Its
/// errors are [`add_reliable_command`]'s; the command is never too long or
/// empty, so there is no warning.
///
/// Original: `CL_SendPureChecksums` at 0x0040D680 (CoD2MP_s.exe 1.3)
/// Original: `CL_SendPureChecksums` at 0x00411450 (CoDUOMP.exe 1.51)
pub fn send_pure_checksums(
    cl: &mut ClientState,
    search_paths: &[crate::fs::SearchPath],
    feed: i32,
    fake: i32,
) -> Result<(), GameError> {
    let checksums = crate::fs::referenced_pure_checksums(cl.game, search_paths, feed, fake);
    let mut command = [0u8; SERVER_COMMAND_SIZE];
    crate::crt::sprintf(&mut command, SERVER_COMMAND_SIZE, b"Va ");
    // Never fails: "Va " is shorter than the buffer.
    let appended = crate::shared::strings::strcat(&mut command, &checksums);
    debug_assert!(appended.is_ok());
    command[0] = command[0].wrapping_add(13);
    command[1] = command[1].wrapping_add(15);
    add_reliable_command(cl, &command).map(|_| ())
}

/// A config string's new text, from a server command's words (`d <index>
/// <text>`; the caller's tokenizer kept the text as one word): the game
/// state is built again with `text` in place of config string `index`'s,
/// every string packed from offset 1 in index order (empty ones point at
/// offset 0, the empty string). Returns `index` when the text changed, and
/// `None`, with nothing changed, when it was the same; when `index` is
/// [`SYSTEM_INFO`], the original then runs [`system_info_changed`]: the
/// caller's.
///
/// Errors: an index outside 0 to 2047 (`atol` of the word); the strings
/// not fitting the game state's 131072 / 20480 bytes, once the strings
/// before the one that didn't fit are in place (as the original leaves
/// them).
///
/// Original: `CL_ConfigstringModified` at 0x00401550 (CoD2MP_s.exe 1.3)
/// Original: `CL_ConfigstringModified` at 0x00401480 (CoDUOMP.exe 1.51)
pub fn configstring_modified(
    cl: &mut ClientState,
    index_word: &[u8],
    text: &[u8],
) -> Result<Option<usize>, GameError> {
    let index = crate::net::oob::atol(index_word);
    if !(0..MAX_CONFIG_STRINGS as i32).contains(&index) {
        return Err(GameError::drop("configstring > MAX_CONFIGSTRINGS"));
    }
    let index = index as usize;
    let text = &text[..crate::cstr::len(text)];
    if cl.game_state.get(index) == text {
        return Ok(None);
    }
    let old = cl.game_state.clone();
    let state = &mut cl.game_state;
    state.offsets.fill(0);
    state.data.fill(0);
    state.data_count = 1;
    let limit = max_game_state_chars(cl.game);
    for i in 0..MAX_CONFIG_STRINGS {
        let string = if i == index { text } else { old.get(i) };
        if string.is_empty() {
            continue;
        }
        let start = state.data_count as usize;
        if start + string.len() + 1 > limit {
            return Err(GameError::drop("MAX_GAMESTATE_CHARS exceeded"));
        }
        state.offsets[i] = state.data_count;
        state.data[start..start + string.len()].copy_from_slice(string);
        state.data[start + string.len()] = 0;
        state.data_count += string.len() as i32 + 1;
    }
    Ok(Some(index))
}

/// The buffer a big config string is put together in, from the server
/// commands `x`, `y` and `z` (Quake III's `bcs0` to `bcs2`): the originals'
/// static 8192 bytes.
pub const BIG_CONFIG_STRING_SIZE: usize = 0x2000;

/// What a server command asks of the client ([`get_server_command`]).
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum ServerCommand {
    /// The client game's (the original returns true): its words are in the
    /// tokens.
    ClientGame,
    /// `d`, or a big config string `z` completed: a config string's new
    /// text, applied ([`configstring_modified`]'s result: its index if it
    /// changed). Also the client game's (true); the words are the command's.
    ConfigString(Option<usize>),
    /// `B` or `n`: the map restarted. The player's commands are cleared;
    /// the caller clears the console's notify lines (Call of Duty 2:
    /// 0x00403C70 and a third list inline; United Offensive: 0x00409340 and
    /// two arrays) and makes the call the originals make through the
    /// function pointer at 0x0068A28C / 0x04958110. The client game's (true).
    MapRestart,
    /// Nothing for the client game (false): `x` or `y`, a part of a big
    /// config string (kept), or, while a demo plays, a command the ring no
    /// longer holds.
    Kept,
    /// `w`: the server ended the connection; the original's
    /// `Com_Error(ERR_SERVERDISCONNECT, ...)`. `reason`: the second word, if
    /// there is one, which the originals show in the localized text
    /// `EXE_SERVERDISCONNECTREASON` (without one, `EXE_SERVER_DISCONNECTED`).
    /// Call of Duty 2 localizes the reason too, unless the third word is
    /// `PB` (any case: PunkBuster's own text; `localized_reason` false).
    Disconnected {
        reason: Option<Vec<u8>>,
        localized_reason: bool,
    },
    /// A big config string that is itself a `z` command adding nothing (no
    /// third word): the original runs it again and again and never returns
    /// (such a command from a server hangs the original client). The port
    /// stops instead.
    Endless,
}

/// The words of server command `number` for the client game, and what the
/// command does to the client first. The server's commands are letters
/// (Quake III's words): `d` a config string, `x`/`y`/`z` a big one in parts
/// (`x <index> <text>` starts it as the text `d <index> <text>`, `y` and `z`
/// add their third word, and `z` then runs the result as a command), `B`
/// and `n` a map restart, `w` the end of the connection; the client game
/// handles the rest. `tokens` holds the words afterwards (the originals'
/// `Cmd_Argv`), split as the original last split them: whole for most, `d`
/// with the text as one third word.
///
/// Errors: a command the ring no longer holds (unless a demo plays; the
/// originals first print the whole ring); a command not received yet; a big
/// config string longer than 8191 characters; [`configstring_modified`]'s.
/// The originals print the command first when `cl_showServerCommands` is
/// set: the caller's.
///
/// Original: `CL_GetServerCommand` at 0x00401710 (CoD2MP_s.exe 1.3)
/// Original: `CL_GetServerCommand` at 0x00401630 (CoDUOMP.exe 1.51)
pub fn get_server_command(
    cl: &mut ClientState,
    number: i32,
    tokens: &mut crate::net::oob::Tokens,
    big: &mut [u8; BIG_CONFIG_STRING_SIZE],
) -> Result<ServerCommand, GameError> {
    let game = cl.game;
    let ring = max_reliable_commands(game) as i32;
    if number <= cl.server_command_sequence.wrapping_sub(ring) {
        if cl.demo_playing != 0 {
            return Ok(ServerCommand::Kept);
        }
        return Err(GameError::drop(
            "CL_GetServerCommand: \x14EXE_ERR_RELIABLE_CYCLED_OUT",
        ));
    }
    if number > cl.server_command_sequence {
        return Err(GameError::drop(
            "CL_GetServerCommand: \x14EXE_ERR_NOT_RECEIVED",
        ));
    }
    cl.last_executed_server_command = number;
    let mut text = cl.server_command(number).to_vec();
    let mut rescanning = false;
    loop {
        tokens.tokenize(game, &text, 0);
        // The first letter, signed (`movsx`): the bytes above 127 are no command.
        let letter = tokens.argv(0).first().copied().unwrap_or(0);
        match letter {
            b'B' | b'n' => {
                cl.cmds.fill(0);
                return Ok(ServerCommand::MapRestart);
            }
            b'd' => {
                tokens.tokenize(game, &text, 3);
                let (index, string) = (tokens.argv(1).to_vec(), tokens.argv(2).to_vec());
                let changed = configstring_modified(cl, &index, &string)?;
                tokens.tokenize(game, &text, 3);
                return Ok(ServerCommand::ConfigString(changed));
            }
            b'w' => {
                let reason = (tokens.argc() >= 2).then(|| tokens.argv(1).to_vec());
                let punkbuster = game == Game::CallOfDuty2
                    && tokens.argc() >= 3
                    && tokens.argv(2).eq_ignore_ascii_case(b"PB");
                return Ok(ServerCommand::Disconnected {
                    reason,
                    localized_reason: !punkbuster,
                });
            }
            b'x' => {
                tokens.tokenize(game, &text, 3);
                let mut formatted = b"d ".to_vec();
                formatted.extend_from_slice(tokens.argv(1));
                formatted.push(b' ');
                formatted.extend_from_slice(tokens.argv(2));
                crate::crt::sprintf(big, BIG_CONFIG_STRING_SIZE, &formatted);
                return Ok(ServerCommand::Kept);
            }
            b'y' | b'z' => {
                tokens.tokenize(game, &text, 3);
                let part = tokens.argv(2);
                let length = crate::cstr::len(big);
                // `y` may fill the buffer to its last byte, `z` one less.
                let room = if letter == b'y' { 0 } else { 1 };
                if length + part.len() + room >= BIG_CONFIG_STRING_SIZE {
                    return Err(GameError::drop("bcs exceeded BIG_INFO_STRING"));
                }
                big[length..length + part.len()].copy_from_slice(part);
                big[length + part.len()] = 0;
                if letter == b'y' {
                    return Ok(ServerCommand::Kept);
                }
                if rescanning && part.is_empty() {
                    // The same text again: the original's endless loop.
                    return Ok(ServerCommand::Endless);
                }
                text = big[..crate::cstr::len(big)].to_vec();
                rescanning = true;
            }
            _ => return Ok(ServerCommand::ClientGame),
        }
    }
}

/// What [`check_timeout`] reads besides the client: the console variables.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct TimeoutSettings {
    /// `cl_paused` and `sv_paused`: the time-out waits while both are set.
    pub client_paused: bool,
    pub server_paused: bool,
    /// `cl_timeout` (seconds, 200 by default in both games).
    pub timeout: f32,
    /// Call of Duty 2's `cl_connectTimeout` (seconds, 200 by default): the
    /// longest wait for the server's answer while joining.
    pub connect_timeout: f32,
}

impl Default for TimeoutSettings {
    fn default() -> Self {
        TimeoutSettings {
            client_paused: false,
            server_paused: false,
            timeout: 200.0,
            connect_timeout: 200.0,
        }
    }
}

/// Whether the server went quiet, once a frame: connected (Call of Duty 2
/// from state 5, United Offensive from 3 but not in its states 7 and 8) and
/// not paused, the time since the server's last message more than
/// `cl_timeout` seconds counts a frame ([`ClientState::timeout_count`]); a
/// sixth such frame in a row is a dropping error, `EXE_ERR_SERVER_TIMEOUT`.
/// Any other frame starts the count again. Call of Duty 2 also gives up
/// joining (states 3 and 4, after a first answer) once the server has been
/// quiet for more than `cl_connectTimeout` seconds, at once.
///
/// The times compare as the originals' x87 code does: the time since the
/// last message, exactly, against the setting times 1000 (rounded to a
/// `float` at Call of Duty 2's precision; exact at United Offensive's); a
/// NaN setting never times out.
///
/// Original: `CL_CheckTimeout` at 0x0040F470 (CoD2MP_s.exe 1.3)
/// Original: `CL_CheckTimeout` at 0x00413390 (CoDUOMP.exe 1.51)
pub fn check_timeout(cl: &mut ClientState, settings: &TimeoutSettings) -> Result<(), GameError> {
    let paused = settings.client_paused && settings.server_paused;
    let quiet = f64::from(cl.realtime.wrapping_sub(cl.last_packet_time));
    let longer = |seconds: f32| match cl.game {
        Game::CallOfDuty2 => quiet > f64::from(seconds * 1000.0),
        Game::UnitedOffensive => quiet > f64::from(seconds) * 1000.0,
    };
    let state = cl.connection_state;
    let connected = match cl.game {
        Game::CallOfDuty2 => state >= 5,
        Game::UnitedOffensive => state >= 3 && state != 7 && state != 8,
    };
    if !paused && connected && longer(settings.timeout) {
        cl.timeout_count = cl.timeout_count.wrapping_add(1);
        if cl.timeout_count > 5 {
            return Err(GameError::drop("EXE_ERR_SERVER_TIMEOUT"));
        }
        return Ok(());
    }
    if cl.game == Game::CallOfDuty2
        && (state == 3 || state == 4)
        && cl.last_packet_time > 0
        && longer(settings.connect_timeout)
    {
        return Err(GameError::drop("EXE_ERR_SERVER_TIMEOUT"));
    }
    cl.timeout_count = 0;
    Ok(())
}

/// Sends the player's settings again when one of them changed: once
/// joining (Call of Duty 2 from state 4, United Offensive from 2) and not
/// paused (`cl_paused`), if the settings' changed flag (2 in
/// `dvar_modifiedFlags` / `cvar_modifiedFlags`) is set, it is cleared and
/// `userinfo "<settings>"` queued ([`add_reliable_command`], whose result
/// this is, or `va`'s error; `None` when nothing was sent). `userinfo` is the settings as an
/// info string (`Dvar_InfoString(2)` / `Cvar_InfoString(2)`, the caller's).
///
/// Original: `CL_CheckUserinfo` at 0x0040F530 (CoD2MP_s.exe 1.3)
/// Original: `CL_CheckUserinfo` at 0x004137C0 (CoDUOMP.exe 1.51)
pub fn check_userinfo(
    cl: &mut ClientState,
    paused: bool,
    modified_flags: &mut i32,
    userinfo: &[u8],
) -> Option<Result<Result<(), crate::net::msg::ReliableCommandWarning>, GameError>> {
    let joining = match cl.game {
        Game::CallOfDuty2 => 4,
        Game::UnitedOffensive => 2,
    };
    if cl.connection_state < joining || paused || *modified_flags & 2 == 0 {
        return None;
    }
    *modified_flags &= !2;
    let mut command = b"userinfo \"".to_vec();
    command.extend_from_slice(&userinfo[..crate::cstr::len(userinfo)]);
    command.push(b'"');
    // (Call of Duty 2's `va` takes at most 1023 characters: longer settings
    // are a dropping error there.)
    let command = match crate::shared::strings::va(cl.game, command) {
        Ok(command) => command,
        Err(e) => return Some(Err(e)),
    };
    Some(add_reliable_command(cl, &command))
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::net::oob::Tokens;

    fn queue(cl: &mut ClientState, text: &[u8]) -> i32 {
        cl.server_command_sequence += 1;
        let ring = max_reliable_commands(cl.game);
        let slot = (cl.server_command_sequence as usize % ring) * SERVER_COMMAND_SIZE;
        cl.server_commands[slot..slot + SERVER_COMMAND_SIZE].fill(0);
        cl.server_commands[slot..slot + text.len()].copy_from_slice(text);
        cl.server_command_sequence
    }

    #[test]
    fn server_commands_change_config_strings_and_end_the_connection() {
        for game in [Game::CallOfDuty2, Game::UnitedOffensive] {
            let mut cl = ClientState::new(game);
            let mut tokens = Box::<Tokens>::default();
            let mut big = [0u8; BIG_CONFIG_STRING_SIZE];
            let mut run = |cl: &mut ClientState, text: &[u8]| {
                let n = queue(cl, text);
                get_server_command(cl, n, &mut tokens, &mut big).unwrap()
            };
            assert_eq!(
                run(&mut cl, b"d 5 hello there"),
                ServerCommand::ConfigString(Some(5))
            );
            assert_eq!(cl.game_state.get(5), b"hello there");
            assert_eq!(
                run(&mut cl, b"d 5 hello there"),
                ServerCommand::ConfigString(None)
            );
            // The text is the rest of the line as it is: quotes stay.
            assert_eq!(
                run(&mut cl, b"d 6 \"quoted\""),
                ServerCommand::ConfigString(Some(6))
            );
            assert_eq!(cl.game_state.get(6), b"\"quoted\"");
            // A big one in three parts, applied by the last (the third word
            // of `y` and `z`, which is the rest of the line too).
            assert_eq!(run(&mut cl, b"x 7 abc"), ServerCommand::Kept);
            assert_eq!(run(&mut cl, b"y 7 def"), ServerCommand::Kept);
            assert_eq!(
                run(&mut cl, b"z 7 ghi"),
                ServerCommand::ConfigString(Some(7))
            );
            assert_eq!(cl.game_state.get(7), b"abcdefghi");
            assert_eq!(cl.game_state.get(5), b"hello there");
            assert_eq!(run(&mut cl, b"e 1 2"), ServerCommand::ClientGame);
            assert_eq!(
                run(&mut cl, b"w \"EXE_KICKED\" PB"),
                ServerCommand::Disconnected {
                    reason: Some(b"EXE_KICKED".to_vec()),
                    localized_reason: game == Game::UnitedOffensive,
                }
            );
            assert_eq!(cl.last_executed_server_command, cl.server_command_sequence);
        }
    }
}
