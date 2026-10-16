//! Answers `tools/decomp/decomp.py check`: runs the Rust ports on the
//! checker's test cases, one request per line on stdin, one reply per line
//! on stdout. The protocol is described in `tools/decomp/harness.py`:
//!
//! ```text
//! request: <function> <arg>...        reply: ok <result> <buffer>...   or   err <reason>
//! i:<decimal>  f:<f32 bits, 8 hex>  d:<f64 bits, 16 hex>  s:<hex string> | s:-  v:<hex bytes, read only>
//! b:<hex bytes the function may change> | b:-   n: (no result)
//! m:<+0>,<maxsize>,<cursize>,<readcount>,<hex data>   a msg_t and its data buffer
//! ```
//!
//! Every `b:` and `m:` argument comes back in the reply, in order, as the
//! function left it. Each area of the game has its own file here (`shared.rs`,
//! `math.rs` ...), so agents working on different areas don't edit the
//! same file; `dispatch` asks each in turn.

#![forbid(unsafe_code)]

mod bit_buffer;
mod fields;
mod math;
mod net;
mod position;
mod script;
mod shared;
mod vehicle;

use std::io::{self, BufRead, Write};

use cod3_game::GameError;

/// One decoded argument.
pub enum Arg {
    Int(i64),
    F32(f32),
    F64(f64),
    Str(Option<Vec<u8>>),
    Bytes(Vec<u8>),
    Buffer(Option<Vec<u8>>),
    Msg(MsgArg),
}

/// A network message (`msg_t`) and its data buffer, from an `m:` token: the
/// structure's integer fields and the bytes its `data` points at.
pub struct MsgArg {
    pub overflowed: i32,
    pub maxsize: i32,
    pub cursize: i32,
    pub readcount: i32,
    pub data: Vec<u8>,
}

impl MsgArg {
    fn parse(text: &str) -> Result<MsgArg, String> {
        let fields: Vec<&str> = text.splitn(5, ',').collect();
        let [overflowed, maxsize, cursize, readcount, data] = fields[..] else {
            return Err(format!("bad message {text:?}"));
        };
        let int = |t: &str| t.parse::<i32>().map_err(|e| format!("{t:?}: {e}"));
        Ok(MsgArg {
            overflowed: int(overflowed)?,
            maxsize: int(maxsize)?,
            cursize: int(cursize)?,
            readcount: int(readcount)?,
            data: parse_hex(data)?,
        })
    }

    fn token(&self) -> String {
        format!(
            "m:{},{},{},{},{}",
            self.overflowed,
            self.maxsize,
            self.cursize,
            self.readcount,
            hex(&self.data)
        )
    }
}

pub struct Args(pub Vec<Arg>);

/// What a port returned.
pub enum Ret {
    Void,
    Int(i64),
    F32(f32),
    F64(f64),
    Str(Option<Vec<u8>>),
}

pub type Answer = Result<Ret, String>;

/// The reply for a port that stopped with the game's error: `Com_Error Drop` / `Com_Error Fatal`.
pub fn game_error(e: GameError) -> String {
    format!("{} {:?}", e.exit_name(), e.level)
}

/// Reads a `vec2_t`/`vec3_t`-like run of big-endian floats.
pub fn floats<const N: usize>(bytes: &[u8]) -> [f32; N] {
    let mut out = [0.0; N];
    for (k, v) in out.iter_mut().enumerate() {
        *v = f32::from_be_bytes([
            bytes[k * 4],
            bytes[k * 4 + 1],
            bytes[k * 4 + 2],
            bytes[k * 4 + 3],
        ]);
    }
    out
}

/// Writes floats back as big-endian bytes.
pub fn store_floats(bytes: &mut [u8], values: &[f32]) {
    for (k, v) in values.iter().enumerate() {
        bytes[k * 4..k * 4 + 4].copy_from_slice(&v.to_be_bytes());
    }
}

impl Args {
    pub fn int(&self, i: usize) -> i64 {
        match &self.0[i] {
            Arg::Int(v) => *v,
            _ => panic!("argument {i} is not an integer"),
        }
    }

    pub fn i32(&self, i: usize) -> i32 {
        self.int(i) as i32
    }

    pub fn f32(&self, i: usize) -> f32 {
        match &self.0[i] {
            Arg::F32(v) => *v,
            _ => panic!("argument {i} is not a float"),
        }
    }

    pub fn f64(&self, i: usize) -> f64 {
        match &self.0[i] {
            Arg::F64(v) => *v,
            _ => panic!("argument {i} is not a double"),
        }
    }

    /// A 32-bit global passed as a 4-byte buffer (big-endian).
    pub fn global_u32(&mut self, i: usize) -> u32 {
        let b = self.buffer(i).expect("a global is never NULL");
        u32::from_be_bytes([b[0], b[1], b[2], b[3]])
    }

    pub fn set_global_u32(&mut self, i: usize, value: u32) {
        if let Some(b) = self.buffer(i) {
            b[..4].copy_from_slice(&value.to_be_bytes());
        }
    }

    /// A string argument; a NULL pointer is `None`.
    pub fn str(&self, i: usize) -> Option<&[u8]> {
        match &self.0[i] {
            Arg::Str(v) => v.as_deref(),
            _ => panic!("argument {i} is not a string"),
        }
    }

    /// Read-only bytes (a vector or structure the function only reads).
    pub fn bytes(&self, i: usize) -> &[u8] {
        match &self.0[i] {
            Arg::Bytes(v) => v,
            Arg::Buffer(Some(v)) => v,
            _ => panic!("argument {i} is not bytes"),
        }
    }

    /// `N` big-endian floats, as the guest keeps a `vec2_t` / `vec3_t`.
    pub fn floats<const N: usize>(&self, i: usize) -> [f32; N] {
        floats(self.bytes(i))
    }

    /// A buffer the function may change; `None` for a NULL pointer.
    pub fn buffer(&mut self, i: usize) -> Option<&mut Vec<u8>> {
        match &mut self.0[i] {
            Arg::Buffer(v) => v.as_mut(),
            _ => panic!("argument {i} is not a buffer"),
        }
    }

    /// Stores floats into a buffer argument (nothing for a NULL one).
    pub fn set_floats(&mut self, i: usize, values: &[f32]) {
        if let Some(b) = self.buffer(i) {
            store_floats(b, values);
        }
    }

    /// A network message the function may change (`m:`).
    pub fn msg(&mut self, i: usize) -> &mut MsgArg {
        match &mut self.0[i] {
            Arg::Msg(m) => m,
            _ => panic!("argument {i} is not a message"),
        }
    }
}

fn parse_hex(text: &str) -> Result<Vec<u8>, String> {
    if text.len() % 2 != 0 {
        return Err(format!("odd hex length in {text:?}"));
    }
    (0..text.len())
        .step_by(2)
        .map(|i| u8::from_str_radix(&text[i..i + 2], 16).map_err(|e| e.to_string()))
        .collect()
}

fn hex(bytes: &[u8]) -> String {
    bytes.iter().map(|b| format!("{b:02x}")).collect()
}

fn parse_arg(token: &str) -> Result<Arg, String> {
    let (kind, value) = token
        .split_once(':')
        .ok_or_else(|| format!("bad token {token:?}"))?;
    Ok(match kind {
        "i" => Arg::Int(value.parse().map_err(|e| format!("{token:?}: {e}"))?),
        "f" => Arg::F32(f32::from_bits(
            u32::from_str_radix(value, 16).map_err(|e| e.to_string())?,
        )),
        "d" => Arg::F64(f64::from_bits(
            u64::from_str_radix(value, 16).map_err(|e| e.to_string())?,
        )),
        "s" if value == "-" => Arg::Str(None),
        "s" => Arg::Str(Some(parse_hex(value)?)),
        "v" => Arg::Bytes(parse_hex(value)?),
        "b" if value == "-" => Arg::Buffer(None),
        "b" => Arg::Buffer(Some(parse_hex(value)?)),
        "m" => Arg::Msg(MsgArg::parse(value)?),
        _ => return Err(format!("unknown token kind {kind:?}")),
    })
}

fn dispatch(name: &str, args: &mut Args) -> Option<Answer> {
    shared::dispatch(name, args)
        .or_else(|| math::dispatch(name, args))
        .or_else(|| script::dispatch(name, args))
        .or_else(|| net::dispatch(name, args))
        .or_else(|| bit_buffer::dispatch(name, args))
        .or_else(|| fields::dispatch(name, args))
        .or_else(|| position::dispatch(name, args))
        .or_else(|| vehicle::dispatch(name, args))
}

fn answer(line: &str) -> String {
    let mut parts = line.split(' ');
    let name = parts.next().unwrap_or("");
    let parsed: Result<Vec<Arg>, String> = parts.filter(|p| !p.is_empty()).map(parse_arg).collect();
    let mut args = match parsed {
        Ok(a) => Args(a),
        Err(e) => return format!("err bad-request {e}"),
    };
    let result = match dispatch(name, &mut args) {
        None => return format!("err unknown-function {name}"),
        Some(r) => r,
    };
    let ret = match result {
        Err(reason) => return format!("err {reason}"),
        Ok(r) => r,
    };
    let mut reply = String::from("ok ");
    reply += &match ret {
        Ret::Void => "n:".to_string(),
        Ret::Int(v) => format!("i:{v}"),
        Ret::F32(v) => format!("f:{:08x}", v.to_bits()),
        Ret::F64(v) => format!("d:{:016x}", v.to_bits()),
        Ret::Str(None) => "s:-".to_string(),
        Ret::Str(Some(s)) => format!("s:{}", hex(&s)),
    };
    for arg in &args.0 {
        match arg {
            Arg::Buffer(None) => reply += " b:-",
            Arg::Buffer(Some(b)) => reply += &format!(" b:{}", hex(b)),
            Arg::Msg(m) => reply += &format!(" {}", m.token()),
            _ => {}
        }
    }
    reply
}

fn main() {
    let stdin = io::stdin();
    let mut out = io::BufWriter::new(io::stdout().lock());
    for line in stdin.lock().lines() {
        let Ok(line) = line else { break };
        let reply = answer(line.trim_end());
        if writeln!(out, "{reply}").and_then(|_| out.flush()).is_err() {
            break;
        }
    }
}
