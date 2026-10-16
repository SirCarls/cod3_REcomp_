//! LZO1X decompressor with no dependencies.
//!
//! Call of Duty 3 stores the data of its `.cod` archives in "NCH" blocks
//! whose payload is a plain LZO1X stream (identified by decompressing the
//! retail files: the sizes in the NCH header match exactly). This is the
//! standard algorithm (as in minilzo's `lzo1x_decompress_safe`), written as
//! a small state machine with bounds checks on every read and copy.
//!
//! Instruction summary (`t` is the instruction byte):
//! * `t >= 64`  (M2): match of 3..8 bytes, distance up to 2 KB, one extra byte.
//! * `t >= 32`  (M3): match length in `t & 31` (extended), distance up to 16 KB.
//! * `t >= 16`  (M4): match with distance 16..48 KB; distance 0 marks the end.
//! * `t < 16`:  after a literal run, a 3-byte match at distance 2 KB + n;
//!   after a match, a 2-byte match at a short distance (M1). In the main
//!   loop, a literal run of `t + 3` bytes (extended when `t == 0`).
//! * The low two bits of the byte before the last one read by a match give
//!   0..3 literals copied right after it.

#![forbid(unsafe_code)]

use std::fmt;

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Error {
    /// The input ended in the middle of an instruction.
    InputOverrun,
    /// A match refers to data before the start of the output.
    LookbehindOverrun,
    /// The output would grow past the caller's limit.
    OutputOverrun,
}

impl fmt::Display for Error {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "{self:?}")
    }
}

impl std::error::Error for Error {}

struct Decoder<'a> {
    input: &'a [u8],
    ip: usize,
    out: Vec<u8>,
    max_out: usize,
}

impl Decoder<'_> {
    fn byte(&mut self) -> Result<usize, Error> {
        let b = *self.input.get(self.ip).ok_or(Error::InputOverrun)?;
        self.ip += 1;
        Ok(b as usize)
    }

    /// Little-endian 16-bit value (the distance field of M3 / M4).
    fn le16(&mut self) -> Result<usize, Error> {
        Ok(self.byte()? | (self.byte()? << 8))
    }

    /// Reads an extended length: zero bytes add 255 each, then `base` + the next byte.
    fn extended_length(&mut self, base: usize) -> Result<usize, Error> {
        let mut t = 0usize;
        loop {
            let b = self.byte()?;
            if b != 0 {
                return Ok(t + base + b);
            }
            t += 255;
        }
    }

    fn literals(&mut self, n: usize) -> Result<(), Error> {
        let src = self
            .input
            .get(self.ip..self.ip + n)
            .ok_or(Error::InputOverrun)?;
        if self.out.len() + n > self.max_out {
            return Err(Error::OutputOverrun);
        }
        self.out.extend_from_slice(src);
        self.ip += n;
        Ok(())
    }

    /// Copies `n` bytes starting `distance` bytes back (may overlap itself).
    fn copy_match(&mut self, distance: usize, n: usize) -> Result<(), Error> {
        if distance == 0 || distance > self.out.len() {
            return Err(Error::LookbehindOverrun);
        }
        if self.out.len() + n > self.max_out {
            return Err(Error::OutputOverrun);
        }
        let start = self.out.len() - distance;
        for k in 0..n {
            let b = self.out[start + k];
            self.out.push(b);
        }
        Ok(())
    }

    /// Literals that follow a match: the low two bits of the byte two back.
    fn trailing_literals(&mut self) -> usize {
        (self.input[self.ip - 2] & 3) as usize
    }
}

#[derive(Clone, Copy)]
enum State {
    /// Top of the main loop: a literal run or a match.
    Next,
    /// Right after a literal run: `t < 16` means the special 3-byte match.
    AfterLiterals,
    /// `t` holds a match instruction.
    Match(usize),
}

/// Decompresses one LZO1X stream. Returns the output and the number of input
/// bytes consumed (up to and including the end marker). `max_out` bounds the
/// output size (use the size stored next to the stream).
pub fn decompress(input: &[u8], max_out: usize) -> Result<(Vec<u8>, usize), Error> {
    let mut d = Decoder {
        input,
        ip: 0,
        out: Vec::with_capacity(max_out),
        max_out,
    };

    // A first byte above 17 is a literal run of (byte - 17) bytes.
    let mut state = State::Next;
    if let Some(&first) = input.first() {
        if first > 17 {
            d.ip = 1;
            let t = first as usize - 17;
            d.literals(t)?;
            state = if t < 4 {
                State::Match(d.byte()?)
            } else {
                State::AfterLiterals
            };
        }
    }

    loop {
        match state {
            State::Next => {
                let t = d.byte()?;
                if t >= 16 {
                    state = State::Match(t);
                    continue;
                }
                let run = if t == 0 { d.extended_length(15)? } else { t };
                d.literals(run + 3)?;
                state = State::AfterLiterals;
            }
            State::AfterLiterals => {
                let t = d.byte()?;
                if t >= 16 {
                    state = State::Match(t);
                    continue;
                }
                let distance = 1 + 0x0800 + (t >> 2) + (d.byte()? << 2);
                d.copy_match(distance, 3)?;
                state = after_match(&mut d)?;
            }
            State::Match(t) => {
                if t >= 64 {
                    // M2
                    let distance = 1 + ((t >> 2) & 7) + (d.byte()? << 3);
                    d.copy_match(distance, (t >> 5) + 1)?;
                } else if t >= 32 {
                    // M3
                    let len = if t & 31 == 0 {
                        d.extended_length(31)?
                    } else {
                        t & 31
                    };
                    let distance = 1 + (d.le16()? >> 2);
                    d.copy_match(distance, len + 2)?;
                } else if t >= 16 {
                    // M4 (distance 0 is the end of the stream)
                    let high = (t & 8) << 11;
                    let len = if t & 7 == 0 {
                        d.extended_length(7)?
                    } else {
                        t & 7
                    };
                    let distance = high + (d.le16()? >> 2);
                    if distance == 0 {
                        return Ok((d.out, d.ip));
                    }
                    d.copy_match(distance + 0x4000, len + 2)?;
                } else {
                    // M1: a 2-byte match right after another match.
                    let distance = 1 + (t >> 2) + (d.byte()? << 2);
                    d.copy_match(distance, 2)?;
                }
                state = after_match(&mut d)?;
            }
        }
    }
}

fn after_match(d: &mut Decoder) -> Result<State, Error> {
    let n = d.trailing_literals();
    if n == 0 {
        return Ok(State::Next);
    }
    d.literals(n)?;
    Ok(State::Match(d.byte()?))
}

#[cfg(test)]
mod tests {
    use super::*;

    // Hand-assembled stream: 5 literals "hello", an M2 match copying 3 bytes
    // from 5 back ("hel"), then the end marker (M4 with distance 0).
    #[test]
    fn literals_match_and_end() {
        // First byte 17 + 5 = 22: a literal run of 5.
        // M2 with t = 0x40 | (4 << 2): length (t >> 5) + 1 = 3,
        // distance 1 + ((t >> 2) & 7) + (next byte << 3) = 5.
        // 0x11 0x00 0x00: M4 with distance 0, the end marker.
        let t = 0x40 | (4 << 2);
        let stream = [22, b'h', b'e', b'l', b'l', b'o', t, 0x00, 0x11, 0x00, 0x00];
        let (out, used) = decompress(&stream, 64).unwrap();
        assert_eq!(out, b"hellohel");
        assert_eq!(used, stream.len());
    }

    #[test]
    fn rejects_lookbehind_overrun() {
        // M4 (first byte 17) with a distance of 16 KB + 1 into an empty output.
        let stream = [0x11, 0x04, 0x00];
        assert_eq!(
            decompress(&stream, 64).unwrap_err(),
            Error::LookbehindOverrun
        );
    }
}
