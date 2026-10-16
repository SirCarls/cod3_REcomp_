//! The games' network code: what a Rust client or server needs to speak the
//! PC protocol of Call of Duty 2 1.3 and United Offensive 1.51 byte for byte,
//! so that the original PC copies and the Rust version play together
//! (`docs/PC_MULTIPLAYER.md`).
//!
//! * [`msg`]: the message buffer (bits and bytes interleaved, strings,
//!   angles, key-coded values) and the whole-message Huffman step.
//! * [`huffman`]: the fixed message tree and United Offensive's adaptive
//!   coder (its `connect` packet).
//! * [`protocol_data`]: the tables the protocol needs that are game data
//!   (the Huffman frequencies, the delta field tables): read at start from
//!   the player's own executable and checked by hash, never stored here.
//! * [`oob`]: packets without a connection (`getinfo`, `getstatus`,
//!   `getchallenge`, `connect` ...), the challenge table.
//! * [`delta`]: delta coding: entities, clients and the player's own
//!   state sent as the fields that changed, from the executable's field
//!   tables.
//! * [`game_state`]: the game state and snapshot messages.
//! * [`netchan`]: the connection layer: header, fragments, ordering, and
//!   the XOR scrambling of message bodies.

pub mod delta;
pub mod game_state;
pub mod huffman;
pub mod msg;
pub mod netchan;
pub mod oob;
pub mod protocol_data;
