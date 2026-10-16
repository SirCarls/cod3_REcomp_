//! Network code: what goes between machines, so the console layer only.
//!
//! Every platform in a match must send the same bytes, so nothing here has
//! an engine-layer version without the console's limits. Two layers, very
//! different in CoD3 (`docs/MP_PROTOCOL.md`):
//!
//! * [`msg`] and [`adr`]: Quake III's messages (`msg_t`) and addresses
//!   (`netadr_t`). Both programs have them; in the multiplayer program they
//!   only link a console's own server to its own split-screen players,
//!   through in-memory queues. Their `Original:` lines give the campaign's
//!   address and the multiplayer program's; `decomp.py check --image codmp`
//!   checks the ports against the multiplayer program's copies too.
//! * [`bit_buffer`]: Demonware's bit buffer, which packs the messages the
//!   consoles really send each other (every console sends its own players'
//!   state to every other console). Multiplayer program only; its
//!   `Original:` addresses are the multiplayer program's.
//! * [`fields`]: the multiplayer program's message fields on top of the bit
//!   buffer (positions, angles, directions, velocities), which the player
//!   state messages are made of.
//! * [`position`]: the position update (message 0x23), each console's
//!   report of its own players; [`view`]: the view update (0x24) that
//!   replaces it in vehicles and on turrets; [`vehicle`]: the vehicle update (0x4C).

pub mod adr;
pub mod bit_buffer;
pub mod fields;
pub mod msg;
pub mod position;
pub mod vehicle;
pub mod view;
