//! A whole session against the server, played by a stand-in client built
//! from the crate's own client-side pieces: the server answers `getinfo`,
//! hands out a challenge, accepts `connect`, sends the game state when the
//! client's first message arrives, lets the client in on its first player
//! command and then sends snapshots.
//!
//! Needs the games' executables for the protocol tables: set `IW_PC_INPUT`
//! to the folders holding `CoD2MP_s.exe` / `CoDUOMP.exe` (the test is
//! skipped otherwise).

use iw_pc::net::delta;
use iw_pc::net::game_state::server_ops;
use iw_pc::net::huffman;
use iw_pc::net::msg::{Msg, BIG_INFO_STRING};
use iw_pc::net::netchan::{self, NetAddress, NetSource, Netchan, Packet};
use iw_pc::net::oob;
use iw_pc::net::protocol_data::{self, GameExecutable};
use iw_pc::server::{Protocol, Server, ServerConfig};
use iw_pc::shared::hash::hash_key;
use iw_pc::Game;

fn protocol(game: Game) -> Option<Protocol> {
    let folders = std::env::var_os("IW_PC_INPUT")?;
    let path = protocol_data::find_executable(game, &folders)?;
    let exe = GameExecutable::open(game, &path).ok()?;
    Some(Protocol::from_executable(&exe).expect("protocol tables"))
}

/// Packets the server sent, by destination.
#[derive(Default)]
struct Wire(Vec<(NetAddress, Vec<u8>)>);

impl Wire {
    fn take(&mut self) -> Vec<Vec<u8>> {
        std::mem::take(&mut self.0)
            .into_iter()
            .map(|(_, p)| p)
            .collect()
    }
}

fn oob_packet(game: Game, text: &[u8]) -> Vec<u8> {
    let mut out = vec![0u8; text.len() + 8];
    let n = oob::out_of_band_print(game, text, &mut out).unwrap();
    out.truncate(n);
    out
}

fn oob_text(packet: &[u8]) -> &[u8] {
    assert_eq!(&packet[..4], &[0xFF; 4], "not out of band: {packet:?}");
    &packet[4..]
}

/// The stand-in client's state.
struct Client {
    game: Game,
    chan: Netchan,
    challenge: i32,
    server_id: i32,
    checksum_feed: i32,
    server_message_sequence: i32,
    server_command_sequence: i32,
    server_commands: Vec<Vec<u8>>,
    reliable_commands: Vec<Vec<u8>>,
    reliable_sequence: i32,
    reliable_acknowledge: i32,
}

impl Client {
    /// A message to the server: the raw header, then the ops, Huffman-coded
    /// and scrambled (as `CL_WritePacket` and `CL_Netchan_Encode` do).
    fn message(&mut self, tree: &huffman::MessageTree, ops: &[u8]) -> Vec<u8> {
        let mut body = vec![0u8; 4096];
        let n = tree.write_bits_compress(ops, &mut body);
        let mut data = Vec::new();
        data.push(self.server_id as u8);
        data.extend_from_slice(&self.server_message_sequence.to_le_bytes());
        data.extend_from_slice(&self.server_command_sequence.to_le_bytes());
        let start = data.len();
        data.extend_from_slice(&body[..n]);
        let ring = self.server_commands.len();
        let text = self.server_commands[self.server_command_sequence as usize % ring].clone();
        let key = netchan::client_encode_key(
            self.challenge,
            self.server_id,
            self.server_message_sequence,
        );
        netchan::scramble(&mut data[start..], key, &text);
        let mut packet = Packet::default();
        self.chan.transmit(&data, &mut packet).unwrap();
        packet.as_bytes().to_vec()
    }

    /// A message from the server, unscrambled and decompressed: the ops.
    fn receive(&mut self, tree: &huffman::MessageTree, packet: &[u8]) -> Option<Vec<u8>> {
        let mut buffer = packet.to_vec();
        buffer.resize(packet.len() + 64, 0);
        let mut msg = Msg::new(self.game, &mut buffer);
        msg.cursize = packet.len();
        if !self.chan.process(&mut msg) {
            return None;
        }
        self.server_message_sequence = self.chan.incoming_sequence;
        let acknowledged = msg.read_long();
        self.reliable_acknowledge = acknowledged;
        let ring = self.reliable_commands.len();
        let text = self.reliable_commands[acknowledged as u32 as usize % ring].clone();
        let key = netchan::client_decode_key(self.challenge, self.server_message_sequence);
        let (start, end) = (msg.readcount, msg.cursize);
        netchan::scramble(&mut msg.data[start..end], key, &text);
        let mut ops = vec![0u8; 0x20000];
        let n = tree.read_bits_compress(&msg.data[start..end], &mut ops);
        ops.truncate(n);
        Some(ops)
    }
}

fn session(game: Game) {
    let Some(protocol) = protocol(game) else {
        eprintln!("IW_PC_INPUT not set or no executable: {game:?} session skipped");
        return;
    };
    let tree = protocol.tree.clone();
    let tables = protocol.tables.clone();
    let mut config = ServerConfig::new(game, "mp_test");
    config.hostname = "test server".into();
    let mut server = Server::new(config, protocol, 12345);
    let me = NetAddress::ip([192, 168, 1, 20], 28961);
    let mut wire = Wire::default();
    let send = |server: &mut Server, data: &[u8], wire: &mut Wire| {
        server.packet(&me, data, &mut |to, bytes| {
            wire.0.push((*to, bytes.to_vec()))
        });
    };

    // The LAN browser asks.
    send(&mut server, &oob_packet(game, b"getinfo xyz"), &mut wire);
    let info = wire.take();
    let text = oob_text(&info[0]);
    assert!(
        text.starts_with(b"infoResponse\n"),
        "{}",
        String::from_utf8_lossy(text)
    );
    let info_string = &text[b"infoResponse\n".len()..];
    assert!(oob::info_response_accepted(game, info_string, b"", 0.0));
    let value = |key: &[u8]| {
        iw_pc::shared::info::value_for_key(info_string, key)
            .unwrap_or(b"")
            .to_vec()
    };
    assert_eq!(value(b"challenge"), b"xyz");
    assert_eq!(value(b"hostname"), b"test server");
    assert_eq!(value(b"mapname"), b"mp_test");

    // Challenge, then connect.
    send(&mut server, &oob_packet(game, b"getchallenge"), &mut wire);
    let reply = wire.take();
    let text = oob_text(&reply[0]);
    assert!(text.starts_with(b"challengeResponse "));
    let challenge = oob::atol(&text[b"challengeResponse ".len()..]);
    let qport = 4242;
    let userinfo = format!(
        "\"\\protocol\\{}\\challenge\\{challenge}\\qport\\{qport}\\name\\Tester\\rate\\25000\"",
        oob::protocol_version(game)
    );
    let mut connect = oob_packet(game, format!("connect {userinfo}").as_bytes());
    if game == Game::UnitedOffensive {
        // United Offensive's client compresses `connect` from byte 12.
        connect.resize(connect.len() * 2 + 64, 0);
        let length = connect.len();
        let used = 4 + format!("connect {userinfo}").len();
        let mut msg = Msg::new(game, &mut connect[..length]);
        msg.cursize = used;
        huffman::compress_adaptive(&mut msg, 12);
        let n = msg.cursize;
        connect.truncate(n);
    }
    send(&mut server, &connect, &mut wire);
    let reply = wire.take();
    assert_eq!(oob_text(&reply[0]), b"connectResponse");
    assert_eq!(server.player_count(), 1);

    let ring = netchan::max_reliable_commands(game);
    let mut client = Client {
        game,
        chan: Netchan::new(game, NetSource::Client, me, qport),
        challenge,
        server_id: 0,
        checksum_feed: 0,
        server_message_sequence: 0,
        server_command_sequence: 0,
        server_commands: vec![Vec::new(); ring],
        reliable_commands: vec![Vec::new(); ring],
        reliable_sequence: 0,
        reliable_acknowledge: 0,
    };
    let ops = server_ops(game);
    let op_bits = if game == Game::CallOfDuty2 { 3 } else { 2 };

    // The client's first message (its server id is still 0): the game state comes back.
    let mut body = [0u8; 64];
    let mut m = Msg::new(game, &mut body);
    m.write_bits(3, op_bits); // clc_EOF
    let n = m.cursize;
    let first = client.message(&tree, &body[..n]);
    send(&mut server, &first, &mut wire);
    let packets = wire.take();
    assert!(!packets.is_empty(), "no game state");
    let mut joined: Option<Vec<u8>> = None;
    for p in &packets {
        if let Some(ops) = client.receive(&tree, p) {
            joined = Some(ops);
        }
    }
    let gamestate = joined.expect("game state message");
    let mut buffer = gamestate.clone();
    let mut msg = Msg::new(game, &mut buffer);
    msg.cursize = gamestate.len();
    let mut config_strings = std::collections::BTreeMap::new();
    loop {
        let op = msg.read_byte();
        if op == i32::from(ops.game_state) {
            client.server_command_sequence = msg.read_long();
        } else if op == i32::from(ops.config_string) {
            let index = msg.read_short();
            let mut text = [0u8; BIG_INFO_STRING];
            let s = msg.read_big_string(&mut text).to_vec();
            config_strings.insert(index, s);
        } else if op == i32::from(ops.eof) {
            let client_num = msg.read_long();
            client.checksum_feed = msg.read_long();
            assert_eq!(client_num, 0);
            break;
        } else {
            panic!("unexpected op {op} in the game state");
        }
    }
    assert_eq!(config_strings.get(&2).map(Vec::as_slice), Some(&b"cod"[..]));
    let system_info = config_strings.get(&1).expect("system info");
    let id = iw_pc::shared::info::value_for_key(system_info, b"sv_serverid").unwrap();
    client.server_id = oob::atol(id);
    let server_info = config_strings.get(&0).expect("server info");
    assert_eq!(
        iw_pc::shared::info::value_for_key(server_info, b"mapname").ok(),
        Some(&b"mp_test"[..])
    );

    // A frame while the client loads: only keep-alive messages.
    server.frame(100, &mut |to, bytes| wire.0.push((*to, bytes.to_vec())));
    for p in wire.take() {
        let ops_bytes = client.receive(&tree, &p).expect("primed message");
        assert_eq!(ops_bytes.first().copied(), Some(ops.eof), "{ops_bytes:?}");
    }

    // The client's first player command for this map: it is in the game. The
    // snapshots that follow are read whole with the delta readers, the first
    // from nothing, then each from the one the client acknowledged.
    let mut seen: Vec<Seen> = Vec::new();
    for (step, time) in [120, 170, 220].into_iter().enumerate() {
        let moved = move_message(&mut client, &tree, op_bits, time);
        send(&mut server, &moved, &mut wire);
        server.frame(50, &mut |to, bytes| wire.0.push((*to, bytes.to_vec())));
        let packets = wire.take();
        assert!(!packets.is_empty(), "no message after command {step}");
        for p in packets {
            let ops_bytes = client.receive(&tree, &p).expect("snapshot message");
            let sequence = client.server_message_sequence;
            let snap = read_server_message(game, &tables, &ops_bytes, &seen, sequence)
                .expect("a snapshot");
            // The first snapshot is whole; later ones are changes from the last
            // one the client acknowledged.
            assert_eq!(snap.delta_num == 0, step == 0, "step {step}");
            let player = |name: &str| {
                let f = tables.player.field(name).expect(name);
                delta::word_at(&snap.player, f.offset)
            };
            assert_eq!(player("commandTime"), snap.server_time);
            assert_eq!(player("clientNum"), 0);
            assert_eq!(player("pm_type"), 4);
            assert_eq!(player("viewHeightTarget"), 60);
            assert_eq!(f32::from_bits(player("viewHeightCurrent") as u32), 60.0);
            assert_eq!(snap.clients.len(), 1, "one player");
            let name_at = tables.client.field("name[0]").unwrap().offset as usize;
            assert_eq!(&snap.clients[0][name_at..name_at + 7], b"Tester\0");
            if let Some(last) = seen.last() {
                assert!(snap.server_time > last.server_time);
            }
            seen.push(snap);
        }
    }
    let _ = (client.reliable_sequence, client.reliable_acknowledge);
}

/// A message with one player command (time `time`, nothing pressed), keyed
/// as the client keys it.
fn move_message(
    client: &mut Client,
    tree: &huffman::MessageTree,
    op_bits: u32,
    time: i32,
) -> Vec<u8> {
    let game = client.game;
    let ring = client.server_commands.len();
    let mut body = [0u8; 256];
    let mut m = Msg::new(game, &mut body);
    m.write_bits(0, op_bits); // clc_move
    m.write_byte(1);
    let key = client.checksum_feed
        ^ client.server_message_sequence
        ^ hash_key(
            &client.server_commands[client.server_command_sequence as usize % ring],
            32,
        );
    let size = delta::usercmd_size(game);
    let zero = vec![0u8; size];
    let mut cmd = vec![0u8; size];
    cmd[..4].copy_from_slice(&time.to_le_bytes());
    delta::write_delta_usercmd_key(&mut m, key, &zero, &cmd);
    m.write_bits(3, op_bits); // clc_EOF
    let n = m.cursize;
    client.message(tree, &body[..n])
}

/// A snapshot as the stand-in client has read it.
struct Seen {
    /// The server message it came in.
    sequence: i32,
    server_time: i32,
    delta_num: i32,
    player: Vec<u8>,
    clients: Vec<Vec<u8>>,
}

/// Reads a server message's ops (after the reliable acknowledge): server
/// commands, then a snapshot, read against the snapshot it is a change from.
fn read_server_message(
    game: Game,
    tables: &delta::FieldTables,
    ops_bytes: &[u8],
    seen: &[Seen],
    sequence: i32,
) -> Option<Seen> {
    let ops = server_ops(game);
    let mut buffer = ops_bytes.to_vec();
    let mut msg = Msg::new(game, &mut buffer);
    msg.cursize = ops_bytes.len();
    let mut snapshot = None;
    loop {
        let op = msg.read_byte();
        if op == i32::from(ops.server_command) {
            let _sequence = msg.read_long();
            let mut text = [0u8; iw_pc::net::msg::MAX_STRING_CHARS];
            msg.read_string(&mut text);
        } else if op == i32::from(ops.snapshot) {
            let server_time = msg.read_long();
            let delta_num = msg.read_byte();
            let old = (delta_num != 0).then(|| {
                seen.iter()
                    .find(|s| s.sequence == sequence - delta_num)
                    .expect("the snapshot this one changes")
            });
            let _flags = msg.read_byte();
            let mut player = vec![0u8; delta::player_state_size(game)];
            delta::read_delta_playerstate(
                &mut msg,
                tables,
                old.map(|o| o.player.as_slice()),
                &mut player,
            );
            // No entities yet: the list is only its end.
            assert_eq!(msg.read_bits(delta::ENTITY_NUMBER_BITS), 1023);
            let clients = read_clients(&mut msg, tables, old.map_or(&[][..], |o| &o.clients));
            snapshot = Some(Seen {
                sequence,
                server_time,
                delta_num,
                player,
                clients,
            });
        } else if op == i32::from(ops.eof) {
            break;
        } else {
            panic!("unexpected op {op} in a server message");
        }
    }
    assert!(!msg.overflowed, "the message read past its end");
    snapshot
}

/// The client list as `CL_ParsePacketClients` merges it: the clients the
/// message names are read (as changes from the old ones, or from nothing),
/// the others carried over.
fn read_clients(msg: &mut Msg, tables: &delta::FieldTables, old: &[Vec<u8>]) -> Vec<Vec<u8>> {
    let mut clients = Vec::new();
    let mut next_old = 0;
    while msg.read_bit() == 1 {
        let number = msg.read_bits(delta::CLIENT_NUMBER_BITS);
        while next_old < old.len() && delta::word_at(&old[next_old], 0) < number {
            clients.push(old[next_old].clone());
            next_old += 1;
        }
        let from =
            (next_old < old.len() && delta::word_at(&old[next_old], 0) == number).then(|| {
                next_old += 1;
                old[next_old - 1].as_slice()
            });
        let mut state = vec![0u8; 0x5C];
        if !delta::read_delta_client(msg, tables, from, &mut state, number) {
            clients.push(state);
        }
    }
    clients.extend(old[next_old..].iter().cloned());
    clients
}

#[test]
fn call_of_duty_2_session() {
    session(Game::CallOfDuty2);
}

#[test]
fn united_offensive_session() {
    session(Game::UnitedOffensive);
}
