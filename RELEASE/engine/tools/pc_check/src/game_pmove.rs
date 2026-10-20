//! Requests for `iw_pc::game::pmove` and `iw_pc::shared::math` (Call of Duty
//! 2; specs/game_pmove.py): the player state and the move's scratch state
//! arrive as buffers of their bytes, the settings as their values.

use iw_pc::game::moves;
use iw_pc::game::pmove::{self, MoveLocal, MoveSettings, PlayerState, Pmove, PMOVE_SIZE};
use iw_pc::game::prone::{self, ProneAngles, ProneQuery};
use iw_pc::game::slide;
use iw_pc::game::trace::{MoveHost, Trace, TRACE_SIZE};
use iw_pc::shared::math;
use iw_pc::Game;

use crate::net_client::{CallLog, Logged};
use crate::{Answer, Args, Ret};

/// The check's world: each trace is logged as the original's stand-ins
/// record it (its handler, the four vectors' bytes, the entity passed, the
/// mask) and answered with the case's next result (specs/game_pmove.py
/// `TraceAnswers`; nothing hit once they run out).
pub(crate) struct CheckWorld {
    answers: Vec<Trace>,
    next: usize,
    pub(crate) log: CallLog,
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
            log: CallLog::default(),
        }
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
        // pm, pml, the move's ten dvars, answers, log
        "PM_AirMove" => (|| {
            let mut pml = MoveLocal::from_bytes(args.buffer(1).ok_or("NULL pml")?);
            let settings = move_settings(args, 2);
            let mut world = CheckWorld::new(args.bytes(12));
            with_pm(args, 0, |pm| {
                moves::air_move(pm, &mut pml, &settings, &mut world)
            })?;
            let slot = args.buffer(1).ok_or("NULL pml")?;
            let n = slot.len();
            slot.copy_from_slice(&pml.bytes[..n]);
            *args.buffer(13).ok_or("NULL call log")? = world.log.0;
            Ok(Ret::Void)
        })(),
        "RoundFloatToInt" => Ok(Ret::Int(i64::from(math::round_float_to_int(args.f32(0))))),
        _ => return None,
    })
}

/// The settings a whole move reads, from the ten dvar arguments at `first`
/// (specs/game_pmove.py `MOVE_DVARS`' order).
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
