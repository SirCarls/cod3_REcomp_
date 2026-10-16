//! The level scripts' library for the engine.
//!
//! The scripts' math and random numbers have no console limit to lift; they
//! are the console's, so scripted events play out the same (the same random
//! sequence from the same seed, the same distances and angles).

pub use crate::script::builtins;
pub use crate::script::random::{ScriptRandom, INITIAL_SEED};
