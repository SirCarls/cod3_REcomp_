//! Call of Duty 3's own code (`default.xex`), decompiled into Rust, in two layers.
//!
//! * **The console layer ([`shared`], [`math`], [`script`], [`net`]): what the Xbox 360
//!   does, bit for bit.** Each function is a port of one function of the game,
//!   written from its PowerPC code and checked against it: the checker
//!   (`tools/decomp/decomp.py check`) runs the original in a CPU emulator and
//!   the port on thousands of inputs and compares every result and every byte
//!   written. This layer keeps the console's limits too (fixed buffers, length
//!   caps, 16-bit angles), so it is the exact record of the original game.
//! * **The engine layer ([`engine`]): the same results without the 360's
//!   limits.** [`engine::info::InfoString`] has no 1024-byte cap,
//!   [`engine::names::Name`] keeps the whole text, [`engine::strings`] has no
//!   buffers to overflow, [`engine::math::angles`] keeps full-precision angles.
//!   Each engine function is tested against its console version on every input
//!   the console accepts, so the engine behaves like the game wherever the game
//!   works, and keeps working where the 360 would have run out of room. The
//!   remake uses this layer for what stays on one machine; what goes over
//!   the network, and the game rules every player runs, stay the console
//!   layer's, because a match can mix an Xbox 360, the recomp and the
//!   engine (see [`engine`]).
//!
//! What is *not* a limit stays exactly the console's: hashes (the scripts
//! and the ripped data refer to names by them), the scripts' random number
//! sequence, the deterministic `sin`/`cos` and vector math (identical on
//! every PC, which Windows/Linux multiplayer needs). The workflow is in
//! `docs/DECOMP.md`.
//!
//! # Conventions for the console layer
//!
//! * **Every ported function has an `Original:` line** in its doc comment:
//!   ``/// Original: `Info_ValueForKey` at 0x82457570``. The name is the
//!   one in the symbol map (`recomp/config/symbols.toml`); the checker and
//!   `decomp.py status` read these lines. (Engine-layer functions don't
//!   have one; they name their console version instead.)
//! * **Same behavior, Rust shape.** Results are the original's, including
//!   its quirks (documented where they differ from Quake III or common
//!   sense). Strings are byte slices; a string the game keeps in a fixed
//!   buffer is a `&mut [u8]` holding a NUL-terminated string (see [`cstr`]);
//!   the game's fatal errors are `Err(`[`GameError`]`)`.
//! * **Floating point follows the PowerPC.** Single-precision instructions
//!   are `f32` operations; a fused multiply-add (`fmadds`) is
//!   [`ppc::fmadds`] (exact on every platform); conversions use the
//!   helpers in [`ppc`], which saturate like the hardware.
//! * **No game data.** Constants that are the game's own numbers are named
//!   `const`s with where they come from; nothing is read from game files.
//!
//! Quake III's source (GPL) helps to name and understand functions the
//! game inherited, but code is never copied from it: every port is written
//! from the game's machine code, which also differs from Quake III in
//! places (see [`shared::strings::stricmp`]).

#![forbid(unsafe_code)]

pub mod bytes;
pub mod cstr;
pub mod engine;
pub mod error;
pub mod math;
pub mod net;
pub mod ppc;
pub mod script;
pub mod shared;
#[cfg(test)]
mod test_random;

pub use error::GameError;
