//! Answers `tools/decomp/x86/pcdecomp.py check`: runs the PC games' Rust
//! ports (`iw_pc`) on the checker's test cases, one request per line on
//! stdin, one reply per line on stdout. The protocol is the one
//! `decomp_check` speaks (described in `tools/decomp/x86/harness_x86.py`):
//!
//! ```text
//! request: <image>.<function> <arg>...      reply: ok <result> <buffer>...   or   err <reason>
//! i:<decimal>  f:<f32 bits, 8 hex>  d:<f64 bits, 16 hex>  s:<hex string> | s:-  v:<hex bytes, read only>
//! b:<hex bytes the function may change> | b:-   n: (no result)   p:<hex message> | p:- (printed or not)
//! ```
//!
//! Function names carry the executable they belong to (`cod2mp.Q_stricmp`,
//! `uomp.Q_stricmp`), since the two games' functions of the same name can
//! differ. `cod3.<name>` runs Call of Duty 3's port (`cod3_game`) on the
//! same arguments, for `pcdecomp.py check --against cod3`. A game error is
//! `err Com_Error <level>`, the level as the game's number (0 fatal, 1 drop).

#![forbid(unsafe_code)]

mod cod3;
mod net_delta;
mod net_msg;
mod net_netchan;
mod net_oob;
mod pc;

use std::io::{self, BufRead, Write};

use iw_pc::{ErrorLevel, GameError};

/// One decoded argument.
pub enum Arg {
    Int(i64),
    F32(f32),
    F64(f64),
    Str(Option<Vec<u8>>),
    Bytes(Vec<u8>),
    Buffer(Option<Vec<u8>>),
}

pub struct Args(pub Vec<Arg>);

/// What a port returned.
pub enum Ret {
    Void,
    Int(i64),
    F32(f32),
    F64(f64),
    Str(Option<Vec<u8>>),
    /// The message a function printed instead of failing (its format), or none.
    Printed(Option<Vec<u8>>),
}

pub type Answer = Result<Ret, String>;

/// The reply for a port that stopped with the game's error.
pub fn game_error(e: GameError) -> String {
    let level = match e.level {
        ErrorLevel::Fatal => 0,
        ErrorLevel::Drop => 1,
    };
    format!("{} {level}", e.exit_name())
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

    /// A string argument; a NULL pointer is `None`.
    pub fn str(&self, i: usize) -> Option<&[u8]> {
        match &self.0[i] {
            Arg::Str(v) => v.as_deref(),
            _ => panic!("argument {i} is not a string"),
        }
    }

    /// A string argument, NULL read as empty.
    pub fn text(&self, i: usize) -> &[u8] {
        self.str(i).unwrap_or(b"")
    }

    /// Read-only bytes.
    pub fn bytes(&self, i: usize) -> &[u8] {
        match &self.0[i] {
            Arg::Bytes(v) => v,
            Arg::Buffer(Some(v)) => v,
            _ => panic!("argument {i} is not bytes"),
        }
    }

    /// A buffer the function may change; `None` for a NULL pointer.
    pub fn buffer(&mut self, i: usize) -> Option<&mut Vec<u8>> {
        match &mut self.0[i] {
            Arg::Buffer(v) => v.as_mut(),
            _ => panic!("argument {i} is not a buffer"),
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
        _ => return Err(format!("unknown token kind {kind:?}")),
    })
}

fn dispatch(name: &str, args: &mut Args) -> Option<Answer> {
    let (image, function) = name.split_once('.')?;
    match image {
        "cod2mp" | "uomp" => pc::dispatch(image, function, args),
        "cod3" => cod3::dispatch(function, args),
        _ => None,
    }
}

fn answer(line: &str) -> String {
    let mut parts = line.split(' ');
    let name = parts.next().unwrap_or("");
    let parsed: Result<Vec<Arg>, String> = parts.filter(|p| !p.is_empty()).map(parse_arg).collect();
    let mut args = match parsed {
        Ok(a) => Args(a),
        Err(e) => return format!("err bad-request {e}"),
    };
    let ret = match dispatch(name, &mut args) {
        None => return format!("err unknown-function {name}"),
        Some(Err(reason)) => return format!("err {reason}"),
        Some(Ok(r)) => r,
    };
    let mut reply = String::from("ok ");
    reply += &match ret {
        Ret::Void => "n:".to_string(),
        Ret::Int(v) => format!("i:{v}"),
        Ret::F32(v) => format!("f:{:08x}", v.to_bits()),
        Ret::F64(v) => format!("d:{:016x}", v.to_bits()),
        Ret::Str(None) => "s:-".to_string(),
        Ret::Str(Some(s)) => format!("s:{}", hex(&s)),
        Ret::Printed(None) => "p:-".to_string(),
        Ret::Printed(Some(s)) => format!("p:{}", hex(&s)),
    };
    for arg in &args.0 {
        if let Arg::Buffer(b) = arg {
            reply += &match b {
                None => " b:-".to_string(),
                Some(b) => format!(" b:{}", hex(b)),
            };
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

#[cfg(test)]
mod tests {
    use super::answer;

    #[test]
    fn answers_the_protocol() {
        assert_eq!(answer("cod2mp.Q_stricmp s:41 s:61"), "ok i:0");
        assert_eq!(answer("uomp.Q_stricmp s:- s:61"), "ok i:-1");
        assert_eq!(answer("cod2mp.Q_strupr b:610062"), "ok s:41 b:410062");
        assert_eq!(answer("cod2mp.Q_strcat b:6162 i:2 s:63"), "err Com_Error 0");
        assert!(answer("nope.Q_stricmp").starts_with("err unknown-function"));
        assert_eq!(answer("cod3.Q_stricmp s:61 s:62"), "ok i:-1");
    }
}
