//! The level scripts' library ("Broc").
//!
//! CoD3's campaign scripts are C++ compiled into each level's DLL. They
//! reach the engine through one table of about 1,000 function pointers, the
//! level interface table, by byte offset ("slot"). These are the engine's
//! side of that table: each port names its slot.

pub mod builtins;
pub mod random;
