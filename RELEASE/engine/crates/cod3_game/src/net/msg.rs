//! Network messages: `msg_t`, a buffer of bytes being written or read.
//!
//! The game's structure is 20 bytes. `MSG_Init` (0x824521D8, not named yet)
//! and the inline set-ups in `SV_SendClientSnapshot` and `Netchan_Transmit`
//! zero exactly these, and the code that reads and writes messages uses them
//! this way:
//!
//! | Offset | Field | What it is |
//! |---|---|---|
//! | +0 | `overflowed` | zeroed; nothing in the message code reads or sets it |
//! | +4 | `data` | the buffer |
//! | +8 | `maxsize` | the buffer's size (`MSG_Init` always gives [`MAX_MSGLEN`]) |
//! | +0xC | `cursize` | how many bytes are written |
//! | +0x10 | `readcount` | where the next read starts |
//!
//! Quake III's `msg_t` also has a bit position and overflow flags that its
//! writes check. CoD3 has no bit position, and its writes don't check the
//! size at all (`MSG_WriteString` here, and the byte, short, long and data
//! writers next to it). Reads stop at `cursize`.
//!
//! Strings differ from Quake III too:
//! * nothing is filtered: Quake III's writer turned bytes above 127 into
//!   `.`, and its reader `%` and bytes above 127; CoD3 sends and receives
//!   every byte as it is;
//! * reading a string is all or nothing: one that is too long, or doesn't
//!   end before `cursize`, reads as an empty string and leaves `readcount`
//!   where it was (see [`Msg::read_string`]); Quake III read what it could
//!   and moved on.

use crate::cstr;
use crate::error::GameError;

/// `MAX_MSGLEN`: the size of the game's message buffers. `MSG_Init` always
/// sets `maxsize` to it, `SV_SendClientSnapshot` builds its message in a
/// buffer of this size, and `Netchan_Transmit` refuses anything longer.
pub const MAX_MSGLEN: usize = 3072;

/// `MAX_STRING_CHARS`: a string in a message has fewer characters than this
/// (255 at most, plus its terminator).
pub const MAX_STRING_CHARS: usize = 256;

/// `msg_t`: a network message being written or read.
#[derive(Debug, PartialEq, Eq)]
pub struct Msg<'a> {
    /// +0: zeroed by `MSG_Init`; no message function reads or sets it. (Quake
    /// III's `msg_t` starts with its overflow flags, hence the name; CoD3's
    /// writes have no overflow check.)
    pub overflowed: i32,
    /// +4: the buffer, at its real size. The functions here never touch
    /// bytes outside it.
    pub data: &'a mut [u8],
    /// +8: the size the game gives the buffer. Neither function here reads it.
    pub maxsize: i32,
    /// +0xC: how many bytes are written; reads stop here.
    pub cursize: i32,
    /// +0x10: where the next read starts.
    pub readcount: i32,
}

impl<'a> Msg<'a> {
    /// An empty message over `data`, with `maxsize` the slice's length.
    ///
    /// (`MSG_Init` also zeroes the buffer and always says [`MAX_MSGLEN`]
    /// bytes; give it a buffer of that size to get the same message.)
    pub fn new(data: &'a mut [u8]) -> Self {
        let maxsize = i32::try_from(data.len()).unwrap_or(i32::MAX);
        Msg {
            overflowed: 0,
            data,
            maxsize,
            cursize: 0,
            readcount: 0,
        }
    }

    /// Appends `text` and its terminator at `cursize`, and moves `cursize`
    /// past them. Text of [`MAX_STRING_CHARS`] characters or more is sent as
    /// an empty string (just the terminator). `text` ends at its first NUL,
    /// if it has one. The original sends an empty string for a NULL pointer
    /// too: pass `b""`.
    ///
    /// The original doesn't check the size: it writes past the end of a full
    /// buffer, over whatever follows it, and it doesn't look at `maxsize`.
    /// Here a string that doesn't fit in `data` is refused instead, with an
    /// error the original doesn't have, and the message stays as it was.
    /// (Its "MAX_STRING_CHARS" print for a long string is a developer print,
    /// compiled out of the game's build.)
    ///
    /// Original: `MSG_WriteString` at 0x82454280 (multiplayer: 0x82484660)
    pub fn write_string(&mut self, text: &[u8]) -> Result<(), GameError> {
        let text = cstr::text(text);
        let sent: &[u8] = if text.len() >= MAX_STRING_CHARS {
            b""
        } else {
            text
        };
        let ran_past = || GameError::fatal("MSG_WriteString: the message ran past its buffer");
        let start = usize::try_from(self.cursize).ok();
        let new_size = self.cursize.checked_add(sent.len() as i32 + 1);
        let (Some(start), Some(new_size)) = (start, new_size) else {
            return Err(ran_past());
        };
        let terminator = start + sent.len();
        if terminator >= self.data.len() {
            return Err(ran_past());
        }
        self.data[start..terminator].copy_from_slice(sent);
        self.data[terminator] = 0;
        self.cursize = new_size;
        Ok(())
    }

    /// Reads the NUL-terminated string at `readcount` and moves `readcount`
    /// past its terminator. The result has no terminator and every byte as
    /// it was sent.
    ///
    /// All or nothing: a string of [`MAX_STRING_CHARS`] characters or more,
    /// or one whose terminator isn't before `cursize` (it starts at or past
    /// the end of the data, or the end cuts it off), reads as an empty
    /// string, and `readcount` stays where it was, so the next read starts
    /// at the same place.
    ///
    /// The original copies the string into a static buffer, which the next
    /// call overwrites. The port returns its place in `data` instead; the
    /// borrow ends before the next read in the same way.
    ///
    /// The original looks for the terminator before it compares with
    /// `cursize`, so when the buffer has no NUL after `readcount` it reads
    /// on past the end of `data`. Here a string without a terminator in
    /// `data`, or a `readcount` outside it, reads as empty: what the
    /// original gives whenever `cursize` is within the buffer.
    ///
    /// Original: `MSG_ReadString` at 0x82451F48 (multiplayer: 0x8247F020)
    pub fn read_string(&mut self) -> &[u8] {
        let Ok(start) = usize::try_from(self.readcount) else {
            return b"";
        };
        let rest = self.data.get(start..).unwrap_or_default();
        let Some(len) = rest.iter().take(MAX_STRING_CHARS).position(|&c| c == 0) else {
            return b"";
        };
        // The terminator (at readcount + len) must come before cursize.
        if i64::from(self.readcount) + len as i64 >= i64::from(self.cursize) {
            return b"";
        }
        self.readcount += len as i32 + 1;
        &self.data[start..start + len]
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn strings_go_through_unchanged() {
        let mut buffer = [0xEEu8; 64];
        let mut msg = Msg::new(&mut buffer);
        msg.write_string(b"say \"100%\" \xE9t\xE9").unwrap();
        msg.write_string(b"").unwrap();
        msg.write_string(b"cut\0here").unwrap();
        assert_eq!(msg.cursize, 15 + 1 + 4);
        assert_eq!(msg.read_string(), b"say \"100%\" \xE9t\xE9");
        assert_eq!(msg.read_string(), b"");
        assert_eq!(msg.read_string(), b"cut");
        assert_eq!(msg.readcount, msg.cursize);
        assert_eq!(buffer[20], 0xEE, "nothing written after the last string");
    }

    #[test]
    fn long_strings_are_sent_empty() {
        let mut buffer = [0xEEu8; 300];
        let mut msg = Msg::new(&mut buffer);
        msg.write_string(&[b'a'; 256]).unwrap();
        assert_eq!(msg.cursize, 1);
        msg.write_string(&[b'b'; 255]).unwrap();
        assert_eq!(msg.cursize, 1 + 256);
        assert_eq!(msg.read_string(), b"");
        assert_eq!(msg.read_string(), &[b'b'; 255][..]);
    }

    #[test]
    fn reading_is_all_or_nothing() {
        let mut buffer = [0u8; 600];
        buffer[..300].fill(b'x'); // 300 characters: too long
        buffer[301..304].copy_from_slice(b"end");
        let mut msg = Msg::new(&mut buffer);
        msg.cursize = 305;
        assert_eq!(msg.read_string(), b"");
        assert_eq!(msg.readcount, 0, "a string too long doesn't move readcount");
        msg.readcount = 301;
        msg.cursize = 304; // the terminator at 304 isn't before cursize
        assert_eq!(msg.read_string(), b"");
        assert_eq!(msg.readcount, 301);
        msg.cursize = 305;
        assert_eq!(msg.read_string(), b"end");
        assert_eq!(msg.readcount, 305);
        assert_eq!(msg.read_string(), b"", "at the end of the data");
        assert_eq!(msg.readcount, 305);
    }

    #[test]
    fn a_string_that_does_not_fit_is_refused() {
        let mut buffer = [0xEEu8; 8];
        let mut msg = Msg::new(&mut buffer);
        msg.cursize = 3;
        assert!(msg.write_string(b"12345").is_err());
        assert_eq!(msg.cursize, 3);
        msg.write_string(b"1234").unwrap();
        assert_eq!(msg.cursize, 8);
        assert_eq!(&buffer[3..], b"1234\0");
    }
}
