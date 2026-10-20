//! Call of Duty 2 (`CoD2MP_s.exe` 1.3) and Call of Duty: United Offensive
//! (`CoDUOMP.exe` 1.51), the PC games' own code, decompiled into Rust.
//!
//! * **The original layer ([`shared`]): what the PC game does, bit for bit.**
//!   Each function is a port of one function of the game, written from its
//!   x86 code and checked against it: the checker
//!   (`tools/decomp/x86/pcdecomp.py check`) runs the original in a CPU
//!   emulator and the port on thousands of inputs and compares every result
//!   and every byte written. The game's limits stay (1024-byte info strings,
//!   63-character file filters ...), so this layer is the exact record, and
//!   what a Rust client or server must use for anything another player's
//!   game also computes (network formats, shared game rules), so that it can
//!   play with the original PC copies.
//! * **The engine layer:** where a fixed buffer is worth lifting, the remake
//!   uses `cod3_game`'s engine layer, which these games share (see
//!   [`shared::info`]); functions without such limits need no second version.
//!
//! The two games are close relatives (United Offensive is Quake III Team
//! Arena's code, Call of Duty 2 its successor), and Call of Duty 3's Xbox
//! 360 code descends from both. Where a PC original behaves exactly like
//! `cod3_game`'s port of the same function, checked against the PC original
//! too, this crate re-exports that port instead of repeating it, with a doc
//! line saying so. Differences between the three games are documented on
//! each function.
//!
//! # Conventions for the original layer
//!
//! * **Every ported function has an `Original:` line per executable** in its
//!   doc comment: ``/// Original: `Q_stricmpn` at 0x0044A670 (CoD2MP_s.exe 1.3)``.
//!   The name is the one in `tools/decomp/x86/names/<image>.tsv` (Quake III's
//!   name where the function is Quake III's; Call of Duty 2 renamed many of
//!   them `I_*`, which the table's evidence column records). The checker and
//!   `pcdecomp.py status` read these lines.
//! * **Same behavior, Rust shape.** Results are the original's, quirks
//!   included. Strings are byte slices (the games' text is bytes, not
//!   UTF-8); a string in a fixed buffer is a `&mut [u8]` the size of the
//!   game's array (see [`cstr`]); a NULL pointer the original accepts is an
//!   `Option`; `Com_Error` is `Err(`[`GameError`]`)` at the same point with
//!   the same level; a warning the original prints instead (`Com_Printf`)
//!   is an `Err` of the function's own refusal type.
//! * **Characters are signed** where the original uses `movsx`: bytes 0x80
//!   and up compare below every ASCII character. The C runtime's `tolower`
//!   and `toupper` run in the "C" locale (the games never call `setlocale`
//!   on their own), so they only change ASCII letters ([`crt`]).
//! * **The registers an original takes its arguments in don't show here.**
//!   Built with whole-program optimization, MSVC 7.1 passed most arguments
//!   of the games' internal functions in registers; the checker's specs
//!   (`tools/decomp/x86/specs/`) say where each one goes.
//! * **Floating point follows the x87 and the game's precision control.**
//!   Call of Duty 2 runs at 24-bit precision (its `Com_Frame` sets it every
//!   frame), so a chain of `float` operations kept in x87 registers rounds
//!   like `f32` arithmetic, except that values beyond `f32`'s exponent range
//!   stay finite (and normal) until they are stored. United Offensive runs at
//!   the C runtime's 53-bit default: intermediate results round like `f64`,
//!   and storing them to a `float` rounds a second time. Ports reproduce the
//!   original's order of operations and stores. (`net::msg`'s angle writers
//!   are the first ports that do; they were checked at each precision, see
//!   `dev/agents/cod2-uo-decomp/pc-msg.md`.)
//! * **No game data.** Constants are named, with where they come from. Tables
//!   the network protocol needs (the Huffman frequencies, the delta field
//!   tables) are game data: they are read at run time from the player's own
//!   executable and checked by hash (`net::protocol_data`), never stored here.
//!
//! Quake III's source (GPL) helps to name and understand functions, but code
//! is never copied from it: every port is written from the game's machine
//! code, which differs from Quake III in places (see [`shared::strings::stricmpn`]).

#![forbid(unsafe_code)]

pub mod client;
pub mod crt;
pub mod cstr;
pub mod fs;
pub mod game;
pub mod net;
pub mod server;
pub mod shared;
pub mod x87;

pub use cod3_game::error::{ErrorLevel, GameError};

/// Which of the two games, where they differ only in a detail (a message's text).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Game {
    /// Call of Duty 2, `CoD2MP_s.exe` 1.3.
    CallOfDuty2,
    /// Call of Duty: United Offensive, `CoDUOMP.exe` 1.51.
    UnitedOffensive,
}
