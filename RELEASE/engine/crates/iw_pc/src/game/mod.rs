//! The game's shared code (`bg_*`): what the server's game and the client
//! game both run, so that a client predicts what the server will do.
//! Ports of the original layer (see the crate's conventions).

pub mod moves;
pub mod pmove;
pub mod prone;
pub mod slide;
pub mod trace;
