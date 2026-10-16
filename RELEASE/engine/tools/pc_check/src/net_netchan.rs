//! Requests for `iw_pc::net::netchan` (helper "pc-oob"). The specs are in
//! `tools/decomp/x86/specs/net_netchan.py`, which describes the encodings:
//!
//! * a channel (`Chan`): 11 big-endian 32-bit numbers (outgoing sequence,
//!   source, dropped, incoming sequence, qport, fragment sequence, fragment
//!   length, unsent fragments, unsent fragment start, unsent length, and the
//!   original's profiling pointer, kept as it is), the 20-byte `netadr_t`,
//!   then the fragment buffer and the unsent buffer;
//! * the packets a call sends (`SentPackets`): for each, a 4-byte
//!   big-endian length, the 16-byte destination `sockaddr_in`, the bytes;
//! * a message (`Msg`): as in `net_msg.rs`.

use iw_pc::net::msg::Msg;
use iw_pc::net::netchan::{
    self, max_message_length, AddressType, NetAddress, NetSource, Netchan, Packet,
};
use iw_pc::Game;

use crate::{game_error, Answer, Args, Ret};

const CHAN_WORDS: usize = 11;
const ADDRESS_SIZE: usize = 20;

fn word(bytes: &[u8], index: usize) -> i32 {
    let at = index * 4;
    i32::from_be_bytes([bytes[at], bytes[at + 1], bytes[at + 2], bytes[at + 3]])
}

/// A `netadr_t` as the guest holds it.
pub fn address_from(bytes: &[u8]) -> Result<NetAddress, String> {
    let kind = u32::from_le_bytes([bytes[0], bytes[1], bytes[2], bytes[3]]);
    let mut ipx = [0u8; 10];
    ipx.copy_from_slice(&bytes[10..20]);
    Ok(NetAddress {
        kind: AddressType::from_number(kind).ok_or(format!("address type {kind}"))?,
        ip: [bytes[4], bytes[5], bytes[6], bytes[7]],
        port: u16::from_be_bytes([bytes[8], bytes[9]]),
        ipx,
    })
}

pub fn address_bytes(address: &NetAddress) -> [u8; ADDRESS_SIZE] {
    let mut out = [0u8; ADDRESS_SIZE];
    out[..4].copy_from_slice(&address.kind.number().to_le_bytes());
    out[4..8].copy_from_slice(&address.ip);
    out[8..10].copy_from_slice(&address.port.to_be_bytes());
    out[10..].copy_from_slice(&address.ipx);
    out
}

fn source_from(number: i32) -> Result<NetSource, String> {
    match number {
        0 => Ok(NetSource::Client),
        1 => Ok(NetSource::Server),
        _ => Err(format!("netsrc {number}")),
    }
}

fn size(value: i32, name: &str) -> Result<usize, String> {
    usize::try_from(value).map_err(|_| format!("negative {name}"))
}

/// The channel in argument `i`, and its profiling pointer (not part of the port).
fn chan_from(game: Game, args: &mut Args, i: usize) -> Result<(Netchan, i32), String> {
    let bytes = args.buffer(i).ok_or("NULL netchan")?;
    let max = max_message_length(game);
    if bytes.len() != chan_token_size(game) {
        return Err("netchan token size".to_string());
    }
    chan_from_bytes(game, bytes, max)
}

/// The size of a channel's token.
fn chan_token_size(game: Game) -> usize {
    CHAN_WORDS * 4 + ADDRESS_SIZE + 2 * max_message_length(game)
}

fn chan_from_bytes(game: Game, bytes: &[u8], max: usize) -> Result<(Netchan, i32), String> {
    let address = address_from(&bytes[CHAN_WORDS * 4..])?;
    let mut chan = Netchan::new(game, source_from(word(bytes, 1))?, address, word(bytes, 4));
    chan.outgoing_sequence = word(bytes, 0);
    chan.dropped = word(bytes, 2);
    chan.incoming_sequence = word(bytes, 3);
    chan.fragment_sequence = word(bytes, 5);
    chan.fragment_length = size(word(bytes, 6), "fragment length")?;
    chan.unsent_fragments = word(bytes, 7) != 0;
    chan.unsent_fragment_start = size(word(bytes, 8), "unsent start")?;
    chan.unsent_length = size(word(bytes, 9), "unsent length")?;
    let buffers = &bytes[CHAN_WORDS * 4 + ADDRESS_SIZE..];
    chan.fragment_buffer.copy_from_slice(&buffers[..max]);
    chan.unsent_buffer.copy_from_slice(&buffers[max..]);
    Ok((chan, word(bytes, 10)))
}

fn chan_to(chan: &Netchan, profile: i32, args: &mut Args, i: usize) -> Result<(), String> {
    let bytes = args.buffer(i).ok_or("NULL netchan")?;
    chan_to_bytes(chan, profile, bytes);
    Ok(())
}

fn chan_to_bytes(chan: &Netchan, profile: i32, bytes: &mut [u8]) {
    let source = match chan.source {
        NetSource::Client => 0,
        NetSource::Server => 1,
    };
    let words = [
        chan.outgoing_sequence,
        source,
        chan.dropped,
        chan.incoming_sequence,
        chan.qport,
        chan.fragment_sequence,
        chan.fragment_length as i32,
        i32::from(chan.unsent_fragments),
        chan.unsent_fragment_start as i32,
        chan.unsent_length as i32,
        profile,
    ];
    for (k, value) in words.iter().enumerate() {
        bytes[k * 4..k * 4 + 4].copy_from_slice(&value.to_be_bytes());
    }
    let at = CHAN_WORDS * 4;
    bytes[at..at + ADDRESS_SIZE].copy_from_slice(&address_bytes(&chan.remote_address));
    let max = chan.fragment_buffer.len();
    let buffers = &mut bytes[at + ADDRESS_SIZE..];
    buffers[..max].copy_from_slice(&chan.fragment_buffer);
    buffers[max..max * 2].copy_from_slice(&chan.unsent_buffer);
}

/// Appends a packet sent to `to` to a `SentPackets` buffer.
pub fn record_sent(out: &mut Vec<u8>, to: &NetAddress, packet: &[u8]) {
    out.extend_from_slice(&(packet.len() as u32).to_be_bytes());
    // sockaddr_in: AF_INET, the port and address as stored, eight zeros.
    out.extend_from_slice(&2u16.to_le_bytes());
    out.extend_from_slice(&to.port.to_be_bytes());
    out.extend_from_slice(&to.ip);
    out.extend_from_slice(&[0; 8]);
    out.extend_from_slice(packet);
}

/// Bytes before the data in a `Msg` token: five 32-bit fields.
const MSG_HEADER: usize = 20;

/// Runs `f` on the message in argument `i` and writes it back (the token
/// layout of `net_msg.rs`).
pub fn with_msg<R>(
    args: &mut Args,
    i: usize,
    game: Game,
    f: impl FnOnce(&mut Msg) -> R,
) -> Result<R, String> {
    let buffer = args.buffer(i).ok_or("NULL msg")?;
    if buffer.len() < MSG_HEADER {
        return Err("short msg token".to_string());
    }
    let (header, data) = buffer.split_at_mut(MSG_HEADER);
    let mut msg = Msg::new(game, data);
    msg.overflowed = word(header, 0) != 0;
    msg.maxsize = size(word(header, 1), "maxsize")?;
    msg.cursize = size(word(header, 2), "cursize")?;
    msg.readcount = size(word(header, 3), "readcount")?;
    msg.bit = size(word(header, 4), "bit")?;
    let result = f(&mut msg);
    let fields = [
        i32::from(msg.overflowed),
        msg.maxsize as i32,
        msg.cursize as i32,
        msg.readcount as i32,
        msg.bit as i32,
    ];
    for (k, value) in fields.iter().enumerate() {
        header[k * 4..k * 4 + 4].copy_from_slice(&value.to_be_bytes());
    }
    Ok(result)
}

fn setup(game: Game, args: &mut Args) -> Answer {
    let (mut chan, _profile) = chan_from(game, args, 0)?;
    let source = source_from(args.i32(1))?;
    let address = address_from(args.bytes(2))?;
    chan.setup(source, address, args.i32(3));
    chan_to(&chan, 0, args, 0)?;
    Ok(Ret::Void)
}

/// The original's result after sending: Call of Duty 2 returns whether the
/// packet went out (always, here: an IPv4 address and an open socket).
fn sent_result(game: Game) -> Ret {
    match game {
        Game::CallOfDuty2 => Ret::Int(1),
        Game::UnitedOffensive => Ret::Void,
    }
}

fn transmit(game: Game, args: &mut Args) -> Answer {
    let (mut chan, profile) = chan_from(game, args, 0)?;
    let length = args.int(1) as usize;
    let data = args.bytes(2);
    let data = data.get(..length).ok_or("length past the data")?.to_vec();
    let mut packet = Packet::default();
    chan.transmit(&data, &mut packet).map_err(game_error)?;
    let to = chan.remote_address;
    chan_to(&chan, profile, args, 0)?;
    record_sent(args.buffer(3).ok_or("NULL sent")?, &to, packet.as_bytes());
    Ok(sent_result(game))
}

fn transmit_next_fragment(game: Game, args: &mut Args) -> Answer {
    let (mut chan, profile) = chan_from(game, args, 0)?;
    let mut packet = Packet::default();
    chan.transmit_next_fragment(&mut packet);
    let to = chan.remote_address;
    chan_to(&chan, profile, args, 0)?;
    record_sent(args.buffer(1).ok_or("NULL sent")?, &to, packet.as_bytes());
    Ok(sent_result(game))
}

fn process(game: Game, args: &mut Args) -> Answer {
    let (mut chan, profile) = chan_from(game, args, 0)?;
    let whole = with_msg(args, 1, game, |msg| chan.process(msg))?;
    chan_to(&chan, profile, args, 0)?;
    Ok(Ret::Int(whole.into()))
}

/// `SV_SendMessageToClient`: (client, message, sent). The client token is
/// the channel's, a 4-byte length and a TLV (challenge, text, time, the
/// coded body); the reply holds the channel, the 32 frames (sent, acked,
/// size) and `nextSnapshotTime`.
fn send_message(game: Game, args: &mut Args) -> Answer {
    let message = args.bytes(1).to_vec();
    let head = message.get(20..24).ok_or("short message")?;
    let head = [head[0], head[1], head[2], head[3]];
    let token = args.buffer(0).ok_or("NULL client")?;
    let size = chan_token_size(game);
    let (mut chan, profile) = chan_from_bytes(game, &token[..size], max_message_length(game))?;
    let extra = token[size + 4..].to_vec();
    let d = crate::net_oob::Description::parse(&extra)?;
    let time = d.int("time");
    let mut scratch = vec![0u8; max_message_length(game) + 4];
    let mut packet = Packet::default();
    let sent = netchan::send_server_message(
        &mut chan,
        d.int("challenge"),
        d.get("text"),
        head,
        d.get("body"),
        &mut scratch,
        &mut packet,
    )
    .map_err(game_error)?;
    let mut reply = vec![0u8; size];
    chan_to_bytes(&chan, profile, &mut reply);
    for frame in 0..32 {
        let values = if frame == sent.frame {
            [time, -1, sent.size as i32]
        } else {
            [0, 0, 0]
        };
        for value in values {
            reply.extend_from_slice(&value.to_be_bytes());
        }
    }
    // A LAN client may get the next snapshot at once.
    reply.extend_from_slice(&time.wrapping_sub(1).to_be_bytes());
    let to = chan.remote_address;
    *args.buffer(0).ok_or("NULL client")? = reply;
    record_sent(args.buffer(2).ok_or("NULL sent")?, &to, packet.as_bytes());
    Ok(Ret::Void)
}

/// The scramblers: (data, key state, length). The key state is the key's
/// numbers (big-endian 32-bit words, `count` of them) and then the text.
fn scramble(args: &mut Args, count: usize, key: impl Fn(&[i32]) -> u8) -> Answer {
    let state = args.bytes(1);
    if state.len() < count * 4 {
        return Err("short key state".to_string());
    }
    let numbers: Vec<i32> = (0..count).map(|k| word(state, k)).collect();
    let text = state[count * 4..].to_vec();
    let length = args.int(2);
    let data = args.buffer(0).ok_or("NULL data")?;
    // The originals do nothing for a length of 0 or less.
    let length = usize::try_from(length).unwrap_or(0);
    let data = data.get_mut(..length).ok_or("length past the data")?;
    netchan::scramble(data, key(&numbers), &text);
    Ok(Ret::Void)
}

/// `None` when `function` isn't one of this module's.
pub fn dispatch(game: Game, function: &str, args: &mut Args) -> Option<Answer> {
    Some(match function {
        "Netchan_Setup" => setup(game, args),
        "Netchan_Transmit" => transmit(game, args),
        "Netchan_TransmitNextFragment" => transmit_next_fragment(game, args),
        "Netchan_Process" => process(game, args),
        "SV_SendMessageToClient" => send_message(game, args),
        // challenge, outgoing sequence
        "SV_Netchan_Encode" => scramble(args, 2, |n| netchan::server_encode_key(n[0], n[1])),
        // challenge, server id, message acknowledge
        "SV_Netchan_Decode" => scramble(args, 3, |n| netchan::server_decode_key(n[0], n[1], n[2])),
        // challenge, server id, server message sequence
        "CL_Netchan_Encode" => scramble(args, 3, |n| netchan::client_encode_key(n[0], n[1], n[2])),
        // challenge, server message sequence
        "CL_Netchan_Decode" => scramble(args, 2, |n| netchan::client_decode_key(n[0], n[1])),
        _ => return None,
    })
}
