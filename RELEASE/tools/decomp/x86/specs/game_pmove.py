"""Checks for iw_pc::game::pmove and iw_pc::shared::math (Call of Duty 2):
player movement. The player state (`playerState_t`) and the move's scratch
state (`pml_t`) are buffers of their bytes (the part the functions use),
compared whole after the call; the settings (dvars) are stand-in variables
the dvar pointers point at (a float or a byte at +8).

Floats are made for movement: velocities and speeds of a few hundred units,
frame times of a few milliseconds, directions of length 1, and the edges:
0, -0, NaN, infinities, tiny and huge values, denormals."""
from __future__ import annotations

import math
import struct

from dataclasses import dataclass

from harness_x86 import Arg, Buffer, CallLog, F32, Int, RetBool8, RetF32, RetI32, RetVoid, spec
from oracle_x86 import StandIn
from specs.net_client import FloatSetting, Setting

IMAGE = "cod2mp"
forever = iter(int, 1)

DVARS = {"stopspeed": 0x019439E0, "friction": 0x01943A28, "jump_slowdownEnable": 0x0196FF8C,
         "inertiaMax": 0x01943A2C, "inertiaAngle": 0x01943A54, "inertiaDebug": 0x01943A14,
         "player_spectateSpeedScale": 0x01943A1C, "jump_height": 0x0196FF94, "jump_stepSize": 0x0196FF90,
         "player_footstepsThreshhold": 0x019439D4}
PS_SIZE = 0x100     # the part of playerState_t these functions use
PML_SIZE = 0x6C     # the part of pml_t
PS_FULL = 0x588     # the player state up to proneDirection
PM_BLOCK = 0xE4     # pmove_t after its player state pointer (iw_pc::game::pmove::PMOVE_SIZE - 4)
# Offsets (iw_pc::game::pmove::ps, ::pml).
PM_TYPE, PM_FLAGS, PM_TIME, VELOCITY, OLD_VELOCITY, JUMP_ORIGIN_Z = 0x04, 0x0C, 0x10, 0x20, 0x2C, 0x74
LEANF, SPEED, GRAVITY, VIEW_HEIGHT_TARGET = 0x4C, 0x50, 0x48, 0xF4
FRAME_TIME, MSEC, WALKING, GROUND_SURFACE_FLAGS = 0x24, 0x28, 0x2C, 0x48
LEGS_TIMER, TORSO_TIMER = 0x78, 0x80
ORIGIN, GROUND_ENTITY_NUM, LADDER_VEC, MOVEMENT_DIR, E_FLAGS = 0x14, 0x60, 0x64, 0x9C, 0xA0
VIEW_ANGLES, PRONE_DIRECTION, PREVIOUS_ORIGIN = 0xE8, 0x584, 0x60


def bits(x: float) -> int:
    return struct.unpack("<I", struct.pack("<f", x))[0]


def f32(rng, scale: float = 400.0) -> int:
    """A float's bits: mostly a movement-sized value, sometimes an edge."""
    k = rng.random()
    if k < 0.06:
        return bits(rng.choice([0.0, -0.0, 1.0, -1.0, 0.5, scale, -scale]))
    if k < 0.09:
        return bits(rng.choice([math.inf, -math.inf, math.nan, 1e30, -1e30, 1e-30, 3e38]))
    if k < 0.11:
        return rng.choice([0x7FC00000, 0xFFC00000, 1, 0x80000001, 0x007FFFFF, 0x00800000])
    if k < 0.3:
        return bits(float(rng.randint(-int(scale), int(scale))))
    if k < 0.4:
        return bits(rng.uniform(-2.0, 2.0))
    return bits(rng.uniform(-scale, scale))


def unit3(rng) -> list[int]:
    """A direction: mostly of length 1 (as the callers' wishdir), sometimes 0 or anything."""
    k = rng.random()
    if k < 0.08:
        return [0, 0, 0]
    if k < 0.15:
        return [f32(rng, 2.0) for _ in range(3)]
    x, y, z = rng.gauss(0, 1), rng.gauss(0, 1), rng.gauss(0, 1) if rng.random() < 0.3 else 0.0
    n = math.sqrt(x * x + y * y + z * z) or 1.0
    return [bits(x / n), bits(y / n), bits(z / n)]


def fl(word: int) -> float:
    """A float's value from its bits (for an F32 argument)."""
    return struct.unpack("<f", struct.pack("<I", word & 0xFFFFFFFF))[0]


def put(buf: bytearray, at: int, word: int) -> None:
    struct.pack_into("<I", buf, at, word & 0xFFFFFFFF)


def player_state(rng, size: int = PS_SIZE) -> bytes:
    ps = bytearray(rng.getrandbits(8) if rng.random() < 0.3 else 0 for _ in range(size))
    put(ps, PM_TYPE, rng.choice([0, 0, 0, 1, 2, 3, 4, 4, 5, 6, rng.getrandbits(8)]))
    flags = 0
    for bit in (0x20, 0x100, 0x200, 0x400, 0x80000):
        if rng.random() < 0.3:
            flags |= bit
    if rng.random() < 0.3:
        flags |= rng.getrandbits(27)
    put(ps, PM_FLAGS, flags)
    put(ps, PM_TIME, rng.choice([0, 1, 1699, 1700, 1800, 1801, rng.randint(0, 2500), rng.randint(-5, 5),
                                 rng.getrandbits(32)]))
    for i in range(3):
        put(ps, VELOCITY + 4 * i, f32(rng) if i < 2 or rng.random() < 0.6 else 0)
    for i in range(2):
        put(ps, OLD_VELOCITY + 4 * i, f32(rng) if rng.random() < 0.8 else 0)
    put(ps, JUMP_ORIGIN_Z, f32(rng))
    put(ps, LEANF, 0 if rng.random() < 0.6 else f32(rng, 1.0))
    for at in (LEGS_TIMER, TORSO_TIMER):
        put(ps, at, rng.choice([0, 0, 1, 50, 500, rng.randint(-100, 2000), rng.getrandbits(32)]))
    put(ps, SPEED, rng.choice([190, 190, 0, 1, 100, 250, rng.randint(0, 1000), rng.getrandbits(32)]))
    return bytes(ps)


def move_local(rng) -> bytes:
    pml = bytearray(rng.getrandbits(8) if rng.random() < 0.3 else 0 for _ in range(PML_SIZE))
    k = rng.random()
    frame = (rng.choice([0.001, 0.008, 0.016, 0.05, 0.1, 0.2]) if k < 0.6 else rng.uniform(0, 0.25)) if k < 0.95 \
        else struct.unpack("<f", struct.pack("<I", f32(rng, 1.0)))[0]
    put(pml, FRAME_TIME, bits(frame))
    put(pml, MSEC, rng.choice([1, 8, 16, 50, 66, 0, rng.randint(0, 300), rng.getrandbits(32)]))
    put(pml, WALKING, rng.choice([0, 1, 1, rng.getrandbits(32)]))
    put(pml, GROUND_SURFACE_FLAGS, rng.choice([0, 2, rng.getrandbits(32)]))
    return bytes(pml)


def setting_float(rng, typical: list[float]) -> int:
    return bits(rng.choice(typical)) if rng.random() < 0.7 else f32(rng, 200.0)


def _vec_cases(n: int):
    def cases(rng):
        for _ in forever:
            words = [f32(rng, rng.choice([1.0, 400.0, 1e5])) for _ in range(n)]
            if rng.random() < 0.1:
                words = [0] * n
            yield [struct.pack(f"<{n}I", *words)]
    return cases


def _jump_cases(rng):
    for _ in forever:
        yield [player_state(rng), rng.choice([0, 1, 1])]


def _friction_cases(rng):
    for _ in forever:
        yield [player_state(rng), move_local(rng), setting_float(rng, [100.0, 0.0, 50.0]),
               setting_float(rng, [5.5, 0.0, 8.0]), rng.choice([0, 1, 1])]


def _inertia_settings(rng) -> list:
    return [setting_float(rng, [50.0, 0.0, 200.0]), setting_float(rng, [0.0, 0.5, -0.5, 1.0]),
            rng.choice([0, 0, 1])]


def _accelerate_cases(rng):
    for _ in forever:
        wish = setting_float(rng, [190.0, 0.0, 100.0, 320.0])
        accel = setting_float(rng, [9.0, 10.0, 1.0, 4.0])
        yield [struct.pack("<3I", *unit3(rng)), player_state(rng), move_local(rng), fl(wish), fl(accel),
               setting_float(rng, [100.0, 0.0])] + _inertia_settings(rng)


def _turn_cases(rng):
    for _ in forever:
        # (inertiaMax only for inertiaDebug's line.)
        yield [player_state(rng), struct.pack("<3I", *unit3(rng)), fl(f32(rng, 60.0))] + _inertia_settings(rng)


def _clamp_cases(rng):
    for _ in forever:
        yield [player_state(rng), fl(f32(rng, 80.0)), struct.pack("<3I", *unit3(rng))] + _inertia_settings(rng)


def _ps(at: str) -> Buffer:
    return Buffer(size=PS_SIZE, terminated=False, at=at)


def _pml(at: str) -> Buffer:
    return Buffer(size=PML_SIZE, terminated=False, at=at)


def _vec(at: str, n: int = 3) -> Buffer:
    return Buffer(size=4 * n, terminated=False, at=at)


def _dvar_float(name: str) -> FloatSetting:
    return FloatSetting(pointer=DVARS[name], offset=8)


def _dvar_byte(name: str) -> Setting:
    return Setting(pointer=DVARS[name], offset=8)


spec(name="Vec3Normalize", image=IMAGE, rust="shared::math::vec3_normalize", args=[_vec("esi")], ret=RetF32(),
     cases=_vec_cases(3))
spec(name="Vec2Normalize", image=IMAGE, rust="shared::math::vec2_normalize", args=[_vec("esi", 2)], ret=RetF32(),
     cases=_vec_cases(2))
spec(name="Jump_ReduceFriction", image=IMAGE, rust="game::pmove::jump_reduce_friction",
     args=[_ps("eax"), _dvar_byte("jump_slowdownEnable")], ret=RetF32(), cases=_jump_cases)
spec(name="PM_Friction", image=IMAGE, rust="game::pmove::friction",
     args=[_ps("esi"), _pml("edi"), _dvar_float("stopspeed"), _dvar_float("friction"),
           _dvar_byte("jump_slowdownEnable")], ret=RetVoid(), cases=_friction_cases)
spec(name="PM_InertiaTurnTooSharp", image=IMAGE, rust="game::pmove::inertia_turn_too_sharp",
     args=[_ps("eax"), _vec("ecx"), F32(), _dvar_float("inertiaMax"), _dvar_float("inertiaAngle"),
           _dvar_byte("inertiaDebug")],
     ret=RetBool8(), cases=_turn_cases)
spec(name="PM_ClampInertia", image=IMAGE, rust="game::pmove::clamp_inertia",
     args=[_ps("edx"), F32(), _vec("stack"), _dvar_float("inertiaMax"), _dvar_float("inertiaAngle"),
           _dvar_byte("inertiaDebug")], ret=RetF32(), cases=_clamp_cases)
spec(name="PM_Accelerate", image=IMAGE, rust="game::pmove::accelerate",
     args=[_vec("eax"), _ps("edi"), _pml("ebx"), F32(), F32(), _dvar_float("stopspeed"), _dvar_float("inertiaMax"),
           _dvar_float("inertiaAngle"), _dvar_byte("inertiaDebug")], ret=RetVoid(), cases=_accelerate_cases)


def usercmd(rng) -> bytes:
    """A player command (28 bytes): forward and right moves at 0x18 and 0x19."""
    cmd = bytearray(rng.getrandbits(8) if rng.random() < 0.3 else 0 for _ in range(28))
    for at in (0x18, 0x19, 0x1A):
        cmd[at] = rng.choice([0, 0, 127, 0x81, 0x80, 64, 0xC0, rng.getrandbits(8)])
    return bytes(cmd)


def _move_scale_cases(rng):
    for _ in forever:
        moves = [fl(rng.choice([0, bits(127.0), bits(-127.0), bits(0.0), f32(rng, 127.0)])) for _ in range(3)]
        yield [player_state(rng)] + moves + [setting_float(rng, [1.0, 0.5, 2.0])]


def _cmd_scale_cases(rng):
    for _ in forever:
        yield [usercmd(rng), player_state(rng), setting_float(rng, [1.0, 0.5, 2.0])]


spec(name="PM_MoveScale", image=IMAGE, rust="game::pmove::move_scale",
     args=[_ps("ecx"), F32(), F32(), F32(), _dvar_float("player_spectateSpeedScale")], ret=RetF32(),
     cases=_move_scale_cases)
spec(name="PM_CmdScale", image=IMAGE, rust="game::pmove::cmd_scale",
     args=[Buffer(size=28, terminated=False, at="esi"), _ps("edi"), _dvar_float("player_spectateSpeedScale")],
     ret=RetF32(), cases=_cmd_scale_cases)


def _drop_timers_cases(rng):
    for _ in forever:
        yield [player_state(rng), move_local(rng)]


spec(name="PM_DropTimers", image=IMAGE, rust="game::pmove::drop_timers", args=[_ps("eax"), _pml("esi")],
     ret=RetVoid(), cases=_drop_timers_cases)


# --- angles ---------------------------------------------------------------------------------

def _angle(rng) -> float:
    k = rng.random()
    if k < 0.2:
        return fl(bits(rng.choice([0.0, -0.0, 90.0, 180.0, -180.0, 270.0, 360.0, 359.99, 180.00002, -90.0])))
    if k < 0.3:
        return fl(f32(rng, 1e5))
    return rng.uniform(-720.0, 720.0)


def _direction(rng) -> bytes:
    k = rng.random()
    if k < 0.15:
        x = y = 0
        return struct.pack("<3I", x, y, rng.choice([0, bits(1.0), bits(-1.0), bits(-0.0), f32(rng)]))
    if k < 0.25:
        return struct.pack("<3I", *(rng.choice([0, bits(-0.0), f32(rng)]) for _ in range(3)))
    return struct.pack("<3I", *(f32(rng, rng.choice([1.0, 400.0])) for _ in range(3)))


def _normalize_to_cases(rng):
    for _ in forever:
        yield [_direction(rng), bytes(rng.getrandbits(8) for _ in range(12))]


spec(name="Vec3NormalizeTo", image=IMAGE, rust="shared::math::vec3_normalize_to",
     args=[_vec("edi"), _vec("esi")], ret=RetF32(), cases=_normalize_to_cases)
spec(name="vectoyaw", image=IMAGE, rust="shared::math::vectoyaw", args=[_vec("ecx")], ret=RetF32(),
     cases=lambda rng: ([_direction(rng)] for _ in forever))
spec(name="VecToAngles", image=IMAGE, rust="shared::math::vec_to_angles", args=[_vec("esi"), _vec("edi")],
     ret=RetVoid(), cases=_normalize_to_cases)
spec(name="AngleNormalize180", image=IMAGE, rust="shared::math::angle_normalize180", args=[F32()], ret=RetF32(),
     cases=lambda rng: ([_angle(rng)] for _ in forever))


# --- movement direction ---------------------------------------------------------------------

@dataclass
class Pmove(Arg):
    """A `pmove_t`: its first word points at the player state. The value is
    (block, ps): the PM_BLOCK bytes after the pointer (iw_pc::game::pmove::pm
    has the offsets, from the start: the command at +4 ...) and the player
    state's bytes. Token: b: block then ps, read back the same way."""
    mutable = True

    def words(self, oracle, value):
        block, ps = value
        ps_ptr = oracle.put(bytes(ps), 16)
        return [oracle.put(struct.pack("<I", ps_ptr) + bytes(block), 16)]

    def token(self, value):
        block, ps = value
        return "b:" + (bytes(block) + bytes(ps)).hex()

    def read_back(self, oracle, pointer, value):
        block, ps = value
        ps_ptr = struct.unpack("<I", oracle.read(pointer, 4))[0]
        return oracle.read(pointer + 4, len(block)) + oracle.read(ps_ptr, len(ps))


PM_NUM_TOUCH, PM_TOUCH_ENTS, PM_XY_SPEED = 0x40, 0x44, 0xDC   # offsets in pmove_t


def pm_block(rng, cmd: bytes | None = None) -> bytes:
    """pmove_t's bytes after the player state pointer: random, a command, a
    touch list of 0 to 32 entities, a horizontal speed."""
    block = bytearray(rng.getrandbits(8) if rng.random() < 0.3 else 0 for _ in range(PM_BLOCK))
    block[0:28] = cmd if cmd is not None else usercmd(rng)
    count = rng.choice([0, 0, 1, 2, 5, 31, 32, rng.randint(0, 32)])
    put(block, PM_NUM_TOUCH - 4, count)
    for i in range(32):
        put(block, PM_TOUCH_ENTS - 4 + 4 * i, rng.choice([0, 1, 2, 3, 0x3FE, rng.randint(0, 1023)])
            if i < count or rng.random() < 0.3 else 0)
    put(block, PM_XY_SPEED - 4, f32(rng, 300.0))
    return bytes(block)


def _yaw_word(rng) -> int:
    k = rng.random()
    if k < 0.75:
        return bits(rng.uniform(-360.0, 360.0) if rng.random() < 0.8 else float(rng.randint(-180, 360)))
    return f32(rng, 1e4)


def _movement_dir_cases(rng):
    for _ in forever:
        ps = bytearray(player_state(rng, PS_FULL))
        flags = struct.unpack_from("<I", ps, PM_FLAGS)[0] & ~0x21
        k = rng.random()
        flags |= 1 if k < 0.3 else 0x20 if k < 0.5 else 0x21 if k < 0.55 else 0
        put(ps, PM_FLAGS, flags)
        put(ps, E_FLAGS, rng.choice([0, 0, 0, 0x100, 0x200, 0x300, rng.getrandbits(32)]))
        put(ps, PRONE_DIRECTION, _yaw_word(rng))
        put(ps, VIEW_ANGLES, _yaw_word(rng))
        put(ps, VIEW_ANGLES + 4, _yaw_word(rng))
        ladder = _direction(rng)
        ps[LADDER_VEC:LADDER_VEC + 12] = ladder
        put(ps, GROUND_ENTITY_NUM, rng.choice([0x3FF, 0x3FE, 0, 1, 1022, rng.getrandbits(32)]))
        origin = [rng.uniform(-4000.0, 4000.0) for _ in range(3)]
        for i in range(3):
            put(ps, ORIGIN + 4 * i, bits(origin[i]) if rng.random() < 0.95 else f32(rng, 1e5))
        pml = bytearray(move_local(rng))
        # The last move: usually a few units (5 frame times of 0.008 s are 0.04).
        scale = rng.choice([0.0, 0.01, 0.05, 0.5, 3.0, 20.0, 1e4])
        for i in range(3):
            d = rng.uniform(-scale, scale) if i < 2 or rng.random() < 0.4 else 0.0
            put(pml, PREVIOUS_ORIGIN + 4 * i, bits(origin[i] - d) if rng.random() < 0.95 else f32(rng))
        cmd = usercmd(rng)
        if rng.random() < 0.15:
            cmd = cmd[:0x18] + bytes([0, 0]) + cmd[0x1A:]
        yield [(pm_block(rng, cmd), bytes(ps)), bytes(pml)]


spec(name="PM_SetMovementDir", image=IMAGE, rust="game::pmove::set_movement_dir",
     args=[Pmove(at="eax"), _pml("ecx")], ret=RetVoid(), cases=_movement_dir_cases)


# --- clipping, touching, jumping, footsteps --------------------------------------------------

def _normal(rng) -> bytes:
    k = rng.random()
    if k < 0.1:
        return struct.pack("<3I", *(f32(rng, 1.0) for _ in range(3)))
    if k < 0.3:
        return struct.pack("<3f", *rng.choice([(0, 0, 1), (1, 0, 0), (0, -1, 0), (0, 0, -1), (0.7071068, 0, 0.7071068)]))
    return struct.pack("<3I", *unit3(rng))


def _clip_cases(rng):
    for _ in forever:
        v = struct.pack("<3I", *(f32(rng) for _ in range(3)))
        yield [v, _normal(rng), v if rng.random() < 0.5 else bytes(rng.getrandbits(8) for _ in range(12))]


def _touch_cases(rng):
    for _ in forever:
        block = pm_block(rng)
        listed = [struct.unpack_from("<i", block, PM_TOUCH_ENTS - 4 + 4 * i)[0] for i in range(32)]
        number = rng.choice([0x3FE, 0x3FF, 0, 1, rng.choice(listed), rng.randint(0, 1023), rng.getrandbits(32)])
        yield [(block, player_state(rng)), number]


def _jump_ps(rng) -> bytes:
    ps = bytearray(player_state(rng))
    z = rng.uniform(-500.0, 500.0)
    put(ps, ORIGIN + 8, bits(z) if rng.random() < 0.95 else f32(rng))
    put(ps, JUMP_ORIGIN_Z, bits(z - rng.uniform(-5.0, 45.0)) if rng.random() < 0.9 else f32(rng))
    put(ps, GRAVITY, rng.choice([800, 800, 0, 1, -800, rng.randint(0, 2000), rng.getrandbits(32)]))
    put(ps, VELOCITY + 8, f32(rng, 300.0))
    return bytes(ps)


def _near_z(rng, ps: bytes) -> bytes:
    """An origin a little below or above the player state's (a move's start)."""
    z = struct.unpack_from("<f", ps, ORIGIN + 8)[0]
    near = z - rng.uniform(-10.0, 30.0) if math.isfinite(z) else 0.0
    return struct.pack("<3I", f32(rng), f32(rng), bits(near) if rng.random() < 0.9 else f32(rng))


def _jump_dvars(rng) -> list:
    return [setting_float(rng, [39.0, 39.0, 0.0, 60.0]), setting_float(rng, [18.0, 18.0, 0.0, 30.0])]


def _step_height_cases(rng):
    for _ in forever:
        ps = _jump_ps(rng)
        yield [ps, _near_z(rng, ps), bytes(rng.getrandbits(8) for _ in range(4))] + _jump_dvars(rng)


def _clamp_velocity_cases(rng):
    for _ in forever:
        ps = _jump_ps(rng)
        yield [ps, _near_z(rng, ps), _jump_dvars(rng)[0]]


def _footsteps_cases(rng):
    for _ in forever:
        ps = bytearray(player_state(rng, PS_SIZE))
        put(ps, VIEW_HEIGHT_TARGET, rng.choice([60, 60, 40, 11, 0, rng.getrandbits(32)]))
        yield [(pm_block(rng), bytes(ps)), setting_float(rng, [0.0, 0.0, 100.0, 200.0])]


def _round_cases(rng):
    for _ in forever:
        k = rng.random()
        if k < 0.3:
            yield [rng.choice([0.5, 1.5, 2.5, -0.5, -1.5, 0.49999997, 2.5000002, 8388607.5, 2147483520.0,
                               2147483648.0, -2147483648.0, 1e-9, -1e-9, 4.656613e-10])]
        else:
            yield [fl(f32(rng, rng.choice([2.0, 1000.0, 1e7])))]


spec(name="PM_ClipVelocity", image=IMAGE, rust="game::pmove::clip_velocity",
     args=[_vec("ecx"), _vec("eax"), _vec("edx")], ret=RetVoid(), cases=_clip_cases)
spec(name="PM_AddTouchEnt", image=IMAGE, rust="game::pmove::add_touch_ent", args=[Pmove(at="eax"), Int(at="edi")],
     ret=RetVoid(), cases=_touch_cases)
spec(name="Jump_GetStepHeight", image=IMAGE, rust="game::pmove::jump_get_step_height",
     args=[_ps("edx"), _vec("ecx"), _vec("edi", 1), _dvar_float("jump_height"), _dvar_float("jump_stepSize")],
     ret=RetBool8(), cases=_step_height_cases)
spec(name="Jump_ClampVelocity", image=IMAGE, rust="game::pmove::jump_clamp_velocity",
     args=[_ps("ecx"), _vec("eax"), _dvar_float("jump_height")], ret=RetVoid(), cases=_clamp_velocity_cases)
spec(name="PM_ShouldMakeFootsteps", image=IMAGE, rust="game::pmove::should_make_footsteps",
     args=[Pmove(at="esi"), _dvar_float("player_footstepsThreshhold")], ret=RetBool8(), cases=_footsteps_cases)
spec(name="RoundFloatToInt", image=IMAGE, rust="shared::math::round_float_to_int", args=[F32()], ret=RetI32(),
     cases=_round_cases)


# --- traces ---------------------------------------------------------------------------------
# The game's two trace functions (pmove_t's handler picks one from the table at 0x005DB818)
# are stand-ins: each call is logged (its handler, the four vectors' bytes, passEntityNum,
# contentMask) and answered with the case's next canned result (TraceAnswers).

TRACE_SIZE = 0x24
TRACE_FUNCTIONS = {0x004DE690: 0, 0x004FD900: 1}   # handler 0 (the server's), 1 (the client game's)
PM_HANDLER, PM_TRACE_MASK = 0xE4, 0x3C
CONTENTS_BODY = 0x02000000
_traces: dict = {"answers": [], "next": 0}


def signed(word: int) -> int:
    return word - (1 << 32) if word & 0x80000000 else word


def default_trace() -> bytes:
    """What a trace answers once the case's answers run out: nothing hit."""
    t = bytearray(TRACE_SIZE)
    put(t, 0, bits(1.0))
    struct.pack_into("<H", t, 0x1C, 0x3FF)
    return bytes(t)


@dataclass
class TraceAnswers(Arg):
    """The traces' answers, in order (TRACE_SIZE bytes each); v: token, their bytes."""
    at: str = "none"

    def words(self, oracle, value):
        _traces["answers"] = [bytes(t) for t in value]
        _traces["next"] = 0
        return [0]

    def token(self, value):
        return "v:" + b"".join(bytes(t) for t in value).hex()


def _trace_effect(handler: int):
    def effect(oracle, results, start, mins, maxs, end, pass_entity, mask):
        k = _traces["next"]
        _traces["next"] = k + 1
        answers = _traces["answers"]
        oracle.write(results & 0xFFFFFFFF, answers[k] if k < len(answers) else default_trace())
        vectors = [oracle.read(p & 0xFFFFFFFF, 12) for p in (start, mins, maxs, end)]
        oracle.calls[-1] = ("trace", [handler, *vectors, pass_entity, mask])
    return effect


def trace_stand_ins() -> dict:
    return {address: StandIn("trace", tuple((i, "int") for i in range(7)), effect=_trace_effect(handler))
            for address, handler in TRACE_FUNCTIONS.items()}


def trace_result(rng) -> bytes:
    t = bytearray(rng.getrandbits(8) if rng.random() < 0.2 else 0 for _ in range(TRACE_SIZE))
    k = rng.random()
    put(t, 0, bits(1.0) if k < 0.35 else bits(0.0) if k < 0.45 else bits(rng.random()) if k < 0.95
        else f32(rng, 1.0))
    t[4:16] = _normal(rng)
    put(t, 0x10, rng.choice([0, 0, 0x2, 0x100, rng.getrandbits(32)]))
    contents = rng.choice([0, 1, CONTENTS_BODY, CONTENTS_BODY | 1, rng.getrandbits(32)])
    put(t, 0x14, contents)
    put(t, 0x18, 0)   # the material: a pointer
    struct.pack_into("<H", t, 0x1C, rng.choice([0x3FF, 0x3FE, 0, 1, 5, rng.randint(0, 1023)]))
    t[0x23] = rng.choice([0, 0, 0, 1, rng.getrandbits(8)])
    return bytes(t)


def trace_answers(rng, n: int) -> list[bytes]:
    return [trace_result(rng) for _ in range(rng.randint(0, n))]


def _box(rng) -> list[bytes]:
    """A move's start, mins, maxs and end."""
    start = [rng.uniform(-2000.0, 2000.0) for _ in range(3)]
    end = [c + rng.uniform(-30.0, 30.0) for c in start]
    mins, maxs = (-15.0, -15.0, 0.0), (15.0, 15.0, rng.choice([70.0, 50.0, 30.0]))
    return [struct.pack("<3f", *start), struct.pack("<3f", *mins), struct.pack("<3f", *maxs),
            struct.pack("<3f", *end)]


def pm_with_handler(rng) -> bytes:
    block = bytearray(pm_block(rng))
    block[PM_HANDLER - 4] = rng.choice([0, 0, 1])
    put(block, PM_TRACE_MASK - 4, rng.choice([0x02810011, 0x02010011, CONTENTS_BODY | 1, rng.getrandbits(32)]))
    return bytes(block)


def _player_trace_cases(rng):
    for _ in forever:
        mask = signed(rng.choice([0x02810011, 0x02010011, CONTENTS_BODY, 1, rng.getrandbits(32)]))
        yield ([(pm_with_handler(rng), player_state(rng)), bytes(rng.getrandbits(8) for _ in range(TRACE_SIZE))]
               + _box(rng) + [rng.choice([0, 1, 63, 0x3FF, rng.randint(0, 1023)]), mask, trace_answers(rng, 3),
                              None])


spec(name="PM_playerTrace", image=IMAGE, rust="game::pmove::player_trace",
     args=[Pmove(at="esi"), Buffer(size=TRACE_SIZE, terminated=False, at="ebx"), _vec("stack"), _vec("stack"),
           _vec("stack"), _vec("stack"), Int(), Int(), TraceAnswers(), CallLog()],
     ret=RetVoid(), stand_ins=trace_stand_ins(), cases=_player_trace_cases)


# --- slide move -----------------------------------------------------------------------------

PM_MINS, PM_MAXS = 0xC4, 0xD0
CLIENT_NUM = 0xCC
GROUND_PLANE, GROUND_NORMAL, IMPACT_SPEED = 0x30, 0x3C, 0x5C


def _plane_palette(rng) -> list[bytes]:
    """A few surfaces a case's traces hit (so that some are hit twice, or make creases)."""
    out = [struct.pack("<3f", 0.0, 0.0, 1.0), struct.pack("<3f", 1.0, 0.0, 0.0), struct.pack("<3f", 0.0, -1.0, 0.0)]
    out += [struct.pack("<3I", *unit3(rng)) for _ in range(3)]
    if rng.random() < 0.1:
        out.append(struct.pack("<3I", *(f32(rng, 1.0) for _ in range(3))))
    return out


def _slide_traces(rng) -> list[bytes]:
    palette = _plane_palette(rng)
    out = []
    for _ in range(rng.randint(0, 9)):
        t = bytearray(trace_result(rng))
        t[4:16] = rng.choice(palette)
        k = rng.random()
        put(t, 0, bits(1.0) if k < 0.2 else bits(0.0) if k < 0.3 else bits(rng.random()) if k < 0.97 else f32(rng, 1.0))
        t[0x22] = 1 if rng.random() < 0.04 else 0      # allsolid
        t[0x23] = 1 if rng.random() < 0.1 else 0       # startsolid
        out.append(bytes(t))
    return out


def _slide_ps(rng) -> bytes:
    ps = bytearray(player_state(rng))
    for i in range(3):
        put(ps, ORIGIN + 4 * i, bits(rng.uniform(-3000.0, 3000.0)) if rng.random() < 0.97 else f32(rng, 1e5))
        put(ps, VELOCITY + 4 * i, bits(rng.uniform(-400.0, 400.0)) if rng.random() < 0.9 else f32(rng))
    put(ps, GRAVITY, rng.choice([800, 800, 800, 0, rng.randint(-100, 2000)]))
    put(ps, PM_TIME, rng.choice([0, 0, 0, 0, 100, rng.getrandbits(32)]))
    put(ps, CLIENT_NUM, rng.choice([0, 1, 5, 63, rng.getrandbits(32)]))
    return bytes(ps)


def _slide_pml(rng) -> bytes:
    pml = bytearray(move_local(rng))
    put(pml, GROUND_PLANE, rng.choice([0, 1, 1, rng.getrandbits(32)]))
    ground = struct.pack("<3f", 0.0, 0.0, 1.0) if rng.random() < 0.5 else struct.pack("<3I", *unit3(rng))
    pml[GROUND_NORMAL:GROUND_NORMAL + 12] = ground
    put(pml, IMPACT_SPEED, rng.choice([0, 0, bits(50.0), f32(rng)]))
    return bytes(pml)


def _slide_pm(rng) -> bytes:
    block = bytearray(pm_with_handler(rng))
    height = rng.choice([70.0, 50.0, 30.0])
    block[PM_MINS - 4:PM_MINS + 8] = struct.pack("<3f", -15.0, -15.0, 0.0)
    block[PM_MAXS - 4:PM_MAXS + 8] = struct.pack("<3f", 15.0, 15.0, height)
    return bytes(block)


def _slide_cases(rng):
    for _ in forever:
        yield [(_slide_pm(rng), _slide_ps(rng)), _slide_pml(rng), rng.choice([0, 1, 1, rng.getrandbits(31)]),
               _slide_traces(rng), None]


def _permute_cases(rng):
    for _ in forever:
        n = rng.randint(1, 8)
        palette = _plane_palette(rng)
        planes = b"".join(rng.choice(palette) if rng.random() < 0.7 else struct.pack("<3I", *unit3(rng))
                          for _ in range(n)) + bytes(rng.getrandbits(8) for _ in range(12 * (8 - n)))
        v = struct.pack("<3I", *(f32(rng) for _ in range(3)))
        yield [planes, v, n, bytes(rng.getrandbits(8) for _ in range(32))]


spec(name="PM_PermuteRestrictiveClipPlanes", image=IMAGE, rust="game::slide::permute_restrictive_clip_planes",
     args=[Buffer(size=96, terminated=False, at="edx"), _vec("stack"), Int(), Buffer(size=32, terminated=False)],
     ret=RetF32(), cases=_permute_cases)
spec(name="PM_SlideMove", image=IMAGE, rust="game::slide::slide_move",
     args=[Pmove(), _pml("stack"), Int(), TraceAnswers(), CallLog()], ret=RetI32(),
     stand_ins=trace_stand_ins(), cases=_slide_cases)


# --- events and footsteps -------------------------------------------------------------------

EVENT_SEQUENCE = 0xA4


def _event_ps(rng) -> bytearray:
    ps = bytearray(player_state(rng))
    put(ps, EVENT_SEQUENCE, rng.choice([0, 1, 3, 4, 7, -1, rng.getrandbits(32)]))
    return ps


def _add_event_cases(rng):
    for _ in forever:
        yield [bytes(_event_ps(rng)), rng.choice([0, 1, 5, 0x18, 0x47, 0x100, 0x1FF, rng.getrandbits(32)])]


def _surface_flags(rng) -> int:
    return rng.choice([0, 0x2000, (rng.randint(0, 31) << 20), (rng.randint(1, 23) << 20) | 0x2000,
                       (rng.randint(1, 23) << 20) | rng.choice([0, 1, 0x10]), rng.getrandbits(32)])


def _footstep_ps(rng) -> bytearray:
    ps = _event_ps(rng)
    flags = struct.unpack_from("<I", ps, PM_FLAGS)[0] & ~0x121
    flags |= rng.choice([0, 0, 1, 0x100, 0x20, 0x21, 0x120])
    put(ps, PM_FLAGS, flags)
    put(ps, LEANF, rng.choice([0, 0, 0, bits(-0.0), bits(0.5), 0x7FC00000]))
    return ps


def _footstep_type_cases(rng):
    for _ in forever:
        pml = bytearray(move_local(rng))
        put(pml, GROUND_SURFACE_FLAGS, _surface_flags(rng))
        yield [bytes(_footstep_ps(rng)), bytes(pml)]


def _footstep_event_cases(rng):
    for _ in forever:
        ps = _footstep_ps(rng)
        put(ps, GROUND_ENTITY_NUM, rng.choice([0x3FF, 0x3FF, 0x3FE, 0, rng.randint(0, 1023)]))
        put(ps, CLIENT_NUM, rng.choice([0, 1, 63]))
        ps[LADDER_VEC:LADDER_VEC + 12] = _direction(rng)
        for i in range(3):
            put(ps, ORIGIN + 4 * i, bits(rng.uniform(-3000.0, 3000.0)) if rng.random() < 0.97 else f32(rng, 1e5))
        pml = bytearray(move_local(rng))
        put(pml, GROUND_SURFACE_FLAGS, _surface_flags(rng))
        block = bytearray(_slide_pm(rng))
        if rng.random() < 0.3:
            for at in (PM_MINS, PM_MAXS):
                block[at - 4:at + 8] = struct.pack("<3I", *(f32(rng, 40.0) for _ in range(3)))
        old = rng.choice([0, 63, 64, 127, 128, 191, 192, rng.randint(0, 255), rng.getrandbits(32)])
        new = rng.choice([old, old + 1, old + 64, old - 1, rng.randint(0, 255), rng.getrandbits(32)]) & 0xFFFFFFFF
        traces = [bytearray(trace_result(rng)) for _ in range(rng.randint(0, 2))]
        for t in traces:
            put(t, 0x10, _surface_flags(rng))
        yield [(bytes(block), bytes(ps)), bytes(pml), signed(old & 0xFFFFFFFF), signed(new),
               rng.choice([0, 1, 1, rng.getrandbits(32)]), [bytes(t) for t in traces], None]


spec(name="PM_AddEvent", image=IMAGE, rust="game::pmove::add_event", args=[_ps("eax"), Int(at="ecx")],
     ret=RetVoid(), cases=_add_event_cases)
spec(name="PM_FootstepType", image=IMAGE, rust="game::pmove::footstep_type", args=[_ps("edx"), _pml("eax")],
     ret=RetI32(), cases=_footstep_type_cases)
spec(name="PM_FootstepEvent", image=IMAGE, rust="game::pmove::footstep_event",
     args=[Pmove(), _pml("stack"), Int(at="ecx"), Int(at="edx"), Int(at="eax"), TraceAnswers(), CallLog()],
     ret=RetVoid(), stand_ins=trace_stand_ins(), cases=_footstep_event_cases)


# --- angles, lerp, the prone check ----------------------------------------------------------

def _moderate_angle(rng) -> float:
    """Angles AngleVectors gets: no infinities or huge values (the x87's fsincos gives no
    result beyond 2^63 radians)."""
    k = rng.random()
    if k < 0.15:
        return rng.choice([0.0, -0.0, 90.0, 180.0, -90.0, 270.0, 360.0, 45.0])
    if k < 0.2:
        return fl(f32(rng, 1e4))
    return rng.uniform(-720.0, 720.0)


def _finite_angle(rng) -> float:
    a = _moderate_angle(rng)
    return a if math.isfinite(a) and abs(a) < 1e6 else rng.uniform(-360.0, 360.0)


def _angle_vectors_cases(rng):
    for _ in forever:
        angles = struct.pack("<3f", *(_finite_angle(rng) for _ in range(3)))
        outs = [bytes(rng.getrandbits(8) for _ in range(12)) if rng.random() < 0.85 else None for _ in range(3)]
        yield [angles] + outs


def _lerp_cases(rng):
    for _ in forever:
        a = struct.pack("<3I", *(f32(rng) for _ in range(3)))
        b = struct.pack("<3I", *(f32(rng) for _ in range(3)))
        frac = rng.choice([0.0, 1.0, 0.5, rng.random(), fl(f32(rng, 1.0))])
        out = a if rng.random() < 0.2 else bytes(rng.getrandbits(8) for _ in range(12))
        yield [a, b, out, frac]


def _angle_subtract_cases(rng):
    for _ in forever:
        k = rng.random()
        if k < 0.1:
            yield [rng.choice([180.0, -180.0, 180.00002, 540.0, 0.0, -0.0]), rng.choice([0.0, 360.0, -180.0])]
        elif k < 0.13:
            yield [fl(0x7FC00000), rng.uniform(-10, 10)]
        else:
            r = rng.choice([360.0, 720.0, 1e4, 1e5])
            yield [rng.uniform(-r, r), rng.uniform(-r, r)]


spec(name="AngleVectors", image=IMAGE, rust="shared::math::angle_vectors",
     args=[_vec("edx"), Buffer(size=12, terminated=False, at="esi"), Buffer(size=12, terminated=False, at="edi"),
           Buffer(size=12, terminated=False, at="ebx")], ret=RetVoid(), cases=_angle_vectors_cases)
spec(name="Vec3Lerp", image=IMAGE, rust="shared::math::vec3_lerp",
     args=[_vec("eax"), _vec("edx"), _vec("ecx"), F32()], ret=RetVoid(), cases=_lerp_cases)
spec(name="vectopitch", image=IMAGE, rust="shared::math::vectopitch", args=[_vec("esi")], ret=RetF32(),
     cases=lambda rng: ([_direction(rng)] for _ in forever))
spec(name="AngleSubtract", image=IMAGE, rust="shared::math::angle_subtract", args=[F32(), F32()], ret=RetF32(),
     cases=_angle_subtract_cases)


def _prone_traces(rng) -> list[bytes]:
    """Up to nine answers: mostly hits on floors (normal z near 1) at varied fractions, some
    misses, walls and steep ground."""
    out = []
    for _ in range(rng.randint(0, 10)):
        t = bytearray(trace_result(rng))
        k = rng.random()
        if k < 0.5:
            n = (rng.uniform(-0.3, 0.3), rng.uniform(-0.3, 0.3), 1.0)
            m = math.sqrt(sum(c * c for c in n))
            t[4:16] = struct.pack("<3f", *(c / m for c in n))
        elif k < 0.6:
            t[4:16] = struct.pack("<3f", 0.0, 0.0, 0.69)
        put(t, 0, bits(1.0) if rng.random() < 0.35 else bits(rng.random()) if rng.random() < 0.95 else f32(rng, 1.0))
        t[0x22] = 1 if rng.random() < 0.05 else 0
        out.append(bytes(t))
    return out


def _floor(rng) -> bytes:
    n = (rng.uniform(-0.2, 0.2), rng.uniform(-0.2, 0.2), 1.0)
    m = math.sqrt(sum(c * c for c in n))
    return struct.pack("<3f", *(c / m for c in n))


def _prone_answer(rng, miss: float, near: float = 0.0) -> bytes:
    t = bytearray(trace_result(rng))
    t[4:16] = _floor(rng) if rng.random() < 0.9 else struct.pack("<3I", *unit3(rng))
    k = rng.random()
    put(t, 0, bits(1.0) if k < miss else bits(rng.uniform(0.97, 0.9999)) if k < miss + near
        else bits(rng.uniform(0.05, 0.99)))
    t[0x22] = 0
    return bytes(t)


def _prone_script(rng, already_prone: bool) -> list[bytes]:
    """Answers that let the check get far: a free room check, feet that mostly miss, a torso
    over a floor, the feet traced again, a floor under them, clear rays."""
    out = [] if already_prone else [_prone_answer(rng, 1.0)]
    out.append(_prone_answer(rng, 0.5, 0.3))                  # the feet
    if rng.random() < 0.3:
        out.append(_prone_answer(rng, 0.5, 0.3))              # the feet, 22 higher
    out.append(_prone_answer(rng, 0.05))                      # the torso
    for _ in range(rng.choice([0, 1, 2])):
        out.append(_prone_answer(rng, 0.6))                   # the feet along the body
    out.append(_prone_answer(rng, 0.05))                      # the ground under the feet
    out += [_prone_answer(rng, 0.85), _prone_answer(rng, 0.85)]   # the rays
    return out


def _prone_cases(rng):
    for _ in forever:
        origin = struct.pack("<3f", *(rng.uniform(-2000.0, 2000.0) for _ in range(3)))
        size = rng.choice([15.0, 15.0, 12.0, fl(f32(rng, 20.0))])
        height = rng.choice([30.0, 30.0, 50.0, fl(f32(rng, 70.0))])
        yaw = _finite_angle(rng)
        outs = [bytes(rng.getrandbits(8) for _ in range(4)) if rng.random() < 0.85 else None for _ in range(3)]
        normal = None if rng.random() < 0.3 else (struct.pack("<3f", 0.0, 0.0, 1.0) if rng.random() < 0.5
                                                   else struct.pack("<3I", *unit3(rng)))
        feet = rng.choice([66.0, 66.0, 50.0, 40.0, fl(f32(rng, 80.0))])
        already = rng.choice([0, 1, 1])
        answers = _prone_script(rng, already != 0) if rng.random() < 0.8 else _prone_traces(rng)
        yield ([rng.choice([0, 1, 63, 1022]), origin, size, height, yaw] + outs
               + [already, rng.choice([0, 1, 1, 1]), normal, rng.choice([0, 0, 1]),
                  rng.choice([0, 0, 1, rng.getrandbits(31)]), feet, answers, None])


spec(name="BG_CheckProne", image=IMAGE, rust="game::prone::check_prone",
     args=[Int(), _vec("stack"), F32(), F32(), F32(), Buffer(size=4, terminated=False),
           Buffer(size=4, terminated=False), Buffer(size=4, terminated=False), Int(), Int(),
           Buffer(size=12, terminated=False), Int(), Int(), F32(), TraceAnswers(), CallLog()],
     ret=RetI32(), stand_ins=trace_stand_ins(), cases=_prone_cases)


# --- prone position, step slide move ---------------------------------------------------------

PS_PRONE = 0x5C0     # the player state up to the prone angles (0x5A8 to 0x5B3)
PRONE_CHECK_SIZE, BOB_CYCLE = 0x578, 0x08


def _step_ps(rng) -> bytes:
    ps = bytearray(player_state(rng, PS_PRONE))
    base = _slide_ps(rng)
    ps[:len(base)] = base
    flags = struct.unpack_from("<I", ps, PM_FLAGS)[0] & ~0x80021
    flags |= rng.choice([0, 0, 1, 0x20, 0x80000, 0x80001, 0x80000])
    put(ps, PM_FLAGS, flags)
    put(ps, PM_TYPE, rng.choice([0, 0, 0, 1, 5, 6, 7]))
    put(ps, GROUND_ENTITY_NUM, rng.choice([0x3FF, 0x3FF, 0x3FE, 0, 5, 63, 64, rng.randint(0, 1023)]))
    put(ps, JUMP_ORIGIN_Z, bits(struct.unpack_from("<f", ps, ORIGIN + 8)[0] - rng.uniform(-5.0, 45.0)))
    put(ps, PRONE_CHECK_SIZE, bits(rng.choice([15.0, 15.0, 12.0])) if rng.random() < 0.95 else f32(rng, 20.0))
    put(ps, PRONE_DIRECTION, bits(_finite_angle(rng)))
    put(ps, BOB_CYCLE, rng.choice([0, 63, 64, 127, 200, rng.randint(0, 255)]))
    put(ps, VIEW_HEIGHT_TARGET, rng.choice([60, 60, 40, 11]))
    put(ps, EVENT_SEQUENCE, rng.randint(0, 9))
    return bytes(ps)


def _step_traces(rng) -> list[bytes]:
    palette = _plane_palette(rng)
    out = []
    for _ in range(rng.randint(0, 24)):
        t = bytearray(trace_result(rng))
        t[4:16] = rng.choice(palette) if rng.random() < 0.7 else _floor(rng)
        k = rng.random()
        put(t, 0, bits(1.0) if k < 0.35 else bits(0.0) if k < 0.4 else bits(rng.random()) if k < 0.98
            else f32(rng, 1.0))
        t[0x22] = 1 if rng.random() < 0.02 else 0
        t[0x23] = 1 if rng.random() < 0.05 else 0
        struct.pack_into("<H", t, 0x1C, rng.choice([0x3FF, 0x3FE, 0x3FE, 0, 5, 63, 64, rng.randint(0, 1023)]))
        out.append(bytes(t))
    return out


def _verify_prone_cases(rng):
    for _ in forever:
        block = _slide_pm(rng)
        ps = _step_ps(rng)
        start = struct.pack("<3f", *(rng.uniform(-3000.0, 3000.0) for _ in range(3)))
        vel = struct.pack("<3f", *(rng.uniform(-300.0, 300.0) for _ in range(3)))
        yield [(block, ps), start, vel, _prone_script(rng, True) if rng.random() < 0.7 else _prone_traces(rng), None]


def _step_slide_cases(rng):
    for _ in forever:
        yield [(_slide_pm(rng), _step_ps(rng)), _slide_pml(rng), rng.choice([0, 1, 1]),
               setting_float(rng, [39.0, 39.0, 60.0]), setting_float(rng, [18.0, 18.0, 30.0]),
               setting_float(rng, [0.0, 0.0, 100.0]), _step_traces(rng), None]


spec(name="PM_VerifyPronePosition", image=IMAGE, rust="game::prone::verify_prone_position",
     args=[Pmove(at="eax"), _vec("stack"), _vec("ebx"), TraceAnswers(), CallLog()], ret=RetI32(),
     stand_ins=trace_stand_ins(), cases=_verify_prone_cases)
spec(name="PM_StepSlideMove", image=IMAGE, rust="game::slide::step_slide_move",
     args=[Pmove(), _pml("stack"), Int(), _dvar_float("jump_height"), _dvar_float("jump_stepSize"),
           _dvar_float("player_footstepsThreshhold"), TraceAnswers(), CallLog()], ret=RetVoid(),
     stand_ins=trace_stand_ins(), cases=_step_slide_cases)


# --- the air move ----------------------------------------------------------------------------

MOVE_DVARS = ["stopspeed", "friction", "jump_slowdownEnable", "inertiaMax", "inertiaAngle", "inertiaDebug",
              "player_spectateSpeedScale", "jump_height", "jump_stepSize", "player_footstepsThreshhold"]


def move_dvar_args() -> list:
    """Every dvar a whole move reads, in MOVE_DVARS' order."""
    return [_dvar_byte(n) if n in ("jump_slowdownEnable", "inertiaDebug") else _dvar_float(n) for n in MOVE_DVARS]


def move_dvar_values(rng) -> list:
    return [setting_float(rng, [100.0, 0.0]), setting_float(rng, [5.5, 0.0, 8.0]), rng.choice([0, 1, 1]),
            setting_float(rng, [50.0, 0.0, 200.0]), setting_float(rng, [0.0, 0.5, -0.5]), 0,
            setting_float(rng, [1.0, 0.5]), setting_float(rng, [39.0, 39.0, 60.0]),
            setting_float(rng, [18.0, 18.0, 30.0]), setting_float(rng, [0.0, 0.0, 100.0])]


def _view_pml(rng) -> bytes:
    pml = bytearray(_slide_pml(rng))
    for at in (0x00, 0x0C):
        pml[at:at + 12] = struct.pack("<3I", *unit3(rng)) if rng.random() < 0.9 else struct.pack(
            "<3I", *(f32(rng, 1.0) for _ in range(3)))
    return bytes(pml)


def _air_move_cases(rng):
    for _ in forever:
        block = bytearray(_slide_pm(rng))
        block[0:28] = usercmd(rng)
        yield [(bytes(block), _step_ps(rng)), _view_pml(rng)] + move_dvar_values(rng) + [_step_traces(rng), None]


spec(name="PM_AirMove", image=IMAGE, rust="game::moves::air_move",
     args=[Pmove(), _pml("eax")] + move_dvar_args() + [TraceAnswers(), CallLog()], ret=RetVoid(),
     stand_ins=trace_stand_ins(), cases=_air_move_cases)
