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
         "player_footstepsThreshhold": 0x019439D4, "player_backSpeedScale": 0x01943A3C,
         "player_strafeSpeedScale": 0x01943A74, "player_dmgtimer_maxTime": 0x01943A58,
         "player_dmgtimer_minScale": 0x01943A08, "jump_spreadAdd": 0x0196FF88, "jump_ladderPushVel": 0x0196FF84,
         "mantle_enable": 0x0194390C, "mantle_view_yawcap": 0x0194391C, "player_view_pitch_up": 0x019439E4,
         "player_view_pitch_down": 0x019439D8, "bg_ladder_yawcap": 0x01943A7C, "bg_prone_yawcap": 0x01943A24,
         "bg_fallDamageMinHeight": 0x01943A6C, "bg_fallDamageMaxHeight": 0x01943A18,
         "bg_foliagesnd_minspeed": 0x019439E8, "bg_foliagesnd_maxspeed": 0x01943A84,
         "bg_foliagesnd_slowinterval": 0x01943A44, "bg_foliagesnd_fastinterval": 0x01943A0C,
         "bg_foliagesnd_resetinterval": 0x01943A38}
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
              "player_spectateSpeedScale", "jump_height", "jump_stepSize", "player_footstepsThreshhold",
              "player_backSpeedScale", "player_strafeSpeedScale", "player_dmgtimer_maxTime",
              "player_dmgtimer_minScale", "jump_spreadAdd", "jump_ladderPushVel", "player_view_pitch_up",
              "player_view_pitch_down", "bg_ladder_yawcap", "bg_prone_yawcap", "mantle_enable", "mantle_view_yawcap"]


def move_dvar_args() -> list:
    """Every dvar a whole move reads, in MOVE_DVARS' order."""
    return [_dvar_byte(n) if n in ("jump_slowdownEnable", "inertiaDebug", "mantle_enable") else _dvar_float(n)
            for n in MOVE_DVARS]


def move_dvar_values(rng) -> list:
    return [setting_float(rng, [100.0, 0.0]), setting_float(rng, [5.5, 0.0, 8.0]), rng.choice([0, 1, 1]),
            setting_float(rng, [50.0, 0.0, 200.0]), setting_float(rng, [0.0, 0.5, -0.5]), 0,
            setting_float(rng, [1.0, 0.5]), setting_float(rng, [39.0, 39.0, 60.0]),
            setting_float(rng, [18.0, 18.0, 30.0]), setting_float(rng, [0.0, 0.0, 100.0]),
            setting_float(rng, [0.7, 0.7, 1.0, 0.0]), setting_float(rng, [0.8, 0.8, 1.0, 0.0]),
            setting_float(rng, [750.0, 750.0, 0.0, 1000.0]), setting_float(rng, [0.0, 0.0, 0.5, 1.0]),
            setting_float(rng, [64.0, 64.0, 0.0, 300.0]), setting_float(rng, [128.0, 128.0, 0.0, 300.0])] + \
        _view_dvar_values(rng)


def _view_dvar_values(rng) -> list:
    """player_view_pitch_up, _down, bg_ladder_yawcap, bg_prone_yawcap, mantle_enable, mantle_view_yawcap
    (the caps at least 1: at 0 or below the mantle's cap loops for ever)."""
    return [bits(rng.choice([85.0, 85.0, 90.0, 45.0, 0.0])), bits(rng.choice([85.0, 85.0, 90.0, 60.0, 0.0])),
            bits(rng.choice([100.0, 100.0, 0.0, 45.0, 360.0])), bits(rng.choice([85.0, 85.0, 45.0, 10.0, 180.0])),
            rng.choice([0, 1, 1]), bits(rng.choice([60.0, 60.0, 30.0, 180.0, 1.0]))]


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


# --- walk move, jumps -----------------------------------------------------------------------

PS_JUMP = 0x5E0      # the player state up to the aim's spread (0x5D4)
DAMAGE_TIMER, WEAPON, AIM_SPREAD_SCALE, JUMP_TIME = 0x90, 0xD4, 0x5D4, 0x70
VIEW_HEIGHT_LERP_TIME, VIEW_HEIGHT_LERP_TARGET, VIEW_HEIGHT_LERP_DOWN = 0xFC, 0x100, 0x104
ALMOST_GROUND_PLANE = 0x34
CMD_BUTTONS, OLD_CMD_BUTTONS = 0x04, 0x20        # in the pm block (pmove_t + 8, + 0x24)
BUTTON_JUMP, BUTTON_SLOW = 0x400, 0x800

WEAPON_DEFS = 0x01728200          # the weapons' definitions (WeaponDef *, one each)
WEAPON_MOVE_SPEED_SCALE = 0x264   # WeaponDef.moveSpeedScale
WEAPONS = 8                       # the weapons a case's player may hold (0: none)


@dataclass
class WeaponScales(Arg):
    """The move-speed scales of weapons 0 to WEAPONS - 1 (their bits): the
    definitions' table gets pointers placed so that each definition's
    moveSpeedScale is that scale. Token: v:, the bits little-endian."""
    at: str = "none"

    def words(self, oracle, value):
        scales = oracle.put(struct.pack(f"<{WEAPONS}I", *value), 16)
        defs = [(scales + 4 * i - WEAPON_MOVE_SPEED_SCALE) & 0xFFFFFFFF for i in range(WEAPONS)]
        oracle.write(WEAPON_DEFS, struct.pack(f"<{WEAPONS}I", *defs))
        return [0]

    def token(self, value):
        return "v:" + struct.pack(f"<{WEAPONS}I", *value).hex()


def weapon_scales(rng) -> list[int]:
    return [0] + [rng.choice([bits(1.0), bits(1.0), bits(0.8), bits(0.0), bits(-1.0), f32(rng, 2.0)])
                  for _ in range(WEAPONS - 1)]


ANIM_SCRIPT_DATA = 0x019A1C7C     # globalScriptData (a pointer)
ANIM_SCRIPT_EVENTS = 0x38564      # its scriptEvents[event] (animScript_t each, numItems first)
ANIM_EVENTS = 32                  # the events a case's scripts cover
ANIM_DURATION = 0x48              # animations[0].duration (animation_t: 0x68 bytes from the start)
ANIM_SCRIPT_SIZE = 0x204          # animScript_t: numItems, 128 item pointers
ANIM_ITEM_COMMANDS = 0x70         # animScriptItem_t.numCommands, then the commands (16 bytes each)
ANIM_ET_JUMP, ANIM_ET_JUMPBK, ANIM_ET_LAND = 3, 4, 5
ANIM_ET_PRONE_TO_CROUCH, ANIM_ET_STAND_TO_CROUCH, ANIM_ET_CROUCH_TO_STAND, ANIM_ET_PRONE_TO_STAND = 12, 13, 14, 16
_anim = {"base": 0, "item": 0, "rand": 0}


@dataclass
class AnimAnswers(Arg):
    """The animation scripts a move may play: (the commands of the item the
    first valid one finds (-1: none found), what rand gives, {event: its
    script's items}, animation 0's duration). Only the items' counts and the
    duration are in the script data (allocated whole, zero); finding the
    item, rand and running a command are stand-ins (anim_stand_ins). Token:
    v:, little-endian words: commands, rand, the duration, how many events,
    then each event (ascending) and its items."""
    at: str = "none"

    def words(self, oracle, value):
        commands, rand, items, duration = value
        base = oracle.alloc(ANIM_SCRIPT_EVENTS + ANIM_EVENTS * ANIM_SCRIPT_SIZE, 16)
        for event, n in items.items():
            oracle.write(base + ANIM_SCRIPT_EVENTS + event * ANIM_SCRIPT_SIZE, struct.pack("<i", n))
        oracle.write(base + ANIM_DURATION, struct.pack("<i", duration))
        oracle.write(ANIM_SCRIPT_DATA, struct.pack("<I", base))
        item = 0
        if commands >= 0:
            item = oracle.put(bytes(ANIM_ITEM_COMMANDS) + struct.pack("<i", commands) + bytes(16 * max(commands, 1)), 16)
        _anim.update(base=base, item=item, rand=rand)
        return [0]

    def token(self, value):
        commands, rand, items, duration = value
        words = [commands, rand, duration, len(items)]
        for event in sorted(items):
            words += [event, items[event]]
        return "v:" + struct.pack(f"<{len(words)}i", *words).hex()


def _first_valid_item(oracle, client, script):
    offset = (script & 0xFFFFFFFF) - _anim["base"] - ANIM_SCRIPT_EVENTS
    event = offset // ANIM_SCRIPT_SIZE if offset % ANIM_SCRIPT_SIZE == 0 and 0 <= offset < ANIM_EVENTS * \
        ANIM_SCRIPT_SIZE else script
    oracle.calls[-1] = ("anim_first_valid_item", [client, signed(event)])


def _execute_command(oracle, command, ps, set_timer, is_continue, force):
    index = ((command & 0xFFFFFFFF) - (_anim["item"] + ANIM_ITEM_COMMANDS + 4)) // 16
    oracle.calls[-1] = ("anim_execute_command", [signed(index), set_timer, is_continue, force])


def anim_stand_ins() -> dict:
    """BG_FirstValidItem (script in eax, client in ecx), the C runtime's rand
    and BG_ExecuteCommand (the command in eax, then (ps, setTimer,
    isContinue, force) on the stack), as AnimAnswers says."""
    return {0x004F8ED0: StandIn("anim_first_valid_item", (("ecx", "int"), ("eax", "int")),
                                result=lambda client, script: _anim["item"], effect=_first_valid_item),
            0x0057BAAD: StandIn("rand", (), result=lambda: _anim["rand"]),
            0x004F9060: StandIn("anim_execute_command", (("eax", "int"), (0, "int"), (1, "int"), (2, "int"),
                                                         (3, "int")), effect=_execute_command)}


def anim_answers(rng) -> tuple:
    jump, back = rng.choice([0, 1, 1, 3, -1]), rng.choice([0, 1, 1, 2])
    commands, rand = rng.choice([-1, 0, 1, 1, 3, 4]), rng.choice([0, 1, 2, 7, 32767, rng.randint(0, 32767)])
    return commands, rand, {ANIM_ET_JUMP: jump, ANIM_ET_JUMPBK: back}, 0


def _stance_ps(rng, ps: bytearray, server_time: int) -> None:
    """A stance, and a view height change going (or not) around `server_time`."""
    put(ps, VIEW_HEIGHT_TARGET, rng.choice([60, 60, 40, 40, 11, 11, 0, rng.getrandbits(32)]))
    start = rng.choice([0, 0, server_time - rng.randint(-50, 600), server_time - rng.randint(0, 500),
                        rng.getrandbits(32)])
    put(ps, VIEW_HEIGHT_LERP_TIME, start)
    put(ps, VIEW_HEIGHT_LERP_TARGET, rng.choice([11, 40, 40, 60, 0, rng.getrandbits(32)]))
    put(ps, VIEW_HEIGHT_LERP_DOWN, rng.choice([0, 0, 1, rng.getrandbits(32)]))


def _walk_ps(rng) -> bytes:
    ps = bytearray(_step_ps(rng)) + bytes(PS_JUMP - PS_PRONE)
    put(ps, DAMAGE_TIMER, rng.choice([0, 0, 1, 100, 750, 2000, rng.randint(-50, 1000), rng.getrandbits(32)]))
    put(ps, WEAPON, rng.choice([0, 0, rng.randint(1, WEAPONS - 1)]))
    put(ps, AIM_SPREAD_SCALE, rng.choice([0, bits(0.0), bits(200.0), bits(254.0), f32(rng, 255.0)]))
    ps[LADDER_VEC:LADDER_VEC + 12] = struct.pack("<3I", *unit3(rng))
    return bytes(ps)


def _walk_block(rng, ps: bytearray | None = None) -> bytes:
    block = bytearray(_slide_pm(rng))
    cmd = bytearray(usercmd(rng))
    buttons = struct.unpack_from("<I", cmd, CMD_BUTTONS)[0] & ~(BUTTON_JUMP | BUTTON_SLOW)
    if rng.random() < 0.6:
        buttons |= BUTTON_JUMP
    if rng.random() < 0.3:
        buttons |= BUTTON_SLOW
    put(cmd, CMD_BUTTONS, buttons)
    block[0:28] = cmd
    old = struct.unpack_from("<I", block, OLD_CMD_BUTTONS)[0] & ~BUTTON_JUMP
    put(block, OLD_CMD_BUTTONS, old | (BUTTON_JUMP if rng.random() < 0.3 else 0))
    return bytes(block)


def _walk_case_pm(rng) -> tuple[bytes, bytes]:
    """A move whose jump timing and stance give each test of the walk's a chance."""
    block = bytearray(_walk_block(rng))
    ps = bytearray(_walk_ps(rng))
    now = struct.unpack_from("<i", block, 0)[0]
    if rng.random() < 0.8:
        now = rng.randint(1000, 1 << 30)
        put(block, 0, now)
    put(ps, JUMP_TIME, rng.choice([now - rng.randint(0, 1000), now - 500, now - 499, 0, rng.getrandbits(32)]))
    flags = struct.unpack_from("<I", ps, PM_FLAGS)[0]
    if rng.random() < 0.7:
        flags &= ~0x1004
    put(ps, PM_FLAGS, flags)
    _stance_ps(rng, ps, now)
    return bytes(block), bytes(ps)


def _stance_cases(rng):
    for _ in forever:
        ps = bytearray(player_state(rng, 0x110))
        put(ps, VIEW_HEIGHT_TARGET, rng.choice([60, 40, 11, 0, 41, rng.getrandbits(32)]))
        yield [bytes(ps)]


def _lerp_cases(rng):
    for _ in forever:
        block = bytearray(pm_block(rng))
        ps = bytearray(player_state(rng, 0x110))
        now = rng.choice([rng.randint(0, 1 << 30), rng.getrandbits(32)])
        put(block, 0, now)
        _stance_ps(rng, ps, now)
        heights = [-1, -1, 11, 40, 40, 60, 0, rng.getrandbits(32)]
        yield [(bytes(block), bytes(ps)), signed(rng.choice(heights)), signed(rng.choice(heights))]


def _damage_cases(rng):
    for _ in forever:
        yield [signed(rng.choice([0, 0, 1, 375, 750, 1500, -5, rng.randint(0, 2000), rng.getrandbits(32)])),
               setting_float(rng, [750.0, 750.0, 0.0, 1000.0, -750.0]), setting_float(rng, [0.0, 0.5, 1.0])]


def _cmd_scale_walk_cases(rng):
    for _ in forever:
        block, ps = _walk_case_pm(rng)
        cmd = block[0:28] if rng.random() < 0.8 else _walk_block(rng)[0:28]
        yield [(block, ps), cmd, setting_float(rng, [0.7, 0.7, 1.0, 0.0]), setting_float(rng, [0.8, 0.8, 1.0, 0.0]),
               weapon_scales(rng)]


def _slowdown_ps(rng) -> bytes:
    ps = bytearray(_jump_ps(rng))
    put(ps, PM_TIME, rng.choice([0, 0, 0, 1, 1200, 1699, 1700, 1800, 1801, rng.randint(-5, 2500),
                                 rng.getrandbits(32)]))
    return bytes(ps)


def _slowdown_cases(rng):
    for _ in forever:
        yield [_slowdown_ps(rng), rng.choice([0, 1, 1, rng.getrandbits(8)])]


def _jump_start_cases(rng):
    for _ in forever:
        block, ps = _walk_case_pm(rng)
        ps = bytearray(ps)
        ps[0:PS_SIZE] = _slowdown_ps(rng)
        put(ps, AIM_SPREAD_SCALE, rng.choice([0, bits(0.0), bits(200.0), bits(254.0), f32(rng, 255.0)]))
        yield [(block, bytes(ps)), _slide_pml(rng), fl(setting_float(rng, [39.0, 39.0, 0.0, 60.0])),
               rng.choice([0, 1, 1]), setting_float(rng, [64.0, 64.0, 0.0, 300.0])]


def _surface_event_cases(rng):
    for _ in forever:
        ps = bytearray(_event_ps(rng))
        flags = struct.unpack_from("<I", ps, PM_FLAGS)[0] & ~0x20
        put(ps, PM_FLAGS, flags | (0x20 if rng.random() < 0.3 else 0))
        pml = bytearray(move_local(rng))
        put(pml, GROUND_SURFACE_FLAGS, _surface_flags(rng))
        yield [bytes(ps), bytes(pml)]


def _ladder_cases(rng):
    for _ in forever:
        ps = bytearray(player_state(rng))
        ps[LADDER_VEC:LADDER_VEC + 12] = struct.pack("<3I", *unit3(rng))
        put(ps, PM_FLAGS, struct.unpack_from("<I", ps, PM_FLAGS)[0] | 0x20)
        yield [bytes(ps), _view_pml(rng), setting_float(rng, [128.0, 128.0, 0.0, 300.0])]


def _jump_check_cases(rng):
    for _ in forever:
        block, ps = _walk_case_pm(rng)
        pml = bytearray(_view_pml(rng))
        put(pml, GROUND_SURFACE_FLAGS, _surface_flags(rng))
        yield [(block, ps), bytes(pml), rng.choice([0, 1, 1]), setting_float(rng, [39.0, 39.0, 0.0, 60.0]),
               setting_float(rng, [64.0, 64.0, 0.0, 300.0]), setting_float(rng, [128.0, 128.0, 0.0, 300.0]),
               anim_answers(rng), None]


def _walk_move_cases(rng):
    for _ in forever:
        block, ps = _walk_case_pm(rng)
        pml = bytearray(_view_pml(rng))
        put(pml, GROUND_SURFACE_FLAGS, rng.choice([0, 0, 2, _surface_flags(rng)]))
        yield ([(block, ps), bytes(pml)] + move_dvar_values(rng) +
               [weapon_scales(rng), anim_answers(rng), _step_traces(rng), None])


spec(name="PM_GetEffectiveStance", image=IMAGE, rust="game::pmove::effective_stance", args=[_ps("eax")],
     ret=RetI32(), cases=_stance_cases)
spec(name="PM_GetViewHeightLerp", image=IMAGE, rust="game::pmove::view_height_lerp",
     args=[Pmove(at="edi"), Int(at="edx"), Int(at="ecx")], ret=RetF32(), cases=_lerp_cases)
spec(name="PM_DamageScale_Walk", image=IMAGE, rust="game::pmove::damage_scale_walk",
     args=[Int(), _dvar_float("player_dmgtimer_maxTime"), _dvar_float("player_dmgtimer_minScale")], ret=RetF32(),
     cases=_damage_cases)
spec(name="PM_CmdScale_Walk", image=IMAGE, rust="game::pmove::cmd_scale_walk",
     args=[Pmove(at="eax"), Buffer(size=28, terminated=False, at="ebx"), _dvar_float("player_backSpeedScale"),
           _dvar_float("player_strafeSpeedScale"), WeaponScales()], ret=RetF32(), cases=_cmd_scale_walk_cases)
spec(name="Jump_ApplySlowdown", image=IMAGE, rust="game::jump::jump_apply_slowdown",
     args=[_ps("ecx"), _dvar_byte("jump_slowdownEnable")], ret=RetVoid(), cases=_slowdown_cases)
spec(name="Jump_GetLandFactor", image=IMAGE, rust="game::jump::jump_get_land_factor",
     args=[_ps("stack"), _dvar_byte("jump_slowdownEnable")], ret=RetF32(), cases=_slowdown_cases)
spec(name="Jump_Start", image=IMAGE, rust="game::jump::jump_start",
     args=[Pmove(at="edi"), _pml("esi"), F32(), _dvar_byte("jump_slowdownEnable"), _dvar_float("jump_spreadAdd")],
     ret=RetVoid(), cases=_jump_start_cases)
spec(name="Jump_AddSurfaceEvent", image=IMAGE, rust="game::jump::jump_add_surface_event",
     args=[_ps("eax"), _pml("stack")], ret=RetVoid(), cases=_surface_event_cases)
spec(name="Jump_PushOffLadder", image=IMAGE, rust="game::jump::jump_push_off_ladder",
     args=[_ps("edi"), _pml("ebx"), _dvar_float("jump_ladderPushVel")], ret=RetVoid(), cases=_ladder_cases)
spec(name="Jump_Check", image=IMAGE, rust="game::jump::jump_check",
     args=[Pmove(), _pml("eax"), _dvar_byte("jump_slowdownEnable"), _dvar_float("jump_height"),
           _dvar_float("jump_spreadAdd"), _dvar_float("jump_ladderPushVel"), AnimAnswers(), CallLog()],
     ret=RetBool8(), stand_ins=anim_stand_ins(), cases=_jump_check_cases)
spec(name="PM_WalkMove", image=IMAGE, rust="game::moves::walk_move",
     args=[Pmove(), _pml("stack")] + move_dvar_args() + [WeaponScales(), AnimAnswers(), TraceAnswers(), CallLog()],
     ret=RetVoid(), stand_ins={**trace_stand_ins(), **anim_stand_ins()}, cases=_walk_move_cases)


# --- fly move, ladder move -------------------------------------------------------------------

BUTTON_DOWN, BUTTON_UP = 0x40, 0x80


def _fly_move_cases(rng):
    for _ in forever:
        block = bytearray(_slide_pm(rng))
        cmd = bytearray(usercmd(rng))
        buttons = struct.unpack_from("<I", cmd, CMD_BUTTONS)[0] & ~(BUTTON_DOWN | BUTTON_UP)
        buttons |= (BUTTON_DOWN if rng.random() < 0.4 else 0) | (BUTTON_UP if rng.random() < 0.4 else 0)
        put(cmd, CMD_BUTTONS, buttons)
        block[0:28] = cmd
        ps = bytearray(_step_ps(rng))
        put(ps, PM_TYPE, rng.choice([2, 2, 3, 3, 4, 4, 0]))
        yield [(bytes(block), bytes(ps)), _view_pml(rng)] + move_dvar_values(rng) + [_step_traces(rng), None]


def _ladder_pml(rng) -> bytes:
    pml = bytearray(_view_pml(rng))
    if rng.random() < 0.7:
        pitch = rng.uniform(-1.5, 1.5)
        yaw = rng.uniform(-math.pi, math.pi)
        pml[0:12] = struct.pack("<3f", math.cos(pitch) * math.cos(yaw), math.cos(pitch) * math.sin(yaw),
                                -math.sin(pitch))
        pml[12:24] = struct.pack("<3f", math.sin(yaw), -math.cos(yaw), 0.0)
    put(pml, WALKING, rng.choice([0, 0, 1, rng.getrandbits(32)]))
    return bytes(pml)


def _ladder_move_cases(rng):
    for _ in forever:
        block, ps = _walk_case_pm(rng)
        ps = bytearray(ps)
        flags = struct.unpack_from("<I", ps, PM_FLAGS)[0]
        put(ps, PM_FLAGS, flags | 0x20 if rng.random() < 0.8 else flags)
        if rng.random() < 0.8:
            yaw = rng.uniform(-math.pi, math.pi)
            ps[LADDER_VEC:LADDER_VEC + 12] = struct.pack("<3f", math.cos(yaw), math.sin(yaw), 0.0)
        put(ps, VIEW_ANGLES + 4, bits(_finite_angle(rng)) if rng.random() < 0.95 else f32(rng, 360.0))
        yield ([(block, bytes(ps)), _ladder_pml(rng)] + move_dvar_values(rng) +
               [anim_answers(rng), _step_traces(rng), None])


spec(name="PM_FlyMove", image=IMAGE, rust="game::moves::fly_move",
     args=[Pmove(), _pml("eax")] + move_dvar_args() + [TraceAnswers(), CallLog()], ret=RetVoid(),
     stand_ins=trace_stand_ins(), cases=_fly_move_cases)
spec(name="PM_LadderMove", image=IMAGE, rust="game::moves::ladder_move",
     args=[Pmove(), _pml("eax")] + move_dvar_args() + [AnimAnswers(), TraceAnswers(), CallLog()], ret=RetVoid(),
     stand_ins={**trace_stand_ins(), **anim_stand_ins()}, cases=_ladder_move_cases)


def _dead_move_cases(rng):
    for _ in forever:
        ps = bytearray(player_state(rng))
        for i in range(3):
            put(ps, VELOCITY + 4 * i, rng.choice([bits(rng.uniform(-30.0, 30.0)), f32(rng), 0]))
        yield [move_local(rng), bytes(ps)]


spec(name="PM_DeadMove", image=IMAGE, rust="game::moves::dead_move", args=[_pml("eax"), _ps("stack")],
     ret=RetVoid(), cases=_dead_move_cases)


# --- angle wraps, lean, pitch on a slope -----------------------------------------------------

def _wrap_angle(rng) -> float:
    """Angles the wrap loops end for (no infinities; a few thousand turns at most)."""
    k = rng.random()
    if k < 0.2:
        return rng.choice([0.0, -0.0, 360.0, -360.0, 180.0, -180.0, 359.99997, 720.0, 540.0, -540.0, 1e-40])
    if k < 0.23:
        return fl(rng.choice([0x7FC00000, 0xFFC00000]))
    if k < 0.3:
        return rng.uniform(-1e6, 1e6)
    return rng.uniform(-1500.0, 1500.0)


def _lean_cases(rng):
    for _ in forever:
        position = struct.pack("<3I", *(f32(rng, 3000.0) for _ in range(3)))
        # (No infinite or huge lean: its roll would reach fsincos beyond 2^63 radians, where the
        # x87 gives no result.)
        lean = rng.choice([0.0, -0.0, 1.0, -1.0, 0.5, -0.25, rng.uniform(-1, 1), rng.uniform(-50, 50),
                           fl(0x7FC00000), 1e-40])
        yield [position, _finite_angle(rng), lean, rng.choice([16.0, 0.0, rng.uniform(-30, 30)]),
               rng.choice([20.0, 0.0, rng.uniform(-40, 40)])]


def _pitch_on_normal_cases(rng):
    for _ in forever:
        yield [_finite_angle(rng), _normal(rng) if rng.random() < 0.9 else struct.pack("<3I", *unit3(rng))]


spec(name="AngleWrap360", image=IMAGE, rust="shared::math::angle_wrap360", args=[F32()], ret=RetF32(),
     cases=lambda rng: ([_wrap_angle(rng)] for _ in forever))
spec(name="AngleWrap180", image=IMAGE, rust="shared::math::angle_wrap180", args=[F32()], ret=RetF32(),
     cases=lambda rng: ([_wrap_angle(rng)] for _ in forever))
spec(name="AngleDelta", image=IMAGE, rust="shared::math::angle_delta", args=[F32(), F32()], ret=RetF32(),
     cases=lambda rng: ([_angle(rng), _angle(rng)] for _ in forever))
spec(name="AddLeanToPosition", image=IMAGE, rust="shared::math::add_lean_to_position",
     args=[_vec("edx"), F32(), F32(), F32(), F32()], ret=RetVoid(), cases=_lean_cases)
spec(name="PitchForYawOnNormal", image=IMAGE, rust="shared::math::pitch_for_yaw_on_normal",
     args=[F32(), _vec("edx")], ret=RetF32(), cases=_pitch_on_normal_cases)


# --- prone turns, leaning, the mantle's view cap ---------------------------------------------

DELTA_YAW, MANTLE_YAW, VIEW_HEIGHT_CURRENT = 0x58, 0x5BC, 0xF8
TRACE_FUNCTION_ADDRESSES = sorted(TRACE_FUNCTIONS)


def _prone_turn_cases(rng):
    for _ in forever:
        ps = bytearray(_step_ps(rng))
        put(ps, VIEW_ANGLES + 4, bits(_finite_angle(rng)))
        put(ps, PRONE_DIRECTION, bits(_finite_angle(rng)))
        new_yaw = _finite_angle(rng)
        yield [bytes(ps), new_yaw, rng.choice([0, 1]),
               _prone_script(rng, True) if rng.random() < 0.7 else _prone_traces(rng), None]


def _lean_traces(rng) -> list[bytes]:
    out = []
    for _ in range(rng.choice([0, 1, 1, 1])):
        t = bytearray(trace_result(rng))
        put(t, 0, rng.choice([bits(1.0), bits(0.0), bits(rng.random()), bits(rng.random()), f32(rng, 1.0)]))
        out.append(bytes(t))
    return out


def _update_lean_cases(rng):
    for _ in forever:
        ps = bytearray(player_state(rng))
        put(ps, PM_TYPE, rng.choice([0, 0, 1, 5, 6]))
        flags = struct.unpack_from("<I", ps, PM_FLAGS)[0]
        put(ps, PM_FLAGS, flags & ~0x8000 if rng.random() < 0.8 else flags | 0x8000)
        put(ps, GROUND_ENTITY_NUM, rng.choice([0x3FF, 0x3FE, 0x3FE, 0, 5]))
        put(ps, E_FLAGS, rng.choice([0, 0, 0, 0x100, 0x200, rng.getrandbits(32)]))
        put(ps, VIEW_HEIGHT_TARGET, rng.choice([60, 40, 11, 11]))
        put(ps, VIEW_HEIGHT_CURRENT, bits(rng.choice([60.0, 40.0, 11.0, rng.uniform(0, 70)])))
        put(ps, LEANF, rng.choice([0, bits(0.0), bits(0.25), bits(-0.5), bits(0.5), bits(rng.uniform(-0.6, 0.6)),
                                   f32(rng, 1.0)]))
        put(ps, VIEW_ANGLES + 4, bits(_finite_angle(rng)))
        for i in range(3):
            put(ps, ORIGIN + 4 * i, bits(rng.uniform(-3000.0, 3000.0)))
        cmd = bytearray(usercmd(rng))
        put(cmd, CMD_BUTTONS, rng.choice([0, 0x40, 0x80, 0xC0, rng.getrandbits(32)]))
        msec = rng.choice([1.0, 8.0, 16.0, 50.0, 66.0, 0.0, rng.uniform(0, 100)])
        yield [bytes(ps), msec, bytes(cmd), rng.choice(TRACE_FUNCTION_ADDRESSES), _lean_traces(rng), None]


def _cap_view_cases(rng):
    for _ in forever:
        ps = bytearray(player_state(rng, PS_PRONE))
        put(ps, MANTLE_YAW, bits(_finite_angle(rng)))
        put(ps, VIEW_ANGLES + 4, bits(_finite_angle(rng)))
        put(ps, DELTA_YAW, rng.getrandbits(32))
        # (No cap of 0 or below: where the angle is past it, the original loops for ever.)
        cap = bits(rng.choice([60.0, 60.0, 30.0, 180.0, 1.0, rng.uniform(0.5, 180.0)])) if rng.random() < 0.97 \
            else rng.choice([0x7FC00000, bits(math.inf)])
        yield [bytes(ps), rng.choice([0, 1, 1, 1]), cap]


spec(name="BG_CheckProneTurned", image=IMAGE, rust="game::prone::check_prone_turned",
     args=[Buffer(size=PS_PRONE, terminated=False, at="eax"), F32(), Int(), TraceAnswers(), CallLog()],
     ret=RetBool8(), stand_ins=trace_stand_ins(), cases=_prone_turn_cases)
spec(name="PM_UpdateLean", image=IMAGE, rust="game::view::update_lean",
     args=[_ps("esi"), F32(), Buffer(size=28, terminated=False, at="eax"), Int(), TraceAnswers(), CallLog()],
     ret=RetVoid(), stand_ins=trace_stand_ins(), cases=_update_lean_cases)
spec(name="Mantle_CapView", image=IMAGE, rust="game::mantle::mantle_cap_view",
     args=[Buffer(size=PS_PRONE, terminated=False, at="esi"), _dvar_byte("mantle_enable"),
           _dvar_float("mantle_view_yawcap")], ret=RetVoid(), cases=_cap_view_cases)


# --- the view's angles ----------------------------------------------------------------------

VIEW_DVARS = ["player_view_pitch_up", "player_view_pitch_down", "bg_ladder_yawcap", "bg_prone_yawcap",
              "mantle_enable", "mantle_view_yawcap"]
DEAD_YAW, VIEW_ANGLE_CLAMP_BASE, VIEW_ANGLE_CLAMP_RANGE, PRONE_PITCH_BASE = 0x130, 0x10C, 0x114, 0x58C
CMD_ANGLES = 0x0C


def _view_ps(rng) -> bytes:
    ps = bytearray(_step_ps(rng))
    put(ps, PM_TYPE, rng.choice([0, 0, 0, 0, 1, 2, 3, 4, 5, 6, 7]))
    flags = struct.unpack_from("<I", ps, PM_FLAGS)[0] & ~0x25
    flags |= rng.choice([0, 0, 1, 1, 1, 0x20, 0x4, 0x21])
    put(ps, PM_FLAGS, flags)
    put(ps, E_FLAGS, rng.choice([0, 0, 0, 0, 0x100, 0x200, rng.getrandbits(32) & ~0x300]))
    for i in range(3):
        put(ps, DELTA_YAW - 4 + 4 * i, rng.choice([0, rng.getrandbits(16), rng.getrandbits(32)]))
    for i in range(3):
        put(ps, VIEW_ANGLES + 4 * i, bits(_finite_angle(rng)))
    put(ps, DEAD_YAW, rng.choice([999, 999, 0, rng.getrandbits(32)]))
    for i in range(2):
        put(ps, VIEW_ANGLE_CLAMP_BASE + 4 * i, bits(_finite_angle(rng)))
        put(ps, VIEW_ANGLE_CLAMP_RANGE + 4 * i, bits(rng.choice([45.0, 90.0, 0.0, 180.0, rng.uniform(0, 180)])))
    put(ps, MANTLE_YAW, bits(_finite_angle(rng)))
    put(ps, PRONE_PITCH_BASE, bits(rng.choice([0.0, rng.uniform(-30, 30), _finite_angle(rng)])))
    ps[LADDER_VEC:LADDER_VEC + 12] = struct.pack("<3I", *unit3(rng))
    # The body's direction near the view's often (the prone turns are by degrees).
    view = struct.unpack_from("<f", ps, VIEW_ANGLES + 4)[0]
    put(ps, PRONE_DIRECTION, bits(rng.choice([view, view + rng.uniform(-10, 10), view + rng.uniform(-100, 100),
                                              _finite_angle(rng)])))
    return bytes(ps)


def _view_cmd(rng, ps: bytes) -> bytes:
    cmd = bytearray(usercmd(rng))
    for i in range(3):
        put(cmd, CMD_ANGLES + 4 * i, rng.choice([rng.getrandbits(16), rng.getrandbits(32), 0,
                                                 rng.randint(-20000, 20000)]))
    return bytes(cmd)


def _view_traces(rng) -> list[bytes]:
    return _prone_script(rng, True) if rng.random() < 0.6 else _step_traces(rng)


def _near_view(rng, ps: bytes, cmd: bytes) -> tuple[bytes, bytes]:
    """The command's yaw (with delta_angles) near the player state's view and body: the prone turns
    and the ease back happen within a few degrees."""
    ps, cmd = bytearray(ps), bytearray(cmd)
    yaw = rng.uniform(0.0, 360.0)
    put(ps, VIEW_ANGLES + 4, bits(yaw + rng.choice([0.0, rng.uniform(-1, 1), rng.uniform(-5, 5)])))
    put(ps, PRONE_DIRECTION, bits(yaw + rng.choice([0.0, rng.uniform(-3, 3), rng.uniform(-15, 15),
                                                    rng.uniform(-90, 90)])))
    delta = struct.unpack_from("<i", ps, DELTA_YAW)[0]
    want = int((yaw + rng.uniform(-2, 2)) * 65536 / 360) & 0xFFFF
    put(cmd, CMD_ANGLES + 4, (want - delta) & 0xFFFF)
    return bytes(ps), bytes(cmd)


def _view_angles_cases(rng):
    for _ in forever:
        ps = _view_ps(rng)
        cmd = _view_cmd(rng, ps)
        if rng.random() < 0.5:
            ps, cmd = _near_view(rng, ps, cmd)
        msec = rng.choice([8.0, 16.0, 50.0, 66.0, 1.0, rng.uniform(0, 100)])
        yield ([ps, msec, cmd, rng.choice([0, 1])] + _view_dvar_values(rng) + [_view_traces(rng), None])


spec(name="PM_UpdateViewAngles", image=IMAGE, rust="game::view::update_view_angles",
     args=[Buffer(size=PS_PRONE, terminated=False, at="eax"), F32(), Buffer(size=28, terminated=False), Int()] +
          [_dvar_byte(n) if n == "mantle_enable" else _dvar_float(n) for n in VIEW_DVARS] +
          [TraceAnswers(), CallLog()],
     ret=RetVoid(), stand_ins=trace_stand_ins(), cases=_view_angles_cases)


# --- the ground: landing, the way out of a solid, the ground trace ---------------------------

PML_FULL = 0x7C            # all of pml_t (iw_pc::game::pmove::MOVE_LOCAL_SIZE)
PREVIOUS_VELOCITY = 0x6C
GROUND_TRACE = 0x38
COM_PRINTF = 0x00431EE0
EVENT_SEQUENCE_AT = 0xA4


def _pml_full(at: str) -> Buffer:
    return Buffer(size=PML_FULL, terminated=False, at=at)


def print_stand_in() -> dict:
    """Com_Printf, logged with its format."""
    return {COM_PRINTF: StandIn("com_printf", ((0, "str"),))}


def land_answers(rng) -> tuple:
    """AnimAnswers with the landing's script."""
    commands, rand, items, duration = anim_answers(rng)
    items[ANIM_ET_LAND] = rng.choice([0, 1, 1, 2])
    return commands, rand, items, duration


def _fall_heights(rng) -> list[int]:
    """bg_fallDamageMinHeight, bg_fallDamageMaxHeight (sometimes the wrong way round)."""
    low = bits(256.0) if rng.random() < 0.7 else rng.choice([bits(0.0), bits(100.0), bits(480.0), bits(500.0),
                                                              f32(rng, 600.0)])
    high = bits(480.0) if rng.random() < 0.7 else rng.choice([bits(256.0), bits(300.0), bits(1.0), bits(1000.0),
                                                               f32(rng, 600.0)])
    return [low, high]


def _landing_ps(rng) -> bytearray:
    ps = bytearray(_event_ps(rng))
    put(ps, ORIGIN + 8, bits(rng.uniform(-3000.0, 3000.0)) if rng.random() < 0.97 else f32(rng, 1e5))
    put(ps, GRAVITY, rng.choice([800, 800, 800, 800, 0, 1, -800, rng.randint(-100, 2000), rng.getrandbits(32)]))
    put(ps, PM_TYPE, rng.choice([0, 0, 0, 1, 5, 6, 7]))
    put(ps, CLIENT_NUM, rng.choice([0, 1, 63]))
    for i in range(3):
        put(ps, VELOCITY + 4 * i, bits(rng.uniform(-400.0, 400.0)) if rng.random() < 0.9 else f32(rng))
    return ps


def _landing_pml(rng, origin_z: float) -> bytearray:
    """A fall onto `origin_z`: from a little above (or below), at a falling speed of up to 1000
    (sometimes anything)."""
    pml = bytearray(move_local(rng)) + bytes(PML_FULL - PML_SIZE)
    put(pml, GROUND_SURFACE_FLAGS, _surface_flags(rng) if rng.random() < 0.7 else
        rng.choice([1, 2, 3, (rng.randint(1, 23) << 20) | 1, (rng.randint(1, 23) << 20) | 2]))
    drop = rng.choice([0.0, rng.uniform(0.0, 5.0), rng.uniform(0.0, 40.0), rng.uniform(-5.0, 0.0), rng.uniform(0, 600)])
    put(pml, PREVIOUS_ORIGIN + 8, bits(origin_z + drop) if rng.random() < 0.95 else f32(rng, 3000.0))
    k = rng.random()
    speed = -rng.uniform(0.0, 1000.0) if k < 0.6 else -rng.uniform(0.0, 200.0) if k < 0.8 else \
        rng.uniform(0.0, 300.0) if k < 0.9 else fl(f32(rng, 2000.0))
    put(pml, PREVIOUS_VELOCITY + 8, bits(speed))
    return pml


def _crash_land_cases(rng):
    for _ in forever:
        ps = _landing_ps(rng)
        pml = _landing_pml(rng, struct.unpack_from("<f", ps, ORIGIN + 8)[0])
        yield [bytes(ps), bytes(pml)] + _fall_heights(rng) + [land_answers(rng), None]


def _landing_surface_cases(rng):
    for _ in forever:
        pml = bytearray(move_local(rng))
        put(pml, GROUND_SURFACE_FLAGS, _surface_flags(rng))
        yield [bytes(pml)]


def _predictable_event_cases(rng):
    for _ in forever:
        yield [rng.choice([0, 1, 0x5D, 0x74, 0x93, 0x100, 0x1FF, rng.getrandbits(32)]),
               rng.choice([0, 1, 24, 100, 0x1FF, rng.getrandbits(32)]), bytes(_event_ps(rng))]


def _ground_answer(rng, solid: float = 0.15) -> bytes:
    t = bytearray(trace_result(rng))
    k = rng.random()
    t[4:16] = _floor(rng) if k < 0.5 else struct.pack("<3f", 0.0, 0.0, 1.0) if k < 0.6 else \
        struct.pack("<3f", 0.6, 0.0, 0.8) if k < 0.7 else _normal(rng)
    k = rng.random()
    put(t, 0, bits(1.0) if k < 0.3 else bits(0.0) if k < 0.4 else bits(rng.random()) if k < 0.9 else
        rng.choice([bits(0.01), bits(0.015625), bits(0.02), f32(rng, 1.0)]))
    t[0x22] = 0
    t[0x23] = 1 if rng.random() < solid else 0
    struct.pack_into("<H", t, 0x1C, rng.choice([0x3FF, 0x3FE, 0x3FE, 0, 5, 63, rng.randint(0, 1023)]))
    return bytes(t)


def _all_solid_traces(rng) -> list[bytes]:
    """Answers for a way out of a solid: some steps that start solid, then (maybe) one that
    doesn't and the trace down. (A step's trace may be made twice, when it starts in a body:
    no way out at all takes more than 26.)"""
    out = []
    for _ in range(rng.choice([0, 1, 3, 25, 26, 60, 80, rng.randint(0, 26)])):
        t = bytearray(_ground_answer(rng))
        t[0x23] = 1
        out.append(bytes(t))
    for _ in range(rng.choice([0, 2, 2, 3])):
        out.append(_ground_answer(rng, solid=0.0))
    return out


def _ground_traces(rng) -> list[bytes]:
    k = rng.random()
    if k < 0.12:
        first = bytearray(_ground_answer(rng))
        first[0x22] = 1
        first[0x23] = 1
        return [bytes(first)] + _all_solid_traces(rng) + [_ground_answer(rng) for _ in range(rng.randint(0, 2))]
    return [_ground_answer(rng) for _ in range(rng.choice([0, 1, 1, 2, 2, 3, 4]))]


def _ground_ps(rng) -> bytearray:
    ps = bytearray(_step_ps(rng))
    landing = _landing_ps(rng)
    for at in (GRAVITY, PM_TYPE, EVENT_SEQUENCE_AT):
        ps[at:at + 4] = landing[at:at + 4]
    put(ps, E_FLAGS, rng.choice([0, 0, 0, 0x100, 0x200, rng.getrandbits(32)]))
    put(ps, GROUND_ENTITY_NUM, rng.choice([0x3FF, 0x3FF, 0x3FF, 0x3FE, 0, 5, rng.randint(0, 1023)]))
    if rng.random() < 0.3:
        put(ps, VELOCITY + 8, bits(rng.uniform(0.0, 300.0)))
    return ps


def _correct_all_solid_cases(rng):
    for _ in forever:
        ps = _ground_ps(rng)
        pml = _landing_pml(rng, struct.unpack_from("<f", ps, ORIGIN + 8)[0])
        yield [(_slide_pm(rng), bytes(ps)), bytes(pml), bytes(rng.getrandbits(8) for _ in range(TRACE_SIZE)),
               _all_solid_traces(rng), None]


def _ground_trace_missed_cases(rng):
    for _ in forever:
        ps = _ground_ps(rng)
        pml = _landing_pml(rng, struct.unpack_from("<f", ps, ORIGIN + 8)[0])
        yield [(_slide_pm(rng), bytes(ps)), bytes(pml), land_answers(rng),
               [_ground_answer(rng) for _ in range(rng.choice([0, 1, 1, 1]))], None]


def _ground_trace_cases(rng):
    for _ in forever:
        ps = _ground_ps(rng)
        pml = _landing_pml(rng, struct.unpack_from("<f", ps, ORIGIN + 8)[0])
        yield [(_slide_pm(rng), bytes(ps)), bytes(pml)] + _fall_heights(rng) + \
            [land_answers(rng), _ground_traces(rng), None]


FALL_DVARS = [_dvar_float("bg_fallDamageMinHeight"), _dvar_float("bg_fallDamageMaxHeight")]

spec(name="BG_AddPredictableEventToPlayerstate", image=IMAGE, rust="game::pmove::add_predictable_event",
     args=[Int(at="ecx"), Int(), _ps("eax")], ret=RetVoid(), cases=_predictable_event_cases)
spec(name="PM_MediumLandingForSurface", image=IMAGE, rust="game::ground::medium_landing_for_surface",
     args=[_pml("eax")], ret=RetI32(), cases=_landing_surface_cases)
spec(name="PM_HardLandingForSurface", image=IMAGE, rust="game::ground::hard_landing_for_surface",
     args=[_pml("eax")], ret=RetI32(), cases=_landing_surface_cases)
spec(name="PM_CrashLand", image=IMAGE, rust="game::ground::crash_land",
     args=[_ps("esi"), _pml_full("stack")] + FALL_DVARS + [AnimAnswers(), CallLog()], ret=RetVoid(),
     stand_ins={**anim_stand_ins(), **print_stand_in()}, cases=_crash_land_cases)
spec(name="PM_CorrectAllSolid", image=IMAGE, rust="game::ground::correct_all_solid",
     args=[Pmove(at="ecx"), _pml_full("stack"), Buffer(size=TRACE_SIZE, terminated=False, at="eax"), TraceAnswers(),
           CallLog()], ret=RetI32(), stand_ins=trace_stand_ins(), cases=_correct_all_solid_cases)
spec(name="PM_GroundTraceMissed", image=IMAGE, rust="game::ground::ground_trace_missed",
     args=[Pmove(at="eax"), _pml_full("ebx"), AnimAnswers(), TraceAnswers(), CallLog()], ret=RetVoid(),
     stand_ins={**trace_stand_ins(), **anim_stand_ins()}, cases=_ground_trace_missed_cases)
spec(name="PM_GroundTrace", image=IMAGE, rust="game::ground::ground_trace",
     args=[Pmove(), _pml_full("stack")] + FALL_DVARS + [AnimAnswers(), TraceAnswers(), CallLog()], ret=RetVoid(),
     stand_ins={**trace_stand_ins(), **anim_stand_ins(), **print_stand_in()}, cases=_ground_trace_cases)


# --- stances: the duck check, the view height's way --------------------------------------

VIEW_HEIGHT_LERP_POS_ADJ, TORSO_ANIM, TORSO_ANIM_DURATION = 0x108, 0x84, 0x8C
PS_MINS, PS_MAXS = 0x56C, 0x578
VIEW_LERP_TABLES = [0x005DB630, 0x005DB6A0, 0x005DB710, 0x005DB7B8]   # the game's own (the port has copies)
STEP_DVARS = ["jump_height", "jump_stepSize", "player_footstepsThreshhold"]


def _step_dvar_values(rng) -> list[int]:
    return [setting_float(rng, [39.0, 39.0, 60.0]), setting_float(rng, [18.0, 18.0, 30.0]),
            setting_float(rng, [0.0, 0.0, 100.0])]


def _activate_slowdown_cases(rng):
    for _ in forever:
        ps = bytearray(player_state(rng))
        put(ps, PM_TIME, rng.choice([0, 0, 1, 1800, rng.getrandbits(32)]))
        yield [bytes(ps)]


def _view_height_ps(rng, ps: bytearray, server_time: int) -> None:
    put(ps, VIEW_HEIGHT_TARGET, rng.choice([0, 8, 11, 11, 40, 40, 60, 60, rng.randint(-5, 80), rng.getrandbits(32)]))
    put(ps, VIEW_HEIGHT_CURRENT, rng.choice([0, bits(11.0), bits(40.0), bits(60.0), bits(8.0),
                                             bits(rng.uniform(0.0, 70.0)), bits(rng.uniform(0.0, 70.0)),
                                             f32(rng, 70.0)]))
    put(ps, VIEW_HEIGHT_LERP_TIME, rng.choice([0, 0, server_time - rng.randint(-50, 450),
                                               server_time - rng.randint(0, 300), rng.getrandbits(32)]))
    put(ps, VIEW_HEIGHT_LERP_TARGET, rng.choice([11, 40, 40, 60, rng.getrandbits(32)]))
    put(ps, VIEW_HEIGHT_LERP_DOWN, rng.choice([0, 1, 1, rng.getrandbits(32)]))
    put(ps, VIEW_HEIGHT_LERP_POS_ADJ, rng.choice([0, 0, 0, bits(0.04), bits(rng.uniform(-3.0, 3.0)), f32(rng, 10.0)]))


def _view_height_adjust_cases(rng):
    for _ in forever:
        block = bytearray(_slide_pm(rng))
        ps = bytearray(_step_ps(rng))
        put(ps, PM_TYPE, rng.choice([0, 0, 1, 4, 5, 6]))
        _view_height_ps(rng, ps, struct.unpack_from("<i", block, 0)[0])
        pml = bytearray(_view_pml(rng)) + bytes(PML_FULL - PML_SIZE)
        yield [(bytes(block), bytes(ps)), bytes(pml)] + _step_dvar_values(rng) + [_step_traces(rng), None]


def _table_lerp_cases(rng):
    for _ in forever:
        yield [rng.choice([0, 0, 1, 50, 99, 100, 100, rng.randint(0, 100), rng.randint(0, 100), rng.randint(-20, 130),
                           rng.getrandbits(32)]), rng.choice(VIEW_LERP_TABLES), bytes(4)]


def _duck_answers(rng) -> tuple:
    commands, rand, items, _ = anim_answers(rng)
    for event in (ANIM_ET_PRONE_TO_CROUCH, ANIM_ET_STAND_TO_CROUCH, ANIM_ET_CROUCH_TO_STAND, ANIM_ET_PRONE_TO_STAND):
        items[event] = rng.choice([0, 1, 1, 2])
    return commands, rand, items, rng.choice([0, 100, 500, rng.randint(0, 3000), rng.getrandbits(31)])


def _duck_traces(rng) -> list[bytes]:
    k = rng.random()
    if k < 0.3:
        out = _prone_script(rng, False)
    elif k < 0.5:
        out = []
    else:
        out = []
        for _ in range(rng.randint(0, 6)):
            t = bytearray(trace_result(rng))
            t[4:16] = _floor(rng) if rng.random() < 0.7 else _normal(rng)
            put(t, 0, rng.choice([bits(1.0), bits(0.0), bits(rng.random()), bits(rng.random())]))
            t[0x22] = 1 if rng.random() < 0.4 else 0
            t[0x23] = 1 if rng.random() < 0.15 else 0
            out.append(bytes(t))
    return out + (_step_traces(rng) if rng.random() < 0.3 else [])


def _duck_cases(rng):
    for _ in forever:
        block = bytearray(_slide_pm(rng))
        put(block, CMD_BUTTONS, rng.choice([0, 0, 0x100, 0x100, 0x200, 0x200, 0x300, 0x2100, 0x2200, 0x2000,
                                            rng.getrandbits(32)]))
        if rng.random() < 0.5:
            block[0x18:0x1A] = bytes(2)            # no forward or right move
        ps = bytearray(_step_ps(rng))
        put(ps, PM_TYPE, rng.choice([0, 0, 0, 0, 1, 4, 5, 6, 7]))
        flags = struct.unpack_from("<I", ps, PM_FLAGS)[0] & ~0x8023
        flags |= rng.choice([0, 0, 1, 1, 2, 2, 3, 0x20, 0x21, 0x8000, 0x8001])
        put(ps, PM_FLAGS, flags)
        put(ps, E_FLAGS, rng.choice([0, 0, 0, 0, 0x100, 0x200, 0x300, 0xC, rng.getrandbits(32)]))
        put(ps, GROUND_ENTITY_NUM, rng.choice([0x3FF, 0x3FE, 0x3FE, 0, 5]))
        ps[PS_MINS:PS_MINS + 24] = struct.pack("<6f", -15.0, -15.0, 0.0, 15.0, 15.0, 70.0) if rng.random() < 0.9 \
            else struct.pack("<6I", *(f32(rng, 70.0) for _ in range(6)))
        put(ps, VIEW_ANGLES, bits(rng.uniform(-90.0, 90.0)) if rng.random() < 0.95 else bits(_finite_angle(rng)))
        put(ps, VIEW_ANGLES + 4, bits(_finite_angle(rng)))
        put(ps, TORSO_ANIM, rng.choice([0, 0x200, 5, 0x205, rng.getrandbits(32)]))
        put(ps, PM_TIME, rng.choice([0, 0, 100, rng.getrandbits(32)]))
        _view_height_ps(rng, ps, struct.unpack_from("<i", block, 0)[0])
        if rng.random() < 0.5:
            put(ps, VIEW_HEIGHT_LERP_TIME, 0)
        pml = bytearray(_view_pml(rng)) + bytes(PML_FULL - PML_SIZE)
        yield [(bytes(block), bytes(ps)), bytes(pml)] + _step_dvar_values(rng) + \
            [_duck_answers(rng), _duck_traces(rng), None]


spec(name="Jump_ActivateSlowdown", image=IMAGE, rust="game::jump::jump_activate_slowdown", args=[_ps("eax")],
     ret=RetVoid(), cases=_activate_slowdown_cases)
spec(name="PM_ExitAimDownSight", image=IMAGE, rust="game::pmove::exit_aim_down_sight", args=[_ps("eax")],
     ret=RetVoid(), cases=lambda rng: ([bytes(_event_ps(rng))] for _ in forever))
spec(name="PM_GetViewHeightLerpTime", image=IMAGE, rust="game::duck::view_height_lerp_time",
     args=[Int(at="eax"), Int()], ret=RetI32(),
     cases=lambda rng: ([rng.choice([11, 40, 60, 8, 0, rng.getrandbits(32)]), rng.choice([0, 1, rng.getrandbits(32)])]
                        for _ in forever))
spec(name="PM_ViewHeightTableLerp", image=IMAGE, rust="game::duck::view_height_table_lerp",
     args=[Int(at="eax"), Int(at="edi"), Buffer(size=4, terminated=False, at="ebx")], ret=RetF32(),
     cases=_table_lerp_cases)
spec(name="PM_ViewHeightAdjust", image=IMAGE, rust="game::duck::view_height_adjust",
     args=[Pmove(), _pml_full("stack")] + [_dvar_float(n) for n in STEP_DVARS] + [TraceAnswers(), CallLog()],
     ret=RetVoid(), stand_ins=trace_stand_ins(), cases=_view_height_adjust_cases)
spec(name="PM_CheckDuck", image=IMAGE, rust="game::duck::check_duck",
     args=[Pmove(at="edi"), _pml_full("stack")] + [_dvar_float(n) for n in STEP_DVARS] +
          [AnimAnswers(), TraceAnswers(), CallLog()],
     ret=RetVoid(), stand_ins={**trace_stand_ins(), **anim_stand_ins()}, cases=_duck_cases)


# --- noclip and UFO moves, foliage sounds, holding the breath, SnapVector ---------------------

FOLIAGE_SOUND_TIME, WEAPON_STATE = 0x44, 0xD8
FOLIAGE_DVARS = ["bg_foliagesnd_minspeed", "bg_foliagesnd_maxspeed", "bg_foliagesnd_slowinterval",
                 "bg_foliagesnd_fastinterval", "bg_foliagesnd_resetinterval"]


def _snap_cases(rng):
    for _ in forever:
        yield [struct.pack("<3I", *(rng.choice([f32(rng), bits(rng.uniform(-3.0, 3.0)), bits(0.5), bits(-1.5),
                                                bits(2.5), f32(rng, 1e10)]) for _ in range(3)))]


def _fly_pml(rng) -> bytes:
    pml = bytearray(_view_pml(rng))
    pml[0x18:0x24] = struct.pack("<3I", *unit3(rng)) if rng.random() < 0.9 else struct.pack(
        "<3I", *(f32(rng, 1.0) for _ in range(3)))
    return bytes(pml)


def _flying_cases(rng):
    for _ in forever:
        block = bytearray(_slide_pm(rng))
        put(block, CMD_BUTTONS, rng.choice([0, 0x40, 0x80, 0xC0, rng.getrandbits(32)]))
        if rng.random() < 0.25:
            block[0x18:0x1A] = bytes(2)
        ps = bytearray(_step_ps(rng))
        put(ps, PM_TYPE, rng.choice([2, 2, 3, 3, 0, 4]))
        k = rng.random()
        if k < 0.3:
            size = 0.6 if k < 0.15 else 60.0     # stopping, or slower than stopspeed
            for i in range(3):
                put(ps, VELOCITY + 4 * i, bits(rng.uniform(-size, size)))
        yield [(bytes(block), bytes(ps)), _fly_pml(rng)] + move_dvar_values(rng)


def _foliage_dvar_values(rng) -> list[int]:
    low = setting_float(rng, [40.0, 40.0, 0.0, 100.0])
    high = setting_float(rng, [180.0, 180.0, 40.0, 300.0])
    return [low, high, rng.choice([1500, 1500, 0, 100, rng.randint(-100, 3000)]),
            rng.choice([500, 500, 0, 2000, rng.randint(-100, 3000)]),
            rng.choice([500, 500, 0, rng.randint(-100, 3000), signed(rng.getrandbits(32))])]


def _foliage_cases(rng):
    for _ in forever:
        block = bytearray(_slide_pm(rng))
        server_time = struct.unpack_from("<i", block, 0)[0]
        put(block, PM_XY_SPEED - 4, rng.choice([bits(0.0), bits(30.0), bits(40.0), bits(100.0), bits(180.0),
                                                bits(250.0), bits(rng.uniform(0.0, 300.0)), f32(rng)]))
        ps = bytearray(_step_ps(rng))
        put(ps, FOLIAGE_SOUND_TIME, rng.choice([0, 0, server_time - rng.randint(-100, 2500),
                                                server_time - rng.randint(0, 600), rng.getrandbits(32)]))
        put(ps, EVENT_SEQUENCE_AT, rng.randint(0, 9))
        traces = [_ground_answer(rng, solid=0.5) for _ in range(rng.choice([0, 1, 1, 1, 2]))]
        yield [(bytes(block), bytes(ps))] + _foliage_dvar_values(rng) + [traces, None]


def _ads_slowdown_cases(rng):
    for _ in forever:
        block = bytearray(_slide_pm(rng))
        put(block, CMD_BUTTONS, rng.choice([0, 0x1000, 0x1000, rng.getrandbits(32)]))
        ps = bytearray(_step_ps(rng))
        put(ps, PM_TYPE, rng.choice([0, 0, 1, 5, 6, 7]))
        flags = struct.unpack_from("<I", ps, PM_FLAGS)[0] & ~0x141
        put(ps, PM_FLAGS, flags | rng.choice([0, 0x40, 0x40, 0x41, 0x100, 0x140]))
        put(ps, WEAPON_STATE, rng.choice([0, 1, 4, 5, 6, 7, 8, 9, 10, rng.getrandbits(32)]))
        yield [(bytes(block), bytes(ps))]


def _dvar_int(name: str) -> Setting:
    return Setting(pointer=DVARS[name], offset=8)


spec(name="SnapVector", image=IMAGE, rust="shared::math::snap_vector", args=[_vec("eax")], ret=RetVoid(),
     cases=_snap_cases)
spec(name="PM_NoclipMove", image=IMAGE, rust="game::moves::noclip_move",
     args=[Pmove(at="ecx"), _pml("eax")] + move_dvar_args(), ret=RetVoid(), cases=_flying_cases)
spec(name="PM_UFOMove", image=IMAGE, rust="game::moves::ufo_move",
     args=[Pmove(at="eax"), _pml("ecx")] + move_dvar_args(), ret=RetVoid(), cases=_flying_cases)
spec(name="PM_FoliageSounds", image=IMAGE, rust="game::pmove::foliage_sounds",
     args=[Pmove(at="eax")] + [_dvar_float(n) for n in FOLIAGE_DVARS[:2]] + [_dvar_int(n) for n in FOLIAGE_DVARS[2:]] +
          [TraceAnswers(), CallLog()],
     ret=RetVoid(), stand_ins=trace_stand_ins(), cases=_foliage_cases)
spec(name="PM_UpdateAdsSlowdown", image=IMAGE, rust="game::pmove::update_ads_slowdown", args=[Pmove(at="ecx")],
     ret=RetVoid(), cases=_ads_slowdown_cases)


# --- the prone body's pitch, ladders --------------------------------------------------------

PRONE_DIRECTION_PITCH, PRONE_PITCH_BASE, PRONE_CHECK_SIZE_AT = 0x588, 0x58C, 0x578
SURF_LADDER = 0x8


def _prone_pitch_cases(rng):
    for _ in forever:
        block = bytearray(_slide_pm(rng))
        ps = bytearray(_step_ps(rng))
        flags = struct.unpack_from("<I", ps, PM_FLAGS)[0] & ~1
        put(ps, PM_FLAGS, flags | rng.choice([1, 1, 1, 0]))
        put(ps, GROUND_ENTITY_NUM, rng.choice([0x3FF, 0x3FF, 0x3FE, 0, 5]))
        for at in (PRONE_DIRECTION_PITCH, PRONE_PITCH_BASE):
            put(ps, at, rng.choice([0, bits(rng.uniform(-30.0, 30.0)), bits(rng.uniform(-179.0, 179.0)),
                                    bits(rng.choice([0.5, -0.5, 179.9, -179.9]))]))
        put(ps, VIEW_ANGLES + 4, bits(_finite_angle(rng)))
        pml = bytearray(_slide_pml(rng))
        put(pml, GROUND_PLANE, rng.choice([0, 1, 1]))
        pml[GROUND_NORMAL:GROUND_NORMAL + 12] = _floor(rng) if rng.random() < 0.6 else \
            struct.pack("<3f", 0.6, 0.0, 0.8) if rng.random() < 0.3 else _normal(rng)
        put(pml, FRAME_TIME, bits(rng.choice([0.008, 0.016, 0.05, 0.1, 1.0, rng.uniform(0.0, 0.3)])))
        traces = _prone_script(rng, True) if rng.random() < 0.7 else _prone_traces(rng)
        yield [(bytes(block), bytes(ps)), bytes(pml), traces, None]


def _check_ladder_traces(rng) -> list[bytes]:
    out = []
    for _ in range(rng.choice([0, 1, 2, 2, 2, 3])):
        t = bytearray(trace_result(rng))
        put(t, 0, rng.choice([bits(1.0), bits(0.5), bits(rng.random()), bits(rng.random()), bits(0.0)]))
        put(t, 0x10, rng.choice([SURF_LADDER, SURF_LADDER, SURF_LADDER, 0, rng.getrandbits(32)]))
        t[4:16] = struct.pack("<3I", *unit3(rng))
        out.append(bytes(t))
    return out


def _check_ladder_cases(rng):
    for _ in forever:
        block = bytearray(_slide_pm(rng))
        if rng.random() < 0.2:
            put(block, PM_MAXS + 8 - 4, bits(rng.choice([0.0, 4.0, 7.9, -10.0, 8.0, float("nan")])))
        server_time = struct.unpack_from("<i", block, 0)[0]
        ps = bytearray(_step_ps(rng))
        flags = struct.unpack_from("<I", ps, PM_FLAGS)[0] & ~0x40620
        flags |= rng.choice([0, 0, 0x20, 0x20, 0x40000, 0x40020, 0x200, 0x400])
        put(ps, PM_FLAGS, flags)
        put(ps, PM_TIME, rng.choice([0, 0, 0, 100, rng.getrandbits(32)]))
        put(ps, PM_TYPE, rng.choice([0, 0, 0, 1, 6]))
        put(ps, VIEW_HEIGHT_TARGET, rng.choice([60, 60, 40, 11]))
        put(ps, JUMP_TIME, rng.choice([0, server_time - rng.randint(0, 600), rng.getrandbits(32)]))
        ps[LADDER_VEC:LADDER_VEC + 12] = struct.pack("<3I", *unit3(rng))
        pml = bytearray(_view_pml(rng))
        put(pml, WALKING, rng.choice([0, 0, 1, 1]))
        yield [(bytes(block), bytes(ps)), bytes(pml), anim_answers(rng), _check_ladder_traces(rng), None]


spec(name="PM_UpdatePronePitch", image=IMAGE, rust="game::prone::update_prone_pitch",
     args=[Pmove(at="eax"), _pml("ebx"), TraceAnswers(), CallLog()], ret=RetVoid(), stand_ins=trace_stand_ins(),
     cases=_prone_pitch_cases)
spec(name="PM_LeaveLadder", image=IMAGE, rust="game::moves::leave_ladder", args=[_ps("eax")], ret=RetVoid(),
     cases=lambda rng: ([bytes(player_state(rng))] for _ in forever))
spec(name="PM_CheckLadderMove", image=IMAGE, rust="game::moves::check_ladder_move",
     args=[Pmove(), _pml("stack"), AnimAnswers(), TraceAnswers(), CallLog()], ret=RetVoid(),
     stand_ins={**trace_stand_ins(), **anim_stand_ins()}, cases=_check_ladder_cases)


# --- weapons: the sights, the aim's spread; the animations' conditions -----------------------

DVARS.update({"player_scopeExitOnDamage": 0x01943A30, "player_adsExitDelay": 0x019439F0,
              "bg_aimSpreadMoveSpeedThreshold": 0x019439F8})
WEAPON_DEF_SIZE = 0x604             # WeaponDef (what the loader allocates)
WEAPON_COUNT = 0x019A1C74           # the highest weapon number defined
DEF_WEAPONS = 4                     # the definitions a case gives (0 to 3: a case's weapons stay below)
DEF_PLAYER_ANIM_TYPE, DEF_WEAP_TYPE, DEF_WEAP_CLASS = 0x74, 0x78, 0x7C
DEF_OVERLAY_RETICLE, DEF_AIM_DOWN_SIGHT, DEF_RECHAMBER_WHILE_ADS, DEF_ADS_FIRE = 0x278, 0x32C, 0x330, 0x348
DEF_SEGMENTED_RELOAD, DEF_RELOAD_ADS_TIME, DEF_OO_POS_IN, DEF_OO_POS_OUT = 0x35C, 0x524, 0x588, 0x58C
DEF_HIP_SPREAD = [(0x2A4, [8.0, 5.0, 1.0, 0.0]), (0x2AC, [0.0, 0.5, 2.0, 10.0]), (0x2B0, [0.0, 2.0, 5.0]),
                  (0x2B4, [1.05, 1.0, 1.5]), (0x2B8, [1.1, 1.0, 2.0])]
BGS = 0x019A1C78                    # the animation state (a pointer)
CLIENT_ANIM_INFO, CLIENT_ANIM_INFO_SIZE = 0xB4BFC, 0x4B8   # clients' animation states in it
ANIM_CONDITIONS = 0x45C             # the conditions in a client's (two words each)
PS_WEAPON = 0x5E0                   # the player state up to the aim's spread
CLIENT_NUM_AT, OFFHAND_INDEX, WEAPON_STATE = 0xCC, 0xD0, 0xD8
WEAPON_TIME, WEAPON_DELAY, WEAPON_POS_FRAC, ADS_DELAY_TIME, DAMAGE_COUNT = 0x34, 0x38, 0xDC, 0xE0, 0x128
CMD_ANGLES, OLD_CMD_ANGLES = 0x0C, 0x28                   # in the pm block (pmove_t + 0x10, + 0x2C)
BUTTON_ATTACK, BUTTON_ADS = 0x1, 0x1000


@dataclass
class WeaponDefs(Arg):
    """The weapons' definitions 0 to DEF_WEAPONS - 1 (their bytes; the
    table at WEAPON_DEFS points at copies) and the highest weapon number
    (the word at WEAPON_COUNT). The value is (count, [definitions]). Token:
    v:, the count (a little-endian word), then the definitions."""
    at: str = "none"

    def words(self, oracle, value):
        count, defs = value
        pointers = [oracle.put(d, 16) for d in defs]
        oracle.write(WEAPON_DEFS, struct.pack(f"<{len(pointers)}I", *pointers))
        oracle.write(WEAPON_COUNT, struct.pack("<i", count))
        return [0]

    def token(self, value):
        count, defs = value
        return "v:" + (struct.pack("<i", count) + b"".join(defs)).hex()


@dataclass
class ClientAnim(Arg):
    """Client `client`'s animation state (CLIENT_ANIM_INFO_SIZE bytes): the
    animation state's pointer (BGS) is placed so that the client's lands on
    a copy, read back after the call. The value is (client, bytes). Token
    and answer: b:, the bytes."""
    at: str = "none"
    mutable = True

    def words(self, oracle, value):
        client, info = value
        address = oracle.put(bytes(info), 16)
        bgs = address - CLIENT_ANIM_INFO - client * CLIENT_ANIM_INFO_SIZE
        oracle.write(BGS, struct.pack("<I", bgs & 0xFFFFFFFF))
        return [address]

    def token(self, value):
        return "b:" + bytes(value[1]).hex()

    def read_back(self, oracle, pointer, value):
        return oracle.read(pointer, CLIENT_ANIM_INFO_SIZE)


def weapon_def(rng) -> bytes:
    d = bytearray(rng.randbytes(WEAPON_DEF_SIZE)) if rng.random() < 0.2 else bytearray(WEAPON_DEF_SIZE)
    put(d, DEF_PLAYER_ANIM_TYPE, rng.choice([0, 1, 5, 31, 32, 40, rng.randint(0, 63), rng.randint(-200, 500)]))
    put(d, DEF_WEAP_TYPE, rng.choice([0, 0, 1, 2, 3, 3, rng.getrandbits(32)]))
    put(d, DEF_WEAP_CLASS, rng.choice([0, 1, 2, 7, 33, rng.randint(0, 63), rng.randint(-200, 500)]))
    for at in (DEF_OVERLAY_RETICLE, DEF_AIM_DOWN_SIGHT, DEF_RECHAMBER_WHILE_ADS, DEF_ADS_FIRE, DEF_SEGMENTED_RELOAD):
        put(d, at, rng.choice([0, 0, 1, 1, 1, rng.getrandbits(32)]))
    put(d, DEF_RELOAD_ADS_TIME, rng.choice([0, 300, 1000, rng.randint(-500, 3000), rng.getrandbits(32)]))
    for at in (DEF_OO_POS_IN, DEF_OO_POS_OUT):
        put(d, at, rng.choice([bits(1 / 300), bits(1 / 150), bits(0.004), 0, f32(rng, 0.05), bits(rng.uniform(0, 0.1))]))
    for at, typical in DEF_HIP_SPREAD:
        put(d, at, setting_float(rng, typical))
    return bytes(d)


def weapon_defs(rng) -> tuple:
    return rng.choice([0, 1, 2, 3, 3, 3]), [weapon_def(rng) for _ in range(DEF_WEAPONS)]


def client_anim_info(rng) -> bytes:
    if rng.random() < 0.3:
        return rng.randbytes(CLIENT_ANIM_INFO_SIZE)
    info = bytearray(CLIENT_ANIM_INFO_SIZE)
    for i in range(18):
        put(info, ANIM_CONDITIONS + 4 * i, rng.choice([0, 0, 1, 2, 0x80000000, rng.getrandbits(32)]))
    return bytes(info)


def _weapon_ps(rng) -> bytearray:
    ps = bytearray(player_state(rng, PS_WEAPON))
    put(ps, PM_TYPE, rng.choice([0, 0, 0, 1, 1, 2, 5, 6, 7, 8, rng.getrandbits(32)]))
    flags = struct.unpack_from("<I", ps, PM_FLAGS)[0] & ~0x20051
    for bit in (0x1, 0x10, 0x40, 0x40, 0x20000):
        if rng.random() < 0.25:
            flags |= bit
    put(ps, PM_FLAGS, flags)
    put(ps, WEAPON, rng.randint(0, DEF_WEAPONS - 1))
    put(ps, OFFHAND_INDEX, rng.randint(0, DEF_WEAPONS - 1))
    put(ps, WEAPON_STATE, rng.choice(list(range(0x18)) + [rng.getrandbits(32)]))
    put(ps, WEAPON_TIME, rng.choice([0, 100, 300, 1000, rng.randint(-500, 3000), rng.getrandbits(32)]))
    put(ps, WEAPON_DELAY, rng.choice([0, 0, 50, rng.getrandbits(32)]))
    put(ps, WEAPON_POS_FRAC, rng.choice([0, bits(1.0), bits(1.0), bits(0.5), bits(rng.random()), bits(-0.0),
                                         f32(rng, 1.0)]))
    put(ps, DAMAGE_COUNT, rng.choice([0, 0, 1, 50]))
    put(ps, CLIENT_NUM_AT, rng.randint(0, 63))
    put(ps, E_FLAGS, rng.choice([0, 4, 8, 0x100, 0x200, 0x40000, 0x4000C, rng.getrandbits(32)]))
    put(ps, GROUND_ENTITY_NUM, rng.choice([0x3FF, 0x3FF, 0x3FE, 0, 3]))
    put(ps, VIEW_ANGLES, rng.choice([0, bits(10.0), bits(-10.0), bits(-0.0), f32(rng, 90.0)]))
    put(ps, AIM_SPREAD_SCALE, rng.choice([0, bits(255.0), bits(100.0), bits(254.9), f32(rng, 255.0)]))
    return ps


def _weapon_pm(rng, ps: bytearray) -> tuple:
    block = bytearray(pm_block(rng))
    put(block, CMD_BUTTONS, rng.choice([0, BUTTON_ADS, BUTTON_ATTACK, BUTTON_ADS | BUTTON_ATTACK, rng.getrandbits(32)]))
    put(block, OLD_CMD_BUTTONS, rng.choice([0, BUTTON_ADS, rng.getrandbits(32)]))
    for i in range(3):
        a = rng.choice([0, rng.getrandbits(16), rng.getrandbits(32)])
        put(block, CMD_ANGLES + 4 * i, a)
        put(block, OLD_CMD_ANGLES + 4 * i, a + rng.choice([0, 0, 1, -1, 100, -2000, 0x8000, rng.getrandbits(32)]))
    now = signed(struct.unpack_from("<I", block, 0)[0])
    put(ps, ADS_DELAY_TIME, rng.choice([0, 0, now + rng.randint(-200, 200), rng.getrandbits(32)]))
    return bytes(block), bytes(ps)


def _weapon_pml(rng) -> bytes:
    pml = bytearray(move_local(rng))
    put(pml, ALMOST_GROUND_PLANE, rng.choice([0, 0, 1]))
    return bytes(pml)


def _condition_cases(rng):
    for _ in forever:
        client, condition = rng.randint(0, 63), rng.randint(0, 8)
        value = rng.choice([0, 1, 2, 31, 32, 33, 63, 64, 100, 543, -1, -32, -8900, rng.randint(0, 63),
                            rng.randint(-8900, 100000)])
        yield [client, condition, value, rng.choice([0, 1, 1, 0x100, rng.getrandbits(32)]),
               (client, client_anim_info(rng))]


def _ps_conditions_cases(rng):
    for _ in forever:
        ps = _weapon_ps(rng)
        client = struct.unpack_from("<I", ps, CLIENT_NUM_AT)[0]
        yield [_weapon_pm(rng, ps), weapon_defs(rng), (client, client_anim_info(rng))]


def _ads_allowed_cases(rng):
    for _ in forever:
        yield [bytes(_weapon_ps(rng)), _weapon_pml(rng), weapon_defs(rng)]


def _ads_flag_cases(rng):
    for _ in forever:
        ps = _weapon_ps(rng)
        client = struct.unpack_from("<I", ps, CLIENT_NUM_AT)[0]
        yield [_weapon_pm(rng, ps), _weapon_pml(rng), weapon_defs(rng), (client, client_anim_info(rng))]


def _ads_lerp_cases(rng):
    for _ in forever:
        yield [_weapon_pm(rng, _weapon_ps(rng)), _weapon_pml(rng), rng.choice([0, 1, 1, 0x100]),
               rng.choice([0, 0, 100, 300, 1000, signed(rng.getrandbits(32))]), weapon_defs(rng)]


def _aim_spread_cases(rng):
    for _ in forever:
        yield [_weapon_pm(rng, _weapon_ps(rng)), _weapon_pml(rng), setting_float(rng, [11.0, 0.0, 300.0]),
               weapon_defs(rng)]


def _fclamp_cases(rng):
    for _ in forever:
        lo, hi = rng.choice([(0.0, 1.0), (0.0, 1.0), (-1.0, 1.0), (fl(f32(rng, 2.0)), fl(f32(rng, 2.0)))])
        yield [fl(rng.choice([bits(0.5), bits(-0.5), bits(1.5), bits(lo), bits(hi), f32(rng, 2.0)])), lo, hi]


spec(name="BG_UpdateConditionValue", image=IMAGE, rust="game::anim::update_condition_value",
     args=[Int(at="eax"), Int(at="edx"), Int(at="ecx"), Int(), ClientAnim()], ret=RetVoid(),
     cases=_condition_cases)
spec(name="BG_AnimUpdatePlayerStateConditions", image=IMAGE, rust="game::anim::update_player_state_conditions",
     args=[Pmove(), WeaponDefs(), ClientAnim()], ret=RetVoid(), cases=_ps_conditions_cases)
spec(name="BG_GetBinocularWeaponIndex", image=IMAGE, rust="game::weapon::binocular_weapon_index",
     args=[WeaponDefs()], ret=RetI32(), cases=lambda rng: ([weapon_defs(rng)] for _ in forever))
spec(name="PM_IsBinocularsUp", image=IMAGE, rust="game::weapon::binoculars_up", args=[Int(at="eax")],
     ret=RetI32(), cases=lambda rng: ([rng.choice(list(range(0x18)) + [signed(rng.getrandbits(32))])]
                                      for _ in forever))
spec(name="PM_IsAdsAllowed", image=IMAGE, rust="game::weapon::is_ads_allowed",
     args=[Buffer(size=PS_WEAPON, terminated=False, at="esi"), _pml("stack"), WeaponDefs()], ret=RetBool8(),
     cases=_ads_allowed_cases)
spec(name="PM_UpdateAimDownSightFlag", image=IMAGE, rust="game::weapon::update_aim_down_sight_flag",
     args=[Pmove(at="edi"), _pml("eax"), WeaponDefs(), ClientAnim()], ret=RetVoid(), cases=_ads_flag_cases)
spec(name="I_fclamp", image=IMAGE, rust="shared::math::i_fclamp", args=[F32(), F32(), F32()], ret=RetF32(),
     cases=_fclamp_cases)
spec(name="PM_UpdateAimDownSightLerp", image=IMAGE, rust="game::weapon::update_aim_down_sight_lerp",
     args=[Pmove(), _pml("stack"), _dvar_byte("player_scopeExitOnDamage"), _dvar_int("player_adsExitDelay"),
           WeaponDefs()], ret=RetVoid(), cases=_ads_lerp_cases)
spec(name="PM_UpdateAimSpreadScale", image=IMAGE, rust="game::weapon::update_aim_spread_scale",
     args=[Pmove(at="ebx"), _pml("stack"), _dvar_float("bg_aimSpreadMoveSpeedThreshold"), WeaponDefs()],
     ret=RetVoid(), cases=_aim_spread_cases)


# --- the weapon's states (PM_Weapon's parts) --------------------------------------------------

DVARS.update({"player_breath_hold_time": 0x01943A78, "player_breath_gasp_time": 0x01943A60,
              "player_breath_gasp_scale": 0x01943A00, "player_breath_hold_lerp": 0x01943A50,
              "player_breath_gasp_lerp": 0x01943A68})
BREATH_DVARS = ["player_breath_hold_time", "player_breath_gasp_time", "player_breath_gasp_scale",
                "player_breath_hold_lerp", "player_breath_gasp_lerp"]
DEF_OFFHAND_CLASS, DEF_AMMO_INDEX, DEF_CLIP_INDEX, DEF_COOK_OFF_HOLD, DEF_ALT_WEAPON = 0x84, 0x1C8, 0x1D0, 0x33C, 0x36C
DEF_TIMES = (0x200, 0x210, 0x214, 0x240, 0x244, 0x248, 0x2C0, 0x2C4)
GRENADE_TIME_LEFT, AMMO, AMMO_CLIP, WEAPONS_HELD, PRIMARY_WEAPONS = 0x3C, 0x144, 0x344, 0x544, 0x555
HOLD_BREATH_SCALE, HOLD_BREATH_TIMER, WEAP_ANIM = 0x5B4, 0x5B8, 0x5D0
CMD_WEAPON = 0x08                   # in the pm block (pmove_t + 0xC)
BUTTON_HOLD_BREATH = 0x8000


def state_weapon_def(rng) -> bytes:
    d = bytearray(weapon_def(rng))
    put(d, DEF_WEAP_TYPE, rng.choice([0, 1, 1, 2, 3, rng.getrandbits(32)]))
    put(d, DEF_WEAP_CLASS, rng.choice([0, 1, 9, 9, rng.randint(0, 12)]))
    put(d, DEF_OFFHAND_CLASS, rng.choice([0, 0, 1, 2, 3, rng.getrandbits(32)]))
    put(d, DEF_AMMO_INDEX, rng.randint(0, 127))
    put(d, DEF_CLIP_INDEX, rng.choice([0, 1, rng.randint(0, 127)]))
    put(d, DEF_COOK_OFF_HOLD, rng.choice([0, 1, 1, rng.getrandbits(32)]))
    put(d, DEF_ALT_WEAPON, rng.choice([0, 1, 2, 3, rng.getrandbits(32)]))
    for at in DEF_TIMES:
        put(d, at, rng.choice([0, 100, 250, 500, 1000, rng.randint(-100, 3000), rng.getrandbits(32)]))
    return bytes(d)


def state_weapon_defs(rng) -> tuple:
    return rng.choice([0, 1, 2, 3, 3, 3]), [state_weapon_def(rng) for _ in range(DEF_WEAPONS)]


def _state_ps(rng) -> bytearray:
    ps = _weapon_ps(rng)
    put(ps, WEAPON_STATE, rng.choice(list(range(0x18)) + [0x11, 0x13, 0x14, 0x16, signed(rng.getrandbits(32))]))
    if rng.random() < 0.3:
        put(ps, WEAPON, 0)
    put(ps, GRENADE_TIME_LEFT, rng.choice([0, 1, 16, 50, 1000, rng.randint(-100, 4000), rng.getrandbits(32)]))
    for i in range(128):
        if rng.random() < 0.5:
            put(ps, AMMO + 4 * i, rng.choice([0, 0, 1, 5, -1, rng.getrandbits(32)]))
        if rng.random() < 0.5:
            put(ps, AMMO_CLIP + 4 * i, rng.choice([0, 0, 1, 5, -1, rng.getrandbits(32)]))
    for i in range(4):
        put(ps, WEAPONS_HELD + 4 * i, rng.choice([0, 0xF, 0xE, 0x6, rng.getrandbits(32)]))
    ps[PRIMARY_WEAPONS] = rng.choice([0, 1, 2, 3, 0xFF, rng.getrandbits(8)])
    ps[PRIMARY_WEAPONS + 1] = rng.choice([0, 1, 2, 3, 0xFF, rng.getrandbits(8)])
    put(ps, HOLD_BREATH_SCALE, rng.choice([bits(1.0), bits(4.5), bits(0.0), f32(rng, 5.0)]))
    put(ps, HOLD_BREATH_TIMER, rng.choice([0, 0, 16, 1000, 4500, 4501, 5500, rng.randint(-100, 7000),
                                           rng.getrandbits(32)]))
    put(ps, WEAP_ANIM, rng.choice([0, 0x200, 0x11, 0x212, 0x13, rng.getrandbits(32)]))
    flags = struct.unpack_from("<I", ps, PM_FLAGS)[0]
    for bit in (0x800, 0x4000, 0x20000, 0x10):
        if rng.random() < 0.25:
            flags ^= bit
    put(ps, PM_FLAGS, flags)
    return ps


def _state_pm(rng) -> tuple:
    block, ps = _weapon_pm(rng, _state_ps(rng))
    block = bytearray(block)
    if rng.random() < 0.5:
        put(block, CMD_BUTTONS, struct.unpack_from("<I", block, CMD_BUTTONS)[0] ^ BUTTON_HOLD_BREATH)
    block[CMD_WEAPON] = rng.choice([0, 0, 1, 2, rng.getrandbits(8)])
    return bytes(block), ps


def _wps(at: str) -> Buffer:
    return Buffer(size=PS_WEAPON, terminated=False, at=at)


def _ps_cases(with_defs: bool = False, extra=None):
    def cases(rng):
        for _ in forever:
            values = [bytes(_state_ps(rng))]
            if extra:
                values += extra(rng)
            if with_defs:
                values.append(state_weapon_defs(rng))
            yield values
    return cases


def _delay(rng) -> list:
    return [rng.choice([0, 0, 100, 1000, signed(rng.getrandbits(32))])]


def _anim_number(rng) -> list:
    return [rng.choice([0, 0x11, 0x12, 0x13, 0x200, 0x211, rng.randint(0, 0x3FF), signed(rng.getrandbits(32))])]


def _diff_track_cases(rng):
    for _ in forever:
        tgt, cur = (fl(f32(rng, 5.0)) for _ in range(2))
        if rng.random() < 0.3:
            cur = tgt + rng.choice([0.0, 0.0005, -0.0005, 0.001, 0.002])
        yield [tgt, cur, fl(rng.choice([bits(4.0), bits(6.0), bits(0.0), f32(rng, 10.0)])),
               fl(rng.choice([bits(0.05), bits(0.016), bits(1.0), f32(rng, 1.0)]))]


def _hold_breath_cases(rng):
    for _ in forever:
        breath = [setting_float(rng, typical) for typical in ([4.5, 0.0, 0.0005, 1.0], [1.0, 0.0, 2.5], [4.5, 1.0],
                                                              [4.0, 0.0], [6.0, 0.0])]
        yield [_state_pm(rng), _weapon_pml(rng)] + breath + [state_weapon_defs(rng)]


def _cook_off_cases(rng):
    for _ in forever:
        yield [bytes(_state_ps(rng)), _weapon_pml(rng), state_weapon_defs(rng)]


def _fire_released_cases(rng):
    for _ in forever:
        yield [_state_pm(rng)] + _delay(rng)


def _offhand_class_cases(rng):
    for _ in forever:
        yield [bytes(_state_ps(rng)), rng.choice([0, 1, 2, 3, signed(rng.getrandbits(32))]), state_weapon_defs(rng)]


def _weapon_valid_cases(rng):
    for _ in forever:
        yield [bytes(_state_ps(rng)), rng.randint(0, DEF_WEAPONS - 1), state_weapon_defs(rng)]


def _drop_binoculars_cases(rng):
    for _ in forever:
        yield [_state_pm(rng), state_weapon_defs(rng)]


_STATE_PS_ONLY = [("PM_Weapon_FinishWeaponRaise", "finish_weapon_raise", "eax"),
                  ("PM_Weapon_FinishReloadEnd", "finish_reload_end", "eax"),
                  ("PM_Weapon_FinishRechamber", "finish_rechamber", "ecx"),
                  ("PM_Weapon_MeleeEnd", "melee_end", "ecx"),
                  ("PM_Weapon_OffHandEmpty", "offhand_empty", "eax"),
                  ("PM_ExitBinoculars", "exit_binoculars", "eax"),
                  ("PM_ResetWeaponState", "reset_weapon_state", "eax"),
                  ("PM_Weapon_Idle", "weapon_idle", "eax")]
_STATE_PS_DEFS = [("PM_Weapon_MeleeFire", "melee_fire", "eax"), ("PM_Weapon_OffHandInit", "offhand_init", "eax"),
                  ("PM_Weapon_OffHandPrepare", "offhand_prepare", "eax"),
                  ("PM_Weapon_OffHandHold", "offhand_hold", "eax"), ("PM_Weapon_OffHandEnd", "offhand_end", "eax"),
                  ("PM_Binoculars_DropWeapon", "binoculars_drop_weapon", "eax"),
                  ("PM_Binoculars_Raise", "binoculars_raise", "esi")]
for _name, _rust, _at in _STATE_PS_ONLY:
    spec(name=_name, image=IMAGE, rust=f"game::weapon_state::{_rust}", args=[_wps(_at)], ret=RetVoid(),
         cases=_ps_cases())
for _name, _rust, _at in _STATE_PS_DEFS:
    spec(name=_name, image=IMAGE, rust=f"game::weapon_state::{_rust}", args=[_wps(_at), WeaponDefs()],
         ret=RetVoid(), cases=_ps_cases(with_defs=True))
spec(name="PM_Binoculars_Lower", image=IMAGE, rust="game::weapon_state::binoculars_lower",
     args=[_wps("esi"), _dvar_int("player_adsExitDelay"), WeaponDefs()], ret=RetVoid(),
     cases=_ps_cases(with_defs=True, extra=_delay))
spec(name="PM_Binoculars_ExitSights", image=IMAGE, rust="game::weapon_state::binoculars_exit_sights",
     args=[_wps("eax"), _dvar_int("player_adsExitDelay"), WeaponDefs()], ret=RetVoid(),
     cases=_ps_cases(with_defs=True, extra=_delay))
spec(name="PM_Binoculars_DropBinoculars", image=IMAGE, rust="game::weapon_state::binoculars_drop",
     args=[Pmove(at="ecx"), WeaponDefs()], ret=RetVoid(), cases=_drop_binoculars_cases)
spec(name="PM_StartWeaponAnim", image=IMAGE, rust="game::weapon_state::start_weapon_anim",
     args=[_wps("eax"), Int(at="edx")], ret=RetVoid(), cases=_ps_cases(extra=_anim_number))
spec(name="PM_ContinueWeaponAnim", image=IMAGE, rust="game::weapon_state::continue_weapon_anim",
     args=[_wps("ecx"), Int(at="edx")], ret=RetVoid(), cases=_ps_cases(extra=_anim_number))
spec(name="PM_WeaponAmmoInClip", image=IMAGE, rust="game::weapon_state::ammo_in_clip",
     args=[_wps("eax"), WeaponDefs()], ret=RetI32(), cases=_ps_cases(with_defs=True))
spec(name="BG_GetFirstOffhandOfClass", image=IMAGE, rust="game::weapon_state::first_offhand_of_class",
     args=[_wps("edx"), Int(at="edi"), WeaponDefs()], ret=RetI32(), cases=_offhand_class_cases)
spec(name="BG_IsWeaponValid", image=IMAGE, rust="game::weapon_state::is_weapon_valid",
     args=[_wps("eax"), Int(at="edx"), WeaponDefs()], ret=RetBool8(), cases=_weapon_valid_cases)
spec(name="PM_Weapon_FireButtonReleased", image=IMAGE, rust="game::weapon_state::fire_button_released",
     args=[Pmove(at="eax"), Int()], ret=RetI32(), cases=_fire_released_cases)
spec(name="PM_Weapon_GrenadeCookOff", image=IMAGE, rust="game::weapon_state::grenade_cook_off",
     args=[_wps("eax"), _pml("stack"), WeaponDefs()], ret=RetBool8(), cases=_cook_off_cases)
spec(name="PM_Weapon_ResetUnlessBusy", image=IMAGE, rust="game::weapon_state::reset_unless_busy",
     args=[_wps("eax")], ret=RetI32(), cases=_ps_cases())
spec(name="DiffTrack", image=IMAGE, rust="shared::math::diff_track", args=[F32(), F32(), F32(), F32()],
     ret=RetF32(), cases=_diff_track_cases)
spec(name="PM_UpdateHoldBreath", image=IMAGE, rust="game::weapon::update_hold_breath",
     args=[Pmove(), _pml("stack")] + [_dvar_float(n) for n in BREATH_DVARS] + [WeaponDefs()], ret=RetVoid(),
     cases=_hold_breath_cases)
