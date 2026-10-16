//! NUL-terminated strings in fixed-size byte buffers, the way the games keep text.
//!
//! The same helpers as Call of Duty 3's port (re-exported from
//! [`cod3_game::cstr`]): a **string argument** is a `&[u8]` holding just the
//! characters, and reading past its end gives 0 ([`at`]); a **string
//! buffer** is a `&mut [u8]` the size of the game's array, holding the
//! characters, a 0, then whatever was there before ([`len`], [`text`]).
//! [`signed_at`] is a byte as x86's `movsx` loads it (a signed char), the
//! same as the PowerPC's `lbz` + `extsb` it was named for.

pub use cod3_game::cstr::{append, at, len, signed_at, text, write_at};
