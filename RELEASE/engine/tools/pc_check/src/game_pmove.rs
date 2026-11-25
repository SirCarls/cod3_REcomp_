//! Requests for `iw_pc::game::pmove` and `iw_pc::shared::math` (Call of Duty
//! 2; specs/game_pmove.py): the player state and the move's scratch state
//! arrive as buffers of their bytes, the settings as their values.

use iw_pc::game::anim::{self, ClientAnimInfo};
use iw_pc::game::duck;
use iw_pc::game::ground;
use iw_pc::game::jump;
use iw_pc::game::mantle::{self, MantleSettings};
use iw_pc::game::moves;
use iw_pc::game::pmove::{self, pm_type, MoveLocal, MoveSettings, PlayerState, Pmove, PMOVE_SIZE};
use iw_pc::game::prone::{self, ProneAngles, ProneQuery};
use iw_pc::game::slide;
use iw_pc::game::trace::{MoveHost, Trace, ANIM_ET_DEATH, TRACE_SIZE};
use iw_pc::game::view;
use iw_pc::game::weapon::{self, WeaponDef, WEAPON_DEF_SIZE};
use iw_pc::game::weapon_state as ws;
use iw_pc::shared::math;
use iw_pc::Game;

use crate::net_client::{CallLog, Logged};
use crate::{Answer, Args, Ret};

/// The check's world: each trace is logged as the original's stand-ins
/// record it (its handler, the four vectors' bytes, the entity passed, the
/// mask) and answered with the case's next result (specs/game_pmove.py
/// `TraceAnswers`; nothing hit once they run out). The weapons' move-speed
/// scales are the case's (`WeaponScales`), and so are the animation
/// scripts (`AnimAnswers`), the weapons' definitions (`WeaponDefs`) and
/// the player's animation state (`ClientAnim`: the one for every client).
pub(crate) struct CheckWorld {
    answers: Vec<Trace>,
    next: usize,
    weapon_scales: Vec<f32>,
    anim: Option<AnimWorld>,
    defs: Vec<WeaponDef>,
    weapon_count: i32,
    no_def: WeaponDef,
    anim_info: ClientAnimInfo,
    pub(crate) log: CallLog,
}

/// specs/game_pmove.py `AnimAnswers`: the commands of the item the first
/// valid one finds (-1: none found), what `rand` gives, animation 0's
/// duration, and how many items each event's script has (none unless
/// listed).
struct AnimWorld {
    commands: i32,
    rand: i32,
    duration: i32,
    items: Vec<(i32, i32)>,
}

/// The little-endian words of `b`.
fn words(b: &[u8]) -> Vec<i32> {
    b.chunks_exact(4)
        .map(|w| i32::from_le_bytes([w[0], w[1], w[2], w[3]]))
        .collect()
}

impl CheckWorld {
    pub(crate) fn new(answers: &[u8]) -> CheckWorld {
        let answers = answers
            .chunks_exact(TRACE_SIZE)
            .map(|c| {
                let mut t = Trace::default();
                t.bytes.copy_from_slice(c);
                t
            })
            .collect();
        CheckWorld {
            answers,
            next: 0,
            weapon_scales: Vec::new(),
            anim: None,
            defs: Vec::new(),
            weapon_count: 0,
            no_def: WeaponDef::default(),
            anim_info: ClientAnimInfo::default(),
            log: CallLog::default(),
        }
    }

    /// With the weapons' definitions (`WeaponDefs`: the highest weapon
    /// number, then the definitions).
    fn defs(mut self, b: &[u8]) -> CheckWorld {
        if let Some((count, defs)) = b.split_first_chunk::<4>() {
            self.weapon_count = i32::from_le_bytes(*count);
            self.defs = defs
                .chunks(WEAPON_DEF_SIZE)
                .map(WeaponDef::from_bytes)
                .collect();
        }
        self
    }

    /// With the player's animation state (`ClientAnim`).
    fn anim_info(mut self, b: &[u8]) -> CheckWorld {
        self.anim_info = ClientAnimInfo::from_bytes(b);
        self
    }

    /// With the weapons' move-speed scales (`WeaponScales`: their bits).
    fn weapons(mut self, scales: &[u8]) -> CheckWorld {
        self.weapon_scales = words(scales)
            .into_iter()
            .map(|w| f32::from_bits(w as u32))
            .collect();
        self
    }

    /// With the animation scripts (`AnimAnswers`).
    fn scripts(mut self, answers: &[u8]) -> CheckWorld {
        let w = words(answers);
        if let [commands, rand, duration, n, ref pairs @ ..] = w[..] {
            if usize::try_from(n).is_ok_and(|n| pairs.len() == 2 * n) {
                self.anim = Some(AnimWorld {
                    commands,
                    rand,
                    duration,
                    items: pairs.chunks_exact(2).map(|p| (p[0], p[1])).collect(),
                });
            }
        }
        self
    }
}

fn vector_bytes(v: &[f32; 3]) -> Vec<u8> {
    v.iter().flat_map(|c| c.to_bits().to_le_bytes()).collect()
}

impl MoveHost for CheckWorld {
    fn trace(
        &mut self,
        handler: u8,
        start: &[f32; 3],
        mins: &[f32; 3],
        maxs: &[f32; 3],
        end: &[f32; 3],
        pass_entity: i32,
        content_mask: i32,
    ) -> Trace {
        let vectors = [start, mins, maxs, end].map(vector_bytes);
        self.log.call(
            "trace",
            &[
                Logged::I(i32::from(handler)),
                Logged::S(&vectors[0]),
                Logged::S(&vectors[1]),
                Logged::S(&vectors[2]),
                Logged::S(&vectors[3]),
                Logged::I(pass_entity),
                Logged::I(content_mask),
            ],
        );
        let k = self.next;
        self.next += 1;
        self.answers.get(k).copied().unwrap_or_else(|| {
            let mut t = Trace::default();
            t.bytes[..4].copy_from_slice(&1f32.to_bits().to_le_bytes());
            t.bytes[0x1C..0x1E].copy_from_slice(&0x3FFu16.to_le_bytes());
            t
        })
    }

    fn weapon_move_speed_scale(&self, weapon: i32) -> f32 {
        usize::try_from(weapon)
            .ok()
            .and_then(|w| self.weapon_scales.get(w))
            .copied()
            .unwrap_or(0.0)
    }

    /// As `BG_AnimScriptEvent` (0x004F92C0) goes, with the case's scripts:
    /// none for the dead (but death's); none when the event's script has no
    /// items; then the first valid item (the stand-in's call: the client and
    /// the event), `rand`, and the command it picks (its index, setTimer 1,
    /// isContinue, force).
    fn anim_script_event(
        &mut self,
        ps: &mut PlayerState,
        event: i32,
        is_continue: bool,
        force: bool,
    ) -> i32 {
        if event != ANIM_ET_DEATH && ps.pm_type() >= pm_type::DEAD {
            return -1;
        }
        let Some(anim) = &self.anim else {
            return -1;
        };
        let items = anim
            .items
            .iter()
            .find(|(e, _)| *e == event)
            .map_or(0, |(_, n)| *n);
        if items == 0 {
            return -1;
        }
        let client = ps.int(pmove::ps::CLIENT_NUM);
        let (commands, rand) = (anim.commands, anim.rand);
        self.log.call(
            "anim_first_valid_item",
            &[Logged::I(client), Logged::I(event)],
        );
        if commands <= 0 {
            return -1;
        }
        self.log.call("rand", &[]);
        let command = rand.wrapping_rem(commands);
        self.log.call(
            "anim_execute_command",
            &[
                Logged::I(command),
                Logged::I(1),
                Logged::I(i32::from(is_continue)),
                Logged::I(i32::from(force)),
            ],
        );
        0
    }

    /// Logged as the original's `Com_Printf` stand-in records it (its
    /// format).
    fn print(&mut self, message: &[u8]) {
        self.log.call("com_printf", &[Logged::S(message)]);
    }

    /// The case's duration for animation 0 (the only one the spec gives).
    fn anim_duration(&self, anim: i32) -> i32 {
        match &self.anim {
            Some(a) if anim == 0 => a.duration,
            _ => 0,
        }
    }

    /// The case's definition (zeros past them).
    fn weapon_def(&self, weapon: i32) -> &WeaponDef {
        usize::try_from(weapon)
            .ok()
            .and_then(|w| self.defs.get(w))
            .unwrap_or(&self.no_def)
    }

    fn weapon_count(&self) -> i32 {
        self.weapon_count
    }

    /// The case's state, whichever client asks (the spec gives the
    /// player's).
    fn client_anim_info(&mut self, _client: i32) -> &mut ClientAnimInfo {
        &mut self.anim_info
    }
}

/// The animation state back into buffer `i` (`ClientAnim`).
fn put_anim_info(args: &mut Args, i: usize, info: &ClientAnimInfo) -> Result<(), String> {
    let slot = args.buffer(i).ok_or("NULL animation state")?;
    let n = slot.len().min(info.bytes.len());
    slot[..n].copy_from_slice(&info.bytes[..n]);
    Ok(())
}

fn floats<const N: usize>(b: &[u8]) -> Result<[f32; N], String> {
    let mut out = [0f32; N];
    for (i, v) in out.iter_mut().enumerate() {
        let w = b.get(4 * i..4 * i + 4).ok_or("short vector")?;
        *v = f32::from_bits(u32::from_le_bytes([w[0], w[1], w[2], w[3]]));
    }
    Ok(out)
}

fn put_floats(b: &mut [u8], v: &[f32]) {
    for (i, c) in v.iter().enumerate() {
        b[4 * i..4 * i + 4].copy_from_slice(&c.to_bits().to_le_bytes());
    }
}

/// A byte setting (a `bool` dvar's value at +8).
fn flag(args: &Args, i: usize) -> bool {
    args.i32(i) & 0xFF != 0
}

/// Runs `f` on buffer `i` as a player state and puts its bytes back.
fn with_ps<R>(
    args: &mut Args,
    i: usize,
    f: impl FnOnce(&mut PlayerState) -> R,
) -> Result<R, String> {
    let slot = args.buffer(i).ok_or("NULL ps")?;
    let mut ps = PlayerState::from_bytes(slot);
    let r = f(&mut ps);
    let n = slot.len();
    slot.copy_from_slice(&ps.bytes[..n]);
    Ok(r)
}

fn vector<const N: usize>(
    args: &mut Args,
    i: usize,
    f: impl FnOnce(&mut [f32; N]) -> f32,
) -> Answer {
    let slot = args.buffer(i).ok_or("NULL vector")?;
    let mut v = floats::<N>(slot)?;
    let length = f(&mut v);
    put_floats(slot, &v);
    Ok(Ret::F32(length))
}

pub fn dispatch(game: Game, function: &str, args: &mut Args) -> Option<Answer> {
    if game != Game::CallOfDuty2 {
        return None;
    }
    Some(match function {
        "Vec3Normalize" => vector::<3>(args, 0, math::vec3_normalize),
        "Vec2Normalize" => vector::<2>(args, 0, math::vec2_normalize),
        // v, out
        "Vec3NormalizeTo" | "VecToAngles" => (|| {
            let v = floats::<3>(args.buffer(0).ok_or("NULL v")?)?;
            let out = args.buffer(1).ok_or("NULL out")?;
            let mut o = floats::<3>(out)?;
            let r = if function == "VecToAngles" {
                math::vec_to_angles(&v, &mut o);
                Ret::Void
            } else {
                Ret::F32(math::vec3_normalize_to(&v, &mut o))
            };
            put_floats(out, &o);
            Ok(r)
        })(),
        "vectoyaw" => (|| {
            let v = floats::<3>(args.buffer(0).ok_or("NULL v")?)?;
            Ok(Ret::F32(math::vectoyaw(&v)))
        })(),
        "AngleNormalize180" => Ok(Ret::F32(math::angle_normalize180(args.f32(0)))),
        // ps, jump_slowdownEnable
        "Jump_ReduceFriction" => (|| {
            let settings = MoveSettings {
                jump_slowdown_enable: flag(args, 1),
                ..MoveSettings::default()
            };
            let r = with_ps(args, 0, |ps| pmove::jump_reduce_friction(ps, &settings))?;
            Ok(Ret::F32(r.store()))
        })(),
        // ps, pml, stopspeed, friction, jump_slowdownEnable
        "PM_Friction" => (|| {
            let settings = MoveSettings {
                stop_speed: args.f32(2),
                friction: args.f32(3),
                jump_slowdown_enable: flag(args, 4),
                ..MoveSettings::default()
            };
            let pml = MoveLocal::from_bytes(args.buffer(1).ok_or("NULL pml")?);
            with_ps(args, 0, |ps| pmove::friction(ps, &pml, &settings))?;
            Ok(Ret::Void)
        })(),
        // ps, dir, amount, inertiaMax (its debug line's), inertiaAngle, inertiaDebug
        "PM_InertiaTurnTooSharp" => (|| {
            let dir = floats::<3>(args.buffer(1).ok_or("NULL dir")?)?;
            let settings = MoveSettings {
                inertia_max: args.f32(3),
                inertia_angle: args.f32(4),
                inertia_debug: flag(args, 5),
                ..MoveSettings::default()
            };
            let amount = args.f32(2);
            let r = with_ps(args, 0, |ps| {
                pmove::inertia_turn_too_sharp(ps, &dir, amount, &settings)
            })?;
            Ok(Ret::Int(i64::from(r)))
        })(),
        // ps, amount, dir, inertiaMax, inertiaAngle, inertiaDebug
        "PM_ClampInertia" => (|| {
            let dir = floats::<3>(args.buffer(2).ok_or("NULL dir")?)?;
            let settings = MoveSettings {
                inertia_max: args.f32(3),
                inertia_angle: args.f32(4),
                inertia_debug: flag(args, 5),
                ..MoveSettings::default()
            };
            let amount = args.f32(1);
            let r = with_ps(args, 0, |ps| {
                pmove::clamp_inertia(ps, amount, &dir, &settings)
            })?;
            Ok(Ret::F32(r))
        })(),
        // wishdir, ps, pml, wishspeed, accel, stopspeed, inertiaMax, inertiaAngle, inertiaDebug
        "PM_Accelerate" => (|| {
            let dir = floats::<3>(args.buffer(0).ok_or("NULL wishdir")?)?;
            let pml = MoveLocal::from_bytes(args.buffer(2).ok_or("NULL pml")?);
            let settings = MoveSettings {
                stop_speed: args.f32(5),
                inertia_max: args.f32(6),
                inertia_angle: args.f32(7),
                inertia_debug: flag(args, 8),
                ..MoveSettings::default()
            };
            let (wish, accel) = (args.f32(3), args.f32(4));
            with_ps(args, 1, |ps| {
                pmove::accelerate(ps, &pml, &dir, wish, accel, &settings)
            })?;
            Ok(Ret::Void)
        })(),
        // ps, forward, right, up, player_spectateSpeedScale
        "PM_MoveScale" => (|| {
            let settings = MoveSettings {
                spectate_speed_scale: args.f32(4),
                ..MoveSettings::default()
            };
            let ps = PlayerState::from_bytes(args.buffer(0).ok_or("NULL ps")?);
            let r = pmove::move_scale(&ps, args.f32(1), args.f32(2), args.f32(3), &settings);
            Ok(Ret::F32(r.store()))
        })(),
        // cmd, ps, player_spectateSpeedScale
        "PM_CmdScale" => (|| {
            let settings = MoveSettings {
                spectate_speed_scale: args.f32(2),
                ..MoveSettings::default()
            };
            let cmd = args.buffer(0).ok_or("NULL cmd")?.clone();
            let ps = PlayerState::from_bytes(args.buffer(1).ok_or("NULL ps")?);
            Ok(Ret::F32(pmove::cmd_scale(&ps, &cmd, &settings).store()))
        })(),
        // ps, pml
        "PM_DropTimers" => (|| {
            let pml = MoveLocal::from_bytes(args.buffer(1).ok_or("NULL pml")?);
            with_ps(args, 0, |ps| pmove::drop_timers(ps, &pml))?;
            Ok(Ret::Void)
        })(),
        // pm, pml
        "PM_SetMovementDir" => (|| {
            let pml = MoveLocal::from_bytes(args.buffer(1).ok_or("NULL pml")?);
            with_pm(args, 0, |pm| pmove::set_movement_dir(pm, &pml))?;
            Ok(Ret::Void)
        })(),
        // in, normal, out
        "PM_ClipVelocity" => (|| {
            let v = floats::<3>(args.buffer(0).ok_or("NULL in")?)?;
            let normal = floats::<3>(args.buffer(1).ok_or("NULL normal")?)?;
            put_floats(
                args.buffer(2).ok_or("NULL out")?,
                &pmove::clip_velocity(&v, &normal),
            );
            Ok(Ret::Void)
        })(),
        // pm, entityNum
        "PM_AddTouchEnt" => (|| {
            let number = args.i32(1);
            with_pm(args, 0, |pm| pmove::add_touch_ent(pm, number))?;
            Ok(Ret::Void)
        })(),
        // ps, origin, step, jump_height, jump_stepSize
        "Jump_GetStepHeight" => (|| {
            let settings = MoveSettings {
                jump_height: args.f32(3),
                jump_step_size: args.f32(4),
                ..MoveSettings::default()
            };
            let ps = PlayerState::from_bytes(args.buffer(0).ok_or("NULL ps")?);
            let origin = floats::<3>(args.buffer(1).ok_or("NULL origin")?)?;
            let slot = args.buffer(2).ok_or("NULL step")?;
            let mut step = floats::<1>(slot)?;
            let r = pmove::jump_get_step_height(&ps, &origin, &mut step[0], &settings);
            put_floats(slot, &step);
            Ok(Ret::Int(i64::from(r)))
        })(),
        // ps, origin, jump_height
        "Jump_ClampVelocity" => (|| {
            let settings = MoveSettings {
                jump_height: args.f32(2),
                ..MoveSettings::default()
            };
            let origin = floats::<3>(args.buffer(1).ok_or("NULL origin")?)?;
            with_ps(args, 0, |ps| {
                pmove::jump_clamp_velocity(ps, &origin, &settings)
            })?;
            Ok(Ret::Void)
        })(),
        // pm, player_footstepsThreshhold
        "PM_ShouldMakeFootsteps" => (|| {
            let settings = MoveSettings {
                footsteps_threshold: args.f32(1),
                ..MoveSettings::default()
            };
            let r = with_pm(args, 0, |pm| pmove::should_make_footsteps(pm, &settings))?;
            Ok(Ret::Int(i64::from(r)))
        })(),
        // pm, results, start, mins, maxs, end, passEntityNum, contentMask, answers, log
        "PM_playerTrace" => (|| {
            let mut v = [[0f32; 3]; 4];
            for (k, i) in (2..6).enumerate() {
                v[k] = floats::<3>(args.buffer(i).ok_or("NULL vector")?)?;
            }
            let [start, mins, maxs, end] = v;
            let (pass, mask) = (args.i32(6), args.i32(7));
            let mut world = CheckWorld::new(args.bytes(8));
            let results = with_pm(args, 0, |pm| {
                pmove::player_trace(pm, &start, &mins, &maxs, &end, pass, mask, &mut world)
            })?;
            args.buffer(1)
                .ok_or("NULL results")?
                .copy_from_slice(&results.bytes);
            *args.buffer(9).ok_or("NULL call log")? = world.log.0;
            Ok(Ret::Void)
        })(),
        // planes, velocity, planeCount, permutation
        "PM_PermuteRestrictiveClipPlanes" => (|| {
            let count = usize::try_from(args.i32(2)).map_err(|_| "bad count")?;
            let raw = args.buffer(0).ok_or("NULL planes")?.clone();
            let planes: Vec<[f32; 3]> = (0..count)
                .map(|i| floats::<3>(raw.get(12 * i..).unwrap_or(&[])))
                .collect::<Result<_, _>>()?;
            let velocity = floats::<3>(args.buffer(1).ok_or("NULL velocity")?)?;
            let slot = args.buffer(3).ok_or("NULL permutation")?;
            let mut order = vec![0usize; count];
            let r = slide::permute_restrictive_clip_planes(&velocity, &planes, &mut order);
            for (i, o) in order.iter().enumerate() {
                slot[4 * i..4 * i + 4].copy_from_slice(&(*o as i32).to_le_bytes());
            }
            Ok(Ret::F32(r))
        })(),
        // pm, pml, gravity, answers, log
        "PM_SlideMove" => (|| {
            let mut pml = MoveLocal::from_bytes(args.buffer(1).ok_or("NULL pml")?);
            let gravity = args.i32(2) != 0;
            let mut world = CheckWorld::new(args.bytes(3));
            let r = with_pm(args, 0, |pm| {
                slide::slide_move(pm, &mut pml, gravity, &mut world)
            })?;
            let slot = args.buffer(1).ok_or("NULL pml")?;
            let n = slot.len();
            slot.copy_from_slice(&pml.bytes[..n]);
            *args.buffer(4).ok_or("NULL call log")? = world.log.0;
            Ok(Ret::Int(i64::from(r)))
        })(),
        // ps, event
        "PM_AddEvent" => (|| {
            let event = args.i32(1);
            with_ps(args, 0, |ps| pmove::add_event(ps, event))?;
            Ok(Ret::Void)
        })(),
        // ps, pml
        "PM_FootstepType" => (|| {
            let ps = PlayerState::from_bytes(args.buffer(0).ok_or("NULL ps")?);
            let pml = MoveLocal::from_bytes(args.buffer(1).ok_or("NULL pml")?);
            Ok(Ret::Int(i64::from(pmove::footstep_type(&ps, &pml))))
        })(),
        // pm, pml, old bob cycle, new, footstep, answers, log
        "PM_FootstepEvent" => (|| {
            let pml = MoveLocal::from_bytes(args.buffer(1).ok_or("NULL pml")?);
            let (old, new, footstep) = (args.i32(2), args.i32(3), args.i32(4) != 0);
            let mut world = CheckWorld::new(args.bytes(5));
            with_pm(args, 0, |pm| {
                pmove::footstep_event(pm, &pml, old, new, footstep, &mut world)
            })?;
            *args.buffer(6).ok_or("NULL call log")? = world.log.0;
            Ok(Ret::Void)
        })(),
        // angles, forward, right, up
        "AngleVectors" => (|| {
            let angles = floats::<3>(args.buffer(0).ok_or("NULL angles")?)?;
            let mut outs: [Option<[f32; 3]>; 3] = [None; 3];
            for (k, o) in outs.iter_mut().enumerate() {
                if let Some(b) = args.buffer(1 + k) {
                    *o = Some(floats::<3>(b)?);
                }
            }
            let [mut f, mut r, mut u] = outs;
            math::angle_vectors(&angles, f.as_mut(), r.as_mut(), u.as_mut());
            for (k, o) in [f, r, u].iter().enumerate() {
                if let (Some(v), Some(b)) = (o, args.buffer(1 + k)) {
                    put_floats(b, v);
                }
            }
            Ok(Ret::Void)
        })(),
        // from, to, out, fraction
        "Vec3Lerp" => (|| {
            let from = floats::<3>(args.buffer(0).ok_or("NULL from")?)?;
            let to = floats::<3>(args.buffer(1).ok_or("NULL to")?)?;
            let out = math::vec3_lerp(&from, &to, args.f32(3));
            put_floats(args.buffer(2).ok_or("NULL out")?, &out);
            Ok(Ret::Void)
        })(),
        "vectopitch" => (|| {
            let v = floats::<3>(args.buffer(0).ok_or("NULL v")?)?;
            Ok(Ret::F32(math::vectopitch(&v)))
        })(),
        "AngleSubtract" => Ok(Ret::F32(math::angle_subtract(args.f32(0), args.f32(1)))),
        "AngleWrap360" => Ok(Ret::F32(math::angle_wrap360(args.f32(0)))),
        "AngleWrap180" => Ok(Ret::F32(math::angle_wrap180(args.f32(0)))),
        "AngleDelta" => Ok(Ret::F32(math::angle_delta(args.f32(0), args.f32(1)))),
        // position, yaw, leanFrac, roll, leanDist
        "AddLeanToPosition" => (|| {
            let (yaw, lean, roll, dist) = (args.f32(1), args.f32(2), args.f32(3), args.f32(4));
            let slot = args.buffer(0).ok_or("NULL position")?;
            let mut position = floats::<3>(slot)?;
            math::add_lean_to_position(&mut position, yaw, lean, roll, dist);
            put_floats(slot, &position);
            Ok(Ret::Void)
        })(),
        // yaw, normal
        "PitchForYawOnNormal" => (|| {
            let normal = floats::<3>(args.buffer(1).ok_or("NULL normal")?)?;
            Ok(Ret::F32(math::pitch_for_yaw_on_normal(
                args.f32(0),
                &normal,
            )))
        })(),
        // passEntityNum, vPos, fSize, fHeight, fYaw, the three outputs,
        // bAlreadyProne, bOnGround, vGroundNormal, handler, proneCheckType,
        // prone_feet_dist, answers, log
        "BG_CheckProne" => (|| {
            let ground_normal = match args.buffer(10) {
                Some(b) => Some(floats::<3>(b)?),
                None => None,
            };
            let q = ProneQuery {
                pass_entity: args.i32(0),
                origin: floats::<3>(args.buffer(1).ok_or("NULL vPos")?)?,
                size: args.f32(2),
                height: args.f32(3),
                yaw: args.f32(4),
                already_prone: args.i32(8) != 0,
                on_ground: args.i32(9) != 0,
                ground_normal,
                handler: args.i32(11) as u8,
                check_type: args.i32(12),
                feet_distance: args.f32(13),
            };
            let mut world = CheckWorld::new(args.bytes(14));
            let mut values: [Option<f32>; 3] = [None; 3];
            for (k, v) in values.iter_mut().enumerate() {
                if let Some(b) = args.buffer(5 + k) {
                    *v = Some(floats::<1>(b)?[0]);
                }
            }
            let [mut h, mut t, mut w] = values;
            let out = ProneAngles {
                torso_height: h.as_mut(),
                torso_pitch: t.as_mut(),
                waist_pitch: w.as_mut(),
            };
            let r = prone::check_prone(&q, out, &mut world);
            for (k, v) in [h, t, w].iter().enumerate() {
                if let (Some(v), Some(b)) = (v, args.buffer(5 + k)) {
                    put_floats(b, &[*v]);
                }
            }
            *args.buffer(15).ok_or("NULL call log")? = world.log.0;
            Ok(Ret::Int(i64::from(r)))
        })(),
        // pm, start origin, start velocity, answers, log
        "PM_VerifyPronePosition" => (|| {
            let origin = floats::<3>(args.buffer(1).ok_or("NULL origin")?)?;
            let velocity = floats::<3>(args.buffer(2).ok_or("NULL velocity")?)?;
            let mut world = CheckWorld::new(args.bytes(3));
            let r = with_pm(args, 0, |pm| {
                prone::verify_prone_position(pm, &origin, &velocity, &mut world)
            })?;
            *args.buffer(4).ok_or("NULL call log")? = world.log.0;
            Ok(Ret::Int(i64::from(r)))
        })(),
        // pm, pml, gravity, jump_height, jump_stepSize,
        // player_footstepsThreshhold, answers, log
        "PM_StepSlideMove" => (|| {
            let mut pml = MoveLocal::from_bytes(args.buffer(1).ok_or("NULL pml")?);
            let gravity = args.i32(2) != 0;
            let settings = MoveSettings {
                jump_height: args.f32(3),
                jump_step_size: args.f32(4),
                footsteps_threshold: args.f32(5),
                ..MoveSettings::default()
            };
            let mut world = CheckWorld::new(args.bytes(6));
            with_pm(args, 0, |pm| {
                slide::step_slide_move(pm, &mut pml, gravity, &settings, &mut world)
            })?;
            let slot = args.buffer(1).ok_or("NULL pml")?;
            let n = slot.len();
            slot.copy_from_slice(&pml.bytes[..n]);
            *args.buffer(7).ok_or("NULL call log")? = world.log.0;
            Ok(Ret::Void)
        })(),
        // pm, pml, the move's dvars, answers, log
        "PM_AirMove" => (|| {
            let mut pml = MoveLocal::from_bytes(args.buffer(1).ok_or("NULL pml")?);
            let settings = move_settings(args, 2);
            let k = 2 + MOVE_DVAR_COUNT;
            let mut world = CheckWorld::new(args.bytes(k));
            with_pm(args, 0, |pm| {
                moves::air_move(pm, &mut pml, &settings, &mut world)
            })?;
            put_pml(args, 1, &pml)?;
            *args.buffer(k + 1).ok_or("NULL call log")? = world.log.0;
            Ok(Ret::Void)
        })(),
        // pml, ps
        "PM_DeadMove" => (|| {
            let pml = MoveLocal::from_bytes(args.buffer(0).ok_or("NULL pml")?);
            with_ps(args, 1, |ps| moves::dead_move(ps, &pml))?;
            Ok(Ret::Void)
        })(),
        // pm, pml, the move's dvars, answers, log
        "PM_FlyMove" => (|| {
            let mut pml = MoveLocal::from_bytes(args.buffer(1).ok_or("NULL pml")?);
            let settings = move_settings(args, 2);
            let k = 2 + MOVE_DVAR_COUNT;
            let mut world = CheckWorld::new(args.bytes(k));
            with_pm(args, 0, |pm| {
                moves::fly_move(pm, &mut pml, &settings, &mut world)
            })?;
            put_pml(args, 1, &pml)?;
            *args.buffer(k + 1).ok_or("NULL call log")? = world.log.0;
            Ok(Ret::Void)
        })(),
        // pm, pml, the move's dvars, scripts, answers, log
        "PM_LadderMove" => (|| {
            let mut pml = MoveLocal::from_bytes(args.buffer(1).ok_or("NULL pml")?);
            let settings = move_settings(args, 2);
            let k = 2 + MOVE_DVAR_COUNT;
            let mut world = CheckWorld::new(args.bytes(k + 1)).scripts(args.bytes(k));
            with_pm(args, 0, |pm| {
                moves::ladder_move(pm, &mut pml, &settings, &mut world)
            })?;
            put_pml(args, 1, &pml)?;
            *args.buffer(k + 2).ok_or("NULL call log")? = world.log.0;
            Ok(Ret::Void)
        })(),
        // pm, pml, the move's dvars, weapons, scripts, answers, log
        "PM_WalkMove" => (|| {
            let mut pml = MoveLocal::from_bytes(args.buffer(1).ok_or("NULL pml")?);
            let settings = move_settings(args, 2);
            let k = 2 + MOVE_DVAR_COUNT;
            let mut world = CheckWorld::new(args.bytes(k + 2))
                .weapons(args.bytes(k))
                .scripts(args.bytes(k + 1));
            with_pm(args, 0, |pm| {
                moves::walk_move(pm, &mut pml, &settings, &mut world)
            })?;
            put_pml(args, 1, &pml)?;
            *args.buffer(k + 3).ok_or("NULL call log")? = world.log.0;
            Ok(Ret::Void)
        })(),
        // ps
        "PM_GetEffectiveStance" => (|| {
            let ps = PlayerState::from_bytes(args.buffer(0).ok_or("NULL ps")?);
            Ok(Ret::Int(i64::from(pmove::effective_stance(&ps))))
        })(),
        // pm, from, to
        "PM_GetViewHeightLerp" => (|| {
            let (from, to) = (args.i32(1), args.i32(2));
            let r = with_pm(args, 0, |pm| pmove::view_height_lerp(pm, from, to))?;
            Ok(Ret::F32(r.store()))
        })(),
        // damage timer, player_dmgtimer_maxTime, player_dmgtimer_minScale
        "PM_DamageScale_Walk" => {
            let settings = MoveSettings {
                dmg_timer_max_time: args.f32(1),
                dmg_timer_min_scale: args.f32(2),
                ..MoveSettings::default()
            };
            Ok(Ret::F32(
                pmove::damage_scale_walk(args.i32(0), &settings).store(),
            ))
        }
        // pm, cmd, player_backSpeedScale, player_strafeSpeedScale, weapons
        "PM_CmdScale_Walk" => (|| {
            let settings = MoveSettings {
                back_speed_scale: args.f32(2),
                strafe_speed_scale: args.f32(3),
                ..MoveSettings::default()
            };
            let world = CheckWorld::new(&[]).weapons(args.bytes(4));
            let cmd = args.buffer(1).ok_or("NULL cmd")?.clone();
            let r = with_pm(args, 0, |pm| {
                pmove::cmd_scale_walk(pm, &cmd, &settings, &world)
            })?;
            Ok(Ret::F32(r.store()))
        })(),
        // ps, jump_slowdownEnable
        "Jump_ApplySlowdown" => (|| {
            let settings = MoveSettings {
                jump_slowdown_enable: flag(args, 1),
                ..MoveSettings::default()
            };
            with_ps(args, 0, |ps| jump::jump_apply_slowdown(ps, &settings))?;
            Ok(Ret::Void)
        })(),
        // ps, jump_slowdownEnable
        "Jump_GetLandFactor" => (|| {
            let settings = MoveSettings {
                jump_slowdown_enable: flag(args, 1),
                ..MoveSettings::default()
            };
            let ps = PlayerState::from_bytes(args.buffer(0).ok_or("NULL ps")?);
            Ok(Ret::F32(jump::jump_get_land_factor(&ps, &settings).store()))
        })(),
        // pm, pml, height, jump_slowdownEnable, jump_spreadAdd
        "Jump_Start" => (|| {
            let settings = MoveSettings {
                jump_slowdown_enable: flag(args, 3),
                jump_spread_add: args.f32(4),
                ..MoveSettings::default()
            };
            let height = args.f32(2);
            let mut pml = MoveLocal::from_bytes(args.buffer(1).ok_or("NULL pml")?);
            with_pm(args, 0, |pm| {
                jump::jump_start(pm, &mut pml, height, &settings)
            })?;
            put_pml(args, 1, &pml)?;
            Ok(Ret::Void)
        })(),
        // ps, pml
        "Jump_AddSurfaceEvent" => (|| {
            let pml = MoveLocal::from_bytes(args.buffer(1).ok_or("NULL pml")?);
            with_ps(args, 0, |ps| jump::jump_add_surface_event(ps, &pml))?;
            Ok(Ret::Void)
        })(),
        // ps, pml, jump_ladderPushVel
        "Jump_PushOffLadder" => (|| {
            let settings = MoveSettings {
                jump_ladder_push_vel: args.f32(2),
                ..MoveSettings::default()
            };
            let pml = MoveLocal::from_bytes(args.buffer(1).ok_or("NULL pml")?);
            with_ps(args, 0, |ps| {
                jump::jump_push_off_ladder(ps, &pml, &settings)
            })?;
            Ok(Ret::Void)
        })(),
        // pm, pml, jump_slowdownEnable, jump_height, jump_spreadAdd, jump_ladderPushVel, scripts, log
        "Jump_Check" => (|| {
            let settings = MoveSettings {
                jump_slowdown_enable: flag(args, 2),
                jump_height: args.f32(3),
                jump_spread_add: args.f32(4),
                jump_ladder_push_vel: args.f32(5),
                ..MoveSettings::default()
            };
            let mut world = CheckWorld::new(&[]).scripts(args.bytes(6));
            let mut pml = MoveLocal::from_bytes(args.buffer(1).ok_or("NULL pml")?);
            let r = with_pm(args, 0, |pm| {
                jump::jump_check(pm, &mut pml, &settings, &mut world)
            })?;
            put_pml(args, 1, &pml)?;
            *args.buffer(7).ok_or("NULL call log")? = world.log.0;
            Ok(Ret::Int(i64::from(r)))
        })(),
        "RoundFloatToInt" => Ok(Ret::Int(i64::from(math::round_float_to_int(args.f32(0))))),
        // ps, newProneYaw, handler, answers, log
        "BG_CheckProneTurned" => (|| {
            let (yaw, handler) = (args.f32(1), args.i32(2) as u8);
            let mut world = CheckWorld::new(args.bytes(3));
            let r = with_ps(args, 0, |ps| {
                prone::check_prone_turned(ps, yaw, handler, &mut world)
            })?;
            *args.buffer(4).ok_or("NULL call log")? = world.log.0;
            Ok(Ret::Int(i64::from(r)))
        })(),
        // ps, msec, cmd, capsuleTrace (the trace function's address), answers, log
        "PM_UpdateLean" => (|| {
            let msec = args.f32(1);
            let command = args.buffer(2).ok_or("NULL cmd")?.clone();
            let handler = trace_handler(args.i32(3))?;
            let mut world = CheckWorld::new(args.bytes(4));
            with_ps(args, 0, |ps| {
                view::update_lean(ps, msec, &command, handler, &mut world)
            })?;
            *args.buffer(5).ok_or("NULL call log")? = world.log.0;
            Ok(Ret::Void)
        })(),
        // ps, msec, cmd, handler, the view's six dvars, answers, log
        "PM_UpdateViewAngles" => (|| {
            let (msec, handler) = (args.f32(1), args.i32(3) as u8);
            let command = args.buffer(2).ok_or("NULL cmd")?.clone();
            let settings = view_settings(args, 4);
            let mut world = CheckWorld::new(args.bytes(10));
            with_ps(args, 0, |ps| {
                view::update_view_angles(ps, msec, &command, handler, &settings, &mut world)
            })?;
            *args.buffer(11).ok_or("NULL call log")? = world.log.0;
            Ok(Ret::Void)
        })(),
        // ps, mantle_enable, mantle_view_yawcap
        "Mantle_CapView" => (|| {
            let settings = MantleSettings {
                enable: flag(args, 1),
                view_yaw_cap: args.f32(2),
            };
            with_ps(args, 0, |ps| mantle::mantle_cap_view(ps, &settings))?;
            Ok(Ret::Void)
        })(),
        // event, eventParm, ps
        "BG_AddPredictableEventToPlayerstate" => (|| {
            let (event, parm) = (args.i32(0), args.i32(1));
            with_ps(args, 2, |ps| pmove::add_predictable_event(event, parm, ps))?;
            Ok(Ret::Void)
        })(),
        // pml
        "PM_MediumLandingForSurface" | "PM_HardLandingForSurface" => (|| {
            let pml = MoveLocal::from_bytes(args.buffer(0).ok_or("NULL pml")?);
            Ok(Ret::Int(i64::from(
                if function == "PM_MediumLandingForSurface" {
                    ground::medium_landing_for_surface(&pml)
                } else {
                    ground::hard_landing_for_surface(&pml)
                },
            )))
        })(),
        // ps, pml, bg_fallDamageMinHeight, bg_fallDamageMaxHeight, scripts, log
        "PM_CrashLand" => (|| {
            let pml = MoveLocal::from_bytes(args.buffer(1).ok_or("NULL pml")?);
            let settings = fall_settings(args, 2);
            let mut world = CheckWorld::new(&[]).scripts(args.bytes(4));
            with_ps(args, 0, |ps| {
                ground::crash_land(ps, &pml, &settings, &mut world)
            })?;
            *args.buffer(5).ok_or("NULL call log")? = world.log.0;
            Ok(Ret::Void)
        })(),
        // pm, pml, trace, answers, log
        "PM_CorrectAllSolid" => (|| {
            let mut pml = MoveLocal::from_bytes(args.buffer(1).ok_or("NULL pml")?);
            let mut trace = Trace::default();
            let raw = args.buffer(2).ok_or("NULL trace")?;
            let n = raw.len().min(TRACE_SIZE);
            trace.bytes[..n].copy_from_slice(&raw[..n]);
            let mut world = CheckWorld::new(args.bytes(3));
            let r = with_pm(args, 0, |pm| {
                ground::correct_all_solid(pm, &mut pml, &mut trace, &mut world)
            })?;
            put_pml(args, 1, &pml)?;
            let slot = args.buffer(2).ok_or("NULL trace")?;
            let n = slot.len().min(TRACE_SIZE);
            slot[..n].copy_from_slice(&trace.bytes[..n]);
            *args.buffer(4).ok_or("NULL call log")? = world.log.0;
            Ok(Ret::Int(i64::from(r)))
        })(),
        // pm, pml, scripts, answers, log
        "PM_GroundTraceMissed" => (|| {
            let mut pml = MoveLocal::from_bytes(args.buffer(1).ok_or("NULL pml")?);
            let mut world = CheckWorld::new(args.bytes(3)).scripts(args.bytes(2));
            with_pm(args, 0, |pm| {
                ground::ground_trace_missed(pm, &mut pml, &mut world)
            })?;
            put_pml(args, 1, &pml)?;
            *args.buffer(4).ok_or("NULL call log")? = world.log.0;
            Ok(Ret::Void)
        })(),
        // pm, pml, bg_fallDamageMinHeight, bg_fallDamageMaxHeight, scripts, answers, log
        "PM_GroundTrace" => (|| {
            let mut pml = MoveLocal::from_bytes(args.buffer(1).ok_or("NULL pml")?);
            let settings = fall_settings(args, 2);
            let mut world = CheckWorld::new(args.bytes(5)).scripts(args.bytes(4));
            with_pm(args, 0, |pm| {
                ground::ground_trace(pm, &mut pml, &settings, &mut world)
            })?;
            put_pml(args, 1, &pml)?;
            *args.buffer(6).ok_or("NULL call log")? = world.log.0;
            Ok(Ret::Void)
        })(),
        // ps
        "Jump_ActivateSlowdown" => (|| {
            with_ps(args, 0, jump::jump_activate_slowdown)?;
            Ok(Ret::Void)
        })(),
        "PM_ExitAimDownSight" => (|| {
            with_ps(args, 0, pmove::exit_aim_down_sight)?;
            Ok(Ret::Void)
        })(),
        // target, down
        "PM_GetViewHeightLerpTime" => Ok(Ret::Int(i64::from(duck::view_height_lerp_time(
            args.i32(0),
            args.i32(1),
        )))),
        // fraction, table (one of the game's, by address), the offset out
        "PM_ViewHeightTableLerp" => (|| {
            let table: &[duck::ViewLerpWaypoint] = match args.i32(1) as u32 {
                0x005D_B630 => &duck::STAND_TO_CROUCH,
                0x005D_B6A0 => &duck::CROUCH_TO_STAND,
                0x005D_B710 => &duck::CROUCH_TO_PRONE,
                0x005D_B7B8 => &duck::PRONE_TO_CROUCH,
                other => return Err(format!("no view height table at {other:#x}")),
            };
            let (height, offset) = duck::view_height_table_lerp(args.i32(0), table);
            put_floats(args.buffer(2).ok_or("NULL offset")?, &[offset]);
            Ok(Ret::F32(height.store()))
        })(),
        // pm, pml, jump_height, jump_stepSize, player_footstepsThreshhold, answers, log
        "PM_ViewHeightAdjust" => (|| {
            let mut pml = MoveLocal::from_bytes(args.buffer(1).ok_or("NULL pml")?);
            let settings = step_settings(args, 2);
            let mut world = CheckWorld::new(args.bytes(5));
            with_pm(args, 0, |pm| {
                duck::view_height_adjust(pm, &mut pml, &settings, &mut world)
            })?;
            put_pml(args, 1, &pml)?;
            *args.buffer(6).ok_or("NULL call log")? = world.log.0;
            Ok(Ret::Void)
        })(),
        // pm, pml, jump_height, jump_stepSize, player_footstepsThreshhold, scripts, answers, log
        "PM_CheckDuck" => (|| {
            let mut pml = MoveLocal::from_bytes(args.buffer(1).ok_or("NULL pml")?);
            let settings = step_settings(args, 2);
            let mut world = CheckWorld::new(args.bytes(6)).scripts(args.bytes(5));
            with_pm(args, 0, |pm| {
                duck::check_duck(pm, &mut pml, &settings, &mut world)
            })?;
            put_pml(args, 1, &pml)?;
            *args.buffer(7).ok_or("NULL call log")? = world.log.0;
            Ok(Ret::Void)
        })(),
        // v
        "SnapVector" => (|| {
            let slot = args.buffer(0).ok_or("NULL vector")?;
            let mut v = floats::<3>(slot)?;
            math::snap_vector(&mut v);
            put_floats(slot, &v);
            Ok(Ret::Void)
        })(),
        // pm, pml, the move's dvars
        "PM_NoclipMove" | "PM_UFOMove" => (|| {
            let pml = MoveLocal::from_bytes(args.buffer(1).ok_or("NULL pml")?);
            let settings = move_settings(args, 2);
            let ufo = function == "PM_UFOMove";
            with_pm(args, 0, |pm| {
                if ufo {
                    moves::ufo_move(pm, &pml, &settings)
                } else {
                    moves::noclip_move(pm, &pml, &settings)
                }
            })?;
            Ok(Ret::Void)
        })(),
        // pm, the five foliage dvars, answers, log
        "PM_FoliageSounds" => (|| {
            let settings = MoveSettings {
                foliage_min_speed: args.f32(1),
                foliage_max_speed: args.f32(2),
                foliage_slow_interval: args.i32(3),
                foliage_fast_interval: args.i32(4),
                foliage_reset_interval: args.i32(5),
                ..MoveSettings::default()
            };
            let mut world = CheckWorld::new(args.bytes(6));
            with_pm(args, 0, |pm| {
                pmove::foliage_sounds(pm, &settings, &mut world)
            })?;
            *args.buffer(7).ok_or("NULL call log")? = world.log.0;
            Ok(Ret::Void)
        })(),
        // pm
        "PM_UpdateAdsSlowdown" => (|| {
            with_pm(args, 0, pmove::update_ads_slowdown)?;
            Ok(Ret::Void)
        })(),
        // pm, pml, answers, log
        "PM_UpdatePronePitch" => (|| {
            let pml = MoveLocal::from_bytes(args.buffer(1).ok_or("NULL pml")?);
            let mut world = CheckWorld::new(args.bytes(2));
            with_pm(args, 0, |pm| {
                prone::update_prone_pitch(pm, &pml, &mut world)
            })?;
            *args.buffer(3).ok_or("NULL call log")? = world.log.0;
            Ok(Ret::Void)
        })(),
        // ps
        "PM_LeaveLadder" => (|| {
            with_ps(args, 0, moves::leave_ladder)?;
            Ok(Ret::Void)
        })(),
        // pm, pml, scripts, answers, log
        "PM_CheckLadderMove" => (|| {
            let mut pml = MoveLocal::from_bytes(args.buffer(1).ok_or("NULL pml")?);
            let mut world = CheckWorld::new(args.bytes(3)).scripts(args.bytes(2));
            with_pm(args, 0, |pm| {
                moves::check_ladder_move(pm, &mut pml, &mut world)
            })?;
            put_pml(args, 1, &pml)?;
            *args.buffer(4).ok_or("NULL call log")? = world.log.0;
            Ok(Ret::Void)
        })(),
        // client, condition, value, checkConversion, the client's animation state
        "BG_UpdateConditionValue" => (|| {
            let (condition, value, check) = (args.i32(1), args.i32(2), args.i32(3) != 0);
            let slot = args.buffer(4).ok_or("NULL animation state")?;
            let mut info = ClientAnimInfo::from_bytes(slot);
            anim::update_condition_value(&mut info, condition, value, check);
            put_anim_info(args, 4, &info)?;
            Ok(Ret::Void)
        })(),
        // pm, definitions, the player's animation state
        "BG_AnimUpdatePlayerStateConditions" => (|| {
            let info = args.buffer(2).ok_or("NULL animation state")?.clone();
            let mut world = CheckWorld::new(&[]).defs(args.bytes(1)).anim_info(&info);
            with_pm(args, 0, |pm| {
                anim::update_player_state_conditions(pm, &mut world)
            })?;
            put_anim_info(args, 2, &world.anim_info)?;
            Ok(Ret::Void)
        })(),
        // definitions
        "BG_GetBinocularWeaponIndex" => {
            let world = CheckWorld::new(&[]).defs(args.bytes(0));
            Ok(Ret::Int(i64::from(weapon::binocular_weapon_index(&world))))
        }
        // weapon state
        "PM_IsBinocularsUp" => Ok(Ret::Int(i64::from(weapon::binoculars_up(args.i32(0))))),
        // ps, pml, definitions
        "PM_IsAdsAllowed" => (|| {
            let pml = MoveLocal::from_bytes(args.buffer(1).ok_or("NULL pml")?);
            let world = CheckWorld::new(&[]).defs(args.bytes(2));
            let allowed = with_ps(args, 0, |ps| weapon::is_ads_allowed(ps, &pml, &world))?;
            Ok(Ret::Int(i64::from(allowed)))
        })(),
        // pm, pml, definitions, the player's animation state
        "PM_UpdateAimDownSightFlag" => (|| {
            let pml = MoveLocal::from_bytes(args.buffer(1).ok_or("NULL pml")?);
            let info = args.buffer(3).ok_or("NULL animation state")?.clone();
            let mut world = CheckWorld::new(&[]).defs(args.bytes(2)).anim_info(&info);
            with_pm(args, 0, |pm| {
                weapon::update_aim_down_sight_flag(pm, &pml, &mut world)
            })?;
            put_anim_info(args, 3, &world.anim_info)?;
            Ok(Ret::Void)
        })(),
        // val, min, max
        "I_fclamp" => Ok(Ret::F32(math::i_fclamp(
            args.f32(0),
            args.f32(1),
            args.f32(2),
        ))),
        // pm, pml, player_scopeExitOnDamage, player_adsExitDelay, definitions
        "PM_UpdateAimDownSightLerp" => (|| {
            let pml = MoveLocal::from_bytes(args.buffer(1).ok_or("NULL pml")?);
            let settings = MoveSettings {
                scope_exit_on_damage: flag(args, 2),
                ads_exit_delay: args.i32(3),
                ..MoveSettings::default()
            };
            let world = CheckWorld::new(&[]).defs(args.bytes(4));
            with_pm(args, 0, |pm| {
                weapon::update_aim_down_sight_lerp(pm, &pml, &settings, &world)
            })?;
            Ok(Ret::Void)
        })(),
        // pm, pml, bg_aimSpreadMoveSpeedThreshold, definitions
        "PM_UpdateAimSpreadScale" => (|| {
            let pml = MoveLocal::from_bytes(args.buffer(1).ok_or("NULL pml")?);
            let settings = MoveSettings {
                aim_spread_move_speed_threshold: args.f32(2),
                ..MoveSettings::default()
            };
            let world = CheckWorld::new(&[]).defs(args.bytes(3));
            with_pm(args, 0, |pm| {
                weapon::update_aim_spread_scale(pm, &pml, &settings, &world)
            })?;
            Ok(Ret::Void)
        })(),
        // ps
        "PM_Weapon_FinishWeaponRaise" => ps_only(args, ws::finish_weapon_raise),
        "PM_Weapon_FinishReloadEnd" => ps_only(args, ws::finish_reload_end),
        "PM_Weapon_FinishRechamber" => ps_only(args, ws::finish_rechamber),
        "PM_Weapon_MeleeEnd" => ps_only(args, ws::melee_end),
        "PM_Weapon_OffHandEmpty" => ps_only(args, ws::offhand_empty),
        "PM_ExitBinoculars" => ps_only(args, ws::exit_binoculars),
        "PM_ResetWeaponState" => ps_only(args, ws::reset_weapon_state),
        "PM_Weapon_Idle" => ps_only(args, ws::weapon_idle),
        "PM_Weapon_ResetUnlessBusy" => {
            with_ps(args, 0, ws::reset_unless_busy).map(|r| Ret::Int(i64::from(r)))
        }
        // ps, definitions
        "PM_Weapon_MeleeFire" => ps_defs(args, ws::melee_fire),
        "PM_Weapon_OffHandInit" => ps_defs(args, ws::offhand_init),
        "PM_Weapon_OffHandPrepare" => ps_defs(args, ws::offhand_prepare),
        "PM_Weapon_OffHandHold" => ps_defs(args, ws::offhand_hold),
        "PM_Weapon_OffHandEnd" => ps_defs(args, ws::offhand_end),
        "PM_Binoculars_DropWeapon" => ps_defs(args, ws::binoculars_drop_weapon),
        "PM_Binoculars_Raise" => ps_defs(args, ws::binoculars_raise),
        "PM_WeaponAmmoInClip" => (|| {
            let world = CheckWorld::new(&[]).defs(args.bytes(1));
            let n = with_ps(args, 0, |ps| ws::ammo_in_clip(ps, &world))?;
            Ok(Ret::Int(i64::from(n)))
        })(),
        // ps, player_adsExitDelay, definitions
        "PM_Binoculars_Lower" | "PM_Binoculars_ExitSights" => (|| {
            let settings = MoveSettings {
                ads_exit_delay: args.i32(1),
                ..MoveSettings::default()
            };
            let world = CheckWorld::new(&[]).defs(args.bytes(2));
            let f = if function == "PM_Binoculars_Lower" {
                ws::binoculars_lower
            } else {
                ws::binoculars_exit_sights
            };
            with_ps(args, 0, |ps| f(ps, &settings, &world))?;
            Ok(Ret::Void)
        })(),
        // pm, definitions
        "PM_Binoculars_DropBinoculars" => (|| {
            let world = CheckWorld::new(&[]).defs(args.bytes(1));
            with_pm(args, 0, |pm| ws::binoculars_drop(pm, &world))?;
            Ok(Ret::Void)
        })(),
        // ps, animation
        "PM_StartWeaponAnim" | "PM_ContinueWeaponAnim" => (|| {
            let anim = args.i32(1);
            let f = if function == "PM_StartWeaponAnim" {
                ws::start_weapon_anim
            } else {
                ws::continue_weapon_anim
            };
            with_ps(args, 0, |ps| f(ps, anim))?;
            Ok(Ret::Void)
        })(),
        // ps, class, definitions
        "BG_GetFirstOffhandOfClass" => (|| {
            let class = args.i32(1);
            let world = CheckWorld::new(&[]).defs(args.bytes(2));
            let w = with_ps(args, 0, |ps| ws::first_offhand_of_class(ps, class, &world))?;
            Ok(Ret::Int(i64::from(w)))
        })(),
        // ps, weapon, definitions
        "BG_IsWeaponValid" => (|| {
            let weapon = args.i32(1);
            let world = CheckWorld::new(&[]).defs(args.bytes(2));
            let valid = with_ps(args, 0, |ps| ws::is_weapon_valid(ps, weapon, &world))?;
            Ok(Ret::Int(i64::from(valid)))
        })(),
        // pm, delayedAction
        "PM_Weapon_FireButtonReleased" => (|| {
            let delayed = args.i32(1);
            let r = with_pm(args, 0, |pm| ws::fire_button_released(pm, delayed))?;
            Ok(Ret::Int(i64::from(r)))
        })(),
        // ps, pml, definitions
        "PM_Weapon_GrenadeCookOff" => (|| {
            let pml = MoveLocal::from_bytes(args.buffer(1).ok_or("NULL pml")?);
            let world = CheckWorld::new(&[]).defs(args.bytes(2));
            let r = with_ps(args, 0, |ps| ws::grenade_cook_off(ps, &pml, &world))?;
            Ok(Ret::Int(i64::from(r)))
        })(),
        // tgt, cur, rate, deltaTime
        "DiffTrack" => Ok(Ret::F32(math::diff_track(
            args.f32(0),
            args.f32(1),
            args.f32(2),
            args.f32(3),
        ))),
        // pm, pml, the five breath dvars, definitions
        "PM_UpdateHoldBreath" => (|| {
            let pml = MoveLocal::from_bytes(args.buffer(1).ok_or("NULL pml")?);
            let settings = MoveSettings {
                breath_hold_time: args.f32(2),
                breath_gasp_time: args.f32(3),
                breath_gasp_scale: args.f32(4),
                breath_hold_lerp: args.f32(5),
                breath_gasp_lerp: args.f32(6),
                ..MoveSettings::default()
            };
            let world = CheckWorld::new(&[]).defs(args.bytes(7));
            with_pm(args, 0, |pm| {
                weapon::update_hold_breath(pm, &pml, &settings, &world)
            })?;
            Ok(Ret::Void)
        })(),
        _ => return None,
    })
}

/// A player state function of buffer 0.
fn ps_only(args: &mut Args, f: fn(&mut PlayerState)) -> Answer {
    with_ps(args, 0, f)?;
    Ok(Ret::Void)
}

/// A player state function of buffer 0 with the weapons' definitions (1).
fn ps_defs(args: &mut Args, f: fn(&mut PlayerState, &dyn MoveHost)) -> Answer {
    let world = CheckWorld::new(&[]).defs(args.bytes(1));
    with_ps(args, 0, |ps| f(ps, &world))?;
    Ok(Ret::Void)
}

/// The step slide move's three dvars at `first` (specs/game_pmove.py
/// `STEP_DVARS`: jump_height, jump_stepSize, player_footstepsThreshhold).
fn step_settings(args: &Args, first: usize) -> MoveSettings {
    MoveSettings {
        jump_height: args.f32(first),
        jump_step_size: args.f32(first + 1),
        footsteps_threshold: args.f32(first + 2),
        ..MoveSettings::default()
    }
}

/// The fall damage's two dvars at `first` (specs/game_pmove.py
/// `FALL_DVARS`: bg_fallDamageMinHeight, bg_fallDamageMaxHeight).
fn fall_settings(args: &Args, first: usize) -> MoveSettings {
    MoveSettings {
        fall_damage_min_height: args.f32(first),
        fall_damage_max_height: args.f32(first + 1),
        ..MoveSettings::default()
    }
}

/// The handler (0 the server's, 1 the client game's) of a trace function's
/// address (specs/game_pmove.py `TRACE_FUNCTIONS`).
fn trace_handler(address: i32) -> Result<u8, String> {
    match address as u32 {
        0x004D_E690 => Ok(0),
        0x004F_D900 => Ok(1),
        other => Err(format!("no trace function at {other:#x}")),
    }
}

/// How many dvars a whole move reads (specs/game_pmove.py `MOVE_DVARS`).
const MOVE_DVAR_COUNT: usize = 22;

/// Puts the move's scratch state back into buffer `i`.
fn put_pml(args: &mut Args, i: usize, pml: &MoveLocal) -> Result<(), String> {
    let slot = args.buffer(i).ok_or("NULL pml")?;
    let n = slot.len().min(pml.bytes.len());
    slot[..n].copy_from_slice(&pml.bytes[..n]);
    Ok(())
}

/// The settings a whole move reads, from the [`MOVE_DVAR_COUNT`] dvar
/// arguments at `first` (specs/game_pmove.py `MOVE_DVARS`' order).
fn move_settings(args: &Args, first: usize) -> MoveSettings {
    MoveSettings {
        stop_speed: args.f32(first),
        friction: args.f32(first + 1),
        jump_slowdown_enable: flag(args, first + 2),
        inertia_max: args.f32(first + 3),
        inertia_angle: args.f32(first + 4),
        inertia_debug: flag(args, first + 5),
        spectate_speed_scale: args.f32(first + 6),
        jump_height: args.f32(first + 7),
        jump_step_size: args.f32(first + 8),
        footsteps_threshold: args.f32(first + 9),
        back_speed_scale: args.f32(first + 10),
        strafe_speed_scale: args.f32(first + 11),
        dmg_timer_max_time: args.f32(first + 12),
        dmg_timer_min_scale: args.f32(first + 13),
        jump_spread_add: args.f32(first + 14),
        jump_ladder_push_vel: args.f32(first + 15),
        ..view_settings(args, first + 16)
    }
}

/// The view's six dvars at `first` (specs/game_pmove.py `VIEW_DVARS`: the
/// pitch limits, the ladder's and the prone body's yaw caps, the mantle's).
fn view_settings(args: &Args, first: usize) -> MoveSettings {
    MoveSettings {
        view_pitch_up: args.f32(first),
        view_pitch_down: args.f32(first + 1),
        ladder_yaw_cap: args.f32(first + 2),
        prone_yaw_cap: args.f32(first + 3),
        mantle: MantleSettings {
            enable: flag(args, first + 4),
            view_yaw_cap: args.f32(first + 5),
        },
        ..MoveSettings::default()
    }
}

/// The bytes of `pmove_t` after its player state pointer that a `pm`
/// buffer carries first (specs/game_pmove.py `PM_BLOCK`), then the player
/// state.
const PM_BLOCK: usize = PMOVE_SIZE - 4;

/// Runs `f` on buffer `i` as a move (its fields, then its player state)
/// and puts the bytes back.
fn with_pm<R>(args: &mut Args, i: usize, f: impl FnOnce(&mut Pmove) -> R) -> Result<R, String> {
    let slot = args.buffer(i).ok_or("NULL pm")?;
    if slot.len() < PM_BLOCK {
        return Err("short pm".into());
    }
    let (block, state) = slot.split_at_mut(PM_BLOCK);
    let mut fields = vec![0u8; 4];
    fields.extend_from_slice(block);
    let mut pm = Pmove::from_bytes(PlayerState::from_bytes(state), &fields);
    let r = f(&mut pm);
    block.copy_from_slice(&pm.bytes[4..]);
    let n = state.len();
    state.copy_from_slice(&pm.ps.bytes[..n]);
    Ok(r)
}
