//! Requests for `iw_pc::net::{oob, netchan}` (helper "pc-oob").
//!
//! The specs are `tools/decomp/x86/specs/net_oob.py` and `net_netchan.py`;
//! the encodings of the structured arguments are described there.

use iw_pc::net::netchan::{AddressType, NetAddress};
use iw_pc::net::oob::{
    self, ChallengeAnswer, ChallengeRequest, ChallengeTable, ClientSlotInfo, ConnectDecision,
    ConnectRequest, Line, PlayerStatus, ServerInfo, ServerListEntry, ServerRequest, ServerStatus,
    Setting, Tokens,
};
use iw_pc::Game;

use crate::net_netchan::{address_bytes, address_from, record_sent};
use crate::{Answer, Args, Ret};

/// Writes `value` big-endian at entry `index` of a buffer of 32-bit words
/// (the checker's encoding of the guest's little-endian words).
fn put_word(buffer: &mut [u8], index: usize, value: u32) {
    buffer[4 * index..4 * index + 4].copy_from_slice(&value.to_be_bytes());
}

/// `Cmd_TokenizeStringInternal`: (text, limit, text buffer, argv[512]) and,
/// for United Offensive, its `argc` global. The argv entries come back as
/// offsets into the text buffer.
fn tokenize(game: Game, args: &mut Args) -> Option<Answer> {
    let text = args.str(0).map(<[u8]>::to_vec);
    let limit = args.int(1) as u32;
    let mut tokens = Box::<Tokens>::default();
    match &text {
        Some(text) => tokens.tokenize(game, text, limit),
        None => tokens.clear(),
    }
    let raw = tokens.raw_text().to_vec();
    let buffer = args.buffer(2)?;
    buffer[..raw.len()].copy_from_slice(&raw);
    let argv = args.buffer(3)?;
    for (i, &start) in tokens.raw_starts().iter().enumerate() {
        put_word(argv, i, u32::from(start));
    }
    match game {
        Game::CallOfDuty2 => Some(Ok(Ret::Int(tokens.argc() as i64))),
        Game::UnitedOffensive => {
            put_word(args.buffer(4)?, 0, tokens.argc() as u32);
            Some(Ok(Ret::Void))
        }
    }
}

/// The packet buffer the framing writes into (the largest the games send).
const PACKET_BUFFER: usize = 0x20000;

/// `NET_OutOfBandPrint`: Call of Duty 2 (sock, address, text, sent);
/// United Offensive (sock, address, format, argument, formatted text, sent).
/// Call of Duty 2 returns whether it sent the packet.
fn out_of_band_print(game: Game, args: &mut Args) -> Answer {
    let (text_arg, sent_arg) = match game {
        Game::CallOfDuty2 => (2, 3),
        Game::UnitedOffensive => (4, 5),
    };
    let to = address_from(args.bytes(1))?;
    let text = args.text(text_arg).to_vec();
    let mut packet = vec![0u8; PACKET_BUFFER];
    let length = oob::out_of_band_print(game, &text, &mut packet);
    if let Some(length) = length {
        record_sent(
            args.buffer(sent_arg).ok_or("NULL sent")?,
            &to,
            &packet[..length],
        );
    }
    Ok(match game {
        Game::CallOfDuty2 => Ret::Int(length.is_some().into()),
        Game::UnitedOffensive => Ret::Void,
    })
}

/// `NET_OutOfBandData`: (sock, address, data, length, sent). United
/// Offensive's packet is compared before its compression (the spec
/// decompresses what the original sent).
fn out_of_band_data(game: Game, args: &mut Args) -> Answer {
    let to = address_from(args.bytes(1))?;
    let length = args.int(3) as usize;
    let data = args
        .bytes(2)
        .get(..length)
        .ok_or("length past the data")?
        .to_vec();
    let mut packet = vec![0u8; PACKET_BUFFER];
    let length = oob::out_of_band_data(game, &data, &mut packet).ok_or("too long")?;
    record_sent(args.buffer(4).ok_or("NULL sent")?, &to, &packet[..length]);
    Ok(match game {
        Game::CallOfDuty2 => Ret::Int(1),
        Game::UnitedOffensive => Ret::Void,
    })
}

/// `SV_ConnectionlessPacket`: (address, packet) -> the handler it runs, by
/// the spec's end-point labels.
fn connectionless_packet(game: Game, args: &mut Args) -> Answer {
    let packet = args.bytes(1).to_vec();
    let mut line = Box::<Line>::default();
    let mut tokens = Box::<Tokens>::default();
    let label = match oob::server_request(game, &packet, &mut line, &mut tokens) {
        ServerRequest::PunkBuster => "pb",
        ServerRequest::Status => "status",
        ServerRequest::Voice => "voice",
        ServerRequest::Info => "info",
        ServerRequest::Challenge => "challenge",
        ServerRequest::Connect => "connect",
        ServerRequest::IpAuthorize => "ipAuthorize",
        ServerRequest::RemoteCommand => "rcon",
        ServerRequest::Ignored => "return",
        // United Offensive reaches Com_DPrintf ("bad connectionless packet");
        // Call of Duty 2 just returns.
        ServerRequest::Unknown if game == Game::UnitedOffensive => "unknown",
        ServerRequest::Unknown => "return",
    };
    Ok(Ret::Str(Some(label.as_bytes().to_vec())))
}

/// The spec's server description: (name, value) pairs, each a 1-byte name
/// length, the name, a 4-byte big-endian value length, the value.
pub struct Description<'a>(Vec<(&'a [u8], &'a [u8])>);

impl<'a> Description<'a> {
    pub fn parse(mut bytes: &'a [u8]) -> Result<Description<'a>, String> {
        let mut items = Vec::new();
        while !bytes.is_empty() {
            let n = usize::from(bytes[0]);
            let name = bytes.get(1..1 + n).ok_or("bad description")?;
            let at = 1 + n;
            let len = bytes.get(at..at + 4).ok_or("bad description")?;
            let len = u32::from_be_bytes([len[0], len[1], len[2], len[3]]) as usize;
            let value = bytes.get(at + 4..at + 4 + len).ok_or("bad description")?;
            items.push((name, value));
            bytes = &bytes[at + 4 + len..];
        }
        Ok(Description(items))
    }

    pub fn get(&self, name: &str) -> &'a [u8] {
        self.0
            .iter()
            .find(|(n, _)| *n == name.as_bytes())
            .map_or(&[][..], |(_, v)| v)
    }

    pub fn int(&self, name: &str) -> i32 {
        std::str::from_utf8(self.get(name))
            .ok()
            .and_then(|t| t.parse().ok())
            .unwrap_or(0)
    }

    fn setting(&self, name: &str) -> Setting<'a> {
        let value = self.get(name);
        match value {
            [b'n', b':', number @ ..] => Setting::Number(
                std::str::from_utf8(number)
                    .ok()
                    .and_then(|t| t.parse().ok())
                    .unwrap_or(0),
            ),
            [b't', b':', text @ ..] => Setting::Text(text),
            _ => Setting::Missing,
        }
    }

    /// Whether the server runs mods, answering the archive checks with the
    /// spec's answers (asked of the game's own function).
    fn modded(&self, game: Game, pure: bool, tokens: &mut Tokens) -> bool {
        let official = self.get("official");
        let mut answers = official.split(|&c| c == b',');
        oob::runs_mods(
            game,
            pure,
            self.get("fs_game"),
            self.get("referenced"),
            tokens,
            |_| answers.next() == Some(b"1"),
        )
    }
}

/// The effective `sv_pure` for the mod test: Call of Duty 2's dvar is a
/// true/false byte, United Offensive's a number.
fn pure_flag(game: Game, pure: i32) -> bool {
    match game {
        Game::CallOfDuty2 => pure as u8 != 0,
        Game::UnitedOffensive => pure != 0,
    }
}

/// `SVC_Info` / `SVC_Status`: (address, server description, sent).
fn respond(game: Game, args: &mut Args, status: bool) -> Answer {
    let to = address_from(args.bytes(0))?;
    let bytes = args.bytes(1).to_vec();
    let d = Description::parse(&bytes)?;
    let mut tokens = Box::<Tokens>::default();
    let pure = d.int("pure");
    let modded = d.modded(game, pure_flag(game, pure), &mut tokens);
    let mut text = vec![0u8; 0x8000];
    let length = if status {
        let lines = d.get("players");
        let mut names = Vec::new();
        let mut players = Vec::new();
        for line in lines.split(|&c| c == b'\n').filter(|l| !l.is_empty()) {
            let mut parts = line.split(|&c| c == b' ');
            let mut number = || -> i32 {
                parts
                    .next()
                    .and_then(|t| std::str::from_utf8(t).ok())
                    .and_then(|t| t.parse().ok())
                    .unwrap_or(0)
            };
            let (score, ping) = (number(), number());
            let hex = parts.next().unwrap_or(b"");
            let name: Vec<u8> = (0..hex.len() / 2)
                .map(|i| {
                    u8::from_str_radix(
                        std::str::from_utf8(&hex[2 * i..2 * i + 2]).unwrap_or("00"),
                        16,
                    )
                    .unwrap_or(0)
                })
                .collect();
            names.push(name);
            players.push((score, ping));
        }
        let players: Vec<PlayerStatus> = players
            .iter()
            .zip(&names)
            .map(|(&(score, ping), name)| PlayerStatus { score, ping, name })
            .collect();
        let server = ServerStatus {
            server_info: d.get("server_info"),
            challenge: d.get("challenge"),
            restricted: d.int("restricted") != 0,
            players: &players,
            disable_client_console: d.int("disable_console"),
            password: d.get("password"),
            modded,
        };
        oob::status_response(game, &server, &mut text)
    } else {
        let states: Vec<i32> = d
            .get("states")
            .split(|&c| c == b',')
            .filter(|t| !t.is_empty())
            .map(|t| {
                std::str::from_utf8(t)
                    .ok()
                    .and_then(|t| t.parse().ok())
                    .unwrap_or(0)
            })
            .collect();
        let server = ServerInfo {
            challenge: d.get("challenge"),
            hostname: d.get("hostname"),
            mapname: d.get("mapname"),
            gametype: d.get("gametype"),
            max_clients: d.int("max_clients"),
            private_clients: d.int("private_clients"),
            client_states: &states,
            pure,
            server_iwd_count: d.int("server_iwds"),
            min_ping: d.int("min_ping"),
            max_ping: d.int("max_ping"),
            fs_game: d.get("fs_game"),
            allow_anonymous: d.int("allow_anonymous"),
            disable_client_console: d.int("disable_console"),
            password: d.get("password"),
            friendly_fire: d.setting("ff"),
            killcam: d.setting("kc"),
            timeouts_allowed: d.setting("timeouts"),
            allow_jeeps: d.setting("jeeps"),
            allow_tanks: d.setting("tanks"),
            dedicated: d.int("dedicated") != 0,
            modded,
            voice: d.int("voice"),
            punkbuster: d.int("pb"),
        };
        oob::info_response(game, &server, &mut text)
    };
    let mut packet = vec![0u8; PACKET_BUFFER];
    if let Some(n) = oob::out_of_band_print(game, &text[..length], &mut packet) {
        record_sent(args.buffer(2).ok_or("NULL sent")?, &to, &packet[..n]);
    }
    Ok(Ret::Void)
}

/// Records a packet the original's `NET_SendPacket` would hand to `sendto`
/// (IPv4 and broadcast addresses; loopback goes to a queue, bots and bad
/// addresses nowhere, IPX has no socket in the checker).
fn record_if_sent(out: &mut Vec<u8>, to: &NetAddress, packet: &[u8]) {
    if matches!(to.kind, AddressType::Ip | AddressType::Broadcast) {
        let mut to = *to;
        if to.kind == AddressType::Broadcast {
            to.ip = [255; 4];
        }
        record_sent(out, &to, packet);
    }
}

/// `challenge_t`'s size and where Call of Duty 2 keeps the PunkBuster hash.
fn challenge_stride(game: Game) -> usize {
    match game {
        Game::CallOfDuty2 => 0x74,
        Game::UnitedOffensive => 0x54,
    }
}
const GUID_AT: usize = 0x51;

fn read_i32(bytes: &[u8], at: usize) -> i32 {
    i32::from_le_bytes([bytes[at], bytes[at + 1], bytes[at + 2], bytes[at + 3]])
}

/// The challenge table in argument 1 (the spec's canonical slots).
fn read_table(game: Game, args: &mut Args) -> Result<ChallengeTable, String> {
    let stride = challenge_stride(game);
    let raw = args.buffer(1).ok_or("NULL table")?;
    if raw.len() != oob::MAX_CHALLENGES * stride {
        return Err("table size".to_string());
    }
    let mut table = ChallengeTable::default();
    for (slot, bytes) in table.slots.iter_mut().zip(raw.chunks(stride)) {
        slot.address = address_from(bytes)?;
        slot.challenge = read_i32(bytes, 0x14);
        slot.time = read_i32(bytes, 0x18);
        slot.ping_time = read_i32(bytes, 0x1C);
        slot.first_time = read_i32(bytes, 0x20);
        slot.first_ping = read_i32(bytes, 0x24);
        slot.connected = read_i32(bytes, 0x28) != 0;
        if game == Game::CallOfDuty2 {
            slot.punkbuster_guid
                .copy_from_slice(&bytes[GUID_AT..GUID_AT + 33]);
        }
    }
    Ok(table)
}

/// Writes the table's fields back into argument 1.
fn write_table(game: Game, table: &ChallengeTable, args: &mut Args) -> Result<(), String> {
    let stride = challenge_stride(game);
    let raw = args.buffer(1).ok_or("NULL table")?;
    for (slot, bytes) in table.slots.iter().zip(raw.chunks_mut(stride)) {
        bytes[..20].copy_from_slice(&address_bytes(&slot.address));
        let fields = [
            slot.challenge,
            slot.time,
            slot.ping_time,
            slot.first_time,
            slot.first_ping,
            i32::from(slot.connected),
        ];
        for (k, value) in fields.iter().enumerate() {
            bytes[0x14 + 4 * k..0x18 + 4 * k].copy_from_slice(&value.to_le_bytes());
        }
        if game == Game::CallOfDuty2 {
            bytes[GUID_AT..GUID_AT + 33].copy_from_slice(&slot.punkbuster_guid);
        }
    }
    Ok(())
}

/// `SV_GetChallenge`: (from, table, world, sent) -> "authorize" or "return".
fn get_challenge(game: Game, args: &mut Args) -> Answer {
    let from = address_from(args.bytes(0))?;
    let world = args.bytes(2).to_vec();
    let d = Description::parse(&world)?;
    let mut table = read_table(game, args)?;
    let guid = (d.int("guid_given") != 0).then(|| d.get("guid"));
    let request = ChallengeRequest {
        from,
        time: d.int("time"),
        lan_authorize: d.int("lan_authorize") != 0,
        from_lan: d.int("from_lan") != 0,
        authorize_known: true,
        last_master_contact: d.int("last_master"),
        from_master: d.int("from_master") != 0,
        punkbuster_guid: guid,
    };
    let number = d.int("challenge");
    let answer = oob::get_challenge(game, &mut table, &request, || number);
    write_table(game, &table, args)?;
    let label = match answer {
        ChallengeAnswer::Respond { slot } => {
            let to = table.slots[slot].address;
            let mut text = [0u8; 64];
            let n = oob::challenge_response_text(
                game,
                table.slots[slot].challenge,
                d.int("only_visible"),
                &mut text,
            );
            let mut packet = [0u8; 80];
            let length = oob::out_of_band_print(game, &text[..n], &mut packet).ok_or("too long")?;
            record_if_sent(args.buffer(3).ok_or("NULL sent")?, &to, &packet[..length]);
            "return"
        }
        ChallengeAnswer::AuthorizeUnknown => "return",
        ChallengeAnswer::Authorize { .. } => "authorize",
    };
    Ok(Ret::Str(Some(label.as_bytes().to_vec())))
}

/// `SV_DirectConnect`: (from, table, world, sent) -> "new <slot>",
/// "reconnect <slot>" or "return" (after a refusal, whose reply is sent).
fn direct_connect(game: Game, args: &mut Args) -> Answer {
    let from = address_from(args.bytes(0))?;
    let world = args.bytes(2).to_vec();
    let d = Description::parse(&world)?;
    let mut table = read_table(game, args)?;
    let clients: Vec<ClientSlotInfo> = d
        .get("clients")
        .chunks(32)
        .map(|c| {
            Ok(ClientSlotInfo {
                state: i32::from_be_bytes([c[0], c[1], c[2], c[3]]),
                qport: i32::from_be_bytes([c[4], c[5], c[6], c[7]]),
                last_connect_time: i32::from_be_bytes([c[8], c[9], c[10], c[11]]),
                address: address_from(&c[12..32])?,
            })
        })
        .collect::<Result<_, String>>()?;
    let float = |name: &str| -> f64 {
        std::str::from_utf8(d.get(name))
            .ok()
            .and_then(|t| t.parse().ok())
            .unwrap_or(0.0)
    };
    let request = ConnectRequest {
        from,
        userinfo: d.get("userinfo"),
        time: d.int("time"),
        reconnect_limit: d.int("reconnect_limit"),
        max_clients: d.int("max_clients"),
        private_clients: d.int("private_clients"),
        private_password: d.get("password"),
        min_ping: float("min_ping"),
        max_ping: float("max_ping"),
        from_lan: d.int("from_lan") != 0,
        clients: &clients,
    };
    let decision = oob::direct_connect(game, &mut table, &request);
    write_table(game, &table, args)?;
    if let Some(text) = decision.reply(game) {
        let mut packet = [0u8; 80];
        let length = oob::out_of_band_print(game, text, &mut packet).ok_or("too long")?;
        record_if_sent(args.buffer(3).ok_or("NULL sent")?, &from, &packet[..length]);
    }
    let label = match decision {
        ConnectDecision::Reconnect { slot, .. } => format!("reconnect {slot}"),
        ConnectDecision::NewSlot { slot, .. } => format!("new {slot}"),
        _ => "return".to_string(),
    };
    Ok(Ret::Str(Some(label.into_bytes())))
}

/// Where each game keeps a `serverInfo_t` field: (byte fields, short
/// fields, text fields) as (offset, which).
struct EntryLayout {
    bytes: &'static [(usize, usize)],
    shorts: &'static [(usize, usize)],
    texts: &'static [(usize, usize, usize)],
}

// Byte fields: 0 net_type, 1 clients, 2 max_clients, 3 allow_anonymous, 4 password,
// 5 pure, 6 friendly_fire, 7 killcam, 8 console_disabled, 9 hardware, 10 modded,
// 11 voice, 12 punkbuster. Shorts: 0 min_ping, 1 max_ping, 2 ping, 3 timeouts,
// 4 jeeps, 5 tanks. Texts (offset, which, size): 0 hostname, 1 mapname, 2 game,
// 3 gametype.
const COD2_ENTRY: EntryLayout = EntryLayout {
    bytes: &[
        (0x14, 0),
        (0x15, 1),
        (0x16, 2),
        (0x18, 3),
        (0x19, 4),
        (0x1A, 5),
        (0x1B, 6),
        (0x1C, 7),
        (0x1D, 8),
        (0x1E, 9),
        (0x1F, 10),
        (0x20, 11),
        (0x21, 12),
    ],
    shorts: &[(0x24, 0), (0x26, 1), (0x28, 2)],
    texts: &[(0x2A, 0, 32), (0x4A, 1, 32), (0x6A, 2, 24), (0x82, 3, 16)],
};
const UO_ENTRY: EntryLayout = EntryLayout {
    bytes: &[
        (0x14, 0),
        (0x15, 1),
        (0x16, 2),
        (0x18, 3),
        (0x19, 4),
        (0x1A, 5),
        (0x1B, 6),
        (0x1C, 7),
        (0x1D, 8),
        (0x1E, 9),
        (0x1F, 10),
        (0x20, 12),
    ],
    shorts: &[
        (0x22, 0),
        (0x24, 1),
        (0x26, 2),
        (0x28, 3),
        (0x2A, 4),
        (0x2C, 5),
    ],
    texts: &[(0x2E, 0, 32), (0x4E, 1, 32), (0x6E, 2, 24), (0x86, 3, 16)],
};

fn entry_bytes(e: &mut ServerListEntry) -> [&mut u8; 13] {
    [
        &mut e.net_type,
        &mut e.clients,
        &mut e.max_clients,
        &mut e.allow_anonymous,
        &mut e.password,
        &mut e.pure,
        &mut e.friendly_fire,
        &mut e.killcam,
        &mut e.console_disabled,
        &mut e.hardware,
        &mut e.modded,
        &mut e.voice,
        &mut e.punkbuster,
    ]
}

fn entry_shorts(e: &mut ServerListEntry) -> [&mut i16; 6] {
    [
        &mut e.min_ping,
        &mut e.max_ping,
        &mut e.ping,
        &mut e.timeouts_allowed,
        &mut e.jeeps,
        &mut e.tanks,
    ]
}

fn entry_texts(e: &mut ServerListEntry) -> [&mut [u8]; 4] {
    [
        &mut e.hostname[..],
        &mut e.mapname[..],
        &mut e.game[..],
        &mut e.gametype[..],
    ]
}

/// `CL_SetServerInfo`: (entry, info or NULL, ping).
fn set_server_info(game: Game, args: &mut Args) -> Answer {
    let info = args.str(1).map(<[u8]>::to_vec);
    let ping = args.int(2) as i16;
    let layout = match game {
        Game::CallOfDuty2 => &COD2_ENTRY,
        Game::UnitedOffensive => &UO_ENTRY,
    };
    let raw = args.buffer(0).ok_or("NULL entry")?;
    let mut entry = ServerListEntry::default();
    for &(at, which) in layout.bytes {
        *entry_bytes(&mut entry)[which] = raw[at];
    }
    for &(at, which) in layout.shorts {
        *entry_shorts(&mut entry)[which] = i16::from_le_bytes([raw[at], raw[at + 1]]);
    }
    for &(at, which, size) in layout.texts {
        entry_texts(&mut entry)[which].copy_from_slice(&raw[at..at + size]);
    }
    oob::set_server_info(game, &mut entry, info.as_deref(), ping);
    for &(at, which) in layout.bytes {
        raw[at] = *entry_bytes(&mut entry)[which];
    }
    for &(at, which) in layout.shorts {
        raw[at..at + 2].copy_from_slice(&entry_shorts(&mut entry)[which].to_le_bytes());
    }
    for &(at, which, size) in layout.texts {
        raw[at..at + size].copy_from_slice(entry_texts(&mut entry)[which]);
    }
    Ok(Ret::Void)
}

/// `CL_ServerInfoPacket`'s protocol test: (from, msg holding the info
/// string, debug_protocol) -> "accepted" or "return".
fn server_info_packet(game: Game, args: &mut Args) -> Answer {
    let debug = args.bytes(2).to_vec();
    let zero = debug
        .iter()
        .position(|&c| c == 0)
        .ok_or("bad debug_protocol")?;
    let value = f32::from_be_bytes([
        debug[zero + 1],
        debug[zero + 2],
        debug[zero + 3],
        debug[zero + 4],
    ]);
    let msg = args.bytes(1);
    // The message's data after the 20-byte header, read from the start up
    // to its zero byte (what MSG_ReadString returns for these strings).
    let data = &msg[20..];
    let info = &data[..data.iter().position(|&c| c == 0).unwrap_or(data.len())];
    let accepted = oob::info_response_accepted(game, info, &debug[..zero], value);
    let label: &[u8] = if accepted { b"accepted" } else { b"return" };
    Ok(Ret::Str(Some(label.to_vec())))
}

/// `None` when `function` isn't one of this module's.
pub fn dispatch(game: Game, function: &str, args: &mut Args) -> Option<Answer> {
    if let Some(answer) = crate::net_netchan::dispatch(game, function, args) {
        return Some(answer);
    }
    match function {
        "Cmd_TokenizeStringInternal" => tokenize(game, args),
        "NET_OutOfBandPrint" => Some(out_of_band_print(game, args)),
        "NET_OutOfBandData" => Some(out_of_band_data(game, args)),
        "SV_ConnectionlessPacket" => Some(connectionless_packet(game, args)),
        "SVC_Info" => Some(respond(game, args, false)),
        "SVC_Status" => Some(respond(game, args, true)),
        "atol" => Some(Ok(Ret::Int(oob::atol(args.text(0)).into()))),
        "SV_GetChallenge" => Some(get_challenge(game, args)),
        "SV_DirectConnect" => Some(direct_connect(game, args)),
        "CL_SetServerInfo" => Some(set_server_info(game, args)),
        "CL_ServerInfoPacket" => Some(server_info_packet(game, args)),
        "NET_CompareAdrSigned" | "NET_CompareBaseAdrSigned" => Some((|| {
            let a = address_from(args.bytes(0))?;
            let b = address_from(args.bytes(1))?;
            let same = if function == "NET_CompareAdrSigned" {
                oob::same_address(&a, &b)
            } else {
                oob::same_base_address(game, &a, &b)
            };
            Ok(Ret::Int(same.into()))
        })()),
        _ => None,
    }
}
