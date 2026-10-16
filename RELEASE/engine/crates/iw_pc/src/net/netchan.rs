//! The connection layer, the games' `netchan_t`: every packet of a
//! connection starts with a small header (sequence number, the client's
//! qport, fragment information); messages too big for one packet go out in
//! fragments and are put back together on arrival; packets that arrive late
//! or twice are dropped. Inside, the message body is Huffman-coded
//! ([`super::msg`]) and scrambled with a running XOR key
//! ([`scramble`]). `docs/PC_MULTIPLAYER.md` section 5.
//!
//! # The packet, byte for byte
//!
//! All numbers little-endian:
//!
//! | Field | Size | When |
//! |---|---|---|
//! | sequence | 32 bits | always; the top bit set means "fragment" |
//! | qport | 16 bits | client to server only |
//! | fragment start | **32 bits in Call of Duty 2, 16 in United Offensive** | fragments only |
//! | fragment length | 16 bits | fragments only |
//! | payload | the rest | |
//!
//! A message of [`FRAGMENT_SIZE`] (1300) bytes or more goes out in
//! 1300-byte fragments, all with the same sequence number; a fragment
//! shorter than 1300 bytes is the last one, so a message whose length is a
//! multiple of 1300 ends with an empty fragment. The receiver only accepts
//! fragments in order.
//!
//! # What goes inside, in what order
//!
//! Read from `SV_SendMessageToClient` (0x0045F370 in CoD2MP_s.exe 1.3,
//! 0x00465DA0 in CoDUOMP.exe 1.51), server to client. The game writes the
//! whole message uncompressed (its first 4 bytes are the last client
//! command the server received, `client->lastClientCommand`), then:
//!
//! 1. copies those first 4 bytes raw to the start of a send buffer;
//! 2. Huffman-codes the rest of the message (bytes 4 to `cursize`) in one go
//!    into the buffer after them (`MSG_WriteBitsCompress`), giving `n` bytes;
//! 3. scrambles those `n` coded bytes, and only them, with
//!    [`server_encode_key`] (the challenge and the channel's
//!    `outgoingSequence`, the sequence number the packet is about to carry)
//!    and the client's last command text (`SV_Netchan_Encode`);
//! 4. hands the `4 + n` bytes to [`Netchan::transmit`], which puts the
//!    header in front and sends the first packet or fragment (the server
//!    sends the remaining fragments one per frame, [`Netchan::transmit_next_fragment`]).
//!
//! Client to server (`CL_WritePacket`, 0x00408E80 / 0x0040CC80) is the same
//! with a 9-byte raw header (server id byte, `serverMessageSequence`,
//! `serverCommandSequence`): 9 bytes raw, the rest Huffman-coded, the coded
//! bytes scrambled with [`client_encode_key`] (`CL_Netchan_Encode`), then
//! `Netchan_Transmit` and, at once, every remaining fragment.
//!
//! On arrival, [`Netchan::process`] checks the header and puts fragments
//! together; the receiver then reads the raw fields after the header and
//! unscrambles the rest (`SV_Netchan_Decode` with [`server_decode_key`],
//! `CL_Netchan_Decode` with [`client_decode_key`]) before Huffman-decoding
//! it. The client reads its 4-byte reliable acknowledge first and
//! unscrambles from there to the end of the message.
//!
//! # The two games
//!
//! Quake III's netchan, with these differences, all checked against the
//! machine code:
//!
//! * the fragment start is 32 bits in Call of Duty 2, 16 in United
//!   Offensive (which reads it back sign-extended, as Quake III does);
//! * the largest message is 128 KiB in Call of Duty 2, 32 KiB in United
//!   Offensive ([`max_message_length`]);
//! * **United Offensive does not update `incomingSequence` when a
//!   fragmented message is complete** (Quake III 1.17's behavior; Call of
//!   Duty 2 has the later fix), so a client's packets acknowledge the last
//!   unfragmented message;
//! * the qport a client sends is the channel's in Call of Duty 2 and the
//!   `net_qport` setting in United Offensive (the same number: the client
//!   sets up its channel with `net_qport`, which can't change while the game
//!   runs);
//! * United Offensive's reads past the end of a packet don't mark the
//!   message as broken ([`Msg`]'s two games).
//!
//! Nothing here allocates per packet: a channel owns its two buffers
//! (fragments coming in, fragments going out), allocated once, and a packet
//! is built in a [`Packet`] on the caller's stack.

use crate::{Game, GameError};

use super::msg::Msg;

/// The size of a fragment: a message this long or longer goes out in pieces.
pub const FRAGMENT_SIZE: usize = 1300;

/// The most a packet holds, header included (`MAX_PACKETLEN`): the games
/// build every packet in a buffer of this size.
pub const MAX_PACKET_SIZE: usize = 1400;

/// The top bit of the sequence number: this packet is a fragment.
pub const FRAGMENT_BIT: u32 = 0x8000_0000;

/// The longest message a channel carries (`MAX_MSGLEN`): 128 KiB in Call of
/// Duty 2, 32 KiB in United Offensive. Both of a channel's buffers have
/// this size.
pub fn max_message_length(game: Game) -> usize {
    match game {
        Game::CallOfDuty2 => 0x20000,
        Game::UnitedOffensive => 0x8000,
    }
}

// --- addresses --------------------------------------------------------------------

/// What kind of address a [`NetAddress`] is (`netadrtype_t`, the same
/// numbers in both games and Quake III).
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub enum AddressType {
    /// 0: a bot (the server's own players without a network connection).
    Bot,
    /// 1: no address.
    #[default]
    Bad,
    /// 2: this machine, without the network.
    Loopback,
    /// 3: every machine on the LAN (IPv4 broadcast).
    Broadcast,
    /// 4: an IPv4 address.
    Ip,
    /// 5: an IPX address.
    Ipx,
    /// 6: IPX broadcast.
    BroadcastIpx,
}

impl AddressType {
    /// The number the games store.
    pub fn number(self) -> u32 {
        match self {
            AddressType::Bot => 0,
            AddressType::Bad => 1,
            AddressType::Loopback => 2,
            AddressType::Broadcast => 3,
            AddressType::Ip => 4,
            AddressType::Ipx => 5,
            AddressType::BroadcastIpx => 6,
        }
    }

    /// The type a stored number means (`None` for a number no address has).
    pub fn from_number(number: u32) -> Option<AddressType> {
        Some(match number {
            0 => AddressType::Bot,
            1 => AddressType::Bad,
            2 => AddressType::Loopback,
            3 => AddressType::Broadcast,
            4 => AddressType::Ip,
            5 => AddressType::Ipx,
            6 => AddressType::BroadcastIpx,
            _ => return None,
        })
    }
}

/// A network address, the games' 20-byte `netadr_t`: type, IPv4 address,
/// port, IPX address. (In memory, Call of Duty 2 keeps them in that order;
/// United Offensive keeps Quake III's order, the port last.)
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub struct NetAddress {
    pub kind: AddressType,
    pub ip: [u8; 4],
    /// The port number. The games store it in network byte order (its
    /// bytes big-endian in memory), as a socket address holds it.
    pub port: u16,
    pub ipx: [u8; 10],
}

impl NetAddress {
    /// An IPv4 address and port.
    pub fn ip(ip: [u8; 4], port: u16) -> NetAddress {
        NetAddress {
            kind: AddressType::Ip,
            ip,
            port,
            ipx: [0; 10],
        }
    }
}

/// Which side of a connection a channel is (`netsrc_t`): only a client
/// sends a qport, only a server reads one.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum NetSource {
    /// 0, `NS_CLIENT`.
    Client,
    /// 1, `NS_SERVER`.
    Server,
}

// --- packets ----------------------------------------------------------------------

/// One packet to send, built in place: the header and a payload, at most
/// [`MAX_PACKET_SIZE`] bytes. Lives on the caller's stack, so sending
/// allocates nothing.
#[derive(Clone)]
pub struct Packet {
    data: [u8; MAX_PACKET_SIZE],
    len: usize,
}

impl Default for Packet {
    fn default() -> Self {
        Packet {
            data: [0; MAX_PACKET_SIZE],
            len: 0,
        }
    }
}

impl Packet {
    /// The packet's bytes.
    pub fn as_bytes(&self) -> &[u8] {
        &self.data[..self.len]
    }

    fn clear(&mut self) {
        self.len = 0;
    }

    /// Appends `bytes` if they fit in [`MAX_PACKET_SIZE`], as the games'
    /// fixed send buffer does (a write that doesn't fit is skipped; with the
    /// netchan's sizes every write fits).
    fn put(&mut self, bytes: &[u8]) {
        let end = self.len + bytes.len();
        if end <= MAX_PACKET_SIZE {
            self.data[self.len..end].copy_from_slice(bytes);
            self.len = end;
        }
    }
}

// --- the channel ------------------------------------------------------------------

/// One connection's state, the games' `netchan_t`.
///
/// The fields are public, as in the original, where the client and server
/// code read the sequence numbers directly.
pub struct Netchan {
    /// Which game's rules the channel follows.
    pub game: Game,
    pub source: NetSource,
    /// Where packets go (and come from).
    pub remote_address: NetAddress,
    /// The client's random port number, sent in each client packet so the
    /// server knows the client even if a router changes its port. An `int`
    /// in the games; 16 bits go on the wire.
    pub qport: i32,
    /// The sequence number of the next message sent (starts at 1).
    pub outgoing_sequence: i32,
    /// The sequence number of the last message received in full.
    pub incoming_sequence: i32,
    /// How many packets were missing before the last one received.
    pub dropped: i32,
    /// The sequence number of the fragmented message being put together.
    pub fragment_sequence: i32,
    /// How many of its bytes have arrived.
    pub fragment_length: usize,
    /// Its bytes so far ([`max_message_length`] long).
    pub fragment_buffer: Box<[u8]>,
    /// Whether fragments of the last message are still to be sent.
    pub unsent_fragments: bool,
    /// Where the next of them starts in `unsent_buffer`.
    pub unsent_fragment_start: usize,
    /// The length of the message being sent in fragments.
    pub unsent_length: usize,
    /// That message ([`max_message_length`] long).
    pub unsent_buffer: Box<[u8]>,
}

impl Netchan {
    /// A channel to `remote_address`, as [`setup`](Self::setup) leaves it.
    /// Allocates its two buffers (2 x 128 KiB in Call of Duty 2, 2 x 32 KiB
    /// in United Offensive), once per connection.
    pub fn new(game: Game, source: NetSource, remote_address: NetAddress, qport: i32) -> Netchan {
        let size = max_message_length(game);
        Netchan {
            game,
            source,
            remote_address,
            qport,
            outgoing_sequence: 1,
            incoming_sequence: 0,
            dropped: 0,
            fragment_sequence: 0,
            fragment_length: 0,
            fragment_buffer: vec![0; size].into_boxed_slice(),
            unsent_fragments: false,
            unsent_fragment_start: 0,
            unsent_length: 0,
            unsent_buffer: vec![0; size].into_boxed_slice(),
        }
    }

    /// Starts the channel over for a new connection: everything zeroed (the
    /// buffers too), then the source, address and qport set, the outgoing
    /// sequence at 1. Reuses the buffers.
    ///
    /// Original: `Netchan_Setup` at 0x00447D50 (CoD2MP_s.exe 1.3)
    /// Original: `Netchan_Setup` at 0x0044D530 (CoDUOMP.exe 1.51)
    pub fn setup(&mut self, source: NetSource, remote_address: NetAddress, qport: i32) {
        self.source = source;
        self.remote_address = remote_address;
        self.qport = qport;
        self.outgoing_sequence = 1;
        self.incoming_sequence = 0;
        self.dropped = 0;
        self.fragment_sequence = 0;
        self.fragment_length = 0;
        self.fragment_buffer.fill(0);
        self.unsent_fragments = false;
        self.unsent_fragment_start = 0;
        self.unsent_length = 0;
        self.unsent_buffer.fill(0);
    }

    /// The qport field of a client's packet header (16 bits).
    fn header_qport(&self) -> [u8; 2] {
        (self.qport as u16).to_le_bytes()
    }

    /// Sends a message: builds the first packet in `packet`, to be sent to
    /// [`remote_address`](Self::remote_address). A message shorter than
    /// [`FRAGMENT_SIZE`] goes out whole, and the outgoing sequence number
    /// moves on; a longer one is copied into the channel and its first
    /// fragment goes out (see
    /// [`transmit_next_fragment`](Self::transmit_next_fragment), which the
    /// caller then calls while [`unsent_fragments`](Self::unsent_fragments)
    /// is set).
    ///
    /// A message longer than [`max_message_length`] is the game's error
    /// (`ERR_DROP`, "Netchan_Transmit: length = %i"), before anything changes.
    ///
    /// The originals also send the packet and (Call of Duty 2) return
    /// whether that worked, or `true` for a fragmented message; here the
    /// caller sends it.
    ///
    /// Original: `Netchan_Transmit` at 0x00447FB0 (CoD2MP_s.exe 1.3)
    /// Original: `Netchan_Transmit` at 0x0044D790 (CoDUOMP.exe 1.51)
    pub fn transmit(&mut self, data: &[u8], packet: &mut Packet) -> Result<(), GameError> {
        let length = data.len();
        if length > max_message_length(self.game) {
            return Err(GameError::drop(format!(
                "Netchan_Transmit: length = {length}"
            )));
        }
        self.unsent_fragment_start = 0;
        if length >= FRAGMENT_SIZE {
            self.unsent_fragments = true;
            self.unsent_length = length;
            self.unsent_buffer[..length].copy_from_slice(data);
            self.transmit_next_fragment(packet);
            return Ok(());
        }
        packet.clear();
        packet.put(&self.outgoing_sequence.to_le_bytes());
        self.outgoing_sequence = self.outgoing_sequence.wrapping_add(1);
        if self.source == NetSource::Client {
            packet.put(&self.header_qport());
        }
        packet.put(data);
        Ok(())
    }

    /// Builds the next fragment of the message being sent in `packet`: the
    /// sequence number with [`FRAGMENT_BIT`] set, the qport (from a
    /// client), where the fragment starts (32 bits in Call of Duty 2, 16 in
    /// United Offensive), its length, its bytes. After a fragment shorter
    /// than [`FRAGMENT_SIZE`] (the last one, possibly empty) the message is
    /// done: the outgoing sequence number moves on and
    /// [`unsent_fragments`](Self::unsent_fragments) is cleared.
    ///
    /// Original: `Netchan_TransmitNextFragment` at 0x00447DB0 (CoD2MP_s.exe 1.3)
    /// Original: `Netchan_TransmitNextFragment` at 0x0044D590 (CoDUOMP.exe 1.51)
    pub fn transmit_next_fragment(&mut self, packet: &mut Packet) {
        packet.clear();
        packet.put(&((self.outgoing_sequence as u32) | FRAGMENT_BIT).to_le_bytes());
        if self.source == NetSource::Client {
            packet.put(&self.header_qport());
        }
        let start = self.unsent_fragment_start;
        let length = FRAGMENT_SIZE.min(self.unsent_length.saturating_sub(start));
        match self.game {
            Game::CallOfDuty2 => packet.put(&(start as u32).to_le_bytes()),
            Game::UnitedOffensive => packet.put(&(start as u16).to_le_bytes()),
        }
        packet.put(&(length as u16).to_le_bytes());
        packet.put(&self.unsent_buffer[start..start + length]);
        self.unsent_fragment_start = start + length;
        if self.unsent_fragment_start == self.unsent_length && length != FRAGMENT_SIZE {
            self.outgoing_sequence = self.outgoing_sequence.wrapping_add(1);
            self.unsent_fragments = false;
        }
    }

    /// Reads a packet of this connection: `msg` holds the packet as it
    /// arrived. Returns `false` for a packet to ignore: one that arrived
    /// late or twice (its sequence number isn't newer than the last), a
    /// fragment that isn't the next one expected, a fragment with a bad
    /// length, or a fragment that doesn't finish its message. Returns
    /// `true` when `msg` holds a whole message to read: an unfragmented
    /// packet, positioned after its header; or the message the fragments
    /// make, copied into `msg` after a 4-byte sequence number, positioned
    /// after it.
    ///
    /// Along the way it counts the packets missing before this one
    /// ([`dropped`](Self::dropped)) and, for a whole message, records its
    /// sequence number (except United Offensive after a fragmented one, see
    /// the module documentation). A finished fragmented message longer than
    /// `msg.maxsize` is dropped (the originals then print
    /// "%s:fragmentLength %i > msg->maxsize").
    ///
    /// Call of Duty 2 reads the header with the message readers' end checks
    /// (a short packet marks `msg` as overflowed); United Offensive doesn't
    /// mark it, nor does it clear the mark when it starts reading.
    ///
    /// Original: `Netchan_Process` at 0x004481B0 (CoD2MP_s.exe 1.3)
    /// Original: `Netchan_Process` at 0x0044D970 (CoDUOMP.exe 1.51)
    pub fn process(&mut self, msg: &mut Msg) -> bool {
        let cod2 = self.game == Game::CallOfDuty2;
        if cod2 {
            msg.begin_reading();
        } else {
            msg.readcount = 0;
            msg.bit = 0;
        }
        let mut sequence = msg.read_long();
        let fragmented = sequence < 0;
        if fragmented {
            sequence &= 0x7FFF_FFFF;
        }
        if self.source == NetSource::Server {
            msg.read_short(); // the qport, read by the caller before
        }
        let (fragment_start, fragment_length) = if fragmented {
            let start = if cod2 {
                msg.read_long()
            } else {
                msg.read_short()
            };
            (start, msg.read_short())
        } else {
            (0, 0)
        };
        if sequence <= self.incoming_sequence {
            return false; // out of order or a duplicate
        }
        self.dropped = sequence
            .wrapping_sub(self.incoming_sequence)
            .wrapping_sub(1);
        if !fragmented {
            self.incoming_sequence = sequence;
            return true;
        }
        if sequence != self.fragment_sequence {
            self.fragment_sequence = sequence;
            self.fragment_length = 0;
        }
        if fragment_start != self.fragment_length as i32 {
            return false; // a fragment went missing: wait for the next message
        }
        let length = match usize::try_from(fragment_length) {
            Ok(length)
                if msg.readcount + length <= msg.cursize
                    && self.fragment_length + length <= max_message_length(self.game) =>
            {
                length
            }
            _ => return false, // an illegal fragment length
        };
        let from = msg.readcount;
        for (i, byte) in self.fragment_buffer[self.fragment_length..self.fragment_length + length]
            .iter_mut()
            .enumerate()
        {
            *byte = msg.data.get(from + i).copied().unwrap_or(0);
        }
        self.fragment_length += length;
        if length == FRAGMENT_SIZE {
            return false; // more to come
        }
        if self.fragment_length > msg.maxsize {
            return false;
        }
        // The whole message: its sequence number, then the fragments' bytes,
        // over the packet (the original writes up to maxsize + 4 bytes).
        let total = self.fragment_length;
        copy_into(msg.data, 0, &sequence.to_le_bytes());
        copy_into(msg.data, 4, &self.fragment_buffer[..total]);
        msg.cursize = total + 4;
        self.fragment_length = 0;
        if cod2 {
            msg.begin_reading();
        } else {
            msg.readcount = 0;
            msg.bit = 0;
        }
        msg.read_long(); // past the sequence number
        if cod2 {
            self.incoming_sequence = sequence;
        }
        true
    }
}

/// Copies `bytes` to `data[at..]`, as far as `data` goes.
fn copy_into(data: &mut [u8], at: usize, bytes: &[u8]) {
    let end = data.len().min(at + bytes.len());
    if at < end {
        data[at..end].copy_from_slice(&bytes[..end - at]);
    }
}

// --- a server's message, start to finish ---------------------------------------------

/// What [`send_server_message`] did, for the server's bookkeeping.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct SentMessage {
    /// The bytes handed to the channel (4 + the coded body): the
    /// snapshot frame's `messageSize`.
    pub size: usize,
    /// The snapshot frame the message belongs to (`outgoingSequence & 31`,
    /// taken before sending): the server sets its `messageSize`,
    /// `messageSent` (`svs.time`) and `messageAcked` (-1).
    pub frame: usize,
}

/// Sends one message from a server to a client, as `SV_SendMessageToClient`
/// does (the order is in the module documentation): `message_head` is the
/// message's first 4 bytes (the last client command the server received,
/// written raw as a long), `compressed_body` the rest of the message
/// Huffman-coded in one go (`MSG_WriteBitsCompress` of bytes 4 to
/// `cursize`, [`super::huffman`]'s `MessageTree::write_bits_compress`).
/// The two go into `scratch` (the original's send buffer, at least `4 +
/// compressed_body.len()` bytes), the body is scrambled with
/// [`server_encode_key`] and `last_client_command`, and the result goes to
/// [`Netchan::transmit`], which builds the first packet in `packet`.
///
/// The original then decides when the next snapshot may go (a LAN or
/// loopback client at once: `nextSnapshotTime = svs.time - 1`; others by
/// their `rate`, `SV_RateMsec`, not ported here).
///
/// Original: `SV_SendMessageToClient` at 0x0045F370 (CoD2MP_s.exe 1.3)
/// Original: `SV_SendMessageToClient` at 0x00465DA0 (CoDUOMP.exe 1.51)
pub fn send_server_message(
    chan: &mut Netchan,
    challenge: i32,
    last_client_command: &[u8],
    message_head: [u8; 4],
    compressed_body: &[u8],
    scratch: &mut [u8],
    packet: &mut Packet,
) -> Result<SentMessage, GameError> {
    let size = 4 + compressed_body.len();
    scratch[..4].copy_from_slice(&message_head);
    scratch[4..size].copy_from_slice(compressed_body);
    let frame = (chan.outgoing_sequence & 31) as usize;
    let key = server_encode_key(challenge, chan.outgoing_sequence);
    scramble(&mut scratch[4..size], key, last_client_command);
    chan.transmit(&scratch[..size], packet)?;
    Ok(SentMessage { size, frame })
}

// --- scrambling -------------------------------------------------------------------

/// Scrambles (or unscrambles: it is its own inverse) the Huffman-coded part
/// of a message with a running XOR key, the same loop in all four of the
/// games' functions:
///
/// for each byte `i` of `data`, the key byte is XORed with the next
/// character of `text` shifted left by `i & 1` (the shift drops the
/// character's top bit), and the data byte is XORed with the key. `text`
/// is used again from its start when its end (or a zero byte) is reached;
/// an empty text leaves the key as it is.
///
/// `key` comes from [`server_encode_key`], [`server_decode_key`],
/// [`client_encode_key`] or [`client_decode_key`]; `text` is the last
/// reliable command the other side acknowledged (see each). Unlike Quake
/// III 1.32 the characters are used as they are: no `%` or bytes above 127
/// are replaced by `.`.
///
/// Original: `SV_Netchan_Encode` at 0x0045C8C0 (CoD2MP_s.exe 1.3)
/// Original: `SV_Netchan_Decode` at 0x0045C920 (CoD2MP_s.exe 1.3)
/// Original: `CL_Netchan_Encode` at 0x004127F0 (CoD2MP_s.exe 1.3)
/// Original: `CL_Netchan_Decode` at 0x00412860 (CoD2MP_s.exe 1.3)
/// Original: `SV_Netchan_Encode` at 0x00463640 (CoDUOMP.exe 1.51)
/// Original: `SV_Netchan_Decode` at 0x004636A0 (CoDUOMP.exe 1.51)
/// Original: `CL_Netchan_Encode` at 0x004173F0 (CoDUOMP.exe 1.51)
/// Original: `CL_Netchan_Decode` at 0x00417460 (CoDUOMP.exe 1.51)
pub fn scramble(data: &mut [u8], key: u8, text: &[u8]) {
    let text = &text[..text.iter().position(|&c| c == 0).unwrap_or(text.len())];
    let mut key = key;
    let mut index = 0;
    for (i, byte) in data.iter_mut().enumerate() {
        if index >= text.len() {
            index = 0;
        }
        let c = text.get(index).copied().unwrap_or(0);
        key ^= c << (i & 1);
        *byte ^= key;
        index += 1;
    }
}

/// The key a server scrambles a message to a client with
/// (`SV_Netchan_Encode`): the low bytes of the client's `challenge` and of
/// its channel's `outgoing_sequence` (the sequence number the packet will
/// carry). The text is the client's last command the server received
/// (`client->lastClientCommandString`).
pub fn server_encode_key(challenge: i32, outgoing_sequence: i32) -> u8 {
    (challenge ^ outgoing_sequence) as u8
}

/// The key a server unscrambles a client's message with
/// (`SV_Netchan_Decode`): the low bytes of the client's `challenge` and of
/// the server id and message acknowledge the packet carries in its raw
/// header (which the server has just read into the client's fields). The
/// text is the server command the packet acknowledges
/// (`client->reliableCommands[reliableAcknowledge % n]`, n = 128 in Call of
/// Duty 2, 64 in United Offensive).
pub fn server_decode_key(challenge: i32, server_id: i32, message_acknowledge: i32) -> u8 {
    (challenge ^ server_id ^ message_acknowledge) as u8
}

/// The key a client scrambles its message with (`CL_Netchan_Encode`): the
/// low bytes of its `challenge`, of the server id and of
/// `serverMessageSequence` (the last two as its raw header carries them).
/// The text is the last server command the client received
/// (`clc.serverCommands[serverCommandSequence % n]`).
pub fn client_encode_key(challenge: i32, server_id: i32, server_message_sequence: i32) -> u8 {
    (challenge ^ server_id ^ server_message_sequence) as u8
}

/// The key a client unscrambles the server's message with
/// (`CL_Netchan_Decode`): the low bytes of its `challenge` and of the
/// message's sequence number (`clc.serverMessageSequence`, set from the
/// packet just before). The text is the client command the server
/// acknowledges (`clc.reliableCommands[reliableAcknowledge % n]`, the
/// acknowledge read from the 4 raw bytes before the scrambled part).
pub fn client_decode_key(challenge: i32, server_message_sequence: i32) -> u8 {
    (challenge ^ server_message_sequence) as u8
}

/// How many reliable commands each side keeps (the text of a scrambling
/// key is picked with the acknowledged command's number modulo this):
/// 128 in Call of Duty 2, 64 in United Offensive.
pub fn max_reliable_commands(game: Game) -> usize {
    match game {
        Game::CallOfDuty2 => 128,
        Game::UnitedOffensive => 64,
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn channel(game: Game, source: NetSource) -> Netchan {
        Netchan::new(game, source, NetAddress::ip([10, 0, 0, 2], 28960), 0x1234)
    }

    #[test]
    fn small_messages_go_out_whole() {
        let mut chan = channel(Game::CallOfDuty2, NetSource::Client);
        let mut packet = Packet::default();
        chan.transmit(b"abc", &mut packet).unwrap();
        assert_eq!(packet.as_bytes(), b"\x01\0\0\0\x34\x12abc");
        assert_eq!(chan.outgoing_sequence, 2);
        let mut chan = channel(Game::UnitedOffensive, NetSource::Server);
        chan.transmit(b"", &mut packet).unwrap();
        assert_eq!(packet.as_bytes(), b"\x01\0\0\0");
    }

    #[test]
    fn fragments_and_their_reassembly() {
        for game in [Game::CallOfDuty2, Game::UnitedOffensive] {
            let message: Vec<u8> = (0..2 * FRAGMENT_SIZE).map(|i| i as u8).collect();
            let mut sender = channel(game, NetSource::Server);
            let mut receiver = channel(game, NetSource::Client);
            let mut packet = Packet::default();
            sender.transmit(&message, &mut packet).unwrap();
            let mut packets = vec![packet.as_bytes().to_vec()];
            while sender.unsent_fragments {
                sender.transmit_next_fragment(&mut packet);
                packets.push(packet.as_bytes().to_vec());
            }
            // Two full fragments and an empty one.
            assert_eq!(packets.len(), 3);
            let start_size = if game == Game::CallOfDuty2 { 4 } else { 2 };
            assert_eq!(packets[2].len(), 4 + start_size + 2);
            assert_eq!(sender.outgoing_sequence, 2);
            let mut buffer = vec![0u8; max_message_length(game) + 8];
            for (k, bytes) in packets.iter().enumerate() {
                buffer[..bytes.len()].copy_from_slice(bytes);
                let mut msg = Msg::new(game, &mut buffer);
                msg.maxsize = max_message_length(game);
                msg.cursize = bytes.len();
                let whole = receiver.process(&mut msg);
                assert_eq!(whole, k == 2);
                if whole {
                    assert_eq!(msg.cursize, 4 + message.len());
                    assert_eq!(msg.readcount, 4);
                    assert_eq!(&msg.data[4..4 + message.len()], &message[..]);
                }
            }
            let expected = if game == Game::CallOfDuty2 { 1 } else { 0 };
            assert_eq!(receiver.incoming_sequence, expected);
        }
    }

    #[test]
    fn late_packets_are_dropped() {
        let mut receiver = channel(Game::CallOfDuty2, NetSource::Server);
        let mut buffer = *b"\x05\0\0\0\x34\x12hello\0\0\0\0";
        let mut msg = Msg::new(Game::CallOfDuty2, &mut buffer);
        msg.cursize = 11;
        assert!(receiver.process(&mut msg));
        assert_eq!((receiver.incoming_sequence, receiver.dropped), (5, 4));
        assert_eq!(msg.readcount, 6);
        assert!(!receiver.process(&mut msg));
    }

    #[test]
    fn scrambling_is_its_own_inverse() {
        let mut data = *b"some coded bytes";
        scramble(&mut data, 0x5A, b"cmd \xe9%");
        assert_ne!(&data, b"some coded bytes");
        scramble(&mut data, 0x5A, b"cmd \xe9%");
        assert_eq!(&data, b"some coded bytes");
        // An empty text keeps the key: every byte XORed with it.
        let mut data = [0u8; 3];
        scramble(&mut data, 0x21, b"");
        assert_eq!(data, [0x21; 3]);
    }
}
