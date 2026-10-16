//! Packets without a connection ("out of band"): the four bytes
//! `FF FF FF FF`, then a line of text the receiver splits into words like a
//! console command (`getinfo xxx`, `getchallenge`, `connect "<userinfo>"`).
//! A server answers with the same kind of packet (`infoResponse`,
//! `challengeResponse`, `error` ...). This is how a client finds servers on
//! the LAN and starts a connection (`docs/PC_MULTIPLAYER.md` section 3).
//!
//! Everything a Rust server sends here is the byte string a CB dedicated
//! server of the same version sends in the same situation.
//!
//! * [`Line`] and [`Tokens`]: reading the first line of a packet and
//!   splitting it into words, exactly as the games do (quotes, `//` and
//!   `/* */` comments, control characters, the 512-word limit).
//!
//! # Call of Duty 2 and United Offensive
//!
//! The two games share this code with small differences, each documented
//! where it matters: United Offensive turns every byte above 127 into `.`
//! when it reads the line, Call of Duty 2 keeps them (and its tokenizer
//! then treats them as spaces); Call of Duty 2's tokenizer reads `\"`
//! inside quotes as a quote; a line of more than 512 words gives no words
//! at all in Call of Duty 2 and the first 512 in United Offensive.

use crate::shared::strings::{stricmpn, strncmp, STRICMP_LIMIT_COD2, STRICMP_LIMIT_UO};
use crate::Game;

use super::netchan::{AddressType, NetAddress};

/// The four bytes a packet without a connection starts with (the 32-bit
/// sequence number -1, which no connection uses).
pub const CONNECTIONLESS_PREFIX: [u8; 4] = [0xFF; 4];

/// Whether `packet` is a packet without a connection: at least four bytes,
/// the first four all 0xFF. Anything else belongs to a connection
/// ([`super::netchan`]).
///
/// The first test of `SV_PacketEvent` (0x0045BB80 in CoD2MP_s.exe 1.3,
/// 0x00462AB0 in CoDUOMP.exe 1.51): `cursize >= 4 && *(int *)data == -1`.
pub fn is_connectionless(packet: &[u8]) -> bool {
    packet.len() >= 4 && packet[..4] == CONNECTIONLESS_PREFIX
}

// --- the first line ---------------------------------------------------------------

/// `MAX_STRING_CHARS`: the line buffer `MSG_ReadStringLine` fills (the
/// longest line is one less, 1023 characters).
pub const MAX_LINE: usize = 1024;

/// The first line of a packet without a connection, as the server reads it:
/// the text after the four 0xFF bytes up to the first newline or zero byte
/// (or the end of the packet), at most 1023 characters, with the games'
/// character changes.
///
/// A fixed buffer, so reading a packet allocates nothing.
#[derive(Clone)]
pub struct Line {
    text: [u8; MAX_LINE],
    len: usize,
}

impl Default for Line {
    fn default() -> Self {
        Line {
            text: [0; MAX_LINE],
            len: 0,
        }
    }
}

impl Line {
    /// The line's characters (without a terminator).
    pub fn as_bytes(&self) -> &[u8] {
        &self.text[..self.len]
    }

    /// Reads the first line of `packet` (the whole packet, prefix included)
    /// the way the server does: `SV_ConnectionlessPacket` starts reading at
    /// byte 4 and calls `MSG_ReadStringLine`, which stops at `\n`, a zero
    /// byte or the end of the message, turns `%` into `.` and the Windows
    /// right quote 0x92 into `'`, and keeps 1023 characters:
    ///
    /// * Call of Duty 2 reads (and drops) the rest of a longer line;
    /// * United Offensive stops after 1023 characters, and also turns every
    ///   byte above 127 into `.`.
    ///
    /// Only the first line matters here, so where each game stops reading
    /// doesn't show. (`MSG_ReadStringLine` itself, on any message, is
    /// [`super::msg`]'s; this is the same rule applied to a packet.)
    pub fn read(&mut self, game: Game, packet: &[u8]) {
        let body = packet.get(4..).unwrap_or(&[]);
        let mut len = 0;
        for &byte in body {
            if byte == b'\n' || byte == 0 || len == MAX_LINE - 1 {
                break;
            }
            self.text[len] = match byte {
                b'%' => b'.',
                0x92 => b'\'',
                0x80.. if game == Game::UnitedOffensive => b'.',
                other => other,
            };
            len += 1;
        }
        self.len = len;
    }

    /// The line `text` as it is (for a text that was not read from a packet:
    /// United Offensive's decompressed `connect`, a test). Keeps 1023 bytes.
    pub fn set(&mut self, text: &[u8]) {
        let len = text.len().min(MAX_LINE - 1);
        self.text[..len].copy_from_slice(&text[..len]);
        self.len = len;
    }
}

// --- words ------------------------------------------------------------------------

/// `MAX_STRING_TOKENS`: the most words a line is split into.
pub const MAX_TOKENS: usize = 512;
/// The tokenizer's text buffer: `BIG_INFO_STRING` + `MAX_STRING_TOKENS`
/// bytes (the games' 0x2200-byte arrays), every word followed by a zero.
pub const TOKEN_TEXT_SIZE: usize = 8192 + MAX_TOKENS;

/// A line split into words, the games' `Cmd_Argc` / `Cmd_Argv`.
///
/// The words are kept as the originals keep them: one after another in a
/// fixed text buffer, each followed by a zero byte, with where each starts.
/// Splitting a line allocates nothing.
#[derive(Clone)]
pub struct Tokens {
    text: [u8; TOKEN_TEXT_SIZE],
    /// How much of `text` the last split wrote.
    used: usize,
    starts: [u16; MAX_TOKENS],
    /// How many entries of `starts` the last split wrote.
    written: usize,
    /// `Cmd_Argc`: usually `written`, but 0 when Call of Duty 2 gave up on
    /// a line of more than 512 words.
    count: usize,
}

impl Default for Tokens {
    fn default() -> Self {
        Tokens {
            text: [0; TOKEN_TEXT_SIZE],
            used: 0,
            starts: [0; MAX_TOKENS],
            written: 0,
            count: 0,
        }
    }
}

/// What the tokenizer calls a space: any byte up to `' '` **as a signed
/// char**, so the bytes 0x80 to 0xFF are spaces too (`cmp dl, 0x20; jg`).
#[inline]
fn is_space(c: u8) -> bool {
    (c as i8) <= b' ' as i8
}

impl Tokens {
    /// `Cmd_Argc`: how many words the last split found.
    pub fn argc(&self) -> usize {
        self.count
    }

    /// `Cmd_Argv`: word `i`, or an empty string past the last one.
    pub fn argv(&self, i: usize) -> &[u8] {
        if i >= self.count {
            return b"";
        }
        let start = usize::from(self.starts[i]);
        let len = self.text[start..self.used]
            .iter()
            .position(|&c| c == 0)
            .unwrap_or(self.used - start);
        &self.text[start..start + len]
    }

    /// No words (United Offensive's tokenizer given a NULL text).
    pub fn clear(&mut self) {
        self.used = 0;
        self.written = 0;
        self.count = 0;
    }

    /// The text buffer as the original leaves it after the last split: the
    /// words, each followed by a zero byte (for the checker).
    pub fn raw_text(&self) -> &[u8] {
        &self.text[..self.used]
    }

    /// Where each word the last split wrote starts in [`raw_text`](Self::raw_text),
    /// including the ones Call of Duty 2 then disowned (for the checker).
    pub fn raw_starts(&self) -> &[u16] {
        &self.starts[..self.written]
    }

    /// Splits `line` into words, as the console and the network code do.
    ///
    /// * Spaces (every byte up to `' '`, and in Call of Duty 2's lines the
    ///   bytes above 127, see [`is_space`]) separate words.
    /// * `"..."` is one word without its quotes (it may be empty); the
    ///   closing quote may be missing. In Call of Duty 2, `\"` inside the
    ///   quotes is a quote character.
    /// * `//` ends the line; `/* ... */` is skipped between words (and
    ///   `/*/` is already a whole comment). A `"`, `//` or `/*` right after
    ///   a word's characters ends the word.
    /// * `limit`: when it is `n` > 0, the `n`th word is the rest of the line
    ///   as it is (from where the `n`th word would start: spaces before it
    ///   are kept but one); 0 means no limit.
    /// * After 512 words, if anything is left of the line: Call of Duty 2
    ///   reports no words at all, United Offensive keeps the 512.
    ///
    /// `line` ends at its first zero byte; at most 8703 bytes are read
    /// (the games' callers pass lines of at most 1023, or a 8191-character
    /// dvar), so the words always fit the buffer.
    ///
    /// Original: `Cmd_TokenizeStringInternal` at 0x004210D0 (CoD2MP_s.exe 1.3)
    /// Original: `Cmd_TokenizeStringInternal` at 0x0042C240 (CoDUOMP.exe 1.51)
    pub fn tokenize(&mut self, game: Game, line: &[u8], limit: u32) {
        let line = &line[..line.len().min(TOKEN_TEXT_SIZE - 1)];
        let line = &line[..line.iter().position(|&c| c == 0).unwrap_or(line.len())];
        // The original reads the C string; past its end it sees the terminator.
        let at = |i: usize| line.get(i).copied().unwrap_or(0);
        let mut p = 0;
        let mut out = 0;
        let mut written = 0;
        let mut remaining = limit;
        self.count = loop {
            remaining = remaining.wrapping_sub(1);
            if remaining == 0 {
                // The rest of the line, as it is, is the last word.
                if at(p) != 0 {
                    self.starts[written] = out as u16;
                    written += 1;
                    let rest = &line[p..];
                    self.text[out..out + rest.len()].copy_from_slice(rest);
                    out += rest.len();
                    self.text[out] = 0;
                    out += 1;
                }
                break written;
            }
            // Spaces and comments before the word.
            let mut ended = false;
            loop {
                let mut c = at(p);
                if c == 0 {
                    ended = true;
                    break;
                }
                while is_space(c) {
                    p += 1;
                    c = at(p);
                    if c == 0 {
                        break;
                    }
                }
                if c == 0 {
                    ended = true;
                    break;
                }
                if c == b'/' {
                    let next = at(p + 1);
                    if next == b'/' {
                        ended = true;
                        break;
                    }
                    if next == b'*' {
                        // Up to the first `*/`, counting the `*` of the `/*`.
                        let mut d = c;
                        loop {
                            if d == b'*' && at(p + 1) == b'/' {
                                break;
                            }
                            d = at(p + 1);
                            p += 1;
                            if d == 0 {
                                break;
                            }
                        }
                        if d == 0 {
                            ended = true;
                            break;
                        }
                        p += 2;
                        continue;
                    }
                }
                break;
            }
            if ended {
                break written;
            }
            // The word.
            self.starts[written] = out as u16;
            written += 1;
            if at(p) == b'"' {
                p += 1;
                let mut d = at(p);
                while d != 0 && d != b'"' {
                    if game == Game::CallOfDuty2 && d == b'\\' && at(p + 1) == b'"' {
                        p += 1;
                    }
                    self.text[out] = at(p);
                    out += 1;
                    d = at(p + 1);
                    p += 1;
                }
                self.text[out] = 0;
                out += 1;
                if at(p) == 0 {
                    break written;
                }
                p += 1;
            } else {
                let mut d = at(p);
                if !is_space(d) {
                    loop {
                        if d == b'"' {
                            break;
                        }
                        if d == b'/' && matches!(at(p + 1), b'/' | b'*') {
                            break;
                        }
                        self.text[out] = d;
                        out += 1;
                        d = at(p + 1);
                        p += 1;
                        if is_space(d) {
                            break;
                        }
                    }
                }
                self.text[out] = 0;
                out += 1;
            }
            // One space after the word is consumed with it.
            let c = at(p);
            if c == 0 {
                break written;
            }
            if is_space(c) {
                p += 1;
            }
            if written == MAX_TOKENS {
                break match game {
                    Game::CallOfDuty2 => 0,
                    Game::UnitedOffensive => written,
                };
            }
        };
        self.used = out;
        self.written = written;
    }
}

// --- sending ----------------------------------------------------------------------

/// The longest text Call of Duty 2 sends without a connection: its
/// `NET_OutOfBandPrint` refuses a text whose length plus terminator is over
/// 0x1FFFC bytes (and prints "OOB Packet is %i bytes - too large to send").
pub const MAX_PRINT_TEXT_COD2: usize = 0x1FFFC - 1;

/// The longest text United Offensive sends without a connection: its
/// `NET_OutOfBandPrint` formats into a 0x8000-byte buffer after the four
/// 0xFF bytes, with no check (a longer text overruns the buffer).
pub const MAX_PRINT_TEXT_UO: usize = 0x8000 - 4 - 1;

/// The most data United Offensive's `NET_OutOfBandData` takes: its packet
/// buffer is 64 KiB (and the adaptive coder it then runs keeps 16-bit sizes).
pub const MAX_DATA_UO: usize = 0x10000 - 4;

/// The most data Call of Duty 2's `NET_OutOfBandData` takes: its 128 KiB
/// packet buffer (it doesn't check).
pub const MAX_DATA_COD2: usize = 0x20000 - 4;

/// Builds a packet without a connection carrying `text`: the four 0xFF
/// bytes, then the text without its terminator (`text` ends at its first
/// zero byte, as the C string would). Returns the packet's length in
/// `out`, or `None` when the game wouldn't send it: a text too long
/// ([`MAX_PRINT_TEXT_COD2`], [`MAX_PRINT_TEXT_UO`]) or a packet too long for
/// `out`.
///
/// United Offensive's original takes a format and arguments (`vsprintf`);
/// Call of Duty 2's takes the formatted text. Either way the packet is the
/// four bytes and the text, which the caller formats. The originals then
/// send it (to the address they are given); the caller does that here.
///
/// Original: `NET_OutOfBandPrint` at 0x00448910 (CoD2MP_s.exe 1.3)
/// Original: `NET_OutOfBandPrint` at 0x0044E140 (CoDUOMP.exe 1.51)
pub fn out_of_band_print(game: Game, text: &[u8], out: &mut [u8]) -> Option<usize> {
    let text = &text[..text.iter().position(|&c| c == 0).unwrap_or(text.len())];
    let limit = match game {
        Game::CallOfDuty2 => MAX_PRINT_TEXT_COD2,
        Game::UnitedOffensive => MAX_PRINT_TEXT_UO,
    };
    if text.len() > limit {
        return None;
    }
    frame(text, out)
}

/// Builds a packet without a connection carrying `data` as it is: the four
/// 0xFF bytes, then the data. Returns the packet's length in `out`, or
/// `None` when it doesn't fit (or is more than the game's buffer holds,
/// [`MAX_DATA_COD2`], [`MAX_DATA_UO`]).
///
/// **United Offensive then compresses the packet** from byte 12 on with
/// Quake III's adaptive Huffman coder (`Huff_Compress(msg, 12)`, 0x00441FA0;
/// the first 12 bytes stay readable: `FF FF FF FF connect `), which is
/// [`super::huffman`]'s: the caller runs it on the packet this returns. The
/// only packet United Offensive sends this way is `connect`; Call of Duty 2
/// sends its `connect` as it is.
///
/// Original: `NET_OutOfBandData` at 0x00448A70 (CoD2MP_s.exe 1.3)
/// Original: `NET_OutOfBandData` at 0x0044E240 (CoDUOMP.exe 1.51)
pub fn out_of_band_data(game: Game, data: &[u8], out: &mut [u8]) -> Option<usize> {
    let limit = match game {
        Game::CallOfDuty2 => MAX_DATA_COD2,
        Game::UnitedOffensive => MAX_DATA_UO,
    };
    if data.len() > limit {
        return None;
    }
    frame(data, out)
}

/// The four 0xFF bytes and `body` in `out`.
fn frame(body: &[u8], out: &mut [u8]) -> Option<usize> {
    let length = 4 + body.len();
    let out = out.get_mut(..length)?;
    out[..4].copy_from_slice(&CONNECTIONLESS_PREFIX);
    out[4..].copy_from_slice(body);
    Some(length)
}

// --- what a server does with a packet ---------------------------------------------

/// What a server does with a packet without a connection, as its
/// `SV_ConnectionlessPacket` decides from the packet's first word.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ServerRequest {
    /// The packet starts with `pb_` (any case): PunkBuster's own packets,
    /// passed to PunkBuster when it runs (a Rust server ignores them).
    PunkBuster,
    /// `getstatus`: answer with `statusResponse` (`SVC_Status`).
    Status,
    /// `v`: Call of Duty 2's voice chat data (`SV_VoicePacket`).
    Voice,
    /// `getinfo`: answer with `infoResponse` (`SVC_Info`).
    Info,
    /// `getchallenge`: answer with `challengeResponse` (`SV_GetChallenge`).
    Challenge,
    /// `connect`: a player joins (`SV_DirectConnect`, after the PunkBuster
    /// hook `PbPassConnectString`).
    Connect,
    /// `ipAuthorize`: the authorize server's answer (`SV_AuthorizeIpPacket`).
    IpAuthorize,
    /// `rcon`: a remote console command (`SVC_RemoteCommand`).
    RemoteCommand,
    /// United Offensive's `disconnect`: ignored without a word.
    Ignored,
    /// Anything else: ignored (United Offensive prints "bad connectionless
    /// packet from %s:\n%s\n" when `developer` is on).
    Unknown,
}

/// Whether a packet is PunkBuster's: its text starts with `pb_` in any case
/// (`Q_stricmpn(data + 4, "pb_", 3)`, the servers' first test). The bytes
/// after the end of `packet` count as zeros.
pub fn is_punkbuster(packet: &[u8]) -> bool {
    stricmpn(packet.get(4..).unwrap_or(&[]), b"pb_", 3) == 0
}

/// Whether United Offensive will decompress this packet before reading it:
/// a packet whose text starts with `connect` exactly (case and all,
/// `strncmp(data + 4, "connect", 7)`) is decompressed from byte 12 on with
/// the adaptive Huffman coder (`Huff_Decompress(msg, 12)`, 0x00441D40;
/// [`super::huffman`]'s), and the result read instead. Call of Duty 2 never
/// decompresses. The bytes after the end of `packet` count as zeros (the
/// original looks at whatever its buffer holds there).
pub fn connect_is_compressed(game: Game, packet: &[u8]) -> bool {
    game == Game::UnitedOffensive && strncmp(packet.get(4..).unwrap_or(&[]), b"connect", 7) == 0
}

/// Decides what a server does with a packet without a connection (`packet`
/// is the whole packet, the four 0xFF bytes included; for United
/// Offensive's compressed `connect`, the packet after decompression, see
/// [`connect_is_compressed`]). Reads its first line into `line` and splits
/// it into `tokens`, as the original does, so the handler can use them
/// (`Cmd_Argv`).
///
/// The commands are compared without case, in this order: Call of Duty 2
/// `getstatus`, `v`, `getinfo`, `getchallenge`, `connect`, `ipAuthorize`,
/// `rcon`; United Offensive `getstatus`, `getinfo`, `getchallenge`,
/// `connect`, `ipAuthorize`, `rcon`, `disconnect`. An empty line is the
/// command `""`.
///
/// Original: `SV_ConnectionlessPacket` at 0x0045B430 (CoD2MP_s.exe 1.3)
/// Original: `SV_ConnectionlessPacket` at 0x004623C0 (CoDUOMP.exe 1.51)
pub fn server_request(
    game: Game,
    packet: &[u8],
    line: &mut Line,
    tokens: &mut Tokens,
) -> ServerRequest {
    if is_punkbuster(packet) {
        return ServerRequest::PunkBuster;
    }
    line.read(game, packet);
    tokens.tokenize(game, line.as_bytes(), 0);
    let command = tokens.argv(0);
    let limit = match game {
        Game::CallOfDuty2 => STRICMP_LIMIT_COD2,
        Game::UnitedOffensive => STRICMP_LIMIT_UO,
    };
    let is = |name: &[u8]| stricmpn(name, command, limit) == 0;
    let cod2 = game == Game::CallOfDuty2;
    if is(b"getstatus") {
        ServerRequest::Status
    } else if cod2 && is(b"v") {
        ServerRequest::Voice
    } else if is(b"getinfo") {
        ServerRequest::Info
    } else if is(b"getchallenge") {
        ServerRequest::Challenge
    } else if is(b"connect") {
        ServerRequest::Connect
    } else if is(b"ipAuthorize") {
        ServerRequest::IpAuthorize
    } else if is(b"rcon") {
        ServerRequest::RemoteCommand
    } else if !cod2 && is(b"disconnect") {
        ServerRequest::Ignored
    } else {
        ServerRequest::Unknown
    }
}

// --- getinfo and getstatus ------------------------------------------------------

/// The longest `infoResponse` text Call of Duty 2 sends: `SVC_Info` builds
/// it in a 1024-byte buffer, so `infoResponse\n` and the info string are
/// cut to 1023 bytes. (United Offensive's can't be longer than 13 + 1023.)
pub const MAX_INFO_RESPONSE_COD2: usize = 1023;

/// The longest `statusResponse` text Call of Duty 2 sends: `SVC_Status`
/// formats it with `Com_sprintf` into 0x2000 bytes, so it is cut to 8191.
/// United Offensive formats it in `NET_OutOfBandPrint` without a limit
/// (see [`MAX_PRINT_TEXT_UO`]).
pub const MAX_STATUS_RESPONSE_COD2: usize = 0x2000 - 1;

/// A client slot's state at which a player counts as on the server
/// (`CS_CONNECTED` and up: connected, primed, active).
pub const CLIENT_STATE_CONNECTED: i32 = 2;

/// A setting the server looks up by name (`Dvar_FindVar` in Call of Duty 2,
/// `Cvar_FindVar` in United Offensive): missing, a number (Call of Duty 2's
/// integer and enum dvars hold one) or text.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub enum Setting<'a> {
    #[default]
    Missing,
    Number(i32),
    Text(&'a [u8]),
}

/// The C runtime's `atol` (the games link the same one): skips white
/// space (space, `\t` to `\r`), takes one sign, then decimal digits, with
/// 32-bit wraparound; anything else ends the number. `text` ends at its
/// first zero byte.
///
/// Original: `atol` at 0x0057A751 (CoD2MP_s.exe 1.3)
/// Original: `atol` at 0x0056C695 (CoDUOMP.exe 1.51)
pub fn atol(text: &[u8]) -> i32 {
    let at = |i: usize| text.get(i).copied().unwrap_or(0);
    let mut i = 0;
    while matches!(at(i), b' ' | b'\t'..=b'\r') {
        i += 1;
    }
    let sign = at(i);
    if sign == b'-' || sign == b'+' {
        i += 1;
    }
    let mut total: i32 = 0;
    while at(i).is_ascii_digit() {
        total = total.wrapping_mul(10).wrapping_add(i32::from(at(i) - b'0'));
        i += 1;
    }
    if sign == b'-' {
        total.wrapping_neg()
    } else {
        total
    }
}

/// A number as `va("%i", n)` writes it, in a small stack buffer.
struct Decimal {
    digits: [u8; 12],
    start: usize,
}

impl Decimal {
    fn new(n: i32) -> Decimal {
        let mut digits = [0u8; 12];
        let mut start = digits.len();
        let mut rest = n.unsigned_abs();
        loop {
            start -= 1;
            digits[start] = b'0' + (rest % 10) as u8;
            rest /= 10;
            if rest == 0 {
                break;
            }
        }
        if n < 0 {
            start -= 1;
            digits[start] = b'-';
        }
        Decimal { digits, start }
    }

    fn as_bytes(&self) -> &[u8] {
        &self.digits[self.start..]
    }
}

/// An info string being built (`MAX_INFO_STRING` bytes, as the games' arrays).
struct InfoBuilder {
    text: [u8; MAX_LINE],
}

impl InfoBuilder {
    fn new() -> InfoBuilder {
        InfoBuilder {
            text: [0; MAX_LINE],
        }
    }

    /// `Info_SetValueForKey`: a refused pair (a value with `\`, `;` or `"`,
    /// or no room left) is left out, as the originals do after printing.
    fn set(&mut self, key: &[u8], value: &[u8]) {
        let _ = crate::shared::info::set_value_for_key(&mut self.text, key, value);
    }

    fn set_number(&mut self, key: &[u8], value: i32) {
        self.set(key, Decimal::new(value).as_bytes());
    }

    fn as_bytes(&self) -> &[u8] {
        crate::cstr::text(&self.text)
    }
}

/// What a server reports about itself in `infoResponse`, the values
/// `SVC_Info` reads (settings by their game names). Fields only one game
/// uses say so.
#[derive(Debug, Clone, Copy, Default)]
pub struct ServerInfo<'a> {
    /// The second word of the `getinfo` request (`Cmd_Argv(1)`), echoed
    /// back as `challenge` so the client can match the answer; empty when
    /// the request had one word.
    pub challenge: &'a [u8],
    /// `sv_hostname`.
    pub hostname: &'a [u8],
    /// `mapname`.
    pub mapname: &'a [u8],
    /// `g_gametype`.
    pub gametype: &'a [u8],
    /// `sv_maxclients`.
    pub max_clients: i32,
    /// `sv_privateClients`: the first slots, kept for password holders.
    pub private_clients: i32,
    /// Each client slot's state, slot 0 first (`client_t.state`; from
    /// [`CLIENT_STATE_CONNECTED`] up the slot counts as a player). Slots
    /// past the end of the slice count as empty.
    pub client_states: &'a [i32],
    /// `sv_pure` (Call of Duty 2 reads it as a true/false setting).
    pub pure: i32,
    /// Call of Duty 2 only: `fs_numServerIwds`, how many archives a pure
    /// server listed (non-zero only in a client that joined one); `pure`
    /// is reported when it or `sv_pure` is set.
    pub server_iwd_count: i32,
    /// `sv_minPing`, `sv_maxPing`.
    pub min_ping: i32,
    pub max_ping: i32,
    /// `fs_game` (the mod folder; empty when not set or missing).
    pub fs_game: &'a [u8],
    /// `sv_allowAnonymous` (a true/false setting in Call of Duty 2).
    pub allow_anonymous: i32,
    /// Call of Duty 2 only: `sv_disableClientConsole` (sent as `con_disabled`;
    /// United Offensive sends it in `statusResponse` instead).
    pub disable_client_console: i32,
    /// `g_password` (empty when not set or missing).
    pub password: &'a [u8],
    /// `scr_friendlyfire`, `scr_killcam` (game settings, looked up by name).
    pub friendly_fire: Setting<'a>,
    pub killcam: Setting<'a>,
    /// United Offensive only: `g_timeoutsallowed`, `scr_allow_jeeps`,
    /// `scr_allow_tanks`.
    pub timeouts_allowed: Setting<'a>,
    pub allow_jeeps: Setting<'a>,
    pub allow_tanks: Setting<'a>,
    /// Whether this is a dedicated server (`dedicated` set): `hw` is 2, else
    /// 5 (Call of Duty 2) or 4 (United Offensive).
    pub dedicated: bool,
    /// Whether the server runs anything but the game's own archives, see
    /// [`runs_mods`] (sent as `mod`).
    pub modded: bool,
    /// Call of Duty 2 only: `sv_voice`.
    pub voice: i32,
    /// `sv_punkbuster`.
    pub punkbuster: i32,
}

/// Counts the players in the client slots, as `SVC_Info` does: those of
/// the first `private_clients` slots, and those of all `max_clients`.
fn count_players(info: &ServerInfo) -> (i32, i32) {
    let connected = |slot: i32| {
        usize::try_from(slot)
            .ok()
            .and_then(|i| info.client_states.get(i))
            .is_some_and(|&state| state >= CLIENT_STATE_CONNECTED)
    };
    let private_count = (0..info.private_clients.max(0))
        .filter(|&i| connected(i))
        .count() as i32;
    let start = info.private_clients.max(0);
    let public_count = (start..info.max_clients).filter(|&i| connected(i)).count() as i32;
    (private_count, private_count + public_count)
}

/// A by-name number setting as Call of Duty 2's `SVC_Info` reads it: an
/// integer or enum dvar's number, otherwise `atol` of its text; 0 when the
/// setting is missing.
fn cod2_number(setting: Setting) -> i32 {
    match setting {
        Setting::Missing => 0,
        Setting::Number(n) => n,
        Setting::Text(text) => atol(text),
    }
}

/// United Offensive's by-name number settings: sent as their text when
/// `atoi` of it isn't 0 (a cvar always has text; a `Number` is taken as its
/// decimal text).
fn uo_flag(builder: &mut InfoBuilder, key: &[u8], setting: Setting) {
    match setting {
        Setting::Missing => {}
        Setting::Number(n) => {
            if n != 0 {
                builder.set_number(key, n);
            }
        }
        Setting::Text(text) => {
            if atol(text) != 0 {
                builder.set(key, text);
            }
        }
    }
}

/// Whether the server reports `mod 1` (in `infoResponse` and
/// `statusResponse`): always, unless `sv_pure` is on (Call of Duty 2 reads
/// it as a true/false setting, its low byte; United Offensive as a number)
/// and `fs_game` is
/// empty; then only if one of the archives clients must have
/// (`sv_referencedIwdNames` in Call of Duty 2, `sv_referencedPakNames` in
/// United Offensive, split into words) isn't one of the game's own.
///
/// `is_official` answers that for one archive name (Call of Duty 2's
/// `FS_iwIwd` at 0x0043B9A0: `main/iw_00` to `main/iw_24` and their
/// `localized_` versions; United Offensive's 0x004403A0, with
/// `fs_basegame`: `pak*`, `pakuo*`, `mp_pak*` ...). Those name checks
/// belong to the pure-server code and are not ported here; the checker
/// asks the game's own function. `tokens` is the word splitter the
/// original reuses (`Cmd_TokenizeString`).
///
/// Part of `SVC_Info` at 0x0045AE80 and `SVC_Status` at 0x0045A880
/// (CoD2MP_s.exe 1.3), 0x004616F0 and 0x00461190 (CoDUOMP.exe 1.51).
pub fn runs_mods(
    game: Game,
    pure: bool,
    fs_game: &[u8],
    referenced_archives: &[u8],
    tokens: &mut Tokens,
    mut is_official: impl FnMut(&[u8]) -> bool,
) -> bool {
    if !pure || !fs_game.is_empty() {
        return true;
    }
    if referenced_archives.is_empty() {
        return false;
    }
    tokens.tokenize(game, referenced_archives, 0);
    (0..tokens.argc()).any(|i| !is_official(tokens.argv(i)))
}

/// Writes the text of a server's `infoResponse` (what follows the four
/// 0xFF bytes: `infoResponse\n` and the info string) into `out`, and
/// returns its length. `out` must hold the answer (1036 bytes always do).
///
/// The keys, in order (each only when its condition holds; an empty value
/// leaves its key out):
///
/// * Call of Duty 2: `challenge`, `protocol` (118), `hostname`, `mapname`,
///   `clients` (players, if any), `sv_maxclients` (public slots plus
///   players in private ones, if more than 0), `gametype`, `pure` (`1`),
///   `minPing`, `maxPing` (if not 0), `game` (`fs_game`), `sv_allowAnonymous`,
///   `con_disabled` (if set), `pswrd` (`1` if there is a password), `ff`,
///   `kc` (if not 0), `hw` (2 or 5), `mod`, `voice`, `pb`. The whole text is
///   cut to [`MAX_INFO_RESPONSE_COD2`] bytes.
/// * United Offensive: `challenge`, `protocol` (22), `hostname`, `mapname`,
///   `clients`, `sv_maxclients`, `gametype`, `pure` (`sv_pure`'s number, if
///   not 0), `minPing`, `maxPing`, `game`, `sv_allowAnonymous`, `pswrd`,
///   `ff`, `kc`, `timeoutsAllowed`, `jps`, `tnk` (their text, if its number
///   isn't 0), `hw` (2 or 4), `pb`, `mod`.
///
/// Each pair goes through `Info_SetValueForKey`, so values lose `\`, `;`
/// and `"`, and a pair that doesn't fit in 1023 characters is left out.
///
/// Original: `SVC_Info` at 0x0045AE80 (CoD2MP_s.exe 1.3)
/// Original: `SVC_Info` at 0x004616F0 (CoDUOMP.exe 1.51)
pub fn info_response(game: Game, info: &ServerInfo, out: &mut [u8]) -> usize {
    let cod2 = game == Game::CallOfDuty2;
    let (private_players, players) = count_players(info);
    let mut b = InfoBuilder::new();
    b.set(b"challenge", info.challenge);
    b.set_number(b"protocol", if cod2 { 118 } else { 22 });
    b.set(b"hostname", info.hostname);
    b.set(b"mapname", info.mapname);
    if players != 0 {
        b.set_number(b"clients", players);
    }
    let slots = info
        .max_clients
        .wrapping_sub(info.private_clients)
        .wrapping_add(private_players);
    if slots > 0 {
        b.set_number(b"sv_maxclients", slots);
    }
    b.set(b"gametype", info.gametype);
    if cod2 {
        if info.pure as u8 != 0 || info.server_iwd_count != 0 {
            b.set(b"pure", b"1");
        }
    } else if info.pure != 0 {
        b.set_number(b"pure", info.pure);
    }
    if info.min_ping != 0 {
        b.set_number(b"minPing", info.min_ping);
    }
    if info.max_ping != 0 {
        b.set_number(b"maxPing", info.max_ping);
    }
    if !info.fs_game.is_empty() {
        b.set(b"game", info.fs_game);
    }
    if cod2 {
        let anonymous = info.allow_anonymous as u8;
        if anonymous != 0 {
            b.set_number(b"sv_allowAnonymous", anonymous.into());
        }
        let console = info.disable_client_console as u8;
        if console != 0 {
            b.set_number(b"con_disabled", console.into());
        }
    } else if info.allow_anonymous != 0 {
        b.set_number(b"sv_allowAnonymous", info.allow_anonymous);
    }
    if !info.password.is_empty() {
        b.set(b"pswrd", b"1");
    }
    if cod2 {
        for (key, setting) in [(&b"ff"[..], info.friendly_fire), (b"kc", info.killcam)] {
            let value = cod2_number(setting);
            if value != 0 {
                b.set_number(key, value);
            }
        }
        b.set_number(b"hw", if info.dedicated { 2 } else { 5 });
        b.set_number(b"mod", info.modded.into());
        b.set_number(b"voice", i32::from(info.voice as u8));
        b.set_number(b"pb", info.punkbuster);
    } else {
        uo_flag(&mut b, b"ff", info.friendly_fire);
        uo_flag(&mut b, b"kc", info.killcam);
        uo_flag(&mut b, b"timeoutsAllowed", info.timeouts_allowed);
        uo_flag(&mut b, b"jps", info.allow_jeeps);
        uo_flag(&mut b, b"tnk", info.allow_tanks);
        b.set_number(b"hw", if info.dedicated { 2 } else { 4 });
        b.set_number(b"pb", info.punkbuster);
        b.set_number(b"mod", info.modded.into());
    }
    let limit = if cod2 {
        MAX_INFO_RESPONSE_COD2
    } else {
        usize::MAX
    };
    let mut text = Writer::new(out, limit);
    text.put(b"infoResponse\n");
    text.put(b.as_bytes());
    text.len
}

/// Appends bytes to `out`, up to `limit` (the rest is cut, as the
/// originals' fixed buffers cut it) and `out`'s end.
struct Writer<'o> {
    out: &'o mut [u8],
    len: usize,
    limit: usize,
}

impl<'o> Writer<'o> {
    fn new(out: &'o mut [u8], limit: usize) -> Writer<'o> {
        let limit = limit.min(out.len());
        Writer { out, len: 0, limit }
    }

    fn put(&mut self, bytes: &[u8]) {
        let n = bytes.len().min(self.limit - self.len);
        self.out[self.len..self.len + n].copy_from_slice(&bytes[..n]);
        self.len += n;
    }
}

/// One player in a `statusResponse`.
#[derive(Debug, Clone, Copy, Default)]
pub struct PlayerStatus<'a> {
    /// The score the game reports (Call of Duty 2 reads the game's client
    /// structure, United Offensive asks the game module; 0 when no game
    /// runs).
    pub score: i32,
    /// `client_t.ping`.
    pub ping: i32,
    /// `client_t.name`.
    pub name: &'a [u8],
}

/// What a server reports in `statusResponse`, the values `SVC_Status`
/// reads.
#[derive(Debug, Clone, Copy, Default)]
pub struct ServerStatus<'a> {
    /// The server's info string: every setting marked as server info
    /// (`Dvar_InfoString`/`Cvar_InfoString`), at most 1023 characters.
    pub server_info: &'a [u8],
    /// The second word of the `getstatus` request (`Cmd_Argv(1)`), or empty.
    pub challenge: &'a [u8],
    /// `fs_restrict` (a demo version's limit): `sv_keywords` then starts
    /// with `demo `.
    pub restricted: bool,
    /// The players: the connected client slots
    /// ([`CLIENT_STATE_CONNECTED`] and up), in slot order.
    pub players: &'a [PlayerStatus<'a>],
    /// United Offensive only: `sv_disableClientConsole` (`con_disabled`,
    /// if not 0).
    pub disable_client_console: i32,
    /// `g_password` (empty when not set or missing): `pswrd` 1 or 0.
    pub password: &'a [u8],
    /// See [`runs_mods`].
    pub modded: bool,
}

/// Writes the text of a server's `statusResponse` into `out` and returns
/// its length: `statusResponse\n`, the server info with `challenge` and
/// the game's keys added, `\n`, then one line `<score> <ping> "<name>"\n`
/// per player. `out` should hold [`MAX_PRINT_TEXT_UO`] bytes (United
/// Offensive's longest; Call of Duty 2's answer is cut to
/// [`MAX_STATUS_RESPONSE_COD2`]); a longer answer is cut at `out`'s end.
///
/// The info keys added, in order: `challenge`; `sv_keywords` (`demo ` and
/// its old value) when restricted; United Offensive's `con_disabled`;
/// `pswrd`; `mod`. A player line is cut to 1023 characters, and the player
/// list stops before the line that would make it 0x20000 (Call of Duty 2)
/// or 0x8000 (United Offensive) bytes or more.
///
/// Original: `SVC_Status` at 0x0045A880 (CoD2MP_s.exe 1.3)
/// Original: `SVC_Status` at 0x00461190 (CoDUOMP.exe 1.51)
pub fn status_response(game: Game, status: &ServerStatus, out: &mut [u8]) -> usize {
    let cod2 = game == Game::CallOfDuty2;
    let mut b = InfoBuilder::new();
    let base = &status.server_info[..status.server_info.len().min(MAX_LINE - 1)];
    b.text[..base.len()].copy_from_slice(base);
    b.set(b"challenge", status.challenge);
    if status.restricted {
        let mut keywords = [0u8; MAX_LINE];
        let old = crate::shared::info::value_for_key(b.as_bytes(), b"sv_keywords").unwrap_or(b"");
        let mut w = Writer::new(&mut keywords, MAX_LINE - 1);
        w.put(b"demo ");
        w.put(old);
        let n = w.len;
        b.set(b"sv_keywords", &keywords[..n]);
    }
    if !cod2 && status.disable_client_console != 0 {
        b.set_number(b"con_disabled", status.disable_client_console);
    }
    b.set(
        b"pswrd",
        if status.password.is_empty() {
            b"0"
        } else {
            b"1"
        },
    );
    b.set_number(b"mod", status.modded.into());

    let limit = if cod2 {
        MAX_STATUS_RESPONSE_COD2
    } else {
        usize::MAX
    };
    let mut text = Writer::new(out, limit);
    text.put(b"statusResponse\n");
    text.put(b.as_bytes());
    text.put(b"\n");
    let list_limit = if cod2 { 0x20000 } else { 0x8000 };
    let mut list_length = 0;
    for player in status.players {
        let mut line = [0u8; MAX_LINE];
        let mut w = Writer::new(&mut line, MAX_LINE - 1);
        w.put(Decimal::new(player.score).as_bytes());
        w.put(b" ");
        w.put(Decimal::new(player.ping).as_bytes());
        w.put(b" \"");
        w.put(crate::cstr::text(player.name));
        w.put(b"\"\n");
        let n = w.len;
        if list_length + n >= list_limit {
            break;
        }
        list_length += n;
        text.put(&line[..n]);
    }
    text.len
}

// --- getchallenge -----------------------------------------------------------------

/// How many challenges a server remembers (`MAX_CHALLENGES`); the oldest
/// is reused when a new address asks.
pub const MAX_CHALLENGES: usize = 1024;

/// Whether two addresses are the same for the games' `NET_CompareAdr`
/// (which returns 0 for "the same"): same type, and for an IPv4 or IPX
/// address the same port and address bytes. Two loopback addresses are
/// always the same, and so are two of any other type (bot, bad, broadcast:
/// the original prints "NET_CompareAdrSigned: bad address type" and says
/// equal).
///
/// Original: `NET_CompareAdrSigned` at 0x00448650 (CoD2MP_s.exe 1.3)
/// Original: `NET_CompareAdrSigned` at 0x0044DEC0 (CoDUOMP.exe 1.51)
pub fn same_address(a: &NetAddress, b: &NetAddress) -> bool {
    if a.kind != b.kind {
        return false;
    }
    match a.kind {
        AddressType::Ip => a.port == b.port && a.ip == b.ip,
        AddressType::Ipx => a.port == b.port && a.ipx == b.ipx,
        _ => true,
    }
}

/// One remembered challenge, the games' `challenge_t`.
#[derive(Debug, Clone, Copy)]
pub struct ChallengeSlot {
    pub address: NetAddress,
    /// The number the client must send back in its `connect`.
    pub challenge: i32,
    /// When it was last given out (`svs.time`): the oldest slot is reused.
    pub time: i32,
    /// When `challengeResponse` was sent.
    pub ping_time: i32,
    /// When the address first asked.
    pub first_time: i32,
    pub first_ping: i32,
    /// Whether a client connected with it (`SV_DirectConnect` sets it).
    pub connected: bool,
    /// Call of Duty 2 only: the 32-character hash a client sends in
    /// `getchallenge 0 "<hash>"`, kept for PunkBuster (with its terminator).
    pub punkbuster_guid: [u8; 33],
}

/// The challenges a server remembers (`svs.challenges`): allocated once.
pub struct ChallengeTable {
    pub slots: Box<[ChallengeSlot]>,
}

impl Default for ChallengeSlot {
    fn default() -> Self {
        ChallengeSlot {
            address: NetAddress::default(),
            challenge: 0,
            time: 0,
            ping_time: 0,
            first_time: 0,
            first_ping: 0,
            connected: false,
            punkbuster_guid: [0; 33],
        }
    }
}

impl Default for ChallengeTable {
    fn default() -> Self {
        ChallengeTable {
            slots: vec![ChallengeSlot::default(); MAX_CHALLENGES].into_boxed_slice(),
        }
    }
}

impl ChallengeTable {
    /// The slot for `from`: the first one not yet connected that holds its
    /// address; or, when there is none, the slot given out longest ago (the
    /// first of equals; slots after a match aren't looked at) is started
    /// over for `from`: a new challenge number (`new_challenge`, see
    /// [`challenge_number`]), its times set to `time`, not connected.
    ///
    /// Part of `SV_GetChallenge` at 0x00452DE0 (CoD2MP_s.exe 1.3) and
    /// 0x00458C50 (CoDUOMP.exe 1.51).
    pub fn find_or_add(
        &mut self,
        from: &NetAddress,
        time: i32,
        new_challenge: impl FnOnce() -> i32,
    ) -> usize {
        let mut oldest = 0;
        let mut oldest_time = i32::MAX;
        for (i, slot) in self.slots.iter().enumerate() {
            if !slot.connected && same_address(from, &slot.address) {
                return i;
            }
            if slot.time < oldest_time {
                oldest_time = slot.time;
                oldest = i;
            }
        }
        let slot = &mut self.slots[oldest];
        slot.challenge = new_challenge();
        slot.address = *from;
        slot.first_time = time;
        slot.first_ping = 0;
        slot.time = time;
        slot.connected = false;
        oldest
    }
}

/// The challenge number a server makes up: `((rand() << 16) ^ rand()) ^
/// clock`, `rand` being the C runtime's (`first` and `second` are two
/// calls of it). The clock is `svs.time` in Call of Duty 2; United
/// Offensive uses another server clock (at 0x04907BC4, advanced with
/// `svs.time` by every server frame). Any number does: the client only
/// sends it back.
pub fn challenge_number(first: i32, second: i32, clock: i32) -> i32 {
    (first.wrapping_shl(16) ^ second) ^ clock
}

/// What `SV_GetChallenge` needs to know besides the table.
#[derive(Debug, Clone, Copy, Default)]
pub struct ChallengeRequest<'a> {
    pub from: NetAddress,
    /// `svs.time`.
    pub time: i32,
    /// `net_lanauthorize`: check LAN players' keys too.
    pub lan_authorize: bool,
    /// Whether `from` is on the LAN (`Sys_IsLANAddress`: loopback, private
    /// and link-local ranges, the machine's own subnets).
    pub from_lan: bool,
    /// Whether the authorize server's address is known (resolved now or
    /// before); when it can't be resolved nothing is sent.
    pub authorize_known: bool,
    /// Call of Duty 2 only: when the master server last talked to this
    /// server (`SV_UpdateLastTimeMasterServerCommunicated`).
    pub last_master_contact: i32,
    /// Whether `from` is the master server's address.
    pub from_master: bool,
    /// Call of Duty 2 only: the request's third word when it has exactly
    /// three (`getchallenge 0 "<hash>"`), else `None`.
    pub punkbuster_guid: Option<&'a [u8]>,
}

/// What a server does about a `getchallenge`.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ChallengeAnswer {
    /// Send `challengeResponse` with the slot's number now (see
    /// [`challenge_response_text`]): to a LAN player, or after the
    /// authorize server stayed silent too long.
    Respond { slot: usize },
    /// The authorize server's address isn't known: nothing is sent.
    AuthorizeUnknown,
    /// Ask the authorize server about the player (`SV_AuthorizeRequest`,
    /// `getIpAuthorize ...`) and answer when it does. A Rust server never
    /// does this (see `docs/PC_MULTIPLAYER.md` section 4).
    Authorize { slot: usize },
}

/// Decides how a server answers `getchallenge` and keeps the table up to
/// date (the slot, its `ping_time` when answering now, Call of Duty 2's
/// PunkBuster hash):
///
/// 1. the slot for the address ([`ChallengeTable::find_or_add`]);
/// 2. a LAN player (unless `net_lanauthorize`) is answered at once;
/// 3. otherwise the authorize server's address must be known;
/// 4. if the player has been asking for more than 7 seconds (Call of Duty
///    2: and the master server hasn't been heard from for 20 minutes) or 5
///    seconds (United Offensive), and isn't the master server, it is
///    answered anyway ("authorize server timed out");
/// 5. otherwise Call of Duty 2 keeps the PunkBuster hash and the authorize
///    server is asked.
///
/// Original: `SV_GetChallenge` at 0x00452DE0 (CoD2MP_s.exe 1.3)
/// Original: `SV_GetChallenge` at 0x00458C50 (CoDUOMP.exe 1.51)
pub fn get_challenge(
    game: Game,
    table: &mut ChallengeTable,
    request: &ChallengeRequest,
    new_challenge: impl FnOnce() -> i32,
) -> ChallengeAnswer {
    let slot = table.find_or_add(&request.from, request.time, new_challenge);
    if !request.lan_authorize && request.from_lan {
        table.slots[slot].ping_time = request.time;
        return ChallengeAnswer::Respond { slot };
    }
    if !request.authorize_known {
        return ChallengeAnswer::AuthorizeUnknown;
    }
    let asking_for = request.time.wrapping_sub(table.slots[slot].first_time);
    let timed_out = match game {
        Game::CallOfDuty2 => {
            request.time.wrapping_sub(request.last_master_contact) > 1_200_000 && asking_for > 7000
        }
        Game::UnitedOffensive => asking_for > 5000,
    };
    if timed_out && !request.from_master {
        table.slots[slot].ping_time = request.time;
        return ChallengeAnswer::Respond { slot };
    }
    if game == Game::CallOfDuty2 {
        if let Some(guid) = request.punkbuster_guid {
            // strncpy 32 characters, then a terminator.
            let guid_slot = &mut table.slots[slot].punkbuster_guid;
            crate::crt::strncpy(&mut guid_slot[..32], guid);
            guid_slot[32] = 0;
        }
    }
    ChallengeAnswer::Authorize { slot }
}

/// The text of the `challengeResponse` packet: `challengeResponse <n>`, and
/// in United Offensive `challengeResponse <n> <m>` when
/// `sv_onlyVisibleClients` (`m`) isn't 0. Returns its length in `out` (32
/// bytes always hold it).
pub fn challenge_response_text(
    game: Game,
    challenge: i32,
    only_visible_clients: i32,
    out: &mut [u8],
) -> usize {
    let mut w = Writer::new(out, usize::MAX);
    w.put(b"challengeResponse ");
    w.put(Decimal::new(challenge).as_bytes());
    if game == Game::UnitedOffensive && only_visible_clients != 0 {
        w.put(b" ");
        w.put(Decimal::new(only_visible_clients).as_bytes());
    }
    w.len
}

// --- connect ------------------------------------------------------------------------

/// Whether two addresses are the same machine for the games'
/// `NET_CompareBaseAdr` (0 for "the same"): same type and address bytes,
/// the port aside, except that bots, and in Call of Duty 2 loopback
/// clients, are told apart by their port; two loopback addresses are
/// always the same in United Offensive, and two of any other type are the
/// same.
///
/// Original: `NET_CompareBaseAdrSigned` at 0x004485B0 (CoD2MP_s.exe 1.3)
/// Original: `NET_CompareBaseAdrSigned` at 0x0044DD50 (CoDUOMP.exe 1.51)
pub fn same_base_address(game: Game, a: &NetAddress, b: &NetAddress) -> bool {
    if a.kind != b.kind {
        return false;
    }
    match a.kind {
        AddressType::Loopback => game == Game::UnitedOffensive || a.port == b.port,
        AddressType::Bot => a.port == b.port,
        AddressType::Ip => a.ip == b.ip,
        AddressType::Ipx => a.ipx == b.ipx,
        _ => true,
    }
}

/// What `SV_DirectConnect` reads of a client slot.
#[derive(Debug, Clone, Copy, Default)]
pub struct ClientSlotInfo {
    /// `client_t.state` (0 free, 1 zombie, 2 connected and up).
    pub state: i32,
    /// `netchan.remoteAddress`.
    pub address: NetAddress,
    /// `netchan.qport`.
    pub qport: i32,
    /// `lastConnectTime`.
    pub last_connect_time: i32,
}

/// What `SV_DirectConnect` needs to know besides the challenge table.
#[derive(Debug, Clone, Copy, Default)]
pub struct ConnectRequest<'a> {
    pub from: NetAddress,
    /// The packet's userinfo (`Cmd_Argv(1)`, the `connect` line's second
    /// word); only its first 1023 characters count.
    pub userinfo: &'a [u8],
    /// `svs.time`.
    pub time: i32,
    /// `sv_reconnectlimit` (seconds).
    pub reconnect_limit: i32,
    /// `sv_maxclients`, `sv_privateClients`, `sv_privatePassword`.
    pub max_clients: i32,
    pub private_clients: i32,
    pub private_password: &'a [u8],
    /// `sv_minPing`, `sv_maxPing` (0 = no limit): Call of Duty 2 compares
    /// their integer, United Offensive their float value.
    pub min_ping: f64,
    pub max_ping: f64,
    /// Whether `from` is on the LAN (`Sys_IsLANAddress`; no ping limits).
    pub from_lan: bool,
    /// The client slots (`svs.clients`, `sv_maxclients` of them; missing
    /// ones count as free).
    pub clients: &'a [ClientSlotInfo],
}

/// What a server does with a `connect`.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ConnectDecision {
    /// The userinfo's `protocol` isn't the server's (118, 22).
    WrongProtocol,
    /// A client from the same machine and qport (or port) connected less
    /// than `sv_reconnectlimit` seconds ago: ignored, no answer.
    ReconnectTooSoon,
    /// No challenge given to this address has this number.
    BadChallenge,
    /// The player's challenge ping is under `sv_minPing` (the server only
    /// takes high pings).
    PingTooLow,
    /// It is over `sv_maxPing`.
    PingTooHigh,
    /// The same player (same machine, same qport or port) takes its old
    /// slot back; if it was connected (`state` 2 and up) the server drops
    /// it first (`SV_FreeClient`).
    Reconnect {
        slot: usize,
        challenge: Option<usize>,
        free_first: bool,
    },
    /// A free slot (from slot 0 with the private password, else after the
    /// private slots).
    NewSlot {
        slot: usize,
        challenge: Option<usize>,
    },
    /// No free slot.
    ServerFull,
}

impl ConnectDecision {
    /// The text of the answer the server sends for a refusal (the client
    /// shows the localized `EXE_` message); `None` when nothing is sent
    /// now. A slot is answered by the game (`ClientConnect`): `connectResponse`,
    /// or `error\n<the game's reason>`.
    pub fn reply(self, game: Game) -> Option<&'static [u8]> {
        Some(match self {
            ConnectDecision::WrongProtocol => match game {
                Game::CallOfDuty2 => b"error\nEXE_SERVER_IS_DIFFERENT_VER\x151.3\n",
                Game::UnitedOffensive => b"error\nEXE_SERVER_IS_DIFFERENT_VER\x151.51\n",
            },
            ConnectDecision::BadChallenge => b"error\nEXE_BAD_CHALLENGE",
            ConnectDecision::PingTooLow => b"error\nEXE_ERR_HIGH_PING_ONLY",
            ConnectDecision::PingTooHigh => b"error\nEXE_ERR_LOW_PING_ONLY",
            ConnectDecision::ServerFull => b"error\nEXE_SERVERISFULL",
            _ => return None,
        })
    }
}

/// The protocol number a server takes: 118 (Call of Duty 2 1.3), 22
/// (United Offensive 1.51).
pub fn protocol_version(game: Game) -> i32 {
    match game {
        Game::CallOfDuty2 => 118,
        Game::UnitedOffensive => 22,
    }
}

/// Decides what a server does with a `connect`, in the originals' order:
///
/// 1. the userinfo's `protocol` must be the server's;
/// 2. a reconnect from the same machine and qport (or port: any slot,
///    whatever its state) within `sv_reconnectlimit` seconds is ignored;
/// 3. except for loopback and bot addresses, the address and the
///    userinfo's `challenge` must match a remembered challenge: its first
///    ping (`svs.time - pingTime`) is kept, it is marked connected, and
///    off the LAN the ping limits apply;
/// 4. (PunkBuster's check, when it runs);
/// 5. the same player in a used slot reconnects into it;
/// 6. otherwise the first free slot, from 0 with the right `password`
///    (`sv_privatePassword`, exact case), else from `sv_privateClients`.
///
/// Changes only the challenge table. The originals then set up the slot
/// and ask the game (`ClientConnect`); the identity number the client gets
/// is the challenge slot's (0 when there is none or it was never checked:
/// "Connecting player #%i has a zero GUID", accepted).
///
/// Original: `SV_DirectConnect` at 0x00453C20 (CoD2MP_s.exe 1.3)
/// Original: `SV_DirectConnect` at 0x00459B20 (CoDUOMP.exe 1.51)
pub fn direct_connect(
    game: Game,
    table: &mut ChallengeTable,
    request: &ConnectRequest,
) -> ConnectDecision {
    let userinfo = &request.userinfo[..request.userinfo.len().min(MAX_LINE - 1)];
    let userinfo = crate::cstr::text(userinfo);
    let value = |key: &[u8]| crate::shared::info::value_for_key(userinfo, key).unwrap_or(b"");
    if atol(value(b"protocol")) != protocol_version(game) {
        return ConnectDecision::WrongProtocol;
    }
    let challenge = atol(value(b"challenge"));
    let qport = atol(value(b"qport"));
    let from = &request.from;
    let slots = usize::try_from(request.max_clients).unwrap_or(0);
    let client = |i: usize| request.clients.get(i).copied().unwrap_or_default();
    let same_player = |cl: &ClientSlotInfo| {
        same_base_address(game, from, &cl.address)
            && (cl.qport == qport || cl.address.port == from.port)
    };
    for i in 0..slots {
        let cl = client(i);
        if same_player(&cl) {
            let since = request.time.wrapping_sub(cl.last_connect_time);
            if since < request.reconnect_limit.wrapping_mul(1000) {
                return ConnectDecision::ReconnectTooSoon;
            }
            break;
        }
    }
    let mut challenge_slot = None;
    if !matches!(from.kind, AddressType::Loopback | AddressType::Bot) {
        let Some(found) = table
            .slots
            .iter()
            .position(|ch| same_address(from, &ch.address) && ch.challenge == challenge)
        else {
            return ConnectDecision::BadChallenge;
        };
        challenge_slot = Some(found);
        let ch = &mut table.slots[found];
        let mut ping = ch.first_ping;
        if ping == 0 {
            ping = request.time.wrapping_sub(ch.ping_time);
            ch.first_ping = ping;
        }
        ch.connected = true;
        if !request.from_lan {
            let ping = f64::from(ping);
            if request.min_ping != 0.0 && ping < request.min_ping {
                return ConnectDecision::PingTooLow;
            }
            if request.max_ping != 0.0 && request.max_ping < ping {
                return ConnectDecision::PingTooHigh;
            }
        }
    }
    for i in 0..slots {
        let cl = client(i);
        if cl.state != 0 && same_player(&cl) {
            return ConnectDecision::Reconnect {
                slot: i,
                challenge: challenge_slot,
                free_first: cl.state >= CLIENT_STATE_CONNECTED,
            };
        }
    }
    let start = if value(b"password") == crate::cstr::text(request.private_password) {
        0
    } else {
        request.private_clients
    };
    for i in start.max(0)..request.max_clients {
        if client(i as usize).state == 0 {
            return ConnectDecision::NewSlot {
                slot: i as usize,
                challenge: challenge_slot,
            };
        }
    }
    ConnectDecision::ServerFull
}

// --- the client side: infoResponse ---------------------------------------------------

/// Whether a client lists the server that sent this `infoResponse` (the
/// info string after the `infoResponse` line, as `MSG_ReadString` reads
/// it): its `protocol` must be the client's (118, 22), or the
/// `debug_protocol` setting's when that is set: Call of Duty 2 compares
/// `atol` of its text, United Offensive its float value with the number.
/// (`debug_protocol_text` is empty when the setting is missing or empty.)
///
/// This is `CL_ServerInfoPacket`'s first step; its rest keeps the client's
/// ping and server lists (Call of Duty 2 adds `nettype` to the info first),
/// not ported here.
///
/// Original: `CL_ServerInfoPacket` at 0x004B34E0 (CoD2MP_s.exe 1.3)
/// Original: `CL_ServerInfoPacket` at 0x004155F0 (CoDUOMP.exe 1.51)
pub fn info_response_accepted(
    game: Game,
    info: &[u8],
    debug_protocol_text: &[u8],
    debug_protocol_value: f32,
) -> bool {
    let protocol = atol(crate::shared::info::value_for_key(info, b"protocol").unwrap_or(b""));
    let debug = crate::cstr::text(debug_protocol_text);
    match game {
        Game::CallOfDuty2 => {
            let expected = if debug.is_empty() { 118 } else { atol(debug) };
            protocol == expected
        }
        Game::UnitedOffensive => {
            let expected = if debug.is_empty() {
                22.0
            } else {
                f64::from(debug_protocol_value)
            };
            f64::from(protocol) == expected
        }
    }
}

/// One server in a client's server list (`serverInfo_t`): what the server
/// browser shows, read from the server's info string.
#[derive(Debug, Clone, Copy, Default)]
pub struct ServerListEntry {
    /// `nettype` (added by the client: the address type).
    pub net_type: u8,
    pub clients: u8,
    /// `sv_maxclients`.
    pub max_clients: u8,
    pub allow_anonymous: u8,
    /// `pswrd`.
    pub password: u8,
    pub pure: u8,
    pub friendly_fire: u8,
    pub killcam: u8,
    pub console_disabled: u8,
    /// `hw`: 2 dedicated server, else a listen server.
    pub hardware: u8,
    /// `mod`.
    pub modded: u8,
    /// Call of Duty 2 only.
    pub voice: u8,
    pub punkbuster: u8,
    pub min_ping: i16,
    pub max_ping: i16,
    /// The ping the client measured.
    pub ping: i16,
    /// United Offensive only: `timeoutsallowed`, `jps`, `tnk`.
    pub timeouts_allowed: i16,
    pub jeeps: i16,
    pub tanks: i16,
    pub hostname: [u8; 32],
    pub mapname: [u8; 32],
    pub game: [u8; 24],
    pub gametype: [u8; 16],
}

/// Fills a server list entry from a server's info string (`None`: only the
/// ping changes). Every number is `atol` of its key's value (keys are found
/// whatever their case), cut to the field's size; the texts are cut to
/// their field (31, 31, 23, 15 characters) and zero-padded.
///
/// Original: `CL_SetServerInfo` at 0x004B3300 (CoD2MP_s.exe 1.3)
/// Original: `CL_SetServerInfo` at 0x00415180 (CoDUOMP.exe 1.51)
pub fn set_server_info(game: Game, entry: &mut ServerListEntry, info: Option<&[u8]>, ping: i16) {
    if let Some(info) = info {
        let value = |key: &[u8]| crate::shared::info::value_for_key(info, key).unwrap_or(b"");
        let number = |key: &[u8]| atol(value(key));
        let text = |field: &mut [u8], key: &[u8]| {
            let last = field.len() - 1;
            crate::crt::strncpy(&mut field[..last], value(key));
            field[last] = 0;
        };
        entry.clients = number(b"clients") as u8;
        text(&mut entry.hostname, b"hostname");
        text(&mut entry.mapname, b"mapname");
        entry.max_clients = number(b"sv_maxclients") as u8;
        text(&mut entry.game, b"game");
        text(&mut entry.gametype, b"gametype");
        entry.net_type = number(b"nettype") as u8;
        entry.min_ping = number(b"minping") as i16;
        entry.max_ping = number(b"maxping") as i16;
        entry.allow_anonymous = number(b"sv_allowAnonymous") as u8;
        entry.console_disabled = number(b"con_disabled") as u8;
        entry.password = number(b"pswrd") as u8;
        entry.pure = number(b"pure") as u8;
        entry.friendly_fire = number(b"ff") as u8;
        entry.killcam = number(b"kc") as u8;
        entry.hardware = number(b"hw") as u8;
        entry.modded = number(b"mod") as u8;
        match game {
            Game::CallOfDuty2 => entry.voice = number(b"voice") as u8,
            Game::UnitedOffensive => {
                entry.timeouts_allowed = number(b"timeoutsallowed") as i16;
                entry.jeeps = number(b"jps") as i16;
                entry.tanks = number(b"tnk") as i16;
            }
        }
        entry.punkbuster = number(b"pb") as u8;
    }
    entry.ping = ping;
}

#[cfg(test)]
mod tests {
    use super::*;

    fn words(game: Game, line: &[u8], limit: u32) -> Vec<Vec<u8>> {
        let mut tokens = Box::<Tokens>::default();
        tokens.tokenize(game, line, limit);
        (0..tokens.argc())
            .map(|i| tokens.argv(i).to_vec())
            .collect()
    }

    #[test]
    fn reads_the_first_line() {
        let mut line = Line::default();
        line.read(
            Game::CallOfDuty2,
            b"\xff\xff\xff\xffgetinfo 100%\x92\xe9\nmore",
        );
        assert_eq!(line.as_bytes(), b"getinfo 100.'\xe9");
        line.read(
            Game::UnitedOffensive,
            b"\xff\xff\xff\xffgetinfo 100%\x92\xe9\nmore",
        );
        assert_eq!(line.as_bytes(), b"getinfo 100.'.");
        line.read(Game::CallOfDuty2, &[0xFF; 3]);
        assert_eq!(line.as_bytes(), b"");
        let long = [b"\xff\xff\xff\xff".as_slice(), &[b'a'; 2000]].concat();
        line.read(Game::CallOfDuty2, &long);
        assert_eq!(line.as_bytes().len(), 1023);
        assert!(is_connectionless(b"\xff\xff\xff\xff"));
        assert!(!is_connectionless(b"\xff\xff\xff"));
    }

    #[test]
    fn splits_like_the_console() {
        let cod2 = Game::CallOfDuty2;
        let uo = Game::UnitedOffensive;
        assert_eq!(
            words(cod2, b"getinfo xxx", 0),
            [b"getinfo".to_vec(), b"xxx".to_vec()]
        );
        assert_eq!(
            words(cod2, b"connect \"\\name\\Price\\rate\\25000\"", 0),
            [b"connect".to_vec(), b"\\name\\Price\\rate\\25000".to_vec()]
        );
        assert_eq!(words(cod2, b"a//b c", 0), [b"a".to_vec()]);
        assert_eq!(
            words(cod2, b"a /* b */ c", 0),
            [b"a".to_vec(), b"c".to_vec()]
        );
        assert_eq!(words(cod2, b"a /*/ c", 0), [b"a".to_vec(), b"c".to_vec()]);
        assert_eq!(words(cod2, b"a\xe9b", 0), [b"a".to_vec(), b"b".to_vec()]);
        assert_eq!(words(cod2, b"\"x\\\"y\"", 0), [b"x\"y".to_vec()]);
        assert_eq!(words(uo, b"\"x\\\"y\"", 0)[0], b"x\\");
        assert_eq!(
            words(cod2, b"rcon pw  map  mp_toujane", 3),
            [
                b"rcon".to_vec(),
                b"pw".to_vec(),
                b" map  mp_toujane".to_vec()
            ]
        );
        assert_eq!(words(cod2, b"  all of it", 1), [b"  all of it".to_vec()]);
        assert_eq!(words(cod2, b"\"\"", 0), [b"".to_vec()]);
    }

    #[test]
    fn the_word_limit_differs() {
        let line: Vec<u8> = b"x ".repeat(513);
        assert_eq!(words(Game::CallOfDuty2, &line, 0).len(), 0);
        assert_eq!(words(Game::UnitedOffensive, &line, 0).len(), 512);
        // Exactly 512 words with nothing after them are kept by both.
        let line = &line[..512 * 2 - 1];
        assert_eq!(words(Game::CallOfDuty2, line, 0).len(), 512);
    }

    #[test]
    fn frames_and_dispatches() {
        let mut out = [0u8; 64];
        let n = out_of_band_print(Game::CallOfDuty2, b"getinfo xxx\0junk", &mut out).unwrap();
        assert_eq!(&out[..n], b"\xff\xff\xff\xffgetinfo xxx");
        let mut line = Line::default();
        let mut tokens = Box::<Tokens>::default();
        let request = |game, packet: &[u8], line: &mut Line, tokens: &mut Tokens| {
            server_request(game, packet, line, tokens)
        };
        let cod2 = Game::CallOfDuty2;
        let uo = Game::UnitedOffensive;
        assert_eq!(
            request(cod2, b"\xff\xff\xff\xffGETINFO 1", &mut line, &mut tokens),
            ServerRequest::Info
        );
        assert_eq!(
            request(cod2, b"\xff\xff\xff\xffv x", &mut line, &mut tokens),
            ServerRequest::Voice
        );
        assert_eq!(
            request(uo, b"\xff\xff\xff\xffv x", &mut line, &mut tokens),
            ServerRequest::Unknown
        );
        assert_eq!(
            request(uo, b"\xff\xff\xff\xffdisconnect", &mut line, &mut tokens),
            ServerRequest::Ignored
        );
        assert_eq!(
            request(cod2, b"\xff\xff\xff\xffPB_SV x", &mut line, &mut tokens),
            ServerRequest::PunkBuster
        );
        assert!(connect_is_compressed(
            uo,
            b"\xff\xff\xff\xffconnect \"\\x\""
        ));
        assert!(!connect_is_compressed(
            cod2,
            b"\xff\xff\xff\xffconnect \"\\x\""
        ));
        assert!(!connect_is_compressed(uo, b"\xff\xff\xff\xffConnect"));
    }

    #[test]
    fn answers_getinfo() {
        let states = [2, 0, 4, 1];
        let info = ServerInfo {
            challenge: b"xxx",
            hostname: b"Rust server",
            mapname: b"mp_toujane",
            gametype: b"tdm",
            max_clients: 4,
            private_clients: 1,
            client_states: &states,
            dedicated: true,
            ..ServerInfo::default()
        };
        let mut out = [0u8; 2048];
        let n = info_response(Game::CallOfDuty2, &info, &mut out);
        assert_eq!(
            &out[..n],
            &b"infoResponse\n\\challenge\\xxx\\protocol\\118\\hostname\\Rust server\\mapname\\mp_toujane\\clients\\2\\sv_maxclients\\4\\gametype\\tdm\\hw\\2\\mod\\0\\voice\\0\\pb\\0"[..]
        );
        let n = info_response(Game::UnitedOffensive, &info, &mut out);
        assert!(out[..n].ends_with(b"\\hw\\2\\pb\\0\\mod\\0"));
        assert_eq!(atol(b" \t-12x"), -12);
        assert_eq!(atol(b"99999999999"), 1_215_752_191); // wraps
    }

    #[test]
    fn challenges_and_connects() {
        let game = Game::CallOfDuty2;
        let mut table = ChallengeTable::default();
        for (i, slot) in table.slots.iter_mut().enumerate() {
            slot.time = 100 + i as i32;
        }
        let from = NetAddress::ip([192, 168, 0, 9], 28960);
        let request = ChallengeRequest {
            from,
            time: 5000,
            from_lan: true,
            ..ChallengeRequest::default()
        };
        let answer = get_challenge(game, &mut table, &request, || 777);
        assert_eq!(answer, ChallengeAnswer::Respond { slot: 0 });
        assert_eq!(
            (table.slots[0].challenge, table.slots[0].ping_time),
            (777, 5000)
        );
        let mut text = [0u8; 32];
        let n = challenge_response_text(game, 777, 1, &mut text);
        assert_eq!(&text[..n], b"challengeResponse 777");
        // The same address asks again: the same slot, the same number.
        assert_eq!(get_challenge(game, &mut table, &request, || 1), answer);

        let userinfo = b"\\name\\x\\protocol\\118\\challenge\\777\\qport\\5";
        let clients = [ClientSlotInfo::default(); 2];
        let connect = ConnectRequest {
            from,
            userinfo,
            time: 5050,
            max_clients: 2,
            from_lan: true,
            clients: &clients,
            ..ConnectRequest::default()
        };
        let decision = direct_connect(game, &mut table, &connect);
        assert_eq!(
            decision,
            ConnectDecision::NewSlot {
                slot: 0,
                challenge: Some(0)
            }
        );
        assert!(table.slots[0].connected);
        assert_eq!(table.slots[0].first_ping, 50);
        let wrong = ConnectRequest {
            userinfo: b"\\protocol\\22",
            ..connect
        };
        assert_eq!(
            direct_connect(game, &mut table, &wrong).reply(game),
            Some(&b"error\nEXE_SERVER_IS_DIFFERENT_VER\x151.3\n"[..])
        );
    }

    #[test]
    fn reads_inforesponse() {
        let info = b"\\protocol\\118\\hostname\\a long host name that does not fit\\clients\\3\\minping\\-5";
        assert!(info_response_accepted(Game::CallOfDuty2, info, b"", 0.0));
        assert!(!info_response_accepted(
            Game::UnitedOffensive,
            info,
            b"",
            0.0
        ));
        assert!(info_response_accepted(
            Game::UnitedOffensive,
            info,
            b"118",
            118.0
        ));
        let mut entry = ServerListEntry::default();
        set_server_info(Game::CallOfDuty2, &mut entry, Some(info), 40);
        assert_eq!(&entry.hostname[..], b"a long host name that does not \0");
        assert_eq!((entry.clients, entry.min_ping, entry.ping), (3, -5, 40));
    }
}
