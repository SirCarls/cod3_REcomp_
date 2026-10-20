//! Player movement (`bg_pmove.c`): what turns a player's commands into
//! motion, run by the server for every player and by the client game to
//! predict its own. Both sides must compute the same numbers, so these are
//! ports of the original layer: each function written from the game's x86
//! code and checked against it (`tools/decomp/x86/specs/game_pmove.py`).
//!
//! The player's state is the game's own structure, kept as its bytes
//! ([`PlayerState`]: the network's player state, `playerState_t`), so that
//! what a port doesn't touch stays exactly as the original leaves it;
//! [`MoveLocal`] is the move's scratch state (`pml_t`) the same way.
//!
//! Call of Duty 2 only so far: its movement is in `CoD2MP_s.exe`, at 24-bit
//! x87 precision (`f32` arithmetic, see the crate's conventions). United
//! Offensive's is in its game module (`uo_game_mp_x86.dll`), not read yet.

use super::trace::{MoveHost, Trace, CONTENTS_BODY};
use crate::shared::math::{
    angle_normalize180, vec2_normalize, vec3_normalize, vec3_normalize_to, vec_to_angles, vectoyaw,
};
use crate::x87::F24;

/// Bytes of Call of Duty 2's player state (`playerState_t`).
pub const PLAYER_STATE_SIZE: usize = 0x26A8;

/// Where Call of Duty 2 keeps a player state's fields (offsets in
/// `playerState_t`; most named by the player state's delta table).
pub mod ps {
    pub const PM_TYPE: usize = 0x04;
    /// The legs' bob cycle (an integer, 0 to 255): footsteps fall on its halves.
    pub const BOB_CYCLE: usize = 0x08;
    pub const PM_FLAGS: usize = 0x0C;
    pub const PM_TIME: usize = 0x10;
    pub const ORIGIN: usize = 0x14;
    pub const VELOCITY: usize = 0x20;
    /// The horizontal velocity before this move (two floats; not sent).
    pub const OLD_VELOCITY: usize = 0x2C;
    /// How far the player leans (-1 to 1).
    pub const LEANF: usize = 0x4C;
    /// Gravity (an integer, units per second squared).
    pub const GRAVITY: usize = 0x48;
    /// The player's speed (an integer, units per second).
    pub const SPEED: usize = 0x50;
    /// The entity the player stands on ([`super::ENTITY_NONE`]: none).
    pub const GROUND_ENTITY_NUM: usize = 0x60;
    /// The ladder's direction (`vLadderVec`).
    pub const LADDER_VEC: usize = 0x64;
    pub const JUMP_TIME: usize = 0x70;
    pub const JUMP_ORIGIN_Z: usize = 0x74;
    pub const LEGS_TIMER: usize = 0x78;
    pub const TORSO_TIMER: usize = 0x80;
    /// Where the player moves relative to where he looks, in degrees
    /// (-90 to 90; an integer).
    pub const MOVEMENT_DIR: usize = 0x9C;
    pub const E_FLAGS: usize = 0xA0;
    /// How many events the player state has had (`eventSequence`); the
    /// last four events and their parameters (`events`, `eventParms`).
    pub const EVENT_SEQUENCE: usize = 0xA4;
    pub const EVENTS: usize = 0xA8;
    pub const EVENT_PARMS: usize = 0xB8;
    /// Pitch, yaw, roll.
    pub const VIEW_ANGLES: usize = 0xE8;
    /// The view height the player's stance moves toward (an integer).
    pub const VIEW_HEIGHT_TARGET: usize = 0xF4;
    /// The player's entity number.
    pub const CLIENT_NUM: usize = 0xCC;
    /// The half width the prone checks give the body (its name not known).
    pub const PRONE_CHECK_SIZE: usize = 0x578;
    /// The yaw a prone player's body points at.
    pub const PRONE_DIRECTION: usize = 0x584;
    /// What [`super::prone::check_prone`] gives a prone player: the torso's
    /// height (0), the torso's and the waist's pitch.
    pub const PRONE_TORSO_HEIGHT: usize = 0x5A8;
    pub const PRONE_TORSO_PITCH: usize = 0x5AC;
    pub const PRONE_WAIST_PITCH: usize = 0x5B0;
}

/// The entity number of "no entity" (`ENTITYNUM_NONE`).
pub const ENTITY_NONE: i32 = 0x3FF;
/// The entity number of the world (`ENTITYNUM_WORLD`).
pub const ENTITY_WORLD: i32 = 0x3FE;

/// The view heights of crouching and lying down (`viewHeightTarget`).
pub const CROUCH_VIEW_HEIGHT: i32 = 40;
pub const PRONE_VIEW_HEIGHT: i32 = 11;

/// Bytes of Call of Duty 2's `pmove_t` the ports know so far.
pub const PMOVE_SIZE: usize = 0xE8;
/// Bytes of a player command (`usercmd_t`).
pub const USERCMD_SIZE: usize = 28;
/// The most entities a move records touching (`MAXTOUCH`).
pub const MAX_TOUCH: i32 = 32;

/// Where Call of Duty 2 keeps a move's fields (offsets in `pmove_t`; the
/// first word points at the player state).
pub mod pm {
    /// This move's command (`usercmd_t`).
    pub const CMD: usize = 0x04;
    /// The previous move's command.
    pub const OLD_CMD: usize = 0x20;
    /// What the player collides with (contents bits).
    pub const TRACE_MASK: usize = 0x3C;
    /// How many entities the move touched (`numtouch`), and their numbers.
    pub const NUM_TOUCH: usize = 0x40;
    pub const TOUCH_ENTS: usize = 0x44;
    /// The player's box.
    pub const MINS: usize = 0xC4;
    pub const MAXS: usize = 0xD0;
    /// The horizontal speed.
    pub const XY_SPEED: usize = 0xDC;
    /// Which of the game's trace functions the move uses (a byte).
    pub const HANDLER: usize = 0xE4;
}

/// `pm_type` values the movement compares (Call of Duty 2's order:
/// normal 0, normal linked 1, noclip 2, ufo 3, spectator 4, intermission 5,
/// dead 6, dead linked 7).
pub mod pm_type {
    /// Dead (6 and above).
    pub const DEAD: i32 = 6;
    /// Flying through walls: 3 times the speed, no inertia clamp.
    pub const NOCLIP: i32 = 2;
    /// Flying: 6 times the speed.
    pub const UFO: i32 = 3;
    /// A spectator: friction 5 times the frame time more, the speed times
    /// `player_spectateSpeedScale`.
    pub const SPECTATOR: i32 = 4;
}

/// `pm_flags` bits the movement tests.
pub mod pm_flags {
    /// Lying down (`PMF_PRONE`).
    pub const PRONE: i32 = 0x1;
    /// On a ladder (`PMF_LADDER`): acceleration goes straight to the wished velocity.
    pub const LADDER: i32 = 0x20;
    /// Moving at 0.4 of the speed, as when leaning (its name not known yet).
    pub const SLOWED: i32 = 0x100;
    /// Friction 0.3 of normal (`PMF_SLIDING`?: a slide after landing).
    pub const SLIDE: i32 = 0x200;
    /// No ground friction (`PMF_TIME_KNOCKBACK`).
    pub const TIME_KNOCKBACK: i32 = 0x400;
    /// A jump is going (`PMF_JUMPING`): friction grows over its first 1.8 s.
    pub const JUMPING: i32 = 0x80000;
}

/// A player state: the game's structure as its bytes.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct PlayerState {
    pub bytes: Vec<u8>,
}

impl PlayerState {
    /// A zeroed player state.
    pub fn new() -> PlayerState {
        PlayerState {
            bytes: vec![0; PLAYER_STATE_SIZE],
        }
    }

    pub fn from_bytes(bytes: &[u8]) -> PlayerState {
        let mut ps = PlayerState::new();
        let n = bytes.len().min(PLAYER_STATE_SIZE);
        ps.bytes[..n].copy_from_slice(&bytes[..n]);
        ps
    }

    pub fn int(&self, at: usize) -> i32 {
        let b = &self.bytes[at..at + 4];
        i32::from_le_bytes([b[0], b[1], b[2], b[3]])
    }

    pub fn set_int(&mut self, at: usize, value: i32) {
        self.bytes[at..at + 4].copy_from_slice(&value.to_le_bytes());
    }

    pub fn float(&self, at: usize) -> f32 {
        f32::from_bits(self.int(at) as u32)
    }

    pub fn set_float(&mut self, at: usize, value: f32) {
        self.set_int(at, value.to_bits() as i32);
    }

    pub fn vec3(&self, at: usize) -> [f32; 3] {
        [self.float(at), self.float(at + 4), self.float(at + 8)]
    }

    pub fn set_vec3(&mut self, at: usize, v: [f32; 3]) {
        for (i, c) in v.into_iter().enumerate() {
            self.set_float(at + 4 * i, c);
        }
    }

    pub fn pm_type(&self) -> i32 {
        self.int(ps::PM_TYPE)
    }

    pub fn pm_flags(&self) -> i32 {
        self.int(ps::PM_FLAGS)
    }

    pub fn velocity(&self) -> [f32; 3] {
        self.vec3(ps::VELOCITY)
    }

    pub fn set_velocity(&mut self, v: [f32; 3]) {
        self.set_vec3(ps::VELOCITY, v);
    }
}

impl Default for PlayerState {
    fn default() -> Self {
        PlayerState::new()
    }
}

/// Bytes of the move's scratch state ([`MoveLocal`]) the ports know so far.
pub const MOVE_LOCAL_SIZE: usize = 0x6C;

/// Where Call of Duty 2 keeps the move's scratch state (`pml_t`).
pub mod pml {
    /// The view's forward and right directions (`forward`, `right`).
    pub const FORWARD: usize = 0x00;
    pub const RIGHT: usize = 0x0C;
    /// The command's duration in seconds.
    pub const FRAME_TIME: usize = 0x24;
    /// The same in milliseconds.
    pub const MSEC: usize = 0x28;
    /// On the ground (not in the air).
    pub const WALKING: usize = 0x2C;
    /// Standing on a surface (`groundPlane`; an integer).
    pub const GROUND_PLANE: usize = 0x30;
    /// The trace down to the ground (`groundTrace`, a [`super::super::trace::Trace`]).
    pub const GROUND_TRACE: usize = 0x38;
    /// The ground's normal (`groundTrace.normal`).
    pub const GROUND_NORMAL: usize = 0x3C;
    /// The ground trace's surface flags (`groundTrace.surfaceFlags`).
    pub const GROUND_SURFACE_FLAGS: usize = 0x48;
    /// How hard the move hit something (`impactSpeed`).
    pub const IMPACT_SPEED: usize = 0x5C;
    /// The origin before this move (`previous_origin`).
    pub const PREVIOUS_ORIGIN: usize = 0x60;
}

/// A surface without friction (`SURF_SLICK`).
pub const SURF_SLICK: i32 = 0x2;

/// The move's scratch state: the game's structure as its bytes.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct MoveLocal {
    pub bytes: Vec<u8>,
}

impl MoveLocal {
    pub fn new() -> MoveLocal {
        MoveLocal {
            bytes: vec![0; MOVE_LOCAL_SIZE],
        }
    }

    pub fn from_bytes(bytes: &[u8]) -> MoveLocal {
        let mut pml = MoveLocal::new();
        let n = bytes.len().min(MOVE_LOCAL_SIZE);
        pml.bytes[..n].copy_from_slice(&bytes[..n]);
        pml
    }

    pub fn int(&self, at: usize) -> i32 {
        let b = &self.bytes[at..at + 4];
        i32::from_le_bytes([b[0], b[1], b[2], b[3]])
    }

    pub fn float(&self, at: usize) -> f32 {
        f32::from_bits(self.int(at) as u32)
    }

    pub fn set_int(&mut self, at: usize, value: i32) {
        self.bytes[at..at + 4].copy_from_slice(&value.to_le_bytes());
    }

    pub fn set_float(&mut self, at: usize, value: f32) {
        self.set_int(at, value.to_bits() as i32);
    }

    pub fn vec3(&self, at: usize) -> [f32; 3] {
        [self.float(at), self.float(at + 4), self.float(at + 8)]
    }

    pub fn set_vec3(&mut self, at: usize, v: [f32; 3]) {
        for (i, c) in v.into_iter().enumerate() {
            self.set_float(at + 4 * i, c);
        }
    }

    pub fn frame_time(&self) -> f32 {
        self.float(pml::FRAME_TIME)
    }

    pub fn walking(&self) -> bool {
        self.int(pml::WALKING) != 0
    }
}

impl Default for MoveLocal {
    fn default() -> Self {
        MoveLocal::new()
    }
}

/// A move (`pmove_t`): the player's state and the move's own fields as
/// their bytes (the first word, the player state's pointer, unused here).
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Pmove {
    pub ps: PlayerState,
    pub bytes: Vec<u8>,
}

impl Pmove {
    pub fn new(ps: PlayerState) -> Pmove {
        Pmove {
            ps,
            bytes: vec![0; PMOVE_SIZE],
        }
    }

    /// `bytes` as the move's fields (past [`PMOVE_SIZE`] dropped, short
    /// ones zeroed).
    pub fn from_bytes(ps: PlayerState, bytes: &[u8]) -> Pmove {
        let mut pm = Pmove::new(ps);
        let n = bytes.len().min(PMOVE_SIZE);
        pm.bytes[..n].copy_from_slice(&bytes[..n]);
        pm
    }

    pub fn int(&self, at: usize) -> i32 {
        let b = &self.bytes[at..at + 4];
        i32::from_le_bytes([b[0], b[1], b[2], b[3]])
    }

    pub fn set_int(&mut self, at: usize, value: i32) {
        self.bytes[at..at + 4].copy_from_slice(&value.to_le_bytes());
    }

    pub fn float(&self, at: usize) -> f32 {
        f32::from_bits(self.int(at) as u32)
    }

    pub fn vec3(&self, at: usize) -> [f32; 3] {
        [self.float(at), self.float(at + 4), self.float(at + 8)]
    }

    /// This move's command (`usercmd_t`, [`USERCMD_SIZE`] bytes).
    pub fn cmd(&self) -> &[u8] {
        &self.bytes[pm::CMD..pm::CMD + USERCMD_SIZE]
    }
}

/// The settings the movement reads (the server's dvars of these names).
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct MoveSettings {
    /// `stopspeed` (100): below it, friction and acceleration count as this speed.
    pub stop_speed: f32,
    /// `friction` (5.5).
    pub friction: f32,
    /// `jump_slowdownEnable` (1): more friction early in a jump.
    pub jump_slowdown_enable: bool,
    /// `inertiaMax` (50): the most a turn sharper than `inertiaAngle` may accelerate.
    pub inertia_max: f32,
    /// `inertiaAngle` (0): the cosine below which a turn is sharp.
    pub inertia_angle: f32,
    /// `inertiaDebug` (0): prints the clamped turns (left out here).
    pub inertia_debug: bool,
    /// `player_spectateSpeedScale` (1): a spectator's speed.
    pub spectate_speed_scale: f32,
    /// `jump_height` (39): how high a jump goes.
    pub jump_height: f32,
    /// `jump_stepSize` (18): the step a jumping player climbs.
    pub jump_step_size: f32,
    /// `player_footstepsThreshhold` (0): the speed footsteps start at.
    pub footsteps_threshold: f32,
}

impl Default for MoveSettings {
    fn default() -> Self {
        MoveSettings {
            stop_speed: 100.0,
            friction: 5.5,
            jump_slowdown_enable: true,
            inertia_max: 50.0,
            inertia_angle: 0.0,
            inertia_debug: false,
            spectate_speed_scale: 1.0,
            jump_height: 39.0,
            jump_step_size: 18.0,
            footsteps_threshold: 0.0,
        }
    }
}

/// How much more friction a jump has: none (1) after 1.8 s of `pm_time`,
/// when the jump also ends (`PMF_JUMPING` and `jumpOriginZ` cleared); with
/// `jump_slowdownEnable` off, none; from 1.7 s on 2.5; before,
/// `pm_time * 0.00088235294 + 1`. The result is the x87 register's.
///
/// Original: `Jump_ReduceFriction` at 0x0052FD30 (CoD2MP_s.exe 1.3; ps in eax, the result on the x87 stack)
pub fn jump_reduce_friction(ps: &mut PlayerState, settings: &MoveSettings) -> F24 {
    let time = ps.int(ps::PM_TIME);
    if time > 1800 {
        let flags = ps.pm_flags() & !pm_flags::JUMPING;
        ps.set_int(ps::PM_FLAGS, flags);
        ps.set_int(ps::JUMP_ORIGIN_Z, 0);
        return F24::load(1.0);
    }
    if !settings.jump_slowdown_enable {
        return F24::load(1.0);
    }
    if time >= 1700 {
        return F24::load(2.5);
    }
    F24::int(time) * F24::load(0.000_882_352_94) + F24::load(1.0)
}

/// Ground friction: the speed (horizontal only while walking) slowed by
/// `max(speed, stopspeed) * friction * frametime` on the ground, unless the
/// surface is slick or knockback is on (0.3 of it sliding; times
/// [`jump_reduce_friction`] in a jump), and by `speed * frametime * 5` more
/// for a spectator; never below 0. Below a speed of 1 the velocity stops.
/// The speed is `sqrt((vy*vy + vx*vx) + vz*vz)`; the comparison with 1 uses
/// the register, the rest the speed stored as a `float`.
///
/// Original: `PM_Friction` at 0x00515490 (CoD2MP_s.exe 1.3; ps in esi, pml in edi)
pub fn friction(ps: &mut PlayerState, pml: &MoveLocal, settings: &MoveSettings) {
    let [vx, vy, vz] = ps.velocity().map(F24::load);
    let walking = pml.walking();
    let vz = if walking { F24::load(0.0) } else { vz };
    let speed = (vy * vy + vx * vx + vz * vz).sqrt();
    let stored = speed.store();
    // (`fcomp` 1, jp: below stops; NaN goes on.)
    if speed < F24::load(1.0) {
        ps.set_velocity([0.0; 3]);
        return;
    }
    let flags = ps.pm_flags();
    let frame_time = F24::load(pml.frame_time());
    let mut drop = F24::load(0.0);
    if walking
        && pml.int(pml::GROUND_SURFACE_FLAGS) & SURF_SLICK == 0
        && flags & pm_flags::TIME_KNOCKBACK == 0
    {
        // (`fcomp`, jp: a NaN speed counts as fast enough.)
        let control = if stored < settings.stop_speed {
            settings.stop_speed
        } else {
            stored
        };
        let scaled = if flags & pm_flags::SLIDE != 0 {
            F24::load(control) * F24::load(0.3)
        } else if flags & pm_flags::JUMPING != 0 {
            jump_reduce_friction(ps, settings) * F24::load(control)
        } else {
            F24::load(control)
        };
        drop = F24::load(settings.friction) * frame_time * scaled;
    }
    if ps.pm_type() == pm_type::SPECTATOR {
        drop = drop + F24::load(stored) * frame_time * F24::load(5.0);
    }
    let mut new_speed = F24::load(stored) - drop;
    // (`fcom` 0, jp: NaN stays.)
    if new_speed < F24::load(0.0) {
        new_speed = F24::load(0.0);
    }
    let scale = new_speed / F24::load(stored);
    let v = ps.velocity();
    ps.set_velocity(v.map(|c| (scale * F24::load(c)).store()));
}

/// Whether accelerating by `amount` along `dir` turns the horizontal
/// velocity sharper than `inertiaAngle` allows: the cosine between the old
/// velocity (normalized) and the new one (stored, normalized) below the
/// setting, or NaN. (With `inertiaDebug` on, the original prints the angle
/// and the clamp; left out.)
///
/// Original: `PM_InertiaTurnTooSharp` at 0x005155A0 (CoD2MP_s.exe 1.3; our name; ps in eax, dir in ecx, amount on the stack)
pub fn inertia_turn_too_sharp(
    ps: &PlayerState,
    dir: &[f32; 3],
    amount: f32,
    settings: &MoveSettings,
) -> bool {
    let v = ps.velocity();
    let a = F24::load(amount);
    let mut new = [
        (a * F24::load(dir[0]) + F24::load(v[0])).store(),
        (a * F24::load(dir[1]) + F24::load(v[1])).store(),
    ];
    let mut old = [ps.float(ps::OLD_VELOCITY), ps.float(ps::OLD_VELOCITY + 4)];
    vec2_normalize(&mut old);
    vec2_normalize(&mut new);
    let cosine = F24::load(old[0]) * F24::load(new[0]) + F24::load(old[1]) * F24::load(new[1]);
    // (`fcomp`, test ah,1: below, or NaN.)
    matches!(
        cosine.partial_cmp(&F24::load(settings.inertia_angle)),
        Some(std::cmp::Ordering::Less) | None
    )
}

/// The acceleration `amount` as inertia allows it: `inertiaMax` instead for
/// a turn sharper than `inertiaAngle` ([`inertia_turn_too_sharp`]) when the
/// amount is above `inertiaMax` and the player had a horizontal velocity
/// (squared length at least 0.0001, or NaN); unless the movement is frozen
/// in a pose ([`pm_type::NOCLIP`]).
///
/// Original: `PM_ClampInertia` at 0x00515680 (CoD2MP_s.exe 1.3; our name; ps in edx, then (amount, dir) on the stack)
pub fn clamp_inertia(
    ps: &PlayerState,
    amount: f32,
    dir: &[f32; 3],
    settings: &MoveSettings,
) -> f32 {
    // (`fcomp`, test ah,1: only a setting at or above the amount keeps it; NaN goes on.)
    if ps.pm_type() == pm_type::NOCLIP || settings.inertia_max >= amount {
        return amount;
    }
    let (ox, oy) = (
        F24::load(ps.float(ps::OLD_VELOCITY)),
        F24::load(ps.float(ps::OLD_VELOCITY + 4)),
    );
    // (`fcomp` with the double 0.0001, jnp: only below skips.)
    if ox * ox + oy * oy < F24::double(0.0001) {
        return amount;
    }
    if inertia_turn_too_sharp(ps, dir, amount, settings) {
        settings.inertia_max
    } else {
        amount
    }
}

/// Accelerates toward `wish_dir` (a unit vector) at `wish_speed`: Quake
/// III's ground acceleration, `max(wish_speed, stopspeed) * frametime *
/// accel`, capped at what is left to `wish_speed` along the direction (and
/// stored as a `float`), then clamped by inertia ([`clamp_inertia`]). On a
/// ladder the velocity goes straight toward `wish_dir * wish_speed`, by at
/// most `wish_speed * frametime * accel` (the distance normalized).
///
/// Original: `PM_Accelerate` at 0x005156E0 (CoD2MP_s.exe 1.3; wishdir in eax, ps in edi, pml in ebx, then (wishspeed, accel) on the stack)
pub fn accelerate(
    ps: &mut PlayerState,
    pml: &MoveLocal,
    wish_dir: &[f32; 3],
    wish_speed: f32,
    accel: f32,
    settings: &MoveSettings,
) {
    let v = ps.velocity();
    let [vx, vy, vz] = v.map(F24::load);
    let [dx, dy, dz] = wish_dir.map(F24::load);
    let (ws, frame_time, accel) = (
        F24::load(wish_speed),
        F24::load(pml.frame_time()),
        F24::load(accel),
    );
    if ps.pm_flags() & pm_flags::LADDER != 0 {
        let wz = (ws * dz).store();
        let mut push = [
            (ws * dx - vx).store(),
            (ws * dy - vy).store(),
            (F24::load(wz) - vz).store(),
        ];
        let push_len = vec3_normalize(&mut push);
        let mut can_push = ws * frame_time * accel;
        // (`fcom`, test ah,0x41, jne: only above takes the distance; NaN stays.)
        if can_push > F24::load(push_len) {
            can_push = F24::load(push_len);
        }
        ps.set_velocity([
            (F24::load(push[0]) * can_push + vx).store(),
            (F24::load(push[1]) * can_push + vy).store(),
            (F24::load(push[2]) * can_push + vz).store(),
        ]);
        return;
    }
    let current = vz * dz + vy * dy + dx * vx;
    let add = ws - current;
    // (`fcom` 0, test ah,0x41, jnp: 0 or below ends it; NaN goes on.)
    if add <= F24::load(0.0) {
        return;
    }
    let control = if wish_speed < settings.stop_speed {
        settings.stop_speed
    } else {
        wish_speed
    };
    let amount = F24::load(control) * frame_time * accel;
    // (`fcomp st(1)`, test ah,0x41, jne: only above takes what is left;
    // NaN stays. Either is stored as a `float`.)
    let amount = if amount > add { add } else { amount }.store();
    let amount = F24::load(clamp_inertia(ps, amount, wish_dir, settings));
    ps.set_velocity([
        (amount * dx + vx).store(),
        (amount * dy + vy).store(),
        (amount * dz + vz).store(),
    ]);
}

/// The speed scale's common end: 0.4 of it with [`pm_flags::SLOWED`] or a
/// lean (any `leanf` but 0), then 3 times it for noclip, 6 for ufo, and
/// `player_spectateSpeedScale` times for a spectator.
fn scale_for_state(ps: &PlayerState, mut scale: F24, settings: &MoveSettings) -> F24 {
    // (`fucompp` with 0: only equal skips; NaN slows.)
    if ps.pm_flags() & pm_flags::SLOWED != 0
        || ps.float(ps::LEANF) != 0.0
        || ps.float(ps::LEANF).is_nan()
    {
        scale = scale * F24::load(0.4);
    }
    match ps.pm_type() {
        pm_type::NOCLIP => scale = scale * F24::load(3.0),
        pm_type::UFO => scale = scale * F24::load(6.0),
        pm_type::SPECTATOR => scale = scale * F24::load(settings.spectate_speed_scale),
        _ => {}
    }
    scale
}

/// How much of the player's speed a move of (`forward`, `right`, `up`)
/// gives: `speed * max(|f|, |r|, |u|) / (sqrt((f*f + r*r) + u*u) * 127)`
/// (0 when all are 0), with [`scale_for_state`]'s factors. The result is the
/// x87 register's.
///
/// Original: `PM_MoveScale` at 0x00515810 (CoD2MP_s.exe 1.3; our name, Call of Duty 4's; ps in ecx, then (forward, right, up) on the stack)
pub fn move_scale(
    ps: &PlayerState,
    forward: f32,
    right: f32,
    up: f32,
    settings: &MoveSettings,
) -> F24 {
    // (Each `fcomp`, test ah,0x41, jne: only a larger one replaces the
    // largest so far, as its stored `float`; NaN doesn't.)
    let mut max = F24::load(forward.abs());
    for c in [right, up] {
        if F24::load(c.abs()) > max {
            max = F24::load(c.abs());
        }
    }
    // (`fucompp` with 0, jp: only equal returns 0; NaN goes on.)
    if max == F24::load(0.0) {
        return F24::load(0.0);
    }
    let speed = F24::int(ps.int(ps::SPEED)) * max;
    let (f, r, u) = (F24::load(forward), F24::load(right), F24::load(up));
    let total = (f * f + r * r + u * u).sqrt();
    scale_for_state(ps, speed / (total * F24::load(127.0)), settings)
}

/// [`move_scale`] for a player command's forward and right moves (signed
/// bytes at 0x18 and 0x19 of the command): integers squared and summed, so
/// the largest is `max(|f|, |r|)` and the length `sqrt(f*f + r*r)`.
///
/// Original: `PM_CmdScale` at 0x005158E0 (CoD2MP_s.exe 1.3; cmd in esi, ps in edi)
pub fn cmd_scale(ps: &PlayerState, cmd: &[u8], settings: &MoveSettings) -> F24 {
    let forward = i32::from(cmd[0x18] as i8);
    let right = i32::from(cmd[0x19] as i8);
    let total = F24::int(forward * forward + right * right).sqrt();
    let max = forward.abs().max(right.abs());
    if max == 0 {
        return F24::load(0.0);
    }
    let speed = F24::int(ps.int(ps::SPEED)) * F24::int(max);
    scale_for_state(ps, speed / (total * F24::load(127.0)), settings)
}

/// The move's timers run down by the command's milliseconds: `pm_time`
/// (when it runs out, the jump, the knockback and the slide end), and the
/// legs' and torso's animation timers (never below 0; one at 0 or below
/// stays).
///
/// Original: `PM_DropTimers` at 0x00518F90 (CoD2MP_s.exe 1.3; ps in eax, pml in esi)
pub fn drop_timers(ps: &mut PlayerState, pml: &MoveLocal) {
    let msec = pml.int(pml::MSEC);
    let time = ps.int(ps::PM_TIME);
    if time != 0 {
        if msec < time {
            ps.set_int(ps::PM_TIME, time - msec);
        } else {
            let flags =
                ps.pm_flags() & !(pm_flags::JUMPING | pm_flags::TIME_KNOCKBACK | pm_flags::SLIDE);
            ps.set_int(ps::PM_FLAGS, flags);
            ps.set_int(ps::PM_TIME, 0);
        }
    }
    for at in [ps::LEGS_TIMER, ps::TORSO_TIMER] {
        let timer = ps.int(at);
        if timer > 0 {
            let left = timer.wrapping_sub(msec);
            ps.set_int(at, if left < 0 { 0 } else { left });
        }
    }
}

/// `eFlags` bits with which a prone player's movement direction doesn't
/// follow his body (their names not known yet).
pub const E_FLAGS_PRONE_FREE: i32 = 0x300;

/// Where the player moves relative to where he looks ([`ps::MOVEMENT_DIR`],
/// for the legs' animation), in whole degrees from -90 to 90:
///
/// - prone (and none of [`E_FLAGS_PRONE_FREE`]): the body's direction
///   ([`ps::PRONE_DIRECTION`]) less the yaw;
/// - on a ladder: the ladder's yaw ([`vectoyaw`]) plus 180, less the yaw;
/// - else, when the command moves (forward or right not 0), the player
///   stands on something and moved more than 5 times the frame time since
///   the last move (`origin - previous_origin`): that move's yaw less the
///   view's; moving backwards (forward below 0), plus 180 (of the whole
///   degrees); otherwise 0.
///
/// Each angle is brought to (-180, 180] ([`angle_normalize180`]), truncated
/// to whole degrees and clamped to 90 either way. The command's forward and
/// right moves are its signed bytes at 0x18 and 0x19.
///
/// Original: `PM_SetMovementDir` at 0x00515C30 (CoD2MP_s.exe 1.3; Quake III's name; pm in eax, pml in ecx)
pub fn set_movement_dir(pm: &mut Pmove, pml: &MoveLocal) {
    let cmd: [u8; USERCMD_SIZE] = pm.cmd().try_into().expect("a command's bytes");
    let ps = &mut pm.ps;
    let cmd = &cmd[..];
    let yaw = ps.float(ps::VIEW_ANGLES + 4);
    let flags = ps.pm_flags();
    let dir = if flags & pm_flags::PRONE != 0 && ps.int(ps::E_FLAGS) & E_FLAGS_PRONE_FREE == 0 {
        relative_dir(ps.float(ps::PRONE_DIRECTION), yaw)
    } else if flags & pm_flags::LADDER != 0 {
        let ladder = (F24::load(vectoyaw(&ps.vec3(ps::LADDER_VEC))) + F24::load(180.0)).store();
        relative_dir(ladder, yaw)
    } else {
        moved_dir(ps, cmd, pml, yaw).unwrap_or(0)
    };
    ps.set_int(ps::MOVEMENT_DIR, dir);
}

/// `ftol2(AngleNormalize180(angle))`: the angle's whole degrees in (-180, 180].
fn whole_degrees(angle: f32) -> i32 {
    crate::net::msg::ftol2(f64::from(angle_normalize180(angle))) as i32
}

/// Whole degrees clamped to 90 either way (as the signed byte the original
/// keeps of it).
fn clamp_dir(degrees: i32) -> i32 {
    let d = if degrees.wrapping_abs() > 90 {
        if degrees <= 0 {
            -90
        } else {
            90
        }
    } else {
        degrees
    };
    i32::from(d as i8)
}

/// `angle - yaw` (stored as a `float`) in clamped whole degrees.
fn relative_dir(angle: f32, yaw: f32) -> i32 {
    clamp_dir(whole_degrees((F24::load(angle) - F24::load(yaw)).store()))
}

/// [`set_movement_dir`]'s direction from the last move; `None`: 0.
fn moved_dir(ps: &PlayerState, cmd: &[u8], pml: &MoveLocal, yaw: f32) -> Option<i32> {
    let origin = ps.vec3(ps::ORIGIN);
    let before = pml.vec3(pml::PREVIOUS_ORIGIN);
    let mut moved = [0f32; 3];
    for ((m, o), b) in moved.iter_mut().zip(origin).zip(before) {
        *m = (F24::load(o) - F24::load(b)).store();
    }
    let forward = cmd[0x18] as i8;
    if forward == 0 && cmd[0x19] == 0 {
        return None;
    }
    if ps.int(ps::GROUND_ENTITY_NUM) == ENTITY_NONE {
        return None;
    }
    let [x, y, z] = moved.map(F24::load);
    let squared = (z * z + x * x + y * y).store();
    let length = F24::load(squared).sqrt().store();
    // (`fucompp` with 0, test ah,0x44, jnp: only equal stops; NaN goes on.)
    if length == 0.0 {
        return None;
    }
    // (`fcomp`, test ah,5, jp: on only when 5 frame times are below the
    // length; NaN stops.)
    let least = F24::load(pml.frame_time()) * F24::load(5.0);
    if !matches!(
        least.partial_cmp(&F24::load(length)),
        Some(std::cmp::Ordering::Less)
    ) {
        return None;
    }
    let mut dir = [0f32; 3];
    vec3_normalize_to(&moved, &mut dir);
    // (In place in the original: it reads the direction before writing.)
    let unit = dir;
    vec_to_angles(&unit, &mut dir);
    let mut degrees = whole_degrees((F24::load(dir[1]) - F24::load(yaw)).store());
    if forward < 0 {
        let back = (F24::load(F24::int(degrees).store()) + F24::load(180.0)).store();
        degrees = whole_degrees(back);
    }
    Some(clamp_dir(degrees))
}

/// `v` clipped against a plane of `normal`: its part into the plane taken
/// away, a little more (0.001 of it) so the result points out of the
/// plane: `v + n * -(b - |b| * 0.001)`, `b = v . n` (Call of Duty 2's
/// `OVERCLIP`), each component stored.
///
/// Original: `PM_ClipVelocity` at 0x00515430 (CoD2MP_s.exe 1.3; Quake III's name; in in ecx, normal in eax, out in edx)
pub fn clip_velocity(v: &[f32; 3], normal: &[f32; 3]) -> [f32; 3] {
    let [x, y, z] = v.map(F24::load);
    let [a, b, c] = normal.map(F24::load);
    let backoff = x * a + y * b + c * z;
    let change = -(backoff - backoff.abs() * F24::load(0.001));
    [
        (change * a + x).store(),
        (change * b + y).store(),
        (change * c + z).store(),
    ]
}

/// Records that the move touched entity `number` (not the world, not twice,
/// at most [`MAX_TOUCH`]). A count outside 0 to 32 (which the game never
/// has) is left alone (the original would write outside the list).
///
/// Original: `PM_AddTouchEnt` at 0x005153F0 (CoD2MP_s.exe 1.3; Quake III's name; pm in eax, the number in edi)
pub fn add_touch_ent(pm: &mut Pmove, number: i32) {
    if number == ENTITY_WORLD {
        return;
    }
    let count = pm.int(pm::NUM_TOUCH);
    if !(0..MAX_TOUCH).contains(&count) {
        return;
    }
    let count = count as usize;
    if (0..count).any(|i| pm.int(pm::TOUCH_ENTS + 4 * i) == number) {
        return;
    }
    pm.set_int(pm::TOUCH_ENTS + 4 * count, number);
    pm.set_int(pm::NUM_TOUCH, count as i32 + 1);
}

/// The step a jumping player at `origin` may still climb: none (false)
/// once at the jump's height (`jumpOriginZ + jump_height`) or above, else
/// `jump_stepSize`, or less when that would go past the height.
///
/// Original: `Jump_GetStepHeight` at 0x0052FBD0 (CoD2MP_s.exe 1.3; Call of Duty 4's name; ps in edx, origin in ecx, the step out in edi; al)
pub fn jump_get_step_height(
    ps: &PlayerState,
    origin: &[f32; 3],
    step: &mut f32,
    settings: &MoveSettings,
) -> bool {
    let top = F24::load(ps.float(ps::JUMP_ORIGIN_Z)) + F24::load(settings.jump_height);
    // (`fcomp`, test ah,0x41, jne: on only when the top is above; NaN stops.)
    if !matches!(
        top.partial_cmp(&F24::load(origin[2])),
        Some(std::cmp::Ordering::Greater)
    ) {
        return false;
    }
    *step = settings.jump_step_size;
    let top = top.store();
    if F24::load(settings.jump_step_size) + F24::load(origin[2]) > F24::load(top) {
        *step = (F24::load(top) - F24::load(origin[2])).store();
    }
    true
}

/// A jump's upward speed kept to what reaches the jump's height, once the
/// player is above `origin` (where the move started): with less than 0.1
/// left to the height, no upward speed; else at most `sqrt(2 * left *
/// gravity)`.
///
/// Original: `Jump_ClampVelocity` at 0x0052FD90 (CoD2MP_s.exe 1.3; Call of Duty 4's name; ps in ecx, origin in eax)
pub fn jump_clamp_velocity(ps: &mut PlayerState, origin: &[f32; 3], settings: &MoveSettings) {
    let z = F24::load(ps.float(ps::ORIGIN + 8));
    // (`fcomp` 0, test ah,0x41, jne: only above goes on; NaN doesn't.)
    if !matches!(
        (z - F24::load(origin[2])).partial_cmp(&F24::load(0.0)),
        Some(std::cmp::Ordering::Greater)
    ) {
        return;
    }
    let left = F24::load(settings.jump_height) + F24::load(ps.float(ps::JUMP_ORIGIN_Z)) - z;
    let velocity_z = ps::VELOCITY + 8;
    // (`fcom` 0.1, test ah,5, jp: below stops the jump; NaN goes on.)
    if left < F24::load(0.1) {
        ps.set_float(velocity_z, 0.0);
        return;
    }
    let most = (F24::int(ps.int(ps::GRAVITY)) * (left + left)).sqrt();
    // (Again: only a speed above the most is clamped; NaN isn't.)
    if most < F24::load(ps.float(velocity_z)) {
        ps.set_float(velocity_z, most.store());
    }
}

/// Whether the move makes footstep sounds: not crouching or prone (by the
/// view height the stance moves to), not [`pm_flags::SLOWED`], and at
/// least `player_footstepsThreshhold` fast (`xyspeed`; NaN isn't).
///
/// Original: `PM_ShouldMakeFootsteps` at 0x005184E0 (CoD2MP_s.exe 1.3; Call of Duty 4's name; pm in esi; eax)
pub fn should_make_footsteps(pm: &Pmove, settings: &MoveSettings) -> bool {
    let target = pm.ps.int(ps::VIEW_HEIGHT_TARGET);
    if target == CROUCH_VIEW_HEIGHT || target == PRONE_VIEW_HEIGHT {
        return false;
    }
    if pm.ps.pm_flags() & pm_flags::SLOWED != 0 {
        return false;
    }
    // (`fcomp`, test ah,1, jne: below or NaN makes none.)
    F24::load(pm.float(pm::XY_SPEED)) >= F24::load(settings.footsteps_threshold)
}

/// A trace for the moving player (the move's `handler` picks the game's
/// trace function): when it starts inside another body
/// ([`CONTENTS_BODY`]), that body counts as touched ([`add_touch_ent`]),
/// the move stops colliding with bodies (its trace mask loses the bit) and
/// the trace is made again without them.
///
/// Original: `PM_playerTrace` at 0x00515310 (CoD2MP_s.exe 1.3; Call of Duty 4's name; pm in esi, results in ebx, then (start, mins, maxs, end, passEntityNum, contentMask) on the stack)
#[allow(clippy::too_many_arguments)]
pub fn player_trace(
    pm: &mut Pmove,
    start: &[f32; 3],
    mins: &[f32; 3],
    maxs: &[f32; 3],
    end: &[f32; 3],
    pass_entity: i32,
    content_mask: i32,
    host: &mut dyn MoveHost,
) -> Trace {
    let handler = pm.bytes[pm::HANDLER];
    let mut results = host.trace(handler, start, mins, maxs, end, pass_entity, content_mask);
    if results.start_solid() && results.contents() & CONTENTS_BODY != 0 {
        add_touch_ent(pm, i32::from(results.entity_num()));
        let mask = pm.int(pm::TRACE_MASK) & !CONTENTS_BODY;
        pm.set_int(pm::TRACE_MASK, mask);
        let handler = pm.bytes[pm::HANDLER];
        results = host.trace(
            handler,
            start,
            mins,
            maxs,
            end,
            pass_entity,
            content_mask & !CONTENTS_BODY,
        );
    }
    results
}

/// Events kept in a player state (`MAX_PS_EVENTS`).
pub const MAX_PS_EVENTS: i32 = 4;

/// The surface flag of surfaces without footsteps (`SURF_NOSTEPS`).
pub const SURF_NOSTEPS: i32 = 0x2000;
/// The first footstep events: running, walking (slowed or leaning) and
/// prone on surface type 0 (each `+ type`; types are the surface flags'
/// bits 20 to 24).
pub const EV_FOOTSTEP_RUN: i32 = 1;
pub const EV_FOOTSTEP_WALK: i32 = 0x18;
pub const EV_FOOTSTEP_PRONE: i32 = 0x2F;
/// The surface type of a ladder with none of its own.
pub const LADDER_SURFACE_TYPE: i32 = 0x15;

/// An event the player's own game predicts (`event` 0 adds none): kept in
/// the next of the player state's four event slots (as a byte), with
/// parameter 0.
///
/// Original: `PM_AddEvent` at 0x005153B0 (CoD2MP_s.exe 1.3; Quake III's name; ps in eax, the event in ecx)
pub fn add_event(ps: &mut PlayerState, event: i32) {
    if event == 0 {
        return;
    }
    let sequence = ps.int(ps::EVENT_SEQUENCE);
    let slot = (sequence & (MAX_PS_EVENTS - 1)) as usize;
    ps.set_int(ps::EVENTS + 4 * slot, event & 0xFF);
    ps.set_int(ps::EVENT_PARMS + 4 * slot, 0);
    ps.set_int(ps::EVENT_SEQUENCE, sequence.wrapping_add(1));
}

/// The footstep event for the ground the move stands on: none on a surface
/// without footsteps or of type 0, else prone, walking (slowed, or leaning)
/// or running on that type.
///
/// Original: `PM_FootstepType` at 0x00516960 (CoD2MP_s.exe 1.3; Call of Duty 4's name; ps in edx, pml in eax)
pub fn footstep_type(ps: &PlayerState, pml: &MoveLocal) -> i32 {
    let flags = pml.int(pml::GROUND_SURFACE_FLAGS);
    if flags & SURF_NOSTEPS != 0 {
        return 0;
    }
    let surface = (flags >> 20) & 0x1F;
    if surface == 0 {
        return 0;
    }
    let pm_flags = ps.pm_flags();
    if pm_flags & pm_flags::PRONE != 0 {
        return surface + EV_FOOTSTEP_PRONE;
    }
    // (`fucompp` with 0: only equal runs; NaN walks.)
    if pm_flags & pm_flags::SLOWED != 0
        || ps.float(ps::LEANF) != 0.0
        || ps.float(ps::LEANF).is_nan()
    {
        return surface + EV_FOOTSTEP_WALK;
    }
    surface + EV_FOOTSTEP_RUN
}

/// A footstep when the bob cycle passes half way (bit 7 of `old + 64` and
/// `new + 64` differs) and `footstep` is set: on the ground, the ground's
/// ([`footstep_type`]); in the air on a ladder, the ladder's: a trace of a
/// smaller box (6 in from the sides, from 8 up) 31 units into the ladder
/// (against `vLadderVec`), its surface type, or [`LADDER_SURFACE_TYPE`] for
/// none or a miss, as a running step.
///
/// Original: `PM_FootstepEvent` at 0x00518380 (CoD2MP_s.exe 1.3; Call of Duty 4's name; old bob cycle in ecx, new in edx, footstep in eax, then (pm, pml) on the stack)
pub fn footstep_event(
    pm: &mut Pmove,
    pml: &MoveLocal,
    old_bob: i32,
    new_bob: i32,
    footstep: bool,
    host: &mut dyn MoveHost,
) {
    if (old_bob.wrapping_add(64) ^ new_bob.wrapping_add(64)) & 0x80 == 0 {
        return;
    }
    if pm.ps.int(ps::GROUND_ENTITY_NUM) != ENTITY_NONE {
        if footstep {
            let event = footstep_type(&pm.ps, pml);
            add_event(&mut pm.ps, event);
        }
        return;
    }
    if !footstep || pm.ps.pm_flags() & pm_flags::LADDER == 0 {
        return;
    }
    let (mins, maxs) = (pm.vec3(pm::MINS), pm.vec3(pm::MAXS));
    let six = F24::load(6.0);
    let small_mins = [
        (F24::load(mins[0]) + six).store(),
        (F24::load(mins[1]) + six).store(),
        8.0,
    ];
    // (`fcomp` 8, test ah,5, jp: only below 8 is raised; NaN stays.)
    let top = if F24::load(maxs[2]) < F24::load(8.0) {
        8.0
    } else {
        maxs[2]
    };
    let small_maxs = [
        (F24::load(maxs[0]) - six).store(),
        (F24::load(maxs[1]) - six).store(),
        top,
    ];
    let origin = pm.ps.vec3(ps::ORIGIN);
    let ladder = pm.ps.vec3(ps::LADDER_VEC);
    let mut end = [0f32; 3];
    for i in 0..3 {
        end[i] = (F24::load(origin[i]) - F24::load(ladder[i]) * F24::load(31.0)).store();
    }
    let pass = pm.ps.int(ps::CLIENT_NUM);
    let mask = pm.int(pm::TRACE_MASK) & !(CONTENTS_BODY | 0x0001_0000);
    let trace = player_trace(
        pm,
        &origin,
        &small_mins,
        &small_maxs,
        &end,
        pass,
        mask,
        host,
    );
    let mut surface = (trace.int(super::trace::at::SURFACE_FLAGS) >> 20) & 0x1F;
    // (`fucompp` with 1: a miss is the ladder's own type; NaN isn't.)
    if trace.fraction() == 1.0 || surface == 0 {
        surface = LADDER_SURFACE_TYPE;
    }
    add_event(&mut pm.ps, surface + EV_FOOTSTEP_RUN);
}
