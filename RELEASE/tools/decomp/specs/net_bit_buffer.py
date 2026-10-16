"""Checks for cod3_game::net::bit_buffer: Demonware's bit buffer (multiplayer program).

A buffer goes to both sides as its whole state: the type-checked and
read-failed flags, the write position, the highest bit written, the read
position (all in bits) and the bytes. The checker builds the game's object
from that state directly (no constructor call), so every case can start
anywhere: at an odd bit, near the end of what was written, type-checked or
not. After the call the state is read back and compared, bytes included.
"""
from __future__ import annotations

import math
import random
import struct
from dataclasses import dataclass

from harness import Arg, Buffer, Bytes, F32, Int, RetBool, RetU32, RetVoid, spec

# bdBitBuffer: +0 vtable, +4 reference count, +8 bdFastArray<unsigned char> (data,
# capacity, size), +0x14 write position, +0x18 highest bit written, +0x1C read
# position, +0x20 read failed, +0x21 type-checked.
VTABLE = 0x8202EAE8
OBJECT = struct.Struct(">8I")
# What both sides exchange: type-checked, read-failed, write position, written, read position; then the bytes.
STATE = struct.Struct(">BBIII")


@dataclass
class State:
    data: bytes
    write_position: int
    written: int
    read_position: int
    read_failed: bool = False
    type_checked: bool = False

    def encode(self) -> bytes:
        return STATE.pack(self.type_checked, self.read_failed, self.write_position, self.written,
                          self.read_position) + bytes(self.data)


@dataclass
class BitBufferState(Arg):
    """A bdBitBuffer the function works on (`this`); the value is a State."""
    mutable = True

    def place(self, oracle, value):
        capacity = len(value.data) + 16  # room to grow; the game's growth policy shows nowhere
        data = oracle.alloc(capacity, 16)
        oracle.write(data, bytes(value.data))
        obj = oracle.alloc(OBJECT.size + 4, 16)
        oracle.write(obj, OBJECT.pack(VTABLE, 0, data, capacity, len(value.data), value.write_position,
                                      value.written, value.read_position)
                     + bytes([value.read_failed, value.type_checked, 0, 0]))
        return obj

    def token(self, value):
        return "b:" + value.encode().hex()

    def read_back(self, oracle, pointer, value):
        _, _, data, _, size, write_position, written, read_position = OBJECT.unpack(oracle.read(pointer, OBJECT.size))
        failed, type_checked = oracle.read(pointer + OBJECT.size, 2)
        return STATE.pack(type_checked, failed, write_position, written, read_position) + oracle.read(data, size)


def random_state(rng: random.Random, type_checked: bool | None = None) -> State:
    """A buffer as writes leave it: the header bit, then up to a few hundred bits."""
    if type_checked is None:
        type_checked = rng.random() < 0.2
    written = rng.choice([1, 1, 7, 8, 9, 16, rng.randint(1, 64), rng.randint(1, 400)])
    data = bytearray(rng.randbytes((written + 7) // 8))
    if written % 8:
        data[-1] &= (1 << (written % 8)) - 1  # bits past the last one written are 0
    data[0] = (data[0] & 0xFE) | type_checked
    r = rng.random()
    read = 1 if r < 0.3 else written if r < 0.4 else rng.randint(1, written)
    return State(bytes(data), written, written, read, rng.random() < 0.05, type_checked)


def u32(rng: random.Random) -> int:
    return rng.choice([0, 1, 2, 31, 255, 256, 0x7FFFFFFF, 0x80000000, 0xFFFFFFFE, 0xFFFFFFFF,
                       rng.getrandbits(8), rng.getrandbits(16), rng.getrandbits(32)])


def i32(rng: random.Random) -> int:
    v = u32(rng)
    return v - (1 << 32) if v >= 1 << 31 else v


def unsigned_range(rng: random.Random) -> tuple[int, int]:
    lo = rng.choice([0, 0, 0, 1, rng.getrandbits(8), rng.getrandbits(31)])
    span = rng.choice([0, 1, 2, 3, 5, 25, 31, 255, 1000, 1023, 0x8000, rng.getrandbits(rng.randint(1, 32))])
    return lo, min(lo + span, 0xFFFFFFFF)


def signed_range(rng: random.Random) -> tuple[int, int]:
    lo = rng.choice([0, -1, -512, -1024, -8191, -(1 << 31), rng.randint(-(1 << 20), 1 << 20)])
    span = rng.choice([0, 1, 3, 255, 1024, 16382, rng.getrandbits(rng.randint(1, 31))])
    return lo, min(lo + span, (1 << 31) - 1)


def in_or_around(rng: random.Random, lo: int, hi: int, low_limit: int, high_limit: int) -> int:
    """A value in the range, at its ends, or just outside them (clamped)."""
    r = rng.random()
    if r < 0.6:
        return rng.randint(lo, hi)
    if r < 0.8:
        return rng.choice([lo, hi])
    return max(low_limit, min(high_limit, rng.choice([lo - 1, hi + 1, lo - 1000, hi + 1000])))


# The ranges and precisions the game uses for positions, speeds and angles, and others.
FLOAT_RANGES = [(-8191.0, 8191.0, 1.0), (-1024.0, 1024.0, 1.0), (-512.0, 512.0, 1.0), (0.0, 1.0, 0.01),
                (-1.0, 1.0, 1.0 / 255.0), (0.0, 360.0, 0.0054931640625), (0.0, 100.0, 0.1), (5.0, 5.0, 1.0)]


def f32(v: float) -> float:
    try:
        return struct.unpack(">f", struct.pack(">f", v))[0]
    except OverflowError:
        return math.copysign(math.inf, v)


def float_range(rng: random.Random) -> tuple[float, float, float]:
    r = rng.random()
    if r < 0.6:
        return rng.choice(FLOAT_RANGES)
    if r < 0.7:  # too many steps: the plain float goes out
        return (-1e9, 1e9, rng.choice([0.001, 0.1]))
    lo = f32(rng.uniform(-5000, 5000))
    hi = f32(lo + rng.choice([0.0, 0.5, 1.0, 100.0, rng.uniform(0, 20000)]))
    return lo, hi, f32(rng.choice([1.0, 0.5, 0.25, 0.1, 0.01, 3.0, rng.uniform(0.001, 10)]))


def float_value(rng: random.Random, lo: float, hi: float) -> float:
    r = rng.random()
    if r < 0.7:
        return f32(rng.uniform(lo, hi))
    if r < 0.85:
        return rng.choice([lo, hi])
    return f32(rng.choice([lo - 1.0, hi + 1.0, lo - 1e6, hi + 1e6, math.inf, -math.inf, math.nan, 0.0, -0.0]))


def _write_bits_cases(rng):
    while True:
        bits = rng.choice([0, 1, 7, 8, 9, 13, 16, 31, 32, 64, rng.randint(0, 80)])
        source = rng.randbytes(max(1, (bits + 7) // 8) + rng.choice([0, 0, 1]))
        yield [random_state(rng), source, bits]


def _read_bits_cases(rng):
    while True:
        state = random_state(rng)
        left = state.written - state.read_position
        bits = rng.choice([0, 1, 8, 13, 32, left, left + 1, rng.randint(0, max(1, left)), rng.randint(0, 80)])
        yield [state, rng.randbytes(max(1, (bits + 7) // 8)), bits]


def _ranged_u32_write_cases(rng):
    while True:
        lo, hi = unsigned_range(rng)
        yield [random_state(rng), in_or_around(rng, lo, hi, 0, 0xFFFFFFFF), lo, hi, rng.choice([0, 1])]


def _ranged_i32_write_cases(rng):
    while True:
        lo, hi = signed_range(rng)
        yield [random_state(rng), in_or_around(rng, lo, hi, -(1 << 31), (1 << 31) - 1), lo, hi]


def _ranged_f32_write_cases(rng):
    while True:
        lo, hi, precision = float_range(rng)
        yield [random_state(rng), float_value(rng, lo, hi), lo, hi, precision]


def _ranged_u32_read_cases(rng):
    while True:
        lo, hi = unsigned_range(rng)
        yield [random_state(rng), rng.randbytes(4), lo, hi, rng.choice([0, 1])]


def _ranged_i32_read_cases(rng):
    while True:
        lo, hi = signed_range(rng)
        yield [random_state(rng), rng.randbytes(4), lo, hi]


def _ranged_f32_read_cases(rng):
    while True:
        lo, hi, precision = float_range(rng)
        yield [random_state(rng), rng.randbytes(4), lo, hi, precision]


def _value_cases(make):
    def cases(rng):
        while True:
            yield [random_state(rng), make(rng)]
    return cases


def _out_cases(size):
    def cases(rng):
        while True:
            yield [random_state(rng), rng.randbytes(size)]
    return cases


THIS = BitBufferState()
OUT1, OUT4 = Buffer(size=1, terminated=False), Buffer(size=4, terminated=False)
RUST = "net::bit_buffer::"
ON_CODMP = {"image": "codmp"}

spec(name="bdBitOperations::highBitNumber", rust=RUST + "high_bit_number", args=[Int()], ret=RetU32(),
     cases=lambda rng: ([u32(rng) | rng.choice([0, 1])] for _ in iter(int, 1)), **ON_CODMP)
spec(name="bdBitBuffer::writeBits", rust=RUST + "BitBuffer::write_bits", args=[THIS, Bytes(), Int()],
     ret=RetVoid(), cases=_write_bits_cases, **ON_CODMP)
spec(name="bdBitBuffer::readBits", rust=RUST + "BitBuffer::read_bits",
     args=[THIS, Buffer(size=0, terminated=False), Int()], ret=RetBool(), cases=_read_bits_cases, **ON_CODMP)
spec(name="bdBitBuffer::writeDataType", rust=RUST + "BitBuffer::write_data_type", args=[THIS, Int()],
     ret=RetVoid(), cases=_value_cases(lambda rng: rng.choice([0, 1, 7, 12, 15, 19, 31, 32, 255])), **ON_CODMP)
spec(name="bdBitBuffer::readDataType", rust=RUST + "BitBuffer::read_data_type", args=[THIS, Int()],
     ret=RetBool(), cases=_value_cases(lambda rng: rng.randint(0, 31)), **ON_CODMP)
spec(name="bdBitBuffer::writeBool", rust=RUST + "BitBuffer::write_bool", args=[THIS, Int(bits=8)], ret=RetVoid(),
     cases=_value_cases(lambda rng: rng.choice([0, 1, 2, 255])), **ON_CODMP)
spec(name="bdBitBuffer::writeInt16", rust=RUST + "BitBuffer::write_i16", args=[THIS, Int(bits=16)], ret=RetVoid(),
     cases=_value_cases(lambda rng: rng.choice([0, 1, -1, 32767, -32768, rng.randint(-32768, 32767)])), **ON_CODMP)
spec(name="bdBitBuffer::writeUInt16", rust=RUST + "BitBuffer::write_u16", args=[THIS, Int(bits=16)], ret=RetVoid(),
     cases=_value_cases(lambda rng: rng.randint(0, 0xFFFF)), **ON_CODMP)
spec(name="bdBitBuffer::writeInt32", rust=RUST + "BitBuffer::write_i32", args=[THIS, Int()], ret=RetVoid(),
     cases=_value_cases(i32), **ON_CODMP)
spec(name="bdBitBuffer::writeUInt32", rust=RUST + "BitBuffer::write_u32", args=[THIS, Int()], ret=RetVoid(),
     cases=_value_cases(u32), **ON_CODMP)
spec(name="bdBitBuffer::writeUInt64", rust=RUST + "BitBuffer::write_u64", args=[THIS, Int(bits=64)], ret=RetVoid(),
     cases=_value_cases(lambda rng: rng.getrandbits(64) - (1 << 63)), **ON_CODMP)
spec(name="bdBitBuffer::writeFloat32", rust=RUST + "BitBuffer::write_f32", args=[THIS, F32()], ret=RetVoid(),
     cases=_value_cases(lambda rng: float_value(rng, -1e6, 1e6)), **ON_CODMP)
spec(name="bdBitBuffer::readBool", rust=RUST + "BitBuffer::read_bool", args=[THIS, OUT1], ret=RetBool(),
     cases=_out_cases(1), **ON_CODMP)
spec(name="bdBitBuffer::readUInt8", rust=RUST + "BitBuffer::read_u8", args=[THIS, OUT1], ret=RetBool(),
     cases=_out_cases(1), **ON_CODMP)
spec(name="bdBitBuffer::readInt32", rust=RUST + "BitBuffer::read_i32", args=[THIS, OUT4], ret=RetBool(),
     cases=_out_cases(4), **ON_CODMP)
spec(name="bdBitBuffer::readUInt32", rust=RUST + "BitBuffer::read_u32", args=[THIS, OUT4], ret=RetBool(),
     cases=_out_cases(4), **ON_CODMP)
spec(name="bdBitBuffer::readFloat32", rust=RUST + "BitBuffer::read_f32", args=[THIS, OUT4], ret=RetBool(),
     cases=_out_cases(4), **ON_CODMP)
spec(name="bdBitBuffer::writeRangedUInt32", rust=RUST + "BitBuffer::write_ranged_u32",
     args=[THIS, Int(), Int(), Int(), Int(bits=8)], ret=RetVoid(), cases=_ranged_u32_write_cases, **ON_CODMP)
spec(name="bdBitBuffer::writeRangedInt32", rust=RUST + "BitBuffer::write_ranged_i32",
     args=[THIS, Int(), Int(), Int()], ret=RetVoid(), cases=_ranged_i32_write_cases, **ON_CODMP)
spec(name="bdBitBuffer::writeRangedFloat32", rust=RUST + "BitBuffer::write_ranged_f32",
     args=[THIS, F32(), F32(), F32(), F32()], ret=RetVoid(), cases=_ranged_f32_write_cases, **ON_CODMP)
spec(name="bdBitBuffer::readRangedUInt32", rust=RUST + "BitBuffer::read_ranged_u32",
     args=[THIS, OUT4, Int(), Int(), Int(bits=8)], ret=RetBool(), cases=_ranged_u32_read_cases, **ON_CODMP)
spec(name="bdBitBuffer::readRangedInt32", rust=RUST + "BitBuffer::read_ranged_i32",
     args=[THIS, OUT4, Int(), Int()], ret=RetBool(), cases=_ranged_i32_read_cases, **ON_CODMP)
spec(name="bdBitBuffer::readRangedFloat32", rust=RUST + "BitBuffer::read_ranged_f32",
     args=[THIS, OUT4, F32(), F32(), F32()], ret=RetBool(), cases=_ranged_f32_read_cases, **ON_CODMP)
