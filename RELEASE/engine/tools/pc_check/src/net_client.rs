//! Requests for `iw_pc::client`: the client's parsers, with the client's
//! globals as the games keep them.
//!
//! The client's `cl` and the part of `clc` the parsers use arrive as
//! regions (`g:`), the game's bytes; they are read into a
//! [`ClientState`] with the layout below, and written back the same way
//! (the same table is in `tools/decomp/x86/specs/net_client.py`). Bytes the
//! port doesn't keep are left as they came (the checks leave them zero).

use iw_pc::client::{
    self, ClientState, OutPacket, SendSettings, ServerMessageParts, Snapshot, PACKET_BACKUP,
};
use iw_pc::net::delta;
use iw_pc::net::msg::Msg;
use iw_pc::Game;

use crate::net_delta::tables;
use crate::net_msg::message_tree;
use crate::net_netchan::{chan_from, chan_to, record_sent, with_msg};
use crate::{game_error, Answer, Args, Ret};

/// Where the client's state is in one game's memory.
struct Layout {
    cl: usize,
    snap: usize,
    old_frame: usize,
    new_snapshots: usize,
    gs_offsets: usize,
    gs_data: usize,
    gs_count: usize,
    pen: usize,
    pcn: usize,
    out_packets: usize,
    snapshots: usize,
    snap_size: usize,
    baselines: usize,
    parse_entities: usize,
    parse_clients: usize,
    clc: usize,
    client_num: usize,
    connect_count: usize,
    checksum_feed: usize,
    msg_seq: usize,
    cmd_seq: usize,
    commands: usize,
    demo_waiting: usize,
    outgoing: usize,
    // The sending side.
    server_id: usize,
    cmds: usize,
    cmd_number: usize,
    challenge: usize,
    reliable_seq: usize,
    reliable_ack: usize,
    reliable_commands: usize,
    last_sent: usize,
    demo_playing: usize,
    last_executed: usize,
    // CL_PacketEvent.
    last_packet_time: usize,
    demo_recording: usize,
    // CL_ConnectionlessPacket.
    server_address: usize,
    connect_time: usize,
    server_message: usize,
    only_visible: Option<usize>,
    // The client game's clock (CL_SetCGameTime).
    server_time: usize,
    old_server_time: usize,
    frame_server_time: usize,
    server_time_delta: usize,
    extrapolated: usize,
    first_demo_frame_skipped: usize,
    time_demo: Option<usize>,
    time_demo_base: usize,
    // The player's commands (CL_FinishMove).
    user_cmd_weapon: usize,
    user_cmd_offhand: Option<usize>,
    cgame_angles: usize,
    view_angles: usize,
}

const COD2: Layout = Layout {
    cl: 0x0096_B650,
    snap: 0x0096_B668,
    old_frame: 0x0096_DD50,
    new_snapshots: 0x0096_DD58,
    gs_offsets: 0x0096_DD5C,
    gs_data: 0x0096_FD5C,
    gs_count: 0x0098_FD5C,
    pen: 0x0098_FDA0,
    pcn: 0x0098_FDA4,
    out_packets: 0x009D_0C30,
    snapshots: 0x009D_0DB0,
    snap_size: 0x26D8,
    baselines: 0x00A1_E8B0,
    parse_entities: 0x00A5_A8B0,
    parse_clients: 0x00AD_28B0,
    clc: 0x0060_9FE8,
    client_num: 0x0060_9FE8,
    connect_count: 0x0060_A00C,
    checksum_feed: 0x0060_A114,
    msg_seq: 0x0062_A120,
    cmd_seq: 0x0062_A124,
    commands: 0x0062_A12C,
    demo_waiting: 0x0064_A178,
    outgoing: 0x0064_A198,
    server_id: 0x0098_FDF8,
    cmds: 0x009C_FE2C,
    cmd_number: 0x009D_0C2C,
    challenge: 0x0060_A110,
    reliable_seq: 0x0060_A118,
    reliable_ack: 0x0060_A11C,
    reliable_commands: 0x0060_A120,
    last_sent: 0x0060_9FEC,
    demo_playing: 0x0064_A170,
    last_executed: 0x0062_A128,
    last_packet_time: 0x0060_9FF0,
    demo_recording: 0x0064_A16C,
    server_address: 0x0060_9FF4,
    connect_time: 0x0060_A008,
    server_message: 0x0060_A010,
    only_visible: None,
    server_time: 0x0096_DD40,
    old_server_time: 0x0096_DD44,
    frame_server_time: 0x0096_DD48,
    server_time_delta: 0x0096_DD4C,
    extrapolated: 0x0096_DD54,
    first_demo_frame_skipped: 0x0064_A17C,
    time_demo: Some(0x0064_A174),
    time_demo_base: 0x0064_A194,
    user_cmd_weapon: 0x0098_FDC8,
    user_cmd_offhand: Some(0x0098_FDCC),
    cgame_angles: 0x0098_FDE0,
    view_angles: 0x0098_FDEC,
};

const UO: Layout = Layout {
    cl: 0x0495_81A0,
    snap: 0x0495_81A4,
    old_frame: 0x0495_C6E8,
    new_snapshots: 0x0495_C6F0,
    gs_offsets: 0x0495_C6F4,
    gs_data: 0x0495_E6F4,
    gs_count: 0x0496_36F4,
    pen: 0x0496_3738,
    pcn: 0x0496_373C,
    out_packets: 0x0496_43D0,
    snapshots: 0x0496_4550,
    snap_size: 0x4534,
    baselines: 0x049E_EBD0,
    parse_entities: 0x04A2_BBD0,
    parse_clients: 0x04AA_5BD0,
    clc: 0x04DC_8860,
    client_num: 0x04DC_8860,
    connect_count: 0x04DC_8884,
    checksum_feed: 0x04DC_898C,
    msg_seq: 0x04DD_899C,
    cmd_seq: 0x04DD_89A0,
    commands: 0x04DD_89A8,
    demo_waiting: 0x04DE_9610,
    outgoing: 0x04DE_967C,
    server_id: 0x0496_37A8,
    cmds: 0x0496_37CC,
    cmd_number: 0x0496_43CC,
    challenge: 0x04DC_8988,
    reliable_seq: 0x04DC_8994,
    reliable_ack: 0x04DC_8998,
    reliable_commands: 0x04DC_899C,
    last_sent: 0x04DC_8864,
    demo_playing: 0x04DE_960C,
    last_executed: 0x04DD_89A4,
    last_packet_time: 0x04DC_8868,
    demo_recording: 0x04DE_9608,
    server_address: 0x04DC_886C,
    connect_time: 0x04DC_8880,
    server_message: 0x04DC_8888,
    only_visible: Some(0x04DC_8990),
    server_time: 0x0495_C6D8,
    old_server_time: 0x0495_C6DC,
    frame_server_time: 0x0495_C6E0,
    server_time_delta: 0x0495_C6E4,
    extrapolated: 0x0495_C6EC,
    first_demo_frame_skipped: 0x04DE_9614,
    time_demo: None,
    time_demo_base: 0x04DE_962C,
    user_cmd_weapon: 0x0496_376C,
    user_cmd_offhand: None,
    cgame_angles: 0x0496_3790,
    view_angles: 0x0496_379C,
};

fn layout(game: Game) -> &'static Layout {
    match game {
        Game::CallOfDuty2 => &COD2,
        Game::UnitedOffensive => &UO,
    }
}

/// A block of the game's memory starting at `base`, read and written by address.
struct Block<'a> {
    bytes: &'a mut [u8],
    base: usize,
}

impl Block<'_> {
    fn holds(&self, address: usize, len: usize) -> bool {
        self.range(address, len).is_ok()
    }

    fn range(&self, address: usize, len: usize) -> Result<std::ops::Range<usize>, String> {
        let at = address
            .checked_sub(self.base)
            .filter(|at| at + len <= self.bytes.len())
            .ok_or_else(|| format!("0x{address:08X} is outside the region"))?;
        Ok(at..at + len)
    }

    fn get(&self, address: usize) -> Result<i32, String> {
        let r = self.range(address, 4)?;
        let b = &self.bytes[r];
        Ok(i32::from_le_bytes([b[0], b[1], b[2], b[3]]))
    }

    fn put(&mut self, address: usize, value: i32) -> Result<(), String> {
        let r = self.range(address, 4)?;
        self.bytes[r].copy_from_slice(&value.to_le_bytes());
        Ok(())
    }

    fn read(&self, address: usize, to: &mut [u8]) -> Result<(), String> {
        let r = self.range(address, to.len())?;
        to.copy_from_slice(&self.bytes[r]);
        Ok(())
    }

    fn write(&mut self, address: usize, from: &[u8]) -> Result<(), String> {
        let r = self.range(address, from.len())?;
        self.bytes[r].copy_from_slice(from);
        Ok(())
    }
}

/// A snapshot from its bytes (`clSnapshot_t`: seven numbers, the player
/// state, five numbers).
fn snapshot_from(game: Game, bytes: &[u8]) -> Result<Snapshot, String> {
    let mut s = Snapshot::new(game);
    let ps = s.player_state.len();
    if bytes.len() < 0x30 + ps {
        return Err("short snapshot".to_string());
    }
    let word =
        |at: usize| i32::from_le_bytes([bytes[at], bytes[at + 1], bytes[at + 2], bytes[at + 3]]);
    s.valid = word(0);
    s.snap_flags = word(4);
    s.server_time = word(8);
    s.message_num = word(12);
    s.delta_num = word(16);
    s.ping = word(20);
    s.cmd_num = word(24);
    s.player_state.copy_from_slice(&bytes[28..28 + ps]);
    let tail = 28 + ps;
    s.num_entities = word(tail);
    s.num_clients = word(tail + 4);
    s.parse_entities_num = word(tail + 8);
    s.parse_clients_num = word(tail + 12);
    s.server_command_num = word(tail + 16);
    Ok(s)
}

fn snapshot_into(s: &Snapshot, bytes: &mut [u8]) {
    let ps = s.player_state.len();
    let mut put = |at: usize, v: i32| bytes[at..at + 4].copy_from_slice(&v.to_le_bytes());
    for (k, v) in [
        s.valid,
        s.snap_flags,
        s.server_time,
        s.message_num,
        s.delta_num,
        s.ping,
        s.cmd_num,
    ]
    .into_iter()
    .enumerate()
    {
        put(4 * k, v);
    }
    let tail = 28 + ps;
    for (k, v) in [
        s.num_entities,
        s.num_clients,
        s.parse_entities_num,
        s.parse_clients_num,
        s.server_command_num,
    ]
    .into_iter()
    .enumerate()
    {
        put(tail + 4 * k, v);
    }
    bytes[28..28 + ps].copy_from_slice(&s.player_state);
}

/// Reads the client's `cl` from its region.
/// `cl.timeoutcount` (CL_CheckTimeout), outside the table above.
fn timeout_count_address(game: Game) -> usize {
    match game {
        Game::CallOfDuty2 => 0x0096_B664,
        Game::UnitedOffensive => 0x0495_81A0,
    }
}

fn read_cl(cl: &mut ClientState, bytes: &mut [u8]) -> Result<(), String> {
    let lay = layout(cl.game);
    let b = Block {
        bytes,
        base: lay.cl,
    };
    cl.timeout_count = b.get(timeout_count_address(cl.game))?;
    let snap_bytes = |b: &Block, address: usize| -> Result<Snapshot, String> {
        let r = b.range(address, lay.snap_size)?;
        snapshot_from(cl.game, &b.bytes[r])
    };
    cl.snap = snap_bytes(&b, lay.snap)?;
    cl.old_frame_server_time = b.get(lay.old_frame)?;
    cl.new_snapshots = b.get(lay.new_snapshots)?;
    cl.server_time = b.get(lay.server_time)?;
    cl.old_server_time = b.get(lay.old_server_time)?;
    cl.frame_server_time = b.get(lay.frame_server_time)?;
    cl.server_time_delta = b.get(lay.server_time_delta)?;
    cl.extrapolated_snapshot = b.get(lay.extrapolated)?;
    for (i, offset) in cl.game_state.offsets.iter_mut().enumerate() {
        *offset = b.get(lay.gs_offsets + 4 * i)?;
    }
    b.read(lay.gs_data, &mut cl.game_state.data)?;
    cl.game_state.data_count = b.get(lay.gs_count)?;
    cl.parse_entities_num = b.get(lay.pen)?;
    cl.parse_clients_num = b.get(lay.pcn)?;
    for (i, p) in cl.out_packets.iter_mut().enumerate() {
        let at = lay.out_packets + 12 * i;
        *p = OutPacket {
            cmd_number: b.get(at)?,
            server_time: b.get(at + 4)?,
            realtime: b.get(at + 8)?,
        };
    }
    for i in 0..PACKET_BACKUP {
        cl.snapshots[i] = snap_bytes(&b, lay.snapshots + i * lay.snap_size)?;
    }
    b.read(lay.baselines, &mut cl.baselines)?;
    b.read(lay.parse_entities, &mut cl.parse_entities)?;
    b.read(lay.parse_clients, &mut cl.parse_clients)?;
    cl.server_id = b.get(lay.server_id)?;
    b.read(lay.cmds, &mut cl.cmds)?;
    cl.cmd_number = b.get(lay.cmd_number)?;
    cl.user_cmd_weapon = b.get(lay.user_cmd_weapon)?;
    if let Some(at) = lay.user_cmd_offhand {
        cl.user_cmd_offhand = b.get(at)?;
    }
    for i in 0..3 {
        cl.cgame_angles[i] = f32::from_bits(b.get(lay.cgame_angles + 4 * i)? as u32);
        cl.view_angles[i] = f32::from_bits(b.get(lay.view_angles + 4 * i)? as u32);
    }
    Ok(())
}

fn write_cl(cl: &ClientState, bytes: &mut [u8]) -> Result<(), String> {
    let lay = layout(cl.game);
    let mut b = Block {
        bytes,
        base: lay.cl,
    };
    b.put(timeout_count_address(cl.game), cl.timeout_count)?;
    let r = b.range(lay.snap, lay.snap_size)?;
    snapshot_into(&cl.snap, &mut b.bytes[r]);
    b.put(lay.old_frame, cl.old_frame_server_time)?;
    b.put(lay.new_snapshots, cl.new_snapshots)?;
    b.put(lay.server_time, cl.server_time)?;
    b.put(lay.old_server_time, cl.old_server_time)?;
    b.put(lay.frame_server_time, cl.frame_server_time)?;
    b.put(lay.server_time_delta, cl.server_time_delta)?;
    b.put(lay.extrapolated, cl.extrapolated_snapshot)?;
    for (i, &offset) in cl.game_state.offsets.iter().enumerate() {
        b.put(lay.gs_offsets + 4 * i, offset)?;
    }
    b.write(lay.gs_data, &cl.game_state.data)?;
    b.put(lay.gs_count, cl.game_state.data_count)?;
    b.put(lay.pen, cl.parse_entities_num)?;
    b.put(lay.pcn, cl.parse_clients_num)?;
    for (i, p) in cl.out_packets.iter().enumerate() {
        let at = lay.out_packets + 12 * i;
        b.put(at, p.cmd_number)?;
        b.put(at + 4, p.server_time)?;
        b.put(at + 8, p.realtime)?;
    }
    for (i, s) in cl.snapshots.iter().enumerate() {
        let r = b.range(lay.snapshots + i * lay.snap_size, lay.snap_size)?;
        snapshot_into(s, &mut b.bytes[r]);
    }
    b.write(lay.baselines, &cl.baselines)?;
    b.write(lay.parse_entities, &cl.parse_entities)?;
    b.write(lay.parse_clients, &cl.parse_clients)?;
    b.put(lay.server_id, cl.server_id)?;
    b.write(lay.cmds, &cl.cmds)?;
    b.put(lay.cmd_number, cl.cmd_number)?;
    b.put(lay.user_cmd_weapon, cl.user_cmd_weapon)?;
    if let Some(at) = lay.user_cmd_offhand {
        b.put(at, cl.user_cmd_offhand)?;
    }
    for i in 0..3 {
        b.put(
            lay.cgame_angles + 4 * i,
            cl.cgame_angles[i].to_bits() as i32,
        )?;
        b.put(lay.view_angles + 4 * i, cl.view_angles[i].to_bits() as i32)?;
    }
    Ok(())
}

/// Reads the connection's part (`clc`) from its region.
fn read_clc(cl: &mut ClientState, bytes: &mut [u8]) -> Result<(), String> {
    let lay = layout(cl.game);
    let b = Block {
        bytes,
        base: lay.clc,
    };
    cl.client_num = b.get(lay.client_num)?;
    cl.connect_packet_count = b.get(lay.connect_count)?;
    cl.checksum_feed = b.get(lay.checksum_feed)?;
    cl.server_message_sequence = b.get(lay.msg_seq)?;
    cl.server_command_sequence = b.get(lay.cmd_seq)?;
    b.read(lay.commands, &mut cl.server_commands)?;
    cl.demo_waiting = b.get(lay.demo_waiting)?;
    // (The channel's first words: the parsers' region ends with the outgoing
    // sequence; the sender's stops before the channel, which comes whole.)
    if b.holds(lay.outgoing, 4) {
        cl.chan.outgoing_sequence = b.get(lay.outgoing)?;
    }
    cl.challenge = b.get(lay.challenge)?;
    cl.reliable_sequence = b.get(lay.reliable_seq)?;
    cl.reliable_acknowledge = b.get(lay.reliable_ack)?;
    b.read(lay.reliable_commands, &mut cl.reliable_commands)?;
    cl.last_packet_sent_time = b.get(lay.last_sent)?;
    cl.demo_playing = b.get(lay.demo_playing)?;
    cl.last_executed_server_command = b.get(lay.last_executed)?;
    cl.last_packet_time = b.get(lay.last_packet_time)?;
    cl.demo_recording = b.get(lay.demo_recording)?;
    let mut address = [0u8; 20];
    b.read(lay.server_address, &mut address)?;
    cl.server_address = crate::net_netchan::address_from(&canonical_address(cl.game, &address))?;
    cl.connect_time = b.get(lay.connect_time)?;
    b.read(lay.server_message, &mut cl.server_message)?;
    if let Some(at) = lay.only_visible {
        cl.only_visible_clients = b.get(at)?;
    }
    cl.first_demo_frame_skipped = b.get(lay.first_demo_frame_skipped)?;
    if let Some(at) = lay.time_demo {
        cl.time_demo = b.get(at)?;
    }
    cl.time_demo_base_time = b.get(lay.time_demo_base)?;
    Ok(())
}

fn write_clc(cl: &ClientState, bytes: &mut [u8]) -> Result<(), String> {
    let lay = layout(cl.game);
    let mut b = Block {
        bytes,
        base: lay.clc,
    };
    b.put(lay.client_num, cl.client_num)?;
    b.put(lay.connect_count, cl.connect_packet_count)?;
    b.put(lay.checksum_feed, cl.checksum_feed)?;
    b.put(lay.msg_seq, cl.server_message_sequence)?;
    b.put(lay.cmd_seq, cl.server_command_sequence)?;
    b.write(lay.commands, &cl.server_commands)?;
    b.put(lay.demo_waiting, cl.demo_waiting)?;
    if b.holds(lay.outgoing, 4) {
        b.put(lay.outgoing, cl.chan.outgoing_sequence)?;
    }
    b.put(lay.challenge, cl.challenge)?;
    b.put(lay.reliable_seq, cl.reliable_sequence)?;
    b.put(lay.reliable_ack, cl.reliable_acknowledge)?;
    b.write(lay.reliable_commands, &cl.reliable_commands)?;
    b.put(lay.last_sent, cl.last_packet_sent_time)?;
    b.put(lay.demo_playing, cl.demo_playing)?;
    b.put(lay.last_executed, cl.last_executed_server_command)?;
    b.put(lay.last_packet_time, cl.last_packet_time)?;
    b.put(lay.demo_recording, cl.demo_recording)?;
    let address = crate::net_netchan::address_bytes(&cl.server_address);
    b.write(lay.server_address, &guest_address(cl.game, &address))?;
    b.put(lay.connect_time, cl.connect_time)?;
    b.write(lay.server_message, &cl.server_message)?;
    if let Some(at) = lay.only_visible {
        b.put(at, cl.only_visible_clients)?;
    }
    b.put(lay.first_demo_frame_skipped, cl.first_demo_frame_skipped)?;
    if let Some(at) = lay.time_demo {
        b.put(at, cl.time_demo)?;
    }
    b.put(lay.time_demo_base, cl.time_demo_base_time)?;
    Ok(())
}

/// A `netadr_t` as the game keeps it, in the canonical layout (Call of Duty
/// 2's: type, ip, port, ipx; United Offensive keeps the port last).
pub(crate) fn canonical_address(game: Game, guest: &[u8; 20]) -> [u8; 20] {
    match game {
        Game::CallOfDuty2 => *guest,
        Game::UnitedOffensive => {
            let mut out = [0u8; 20];
            out[..8].copy_from_slice(&guest[..8]);
            out[8..10].copy_from_slice(&guest[18..20]);
            out[10..].copy_from_slice(&guest[8..18]);
            out
        }
    }
}

pub(crate) fn guest_address(game: Game, canonical: &[u8; 20]) -> [u8; 20] {
    match game {
        Game::CallOfDuty2 => *canonical,
        Game::UnitedOffensive => {
            let mut out = [0u8; 20];
            out[..8].copy_from_slice(&canonical[..8]);
            out[8..18].copy_from_slice(&canonical[10..20]);
            out[18..20].copy_from_slice(&canonical[8..10]);
            out
        }
    }
}

/// Which of the client's regions a request carries (argument indexes).
#[derive(Clone, Copy)]
pub(crate) struct Regions {
    pub(crate) cl: Option<usize>,
    pub(crate) clc: Option<usize>,
}

pub(crate) fn load(game: Game, args: &mut Args, regions: Regions) -> Result<ClientState, String> {
    let mut cl = ClientState::new(game);
    if let Some(i) = regions.cl {
        read_cl(&mut cl, args.region(i))?;
    }
    if let Some(i) = regions.clc {
        read_clc(&mut cl, args.region(i))?;
    }
    Ok(cl)
}

pub(crate) fn store(cl: &ClientState, args: &mut Args, regions: Regions) -> Result<(), String> {
    if let Some(i) = regions.cl {
        write_cl(cl, args.region(i))?;
    }
    if let Some(i) = regions.clc {
        write_clc(cl, args.region(i))?;
    }
    Ok(())
}

/// `cls.realtime`, a 4-byte big-endian buffer argument.
fn realtime(args: &mut Args, i: usize) -> Result<i32, String> {
    let b = args.buffer(i).ok_or("NULL realtime")?;
    let b: [u8; 4] = b
        .as_slice()
        .try_into()
        .map_err(|_| "realtime is not 4 bytes")?;
    Ok(i32::from_be_bytes(b))
}

fn label(stopped: Option<&str>) -> Ret {
    Ret::Str(Some(stopped.unwrap_or("return").as_bytes().to_vec()))
}

/// The originals' stop points after a game state and at a download: the
/// checks end the call there.
#[derive(Default)]
struct StopAt {
    stopped: Option<&'static str>,
}

impl ServerMessageParts for StopAt {
    fn game_state_parsed(&mut self, _cl: &mut ClientState) -> bool {
        self.stopped = Some("systeminfo");
        false
    }

    fn download(&mut self, _msg: &mut Msg, _cl: &mut ClientState) -> bool {
        self.stopped = Some("download");
        false
    }
}

/// The message header (5 big-endian words, as `with_msg` reads them).
const MSG_HEADER: usize = 20;

fn header_word(bytes: &[u8], k: usize) -> Result<usize, String> {
    let b = bytes.get(4 * k..4 * k + 4).ok_or("short msg token")?;
    usize::try_from(i32::from_be_bytes([b[0], b[1], b[2], b[3]]))
        .map_err(|_| "negative msg size".to_string())
}

/// CL_ParseServerMessage: the plain message in argument 0 (`v:`, header
/// words then data), coded here as the guest's is coded by the original's
/// `MSG_WriteBitsCompress`.
fn server_message(game: Game, args: &mut Args) -> Answer {
    let tree = message_tree(game)?;
    let tables = tables(game)?;
    let token = args.bytes(0).to_vec();
    let cursize = header_word(&token, 2)?;
    let readcount = header_word(&token, 3)?;
    let data = token.get(MSG_HEADER..).ok_or("short msg token")?;
    let plain = data
        .get(readcount..cursize)
        .ok_or("msg sizes past its data")?;
    let mut coded = vec![0u8; 4 * plain.len() + 64];
    let length = tree.write_bits_compress(plain, &mut coded);
    let mut buffer = data[..readcount].to_vec();
    buffer.extend_from_slice(&coded[..length]);
    let size = buffer.len();
    buffer.resize(size + 64, 0);
    let regions = Regions {
        cl: Some(1),
        clc: Some(2),
    };
    let mut cl = load(game, args, regions)?;
    cl.realtime = realtime(args, 3)?;
    let mut msg = Msg::new(game, &mut buffer);
    msg.maxsize = size + 16;
    msg.cursize = size;
    msg.readcount = readcount;
    let mut parts = StopAt::default();
    client::parse_server_message(&mut msg, tree, tables, &mut cl, &mut parts)
        .map_err(game_error)?;
    store(&cl, args, regions)?;
    Ok(label(parts.stopped))
}

/// CL_ParsePacketEntities and CL_ParsePacketClients: (msg, old snapshot
/// index or -1, new snapshot bytes, cl).
fn packet(game: Game, args: &mut Args, clients: bool) -> Answer {
    let tables = tables(game)?;
    let regions = Regions {
        cl: Some(3),
        clc: None,
    };
    let mut cl = load(game, args, regions)?;
    let old = usize::try_from(args.i32(1)).ok();
    if old.is_some_and(|i| i >= PACKET_BACKUP) {
        return Err("old snapshot index past 31".to_string());
    }
    let mut new = snapshot_from(game, args.buffer(2).ok_or("NULL new snapshot")?)?;
    with_msg(args, 0, game, |m| {
        if clients {
            client::parse_packet_clients(m, tables, &mut cl, old, &mut new)
        } else {
            client::parse_packet_entities(m, tables, &mut cl, old, &mut new)
        }
    })?
    .map_err(game_error)?;
    snapshot_into(&new, args.buffer(2).ok_or("NULL new snapshot")?);
    store(&cl, args, regions)?;
    Ok(Ret::Void)
}

/// CL_WritePacket: (cls.state, cl, clc before the channel, the channel,
/// realtime, cl_packetdup, cl_nodelta, the packets sent).
fn write_packet(game: Game, args: &mut Args) -> Answer {
    let tree = message_tree(game)?;
    let state = realtime(args, 0)?;
    let regions = Regions {
        cl: Some(1),
        clc: Some(2),
    };
    let mut cl = load(game, args, regions)?;
    cl.connection_state = state;
    let (chan, profile) = chan_from(game, args, 3)?;
    cl.chan = chan;
    cl.realtime = realtime(args, 4)?;
    let settings = SendSettings {
        packet_dup: args.i32(5),
        no_delta: args.i32(6) != 0,
    };
    let mut sent = Vec::new();
    client::write_packet(&mut cl, tree, &settings, &mut |to, bytes| {
        record_sent(&mut sent, to, bytes)
    })
    .map_err(game_error)?;
    store(&cl, args, regions)?;
    chan_to(&cl.chan, profile, args, 3)?;
    *args.buffer(7).ok_or("NULL sent packets")? = sent;
    Ok(Ret::Void)
}

/// CL_PacketEvent: (the packet's msg, from, the original's extra word,
/// cls.state, cl, clc before the channel, the channel, realtime) -> the stop
/// reached, else Call of Duty 2's result (i:) or United Offensive's "return".
fn packet_event(game: Game, args: &mut Args) -> Answer {
    let tree = message_tree(game)?;
    let tables = tables(game)?;
    let from = crate::net_netchan::address_from(args.bytes(1))?;
    let state = realtime(args, 3)?;
    let regions = Regions {
        cl: Some(4),
        clc: Some(5),
    };
    let mut cl = load(game, args, regions)?;
    cl.connection_state = state;
    let (chan, profile) = chan_from(game, args, 6)?;
    cl.chan = chan;
    cl.realtime = realtime(args, 7)?;
    let mut parts = StopAt::default();
    let outcome = with_msg(args, 0, game, |m| {
        client::packet_event(&mut cl, &from, m, tree, tables, &mut parts)
    })?
    .map_err(game_error)?;
    store(&cl, args, regions)?;
    chan_to(&cl.chan, profile, args, 6)?;
    Ok(match (outcome, parts.stopped) {
        (client::PacketOutcome::Connectionless, _) => label(Some("connectionless")),
        (_, Some(stop)) => label(Some(stop)),
        (client::PacketOutcome::Read { demo: true }, None) => label(Some("demo")),
        (outcome, None) if game == Game::CallOfDuty2 => Ret::Int(i64::from(outcome.cod2_result())),
        _ => label(None),
    })
}

/// CL_ConnectionlessPacket: (from, msg, the original's extra word,
/// cls.state, clc before the channel, the channel, realtime, the qport, the
/// packets sent) -> the stop reached (the handlers the port leaves to its
/// caller), else Call of Duty 2's result (i:) or "return".
fn connectionless(game: Game, args: &mut Args) -> Answer {
    use client::ClientPacket as P;
    let from = crate::net_netchan::address_from(args.bytes(0))?;
    let state = realtime(args, 3)?;
    let regions = Regions {
        cl: None,
        clc: Some(4),
    };
    let mut cl = load(game, args, regions)?;
    cl.connection_state = state;
    let (chan, profile) = chan_from(game, args, 5)?;
    cl.chan = chan;
    cl.realtime = realtime(args, 6)?;
    cl.qport = args.i32(7);
    let mut sent = Vec::new();
    let packet = with_msg(args, 1, game, |m| {
        client::connectionless_packet(&mut cl, &from, m, &mut |to, bytes| {
            record_sent(&mut sent, to, bytes)
        })
    })?;
    let cod2 = game == Game::CallOfDuty2;
    let stop = match &packet {
        P::Voice => Some("voice"),
        P::Info => Some("info"),
        P::Status => Some("status"),
        P::Disconnect => Some("disconnect"),
        P::Motd => Some("motd"),
        P::Update => Some("update"),
        P::Servers => Some("servers"),
        P::NeedCdKey => Some(if cod2 { "needcdkey" } else { "localize" }),
        P::NewMap(_) => Some("newmap"),
        // (United Offensive's print goes to a console window: the check stops there.)
        P::Print if !cod2 => Some("print"),
        P::ServerError(text) if cod2 => {
            return Err(game_error(iw_pc::GameError::drop(
                String::from_utf8_lossy(text).into_owned(),
            )))
        }
        P::ServerError(_) => Some("localize"),
        _ => None,
    };
    // (cls.state: the guest's is a buffer argument of its own.)
    *args.buffer(3).ok_or("NULL state")? = cl.connection_state.to_be_bytes().to_vec();
    store(&cl, args, regions)?;
    chan_to(&cl.chan, profile, args, 5)?;
    *args.buffer(8).ok_or("NULL sent packets")? = sent;
    Ok(match stop {
        Some(label_text) => label(Some(label_text)),
        None if cod2 => Ret::Int(i64::from(packet.cod2_result())),
        None => label(None),
    })
}

/// The 4-byte addresses of a LocalAddresses token.
fn local_addresses(bytes: &[u8]) -> Vec<[u8; 4]> {
    bytes
        .chunks_exact(4)
        .map(|c| [c[0], c[1], c[2], c[3]])
        .collect()
}

/// CL_CheckForResend: (cls.state, clc before the channel, realtime, the
/// qport, the userinfo the original built, Call of Duty 2's key hash,
/// net_lanauthorize, the local addresses, cvar_modifiedFlags, sent packets)
/// -> "authorize" when the original goes on to ask the key server (the
/// check stops there, before the packet), else "return".
fn check_for_resend(game: Game, args: &mut Args) -> Answer {
    use client::Resend;
    let state = realtime(args, 0)?;
    let regions = Regions {
        cl: None,
        clc: Some(1),
    };
    let mut cl = load(game, args, regions)?;
    cl.connection_state = state;
    cl.realtime = realtime(args, 2)?;
    cl.qport = args.i32(3);
    let userinfo = args.str(4).ok_or("NULL userinfo")?.to_vec();
    let key_hash = args.str(5).ok_or("NULL key hash")?.to_vec();
    let local = local_addresses(args.bytes(7));
    let settings = client::ResendSettings {
        userinfo: &userinfo,
        key_hash: &key_hash,
        lan_authorize: args.i32(6) != 0,
        local_addresses: &local,
    };
    let mut sent = Vec::new();
    let resend = client::check_for_resend(&mut cl, &settings, &mut |to, bytes| {
        record_sent(&mut sent, to, bytes)
    });
    let stop = match resend {
        Resend::Challenge { authorize: true } => {
            sent.clear();
            Some("authorize")
        }
        Resend::Connect => {
            // The caller marks the settings as sent: the userinfo bit of the changed flags.
            let flags = args.buffer(8).ok_or("NULL modified flags")?;
            let value = u32::from_be_bytes(
                flags
                    .as_slice()
                    .try_into()
                    .map_err(|_| "flags not 4 bytes")?,
            ) & !2;
            *flags = value.to_be_bytes().to_vec();
            None
        }
        _ => None,
    };
    *args.buffer(0).ok_or("NULL state")? = cl.connection_state.to_be_bytes().to_vec();
    store(&cl, args, regions)?;
    *args.buffer(9).ok_or("NULL sent packets")? = sent;
    Ok(label(stop))
}

/// A CallLog buffer being written: each call's label, a 0 byte, the number
/// of arguments, then each (b"i" and a big-endian word, or b"s", a
/// big-endian length and the bytes).
#[derive(Default)]
pub(crate) struct CallLog(pub(crate) Vec<u8>);

pub(crate) enum Logged<'a> {
    I(i32),
    S(&'a [u8]),
}

impl CallLog {
    pub(crate) fn call(&mut self, label: &str, values: &[Logged]) {
        self.0.extend_from_slice(label.as_bytes());
        self.0.push(0);
        self.0.push(values.len() as u8);
        for v in values {
            match v {
                Logged::I(n) => {
                    self.0.push(b'i');
                    self.0.extend_from_slice(&n.to_be_bytes());
                }
                Logged::S(text) => {
                    self.0.push(b's');
                    self.0.extend_from_slice(&(text.len() as u32).to_be_bytes());
                    self.0.extend_from_slice(text);
                }
            }
        }
    }
}

/// CL_SystemInfoChanged: (cl, clc before the channel, sv_running, the
/// settings the client has, the call log) -> nothing. What the port hands
/// the caller is turned into the calls the original makes (the console
/// variables and the file system are stand-ins in the check).
fn system_info_changed(game: Game, args: &mut Args) -> Answer {
    use client::SystemInfo;
    use Logged::{I, S};
    let regions = Regions {
        cl: Some(0),
        clc: Some(1),
    };
    let mut cl = load(game, args, regions)?;
    let running = args.i32(2) != 0;
    let existing: Vec<Vec<u8>> = args
        .bytes(3)
        .split(|&c| c == 0)
        .filter(|n| !n.is_empty())
        .map(|n| n.to_ascii_lowercase())
        .collect();
    let info = client::system_info_changed(&mut cl, running).map_err(game_error)?;
    let mut log = CallLog::default();
    if let Some(SystemInfo {
        cheats_off,
        loaded_archives,
        referenced_archives,
        settings,
    }) = &info
    {
        if *cheats_off {
            log.call("cheats", &[]);
        }
        log.call("loaded", &[S(&loaded_archives.0), S(&loaded_archives.1)]);
        log.call(
            "referenced",
            &[S(&referenced_archives.0), S(&referenced_archives.1)],
        );
        for (key, value) in settings {
            match game {
                Game::CallOfDuty2 => {
                    log.call("find", &[S(key)]);
                    if existing.contains(&key.to_ascii_lowercase()) {
                        log.call("set", &[S(value), I(0)]);
                    } else {
                        log.call("find", &[S(key)]);
                        log.call("register", &[S(key), I(7), I(0x4000), S(value), I(0), I(0)]);
                    }
                }
                Game::UnitedOffensive => log.call("set", &[S(key), S(value), I(1)]),
            }
        }
        // The client's own sv_pure, for cl_connectedToPureServer.
        log.call("find", &[S(b"sv_pure")]);
    }
    store(&cl, args, regions)?;
    *args.buffer(4).ok_or("NULL call log")? = log.0;
    Ok(Ret::Void)
}

/// The check's side of the client's clock: the activeAction setting's
/// calls go into the log; demo playback is where the checks stop.
struct ClockCheck<'a> {
    log: CallLog,
    action: &'a [u8],
    stopped: Option<&'static str>,
}

impl client::ClockParts for ClockCheck<'_> {
    fn became_active(&mut self, cl: &mut ClientState) {
        use Logged::{I, S};
        if self.action.is_empty() {
            return;
        }
        self.log.call("cbuf", &[S(self.action)]);
        self.log.call("cbuf", &[S(b"\n")]);
        match cl.game {
            Game::CallOfDuty2 => self.log.call("clear", &[S(b""), I(0)]),
            Game::UnitedOffensive => self.log.call("set", &[S(b"activeAction"), S(b""), I(1)]),
        }
    }
    fn read_demo_message(&mut self, _cl: &mut ClientState) -> bool {
        self.stopped = Some("readdemo");
        false
    }
    fn time_demo_frame(&mut self, _cl: &mut ClientState) -> bool {
        self.stopped = Some("timedemo");
        false
    }
}

/// CL_SetCGameTime: (cls.state, cl, clc before the channel, realtime, the
/// server's name, cl_paused, sv_paused, sv_running, cl_freezeDemo,
/// cl_timeNudge, timescale, timedemo, activeAction, the call log) -> the
/// stop reached (demo playback), else "return".
fn set_cgame_time(game: Game, args: &mut Args) -> Answer {
    let state = realtime(args, 0)?;
    let regions = Regions {
        cl: Some(1),
        clc: Some(2),
    };
    let mut cl = load(game, args, regions)?;
    cl.connection_state = state;
    // (The clock the client game runs on: United Offensive's second one.)
    let now = realtime(args, 3)?;
    match game {
        Game::CallOfDuty2 => cl.realtime = now,
        Game::UnitedOffensive => cl.sim_time = now,
    }
    let name = args.str(4).ok_or("NULL server name")?;
    cl.server_name.fill(0);
    cl.server_name[..name.len()].copy_from_slice(name);
    let settings = client::ClockSettings {
        client_paused: args.i32(5) != 0,
        server_paused: args.i32(6) != 0,
        server_running: args.i32(7) != 0,
        freeze_demo: args.i32(8) != 0,
        time_nudge: args.i32(9),
        timescale: args.f32(10),
        time_demo: args.i32(11) != 0,
    };
    let action = args.str(12).ok_or("NULL activeAction")?.to_vec();
    let mut parts = ClockCheck {
        log: CallLog::default(),
        action: &action,
        stopped: None,
    };
    client::set_cgame_time(&mut cl, &settings, &mut parts).map_err(game_error)?;
    let (log, stopped) = (parts.log, parts.stopped);
    *args.buffer(0).ok_or("NULL state")? = cl.connection_state.to_be_bytes().to_vec();
    store(&cl, args, regions)?;
    *args.buffer(13).ok_or("NULL call log")? = log.0;
    Ok(label(stopped))
}

/// CL_CreateNewCommands: (cls.state, cl, com_frameTime, the input timing's
/// old frame time and frame_msec (United Offensive's), the command the
/// caller's CL_CreateCmd makes, the call log) -> nothing.
fn create_new_commands(game: Game, args: &mut Args) -> Answer {
    let state = realtime(args, 0)?;
    let regions = Regions {
        cl: Some(1),
        clc: None,
    };
    let mut cl = load(game, args, regions)?;
    cl.connection_state = state;
    let frame_time = args.i32(2);
    let mut timing = client::InputTiming {
        old_frame_time: realtime(args, 3)?,
        frame_msec: realtime(args, 4)?,
    };
    let made = args.bytes(5).to_vec();
    let mut log = CallLog::default();
    client::create_new_commands(&mut cl, frame_time, &mut timing, &mut |_, _| {
        log.call("create", &[]);
        made.clone()
    });
    *args.buffer(0).ok_or("NULL state")? = cl.connection_state.to_be_bytes().to_vec();
    store(&cl, args, regions)?;
    *args.buffer(3).ok_or("NULL old frame time")? = timing.old_frame_time.to_be_bytes().to_vec();
    *args.buffer(4).ok_or("NULL frame msec")? = timing.frame_msec.to_be_bytes().to_vec();
    *args.buffer(6).ok_or("NULL call log")? = log.0;
    Ok(Ret::Void)
}

/// CL_ReadyToSendPacket: (cls.state, cl, clc before the channel, the
/// channel, realtime, downloading, the local addresses, cl_maxpackets, the
/// call log) -> whether to send.
fn ready_to_send_packet(game: Game, args: &mut Args) -> Answer {
    use Logged::{I, S};
    let state = realtime(args, 0)?;
    let regions = Regions {
        cl: Some(1),
        clc: Some(2),
    };
    let mut cl = load(game, args, regions)?;
    cl.connection_state = state;
    let (chan, _) = chan_from(game, args, 3)?;
    cl.chan = chan;
    cl.realtime = realtime(args, 4)?;
    let downloading = args.i32(5) & 0xFF != 0;
    let local = local_addresses(args.bytes(6));
    let mut max_packets = args.i32(7);
    let before = max_packets;
    let ready = client::ready_to_send_packet(&cl, downloading, &local, &mut max_packets);
    let mut log = CallLog::default();
    if max_packets != before {
        let text = max_packets.to_string();
        log.call("set", &[S(b"cl_maxpackets"), S(text.as_bytes()), I(1)]);
    }
    *args.buffer(8).ok_or("NULL call log")? = log.0;
    Ok(Ret::Int(i64::from(ready)))
}

/// CL_GetServerCommand: (number, cl, clc before the channel, the big config
/// string, the words, the call log) -> true/false, or "restart" where the
/// original reached the map restart's last call (a stop). The calls the
/// original makes on the way are logged as the port's results say:
/// CL_SystemInfoChanged after the system info changed, the console's notify
/// lines cleared on a map restart.
fn get_server_command(game: Game, args: &mut Args) -> Answer {
    let number = args.i32(0);
    let regions = Regions {
        cl: Some(1),
        clc: Some(2),
    };
    let mut cl = load(game, args, regions)?;
    let mut big = [0u8; client::BIG_CONFIG_STRING_SIZE];
    {
        let buffer = args.region(3);
        big.copy_from_slice(&buffer[..client::BIG_CONFIG_STRING_SIZE]);
    }
    let mut tokens = Box::<iw_pc::net::oob::Tokens>::default();
    let command =
        client::get_server_command(&mut cl, number, &mut tokens, &mut big).map_err(game_error)?;
    let mut log = CallLog::default();
    let result = match command {
        client::ServerCommand::Disconnected { .. } => {
            // Com_Error(ERR_SERVERDISCONNECT, ...): the game's error, level 2.
            return Err("Com_Error 2".to_string());
        }
        client::ServerCommand::MapRestart => {
            log.call("notify", &[]);
            Ret::Str(Some(b"restart".to_vec()))
        }
        client::ServerCommand::ConfigString(changed) => {
            if changed == Some(client::SYSTEM_INFO) {
                log.call("systeminfo", &[]);
            }
            Ret::Int(1)
        }
        client::ServerCommand::Endless => {
            return Err("the original never returns (the checks don't give this)".to_string());
        }
        client::ServerCommand::ClientGame => Ret::Int(1),
        client::ServerCommand::Kept => Ret::Int(0),
    };
    store(&cl, args, regions)?;
    args.region(3)[..client::BIG_CONFIG_STRING_SIZE].copy_from_slice(&big);
    let mut words = (tokens.argc() as u32).to_be_bytes().to_vec();
    for i in 0..tokens.argc() {
        let word = tokens.argv(i);
        words.extend_from_slice(&(word.len() as u32).to_be_bytes());
        words.extend_from_slice(word);
    }
    *args.buffer(4).ok_or("NULL words")? = words;
    *args.buffer(5).ok_or("NULL call log")? = log.0;
    Ok(result)
}

pub fn dispatch(game: Game, function: &str, args: &mut Args) -> Option<Answer> {
    Some(match function {
        // netadr, local addresses -> bool
        "Sys_IsLANAddress" => (|| {
            let address = crate::net_netchan::address_from(args.bytes(0))?;
            let local = local_addresses(args.bytes(1));
            Ok(Ret::Int(i64::from(iw_pc::net::oob::is_lan_address(
                &address, &local,
            ))))
        })(),
        // command, clc -> the copy's warning
        "CL_AddReliableCommand" => (|| {
            let regions = Regions {
                cl: None,
                clc: Some(1),
            };
            let mut cl = load(game, args, regions)?;
            let command = args.text(0).to_vec();
            let queued = client::add_reliable_command(&mut cl, &command).map_err(game_error)?;
            store(&cl, args, regions)?;
            Ok(Ret::Printed(
                queued.err().map(|w| w.message_format().as_bytes().to_vec()),
            ))
        })(),
        // search path, checksum feed, fake checksum, clc -> nothing
        "CL_SendPureChecksums" => (|| {
            let regions = Regions {
                cl: None,
                clc: Some(3),
            };
            let mut cl = load(game, args, regions)?;
            let paths = crate::pc::search_paths(args.bytes(0));
            client::send_pure_checksums(&mut cl, &paths, args.i32(1), args.i32(2))
                .map_err(game_error)?;
            store(&cl, args, regions)?;
            Ok(Ret::Void)
        })(),
        "CL_GetServerCommand" => get_server_command(game, args),
        // argv(1), argv(2), argc, cl, the call log -> nothing
        "CL_ConfigstringModified" => (|| {
            let regions = Regions {
                cl: Some(3),
                clc: None,
            };
            let mut cl = load(game, args, regions)?;
            // The original reads "" past the last word (an unsigned argc).
            let argc = args.i32(2) as u32;
            let index = if argc > 1 {
                args.text(0).to_vec()
            } else {
                Vec::new()
            };
            let text = if argc > 2 {
                args.text(1).to_vec()
            } else {
                Vec::new()
            };
            let changed =
                client::configstring_modified(&mut cl, &index, &text).map_err(game_error)?;
            store(&cl, args, regions)?;
            let mut log = CallLog::default();
            if changed == Some(client::SYSTEM_INFO) {
                log.call("systeminfo", &[]);
            }
            *args.buffer(4).ok_or("NULL call log")? = log.0;
            Ok(Ret::Void)
        })(),
        // cls.state, cl, clc, realtime, cl_paused, sv_paused, cl_timeout, cl_connectTimeout
        "CL_CheckTimeout" => (|| {
            let state = realtime(args, 0)?;
            let regions = Regions {
                cl: Some(1),
                clc: Some(2),
            };
            let mut cl = load(game, args, regions)?;
            cl.connection_state = state;
            cl.realtime = realtime(args, 3)?;
            let settings = client::TimeoutSettings {
                client_paused: args.i32(4) != 0,
                server_paused: args.i32(5) != 0,
                timeout: args.f32(6),
                connect_timeout: if game == Game::CallOfDuty2 {
                    args.f32(7)
                } else {
                    0.0
                },
            };
            client::check_timeout(&mut cl, &settings).map_err(game_error)?;
            store(&cl, args, regions)?;
            Ok(Ret::Void)
        })(),
        // cls.state, cl_paused, the modified flags, the settings text, clc, the call log
        "CL_CheckUserinfo" => (|| {
            let state = realtime(args, 0)?;
            let regions = Regions {
                cl: None,
                clc: Some(4),
            };
            let mut cl = load(game, args, regions)?;
            cl.connection_state = state;
            let mut flags = realtime(args, 2)?;
            let info = args.text(3).to_vec();
            let sent = client::check_userinfo(&mut cl, args.i32(1) != 0, &mut flags, &info);
            let mut log = CallLog::default();
            let printed = match sent {
                None => None,
                Some(result) => {
                    log.call("infostring", &[Logged::I(2)]);
                    result.map_err(game_error)?.err()
                }
            };
            store(&cl, args, regions)?;
            *args.buffer(2).ok_or("NULL flags")? = flags.to_be_bytes().to_vec();
            *args.buffer(5).ok_or("NULL call log")? = log.0;
            Ok(Ret::Printed(
                printed.map(|w| w.message_format().as_bytes().to_vec()),
            ))
        })(),
        "CL_CheckForResend" => check_for_resend(game, args),
        "CL_SystemInfoChanged" => system_info_changed(game, args),
        "CL_SetCGameTime" => set_cgame_time(game, args),
        // cl, clc, the client game's clock, timescale -> nothing
        "CL_AdjustTimeDelta" => (|| {
            let regions = Regions {
                cl: Some(0),
                clc: Some(1),
            };
            let mut cl = load(game, args, regions)?;
            let now = realtime(args, 2)?;
            cl.realtime = now;
            cl.sim_time = now;
            let settings = client::ClockSettings {
                timescale: args.f32(3),
                ..Default::default()
            };
            client::adjust_time_delta(&mut cl, &settings);
            store(&cl, args, regions)?;
            Ok(Ret::Void)
        })(),
        // cls.state, cl, clc, the client game's clock, activeAction, the call log -> nothing
        "CL_FirstSnapshot" => (|| {
            let state = realtime(args, 0)?;
            let regions = Regions {
                cl: Some(1),
                clc: Some(2),
            };
            let mut cl = load(game, args, regions)?;
            cl.connection_state = state;
            let now = realtime(args, 3)?;
            cl.realtime = now;
            cl.sim_time = now;
            let action = args.str(4).ok_or("NULL activeAction")?.to_vec();
            let mut parts = ClockCheck {
                log: CallLog::default(),
                action: &action,
                stopped: None,
            };
            if client::first_snapshot(&mut cl) {
                client::ClockParts::became_active(&mut parts, &mut cl);
            }
            *args.buffer(0).ok_or("NULL state")? = cl.connection_state.to_be_bytes().to_vec();
            store(&cl, args, regions)?;
            *args.buffer(5).ok_or("NULL call log")? = parts.log.0;
            Ok(Ret::Void)
        })(),
        "CL_CreateNewCommands" => create_new_commands(game, args),
        "CL_ReadyToSendPacket" => ready_to_send_packet(game, args),
        // cmd, cl -> nothing
        "CL_FinishMove" => (|| {
            let regions = Regions {
                cl: Some(1),
                clc: None,
            };
            let cl = load(game, args, regions)?;
            let cmd = args.buffer(0).ok_or("NULL cmd")?;
            client::finish_move(&cl, cmd);
            Ok(Ret::Void)
        })(),
        // msg, clc
        "CL_ParseCommandString" => (|| {
            let regions = Regions {
                cl: None,
                clc: Some(1),
            };
            let mut cl = load(game, args, regions)?;
            with_msg(args, 0, game, |m| client::parse_command_string(m, &mut cl))?;
            store(&cl, args, regions)?;
            Ok(Ret::Void)
        })(),
        // msg, cl, clc -> "systeminfo" (where the original goes on to apply it)
        "CL_ParseGamestate" => (|| {
            let tables = tables(game)?;
            let regions = Regions {
                cl: Some(1),
                clc: Some(2),
            };
            let mut cl = load(game, args, regions)?;
            with_msg(args, 0, game, |m| {
                client::parse_game_state(m, tables, &mut cl)
            })?
            .map_err(game_error)?;
            store(&cl, args, regions)?;
            Ok(label(Some("systeminfo")))
        })(),
        "CL_ParsePacketEntities" => packet(game, args, false),
        "CL_ParsePacketClients" => packet(game, args, true),
        // msg, cl, clc, realtime
        "CL_ParseSnapshot" => (|| {
            let tables = tables(game)?;
            let regions = Regions {
                cl: Some(1),
                clc: Some(2),
            };
            let mut cl = load(game, args, regions)?;
            cl.realtime = realtime(args, 3)?;
            with_msg(args, 0, game, |m| {
                client::parse_snapshot(m, tables, &mut cl)
            })?
            .map_err(game_error)?;
            store(&cl, args, regions)?;
            Ok(Ret::Void)
        })(),
        // coded msg, cl, clc, realtime -> "systeminfo", "download" or "return"
        "CL_ParseServerMessage" => server_message(game, args),
        // ps, cmd
        "MSG_SetDefaultUserCmd" => (|| {
            let ps = args.bytes(0).to_vec();
            if ps.len() < delta::player_state_size(game) {
                return Err("short player state".to_string());
            }
            let cmd = args.buffer(1).ok_or("NULL cmd")?;
            if cmd.len() < delta::usercmd_size(game) {
                return Err("short cmd".to_string());
            }
            delta::set_default_usercmd(game, &ps, cmd);
            Ok(Ret::Void)
        })(),
        "CL_WritePacket" => write_packet(game, args),
        "CL_PacketEvent" => packet_event(game, args),
        "CL_ConnectionlessPacket" => connectionless(game, args),
        _ => return None,
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn a_snapshot_goes_back_to_the_same_bytes() {
        for game in [Game::CallOfDuty2, Game::UnitedOffensive] {
            let size = layout(game).snap_size;
            let bytes: Vec<u8> = (0..size).map(|i| (i * 7 + 3) as u8).collect();
            let s = snapshot_from(game, &bytes).unwrap();
            let mut back = vec![0u8; size];
            snapshot_into(&s, &mut back);
            assert_eq!(back, bytes);
            assert_eq!(0x30 + delta::player_state_size(game), size);
        }
    }
}
