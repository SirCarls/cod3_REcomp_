//! A LAN game server that the games' own PC clients (Call of Duty 2 1.3,
//! United Offensive 1.51) can find, join and stay in, built on the ported
//! network code ([`crate::net`]).
//!
//! It has no sockets inside: [`Server::packet`] takes a packet that arrived,
//! [`Server::frame`] advances the clock, and both hand the packets to send
//! to a callback. The binary `iw-server` (`engine/tools/iw_server`) wraps it
//! in a UDP loop; tests drive it directly.
//!
//! # What it does (the original's server, `sv_*_mp`)
//!
//! * Answers `getinfo` and `getstatus` (the LAN browser and server lists).
//! * `getchallenge` → `challengeResponse`; `connect` → a client slot and
//!   `connectResponse` (the checks of `SV_DirectConnect`).
//! * Sends the game state (config strings, no baselines), then snapshots at
//!   the server frame rate: the player's own state and the client list.
//! * Reads the client's messages: reliable commands (`userinfo`,
//!   `disconnect`, `cp`, ...) and player commands (the keyed delta coding),
//!   and moves the client from primed to active on its first command for
//!   the current map.
//! * Drops clients that go quiet (`sv_timeout`, 40 s; 2 s for a zombie).
//!
//! What it doesn't do yet: the game itself (the game module's scripts,
//! movement, weapons), downloads, PunkBuster, the authorize and master
//! servers (LAN and direct `connect` only; players get identity 0).

use crate::net::delta::{self, FieldTables};
use crate::net::game_state::{self, GameState, ServerCommands, Snapshot};
use crate::net::huffman::MessageTree;
use crate::net::msg::Msg;
use crate::net::netchan::{self, NetAddress, NetSource, Netchan, Packet};
use crate::net::oob::{self, ChallengeTable, ConnectDecision, Line, ServerRequest, Tokens};
use crate::net::protocol_data::{GameExecutable, ProtocolDataError};
use crate::Game;

mod config;
pub use config::{ServerConfig, ServerInfoKeys};

/// The tables the protocol needs, read once from the player's executable.
pub struct Protocol {
    pub tables: FieldTables,
    pub tree: MessageTree,
}

impl Protocol {
    /// Reads and checks the tables of `exe`'s game.
    pub fn from_executable(exe: &GameExecutable) -> Result<Protocol, ProtocolDataError> {
        Ok(Protocol {
            tables: FieldTables::from_executable(exe)?,
            tree: MessageTree::from_frequencies(&exe.huffman_frequencies()?),
        })
    }
}

/// A client slot's state (`clientState_t`'s `state`).
#[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord)]
pub enum ClientState {
    /// Unused.
    Free = 0,
    /// Disconnected; kept a moment so its last packets are ignored.
    Zombie = 1,
    /// Connected, waiting for its first packet to get the game state.
    Connected = 2,
    /// Has the game state, loading the map.
    Primed = 3,
    /// In the game, getting snapshots.
    Active = 4,
}

/// Snapshot frames kept per client (`PACKET_BACKUP`).
const PACKET_BACKUP: usize = 32;
/// Most player commands in one client message (`MAX_PACKET_USERCMDS`).
const MAX_PACKET_USERCMDS: usize = 32;
/// The client message ops (`clc_ops_e`).
const CLC_MOVE: i32 = 0;
const CLC_MOVE_NO_DELTA: i32 = 1;
const CLC_CLIENT_COMMAND: i32 = 2;
const CLC_EOF: i32 = 3;
/// Characters of a reliable command that key the player commands
/// (`Com_HashKey(.., 32)`).
const COMMAND_KEY_CHARS: i32 = 32;
/// A client that sends nothing for this long is dropped (`sv_timeout`).
const TIMEOUT_MS: i32 = 40_000;
/// A zombie is freed after this long (`sv_zombietime`).
const ZOMBIE_MS: i32 = 2_000;

/// One kept snapshot: the player state and client list the client was sent.
#[derive(Clone)]
struct Frame {
    player: Vec<u8>,
    clients: Vec<Vec<u8>>,
    /// The server time it was sent at (`messageSent`), -1 when unused.
    sent: i32,
}

struct Client {
    state: ClientState,
    chan: Netchan,
    challenge: i32,
    userinfo: Vec<u8>,
    name: Vec<u8>,
    /// Server-to-client reliable commands, a ring of
    /// [`netchan::max_reliable_commands`] texts.
    reliable_commands: Vec<Vec<u8>>,
    reliable_sequence: i32,
    reliable_acknowledge: i32,
    last_client_command: i32,
    last_client_command_text: Vec<u8>,
    message_acknowledge: i32,
    gamestate_message_num: i32,
    server_id: i32,
    /// The snapshot the next one is a change from (-1: none).
    delta_message: i32,
    last_packet_time: i32,
    last_connect_time: i32,
    last_usercmd: Vec<u8>,
    frames: Vec<Frame>,
}

/// The server.
pub struct Server {
    config: ServerConfig,
    protocol: Protocol,
    game: Game,
    /// Milliseconds since the server started (`svs.time`).
    time: i32,
    /// The map's id: +0x10 per map, low nibble +1 per restart.
    server_id: i32,
    checksum_feed: i32,
    snap_flag_server_bit: u8,
    config_strings: Vec<Vec<u8>>,
    challenges: ChallengeTable,
    clients: Vec<Option<Client>>,
    random: u32,
    next_frame: i32,
    // Scratch buffers kept between packets (no allocation per packet).
    line: Line,
    tokens: Tokens,
    /// Outgoing messages are written here before coding.
    message: Vec<u8>,
    /// Incoming messages are decompressed here.
    incoming: Vec<u8>,
    coded: Vec<u8>,
    scratch: Vec<u8>,
    out_packet: Packet,
}

/// Where packets go: the destination and the bytes.
pub type Sink<'a> = &'a mut dyn FnMut(&NetAddress, &[u8]);

impl Server {
    /// A server for `config.game`, on `config.map`. `seed` seeds the
    /// challenge numbers and the checksum feed.
    pub fn new(config: ServerConfig, protocol: Protocol, seed: u32) -> Server {
        let game = config.game;
        let size = game_state::game_state_buffer_size(game);
        let mut server = Server {
            game,
            time: 0,
            server_id: 0x10,
            checksum_feed: 0,
            snap_flag_server_bit: 0,
            config_strings: vec![Vec::new(); game_state::MAX_CONFIG_STRINGS],
            challenges: ChallengeTable::default(),
            clients: (0..config.max_clients).map(|_| None).collect(),
            random: seed | 1,
            next_frame: 0,
            line: Line::default(),
            tokens: Tokens::default(),
            message: vec![0; size],
            incoming: vec![0; game_state::game_state_buffer_size(game)],
            coded: vec![0; size + 64],
            scratch: vec![0; size + 64],
            out_packet: Packet::default(),
            config,
            protocol,
        };
        server.checksum_feed = server.next_random() as i32;
        server.build_config_strings();
        server
    }

    /// The game this server speaks.
    pub fn game(&self) -> Game {
        self.game
    }

    /// Milliseconds since the server started.
    pub fn time(&self) -> i32 {
        self.time
    }

    /// How many slots are in use (connected or later).
    pub fn player_count(&self) -> usize {
        self.clients
            .iter()
            .flatten()
            .filter(|c| c.state >= ClientState::Connected)
            .count()
    }

    fn next_random(&mut self) -> u32 {
        // xorshift32: the challenge numbers only need to be hard to guess per run.
        let mut x = self.random;
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        self.random = x;
        x
    }

    fn build_config_strings(&mut self) {
        for (index, text) in self
            .config
            .config_strings(self.game, self.server_id, self.time)
        {
            if let Some(slot) = self.config_strings.get_mut(index) {
                *slot = text;
            }
        }
    }

    // --- packets in -------------------------------------------------------------------

    /// Handles a packet from `from`.
    pub fn packet(&mut self, from: &NetAddress, data: &[u8], out: Sink) {
        if data.len() >= 4 && data[..4] == oob::CONNECTIONLESS_PREFIX {
            self.connectionless(from, data, out);
            return;
        }
        if data.len() < 6 {
            return;
        }
        let qport = i32::from(u16::from_le_bytes([data[4], data[5]]));
        let found = self.clients.iter().position(|c| {
            c.as_ref().is_some_and(|c| {
                c.state != ClientState::Free
                    && oob::same_base_address(self.game, from, &c.chan.remote_address)
                    && c.chan.qport == qport
            })
        });
        let Some(slot) = found else {
            self.send_oob(from, b"disconnect", out);
            return;
        };
        self.client_packet(slot, from, data, out);
    }

    fn connectionless(&mut self, from: &NetAddress, data: &[u8], out: Sink) {
        let mut decompressed = Vec::new();
        let packet: &[u8] = if oob::connect_is_compressed(self.game, data) {
            // United Offensive's `connect` is Huffman-coded (adaptively) from byte 12.
            decompressed.extend_from_slice(data);
            decompressed.resize(oob::MAX_LINE * 4, 0);
            let mut msg = Msg::new(self.game, &mut decompressed);
            msg.cursize = data.len();
            crate::net::huffman::decompress_adaptive(&mut msg, 12);
            let n = msg.cursize;
            decompressed.truncate(n);
            &decompressed
        } else {
            data
        };
        match oob::server_request(self.game, packet, &mut self.line, &mut self.tokens) {
            ServerRequest::Info => self.answer_info(from, out),
            ServerRequest::Status => self.answer_status(from, out),
            ServerRequest::Challenge => self.answer_challenge(from, out),
            ServerRequest::Connect => self.direct_connect(from, out),
            _ => {}
        }
    }

    fn client_states(&self) -> Vec<i32> {
        self.clients
            .iter()
            .map(|c| c.as_ref().map_or(0, |c| c.state as i32))
            .collect()
    }

    fn answer_info(&mut self, from: &NetAddress, out: Sink) {
        let challenge = self.tokens.argv(1).to_vec();
        let states = self.client_states();
        let info = oob::ServerInfo {
            challenge: &challenge,
            hostname: self.config.hostname.as_bytes(),
            mapname: self.config.map.as_bytes(),
            gametype: self.config.gametype.as_bytes(),
            max_clients: self.config.max_clients as i32,
            client_states: &states,
            dedicated: true,
            voice: 0,
            ..Default::default()
        };
        let mut text = vec![0u8; 0x2000];
        let n = oob::info_response(self.game, &info, &mut text);
        self.send_oob(from, &text[..n], out);
    }

    fn answer_status(&mut self, from: &NetAddress, out: Sink) {
        let challenge = self.tokens.argv(1).to_vec();
        let server_info = self.config_strings[0].clone();
        let names: Vec<Vec<u8>> = self
            .clients
            .iter()
            .flatten()
            .filter(|c| c.state >= ClientState::Connected)
            .map(|c| c.name.clone())
            .collect();
        let players: Vec<oob::PlayerStatus> = names
            .iter()
            .map(|n| oob::PlayerStatus {
                score: 0,
                ping: 0,
                name: n,
            })
            .collect();
        let status = oob::ServerStatus {
            server_info: &server_info,
            challenge: &challenge,
            players: &players,
            ..Default::default()
        };
        let mut text = vec![0u8; 0x4000];
        let n = oob::status_response(self.game, &status, &mut text);
        self.send_oob(from, &text[..n], out);
    }

    fn answer_challenge(&mut self, from: &NetAddress, out: Sink) {
        let request = oob::ChallengeRequest {
            from: *from,
            time: self.time,
            lan_authorize: false,
            from_lan: true,
            ..Default::default()
        };
        let number = self.next_random() as i32 & 0x7FFF_FFFF;
        let answer = oob::get_challenge(self.game, &mut self.challenges, &request, || number);
        if let oob::ChallengeAnswer::Respond { slot } = answer {
            let challenge = self.challenges.slots[slot].challenge;
            let mut text = [0u8; 64];
            let n = oob::challenge_response_text(self.game, challenge, 0, &mut text);
            self.send_oob(from, &text[..n], out);
        }
    }

    fn direct_connect(&mut self, from: &NetAddress, out: Sink) {
        let userinfo = self.tokens.argv(1).to_vec();
        let infos: Vec<oob::ClientSlotInfo> = self
            .clients
            .iter()
            .map(|c| match c {
                Some(c) => oob::ClientSlotInfo {
                    state: c.state as i32,
                    address: c.chan.remote_address,
                    qport: c.chan.qport,
                    last_connect_time: c.last_connect_time,
                },
                None => oob::ClientSlotInfo::default(),
            })
            .collect();
        let request = oob::ConnectRequest {
            from: *from,
            userinfo: &userinfo,
            time: self.time,
            reconnect_limit: 0,
            max_clients: self.config.max_clients as i32,
            from_lan: true,
            clients: &infos,
            ..Default::default()
        };
        let decision = oob::direct_connect(self.game, &mut self.challenges, &request);
        let (slot, challenge_slot) = match decision {
            ConnectDecision::NewSlot { slot, challenge } => (slot, challenge),
            ConnectDecision::Reconnect {
                slot, challenge, ..
            } => (slot, challenge),
            other => {
                if let Some(reply) = other.reply(self.game) {
                    self.send_oob(from, reply, out);
                }
                return;
            }
        };
        let info_text = crate::cstr::text(&userinfo);
        let value = |key: &[u8]| {
            crate::shared::info::value_for_key(info_text, key)
                .unwrap_or(b"")
                .to_vec()
        };
        let qport = oob::atol(&value(b"qport")) & 0xFFFF;
        let challenge = challenge_slot.map_or(0, |i| self.challenges.slots[i].challenge);
        let commands = netchan::max_reliable_commands(self.game);
        let client = Client {
            state: ClientState::Connected,
            chan: Netchan::new(self.game, NetSource::Server, *from, qport),
            challenge,
            name: value(b"name"),
            userinfo: info_text.to_vec(),
            reliable_commands: vec![Vec::new(); commands],
            reliable_sequence: 0,
            reliable_acknowledge: 0,
            last_client_command: 0,
            last_client_command_text: Vec::new(),
            message_acknowledge: 0,
            gamestate_message_num: -1,
            server_id: 0,
            delta_message: -1,
            last_packet_time: self.time,
            last_connect_time: self.time,
            last_usercmd: vec![0; delta::usercmd_size(self.game)],
            frames: vec![
                Frame {
                    player: Vec::new(),
                    clients: Vec::new(),
                    sent: -1,
                };
                PACKET_BACKUP
            ],
        };
        self.clients[slot] = Some(client);
        self.send_oob(from, b"connectResponse", out);
    }

    fn client_packet(&mut self, slot: usize, from: &NetAddress, data: &[u8], out: Sink) {
        let game = self.game;
        let mut buffer = data.to_vec();
        buffer.resize(data.len().max(netchan::MAX_PACKET_SIZE) + 16, 0);
        let mut msg = Msg::new(game, &mut buffer);
        msg.cursize = data.len();
        let Some(client) = self.clients[slot].as_mut() else {
            return;
        };
        if client.chan.remote_address.port != from.port {
            client.chan.remote_address.port = from.port; // a translated port (NAT)
        }
        if !client.chan.process(&mut msg) {
            return;
        }
        // The header the client writes raw: server id, last server message, last
        // server command it has.
        let server_id = msg.read_byte();
        let message_acknowledge = msg.read_long();
        client.server_id = server_id;
        client.message_acknowledge = message_acknowledge;
        if message_acknowledge < 0 {
            return;
        }
        let reliable_acknowledge = msg.read_long();
        client.reliable_acknowledge = reliable_acknowledge;
        if client.reliable_sequence.wrapping_sub(reliable_acknowledge)
            >= client.reliable_commands.len() as i32
        {
            client.reliable_acknowledge = client.reliable_sequence;
            return;
        }
        // Unscramble the coded body with the key both sides know.
        let ring = client.reliable_commands.len();
        let text = &client.reliable_commands[reliable_acknowledge as u32 as usize % ring];
        let key = netchan::server_decode_key(client.challenge, server_id, message_acknowledge);
        let start = msg.readcount.min(msg.cursize);
        let end = msg.cursize;
        netchan::scramble(&mut msg.data[start..end], key, text);
        if client.state == ClientState::Zombie {
            return;
        }
        client.last_packet_time = self.time;
        // Decompress the body and read it.
        let coded = &msg.data[start..end];
        let n = self
            .protocol
            .tree
            .read_bits_compress(coded, &mut self.incoming);
        self.execute_client_message(slot, n, out);
    }

    fn execute_client_message(&mut self, slot: usize, length: usize, out: Sink) {
        let game = self.game;
        let mut body = std::mem::take(&mut self.incoming);
        let length = length.min(body.len());
        {
            let mut msg = Msg::new(game, &mut body);
            msg.cursize = length;
            let op_bits = match game {
                Game::CallOfDuty2 => 3,
                Game::UnitedOffensive => 2,
            };
            let server_id = self.clients[slot].as_ref().map_or(-1, |c| c.server_id);
            if server_id == self.server_id {
                let mut op = msg.read_bits(op_bits);
                while op == CLC_CLIENT_COMMAND {
                    if !self.client_command(slot, &mut msg, out) {
                        break;
                    }
                    if self.clients[slot]
                        .as_ref()
                        .map_or(true, |c| c.state == ClientState::Zombie)
                    {
                        break;
                    }
                    op = msg.read_bits(op_bits);
                }
                if op == CLC_MOVE || op == CLC_MOVE_NO_DELTA {
                    self.user_move(slot, &mut msg, op == CLC_MOVE);
                }
                let _ = CLC_EOF;
            } else if (server_id ^ self.server_id) & 0xF0 == 0 {
                // The same map, restarted: a primed client enters the game.
                if let Some(c) = self.clients[slot].as_mut() {
                    if c.state == ClientState::Primed {
                        c.state = ClientState::Active;
                    }
                }
            } else if let Some(c) = self.clients[slot].as_ref() {
                if c.message_acknowledge > c.gamestate_message_num {
                    // The client never got this map's game state: send it again.
                    self.send_game_state(slot, out);
                }
            }
        }
        self.incoming = body;
    }

    /// One reliable command from the client. Returns false when the client
    /// was dropped.
    fn client_command(&mut self, slot: usize, msg: &mut Msg, out: Sink) -> bool {
        let sequence = msg.read_long();
        let mut text = [0u8; crate::net::msg::MAX_STRING_CHARS];
        let command = msg.read_string(&mut text).to_vec();
        let Some(client) = self.clients[slot].as_mut() else {
            return false;
        };
        if sequence <= client.last_client_command {
            return true; // already done
        }
        if sequence > client.last_client_command.wrapping_add(1) {
            self.drop_client(slot, b"EXE_LOSTRELIABLECOMMANDS", out);
            return false;
        }
        client.last_client_command = sequence;
        client.last_client_command_text = command.clone();
        self.tokens.tokenize(self.game, &command, 0);
        let name = self.tokens.argv(0).to_vec();
        match name.as_slice() {
            b"userinfo" => {
                let info = self.tokens.argv(1).to_vec();
                if let Some(c) = self.clients[slot].as_mut() {
                    let text = crate::cstr::text(&info).to_vec();
                    c.name = crate::shared::info::value_for_key(&text, b"name")
                        .unwrap_or(b"")
                        .to_vec();
                    c.userinfo = text;
                }
            }
            b"disconnect" => {
                self.drop_client(slot, b"EXE_DISCONNECTED", out);
                return false;
            }
            // `cp` (pure checksums), downloads, `vdr`: nothing to check on an
            // unpure server without downloads; game commands (`say`, `mr`,
            // `team` ...) belong to the game module, which this server doesn't
            // run yet.
            _ => {}
        }
        true
    }

    /// Player commands: read with the key both sides know; the first one for
    /// the current map makes a primed client active.
    fn user_move(&mut self, slot: usize, msg: &mut Msg, delta_compressed: bool) {
        let game = self.game;
        let Some(client) = self.clients[slot].as_mut() else {
            return;
        };
        client.delta_message = if delta_compressed {
            client.message_acknowledge
        } else {
            -1
        };
        let count = msg.read_byte();
        if count < 1 || count as usize > MAX_PACKET_USERCMDS {
            return;
        }
        let ring = client.reliable_commands.len();
        let acknowledged =
            &client.reliable_commands[client.reliable_acknowledge as u32 as usize % ring];
        let key = crate::shared::hash::hash_key(acknowledged, COMMAND_KEY_CHARS)
            ^ client.message_acknowledge
            ^ self.checksum_feed;
        let size = delta::usercmd_size(game);
        let mut old = vec![0u8; size];
        let mut cmd = vec![0u8; size];
        for _ in 0..count {
            delta::read_delta_usercmd_key(msg, key, &old, &mut cmd);
            std::mem::swap(&mut old, &mut cmd);
        }
        client.last_usercmd.copy_from_slice(&old);
        if client.state == ClientState::Primed {
            client.state = ClientState::Active;
            client.delta_message = -1;
        }
    }

    // --- packets out ------------------------------------------------------------------

    fn send_oob(&mut self, to: &NetAddress, text: &[u8], out: Sink) {
        let mut packet = vec![0u8; text.len() + 8];
        if let Some(n) = oob::out_of_band_print(self.game, text, &mut packet) {
            out(to, &packet[..n]);
        }
    }

    /// Queues a reliable server command (`SV_AddServerCommand`).
    fn add_server_command(&mut self, slot: usize, text: &[u8]) {
        if let Some(c) = self.clients[slot].as_mut() {
            c.reliable_sequence = c.reliable_sequence.wrapping_add(1);
            let ring = c.reliable_commands.len();
            let i = c.reliable_sequence as u32 as usize % ring;
            c.reliable_commands[i] = text.to_vec();
        }
    }

    fn drop_client(&mut self, slot: usize, reason: &[u8], out: Sink) {
        let mut command = b"w \"".to_vec();
        command.extend_from_slice(reason);
        command.push(b'"');
        self.add_server_command(slot, &command);
        // One last message so the client sees the reason.
        if let Some(c) = self.clients[slot].as_mut() {
            c.state = ClientState::Active;
        }
        self.send_snapshot(slot, out);
        if let Some(c) = self.clients[slot].as_mut() {
            c.state = ClientState::Zombie;
            c.last_packet_time = self.time;
        }
    }

    /// Writes a message (its first 4 bytes the last client command) through
    /// the Huffman coder, the scrambling and the netchan.
    fn transmit(&mut self, slot: usize, length: usize, out: Sink) -> Option<netchan::SentMessage> {
        let client = self.clients[slot].as_mut()?;
        let head = [
            self.message[0],
            self.message[1],
            self.message[2],
            self.message[3],
        ];
        let n = self
            .protocol
            .tree
            .write_bits_compress(&self.message[4..length], &mut self.coded);
        let sent = netchan::send_server_message(
            &mut client.chan,
            client.challenge,
            &client.last_client_command_text,
            head,
            &self.coded[..n],
            &mut self.scratch,
            &mut self.out_packet,
        )
        .ok()?;
        out(&client.chan.remote_address, self.out_packet.as_bytes());
        while client.chan.unsent_fragments {
            client.chan.transmit_next_fragment(&mut self.out_packet);
            out(&client.chan.remote_address, self.out_packet.as_bytes());
        }
        Some(sent)
    }

    fn send_game_state(&mut self, slot: usize, out: Sink) {
        let game = self.game;
        let Some(client) = self.clients[slot].as_mut() else {
            return;
        };
        client.state = ClientState::Primed;
        client.gamestate_message_num = client.chan.outgoing_sequence;
        let texts: Vec<&[u8]> = client.reliable_commands.iter().map(Vec::as_slice).collect();
        let strings: Vec<&[u8]> = self.config_strings.iter().map(Vec::as_slice).collect();
        let state = GameState {
            last_client_command: client.last_client_command,
            commands: ServerCommands {
                acknowledged: client.reliable_acknowledge,
                sequence: client.reliable_sequence,
                texts: &texts,
            },
            config_strings: &strings,
            baselines: &[],
            client_num: slot as i32,
            checksum_feed: self.checksum_feed,
        };
        let mut msg = Msg::new(game, &mut self.message);
        let _ = game_state::write_game_state(&mut msg, &self.protocol.tables, &state);
        let length = msg.cursize;
        drop(texts);
        drop(strings);
        self.transmit(slot, length, out);
    }

    /// The player state this server gives a client: still, at the map's
    /// origin, as a spectator (no game module yet to place it).
    fn player_state(&self, slot: usize) -> Vec<u8> {
        let tables = &self.protocol.tables;
        let mut ps = vec![0u8; delta::player_state_size(self.game)];
        let mut set = |name: &str, value: i32| {
            if let Some(f) = tables.player.field(name) {
                let at = f.offset as usize;
                ps[at..at + 4].copy_from_slice(&value.to_le_bytes());
            }
        };
        set("commandTime", self.time);
        set("clientNum", slot as i32);
        set("pm_type", self.config.spectator_pm_type(self.game));
        set("viewHeightTarget", 60);
        set("viewHeightCurrent", 60.0f32.to_bits() as i32);
        ps
    }

    /// The client list: every connected player's client state (number, name).
    fn client_list(&self) -> Vec<Vec<u8>> {
        let tables = &self.protocol.tables;
        let name_at = tables.client.field("name[0]").map(|f| f.offset as usize);
        self.clients
            .iter()
            .enumerate()
            .filter_map(|(i, c)| c.as_ref().map(|c| (i, c)))
            .filter(|(_, c)| c.state >= ClientState::Primed)
            .map(|(i, c)| {
                let mut cs = vec![0u8; 0x5C];
                cs[..4].copy_from_slice(&(i as i32).to_le_bytes());
                if let Some(at) = name_at {
                    let n = c.name.len().min(31);
                    cs[at..at + n].copy_from_slice(&c.name[..n]);
                }
                cs
            })
            .collect()
    }

    /// A client loading the map gets only the last client command and the
    /// end marker each frame (its reliable commands and snapshots wait until
    /// it is in the game), as `SV_SendClientSnapshot` does for it.
    fn send_primed_message(&mut self, slot: usize, out: Sink) {
        let game = self.game;
        let Some(client) = self.clients[slot].as_ref() else {
            return;
        };
        let last_command = client.last_client_command;
        let mut msg = Msg::new(game, &mut self.message);
        msg.write_long(last_command);
        msg.write_byte(game_state::server_ops(game).eof);
        let length = msg.cursize;
        self.transmit(slot, length, out);
    }

    fn send_snapshot(&mut self, slot: usize, out: Sink) {
        let game = self.game;
        let player = self.player_state(slot);
        let clients = self.client_list();
        let Some(client) = self.clients[slot].as_mut() else {
            return;
        };
        let frame_index = (client.chan.outgoing_sequence as u32 as usize) % PACKET_BACKUP;
        // The frame the client last acknowledged, if it is recent enough.
        let mut delta_num = 0u8;
        let mut from: Option<Frame> = None;
        if client.delta_message > 0 && client.state == ClientState::Active {
            let back = client
                .chan
                .outgoing_sequence
                .wrapping_sub(client.delta_message);
            if (1..(PACKET_BACKUP as i32 - 3)).contains(&back) {
                let old = &client.frames[(client.delta_message as u32 as usize) % PACKET_BACKUP];
                if old.sent >= 0 && !old.player.is_empty() {
                    delta_num = back as u8;
                    from = Some(old.clone());
                }
            }
        }
        let mut flags = self.snap_flag_server_bit;
        if client.state != ClientState::Active {
            flags |= 2; // not active yet
        }
        let last_command = client.last_client_command;
        let empty: Vec<Vec<u8>> = Vec::new();
        let from_clients: Vec<&[u8]> = from
            .as_ref()
            .map_or(&empty, |f| &f.clients)
            .iter()
            .map(Vec::as_slice)
            .collect();
        let to_clients: Vec<&[u8]> = clients.iter().map(Vec::as_slice).collect();
        let zero_entity = [0u8; 0x100];
        let baseline = |_n: i32| -> &[u8] { &zero_entity };
        let snapshot = Snapshot {
            server_time: self.time,
            delta_num,
            snap_flags: flags,
            player_from: from.as_ref().map(|f| f.player.as_slice()),
            player_to: &player,
            entities_from: &[],
            entities_to: &[],
            baseline: &baseline,
            clients_from: &from_clients,
            clients_to: &to_clients,
            padding: 0,
        };
        // Pending reliable commands go first, then the snapshot.
        let texts: Vec<&[u8]> = client.reliable_commands.iter().map(Vec::as_slice).collect();
        let commands = ServerCommands {
            acknowledged: client.reliable_acknowledge,
            sequence: client.reliable_sequence,
            texts: &texts,
        };
        let mut msg = Msg::new(game, &mut self.message);
        msg.write_long(last_command);
        let _ = game_state::write_server_commands(&mut msg, &commands);
        game_state::write_snapshot(&mut msg, &self.protocol.tables, &snapshot);
        msg.write_byte(game_state::server_ops(game).eof);
        let length = msg.cursize;
        drop(texts);
        drop(from_clients);
        drop(to_clients);
        let time = self.time;
        if self.transmit(slot, length, out).is_some() {
            if let Some(c) = self.clients[slot].as_mut() {
                c.frames[frame_index] = Frame {
                    player,
                    clients,
                    sent: time,
                };
            }
        }
    }

    // --- the clock --------------------------------------------------------------------

    /// Advances the clock by `elapsed_ms` and runs the server frames that
    /// fall in it: game states for new clients, snapshots, timeouts.
    pub fn frame(&mut self, elapsed_ms: i32, out: Sink) {
        self.time = self.time.wrapping_add(elapsed_ms.max(0));
        let period = 1000 / self.config.fps.max(1) as i32;
        while self.time.wrapping_sub(self.next_frame) >= 0 {
            self.next_frame = self.next_frame.wrapping_add(period);
            self.server_frame(out);
        }
    }

    fn server_frame(&mut self, out: Sink) {
        for slot in 0..self.clients.len() {
            let Some(state) = self.clients[slot].as_ref().map(|c| c.state) else {
                continue;
            };
            let quiet = self.clients[slot]
                .as_ref()
                .map_or(0, |c| self.time.wrapping_sub(c.last_packet_time));
            match state {
                ClientState::Zombie if quiet > ZOMBIE_MS => self.clients[slot] = None,
                ClientState::Zombie | ClientState::Free => {}
                _ if quiet > TIMEOUT_MS => self.drop_client(slot, b"EXE_TIMEDOUT", out),
                // A connected client gets its game state when its first packet
                // arrives (it has no server id yet: `execute_client_message`).
                ClientState::Connected => {}
                ClientState::Primed => self.send_primed_message(slot, out),
                ClientState::Active => self.send_snapshot(slot, out),
            }
        }
    }
}
