"""Checks for cod3_game::net::fields (the multiplayer program's message
fields), the bit buffer readers they use, and cod3_game::math::yaw.

The field functions take the bit buffer as a `bdReference<bdBitBuffer>` by
value: a pointer to a cell holding the object's address, whose reference the
function drops at the end (0x820DB148). The checker gives the object a
reference count of 2, so dropping one never deletes it.

The readers share a status byte (a global, 0x82B793E6). The origin's lower
bound is a global too (0x82C9DD00), which the program sets to -8191 when it
starts; `setup` writes it before every call. Direction bytes index Quake's
162 normals, which the checker reads from the program (0x825FC698) and
passes to the Rust side as an extra argument the original ignores. (Those
three get no golden answers: their cases carry the table, the game's data.)
"""
from __future__ import annotations

import math
import random
import struct
from dataclasses import dataclass

from harness import (Buffer, Bytes, F32, FloatBuffer, Floats, GlobalU8, OutFloats, RetBool, RetF32, RetU32,
                     RetVoid, spec)
from specs.net_bit_buffer import BitBufferState, f32, random_state

READ_STATUS = 0x82B793E6
SIN_COS_LANES = 0x820B3920
COORD_MIN_GLOBAL = 0x82C9DD00
BYTE_DIRECTIONS = 0x825FC698
BYTE_DIRECTIONS_SIZE = 162 * 12


@dataclass
class BitBufferRef(BitBufferState):
    """A `bdReference<bdBitBuffer>` passed by value: a cell holding the buffer's address."""

    def place(self, oracle, value):
        obj = super().place(oracle, value)
        oracle.write(obj + 4, struct.pack(">I", 2))  # the reference count: the caller's and this one
        return oracle.put(struct.pack(">I", obj), 16)

    def read_back(self, oracle, pointer, value):
        obj = struct.unpack(">I", oracle.read(pointer, 4))[0]
        return super().read_back(oracle, obj, value)


@dataclass
class MaybeFloats(Floats):
    """Floats the function only reads, or a NULL pointer (None)."""

    def place(self, oracle, value):
        return 0 if value is None else super().place(oracle, value)

    def token(self, value):
        return "v:" if value is None else super().token(value)


def _coord_min(oracle, values):
    oracle.write(COORD_MIN_GLOBAL, struct.pack(">f", -8191.0))


def _sin_cos_stand_in(oracle, out, lanes, *_):
    """What the checks run instead of the game's VMX128 sine and cosine
    (0x820B3920, out in r3, in in r4), and the Rust side too
    (`decomp_check`'s SIN_COS_STAND_IN): lane i times [0.5, -0.25, 2, 1]."""
    a = struct.unpack(">4f", oracle.read(lanes, 16))
    oracle.write(out, struct.pack(">4f", a[0] * 0.5, a[1] * -0.25, a[2] * 2.0, a[3]))
    return out


def replace_sin_cos(oracle, values):
    oracle.replace(SIN_COS_LANES, _sin_cos_stand_in)


def _velocity_read(oracle, values):
    replace_sin_cos(oracle, values)


_table: bytes | None = None


def byte_directions() -> bytes:
    """The program's normals table (read once from the decrypted image)."""
    global _table
    if _table is None:
        import image
        _table = image.load("codmp").read(BYTE_DIRECTIONS, BYTE_DIRECTIONS_SIZE)
    return _table


def special(rng: random.Random) -> float:
    return rng.choice([0.0, -0.0, math.inf, -math.inf, math.nan, 1e-40, -1e-40, 3e38, -3e38, 1e10])


def angle(rng: random.Random) -> float:
    r = rng.random()
    if r < 0.7:
        return f32(rng.uniform(-720.0, 720.0))
    if r < 0.9:
        return rng.choice([0.0, 90.0, 180.0, -180.0, 360.0, 359.999, -0.001, 0.00549316, 179.99725])
    return special(rng)


def coordinate(rng: random.Random, limit: float = 9000.0) -> float:
    r = rng.random()
    if r < 0.7:
        return f32(rng.uniform(-limit, limit))
    if r < 0.9:
        return rng.choice([8191.0, -8191.0, 8191.5, -8191.5, 8190.5, 0.5, -0.5, 1.5, 511.5, -512.0, 512.0])
    return special(rng)


def speed(rng: random.Random) -> float:
    r = rng.random()
    if r < 0.6:
        return f32(rng.uniform(-1500.0, 1500.0))
    if r < 0.75:
        return f32(rng.uniform(-3.0, 3.0))
    if r < 0.9:
        return rng.choice([0.0, -0.0, 1024.0, -1024.0, 1024.5, 1023.9, 1e-7, 724.1])
    return special(rng)


def velocity(rng: random.Random) -> list[float]:
    r = rng.random()
    if r < 0.15:
        return [0.0, 0.0, speed(rng)]  # straight up or down: no direction byte
    if r < 0.25:
        return [rng.choice([0.0, -0.0]), speed(rng), speed(rng)]
    if r < 0.35:
        return [speed(rng), rng.choice([0.0, -0.0]), speed(rng)]
    return [speed(rng), speed(rng), speed(rng)]


def direction(rng: random.Random) -> list[float]:
    r = rng.random()
    if r < 0.6:  # a unit vector
        z = rng.uniform(-1, 1)
        a = rng.uniform(0, 2 * math.pi)
        s = math.sqrt(1 - z * z)
        return [f32(s * math.cos(a)), f32(s * math.sin(a)), f32(z)]
    if r < 0.8:
        return [coordinate(rng, 2.0) for _ in range(3)]
    return [rng.choice([0.0, 1.0, -1.0, 0.525731, special(rng)]) for _ in range(3)]


def status(rng: random.Random) -> int:
    return rng.choice([0, 1, 1, 1])


def _write(make):
    def cases(rng):
        while True:
            yield [random_state(rng), make(rng)]
    return cases


def _read3(rng):
    while True:
        yield [random_state(rng), [coordinate(rng) for _ in range(3)], status(rng)]


def _dir_write(rng):
    while True:
        yield [random_state(rng), direction(rng), byte_directions()]


def _dir_read(rng):
    while True:
        yield [random_state(rng), [coordinate(rng) for _ in range(3)], byte_directions()]


def _dir_to_byte(rng):
    while True:
        yield [None if rng.random() < 0.03 else direction(rng), byte_directions()]


def _coord_delta_write(rng):
    while True:
        base = coordinate(rng)
        r = rng.random()
        value = f32(base + rng.uniform(-600, 600)) if r < 0.7 else coordinate(rng)
        yield [random_state(rng), base, value]


def _coord_delta_read(rng):
    while True:
        yield [random_state(rng), coordinate(rng), [coordinate(rng)], status(rng)]


def _small_out(size):
    def cases(rng):
        while True:
            yield [random_state(rng), rng.randbytes(size)]
    return cases


def _yaw(rng):
    while True:
        r = rng.random()
        if r < 0.1:
            yield [[0.0, 0.0, speed(rng)]]
        elif r < 0.2:  # on an axis, and near the 45° seams of the series
            a, b = rng.choice([0.0, -0.0, 1.0, -1.0]), rng.choice([0.0, -0.0, 1.0, -1.0, 0.99999, 1.00001])
            yield [[a, b, 0.0] if rng.random() < 0.5 else [b, a, 0.0]]
        else:
            yield [velocity(rng)]


REF = BitBufferRef()
VEC3 = Floats(count=3)
OUT3 = FloatBuffer(count=3)
STATUS = GlobalU8(address=READ_STATUS)
TABLE = Bytes()
RUST = "net::fields::"
ON_CODMP = {"image": "codmp", "golden_cases": 40}  # golden: bits more than floats, 40 are plenty
BB = "net::bit_buffer::BitBuffer::"

spec(name="bdBitBuffer::readInt8", rust=BB + "read_i8", args=[BitBufferState(), Buffer(size=1, terminated=False)],
     ret=RetBool(), cases=_small_out(1), **ON_CODMP)
spec(name="bdBitBuffer::readInt16", rust=BB + "read_i16", args=[BitBufferState(), Buffer(size=2, terminated=False)],
     ret=RetBool(), cases=_small_out(2), **ON_CODMP)
spec(name="bdBitBuffer::readUInt16", rust=BB + "read_u16", args=[BitBufferState(), Buffer(size=2, terminated=False)],
     ret=RetBool(), cases=_small_out(2), **ON_CODMP)
spec(name="bdBitBuffer::readUInt64", rust=BB + "read_u64", args=[BitBufferState(), Buffer(size=8, terminated=False)],
     ret=RetBool(), cases=_small_out(8), **ON_CODMP)

spec(name="vectoyaw", rust="math::yaw::vector_to_yaw", args=[VEC3], ret=RetF32(), cases=_yaw, **ON_CODMP)
spec(name="vectoyaw_signed", rust="math::yaw::vector_to_yaw_signed", args=[VEC3], ret=RetF32(), cases=_yaw,
     **ON_CODMP)
spec(name="DirToByte", rust=RUST + "dir_to_byte", args=[MaybeFloats(count=3), TABLE], ret=RetU32(),
     cases=_dir_to_byte, golden=False, **ON_CODMP)

spec(name="MSGBB_WriteAngle", rust=RUST + "write_angle", args=[REF, F32()], ret=RetVoid(),
     cases=_write(angle), **ON_CODMP)
for name in ("MSGBB_WriteAngles2", "MSGBB_WriteAngles2_copy"):
    spec(name=name, rust=RUST + "write_angles2", args=[REF, VEC3], ret=RetVoid(),
         cases=_write(lambda rng: [angle(rng) for _ in range(3)]), **ON_CODMP)
for name in ("MSGBB_WriteAngles3", "MSGBB_WriteAngles3_copy"):
    spec(name=name, rust=RUST + "write_angles3", args=[REF, VEC3], ret=RetVoid(),
         cases=_write(lambda rng: [angle(rng) for _ in range(3)]), **ON_CODMP)
spec(name="MSGBB_WriteVector", rust=RUST + "write_vector", args=[REF, VEC3], ret=RetVoid(),
     cases=_write(lambda rng: [coordinate(rng) for _ in range(3)]), **ON_CODMP)
for name in ("MSGBB_WriteDir", "MSGBB_WriteDir_copy"):
    spec(name=name, rust=RUST + "write_dir", args=[REF, VEC3, TABLE], ret=RetVoid(), cases=_dir_write, golden=False,
         **ON_CODMP)
for name in ("MSGBB_WriteOrigin", "MSGBB_WriteOrigin_copy"):
    spec(name=name, rust=RUST + "write_origin", args=[REF, VEC3], ret=RetVoid(),
         cases=_write(lambda rng: [coordinate(rng) for _ in range(3)]), setup=_coord_min, **ON_CODMP)
spec(name="MSGBB_WriteOriginInt", rust=RUST + "write_origin_int", args=[REF, VEC3], ret=RetVoid(),
     cases=_write(lambda rng: [coordinate(rng) for _ in range(3)]), **ON_CODMP)
spec(name="MSGBB_WriteCoordDelta", rust=RUST + "write_coord_delta", args=[REF, F32(), F32()], ret=RetVoid(),
     cases=_coord_delta_write, **ON_CODMP)
spec(name="MSGBB_WriteVelocity", rust=RUST + "write_velocity", args=[REF, VEC3], ret=RetVoid(),
     cases=_write(velocity), **ON_CODMP)

spec(name="MSGBB_ReadAngles2", rust=RUST + "read_angles2", args=[REF, OUT3, STATUS], ret=RetBool(),
     cases=_read3, **ON_CODMP)
spec(name="MSGBB_ReadAngles3", rust=RUST + "read_angles3", args=[REF, OUT3, STATUS], ret=RetBool(),
     cases=_read3, **ON_CODMP)
spec(name="MSGBB_ReadVector", rust=RUST + "read_vector", args=[REF, OUT3, STATUS], ret=RetBool(),
     cases=_read3, **ON_CODMP)
spec(name="MSGBB_ReadOrigin", rust=RUST + "read_origin", args=[REF, OUT3, STATUS], ret=RetBool(),
     cases=_read3, setup=_coord_min, **ON_CODMP)
spec(name="MSGBB_ReadOriginInt", rust=RUST + "read_origin_int", args=[REF, OUT3, STATUS], ret=RetBool(),
     cases=_read3, **ON_CODMP)
spec(name="MSGBB_ReadDir", rust=RUST + "read_dir", args=[REF, OUT3, TABLE], ret=RetBool(), cases=_dir_read,
     golden=False, **ON_CODMP)
spec(name="MSGBB_ReadVelocity", rust=RUST + "read_velocity", args=[REF, OUT3, STATUS], ret=RetBool(),
     cases=_read3, setup=_velocity_read, **ON_CODMP)
spec(name="AngleWrap180", rust="math::angles::angle_wrap_180", args=[OutFloats(count=1), Floats(count=1)],
     ret=RetVoid(), cases=lambda rng: ([0, [rng.choice([angle(rng), coordinate(rng)])]] for _ in iter(int, 1)),
     **ON_CODMP)
spec(name="MSGBB_ReadCoordDelta", rust=RUST + "read_coord_delta", args=[REF, F32(), FloatBuffer(count=1), STATUS],
     ret=RetBool(), cases=_coord_delta_read, **ON_CODMP)
