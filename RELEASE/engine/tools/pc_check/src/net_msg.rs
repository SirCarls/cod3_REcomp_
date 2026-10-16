//! Requests for `iw_pc::net::{msg, huffman, protocol_data}` (helper "pc-msg").
//!
//! A message buffer arrives as the checker's `Msg` argument kind
//! (`tools/decomp/x86/harness_x86.py`): a `b:` token holding the `msg_t`
//! fields `overflowed`, `maxsize`, `cursize`, `readcount`, `bit` as five
//! big-endian 32-bit numbers, then the data bytes (`maxsize` of them plus
//! the slack the original may run into). The reply carries it back in the
//! same layout.
//!
//! The Huffman ports need the message tree, built from the frequency table
//! in the game's executable: found in the folders `IW_PC_INPUT` names (as
//! the checker's own), read and checked by `iw_pc::net::protocol_data`.

use std::sync::OnceLock;

use iw_pc::net::huffman::{self, MessageTree};
use iw_pc::net::msg::{self, Msg, BIG_INFO_STRING, MAX_STRING_CHARS};
use iw_pc::net::protocol_data::{self, GameExecutable};
use iw_pc::Game;

use crate::{Answer, Args, Ret};

/// Bytes before the data in a `Msg` token: five 32-bit fields.
const MSG_HEADER: usize = 20;

fn field(header: &[u8], index: usize) -> i32 {
    let at = index * 4;
    i32::from_be_bytes([header[at], header[at + 1], header[at + 2], header[at + 3]])
}

fn position(header: &[u8], index: usize, name: &str) -> Result<usize, String> {
    usize::try_from(field(header, index)).map_err(|_| format!("negative msg {name}"))
}

/// Runs `f` on the message in argument `i` and writes the message back into
/// the argument, so the reply carries it.
fn with_msg<R>(
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
    let overflowed = field(header, 0) != 0;
    let maxsize = position(header, 1, "maxsize")?;
    let cursize = position(header, 2, "cursize")?;
    let readcount = position(header, 3, "readcount")?;
    let bit = position(header, 4, "bit")?;
    let mut msg = Msg::new(game, data);
    msg.overflowed = overflowed;
    msg.maxsize = maxsize;
    msg.cursize = cursize;
    msg.readcount = readcount;
    msg.bit = bit;
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

/// A port that returns nothing.
fn void(result: Result<(), String>) -> Answer {
    result.map(|_| Ret::Void)
}

fn int(result: Result<i32, String>) -> Answer {
    result.map(|v| Ret::Int(v.into()))
}

/// The warning a function printed instead of failing, or none.
fn printed<E>(result: Result<Result<(), E>, String>, format: impl Fn(E) -> &'static str) -> Answer {
    result.map(|r| Ret::Printed(r.err().map(|e| format(e).as_bytes().to_vec())))
}

/// Takes buffer argument `i` out of the arguments (to use it next to the
/// message); [`put_back`] returns it for the reply.
fn take_buffer(args: &mut Args, i: usize) -> Result<Vec<u8>, String> {
    args.buffer(i)
        .map(std::mem::take)
        .ok_or_else(|| "NULL buffer".to_string())
}

fn put_back(args: &mut Args, i: usize, buffer: Vec<u8>) {
    if let Some(slot) = args.buffer(i) {
        *slot = buffer;
    }
}

/// A string reader: the message in argument 0, the static buffer the
/// original returns in argument 1 (`N` bytes).
fn read_string<const N: usize>(
    args: &mut Args,
    game: Game,
    read: impl FnOnce(&mut Msg, &mut [u8; N]) -> Vec<u8>,
) -> Answer {
    let buffer = take_buffer(args, 1)?;
    let mut out: [u8; N] = buffer
        .as_slice()
        .try_into()
        .map_err(|_| format!("the string buffer must be {N} bytes"))?;
    let text = with_msg(args, 0, game, |m| read(m, &mut out));
    put_back(args, 1, out.to_vec());
    text.map(|t| Ret::Str(Some(t)))
}

/// The message tree of `game`, built once from its executable.
fn message_tree(game: Game) -> Result<&'static MessageTree, String> {
    static TREES: [OnceLock<Result<MessageTree, String>>; 2] = [OnceLock::new(), OnceLock::new()];
    let slot = &TREES[usize::from(game == Game::UnitedOffensive)];
    slot.get_or_init(|| {
        let folders = std::env::var_os("IW_PC_INPUT").ok_or("IW_PC_INPUT is not set")?;
        let path = protocol_data::find_executable(game, &folders).ok_or_else(|| {
            format!(
                "{} not found in IW_PC_INPUT",
                protocol_data::executable_name(game)
            )
        })?;
        let exe = GameExecutable::open(game, &path).map_err(|e| e.to_string())?;
        let frequencies = exe.huffman_frequencies().map_err(|e| e.to_string())?;
        Ok(MessageTree::from_frequencies(&frequencies))
    })
    .as_ref()
    .map_err(Clone::clone)
}

/// A 32-bit number in a 4-byte buffer argument, as the guest keeps it (little-endian).
fn le_int(args: &mut Args, i: usize) -> Result<i32, String> {
    let bytes = args.buffer(i).ok_or("NULL pointer")?;
    let word: [u8; 4] = bytes
        .get(..4)
        .and_then(|b| b.try_into().ok())
        .ok_or("short int")?;
    Ok(i32::from_le_bytes(word))
}

fn set_le_int(args: &mut Args, i: usize, value: i32) {
    if let Some(bytes) = args.buffer(i) {
        bytes[..4].copy_from_slice(&value.to_le_bytes());
    }
}

/// A global the checker passes as a big-endian 4-byte buffer (`GlobalU32`).
fn global(args: &mut Args, i: usize) -> Result<i32, String> {
    let bytes = args.buffer(i).ok_or("NULL global")?;
    let word: [u8; 4] = bytes
        .get(..4)
        .and_then(|b| b.try_into().ok())
        .ok_or("short global")?;
    Ok(i32::from_be_bytes(word))
}

fn set_global(args: &mut Args, i: usize, value: i32) {
    if let Some(bytes) = args.buffer(i) {
        bytes[..4].copy_from_slice(&value.to_be_bytes());
    }
}

/// The Huffman ports; `None` when `function` isn't one of them.
fn huffman_dispatch(game: Game, function: &str, args: &mut Args) -> Option<Answer> {
    let uo = game == Game::UnitedOffensive;
    let answer = match function {
        "MSG_WriteBitsCompress" | "MSG_ReadBitsCompress" => (|| {
            let tree = message_tree(game)?;
            let length = args.int(2) as usize;
            let input = args.bytes(0)[..length].to_vec();
            let mut out = take_buffer(args, 1)?;
            let n = if function == "MSG_WriteBitsCompress" {
                tree.write_bits_compress(&input, &mut out)
            } else {
                tree.read_bits_compress(&input, &mut out)
            };
            put_back(args, 1, out);
            Ok(Ret::Int(n as i64))
        })(),
        // (huff, ch, fout, &offset), the bit position also left in the global.
        "Huff_offsetTransmit" => (|| {
            let tree = message_tree(game)?;
            let symbol = args.int(1) as u16;
            let mut offset = le_int(args, 3)? as usize;
            let mut out = take_buffer(args, 2)?;
            tree.offset_transmit(symbol, &mut out, &mut offset);
            put_back(args, 2, out);
            set_le_int(args, 3, offset as i32);
            set_global(args, 4, offset as i32);
            Ok(Ret::Void)
        })(),
        // (node, &ch, fin, &offset), the bit position also left in the global.
        "Huff_offsetReceive" => (|| {
            let tree = message_tree(game)?;
            let input = args.bytes(2).to_vec();
            let mut offset = le_int(args, 3)? as usize;
            let symbol = tree.offset_receive(&input, &mut offset);
            set_le_int(args, 1, i32::from(symbol));
            set_le_int(args, 3, offset as i32);
            set_global(args, 4, offset as i32);
            Ok(Ret::Void)
        })(),
        // (huff, ch, fout), the bit position in the global.
        "Huff_transmit" => (|| {
            let tree = message_tree(game)?;
            let symbol = args.int(1) as u16;
            let mut bit = global(args, 3)? as usize;
            let mut out = take_buffer(args, 2)?;
            tree.tree().transmit(symbol, &mut out, &mut bit);
            put_back(args, 2, out);
            set_global(args, 3, bit as i32);
            Ok(Ret::Void)
        })(),
        // (node, &ch, fin) -> the symbol, or 0 without touching ch.
        "Huff_Receive" => (|| {
            let tree = message_tree(game)?;
            let input = args.bytes(2).to_vec();
            let mut bit = global(args, 3)? as usize;
            let symbol = tree.tree().receive(&input, &mut bit);
            if let Some(symbol) = symbol {
                set_le_int(args, 1, i32::from(symbol));
            }
            set_global(args, 3, bit as i32);
            Ok(Ret::Int(symbol.map_or(0, i64::from)))
        })(),
        "Huff_Compress" | "Huff_Decompress" if uo => {
            let offset = args.int(1) as usize;
            let compress = function == "Huff_Compress";
            void(with_msg(args, 0, game, |m| {
                if compress {
                    huffman::compress_adaptive(m, offset)
                } else {
                    huffman::decompress_adaptive(m, offset)
                }
            }))
        }
        _ => return None,
    };
    Some(answer)
}

/// `None` when `function` isn't one of this module's.
pub fn dispatch(game: Game, function: &str, args: &mut Args) -> Option<Answer> {
    if let Some(answer) = huffman_dispatch(game, function, args) {
        return Some(answer);
    }
    let cod2 = game == Game::CallOfDuty2;
    Some(match function {
        // --- bits ------------------------------------------------------------
        "MSG_BeginReading" if cod2 => void(with_msg(args, 0, game, |m| m.begin_reading())),
        "MSG_GetUsedBitCount" if cod2 => {
            int(with_msg(args, 0, game, |m| m.used_bit_count() as i32))
        }
        "MSG_WriteBits" => {
            let (value, bits) = (args.i32(1), args.int(2) as u32);
            void(with_msg(args, 0, game, |m| m.write_bits(value, bits)))
        }
        "MSG_WriteBit0" => void(with_msg(args, 0, game, |m| m.write_bit0())),
        "MSG_WriteBit1" => void(with_msg(args, 0, game, |m| m.write_bit1())),
        "MSG_ReadBits" => {
            // The original takes an int: zero or less reads nothing.
            let bits = args.int(1).max(0) as u32;
            int(with_msg(args, 0, game, |m| m.read_bits(bits)))
        }
        "MSG_ReadBit" => int(with_msg(args, 0, game, |m| m.read_bit())),
        // --- bytes -----------------------------------------------------------
        "MSG_WriteByte" => {
            let c = args.int(1) as u8;
            void(with_msg(args, 0, game, |m| m.write_byte(c)))
        }
        "MSG_WriteData" => {
            let length = args.int(2) as usize;
            let bytes = args.bytes(1)[..length].to_vec();
            void(with_msg(args, 0, game, |m| m.write_data(&bytes)))
        }
        "MSG_WriteShort" => {
            let c = args.int(1) as i16;
            void(with_msg(args, 0, game, |m| m.write_short(c)))
        }
        "MSG_WriteLong" => {
            let c = args.i32(1);
            void(with_msg(args, 0, game, |m| m.write_long(c)))
        }
        "MSG_ReadByte" => int(with_msg(args, 0, game, |m| m.read_byte())),
        "MSG_ReadShort" => int(with_msg(args, 0, game, |m| m.read_short())),
        "MSG_ReadLong" => int(with_msg(args, 0, game, |m| m.read_long())),
        "MSG_ReadData" => {
            let length = args.int(2) as usize;
            let mut out = match take_buffer(args, 1) {
                Ok(out) => out,
                Err(e) => return Some(Err(e)),
            };
            let result = with_msg(args, 0, game, |m| m.read_data(&mut out[..length]));
            put_back(args, 1, out);
            void(result)
        }
        // --- strings ---------------------------------------------------------
        "MSG_WriteString" | "MSG_WriteBigString" => {
            let text = args.text(0).to_vec(); // United Offensive: NULL is ""
            let big = function == "MSG_WriteBigString";
            let result = with_msg(args, 1, game, |m| {
                if big {
                    m.write_big_string(&text)
                } else {
                    m.write_string(&text)
                }
            });
            printed(result, |e: msg::StringTooLong| e.message_format())
        }
        "MSG_ReadString" => {
            read_string::<MAX_STRING_CHARS>(args, game, |m, out| m.read_string(out).to_vec())
        }
        "MSG_ReadBigString" => {
            read_string::<BIG_INFO_STRING>(args, game, |m, out| m.read_big_string(out).to_vec())
        }
        "MSG_ReadStringLine" => {
            read_string::<MAX_STRING_CHARS>(args, game, |m, out| m.read_string_line(out).to_vec())
        }
        "MSG_WriteReliableCommandToBuffer" => {
            let command = args.text(0).to_vec();
            let size = args.int(2) as usize;
            let mut out = match take_buffer(args, 1) {
                Ok(out) => out,
                Err(e) => return Some(Err(e)),
            };
            let result = msg::write_reliable_command_to_buffer(game, &command, &mut out[..size]);
            put_back(args, 1, out);
            printed(Ok(result), |e: msg::ReliableCommandWarning| {
                e.message_format()
            })
        }
        // --- angles ----------------------------------------------------------
        "MSG_WriteAngle" => {
            let angle = args.f32(1);
            void(with_msg(args, 0, game, |m| m.write_angle(angle)))
        }
        "MSG_WriteAngle16" => {
            let angle = args.f32(1);
            void(with_msg(args, 0, game, |m| m.write_angle16(angle)))
        }
        "MSG_ReadAngle16" => with_msg(args, 0, game, |m| m.read_angle16()).map(Ret::F32),
        // --- key-coded values ------------------------------------------------
        "MSG_WriteDeltaKey" => {
            let (key, old, new, bits) = (args.i32(1), args.i32(2), args.i32(3), args.int(4) as u32);
            void(with_msg(args, 0, game, |m| {
                m.write_delta_key(key, old, new, bits)
            }))
        }
        "MSG_ReadDeltaKey" => {
            let (key, old, bits) = (args.i32(1), args.i32(2), args.int(3) as u32);
            int(with_msg(args, 0, game, |m| {
                m.read_delta_key(key, old, bits)
            }))
        }
        "MSG_WriteDeltaKeyByte" | "MSG_WriteDeltaKeyShort" => {
            let (key, old, new) = (args.i32(1), args.i32(2), args.i32(3));
            let byte = function == "MSG_WriteDeltaKeyByte";
            void(with_msg(args, 0, game, |m| {
                if byte {
                    m.write_delta_key_byte(key, old, new)
                } else {
                    m.write_delta_key_short(key, old, new)
                }
            }))
        }
        "MSG_ReadDeltaKeyByte" => {
            let (key, old) = (args.i32(1), args.i32(2));
            int(with_msg(args, 0, game, |m| m.read_delta_key_byte(key, old)))
        }
        "MSG_ReadDeltaKeyShort" => {
            let (key, old) = (args.i32(1), args.i32(2));
            int(with_msg(args, 0, game, |m| {
                m.read_delta_key_short(key, old)
            }))
        }
        _ => return None,
    })
}
