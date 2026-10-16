//! The message buffer, the games' `msg_t`: what every packet of a
//! connection is written into and read from, bit for bit.
//!
//! Single bits and whole bytes are interleaved in an unusual way, the same
//! in both games: a bit write that needs a new byte starts one at the end of
//! the message (`cursize`), and the following bits fill that byte, lowest bit
//! first; a byte, short, long or string write appends raw bytes at the end,
//! after it. Reading mirrors this with `readcount` and `bit`. Numbers are
//! little-endian. The finished message is then Huffman-coded as a whole
//! ([`MessageTree::write_bits_compress`](super::huffman::MessageTree::write_bits_compress)).
//!
//! # The two games
//!
//! The writers are the same machine code in both games (`MSG_WriteBits` is
//! byte for byte identical). The readers differ, and [`Msg::game`] picks the
//! game's behavior:
//!
//! | | Call of Duty 2 1.3 | United Offensive 1.51 |
//! |---|---|---|
//! | Reading past `cursize` | marks the message [`overflowed`](Msg::overflowed), returns -1 | bit readers carry on into the bytes after `cursize`; byte readers return -1; nothing is marked |
//! | String writers change | `0x92` to `'` | `0x92` to `'`, every byte above 127 to `.` |
//! | String readers change | `0x92` to `'`; `%` to `.` in lines and big strings | the same, and every byte above 127 to `.` |
//! | A string read longer than the buffer | the rest is read and dropped | reading stops (the rest stays in the message) |
//! | Angles (`x87` precision) | 24 bits | 53 bits |
//!
//! # Memory past the buffer
//!
//! The original works on a pointer, so a read past the message's bytes
//! (United Offensive's bit readers past `cursize`) sees whatever memory
//! follows. The port reads [`Msg::data`] as far as it goes and zeros after
//! it; a caller that wants exactly the original's view passes the whole
//! buffer the original would read (the checker gives both sides the same
//! bytes there). Writes stay inside `maxsize`, as in the original, unless a
//! caller set `maxsize` larger than `data`: then the bytes past `data` are
//! dropped.
//!
//! # Strings without allocation
//!
//! The original string readers return a pointer to a static buffer of the
//! game (one per reader), overwritten by the next call. Here the caller owns
//! that buffer: [`Msg::read_string`] and friends fill a fixed-size array the
//! caller passes (`[u8; MAX_STRING_CHARS]`, `[u8; BIG_INFO_STRING]`) exactly
//! as the original fills its static one (bytes after the string keep what an
//! earlier read left there), and return the string's characters as a slice
//! of it. A caller that keeps one array per reader, as the game does, never
//! allocates.

use std::fmt;

use crate::cstr;
use crate::Game;

/// `MAX_STRING_CHARS`: a normal string's buffer, terminator included. A
/// written string must be shorter.
pub const MAX_STRING_CHARS: usize = 1024;
/// `BIG_INFO_STRING`: a big string's buffer (config strings), terminator included.
pub const BIG_INFO_STRING: usize = 8192;

/// `65536 / 360` as the games store it (an `f32`): degrees to 16-bit angle steps.
const ANGLE16_SCALE: f32 = 65536.0 / 360.0;
/// `256 / 360` as an `f32`: degrees to 8-bit angle steps.
const ANGLE8_SCALE: f32 = 256.0 / 360.0;
/// `360 / 65536`, exact in an `f32`: 16-bit angle steps to degrees.
const ANGLE16_TO_DEGREES: f32 = 360.0 / 65536.0;

/// The `msg_t` buffer: the message bytes and the positions in them.
///
/// The fields are public, as in the original, where the network code sets
/// them directly (`readcount` to skip a header, `cursize` after receiving a
/// packet). Sizes and positions are byte counts except [`bit`](Self::bit).
#[derive(Debug)]
pub struct Msg<'a> {
    /// Which game's rules the readers and the string functions follow.
    pub game: Game,
    /// The message's bytes. Usually `maxsize` long; it may be longer, so
    /// that reads past `maxsize` see the caller's bytes (see the module
    /// documentation).
    pub data: &'a mut [u8],
    /// Set when a write didn't fit (both games) or, in Call of Duty 2, when a
    /// read went past `cursize`. Never cleared by the writers and readers.
    pub overflowed: bool,
    /// How many bytes the message may hold.
    pub maxsize: usize,
    /// How many bytes are written.
    pub cursize: usize,
    /// How many bytes are read (a bit read counts the byte it starts).
    pub readcount: usize,
    /// The bit position of the bit writer or reader, counted from the start
    /// of `data`: `bit / 8` is the byte the next bit goes into or comes
    /// from, unless `bit % 8 == 0`, when the next bit starts a new byte.
    pub bit: usize,
}

/// A string writer's refusal: the string was too long for the limit, so an
/// empty string was written in its place (as the original does, after
/// printing a warning).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum StringTooLong {
    /// [`Msg::write_string`]: [`MAX_STRING_CHARS`] characters or more.
    Normal,
    /// [`Msg::write_big_string`]: [`BIG_INFO_STRING`] characters or more.
    Big,
}

impl StringTooLong {
    /// The warning the original prints (both games, the same text).
    pub fn message_format(self) -> &'static str {
        match self {
            StringTooLong::Normal => "MSG_WriteString: MAX_STRING_CHARS",
            StringTooLong::Big => "MSG_WriteString: BIG_INFO_STRING",
        }
    }
}

impl fmt::Display for StringTooLong {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str(self.message_format())
    }
}

impl std::error::Error for StringTooLong {}

/// What [`write_reliable_command_to_buffer`] warns about (it copies anyway).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ReliableCommandWarning {
    /// The command didn't fit and was cut.
    TooLong,
    /// The command was empty.
    Empty,
}

impl ReliableCommandWarning {
    /// The format of the warning the original prints (both games).
    pub fn message_format(self) -> &'static str {
        match self {
            ReliableCommandWarning::TooLong => {
                "WARNING: Reliable command is too long (%i/%i) and will be truncated: '%s'\n"
            }
            ReliableCommandWarning::Empty => "WARNING: Empty reliable command\n",
        }
    }
}

impl fmt::Display for ReliableCommandWarning {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str(self.message_format().trim_end())
    }
}

impl std::error::Error for ReliableCommandWarning {}

/// `kbitmask[bits]`: the low `bits` bits set (0 to 32). The games keep this
/// as a table of 33 numbers (Call of Duty 2 at 0x005CF488, United Offensive
/// at 0x005C5698) with other data after it; the delta-key readers are only
/// given 0 to 32.
fn low_bits_mask(bits: u32) -> i32 {
    if bits >= 32 {
        -1
    } else {
        ((1u32 << bits) - 1) as i32
    }
}

/// `__ftol2`, the C runtime's float-to-integer conversion (MSVC 7.1): the
/// x87 value truncated toward zero to 64 bits; NaN and anything outside the
/// 64-bit range give `i64::MIN` (the x87's "integer indefinite").
///
/// Original: `__ftol2` at 0x0057F114 (CoD2MP_s.exe 1.3)
/// Original: `__ftol2` at 0x00570FD8 (CoDUOMP.exe 1.51)
pub(crate) fn ftol2(value: f64) -> i64 {
    const LIMIT: f64 = 9_223_372_036_854_775_808.0; // 2^63
    if value.is_nan() || !(-LIMIT..LIMIT).contains(&value) {
        i64::MIN
    } else {
        value.trunc() as i64
    }
}

/// `angle * scale` as the game's x87 computes it (`fld`, `fmul dword`), at
/// the game's precision: Call of Duty 2 rounds the product to 24 bits, United
/// Offensive to 53 bits, where the product of two `f32` values is exact.
///
/// Rounding to `f32` stands in for the x87's 24-bit precision: the two only
/// differ beyond `f32`'s exponent range (an infinity where the x87 keeps a
/// huge finite number, or a lost bit far below 1), and truncating to an
/// integer gives the same result there.
fn scaled(game: Game, angle: f32, scale: f32) -> f64 {
    match game {
        Game::CallOfDuty2 => f64::from(angle * scale),
        Game::UnitedOffensive => f64::from(angle) * f64::from(scale),
    }
}

/// A character as the string writers send it.
#[inline]
fn written_char(game: Game, c: u8) -> u8 {
    match c {
        0x92 => b'\'',
        0x80..=0xFF if game == Game::UnitedOffensive => b'.',
        _ => c,
    }
}

/// A character as the string readers keep it; `percent` for the readers
/// that also change `%` (big strings and lines).
#[inline]
fn read_char(game: Game, c: u8, percent: bool) -> u8 {
    match c {
        b'%' if percent => b'.',
        0x92 => b'\'',
        0x80..=0xFF if game == Game::UnitedOffensive => b'.',
        _ => c,
    }
}

/// Which string reader: the buffer size and what ends or changes a string.
#[derive(Clone, Copy)]
struct StringReader {
    /// The buffer size, terminator included.
    size: usize,
    /// `%` becomes `.`.
    percent: bool,
    /// A newline ends the string (and is consumed).
    line: bool,
}

impl<'a> Msg<'a> {
    /// An empty message over `data`, `maxsize` its length.
    ///
    /// Call of Duty 2's `MSG_Init` also builds the Huffman tree the first
    /// time it runs (`MSG_initHuffman`); here the tree is built once by the
    /// caller ([`huffman::MessageTree`](super::huffman::MessageTree)).
    /// United Offensive does the same as `MSG_Init` inline wherever it
    /// starts a message.
    ///
    /// Original: `MSG_Init` at 0x00443DE0 (CoD2MP_s.exe 1.3)
    pub fn new(game: Game, data: &'a mut [u8]) -> Msg<'a> {
        let maxsize = data.len();
        Msg {
            game,
            data,
            overflowed: false,
            maxsize,
            cursize: 0,
            readcount: 0,
            bit: 0,
        }
    }

    fn cod2(&self) -> bool {
        self.game == Game::CallOfDuty2
    }

    /// The byte at `index`, or 0 past the end of [`data`](Self::data).
    #[inline]
    fn byte_at(&self, index: usize) -> u8 {
        self.data.get(index).copied().unwrap_or(0)
    }

    #[inline]
    fn set_byte(&mut self, index: usize, value: u8) {
        if let Some(byte) = self.data.get_mut(index) {
            *byte = value;
        }
    }

    #[inline]
    fn or_byte(&mut self, index: usize, value: u8) {
        if let Some(byte) = self.data.get_mut(index) {
            *byte |= value;
        }
    }

    /// Copies `bytes` to `data[at..]`, as far as `data` goes.
    fn copy_to(&mut self, at: usize, bytes: &[u8]) {
        let end = self.data.len().min(at.saturating_add(bytes.len()));
        if at < end {
            self.data[at..end].copy_from_slice(&bytes[..end - at]);
        }
    }

    /// A bit write at a byte boundary: the next byte at the end of the
    /// message becomes the bit writer's byte, cleared.
    #[inline]
    fn start_bit_byte(&mut self) {
        self.bit = self.cursize * 8;
        self.set_byte(self.cursize, 0);
        self.cursize += 1;
    }

    /// The next bit for a bit reader: starts a new byte at `readcount` when
    /// the last one is used up. `None` when Call of Duty 2 finds no byte
    /// left (the message is then marked overflowed).
    #[inline]
    fn next_read_bit(&mut self) -> Option<i32> {
        let shift = self.bit & 7;
        if shift == 0 {
            if self.cod2() && self.readcount >= self.cursize {
                self.overflowed = true;
                return None;
            }
            self.bit = self.readcount * 8;
            self.readcount += 1;
        }
        let value = (self.byte_at(self.bit >> 3) >> shift) & 1;
        self.bit += 1;
        Some(value.into())
    }

    /// The flag bit in front of a key-coded value: whether the value
    /// changed. A Call of Duty 2 message with no byte left counts as
    /// changed (after marking the message overflowed), as in the original.
    #[inline]
    fn read_change_bit(&mut self) -> bool {
        self.next_read_bit() != Some(0)
    }

    /// Back to the start for reading: `overflowed`, `readcount` and `bit`
    /// cleared. United Offensive does this inline.
    ///
    /// Original: `MSG_BeginReading` at 0x00443E20 (CoD2MP_s.exe 1.3)
    pub fn begin_reading(&mut self) {
        self.overflowed = false;
        self.readcount = 0;
        self.bit = 0;
    }

    /// The number of bits written: every byte up to `cursize`, less the
    /// unused high bits of the bit writer's last byte (counted as if it were
    /// the last byte of the message).
    ///
    /// Original: `MSG_GetUsedBitCount` at 0x00443E30 (CoD2MP_s.exe 1.3)
    pub fn used_bit_count(&self) -> usize {
        (self.cursize * 8).wrapping_sub(self.bit.wrapping_neg() & 7)
    }

    /// Writes the low `bits` bits of `value`, lowest first. Needs room for 4
    /// more bytes, however few bits are written (even none): otherwise the
    /// message is marked overflowed and nothing is written. Past 32 bits the
    /// value's sign bit repeats (the original shifts it right arithmetically)
    /// and only the first 4 bytes were checked for room.
    ///
    /// Original: `MSG_WriteBits` at 0x00443E50 (CoD2MP_s.exe 1.3)
    /// Original: `MSG_WriteBits` at 0x004495F0 (CoDUOMP.exe 1.51)
    pub fn write_bits(&mut self, value: i32, bits: u32) {
        if self.cursize + 4 > self.maxsize {
            self.overflowed = true;
            return;
        }
        let mut value = value;
        for _ in 0..bits {
            let shift = self.bit & 7;
            if shift == 0 {
                self.start_bit_byte();
            }
            if value & 1 != 0 {
                self.or_byte(self.bit >> 3, 1 << shift);
            }
            self.bit += 1;
            value >>= 1;
        }
    }

    /// Writes a 0 bit (needs room for one more byte).
    ///
    /// Original: `MSG_WriteBit0` at 0x00443EC0 (CoD2MP_s.exe 1.3)
    /// Original: `MSG_WriteBit0` at 0x00449660 (CoDUOMP.exe 1.51)
    pub fn write_bit0(&mut self) {
        if self.cursize >= self.maxsize {
            self.overflowed = true;
            return;
        }
        if self.bit & 7 == 0 {
            self.start_bit_byte();
        }
        self.bit += 1;
    }

    /// Writes a 1 bit (needs room for one more byte).
    ///
    /// Original: `MSG_WriteBit1` at 0x00443EF0 (CoD2MP_s.exe 1.3)
    /// Original: `MSG_WriteBit1` at 0x00449690 (CoDUOMP.exe 1.51)
    pub fn write_bit1(&mut self) {
        if self.cursize >= self.maxsize {
            self.overflowed = true;
            return;
        }
        let shift = self.bit & 7;
        if shift == 0 {
            self.start_bit_byte();
        }
        self.or_byte(self.bit >> 3, 1 << shift);
        self.bit += 1;
    }

    /// Reads `bits` bits, lowest first (bit 32 and up land on bits 0 and up
    /// again, as x86 shifts count modulo 32). The original takes an `int`:
    /// zero or less reads nothing and returns 0 (pass 0 for those).
    ///
    /// Call of Duty 2: when a new byte is needed and none is left before
    /// `cursize`, marks the message overflowed and returns -1 (the bits read
    /// so far stay read). United Offensive reads on past `cursize`.
    ///
    /// Original: `MSG_ReadBits` at 0x00443F40 (CoD2MP_s.exe 1.3)
    /// Original: `MSG_ReadBits` at 0x004496E0 (CoDUOMP.exe 1.51)
    pub fn read_bits(&mut self, bits: u32) -> i32 {
        let mut value = 0i32;
        for i in 0..bits {
            match self.next_read_bit() {
                Some(bit) => value |= bit.wrapping_shl(i),
                None => return -1,
            }
        }
        value
    }

    /// Reads one bit: 0 or 1, or -1 when Call of Duty 2 finds nothing left
    /// (see [`read_bits`](Self::read_bits)).
    ///
    /// Original: `MSG_ReadBit` at 0x00443FB0 (CoD2MP_s.exe 1.3)
    /// Original: `MSG_ReadBit` at 0x00449740 (CoDUOMP.exe 1.51)
    pub fn read_bit(&mut self) -> i32 {
        self.next_read_bit().unwrap_or(-1)
    }

    /// Appends one raw byte.
    ///
    /// Original: `MSG_WriteByte` at 0x004440B0 (CoD2MP_s.exe 1.3)
    /// Original: `MSG_WriteByte` at 0x00449830 (CoDUOMP.exe 1.51)
    pub fn write_byte(&mut self, c: u8) {
        if self.cursize >= self.maxsize {
            self.overflowed = true;
            return;
        }
        self.set_byte(self.cursize, c);
        self.cursize += 1;
    }

    /// Appends raw bytes, all or none.
    ///
    /// Original: `MSG_WriteData` at 0x004440D0 (CoD2MP_s.exe 1.3)
    /// Original: `MSG_WriteData` at 0x00449850 (CoDUOMP.exe 1.51)
    pub fn write_data(&mut self, bytes: &[u8]) {
        if self.cursize + bytes.len() > self.maxsize {
            self.overflowed = true;
            return;
        }
        self.copy_to(self.cursize, bytes);
        self.cursize += bytes.len();
    }

    /// Appends a 16-bit number, little-endian.
    ///
    /// Original: `MSG_WriteShort` at 0x00444110 (CoD2MP_s.exe 1.3)
    /// Original: `MSG_WriteShort` at 0x00449890 (CoDUOMP.exe 1.51)
    pub fn write_short(&mut self, c: i16) {
        self.write_data(&c.to_le_bytes());
    }

    /// Appends a 32-bit number, little-endian.
    ///
    /// Original: `MSG_WriteLong` at 0x00444140 (CoD2MP_s.exe 1.3)
    /// Original: `MSG_WriteLong` at 0x004498C0 (CoDUOMP.exe 1.51)
    pub fn write_long(&mut self, c: i32) {
        self.write_data(&c.to_le_bytes());
    }

    /// Appends `s` (up to its first NUL, if it has one) and a terminator,
    /// all or none, `0x92` sent as `'` (and, in United Offensive, every byte
    /// above 127 as `.`). A string of [`MAX_STRING_CHARS`] characters or
    /// more is refused: an empty string is written instead and the
    /// refusal returned (the original prints it as a warning).
    ///
    /// United Offensive also takes a NULL pointer, as an empty string: pass
    /// `b""`.
    ///
    /// Original: `MSG_WriteString` at 0x004441B0 (CoD2MP_s.exe 1.3)
    /// Original: `MSG_WriteString` at 0x004498F0 (CoDUOMP.exe 1.51)
    pub fn write_string(&mut self, s: &[u8]) -> Result<(), StringTooLong> {
        self.write_limited_string(s, MAX_STRING_CHARS, StringTooLong::Normal)
    }

    /// [`write_string`](Self::write_string) for config strings: the limit is
    /// [`BIG_INFO_STRING`].
    ///
    /// Original: `MSG_WriteBigString` at 0x00444270 (CoD2MP_s.exe 1.3)
    /// Original: `MSG_WriteBigString` at 0x004499F0 (CoDUOMP.exe 1.51)
    pub fn write_big_string(&mut self, s: &[u8]) -> Result<(), StringTooLong> {
        self.write_limited_string(s, BIG_INFO_STRING, StringTooLong::Big)
    }

    fn write_limited_string(
        &mut self,
        s: &[u8],
        limit: usize,
        refusal: StringTooLong,
    ) -> Result<(), StringTooLong> {
        let length = cstr::len(s);
        if length >= limit {
            self.write_data(&[0]);
            return Err(refusal);
        }
        if self.cursize + length + 1 > self.maxsize {
            self.overflowed = true;
            return Ok(());
        }
        let game = self.game;
        for (i, &c) in s[..length].iter().enumerate() {
            self.set_byte(self.cursize + i, written_char(game, c));
        }
        self.set_byte(self.cursize + length, 0);
        self.cursize += length + 1;
        Ok(())
    }

    /// Appends an angle in degrees as one byte (256 steps a turn), truncated
    /// toward zero by the C runtime (`__ftol2`) after a multiplication at the
    /// game's x87 precision (see [`write_angle16`](Self::write_angle16)).
    ///
    /// Original: `MSG_WriteAngle` at 0x00444350 (CoD2MP_s.exe 1.3)
    /// Original: `MSG_WriteAngle` at 0x00449B50 (CoDUOMP.exe 1.51)
    pub fn write_angle(&mut self, angle: f32) {
        if self.cursize >= self.maxsize {
            self.overflowed = true;
            return;
        }
        let steps = ftol2(scaled(self.game, angle, ANGLE8_SCALE));
        self.set_byte(self.cursize, steps as u8);
        self.cursize += 1;
    }

    /// Appends an angle in degrees as a 16-bit number (65536 steps a turn):
    /// `angle * (65536 / 360)` (the constant an `f32`), computed by the x87
    /// at the game's precision (Call of Duty 2 rounds the product to 24 bits,
    /// United Offensive keeps it exact), then truncated toward zero to 64 bits
    /// by `__ftol2` and cut to 16. A product of a number of steps plus a hair
    /// less than one can round up to the whole number at 24 bits, so the two
    /// games send different numbers for a few angles. Beyond the 64-bit
    /// range (or NaN) the steps are 0.
    ///
    /// Original: `MSG_WriteAngle16` at 0x00444380 (CoD2MP_s.exe 1.3)
    /// Original: `MSG_WriteAngle16` at 0x00449B80 (CoDUOMP.exe 1.51)
    pub fn write_angle16(&mut self, angle: f32) {
        if self.cursize + 2 > self.maxsize {
            self.overflowed = true;
            return;
        }
        let steps = ftol2(scaled(self.game, angle, ANGLE16_SCALE)) as u16;
        let at = self.cursize;
        self.copy_to(at, &steps.to_le_bytes());
        self.cursize += 2;
    }

    /// The next `N` raw bytes, or `None` when fewer than `N` are left before
    /// `cursize` (Call of Duty 2 then marks the message overflowed).
    fn read_raw<const N: usize>(&mut self) -> Option<[u8; N]> {
        if self.readcount + N > self.cursize {
            if self.cod2() {
                self.overflowed = true;
            }
            return None;
        }
        let mut out = [0u8; N];
        for (i, byte) in out.iter_mut().enumerate() {
            *byte = self.byte_at(self.readcount + i);
        }
        self.readcount += N;
        Some(out)
    }

    /// Reads one raw byte (0 to 255), or -1 when none is left before
    /// `cursize` (Call of Duty 2 also marks the message overflowed).
    ///
    /// Original: `MSG_ReadByte` at 0x004443C0 (CoD2MP_s.exe 1.3)
    /// Original: `MSG_ReadByte` at 0x00449BC0 (CoDUOMP.exe 1.51)
    pub fn read_byte(&mut self) -> i32 {
        self.read_raw::<1>().map_or(-1, |b| b[0].into())
    }

    /// Reads a signed 16-bit number, or -1 (as [`read_byte`](Self::read_byte)).
    ///
    /// Original: `MSG_ReadShort` at 0x004443E0 (CoD2MP_s.exe 1.3)
    /// Original: `MSG_ReadShort` at 0x00449BE0 (CoDUOMP.exe 1.51)
    pub fn read_short(&mut self) -> i32 {
        self.read_raw::<2>()
            .map_or(-1, |b| i16::from_le_bytes(b).into())
    }

    /// Reads a 32-bit number, or -1 (as [`read_byte`](Self::read_byte)).
    ///
    /// Original: `MSG_ReadLong` at 0x00444410 (CoD2MP_s.exe 1.3)
    /// Original: `MSG_ReadLong` at 0x00449C10 (CoDUOMP.exe 1.51)
    pub fn read_long(&mut self) -> i32 {
        self.read_raw::<4>().map_or(-1, i32::from_le_bytes)
    }

    /// Reads `out.len()` raw bytes into `out`. When fewer are left before
    /// `cursize`, nothing is read and `out` is filled with 0xFF (Call of
    /// Duty 2 also marks the message overflowed).
    ///
    /// Original: `MSG_ReadData` at 0x00444600 (CoD2MP_s.exe 1.3)
    /// Original: `MSG_ReadData` at 0x00449D90 (CoDUOMP.exe 1.51)
    pub fn read_data(&mut self, out: &mut [u8]) {
        if self.readcount + out.len() > self.cursize {
            if self.cod2() {
                self.overflowed = true;
            }
            out.fill(0xFF);
            return;
        }
        for (i, byte) in out.iter_mut().enumerate() {
            *byte = self.byte_at(self.readcount + i);
        }
        self.readcount += out.len();
    }

    /// Reads a string up to its terminator into `out` (the original's static
    /// buffer; see the module documentation) and returns its characters.
    /// `0x92` is kept as `'` (United Offensive: every byte above 127 as `.`).
    ///
    /// The string ends at a NUL or at `cursize`. Call of Duty 2 keeps the
    /// first 1023 characters and reads the rest of a longer string to its
    /// end (always zeroing `out[1023]`); reaching `cursize` marks the message
    /// overflowed. United Offensive stops after 1023 characters, leaving the
    /// rest in the message.
    ///
    /// Original: `MSG_ReadString` at 0x00444480 (CoD2MP_s.exe 1.3)
    /// Original: `MSG_ReadString` at 0x00449C30 (CoDUOMP.exe 1.51)
    pub fn read_string<'b>(&mut self, out: &'b mut [u8; MAX_STRING_CHARS]) -> &'b [u8] {
        let reader = StringReader {
            size: MAX_STRING_CHARS,
            percent: false,
            line: false,
        };
        self.read_string_into(out, reader)
    }

    /// [`read_string`](Self::read_string) for config strings and printed
    /// text: up to 8191 characters, and `%` is kept as `.`.
    ///
    /// Original: `MSG_ReadBigString` at 0x004444E0 (CoD2MP_s.exe 1.3)
    /// Original: `MSG_ReadBigString` at 0x00449C80 (CoDUOMP.exe 1.51)
    pub fn read_big_string<'b>(&mut self, out: &'b mut [u8; BIG_INFO_STRING]) -> &'b [u8] {
        let reader = StringReader {
            size: BIG_INFO_STRING,
            percent: true,
            line: false,
        };
        self.read_string_into(out, reader)
    }

    /// [`read_string`](Self::read_string) that also ends at a newline (read
    /// and dropped), and keeps `%` as `.`: how out-of-band packets are read.
    ///
    /// Original: `MSG_ReadStringLine` at 0x00444540 (CoD2MP_s.exe 1.3)
    /// Original: `MSG_ReadStringLine` at 0x00449CE0 (CoDUOMP.exe 1.51)
    pub fn read_string_line<'b>(&mut self, out: &'b mut [u8; MAX_STRING_CHARS]) -> &'b [u8] {
        let reader = StringReader {
            size: MAX_STRING_CHARS,
            percent: true,
            line: true,
        };
        self.read_string_into(out, reader)
    }

    fn read_string_into<'b>(&mut self, out: &'b mut [u8], reader: StringReader) -> &'b [u8] {
        let game = self.game;
        let last = reader.size - 1;
        let mut length = 0;
        if self.cod2() {
            loop {
                let c = match self.read_raw::<1>() {
                    Some([c]) if reader.line && c == b'\n' => 0,
                    Some([c]) => c,
                    None => 0, // overflowed is set
                };
                if length < reader.size {
                    out[length] = read_char(game, c, reader.percent);
                }
                if c == 0 {
                    break;
                }
                length += 1;
            }
            out[last] = 0;
            length = length.min(last);
        } else {
            while length < last {
                let Some([c]) = self.read_raw::<1>() else {
                    break;
                };
                if c == 0 || (reader.line && c == b'\n') {
                    break;
                }
                out[length] = read_char(game, c, reader.percent);
                length += 1;
            }
            out[length] = 0;
        }
        &out[..length]
    }

    /// Reads a 16-bit angle and returns it in degrees (`short * 360 / 65536`,
    /// exact in an `f32`); a missing short reads as -1 (see
    /// [`read_short`](Self::read_short)).
    ///
    /// Original: `MSG_ReadAngle16` at 0x004445B0 (CoD2MP_s.exe 1.3)
    /// Original: `MSG_ReadAngle16` at 0x00449D40 (CoDUOMP.exe 1.51)
    pub fn read_angle16(&mut self) -> f32 {
        self.read_short() as f32 * ANGLE16_TO_DEGREES
    }

    /// Writes a value as a change from `old`: one 0 bit when `new == old`,
    /// otherwise a 1 bit and the low `bits` bits of `new ^ key` (through
    /// [`write_bits`](Self::write_bits), so they need room for 4 bytes).
    ///
    /// Original: `MSG_WriteDeltaKey` at 0x00444650 (CoD2MP_s.exe 1.3)
    /// Original: `MSG_WriteDeltaKey` at 0x00449E90 (CoDUOMP.exe 1.51)
    pub fn write_delta_key(&mut self, key: i32, old: i32, new: i32, bits: u32) {
        if old == new {
            self.write_bit0();
            return;
        }
        self.write_bit1();
        self.write_bits(new ^ key, bits);
    }

    /// Reads what [`write_delta_key`](Self::write_delta_key) wrote: `old`
    /// after a 0 bit, otherwise `bits` bits xored with the low `bits` bits of
    /// `key` (`bits` from 0 to 32). In Call of Duty 2 a message with no byte
    /// left for the flag bit counts as changed: the value then reads as -1
    /// xored with the key's bits.
    ///
    /// Original: `MSG_ReadDeltaKey` at 0x004446A0 (CoD2MP_s.exe 1.3)
    /// Original: `MSG_ReadDeltaKey` at 0x00449EE0 (CoDUOMP.exe 1.51)
    pub fn read_delta_key(&mut self, key: i32, old: i32, bits: u32) -> i32 {
        if !self.read_change_bit() {
            return old;
        }
        self.read_bits(bits) ^ (key & low_bits_mask(bits))
    }

    /// [`write_delta_key`](Self::write_delta_key) for a byte: compares the
    /// low bytes of `old` and `new`, and writes the changed one as a raw byte
    /// `new ^ key` after the 1 bit.
    ///
    /// Original: `MSG_WriteDeltaKeyByte` at 0x00444750 (CoD2MP_s.exe 1.3)
    /// Original: `MSG_WriteDeltaKeyByte` at 0x00449F80 (CoDUOMP.exe 1.51)
    pub fn write_delta_key_byte(&mut self, key: i32, old: i32, new: i32) {
        if old as u8 == new as u8 {
            self.write_bit0();
            return;
        }
        self.write_bit1();
        self.write_byte((key ^ new) as u8);
    }

    /// Reads what [`write_delta_key_byte`](Self::write_delta_key_byte)
    /// wrote: `old` (all 32 bits) after a 0 bit, otherwise the raw byte
    /// xored with the key's low byte (0 to 255; a missing byte reads as
    /// 0xFF).
    ///
    /// Original: `MSG_ReadDeltaKeyByte` at 0x004447B0 (CoD2MP_s.exe 1.3)
    /// Original: `MSG_ReadDeltaKeyByte` at 0x00449FE0 (CoDUOMP.exe 1.51)
    pub fn read_delta_key_byte(&mut self, key: i32, old: i32) -> i32 {
        if !self.read_change_bit() {
            return old;
        }
        i32::from(key as u8 ^ self.read_byte() as u8)
    }

    /// [`write_delta_key`](Self::write_delta_key) for a 16-bit number: a raw
    /// short `new ^ key` after the 1 bit.
    ///
    /// Original: `MSG_WriteDeltaKeyShort` at 0x00444840 (CoD2MP_s.exe 1.3)
    /// Original: `MSG_WriteDeltaKeyShort` at 0x0044A060 (CoDUOMP.exe 1.51)
    pub fn write_delta_key_short(&mut self, key: i32, old: i32, new: i32) {
        if old as i16 == new as i16 {
            self.write_bit0();
            return;
        }
        self.write_bit1();
        self.write_short((key ^ new) as i16);
    }

    /// Reads what [`write_delta_key_short`](Self::write_delta_key_short)
    /// wrote: `old` after a 0 bit, otherwise the raw short xored with the
    /// key's low 16 bits, sign-extended (a missing short reads as -1).
    ///
    /// Original: `MSG_ReadDeltaKeyShort` at 0x004448B0 (CoD2MP_s.exe 1.3)
    /// Original: `MSG_ReadDeltaKeyShort` at 0x0044A0D0 (CoDUOMP.exe 1.51)
    pub fn read_delta_key_short(&mut self, key: i32, old: i32) -> i32 {
        if !self.read_change_bit() {
            return old;
        }
        i32::from(key as i16 ^ self.read_short() as i16)
    }
}

/// Copies a reliable command (a server or client command sent until it is
/// acknowledged) into `out`, `out.len()` being the original's buffer size:
/// at most `out.len() - 1` characters and a terminator, `0x92` as `'`,
/// `%` as `.` (United Offensive: every byte above 127 as `.` too). A command
/// that doesn't fit is cut; the original warns about that and about an empty
/// command, which is returned here after the copy. The copied string isn't
/// padded: bytes after its terminator keep their value.
///
/// An empty `out` is left alone (the original, given a size of 0, would
/// write a terminator just before its buffer).
///
/// Original: `MSG_WriteReliableCommandToBuffer` at 0x00444940 (CoD2MP_s.exe 1.3)
/// Original: `MSG_WriteReliableCommandToBuffer` at 0x0044A150 (CoDUOMP.exe 1.51)
pub fn write_reliable_command_to_buffer(
    game: Game,
    command: &[u8],
    out: &mut [u8],
) -> Result<(), ReliableCommandWarning> {
    let length = cstr::len(command);
    let warning = if length >= out.len() {
        Err(ReliableCommandWarning::TooLong)
    } else if length == 0 {
        Err(ReliableCommandWarning::Empty)
    } else {
        Ok(())
    };
    let size = out.len();
    for (i, slot) in out.iter_mut().enumerate() {
        let c = cstr::at(command, i);
        if c == 0 {
            *slot = 0;
            return warning;
        }
        *slot = match written_char(game, c) {
            b'%' => b'.',
            c => c,
        };
    }
    if let Some(last) = size.checked_sub(1) {
        out[last] = 0;
    }
    warning
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn bits_and_bytes_interleave() {
        let mut buffer = [0xAAu8; 16];
        let mut msg = Msg::new(Game::CallOfDuty2, &mut buffer);
        msg.write_bits(0b101, 3); // starts byte 0
        msg.write_byte(0x7F); // byte 1, after the bit byte
        msg.write_bit1(); // still byte 0, bit 3
        msg.write_bits(-1, 6); // bits 4-7 of byte 0, then a new byte 2
        assert_eq!(msg.cursize, 3);
        assert_eq!(&msg.data[..3], &[0b1111_1101, 0x7F, 0b11]);
        assert_eq!(msg.used_bit_count(), 3 * 8 - 6);

        msg.begin_reading();
        assert_eq!(msg.read_bits(3), 0b101);
        assert_eq!(msg.read_byte(), 0x7F);
        assert_eq!(msg.read_bit(), 1);
        assert_eq!(msg.read_bits(6), 0b11_1111);
        assert!(!msg.overflowed);
    }

    #[test]
    fn readers_past_the_end_differ_by_game() {
        let mut buffer = [1u8, 2, 3, 4];
        let mut msg = Msg::new(Game::CallOfDuty2, &mut buffer);
        msg.cursize = 1;
        assert_eq!(msg.read_bits(16), -1);
        assert!(msg.overflowed);

        let mut buffer = [1u8, 2, 3, 4];
        let mut msg = Msg::new(Game::UnitedOffensive, &mut buffer);
        msg.cursize = 1;
        assert_eq!(msg.read_bits(16), 0x0201);
        assert_eq!(msg.read_bits(32), 0x0403); // and zeros past the slice
        assert_eq!(msg.read_byte(), -1);
        assert!(!msg.overflowed);
    }

    #[test]
    fn writers_need_room() {
        let mut buffer = [0u8; 5];
        let mut msg = Msg::new(Game::UnitedOffensive, &mut buffer);
        msg.write_byte(1);
        msg.write_bits(0, 0); // 4 bytes of room: fine
        assert!(!msg.overflowed);
        msg.write_byte(2);
        msg.write_bits(0, 0); // only 3 left
        assert!(msg.overflowed);
        assert_eq!(msg.cursize, 2);
        msg.write_long(7);
        assert_eq!(msg.cursize, 2);
        msg.write_short(-2);
        assert_eq!(&msg.data[..4], &[1, 2, 0xFE, 0xFF]);
    }

    #[test]
    fn strings_round_trip_with_the_games_changes() {
        for game in [Game::CallOfDuty2, Game::UnitedOffensive] {
            let mut buffer = [0u8; 64];
            let mut msg = Msg::new(game, &mut buffer);
            msg.write_string(b"it\x92s 100%\xE9").unwrap();
            msg.write_byte(b'x');
            msg.write_data(b"line one\nline two\0");
            msg.begin_reading();
            let mut out = [0u8; MAX_STRING_CHARS];
            let expected: &[u8] = if game == Game::CallOfDuty2 {
                b"it's 100%\xE9"
            } else {
                b"it's 100%."
            };
            assert_eq!(msg.read_string(&mut out), expected);
            assert_eq!(msg.read_byte(), i32::from(b'x'));
            assert_eq!(msg.read_string_line(&mut out), b"line one");
            assert_eq!(msg.read_string_line(&mut out), b"line two");
            assert!(!msg.overflowed);
        }
    }

    #[test]
    fn long_strings() {
        let long = [b'a'; MAX_STRING_CHARS];
        let mut buffer = [0u8; 2100];
        let mut msg = Msg::new(Game::CallOfDuty2, &mut buffer);
        assert_eq!(msg.write_string(&long), Err(StringTooLong::Normal));
        assert_eq!(msg.cursize, 1); // an empty string instead
        msg.write_data(&long);
        msg.write_data(b"b\0c");
        let mut out = [0u8; MAX_STRING_CHARS];
        for (game, rest) in [(Game::CallOfDuty2, b'c'), (Game::UnitedOffensive, b'a')] {
            msg.game = game;
            msg.begin_reading();
            assert_eq!(msg.read_string(&mut out), b"");
            assert_eq!(msg.read_string(&mut out), &long[..1023]);
            assert_eq!(msg.read_byte(), i32::from(rest));
        }
    }

    #[test]
    fn angles_follow_the_precision() {
        // This angle times (65536 / 360) is 4.99999978 exactly; rounded to
        // 24 bits it is 5.
        let angle = f32::from_bits(0x3CE0_FFFF);
        let exact = f64::from(angle) * f64::from(ANGLE16_SCALE);
        assert!(exact < 5.0 && angle * ANGLE16_SCALE == 5.0);
        let mut steps = [0u16; 2];
        for (game, slot) in [(Game::CallOfDuty2, 0), (Game::UnitedOffensive, 1)] {
            let mut buffer = [0u8; 2];
            let mut msg = Msg::new(game, &mut buffer);
            msg.write_angle16(angle);
            steps[slot] = u16::from_le_bytes(buffer);
        }
        assert_eq!(steps, [5, 4]);
        assert_eq!(ftol2(-1.5), -1);
        assert_eq!(ftol2(f64::NAN), i64::MIN);
        assert_eq!(ftol2(1e30), i64::MIN);
    }

    #[test]
    fn delta_keys_round_trip() {
        let mut buffer = [0u8; 32];
        let mut msg = Msg::new(Game::UnitedOffensive, &mut buffer);
        let key = 0x1234_5678;
        msg.write_delta_key(key, 5, 5, 12);
        msg.write_delta_key(key, 5, 0x0ABC, 12);
        msg.write_delta_key_byte(key, 0x100, 0x200);
        msg.write_delta_key_byte(key, 1, 0xFE);
        msg.write_delta_key_short(key, 7, -300);
        msg.begin_reading();
        assert_eq!(msg.read_delta_key(key, 5, 12), 5);
        assert_eq!(msg.read_delta_key(key, 5, 12), 0x0ABC);
        assert_eq!(msg.read_delta_key_byte(key, 0x100), 0x100);
        assert_eq!(msg.read_delta_key_byte(key, 1), 0xFE);
        assert_eq!(msg.read_delta_key_short(key, 7), -300);
    }

    #[test]
    fn reliable_commands() {
        let mut out = [0x41u8; 8];
        assert_eq!(
            write_reliable_command_to_buffer(Game::UnitedOffensive, b"say 50%\xE9!", &mut out),
            Err(ReliableCommandWarning::TooLong)
        );
        assert_eq!(&out, b"say 50.\0");
        let mut out = [0x41u8; 8];
        assert_eq!(
            write_reliable_command_to_buffer(Game::CallOfDuty2, b"a\x92", &mut out),
            Ok(())
        );
        assert_eq!(&out, b"a'\0AAAAA");
        assert_eq!(
            write_reliable_command_to_buffer(Game::CallOfDuty2, b"", &mut out),
            Err(ReliableCommandWarning::Empty)
        );
    }
}
