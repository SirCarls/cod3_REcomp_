//! What the movement asks of the world: traces (a box swept from one point
//! to another), answered by the game's own trace functions. Call of Duty 2
//! keeps two (the server's and the client game's); a move's `handler` byte
//! ([`super::pmove::pm::HANDLER`]) picks one. They are the host's
//! ([`MoveHost`]): the collision model isn't ported yet.

/// Bytes of Call of Duty 2's trace result (`trace_t`; it has no end point:
/// callers compute it from the fraction).
pub const TRACE_SIZE: usize = 0x24;

/// Where a trace result keeps its fields (offsets in `trace_t`).
pub mod at {
    /// How far the box got (0 to 1).
    pub const FRACTION: usize = 0x00;
    /// The normal of the surface hit.
    pub const NORMAL: usize = 0x04;
    pub const SURFACE_FLAGS: usize = 0x10;
    pub const CONTENTS: usize = 0x14;
    /// The entity hit (16 bits; `ENTITYNUM_NONE` for nothing, `ENTITYNUM_WORLD`
    /// for the world).
    pub const ENTITY_NUM: usize = 0x1C;
    /// The box is inside something solid all the way (a byte).
    pub const ALL_SOLID: usize = 0x22;
    /// The box started inside something solid (a byte).
    pub const START_SOLID: usize = 0x23;
}

/// The contents bit of players' and other bodies' boxes (`CONTENTS_BODY`).
pub const CONTENTS_BODY: i32 = 0x0200_0000;

/// A trace's result: the game's structure as its bytes.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Trace {
    pub bytes: [u8; TRACE_SIZE],
}

impl Trace {
    pub fn int(&self, at: usize) -> i32 {
        let b = &self.bytes[at..at + 4];
        i32::from_le_bytes([b[0], b[1], b[2], b[3]])
    }

    pub fn float(&self, at: usize) -> f32 {
        f32::from_bits(self.int(at) as u32)
    }

    pub fn fraction(&self) -> f32 {
        self.float(at::FRACTION)
    }

    pub fn normal(&self) -> [f32; 3] {
        [
            self.float(at::NORMAL),
            self.float(at::NORMAL + 4),
            self.float(at::NORMAL + 8),
        ]
    }

    pub fn contents(&self) -> i32 {
        self.int(at::CONTENTS)
    }

    pub fn entity_num(&self) -> u16 {
        u16::from_le_bytes([self.bytes[at::ENTITY_NUM], self.bytes[at::ENTITY_NUM + 1]])
    }

    pub fn all_solid(&self) -> bool {
        self.bytes[at::ALL_SOLID] != 0
    }

    pub fn start_solid(&self) -> bool {
        self.bytes[at::START_SOLID] != 0
    }
}

impl Default for Trace {
    fn default() -> Self {
        Trace {
            bytes: [0; TRACE_SIZE],
        }
    }
}

/// What the movement needs from the rest of the game.
pub trait MoveHost {
    /// The box (`mins`, `maxs`) swept from `start` to `end` through what
    /// `content_mask` collides with, ignoring entity `pass_entity`, by the
    /// game's trace function `handler` (0 the server's, 1 the client
    /// game's).
    #[allow(clippy::too_many_arguments)]
    fn trace(
        &mut self,
        handler: u8,
        start: &[f32; 3],
        mins: &[f32; 3],
        maxs: &[f32; 3],
        end: &[f32; 3],
        pass_entity: i32,
        content_mask: i32,
    ) -> Trace;
}
