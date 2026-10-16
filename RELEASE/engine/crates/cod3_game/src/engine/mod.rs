//! The engine layer: the console's results without the Xbox 360's limits.
//!
//! The rest of this crate (`shared`, `math`, `script`) is the exact record
//! of what the console does, checked against the original machine code,
//! limits included: 1024-byte info strings, 128-byte paths, 27-character
//! name copies, angles snapped to 16 bits, fatal errors where a buffer would
//! overflow. The remake uses this layer instead. Each function here is
//! tested against its console version on every input the console accepts,
//! and simply keeps working beyond those limits.
//!
//! What isn't a limit stays the console's: the name hash (scripts and
//! ripped data refer to names by it), the scripts' random sequence, the
//! deterministic `sin`/`cos` and the order of float operations (identical on
//! every PC, which Windows/Linux multiplayer needs), and format rules.
//!
//! # Multiplayer: the wire keeps the console's limits
//!
//! One game can mix an Xbox 360 (a custom copy), the ReXGlue recomp and
//! this engine, and the 360 and the recomp *are* the console. So what the
//! engine sends to a peer, and the game rules every peer runs (movement,
//! anything predicted or checked on both ends), use the console's
//! functions and stay within the console's limits: for example an info
//! string must pass [`info::InfoString::fits_console`], and angles in the
//! shared game rules are the console's 16-bit ones, not
//! [`math::angles`]' full precision. The lifted limits are for what stays
//! on one machine: the menus, the camera, local files and tools.
//!
//! # Speed
//!
//! The engine runs every frame, so these functions allocate nothing on
//! their hot paths (except where they return new text), take time in
//! proportion to their input at most, and are measured by
//! `cargo run --release -p cod3_game --example bench` (numbers in
//! `docs/DECOMP.md`).

pub mod info;
pub mod math;
pub mod names;
pub mod script;
pub mod strings;
