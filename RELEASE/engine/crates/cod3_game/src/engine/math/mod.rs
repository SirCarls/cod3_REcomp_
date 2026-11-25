//! The engine's math: the console's functions where they have no limit to
//! lift (vectors, deterministic `sin`/`cos`), full-precision angles where the
//! console snaps them to 16 bits.

pub mod angles;

pub use crate::math::angles::Vec3;
pub use crate::math::crt::{cos, sin};
pub use crate::math::vector::{
    distance, distance_2d, distance_squared_2d, length_squared, normalize, normalize_2d, normalized,
};
